#include "d3d12_shader_cache.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "../d3d10/d3d10_blob.hpp"
#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include <deque>
#include <thread>
#include <limits>

namespace dxmt {
namespace {

// ponytail: one process-wide lock serializes cache I/O; split by cache only if profiling warrants it.
std::mutex cache_mutex;
bool caches_disabled = false;
constexpr uint64_t kMagic = 0x31435354584d4444ull; // DDMXTSC1; fixed-width, little-endian disk format.
struct FileHeader { uint64_t magic, version; };
struct RecordHeader { uint32_t key_size, value_size; Sha1Digest key_hash, checksum; };
static_assert(sizeof(FileHeader) == 16 && sizeof(RecordHeader) == 48);

struct Entry {
  Sha1Digest hash;
  std::string key, value;
};
struct SearchHandle {
  HANDLE value;
  ~SearchHandle() { FindClose(value); }
};

HRESULT FileError() { return HRESULT_FROM_WIN32(GetLastError()); }

bool Seek(HANDLE file, uint64_t offset) {
  LARGE_INTEGER position;
  position.QuadPart = offset;
  return SetFilePointerEx(file, position, nullptr, FILE_BEGIN);
}

bool Read(HANDLE file, void *data, DWORD size) {
  DWORD done = 0;
  if (!ReadFile(file, data, size, &done, nullptr)) return false;
  if (done != size) { SetLastError(ERROR_READ_FAULT); return false; }
  return true;
}

bool Write(HANDLE file, const void *data, DWORD size) {
  DWORD done = 0;
  if (!WriteFile(file, data, size, &done, nullptr)) return false;
  if (done != size) { SetLastError(ERROR_WRITE_FAULT); return false; }
  return true;
}

struct Cache {
  D3D12_SHADER_CACHE_SESSION_DESC desc;
  std::wstring path;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::list<Entry> entries;
  struct DiskRecord { RecordHeader header; uint64_t offset; };
  std::unordered_multimap<Sha1Digest, DiskRecord> records;
  bool indexed = true;
  uint64_t memory_bytes = 0, file_bytes = 0;
  bool delete_on_destroy = false, corrupt = false;

  explicit Cache(const D3D12_SHADER_CACHE_SESSION_DESC &description) : desc(description) {
    if (!desc.MaximumInMemoryCacheSizeBytes) desc.MaximumInMemoryCacheSizeBytes = 1024;
    if (!desc.MaximumInMemoryCacheEntries) desc.MaximumInMemoryCacheEntries = 128;
    if (!desc.MaximumValueFileSizeBytes) desc.MaximumValueFileSizeBytes = 128 * 1024 * 1024;
  }
  ~Cache() {
    if (file != INVALID_HANDLE_VALUE) {
      if (delete_on_destroy) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
      }
      CloseHandle(file);
    }
  }

  HRESULT clear() {
    if (file != INVALID_HANDLE_VALUE) {
      const FileHeader header{kMagic, desc.Version};
      if (!Seek(file, 0) || !Write(file, &header, sizeof(header)) || !SetEndOfFile(file) || !FlushFileBuffers(file)) {
        corrupt = true;
        return FileError();
      }
      file_bytes = sizeof(header);
    }
    corrupt = false;
    entries.clear();
    records.clear();
    indexed = true;
    memory_bytes = 0;
    return S_OK;
  }

  HRESULT open() {
    // Exclusive access is acquired BEFORE examining or replacing a generation.
    file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return GetLastError() == ERROR_SHARING_VIOLATION ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : FileError();
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) return FileError();
    file_bytes = size.QuadPart;
    if (!file_bytes) return clear();
    FileHeader header{};
    if (file_bytes < sizeof(header) || !Read(file, &header, sizeof(header)) || header.magic != kMagic)
      return E_FAIL;
    if (header.version != desc.Version) return clear();
    // A smaller reopening budget must not invalidate an existing cache. Keep the constant-memory
    // scan for oversized files; otherwise record count is bounded by the configured disk budget / 48.
    indexed = file_bytes <= desc.MaximumValueFileSizeBytes;
    // Validate on open, so malformed lengths never become allocations or false cache hits.
    for (uint64_t offset = sizeof(FileHeader); offset < file_bytes;) {
      RecordHeader record{};
      if (file_bytes - offset < sizeof(record) || !Seek(file, offset) || !Read(file, &record, sizeof(record)))
        return E_FAIL;
      bool same_key = false;
      HRESULT hr = verify(record, offset, nullptr, 0, same_key);
      if (FAILED(hr)) return hr;
      if (indexed) records.emplace(record.key_hash, DiskRecord{record, offset});
      offset += sizeof(record) + uint64_t(record.key_size) + record.value_size;
    }
    return S_OK;
  }

  HRESULT verify(const RecordHeader &record, uint64_t offset, const void *key, UINT key_size, bool &same_key) {
    const uint64_t payload_size = uint64_t(record.key_size) + record.value_size;
    if (offset > file_bytes || file_bytes - offset < sizeof(record) ||
        payload_size > file_bytes - offset - sizeof(record) || !Seek(file, offset + sizeof(record))) return E_FAIL;
    std::array<uint8_t, 4096> scratch;
    Sha1HashState key_digest, checksum;
    same_key = key && record.key_size == key_size;
    for (uint64_t pos = 0; pos < payload_size;) {
      const UINT count = UINT(std::min<uint64_t>(scratch.size(), payload_size - pos));
      if (!Read(file, scratch.data(), count)) return E_FAIL;
      checksum.update(scratch.data(), count);
      const UINT key_count = pos < record.key_size ? UINT(std::min<uint64_t>(count, record.key_size - pos)) : 0;
      if (key_count) {
        key_digest.update(scratch.data(), key_count);
        if (same_key && memcmp(scratch.data(), static_cast<const uint8_t *>(key) + pos, key_count)) same_key = false;
      }
      pos += count;
    }
    if (key_digest.final() != record.key_hash || checksum.final() != record.checksum) return E_FAIL;
    return S_OK;
  }

