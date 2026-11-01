// CPU-only production automatic disk cache, real WinAPI and cross-process coordination.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include "DXBCParser/StageCache.h"
#include "sha1/sha1_util.hpp"
#include "com/com_pointer.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#define WINEMETAL_API extern "C"
#include "../../src/d3d12/d3d12_pipeline_cache.hpp"
#undef WINEMETAL_API
uint64_t enumerations = 0;
uint64_t stage_enumerations = 0;
HANDLE counted_find(const WCHAR *pattern, WIN32_FIND_DATAW *info) {
  ++enumerations;
  if (std::wcsstr(pattern, L"d3dalloy-stage-*")) ++stage_enumerations;
  return FindFirstFileW(pattern, info);
}
namespace dxmt {
HRESULT CreateBlobFromMalloc(SIZE_T size, ID3DBlob **blob) { return D3DCreateBlob(size, blob); }
void PublishAutomaticStageArtifact(const std::string &, std::shared_ptr<const ShaderStageArtifact>, uint64_t) {}
}
#define FindFirstFileW counted_find
#include PRODUCTION_CACHE
#undef FindFirstFileW
using namespace dxmt;
extern "C" bool WMTGetPipelineCacheIdentity(WMTPipelineCacheIdentity *identity) {
  *identity = {}; identity->compiler_uuid[0] = 1; return true;
}
constexpr uint64_t MiB = 1024 * 1024;
void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "AUTOMATIC_CACHE_DISK_FAIL %s error=%lu\n", message, GetLastError()); std::exit(1); }
}
Sha1Digest key(unsigned value) { return Sha1HashState::compute(&value, sizeof(value)); }
std::wstring directory() {
  std::wstring path, token;
  require(Paths(false, path, token) == S_OK, "cache directory");
  return path;
}
std::wstring path(unsigned value) { return directory() + L"d3dalloy-pso-" + Hex(key(value).data, 20) + L".bin"; }
void store(unsigned value, size_t size, unsigned char fill) {
  Com<ID3DBlob> blob;
  require(D3DCreateBlob(size, &blob) == S_OK, "allocate blob");
  std::memset(blob->GetBufferPointer(), fill, size);
  WriteAutomaticPipelineCache(key(value), blob.ptr());
}
void contents(unsigned value, size_t size, unsigned char fill) {
  Com<ID3DBlob> blob;
  require(ReadAutomaticPipelineCache(key(value), &blob) == S_OK, "read publication");
  require(blob->GetBufferSize() == size, "publication byte length");
  const auto *bytes = static_cast<const unsigned char *>(blob->GetBufferPointer());
  require(std::all_of(bytes, bytes + size, [fill](auto byte) { return byte == fill; }), "publication contents");
}
uint64_t disk_bytes() {
  WIN32_FIND_DATAW info;
  HANDLE search = FindFirstFileW((directory() + L"*").c_str(), &info);
  require(search != INVALID_HANDLE_VALUE, "enumerate owned files");
  uint64_t total = 0;
  do {
    bool temporary = false;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && AutomaticFileName(info.cFileName, temporary)) {
      require(!temporary, "no leftover temporary publication");
      total += (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }
  } while (FindNextFileW(search, &info));
  require(GetLastError() == ERROR_NO_MORE_FILES, "complete enumeration");
  FindClose(search);
  require(total <= 256 * MiB, "256 MiB owned-file cap");
  return total;
}
void clear() { require(ClearAutomaticPipelineCacheDisk() == S_OK, "clear automatic cache"); }
void stageStore(unsigned value) {
  ShaderStageArtifact artifact;
  artifact.library = {'M', 'T', 'L', 'B'};
  artifact.metadata_sizes = {0};
  artifact.constants = {0};
  artifact.view.ConstantDataAlignment = 1;
  WriteAutomaticStageArtifact("stage-" + std::to_string(value), artifact);
}
std::wstring stagePath(const std::string &value) {
  const auto digest = Sha1HashState::compute(value.data(), value.size());
  return directory() + L"d3dalloy-stage-" + Hex(digest.data, sizeof(digest.data)) + L".bin";
}
std::vector<uint8_t> stageBytes(const std::string &value) {
  FileHandle file{CreateFileW(stagePath(value).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  require(file.value != INVALID_HANDLE_VALUE, "open stage publication");
  LARGE_INTEGER size{};
  require(GetFileSizeEx(file.value, &size) && size.QuadPart >= 0 && uint64_t(size.QuadPart) <= 8 * MiB,
          "bounded stage publication");
  std::vector<uint8_t> bytes((size_t(size.QuadPart)));
  require(Read(file.value, bytes.data(), DWORD(bytes.size())), "read stage publication bytes");
  return bytes;
}
void replaceStageBytes(const std::string &value, const std::vector<uint8_t> &bytes) {
  FileHandle file{CreateFileW(stagePath(value).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
  require(file.value != INVALID_HANDLE_VALUE && Write(file.value, bytes.data(), DWORD(bytes.size())) &&
              FlushFileBuffers(file.value), "replace owned stage fixture");
}
void stageChecksum(std::vector<uint8_t> &bytes, const DxmtStageCacheLayout &layout) {
  Sha1HashState checksum;
  checksum.update(bytes.data() + layout.checksum_header.offset, size_t(layout.checksum_header.size))
      .update(bytes.data() + layout.checksum_payload.offset, bytes.size() - size_t(layout.checksum_payload.offset));
  const auto digest = checksum.final();
  std::memcpy(bytes.data() + layout.checksum.offset, digest.data, sizeof(digest.data));
}
void testStageCodec() {
  clear();
  const std::string value("stage-\0payload", 14);
  ShaderStageArtifact source;
  source.library = {'M', 'T', 'L', 'B', 0, 0xff, 1};
  source.metadata_sizes = {0, 3, 2};
  source.metadata = {0, 1, 0xff, 3, 4};
  source.constants = {0, UINT64_MAX, 0x0123456789abcdefull, 0xfedcba9876543210ull};
  source.relocations = {{0, 8}, {8, 32}, {16, SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE},
                        {24, SM50_CONSTANT_RELOCATION_TARGET_ATOMIC64_CONTEXT}};
  source.view.ConstantDataAlignment = 256;
  WriteAutomaticStageArtifact(value, source);
  std::shared_ptr<const ShaderStageArtifact> loaded;
  require(ReadAutomaticStageArtifact(value, loaded) == S_OK && loaded, "full stage disk roundtrip");
  require(loaded->library == source.library && loaded->metadata_sizes == source.metadata_sizes &&
              loaded->metadata == source.metadata && loaded->constants == source.constants &&
              loaded->relocations.size() == source.relocations.size() &&
              !std::memcmp(loaded->relocations.data(), source.relocations.data(),
                           source.relocations.size() * sizeof(SM50_CONSTANT_RELOCATION)),
          "stage library metadata constants and relocations roundtrip");
  require(loaded->view.Data == loaded->library.data() && loaded->view.Size == loaded->library.size() &&
              loaded->view.ConstantData == loaded->constants.data() &&
              loaded->view.ConstantDataSize == source.constants.size() * sizeof(uint64_t) &&
              loaded->view.ConstantDataAlignment == 256 &&
              loaded->view.ConstantRelocations == loaded->relocations.data() &&
              loaded->view.ConstantRelocationCount == source.relocations.size(),
          "stage decoded views retain their artifact owner");
  WMTPipelineCacheIdentity identity{};
  require(StageIdentity(identity), "stage test identity");
  const auto original = stageBytes(value);
  DxmtStageCacheLayout layout{};
  require(dxmt_stage_cache_decode(original.data(), original.size(), value.data(), value.size(), &identity, &layout) ==
              DXMT_STAGE_CACHE_SUCCESS, "decode published stage layout");
  require(layout.checksum_header.offset == 0 && layout.checksum_header.size == 104 &&
              layout.checksum_payload.offset == 128 && layout.checksum_payload.size == original.size() - 128 &&
              layout.checksum.offset == 104 && layout.checksum.size == 20,
          "stage version two checksum excludes digest and trailing header padding");
  const DxmtStageCacheParts parts{
      value.data(), source.library.data(), source.metadata_sizes.data(), source.metadata.data(),
      source.constants.data(), source.relocations.data(), &identity,
      value.size(), source.library.size(), source.metadata_sizes.size() * sizeof(uint32_t), source.metadata.size(),
      source.constants.size() * sizeof(uint64_t), source.relocations.size() * sizeof(SM50_CONSTANT_RELOCATION), 256};
  uint64_t encoded_size = 0;
  require(dxmt_stage_cache_measure(&parts, &encoded_size) == DXMT_STAGE_CACHE_SUCCESS && encoded_size == original.size(),
          "stage encoder measures complete payload");
  std::vector<uint8_t> encoded(size_t(encoded_size), 0xa5);
  DxmtStageCacheLayout encoded_layout{};
  require(dxmt_stage_cache_encode(&parts, encoded.data(), encoded.size(), &encoded_layout) == DXMT_STAGE_CACHE_SUCCESS,
          "stage encoder assembles actual artifact regions");
  require(std::all_of(encoded.begin() + 104, encoded.begin() + 128, [](auto byte) { return byte == 0; }),
          "stage encoder initializes checksum and header padding");
  stageChecksum(encoded, encoded_layout);
  require(encoded == original, "stage encoder matches published bytes");
  auto invalid = [&](std::vector<uint8_t> bytes, const char *message) {
    DxmtStageCacheLayout rejected_layout;
    std::memset(&rejected_layout, 0xa5, sizeof(rejected_layout));
    const auto untouched = rejected_layout;
    require(dxmt_stage_cache_decode(bytes.data(), bytes.size(), value.data(), value.size(), &identity, &rejected_layout) !=
                DXMT_STAGE_CACHE_SUCCESS && !std::memcmp(&rejected_layout, &untouched, sizeof(untouched)), message);
    stageChecksum(bytes, layout);
    replaceStageBytes(value, bytes);
    auto rejected = loaded;
    require(ReadAutomaticStageArtifact(value, rejected) == E_FAIL && !rejected, message);
  };
  auto bytes = original;
  const uint32_t huge_count = UINT32_MAX;
  std::memcpy(bytes.data() + layout.metadata_sizes.offset - sizeof(uint32_t), &huge_count, sizeof(huge_count));
  invalid(bytes, "reject independently malformed metadata count without publishing layout");
  bytes = original;
  const uint32_t short_length = 1, long_length = 3;
  std::memcpy(bytes.data() + layout.metadata_sizes.offset + 2 * sizeof(uint32_t), &short_length, sizeof(short_length));
  invalid(bytes, "reject metadata length sum below payload without publishing layout");
  bytes = original;
  std::memcpy(bytes.data() + layout.metadata_sizes.offset + 2 * sizeof(uint32_t), &long_length, sizeof(long_length));
  invalid(bytes, "reject metadata length sum above payload without publishing layout");
  bytes = original;
  const auto metadata_start = size_t(layout.metadata_sizes.offset) - sizeof(uint32_t);
  bytes.erase(bytes.begin() + metadata_start + 3, bytes.begin() + layout.constants.offset);
  const uint64_t missing_count = 3;
  std::memcpy(bytes.data() + 24, &missing_count, sizeof(missing_count));
  invalid(bytes, "reject metadata region shorter than its count field with valid total length");
  bytes = original;
  bytes.pop_back();
  invalid(bytes, "reject truncated stage length without publishing layout");
  bytes = original;
  bytes.push_back(0);
  invalid(bytes, "reject trailing stage bytes without publishing layout");
  bytes = original;
  const uint64_t huge_length = UINT64_MAX;
  std::memcpy(bytes.data() + 16, &huge_length, sizeof(huge_length));
  invalid(bytes, "reject overflowing stage region length without publishing layout");
  bytes = original;
  const uint64_t bad_source = source.constants.size() * sizeof(uint64_t) - 7;
  std::memcpy(bytes.data() + layout.relocations.offset, &bad_source, sizeof(bad_source));
  invalid(bytes, "reject relocation source beyond last complete constant without publishing layout");
  bytes = original;
  const uint64_t bad_target = source.constants.size() * sizeof(uint64_t) + 1;
  std::memcpy(bytes.data() + layout.relocations.offset + sizeof(uint64_t), &bad_target, sizeof(bad_target));
  invalid(bytes, "reject relocation target outside constants and reserved targets without publishing layout");
  bytes = original;
  bytes.insert(bytes.end(), sizeof(SM50_CONSTANT_RELOCATION), 0);
  const uint64_t too_many_relocations = layout.relocations.size + sizeof(SM50_CONSTANT_RELOCATION);
  std::memcpy(bytes.data() + 48, &too_many_relocations, sizeof(too_many_relocations));
  invalid(bytes, "reject relocation count exceeding constant words with valid total length");
  bytes = original;
  bytes[size_t(layout.library.offset)] = 'X';
  invalid(bytes, "reject non MTLB stage without publishing layout");
  bytes = original;
  bytes[56] ^= 1;
  invalid(bytes, "reject stage identity mismatch without publishing layout");
  bytes = original;
  bytes[128] ^= 1;
  invalid(bytes, "reject stage key mismatch without publishing layout");
  bytes = original;
  bytes[size_t(layout.checksum.offset)] ^= 1;
  replaceStageBytes(value, bytes);
  auto rejected = loaded;
  require(ReadAutomaticStageArtifact(value, rejected) == E_FAIL && !rejected, "reject stage checksum mismatch");
  bytes = original;
  std::fill(bytes.begin() + 124, bytes.begin() + 128, 0xa5);
  replaceStageBytes(value, bytes);
  require(ReadAutomaticStageArtifact(value, loaded) == S_OK, "stage header padding remains outside checksum");
  auto invalid_parts = parts;
  const uint32_t invalid_lengths[] = {0, 3, 1};
  invalid_parts.metadata_sizes = invalid_lengths;
  uint64_t untouched_size = UINT64_MAX;
  require(dxmt_stage_cache_measure(&invalid_parts, &untouched_size) == DXMT_STAGE_CACHE_INVALID_DATA &&
              untouched_size == UINT64_MAX, "invalid stage measure leaves size unchanged");
  std::fill(encoded.begin(), encoded.end(), 0xa5);
  const auto untouched_bytes = encoded;
  DxmtStageCacheLayout untouched_layout;
  std::memset(&untouched_layout, 0xa5, sizeof(untouched_layout));
  encoded_layout = untouched_layout;
  require(dxmt_stage_cache_encode(&invalid_parts, encoded.data(), encoded.size(), &encoded_layout) ==
              DXMT_STAGE_CACHE_INVALID_DATA && encoded == untouched_bytes &&
              !std::memcmp(&encoded_layout, &untouched_layout, sizeof(untouched_layout)),
          "invalid stage encode leaves destination and layout unchanged");
  require(dxmt_stage_cache_encode(&parts, encoded.data(), encoded.size() - 1, &encoded_layout) == DXMT_STAGE_CACHE_OUTPUT_SIZE &&
              encoded == untouched_bytes && !std::memcmp(&encoded_layout, &untouched_layout, sizeof(untouched_layout)),
          "undersized stage encode leaves destination and layout unchanged");
  invalid_parts = parts;
  invalid_parts.metadata_sizes = nullptr;
  require(dxmt_stage_cache_encode(&invalid_parts, encoded.data(), encoded.size(), &encoded_layout) ==
              DXMT_STAGE_CACHE_INVALID_ARGUMENT && encoded == untouched_bytes &&
              !std::memcmp(&encoded_layout, &untouched_layout, sizeof(untouched_layout)),
          "null nonempty stage input leaves destination and layout unchanged");
  require(dxmt_stage_cache_file_size(127) != DXMT_STAGE_CACHE_SUCCESS &&
              dxmt_stage_cache_file_size(8 * MiB + 1) != DXMT_STAGE_CACHE_SUCCESS &&
              dxmt_stage_cache_file_size(128) == DXMT_STAGE_CACHE_SUCCESS &&
              dxmt_stage_cache_file_size(8 * MiB) == DXMT_STAGE_CACHE_SUCCESS,
          "stage file bounds checked before allocation");
  source.library[0] = 'X';
  WriteAutomaticStageArtifact(value, source);
  require(stageBytes(value)[size_t(layout.library.offset)] == 'X' &&
              ReadAutomaticStageArtifact(value, loaded) == E_FAIL && !loaded,
          "stage writer retains legacy reader-only library magic check");
  source.library[0] = 'M';
  source.relocations[0].Offset = bad_source;
  WriteAutomaticStageArtifact(value, source);
  bytes = stageBytes(value);
  uint64_t encoded_source = 0;
  std::memcpy(&encoded_source, bytes.data() + layout.relocations.offset, sizeof(encoded_source));
  require(encoded_source == bad_source && ReadAutomaticStageArtifact(value, loaded) == E_FAIL && !loaded,
          "stage writer retains legacy reader-only relocation check");
  source.relocations[0].Offset = 0;
  source.view.ConstantDataAlignment = uint64_t(1) << 32;
  WriteAutomaticStageArtifact(value, source);
  bytes = stageBytes(value);
  uint64_t encoded_alignment = 0;
  std::memcpy(&encoded_alignment, bytes.data() + 40, sizeof(encoded_alignment));
  require(encoded_alignment == source.view.ConstantDataAlignment &&
              ReadAutomaticStageArtifact(value, loaded) == E_FAIL && !loaded,
          "stage writer retains legacy reader-only alignment limit");
  source.library = {'M', 'T', 'L', 'B'};
  source.metadata.clear(); source.metadata_sizes.clear();
  source.constants.clear(); source.relocations.clear();
  source.view.ConstantDataAlignment = 1;
  WriteAutomaticStageArtifact(value, source);
  require(ReadAutomaticStageArtifact(value, loaded) == S_OK && loaded->metadata_sizes.empty() && loaded->metadata.empty() &&
              loaded->constants.empty() && loaded->relocations.empty() && !loaded->view.ConstantData &&
              !loaded->view.ConstantRelocations && loaded->view.ConstantDataAlignment == 1,
          "empty stage regions and zero metadata records roundtrip");
  clear();
  std::puts("STAGE_CACHE_DISK_PASS full-codec-roundtrip-and-malformed-inputs");
}
void testStageAccounting() {
  stage_enumerations = 0;
  stageStore(1);
  stageStore(2);
  require(stage_enumerations == 1, "stage publication rescans directory once");
  {
    const auto cache_directory = directory();
    AutomaticDiskLock lock;
    require(lock.acquire(cache_directory) == S_OK, "inspect accounting mappings");
    require(lock.accounting != lock.stage_accounting, "whole-PSO and stage accounting remain separate");
    AutomaticDiskAccounting *whole = nullptr, *stage = nullptr;
    require(AutomaticAccounting(cache_directory, whole) == S_OK && whole == lock.accounting,
            "whole-PSO registry retains its mapping");
    require(StageAccounting(cache_directory, stage) == S_OK && stage == lock.stage_accounting,
            "stage registry retains its mapping");
    const auto hash = Sha1HashState::compute(cache_directory.data(), cache_directory.size() * sizeof(WCHAR));
    const auto suffix = Hex(hash.data, sizeof(hash.data));
    const struct {
      const WCHAR *prefix;
      AutomaticDiskAccounting *state;
    } expected[] = {{L"Local\\d3dalloy-pso-accounting-v1-", whole}, {L"Local\\d3dalloy-stage-accounting-v1-", stage}};
    for (const auto &entry : expected) {
      AutomaticAccountingMapping mapping;
      mapping.handle = OpenFileMappingW(FILE_MAP_READ, FALSE, (entry.prefix + suffix).c_str());
      require(mapping.handle != nullptr, "accounting mapping keeps its cross-process name");
      mapping.state = static_cast<AutomaticDiskAccounting *>(
          MapViewOfFile(mapping.handle, FILE_MAP_READ, 0, 0, sizeof(AutomaticDiskAccounting)));
      require(mapping.state && mapping.state->bytes == entry.state->bytes && mapping.state->valid == entry.state->valid,
              "named mapping exposes the retained accounting state");
    }
  }
  std::printf("STAGE_CACHE_DISK_OPERATIONS publications=2 stage_metadata_enumerations=%llu\n",
              static_cast<unsigned long long>(stage_enumerations));
  clear();
}
void child(const std::wstring &arguments) {
  WCHAR executable[32768];
  require(GetModuleFileNameW(nullptr, executable, std::size(executable)) != 0, "child executable");
  std::wstring command = L"\"" + std::wstring(executable) + L"\" " + arguments;
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process), "start child");
  CloseHandle(process.hThread);
  require(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0, "child completion signal");
  DWORD code = 1;
  require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "child successful exit");
  CloseHandle(process.hProcess);
}
int main(int argc, char **argv) {
  if (argc > 1) {
    if (!std::strcmp(argv[1], "--operations")) {
      clear();
      enumerations = 0;
      for (unsigned value = 100; value < 356; ++value) store(value, 1024, value & 255);
      require(disk_bytes() == 256 * 1024, "small publication workload bytes");
      std::printf("AUTOMATIC_CACHE_DISK_OPERATIONS stores=256 metadata_enumerations=%llu\n", static_cast<unsigned long long>(enumerations));
      clear();
    } else if (!std::strcmp(argv[1], "--write")) {
      const unsigned value = std::strtoul(argv[2], nullptr, 10);
      const size_t size = std::strtoull(argv[3], nullptr, 10);
      store(value, size, value & 255); contents(value, size, value & 255);
    } else if (!std::strcmp(argv[1], "--clear")) {
      clear();
    } else if (!std::strcmp(argv[1], "--abandon")) {
      AutomaticDiskLock lock;
      require(lock.acquire(directory()) == S_OK, "abandoned writer acquires mutex");
      const auto temporary = directory() + L"d3dalloy-pso-0000000000000000000000000000000000000000-00000000-00000000.tmp";
      HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(file != INVALID_HANDLE_VALUE, "abandoned writer temp");
      DWORD done = 0;
      require(WriteFile(file, "partial", 7, &done, nullptr) && done == 7, "abandoned partial write");
      HANDLE published = CreateFileW(path(40).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(published != INVALID_HANDLE_VALUE, "crashed writer publication");
      LARGE_INTEGER size; size.QuadPart = 64 * MiB;
      require(SetFilePointerEx(published, size, nullptr, FILE_BEGIN) && SetEndOfFile(published) && FlushFileBuffers(published),
              "publication committed before accounting update");
      ExitProcess(0); // Deliberately bypass RAII while owning the real named mutex.
    } else return 2;
    return 0;
  }
  testStageAccounting();
  testStageCodec();
  clear();
  const auto foreign = directory() + L"d3dalloy-pso-not-owned.bin";
  HANDLE foreign_file = CreateFileW(foreign.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(foreign_file != INVALID_HANDLE_VALUE, "create foreign file");
  DWORD done = 0;
  require(WriteFile(foreign_file, "foreign", 7, &done, nullptr) && done == 7, "foreign contents");
  CloseHandle(foreign_file);
  store(1, 4096, 1); contents(1, 4096, 1);
  store(1, 8192, 2); contents(1, 8192, 2);
  require(disk_bytes() == 8192, "growing replacement exact bytes");
  store(1, 1024, 3); contents(1, 1024, 3);
  require(disk_bytes() == 1024, "shrinking replacement exact bytes");
  RemoveAutomaticPipelineCache(key(1));
  require(disk_bytes() == 0, "remove exact bytes");
  clear();
  for (unsigned value = 1; value <= 4; ++value) {
    store(value, 64 * MiB, value);
    HANDLE file = CreateFileW(path(value).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "set deterministic eviction order");
    const FILETIME age{value, 1};
    require(SetFileTime(file, nullptr, nullptr, &age), "deterministic publication age");
    CloseHandle(file);
  }
  require(disk_bytes() == 256 * MiB, "fill exact cap");
  store(4, 64 * MiB, 9);
  contents(4, 64 * MiB, 9);
  for (unsigned value = 1; value <= 3; ++value) contents(value, 64 * MiB, value);
  require(disk_bytes() == 256 * MiB, "same-size replacement must not evict other keys");
  std::puts("AUTOMATIC_CACHE_DISK_PASS replacement-at-cap");
  store(5, 16 * MiB, 5); contents(5, 16 * MiB, 5); disk_bytes();
  clear();
  store(10, 64 * MiB, 10); // Keep this process's accounting live across child writes and clear.
  child(L"--write 11 67108864"); child(L"--write 12 67108864"); child(L"--write 13 67108864");
  require(disk_bytes() == 256 * MiB, "cross-process exact bytes");
  store(14, 16 * MiB, 14); contents(14, 16 * MiB, 14); disk_bytes();
  child(L"--clear");
  require(disk_bytes() == 0, "child clear visible to retained writer");
  for (unsigned value = 20; value < 24; ++value) store(value, 64 * MiB, value);
  for (unsigned value = 20; value < 24; ++value) contents(value, 64 * MiB, value);
  require(disk_bytes() == 256 * MiB, "writer accounts after foreign-process clear");
  std::puts("AUTOMATIC_CACHE_DISK_PASS cross-process-accounting-and-clear");
  clear(); store(30, 4096, 30);
  HANDLE pinned = CreateFileW(path(30).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(pinned != INVALID_HANDLE_VALUE, "pin publication against rename");
  store(30, 8192, 31); contents(30, 4096, 30);
  require(disk_bytes() == 4096, "failed rename preserves previous contents and bytes");
  CloseHandle(pinned);
  store(30, 8192, 31); contents(30, 8192, 31);
  require(disk_bytes() == 8192, "retry after failed publish");
  const auto cache_directory = directory();
  const auto hash = Sha1HashState::compute(cache_directory.data(), cache_directory.size() * sizeof(WCHAR));
  const auto mutex_name = L"Local\\d3dalloy-pso-" + Hex(hash.data, 20);
  HANDLE keeper = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
  require(keeper != nullptr, "retain abandoned mutex");
  child(L"--abandon");
  store(31, 4096, 31); contents(31, 4096, 31);
  require(disk_bytes() == 64 * MiB + 12288, "recover abandoned publication and stale temp");
  contents(40, 64 * MiB, 0);
  for (unsigned value = 41; value <= 44; ++value) { store(value, 64 * MiB, value); disk_bytes(); }
  CloseHandle(keeper);
  std::puts("AUTOMATIC_CACHE_DISK_PASS failed-publication-and-abandoned-writer");
  clear();
  HANDLE preserved = CreateFileW(foreign.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(preserved != INVALID_HANDLE_VALUE, "foreign file survives eviction and clear");
  char bytes[7]{};
  require(ReadFile(preserved, bytes, 7, &done, nullptr) && done == 7 && !std::memcmp(bytes, "foreign", 7), "foreign bytes preserved");
  CloseHandle(preserved); require(DeleteFileW(foreign.c_str()), "remove own foreign fixture");
  clear();
  std::puts("AUTOMATIC_CACHE_DISK_PASS all invariants");
  return 0;
}
