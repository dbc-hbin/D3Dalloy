// CPU-only production automatic disk cache, real WinAPI and cross-process coordination.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
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
  if (std::wcsstr(pattern, L"dxmt-stage-*")) ++stage_enumerations;
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
std::wstring path(unsigned value) { return directory() + L"dxmt-pso-" + Hex(key(value).data, 20) + L".bin"; }
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
void testStageAccounting() {
  stage_enumerations = 0;
  stageStore(1);
  stageStore(2);
  require(stage_enumerations == 1, "stage publication rescans directory once");
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
      const auto temporary = directory() + L"dxmt-pso-0000000000000000000000000000000000000000-00000000-00000000.tmp";
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
  clear();
  const auto foreign = directory() + L"dxmt-pso-not-owned.bin";
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
  const auto mutex_name = L"Local\\dxmt-pso-" + Hex(hash.data, 20);
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