  HRESULT locate(const void *key, UINT key_size, const Sha1Digest &hash, RecordHeader &found, uint64_t &value_offset) {
    if (corrupt) return E_FAIL;
    bool matched = false, collision = false;
    auto inspect = [&](const RecordHeader &record, uint64_t record_offset) {
      bool same_key = false;
      HRESULT hr = verify(record, record_offset, key, key_size, same_key);
      if (FAILED(hr)) return hr;
      if (record.key_hash != hash) return S_OK;
      if (same_key) {
        const uint64_t offset = record_offset + sizeof(record) + record.key_size;
        if (!matched || offset > value_offset) {
          found = record;
          value_offset = offset;
        }
        matched = true;
      } else collision = true;
      return S_OK;
    };
    if (!indexed) {
      for (uint64_t offset = sizeof(FileHeader); offset < file_bytes;) {
        RecordHeader record{};
        if (file_bytes - offset < sizeof(record) || !Seek(file, offset) || !Read(file, &record, sizeof(record)))
          return E_FAIL;
        HRESULT hr = inspect(record, offset);
        if (FAILED(hr)) return hr;
        offset += sizeof(record) + uint64_t(record.key_size) + record.value_size;
      }
    } else {
      const auto [begin, end] = records.equal_range(hash);
      for (auto it = begin; it != end; ++it) {
        const auto &selected = it->second;
        RecordHeader record{};
        if (!Seek(file, selected.offset) || !Read(file, &record, sizeof(record)) ||
            record.key_size != selected.header.key_size || record.value_size != selected.header.value_size ||
            record.key_hash != selected.header.key_hash || record.checksum != selected.header.checksum) return E_FAIL;
        HRESULT hr = inspect(record, selected.offset);
        if (FAILED(hr)) return hr;
      }
    }
    return collision ? DXGI_ERROR_CACHE_HASH_COLLISION : matched ? S_OK : DXGI_ERROR_NOT_FOUND;
  }

  void remember(const void *key, UINT key_size, const void *value, UINT value_size, const Sha1Digest &hash) {
    const uint64_t bytes = uint64_t(key_size) + value_size;
    if (bytes > desc.MaximumInMemoryCacheSizeBytes) return;
    // Allocate before eviction. Failure to populate this optional disk lookaside must not undo a committed store.
    Entry entry{hash, std::string(static_cast<const char *>(key), key_size),
                std::string(static_cast<const char *>(value), value_size)};
    entries.push_front(std::move(entry));
    memory_bytes += bytes;
    while (memory_bytes > desc.MaximumInMemoryCacheSizeBytes || entries.size() > desc.MaximumInMemoryCacheEntries) {
      memory_bytes -= entries.back().key.size() + entries.back().value.size();
      entries.pop_back();
    }
  }

  HRESULT find(const void *key, UINT key_size, void *value, UINT *value_size) {
    if (caches_disabled) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    if ((!key && key_size) || !value_size) return E_INVALIDARG;
    if (!key) key = "";
    if (corrupt) return E_FAIL;
    const auto hash = Sha1HashState::compute(key, key_size);
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (it->hash != hash) continue;
      if (it->key.size() != key_size || memcmp(it->key.data(), key, key_size)) return DXGI_ERROR_CACHE_HASH_COLLISION;
      const UINT capacity = *value_size;
      *value_size = UINT(it->value.size());
      if (value && capacity < *value_size) return DXGI_ERROR_MORE_DATA;
      if (value) memcpy(value, it->value.data(), *value_size);
      entries.splice(entries.begin(), entries, it);
      return S_OK;
    }
    if (file == INVALID_HANDLE_VALUE) return DXGI_ERROR_NOT_FOUND;
    RecordHeader record{};
    uint64_t offset = 0;
    HRESULT hr = locate(key, key_size, hash, record, offset);
    if (FAILED(hr)) return hr;
    const UINT capacity = *value_size;
    *value_size = record.value_size;
    if (!value) return S_OK;
    if (capacity < *value_size) return DXGI_ERROR_MORE_DATA;
    if (!Seek(file, offset) || !Read(file, value, *value_size)) return E_FAIL;
    try { remember(key, key_size, value, *value_size, hash); } catch (const std::bad_alloc &) {}
    return S_OK;
  }

  HRESULT store(const void *key, UINT key_size, const void *value, UINT value_size) {
    if (caches_disabled) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    if ((!key && key_size) || (!value && value_size)) return E_INVALIDARG;
    if (!key) key = "";
    if (!value) value = "";
    if (corrupt) return E_FAIL;
    const auto hash = Sha1HashState::compute(key, key_size);
    if (file == INVALID_HANDLE_VALUE) {
      for (const auto &entry : entries) {
        if (entry.hash != hash) continue;
        return entry.key.size() == key_size && !memcmp(entry.key.data(), key, key_size) ?
            DXGI_ERROR_ALREADY_EXISTS : DXGI_ERROR_CACHE_HASH_COLLISION;
      }
      const uint64_t bytes = uint64_t(key_size) + value_size;
      if (bytes > desc.MaximumInMemoryCacheSizeBytes - memory_bytes || entries.size() >= desc.MaximumInMemoryCacheEntries)
        return DXGI_ERROR_CACHE_FULL;
      remember(key, key_size, value, value_size, hash);
      return S_OK;
    }
    RecordHeader record{};
    uint64_t offset = 0;
    HRESULT hr = locate(key, key_size, hash, record, offset);
    if (hr == S_OK) return DXGI_ERROR_ALREADY_EXISTS;
    if (hr != DXGI_ERROR_NOT_FOUND) return hr;
    const uint64_t bytes = sizeof(record) + uint64_t(key_size) + value_size;
    if (file_bytes > desc.MaximumValueFileSizeBytes || bytes > desc.MaximumValueFileSizeBytes - file_bytes)
      return DXGI_ERROR_CACHE_FULL;
    record = {key_size, value_size, hash, Sha1HashState().update(key, key_size).update(value, value_size).final()};
    // Allocate index storage before append; publish its node only after the durable flush.
    decltype(records) pending;
    pending.emplace(hash, DiskRecord{record, file_bytes});
    records.reserve(records.size() + 1);
    if (!Seek(file, file_bytes) || !Write(file, &record, sizeof(record)) || !Write(file, key, key_size) ||
        !Write(file, value, value_size) || !FlushFileBuffers(file)) {
      const HRESULT error = FileError();
      if (!Seek(file, file_bytes) || !SetEndOfFile(file) || !FlushFileBuffers(file)) corrupt = true;
      return FAILED(error) ? error : E_FAIL;
    }
    records.insert(pending.extract(pending.begin()));
    file_bytes += bytes;
    try { remember(key, key_size, value, value_size, hash); } catch (const std::bad_alloc &) {}
    return S_OK;
  }
};

std::map<std::wstring, std::weak_ptr<Cache>> registry;

std::wstring Hex(const void *data, size_t size) {
  static constexpr WCHAR digits[] = L"0123456789abcdef";
  std::wstring result(size * 2, L'0');
  auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    result[i * 2] = digits[bytes[i] >> 4];
    result[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return result;
}

HRESULT Paths(bool working_directory, std::wstring &directory, std::wstring &exe_token) {
  WCHAR buffer[32768];
  DWORD count = GetModuleFileNameW(nullptr, buffer, std::size(buffer));
  if (!count || count >= std::size(buffer)) return E_FAIL;
  CharLowerBuffW(buffer, count);
  const auto hash = Sha1HashState::compute(buffer, count * sizeof(WCHAR));
  exe_token = Hex(hash.data, sizeof(hash.data));
  if (working_directory) {
    count = GetCurrentDirectoryW(std::size(buffer), buffer);
    if (!count || count >= std::size(buffer)) return E_FAIL;
    directory.assign(buffer, count);
  } else {
    count = GetTempPathW(std::size(buffer), buffer);
    if (!count || count >= std::size(buffer)) return E_FAIL;
    directory.assign(buffer, count);
    directory += L"dxmt-shader-sessions";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return FileError();
    directory += L"\\" + exe_token;
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return FileError();
  }
  directory += L"\\";
  return S_OK;
}

constexpr uint64_t kAutomaticEntryBytes = 64ull * 1024 * 1024;
constexpr uint64_t kAutomaticDiskBytes = 256ull * 1024 * 1024;
constexpr uint64_t kStageMagic = 0x314754534d584444ull; // DDXMSTG1
constexpr uint32_t kStageVersion = 2;
constexpr uint64_t kStageEntryBytes = 8ull * 1024 * 1024;
constexpr uint64_t kStageDiskBytes = 256ull * 1024 * 1024;

struct StageFileHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t key_size;
  uint64_t library_size;
  uint64_t metadata_size;
  uint64_t constants_size;
  uint64_t constant_alignment;
  uint64_t relocations_size;
  WMTPipelineCacheIdentity identity;
  Sha1Digest checksum;
};
static_assert(sizeof(StageFileHeader) % 8 == 0);
bool StageIdentity(WMTPipelineCacheIdentity &identity) { return WMTGetPipelineCacheIdentity(&identity); }
bool StageDiskHasRoom(const std::wstring &directory, const std::wstring &path, uint64_t incoming) {
  WIN32_FIND_DATAW data{};
  const auto pattern = directory + L"dxmt-stage-*";
  HANDLE search = FindFirstFileW(pattern.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_FILE_NOT_FOUND && incoming <= kStageDiskBytes;
  }
  uint64_t total = 0;
  do {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const auto current = directory + data.cFileName;
    if (current != path)
      total += (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  } while (FindNextFileW(search, &data));
  const DWORD enumeration_error = GetLastError();
  FindClose(search);
  if (enumeration_error != ERROR_NO_MORE_FILES) return false;
  return total <= kStageDiskBytes && incoming <= kStageDiskBytes - total;
}

struct FileHandle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct AutomaticDiskAccounting {
  uint64_t bytes;
  DWORD valid, reserved;
};
static_assert(sizeof(AutomaticDiskAccounting) == 16);

struct AutomaticAccountingMapping {
  HANDLE handle = nullptr;
  AutomaticDiskAccounting *state = nullptr;
  ~AutomaticAccountingMapping() {
    if (state) UnmapViewOfFile(state);
    if (handle) CloseHandle(handle);
  }
};

// Retain handles so successive publications (and overlapping processes) share accounting.
// This lock protects only registry ownership, never disk I/O or application-session caches.
std::mutex automatic_accounting_mutex;
std::map<std::wstring, std::unique_ptr<AutomaticAccountingMapping>> automatic_accounting;

HRESULT AutomaticAccounting(const std::wstring &directory, AutomaticDiskAccounting *&state) {
  std::lock_guard lock(automatic_accounting_mutex);
  auto found = automatic_accounting.find(directory);
  if (found == automatic_accounting.end()) {
    auto mapping = std::make_unique<AutomaticAccountingMapping>();
    const auto hash = Sha1HashState::compute(directory.data(), directory.size() * sizeof(WCHAR));
    const auto name = L"Local\\dxmt-pso-accounting-v1-" + Hex(hash.data, sizeof(hash.data));
    mapping->handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(AutomaticDiskAccounting), name.c_str());
    if (!mapping->handle) return FileError();
    mapping->state = static_cast<AutomaticDiskAccounting *>(
        MapViewOfFile(mapping->handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AutomaticDiskAccounting)));
    if (!mapping->state) return FileError();
    found = automatic_accounting.emplace(directory, std::move(mapping)).first;
  }
  state = found->second->state;
  return S_OK;
}

// Disk metadata transactions never call into the in-process pipeline cache. Reads use atomic
// published files and FILE_SHARE_DELETE, so they need neither this mutex nor a directory scan.
struct AutomaticDiskLock {
  HANDLE value = nullptr;
  bool acquired = false;
  AutomaticDiskAccounting *accounting = nullptr;
  HRESULT acquire(const std::wstring &directory) {
    const auto hash = Sha1HashState::compute(directory.data(), directory.size() * sizeof(WCHAR));
    const auto name = L"Local\\dxmt-pso-" + Hex(hash.data, sizeof(hash.data));
    value = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!value) return FileError();
    const DWORD result = WaitForSingleObject(value, 1000);
    acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    if (acquired) {
      const HRESULT hr = AutomaticAccounting(directory, accounting);
      if (FAILED(hr)) return hr;
      if (result == WAIT_ABANDONED) accounting->valid = 0;
      return S_OK;
    }
    return result == WAIT_FAILED ? FileError() : DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  }
  ~AutomaticDiskLock() {
    if (acquired) ReleaseMutex(value);
    if (value) CloseHandle(value);
  }
};

struct AutomaticTempFile {
  std::wstring path;
  FileHandle file;
  bool owned = false;
  ~AutomaticTempFile() {
    if (file.value != INVALID_HANDLE_VALUE) {
      CloseHandle(file.value);
      file.value = INVALID_HANDLE_VALUE;
    }
    if (owned) DeleteFileW(path.c_str());
  }
};

// Only these exact names belong to the automatic cache; application session files are untouched.
bool AutomaticFileName(const std::wstring &name, bool &temporary) {
  if (name.size() != 53 && name.size() != 71) return false;
  if (name.compare(0, 9, L"dxmt-pso-")) return false;
  for (size_t i = 9; i < 49; ++i)
    if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f'))) return false;
  temporary = name.size() == 71;
  if (!temporary) return name.compare(49, 4, L".bin") == 0;
  if (name[49] != L'-' || name[58] != L'-' || name.compare(67, 4, L".tmp")) return false;
  for (size_t i = 50; i < 67; ++i) {
    if (i == 58) continue;
    if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f'))) return false;
  }
  return true;
}

struct AutomaticDiskEntry {
  std::wstring name;
  uint64_t bytes, age;
};

HRESULT AutomaticDiskEntries(const std::wstring &directory, std::vector<AutomaticDiskEntry> &entries,
                             uint64_t &total, bool clear) {
  WIN32_FIND_DATAW info;
  HANDLE search = FindFirstFileW((directory + L"dxmt-pso-*").c_str(), &info);
  if (search == INVALID_HANDLE_VALUE)
    return GetLastError() == ERROR_FILE_NOT_FOUND ? S_OK : FileError();
  const SearchHandle search_handle{search};
  do {
    bool temporary = false;
    const std::wstring name = info.cFileName;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !AutomaticFileName(name, temporary)) continue;
    // A writer holds the same mutex until its temp is removed/published; these are crashed writers.
    if (clear || temporary) {
      if (!DeleteFileW((directory + name).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return FileError();
    } else {
      const uint64_t bytes = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
      if (bytes > UINT64_MAX - total) return E_FAIL;
      total += bytes;
      entries.push_back({name, bytes, (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
                                    info.ftLastWriteTime.dwLowDateTime});
    }
  } while (FindNextFileW(search, &info));
  return GetLastError() == ERROR_NO_MORE_FILES ? S_OK : FileError();
}

class ShaderCacheSession final : public MTLD3D12DeviceChild<ID3D12ShaderCacheSession> {
  D3D12_SHADER_CACHE_SESSION_DESC desc_;
  std::shared_ptr<Cache> cache_;
public:
  ShaderCacheSession(MTLD3D12Device *device, const D3D12_SHADER_CACHE_SESSION_DESC &desc, std::shared_ptr<Cache> cache)
      : MTLD3D12DeviceChild(device), desc_(desc), cache_(std::move(cache)) {}
  ~ShaderCacheSession() override {
    std::lock_guard lock(cache_mutex);
    cache_.reset();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3D12Object) && riid != __uuidof(ID3D12DeviceChild) &&
        riid != __uuidof(ID3D12ShaderCacheSession)) return E_NOINTERFACE;
    *out = static_cast<ID3D12ShaderCacheSession *>(this);
    AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FindValue(const void *key, UINT key_size, void *value, UINT *value_size) override {
    std::lock_guard lock(cache_mutex);
    try { return cache_->find(key, key_size, value, value_size); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE StoreValue(const void *key, UINT key_size, const void *value, UINT value_size) override {
    std::lock_guard lock(cache_mutex);
    try { return cache_->store(key, key_size, value, value_size); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  void STDMETHODCALLTYPE SetDeleteOnDestroy() override {
    std::lock_guard lock(cache_mutex);
    cache_->delete_on_destroy = true;
  }
  D3D12_SHADER_CACHE_SESSION_DESC *STDMETHODCALLTYPE GetDesc(D3D12_SHADER_CACHE_SESSION_DESC *out) override {
    *out = desc_;
    return out;
  }
};

} // namespace

HRESULT ReadAutomaticPipelineCache(const Sha1Digest &key, ID3DBlob **blob) {
  if (!blob) return E_POINTER;
  *blob = nullptr;
  try {
    std::wstring directory, executable;
    HRESULT hr = Paths(false, directory, executable);
    if (FAILED(hr)) return hr;
    const auto path = directory + L"dxmt-pso-" + Hex(key.data, sizeof(key.data)) + L".bin";
    const FileHandle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
      return GetLastError() == ERROR_FILE_NOT_FOUND ? DXGI_ERROR_NOT_FOUND : FileError();
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.value, &size)) return FileError();
    if (size.QuadPart <= 0 || uint64_t(size.QuadPart) > kAutomaticEntryBytes) return E_FAIL;
    Com<ID3DBlob> result;
    hr = CreateBlobFromMalloc(SIZE_T(size.QuadPart), &result);
    if (FAILED(hr)) return hr;
    if (!Read(file.value, result->GetBufferPointer(), DWORD(size.QuadPart))) return FileError();
    *blob = result.takeOwnership();
    return S_OK;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

void WriteAutomaticPipelineCache(const Sha1Digest &key, ID3DBlob *blob) {
  if (!blob || !blob->GetBufferSize() || blob->GetBufferSize() > kAutomaticEntryBytes) return;
  try {
    std::wstring directory, executable;
    if (FAILED(Paths(false, directory, executable))) return;
    AutomaticDiskLock lock;
    if (FAILED(lock.acquire(directory))) return;
    auto &accounting = *lock.accounting;
    const bool rescan = !accounting.valid;
    uint64_t total = accounting.bytes;
    // Invalid until publication commits: temp cleanup, eviction, errors and crashes need a rescan.
    accounting.valid = 0;
    std::vector<AutomaticDiskEntry> entries;
    if (rescan) {
      total = 0;
      if (FAILED(AutomaticDiskEntries(directory, entries, total, false))) return;
    }
    const uint64_t size = blob->GetBufferSize();
    const auto name = L"dxmt-pso-" + Hex(key.data, sizeof(key.data)) + L".bin";
    const auto path = directory + name;
    uint64_t replaced = 0;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
      if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return;
      replaced = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return;
    if (replaced > total) return;
    if (total - replaced > kAutomaticDiskBytes - size) {
      if (!rescan) {
        total = 0;
        if (FAILED(AutomaticDiskEntries(directory, entries, total, false))) return;
        if (replaced > total) return;
      }
      const uint64_t target = kAutomaticDiskBytes - std::max<uint64_t>(size, 32ull * 1024 * 1024);
      // Oldest publication first; reserve a batch of space, without read-side LRU writes.
      std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.age < b.age; });
      for (const auto &entry : entries) {
        if (total - replaced <= target) break;
        if (entry.name == name) continue; // Keep the replacement available until atomic publication.
        if (!DeleteFileW((directory + entry.name).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return;
        total -= entry.bytes;
      }
    }
    if (total - replaced > kAutomaticDiskBytes - size) return;
    const auto stem = directory + L"dxmt-pso-" + Hex(key.data, sizeof(key.data));
    static std::atomic<uint32_t> sequence{0};
    const uint32_t pid = GetCurrentProcessId(), serial = ++sequence;
    AutomaticTempFile temporary;
    temporary.path = stem + L"-" + Hex(&pid, sizeof(pid)) + L"-" + Hex(&serial, sizeof(serial)) + L".tmp";
    temporary.file.value = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temporary.file.value == INVALID_HANDLE_VALUE) return;
    temporary.owned = true;
    if (!Write(temporary.file.value, blob->GetBufferPointer(), DWORD(size)) ||
        !FlushFileBuffers(temporary.file.value)) return;
    if (!CloseHandle(temporary.file.value)) return;
    temporary.file.value = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
    temporary.owned = false;
    accounting.bytes = total - replaced + size;
    accounting.valid = 1;
  } catch (const std::bad_alloc &) {} // Persistence must never turn successful compilation into failure.
}

HRESULT ReadAutomaticStageArtifact(const std::string &key, std::shared_ptr<const ShaderStageArtifact> &artifact) {
  artifact.reset();
  if (key.empty() || key.size() > UINT32_MAX) return E_INVALIDARG;
  try {
    std::wstring directory, executable;
    if (FAILED(Paths(false, directory, executable))) return E_FAIL;
    const auto digest = Sha1HashState::compute(key.data(), key.size());
    const auto path = directory + L"dxmt-stage-" + Hex(digest.data, sizeof(digest.data)) + L".bin";
    const FileHandle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return DXGI_ERROR_NOT_FOUND;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(StageFileHeader)) ||
        uint64_t(size.QuadPart) > kStageEntryBytes) return E_FAIL;
    StageFileHeader header{};
    WMTPipelineCacheIdentity identity{};
    if (!Seek(file.value, 0) || !Read(file.value, &header, sizeof(header)) || header.magic != kStageMagic ||
        header.version != kStageVersion || header.key_size != key.size() || !StageIdentity(identity) ||
        memcmp(&header.identity, &identity, sizeof(identity))) return E_FAIL;
    const uint64_t payload = uint64_t(header.key_size) + header.library_size + header.metadata_size +
                             header.constants_size + header.relocations_size;
    if (payload > kStageEntryBytes - sizeof(header) || sizeof(header) + payload != uint64_t(size.QuadPart) ||
        header.library_size < 4 || header.library_size > UINT32_MAX || header.metadata_size > UINT32_MAX ||
        !header.constant_alignment || (header.constant_alignment & (header.constant_alignment - 1)) ||
        header.constant_alignment > UINT32_MAX || (!header.constants_size && header.constant_alignment != 1) ||
        header.constants_size > UINT32_MAX || header.relocations_size > UINT32_MAX ||
        header.constants_size % sizeof(uint64_t) || header.relocations_size % sizeof(SM50_CONSTANT_RELOCATION) ||
        header.relocations_size > header.constants_size / sizeof(uint64_t) * sizeof(SM50_CONSTANT_RELOCATION)) return E_FAIL;
    std::vector<uint8_t> bytes(payload);
    if (!Read(file.value, bytes.data(), DWORD(bytes.size()))) return E_FAIL;
    Sha1HashState checksum;
    const auto checksum_header = [&] { auto copy = header; copy.checksum = {}; return copy; }();
    checksum.update(&checksum_header, offsetof(StageFileHeader, checksum)).update(bytes.data(), bytes.size());
    if (checksum.final() != header.checksum || memcmp(bytes.data(), key.data(), key.size())) return E_FAIL;
    auto value = std::make_shared<ShaderStageArtifact>();
    size_t offset = key.size();
    const size_t library_end = offset + header.library_size;
    value->library.assign(bytes.begin() + offset, bytes.begin() + library_end);
    offset = library_end;
    if (memcmp(value->library.data(), "MTLB", 4)) return E_FAIL;
    if (header.metadata_size < sizeof(uint32_t)) return E_FAIL;
    uint32_t metadata_count = 0;
    memcpy(&metadata_count, bytes.data() + offset, sizeof(metadata_count));
    const uint64_t metadata_sizes_bytes = uint64_t(metadata_count) * sizeof(uint32_t);
    if (metadata_sizes_bytes > header.metadata_size - sizeof(uint32_t)) return E_FAIL;
    const size_t metadata_data_offset = offset + sizeof(uint32_t) + size_t(metadata_sizes_bytes);
    const size_t metadata_data_size = header.metadata_size - sizeof(uint32_t) - size_t(metadata_sizes_bytes);
    value->metadata_sizes.resize(metadata_count);
    memcpy(value->metadata_sizes.data(), bytes.data() + offset + sizeof(uint32_t), size_t(metadata_sizes_bytes));
    uint64_t metadata_sum = 0;
    for (uint32_t metadata_size : value->metadata_sizes) {
      metadata_sum += metadata_size;
      if (metadata_sum > metadata_data_size) return E_FAIL;
    }
    if (metadata_sum != metadata_data_size) return E_FAIL;
    value->metadata.assign(bytes.begin() + metadata_data_offset, bytes.begin() + metadata_data_offset + metadata_data_size);
    offset += header.metadata_size;
    value->constants.resize(header.constants_size / sizeof(uint64_t));
    memcpy(value->constants.data(), bytes.data() + offset, header.constants_size); offset += header.constants_size;
    value->relocations.resize(header.relocations_size / sizeof(SM50_CONSTANT_RELOCATION));
    memcpy(value->relocations.data(), bytes.data() + offset, header.relocations_size);
    for (const auto &relocation : value->relocations) {
      if (header.constants_size < sizeof(uint64_t) ||
          relocation.Offset > header.constants_size - sizeof(uint64_t) ||
          (relocation.TargetOffset > header.constants_size &&
           relocation.TargetOffset != SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE &&
           relocation.TargetOffset != SM50_CONSTANT_RELOCATION_TARGET_ATOMIC64_CONTEXT)) return E_FAIL;
    }
    value->view.Data = value->library.data(); value->view.Size = value->library.size();
    value->view.ConstantData = value->constants.empty() ? nullptr : value->constants.data();
    value->view.ConstantDataSize = header.constants_size; value->view.ConstantDataAlignment = header.constant_alignment;
    value->view.ConstantRelocations = value->relocations.empty() ? nullptr : value->relocations.data();
    value->view.ConstantRelocationCount = value->relocations.size();
    artifact = std::move(value);
    return S_OK;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

void WriteAutomaticStageArtifact(const std::string &key, const ShaderStageArtifact &artifact) {
  if (key.empty() || key.size() > UINT32_MAX || artifact.library.size() < 4 || artifact.library.size() > UINT32_MAX ||
      artifact.metadata.size() > UINT32_MAX || artifact.constants.size() > UINT32_MAX / sizeof(uint64_t) ||
      artifact.relocations.size() > UINT32_MAX / sizeof(SM50_CONSTANT_RELOCATION)) return;
  if (artifact.metadata_sizes.size() > UINT32_MAX / sizeof(uint32_t)) return;
  uint64_t metadata_sizes_bytes = artifact.metadata_sizes.size() * sizeof(uint32_t);
  uint64_t metadata_sum = 0;
  for (uint32_t metadata_size : artifact.metadata_sizes) {
    metadata_sum += metadata_size;
    if (metadata_sum > artifact.metadata.size()) return;
  }
  if (metadata_sum != artifact.metadata.size()) return;
  const uint64_t encoded_metadata_size = sizeof(uint32_t) + metadata_sizes_bytes + artifact.metadata.size();
  if (encoded_metadata_size > UINT32_MAX) return;
  std::vector<uint8_t> encoded_metadata((size_t(encoded_metadata_size)));
  const uint32_t metadata_count = uint32_t(artifact.metadata_sizes.size());
  memcpy(encoded_metadata.data(), &metadata_count, sizeof(metadata_count));
  if (metadata_sizes_bytes)
    memcpy(encoded_metadata.data() + sizeof(metadata_count), artifact.metadata_sizes.data(), size_t(metadata_sizes_bytes));
  if (!artifact.metadata.empty())
    memcpy(encoded_metadata.data() + sizeof(metadata_count) + size_t(metadata_sizes_bytes), artifact.metadata.data(), artifact.metadata.size());
  const uint64_t payload = key.size() + artifact.library.size() + encoded_metadata.size() +
                           artifact.constants.size() * sizeof(uint64_t) + artifact.relocations.size() * sizeof(SM50_CONSTANT_RELOCATION);
  WMTPipelineCacheIdentity identity{};
  if (payload > kStageEntryBytes - sizeof(StageFileHeader) || !StageIdentity(identity) || !artifact.view.ConstantDataAlignment ||
      (artifact.view.ConstantDataAlignment & (artifact.view.ConstantDataAlignment - 1)) ||
      (!artifact.constants.size() && artifact.view.ConstantDataAlignment != 1)) return;
  try {
    std::wstring directory, executable;
    if (FAILED(Paths(false, directory, executable))) return;
    AutomaticDiskLock disk_lock;
    if (FAILED(disk_lock.acquire(directory))) return;
    const auto digest = Sha1HashState::compute(key.data(), key.size());
    const auto path = directory + L"dxmt-stage-" + Hex(digest.data, sizeof(digest.data)) + L".bin";
    static std::atomic<uint32_t> sequence{0};
    const uint32_t pid = GetCurrentProcessId(), serial = ++sequence;
    const auto temp = path + L"-" + Hex(&pid, sizeof(pid)) + L"-" + Hex(&serial, sizeof(serial)) + L".tmp";
    if (!StageDiskHasRoom(directory, path, sizeof(StageFileHeader) + payload)) return;
    FileHandle file{CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return;
    StageFileHeader header{kStageMagic, kStageVersion, uint32_t(key.size()), artifact.library.size(), encoded_metadata.size(),
                           artifact.constants.size() * sizeof(uint64_t), artifact.view.ConstantDataAlignment,
                           artifact.relocations.size() * sizeof(SM50_CONSTANT_RELOCATION),
                           identity, {}};
    Sha1HashState checksum;
    checksum.update(&header, offsetof(StageFileHeader, checksum)).update(key.data(), key.size())
      .update(artifact.library.data(), artifact.library.size()).update(encoded_metadata.data(), encoded_metadata.size())
      .update(artifact.constants.data(), header.constants_size).update(artifact.relocations.data(), header.relocations_size);
    header.checksum = checksum.final();
    if (!Write(file.value, &header, sizeof(header)) || !Write(file.value, key.data(), DWORD(key.size())) ||
        !Write(file.value, artifact.library.data(), DWORD(artifact.library.size())) ||
        !Write(file.value, encoded_metadata.data(), DWORD(encoded_metadata.size())) ||
        !Write(file.value, artifact.constants.data(), DWORD(header.constants_size)) ||
        !Write(file.value, artifact.relocations.data(), DWORD(header.relocations_size)) || !FlushFileBuffers(file.value)) {
      CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
      DeleteFileW(temp.c_str()); return;
    }
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      DeleteFileW(temp.c_str());
  } catch (...) {}
}

class StageArtifactWriter {
  struct Job {
    std::string key;
    std::shared_ptr<const ShaderStageArtifact> artifact;
    uint64_t generation;
  };
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> jobs_;
  bool stop_ = false;
  std::thread thread_;
  std::thread StartWorker() {
    try {
      return std::thread([this] {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) {
          if (stop_) return;
          continue;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
        ready_.notify_all();
      }
      try {
        PublishAutomaticStageArtifact(job.key, std::move(job.artifact), job.generation);
      } catch (...) {
        // Optional persistence must not terminate the worker or the producer process.
      }
    }
      });
    } catch (...) {
      stop_ = true;
      return {};
    }
  }
public:
  StageArtifactWriter() : thread_(StartWorker()) {}
  ~StageArtifactWriter() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    ready_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void enqueue(std::string key, std::shared_ptr<const ShaderStageArtifact> artifact, uint64_t generation) noexcept {
    try {
      std::unique_lock lock(mutex_);
      constexpr size_t limit = 8;
      if (stop_ || jobs_.size() >= limit) return;
      jobs_.push_back({std::move(key), std::move(artifact), generation});
    } catch (...) {
    }
    ready_.notify_one();
  }
};

StageArtifactWriter &StageArtifactWriterInstance() {
  static StageArtifactWriter writer;
  return writer;
}

void QueueAutomaticStageArtifact(const std::string &key, std::shared_ptr<const ShaderStageArtifact> artifact,
                                 uint64_t generation) {
  if (artifact) StageArtifactWriterInstance().enqueue(key, std::move(artifact), generation);
}


void RemoveAutomaticPipelineCache(const Sha1Digest &key) {
  try {
    std::wstring directory, executable;
    if (FAILED(Paths(false, directory, executable))) return;
    AutomaticDiskLock lock;
    if (FAILED(lock.acquire(directory))) return;
    lock.accounting->valid = 0;
    const auto path = directory + L"dxmt-pso-" + Hex(key.data, sizeof(key.data)) + L".bin";
    DeleteFileW(path.c_str());
  } catch (const std::bad_alloc &) {}
}

HRESULT ClearAutomaticPipelineCacheDisk() {
  try {
    std::wstring directory, executable;
    HRESULT hr = Paths(false, directory, executable);
    if (FAILED(hr)) return hr;
    AutomaticDiskLock lock;
    hr = lock.acquire(directory);
    if (FAILED(hr)) return hr;
    lock.accounting->valid = 0;
    std::vector<AutomaticDiskEntry> unused;
    uint64_t total = 0;
    hr = AutomaticDiskEntries(directory, unused, total, true);
    if (SUCCEEDED(hr)) {
      WIN32_FIND_DATAW info{};
      HANDLE search = FindFirstFileW((directory + L"dxmt-stage-*").c_str(), &info);
      if (search != INVALID_HANDLE_VALUE) {
        const SearchHandle search_handle{search};
        do {
          if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
              !DeleteFileW((directory + info.cFileName).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            return FileError();
        } while (FindNextFileW(search, &info));
        if (GetLastError() != ERROR_NO_MORE_FILES) return FileError();
      } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return FileError();
      }
    }
    if (SUCCEEDED(hr)) {
      lock.accounting->bytes = 0;
      lock.accounting->valid = 1;
    }
    return hr;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

HRESULT CreateShaderCacheSession(MTLD3D12Device *device, const D3D12_SHADER_CACHE_SESSION_DESC *desc,
                                 const void *driver_identity, size_t driver_identity_size, REFIID riid, void **out) {
  if (out) *out = nullptr;
  if (!desc || (desc->Mode != D3D12_SHADER_CACHE_MODE_MEMORY && desc->Mode != D3D12_SHADER_CACHE_MODE_DISK) ||
      (UINT(desc->Flags) & ~3u)) return E_INVALIDARG;
  if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3D12Object) && riid != __uuidof(ID3D12DeviceChild) &&
      riid != __uuidof(ID3D12ShaderCacheSession)) return E_NOINTERFACE;
  std::lock_guard lock(cache_mutex);
  std::shared_ptr<Cache> cache;
  {
    if (caches_disabled) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    try {
      std::wstring directory, executable;
      std::wstring token = Hex(&desc->Identifier, sizeof(desc->Identifier));
      if (desc->Flags & D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED) {
        if (!driver_identity || !driver_identity_size) return E_FAIL;
        token += L"-" + Hex(driver_identity, driver_identity_size);
      }
      const std::wstring identity = token;
      for (auto it = registry.begin(); it != registry.end();) {
        if (it->second.expired()) it = registry.erase(it); else ++it;
      }
      auto existing = registry.find(identity);
      if (existing != registry.end()) cache = existing->second.lock();
      if (cache) {
        if (cache->desc.Version != desc->Version || cache->desc.Mode != desc->Mode || cache->desc.Flags != desc->Flags)
          return DXGI_ERROR_ALREADY_EXISTS;
      } else {
        if (!out) return S_FALSE; // A validation-only call must not replace a persisted version.
        cache = std::make_shared<Cache>(*desc);
        if (desc->Mode == D3D12_SHADER_CACHE_MODE_DISK) {
          HRESULT hr = Paths(desc->Flags & D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR, directory, executable);
          if (FAILED(hr)) return hr;
          cache->path = directory + L"dxmt-sc-" + executable + L"-" + token + L".val";
          hr = cache->open();
          if (FAILED(hr)) return hr;
        }
        registry[identity] = cache;
      }
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  if (!out) return S_FALSE;
  try {
    auto session = Com(new ShaderCacheSession(device, *desc, std::move(cache)));
    return session->QueryInterface(riid, out);
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

HRESULT ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control) {
  if (!UINT(kinds) || (UINT(kinds) & ~15u) || !UINT(control) || (UINT(control) & ~7u) ||
      (control & D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE && control & D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE))
    return E_INVALIDARG;
  DWORD developer = 0, size = sizeof(developer);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                  L"AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, nullptr, &developer, &size) != ERROR_SUCCESS ||
      !developer) return E_ACCESSDENIED;
  if (kinds & D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED) {
    const HRESULT hr = AutomaticPipelineCacheControl(control);
    if (FAILED(hr)) return hr;
  }
  // The implicit D3D conversion/runtime caches are absent. Application sessions remain independent.
  if (!(kinds & D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED)) return S_OK;
  std::lock_guard lock(cache_mutex);
  try {
    if (control & D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR) {
      for (auto &[identity, weak] : registry) {
        if (auto cache = weak.lock()) {
          HRESULT hr = cache->clear();
          if (FAILED(hr)) return hr;
        }
      }
      std::wstring directory, executable;
      HRESULT hr = Paths(false, directory, executable);
      if (FAILED(hr)) return hr;
      WIN32_FIND_DATAW info;
      HANDLE search = FindFirstFileW((directory + L"dxmt-sc-*.val").c_str(), &info);
      if (search != INVALID_HANDLE_VALUE) {
        const SearchHandle search_handle{search};
        HRESULT error = S_OK;
        do {
          const std::wstring path = directory + info.cFileName;
          bool active = false;
          for (const auto &[identity, weak] : registry)
            if (auto cache = weak.lock(); cache && cache->path == path) { active = true; break; }
          if (active) continue;
          HANDLE file = CreateFileW(path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (file == INVALID_HANDLE_VALUE) { error = FileError(); break; }
          FILE_DISPOSITION_INFO disposition{TRUE};
          if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) error = FileError();
          CloseHandle(file);
          if (FAILED(error)) break;
        } while (FindNextFileW(search, &info));
        if (SUCCEEDED(error) && GetLastError() != ERROR_NO_MORE_FILES) error = FileError();
        if (FAILED(error)) return error;
      } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return FileError();
    }
    if (control & D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE) caches_disabled = true;
    if (control & D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) caches_disabled = false;
    return S_OK;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

} // namespace dxmt
