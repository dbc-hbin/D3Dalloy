// CPU cache API proof through the real owned Device9, including fresh-process disk reopen.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
[[noreturn]] void fail(const char *step, HRESULT got = E_FAIL, HRESULT wanted = S_OK) {
  std::fprintf(stderr, "OWNED_D3D12_SHADER_CACHE_FAIL step=%s actual=%08lx expected=%08lx\n", step,
               static_cast<unsigned long>(got), static_cast<unsigned long>(wanted));
  throw std::runtime_error(step);
}
void expect(HRESULT got, HRESULT wanted, const char *step) { if (got != wanted) fail(step, got, wanted); }
void check(HRESULT got, const char *step) { if (FAILED(got)) fail(step, got); }
void passed(const char *name) { std::printf("OWNED_D3D12_SHADER_CACHE_CASE_PASS %s\n", name); std::fflush(stdout); }
ComPtr<ID3D12Device9> device() {
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  ComPtr<ID3D12Device9> result;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&result)), "device9");
  return result;
}
D3D12_SHADER_CACHE_SESSION_DESC desc(UINT identifier = 1, bool disk = false, bool temporary = false) {
  D3D12_SHADER_CACHE_SESSION_DESC result{};
  result.Identifier = {0xe03bb121 + identifier, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
  result.Mode = disk ? D3D12_SHADER_CACHE_MODE_DISK : D3D12_SHADER_CACHE_MODE_MEMORY;
  result.Flags = disk && !temporary ? D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR : D3D12_SHADER_CACHE_FLAG_NONE;
  result.MaximumInMemoryCacheSizeBytes = 1024;
  result.MaximumInMemoryCacheEntries = 128;
  result.MaximumValueFileSizeBytes = 65536;
  result.Version = 7;
  return result;
}
ComPtr<ID3D12ShaderCacheSession> session(ID3D12Device9 *dev, const D3D12_SHADER_CACHE_SESSION_DESC &d) {
  ComPtr<ID3D12ShaderCacheSession> result;
  check(dev->CreateShaderCacheSession(&d, IID_PPV_ARGS(&result)), "session");
  return result;
}
const unsigned char key[] = {0, 0x80, 0xff, 0, 0x17};
const unsigned char value[] = {0xff, 0, 1, 2, 0x80, 0, 0x33};
void retrieve(ID3D12ShaderCacheSession *cache) {
  UINT size = 0;
  expect(cache->FindValue(key, sizeof(key), nullptr, &size), S_OK, "size query");
  if (size != sizeof(value)) fail("required value size");
  unsigned char output[sizeof(value)];
  memset(output, 0x5a, sizeof(output));
  size = sizeof(output) - 1;
  expect(cache->FindValue(key, sizeof(key), output, &size), DXGI_ERROR_MORE_DATA, "short buffer");
  for (auto byte : output) if (byte != 0x5a) fail("short buffer modified");
  if (size != sizeof(value)) fail("short required size");
  size = sizeof(output);
  check(cache->FindValue(key, sizeof(key), output, &size), "retrieve");
  if (size != sizeof(value) || memcmp(output, value, size)) fail("binary value mismatch");
}
void missing(ID3D12ShaderCacheSession *cache) {
  UINT size = 17;
  expect(cache->FindValue(key, sizeof(key), nullptr, &size), DXGI_ERROR_NOT_FOUND, "missing key");
}
void child(const wchar_t *argument, bool different_executable = false) {
  wchar_t self[32768];
  if (!GetModuleFileNameW(nullptr, self, 32768)) fail("self path");
  std::wstring executable = self;
  if (different_executable) {
    executable += L".isolated.exe";
    if (!CopyFileW(self, executable.c_str(), FALSE)) fail("copy isolated executable");
  }
  std::wstring command = L"\"" + executable + L"\" " + argument;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    fail("child create", HRESULT_FROM_WIN32(GetLastError()));
  DWORD wait = WaitForSingleObject(process.hProcess, 60000), code = 1;
  if (wait == WAIT_TIMEOUT) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE); }
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (different_executable) DeleteFileW(executable.c_str());
  if (wait != WAIT_OBJECT_0 || code) fail("child result");
}
void concurrent(ID3D12ShaderCacheSession *cache) {
  std::atomic<unsigned> winners{0}, duplicates{0}, errors{0};
  std::vector<std::thread> threads;
  const UINT shared_key = 0xc0111de;
  for (UINT i = 0; i < 8; ++i) threads.emplace_back([&, i] {
    HRESULT hr = cache->StoreValue(&shared_key, sizeof(shared_key), &i, sizeof(i));
    if (hr == S_OK) ++winners; else if (hr == DXGI_ERROR_ALREADY_EXISTS) ++duplicates; else ++errors;
    const UINT own_key = 0x80000000 + i;
    if (cache->StoreValue(&own_key, sizeof(own_key), &i, sizeof(i)) != S_OK) ++errors;
    UINT output = 99, size = sizeof(output);
    if (cache->FindValue(&own_key, sizeof(own_key), &output, &size) != S_OK || output != i) ++errors;
  });
  for (auto &thread : threads) thread.join();
  if (winners != 1 || duplicates != 7 || errors) fail("concurrent duplicates/data");
}
struct DeveloperMode {
  HKEY key = nullptr;
  DWORD previous = 0;
  bool existed = false;
  DeveloperMode() {
    char isolated[2]{};
    if (GetEnvironmentVariableA("OWNED_SHADER_CACHE_CONTROL_ISOLATED", isolated, sizeof(isolated)) != 1 ||
        isolated[0] != '1') fail("control requires isolated runner prefix");
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock", 0,
        nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr)) fail("developer registry open");
    DWORD type = 0, size = sizeof(previous);
    existed = RegQueryValueExW(key, L"AllowDevelopmentWithoutDevLicense", nullptr, &type,
                              reinterpret_cast<BYTE *>(&previous), &size) == ERROR_SUCCESS;
    if (existed && (type != REG_DWORD || size != sizeof(previous))) fail("unexpected developer registry type");
  }
  void set(DWORD enabled) {
    if (RegSetValueExW(key, L"AllowDevelopmentWithoutDevLicense", 0, REG_DWORD,
                      reinterpret_cast<const BYTE *>(&enabled), sizeof(enabled))) fail("developer registry set");
  }
  ~DeveloperMode() {
    if (existed) RegSetValueExW(key, L"AllowDevelopmentWithoutDevLicense", 0, REG_DWORD,
                               reinterpret_cast<const BYTE *>(&previous), sizeof(previous));
    else RegDeleteValueW(key, L"AllowDevelopmentWithoutDevLicense");
    RegCloseKey(key);
  }
};
void memory(ID3D12Device9 *dev) {
  auto d = desc();
  auto a = session(dev, d);
  check(a->StoreValue(key, sizeof(key), value, sizeof(value)), "memory store");
  expect(a->StoreValue(key, sizeof(key), value, sizeof(value)), DXGI_ERROR_ALREADY_EXISTS, "duplicate");
  retrieve(a.Get());
  auto other_device = device();
  auto b = session(other_device.Get(), d);
  retrieve(b.Get());
  ComPtr<ID3D12Device9> owner;
  check(b->GetDevice(IID_PPV_ARGS(&owner)), "get device");
  if (owner.Get() != other_device.Get()) fail("wrong device owner");
  owner.Reset(); other_device.Reset();
  check(b->GetDevice(IID_PPV_ARGS(&owner)), "retained device lifetime");
  const auto returned = b->GetDesc();
  if (returned.Identifier != d.Identifier || returned.Version != d.Version || returned.Mode != d.Mode ||
      returned.Flags != d.Flags || returned.MaximumInMemoryCacheSizeBytes != d.MaximumInMemoryCacheSizeBytes ||
      returned.MaximumInMemoryCacheEntries != d.MaximumInMemoryCacheEntries ||
      returned.MaximumValueFileSizeBytes != d.MaximumValueFileSizeBytes) fail("description");
  d.Version++;
  void *out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&d, __uuidof(ID3D12ShaderCacheSession), &out), DXGI_ERROR_ALREADY_EXISTS,
         "live version conflict");
  if (out) fail("conflict output");
  expect(dev->CreateShaderCacheSession(nullptr, __uuidof(ID3D12ShaderCacheSession), &out), E_INVALIDARG, "null desc");
  if (out) fail("null desc output");
  auto invalid = desc(99); invalid.Mode = D3D12_SHADER_CACHE_MODE(2); out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&invalid, __uuidof(ID3D12ShaderCacheSession), &out), E_INVALIDARG, "invalid mode");
  if (out) fail("invalid mode output");
  invalid = desc(99); invalid.Flags = D3D12_SHADER_CACHE_FLAGS(4); out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&invalid, __uuidof(ID3D12ShaderCacheSession), &out), E_INVALIDARG, "invalid flags");
  if (out) fail("invalid flags output");
  invalid = desc(99); out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&invalid, __uuidof(ID3D12Resource), &out), E_NOINTERFACE, "unsupported iid");
  if (out) fail("unsupported iid output");
  UINT size = 0;
  expect(a->FindValue(nullptr, 1, nullptr, &size), E_INVALIDARG, "null key");
  expect(a->FindValue(key, sizeof(key), nullptr, nullptr), E_INVALIDARG, "null size");
  expect(a->StoreValue(key, sizeof(key), nullptr, 1), E_INVALIDARG, "null value");
  check(a->StoreValue(nullptr, 0, nullptr, 0), "empty bytes store");
  check(a->FindValue(nullptr, 0, nullptr, &size), "empty bytes find");
  if (size) fail("empty bytes size");
  concurrent(a.Get());
  a.Reset(); b.Reset();
  auto empty = session(dev, desc());
  missing(empty.Get());
  d = desc(2); d.MaximumInMemoryCacheSizeBytes = 128; d.MaximumInMemoryCacheEntries = 1;
  auto bounded = session(dev, d);
  check(bounded->StoreValue(key, sizeof(key), value, sizeof(value)), "memory entry bound");
  expect(bounded->StoreValue("k", 1, "v", 1), DXGI_ERROR_CACHE_FULL, "entry full");
  retrieve(bounded.Get());
  d = desc(3); d.MaximumInMemoryCacheSizeBytes = sizeof(key) + sizeof(value) - 1;
  bounded = session(dev, d);
  expect(bounded->StoreValue(key, sizeof(key), value, sizeof(value)), DXGI_ERROR_CACHE_FULL, "byte bound");
  d = desc(4); d.MaximumInMemoryCacheSizeBytes = sizeof(key) + sizeof(value);
  bounded = session(dev, d);
  check(bounded->StoreValue(key, sizeof(key), value, sizeof(value)), "exact byte bound");
  expect(bounded->StoreValue("k", 1, "v", 1), DXGI_ERROR_CACHE_FULL, "byte full");
  passed("memory-shared-binary-limits-errors-concurrency");
}
void disk(ID3D12Device9 *dev) {
  // SHAttered's 320-byte collision blocks: https://shattered.io/static/shattered-{1,2}.pdf
  const unsigned char collision_a[] = {
    0x25,0x50,0x44,0x46,0x2d,0x31,0x2e,0x33,0x0a,0x25,0xe2,0xe3,0xcf,0xd3,0x0a,0x0a,
    0x0a,0x31,0x20,0x30,0x20,0x6f,0x62,0x6a,0x0a,0x3c,0x3c,0x2f,0x57,0x69,0x64,0x74,
    0x68,0x20,0x32,0x20,0x30,0x20,0x52,0x2f,0x48,0x65,0x69,0x67,0x68,0x74,0x20,0x33,
    0x20,0x30,0x20,0x52,0x2f,0x54,0x79,0x70,0x65,0x20,0x34,0x20,0x30,0x20,0x52,0x2f,
    0x53,0x75,0x62,0x74,0x79,0x70,0x65,0x20,0x35,0x20,0x30,0x20,0x52,0x2f,0x46,0x69,
    0x6c,0x74,0x65,0x72,0x20,0x36,0x20,0x30,0x20,0x52,0x2f,0x43,0x6f,0x6c,0x6f,0x72,
    0x53,0x70,0x61,0x63,0x65,0x20,0x37,0x20,0x30,0x20,0x52,0x2f,0x4c,0x65,0x6e,0x67,
    0x74,0x68,0x20,0x38,0x20,0x30,0x20,0x52,0x2f,0x42,0x69,0x74,0x73,0x50,0x65,0x72,
    0x43,0x6f,0x6d,0x70,0x6f,0x6e,0x65,0x6e,0x74,0x20,0x38,0x3e,0x3e,0x0a,0x73,0x74,
    0x72,0x65,0x61,0x6d,0x0a,0xff,0xd8,0xff,0xfe,0x00,0x24,0x53,0x48,0x41,0x2d,0x31,
    0x20,0x69,0x73,0x20,0x64,0x65,0x61,0x64,0x21,0x21,0x21,0x21,0x21,0x85,0x2f,0xec,
    0x09,0x23,0x39,0x75,0x9c,0x39,0xb1,0xa1,0xc6,0x3c,0x4c,0x97,0xe1,0xff,0xfe,0x01,
    0x73,0x46,0xdc,0x91,0x66,0xb6,0x7e,0x11,0x8f,0x02,0x9a,0xb6,0x21,0xb2,0x56,0x0f,
    0xf9,0xca,0x67,0xcc,0xa8,0xc7,0xf8,0x5b,0xa8,0x4c,0x79,0x03,0x0c,0x2b,0x3d,0xe2,
    0x18,0xf8,0x6d,0xb3,0xa9,0x09,0x01,0xd5,0xdf,0x45,0xc1,0x4f,0x26,0xfe,0xdf,0xb3,
    0xdc,0x38,0xe9,0x6a,0xc2,0x2f,0xe7,0xbd,0x72,0x8f,0x0e,0x45,0xbc,0xe0,0x46,0xd2,
    0x3c,0x57,0x0f,0xeb,0x14,0x13,0x98,0xbb,0x55,0x2e,0xf5,0xa0,0xa8,0x2b,0xe3,0x31,
    0xfe,0xa4,0x80,0x37,0xb8,0xb5,0xd7,0x1f,0x0e,0x33,0x2e,0xdf,0x93,0xac,0x35,0x00,
    0xeb,0x4d,0xdc,0x0d,0xec,0xc1,0xa8,0x64,0x79,0x0c,0x78,0x2c,0x76,0x21,0x56,0x60,
    0xdd,0x30,0x97,0x91,0xd0,0x6b,0xd0,0xaf,0x3f,0x98,0xcd,0xa4,0xbc,0x46,0x29,0xb1,
  };
  const unsigned char collision_b[] = {
    0x25,0x50,0x44,0x46,0x2d,0x31,0x2e,0x33,0x0a,0x25,0xe2,0xe3,0xcf,0xd3,0x0a,0x0a,
    0x0a,0x31,0x20,0x30,0x20,0x6f,0x62,0x6a,0x0a,0x3c,0x3c,0x2f,0x57,0x69,0x64,0x74,
    0x68,0x20,0x32,0x20,0x30,0x20,0x52,0x2f,0x48,0x65,0x69,0x67,0x68,0x74,0x20,0x33,
    0x20,0x30,0x20,0x52,0x2f,0x54,0x79,0x70,0x65,0x20,0x34,0x20,0x30,0x20,0x52,0x2f,
    0x53,0x75,0x62,0x74,0x79,0x70,0x65,0x20,0x35,0x20,0x30,0x20,0x52,0x2f,0x46,0x69,
    0x6c,0x74,0x65,0x72,0x20,0x36,0x20,0x30,0x20,0x52,0x2f,0x43,0x6f,0x6c,0x6f,0x72,
    0x53,0x70,0x61,0x63,0x65,0x20,0x37,0x20,0x30,0x20,0x52,0x2f,0x4c,0x65,0x6e,0x67,
    0x74,0x68,0x20,0x38,0x20,0x30,0x20,0x52,0x2f,0x42,0x69,0x74,0x73,0x50,0x65,0x72,
    0x43,0x6f,0x6d,0x70,0x6f,0x6e,0x65,0x6e,0x74,0x20,0x38,0x3e,0x3e,0x0a,0x73,0x74,
    0x72,0x65,0x61,0x6d,0x0a,0xff,0xd8,0xff,0xfe,0x00,0x24,0x53,0x48,0x41,0x2d,0x31,
    0x20,0x69,0x73,0x20,0x64,0x65,0x61,0x64,0x21,0x21,0x21,0x21,0x21,0x85,0x2f,0xec,
    0x09,0x23,0x39,0x75,0x9c,0x39,0xb1,0xa1,0xc6,0x3c,0x4c,0x97,0xe1,0xff,0xfe,0x01,
    0x7f,0x46,0xdc,0x93,0xa6,0xb6,0x7e,0x01,0x3b,0x02,0x9a,0xaa,0x1d,0xb2,0x56,0x0b,
    0x45,0xca,0x67,0xd6,0x88,0xc7,0xf8,0x4b,0x8c,0x4c,0x79,0x1f,0xe0,0x2b,0x3d,0xf6,
    0x14,0xf8,0x6d,0xb1,0x69,0x09,0x01,0xc5,0x6b,0x45,0xc1,0x53,0x0a,0xfe,0xdf,0xb7,
    0x60,0x38,0xe9,0x72,0x72,0x2f,0xe7,0xad,0x72,0x8f,0x0e,0x49,0x04,0xe0,0x46,0xc2,
    0x30,0x57,0x0f,0xe9,0xd4,0x13,0x98,0xab,0xe1,0x2e,0xf5,0xbc,0x94,0x2b,0xe3,0x35,
    0x42,0xa4,0x80,0x2d,0x98,0xb5,0xd7,0x0f,0x2a,0x33,0x2e,0xc3,0x7f,0xac,0x35,0x14,
    0xe7,0x4d,0xdc,0x0f,0x2c,0xc1,0xa8,0x74,0xcd,0x0c,0x78,0x30,0x5a,0x21,0x56,0x64,
    0x61,0x30,0x97,0x89,0x60,0x6b,0xd0,0xbf,0x3f,0x98,0xcd,0xa8,0x04,0x46,0x29,0xa1,
  };
  for (bool disk_mode : {false, true}) {
    auto collision_desc = desc(disk_mode ? 14 : 13, disk_mode);
    if (disk_mode) collision_desc.MaximumInMemoryCacheSizeBytes = 4;
    auto collision_cache = session(dev, collision_desc);
    check(collision_cache->StoreValue(collision_a, sizeof(collision_a), value, sizeof(value)), "collision seed");
    expect(collision_cache->StoreValue(collision_b, sizeof(collision_b), value, sizeof(value)),
           DXGI_ERROR_CACHE_HASH_COLLISION, "store real collision");
    UINT size = 0;
    expect(collision_cache->FindValue(collision_b, sizeof(collision_b), nullptr, &size),
           DXGI_ERROR_CACHE_HASH_COLLISION, "find real collision");
    collision_cache->SetDeleteOnDestroy();
  }
  auto d = desc(10, true);
  d.MaximumInMemoryCacheSizeBytes = 4; d.MaximumInMemoryCacheEntries = 1;
  auto a = session(dev, d);
  check(a->StoreValue(key, sizeof(key), value, sizeof(value)), "disk store bypass lookaside");
  retrieve(a.Get());
  child(L"--contend");
  concurrent(a.Get());
  a.Reset();
  auto probe = d; probe.Version++;
  expect(dev->CreateShaderCacheSession(&probe, __uuidof(ID3D12ShaderCacheSession), nullptr), S_FALSE, "version probe");
  child(L"--restart");
  child(L"--isolated", true);
  a = session(dev, d);
  retrieve(a.Get());
  UINT marker = 0, size = sizeof(marker);
  check(a->FindValue("child", 5, &marker, &size), "child committed store");
  if (marker != 0x12345678) fail("child marker");
  auto different = session(dev, desc(11, true));
  missing(different.Get());
  different->SetDeleteOnDestroy(); different.Reset();
  auto driver_desc = d;
  driver_desc.Flags = D3D12_SHADER_CACHE_FLAGS(driver_desc.Flags | D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED);
  auto driver = session(dev, driver_desc);
  missing(driver.Get());
  check(driver->StoreValue(key, sizeof(key), value, sizeof(value)), "driver-versioned store");
  driver.Reset(); driver = session(dev, driver_desc); retrieve(driver.Get());
  driver->SetDeleteOnDestroy(); driver.Reset();
  auto next = d; next.Version++;
  void *out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&next, __uuidof(ID3D12ShaderCacheSession), &out), DXGI_ERROR_ALREADY_EXISTS,
         "disk live version conflict");
  if (out) fail("disk conflict output");
  a.Reset();
  auto replaced = session(dev, next);
  missing(replaced.Get());
  check(replaced->StoreValue(key, sizeof(key), value, sizeof(value)), "new version store");
  replaced.Reset();
  a = session(dev, d); missing(a.Get());
  check(a->StoreValue(key, sizeof(key), value, sizeof(value)), "deletion store");
  auto b = session(dev, d);
  a->SetDeleteOnDestroy(); a.Reset(); retrieve(b.Get());
  b.Reset(); child(L"--deleted");
  d = desc(12, true); d.MaximumValueFileSizeBytes = 16 + 48 + sizeof(key) + sizeof(value);
  a = session(dev, d);
  check(a->StoreValue(key, sizeof(key), value, sizeof(value)), "exact disk bound");
  expect(a->StoreValue("k", 1, "v", 1), DXGI_ERROR_CACHE_FULL, "disk full");
  a->SetDeleteOnDestroy(); a.Reset();
  d = desc(15, true); d.MaximumInMemoryCacheEntries = 1;
  a = session(dev, d);
  check(a->StoreValue(key, sizeof(key), value, sizeof(value)), "LRU seed");
  check(a->StoreValue("evict", 5, "v", 1), "LRU eviction");
  retrieve(a.Get());
  expect(a->StoreValue(key, sizeof(key), value, sizeof(value)), DXGI_ERROR_ALREADY_EXISTS, "evicted duplicate");
  a->SetDeleteOnDestroy(); a.Reset();
  passed("disk-restart-isolation-version-delete-limits-concurrency-lock");
}
void control(ID3D12Device9 *dev) {
  DeveloperMode developer;
  constexpr auto kinds = D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED;
  developer.set(0);
  expect(dev->ShaderCacheControl(kinds, D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR), E_ACCESSDENIED, "developer denial");
  developer.set(1);
  expect(dev->ShaderCacheControl(kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS(3)), E_INVALIDARG, "enable disable conflict");
  expect(dev->ShaderCacheControl(kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS(0)), E_INVALIDARG, "no control");
  expect(dev->ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS(16), D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR),
         E_INVALIDARG, "unknown kind");
  auto a = session(dev, desc(20));
  auto active_disk = session(dev, desc(21, true));
  auto inactive = session(dev, desc(22, true));
  auto temporary = session(dev, desc(23, true, true));
  for (auto *cache : {a.Get(), active_disk.Get(), inactive.Get(), temporary.Get()})
    check(cache->StoreValue(key, sizeof(key), value, sizeof(value)), "control seed");
  inactive.Reset(); temporary.Reset();
  check(dev->ShaderCacheControl(kinds, D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE), "disable");
  UINT size = 0;
  expect(a->FindValue(key, sizeof(key), nullptr, &size), DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "disabled find");
  expect(active_disk->StoreValue("k", 1, "v", 1), DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "disabled store");
  void *out = reinterpret_cast<void *>(1);
  auto d = desc(24);
  expect(dev->CreateShaderCacheSession(&d, __uuidof(ID3D12ShaderCacheSession), &out),
         DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "disabled create");
  if (out) fail("disabled output");
  check(dev->ShaderCacheControl(kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS(D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE |
        D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)), "clear enable");
  missing(a.Get()); missing(active_disk.Get());
  active_disk.Reset(); active_disk = session(dev, desc(21, true)); missing(active_disk.Get());
  child(L"--control-inactive");
  inactive = session(dev, desc(22, true)); retrieve(inactive.Get());
  temporary = session(dev, desc(23, true, true)); missing(temporary.Get());
  for (auto *cache : {active_disk.Get(), inactive.Get(), temporary.Get()}) cache->SetDeleteOnDestroy();
  passed("developer-control-disable-enable-clear-active-inactive");
}
void corruption(ID3D12Device9 *dev) {
  auto d = desc(30, true);
  auto cache = session(dev, d);
  check(cache->StoreValue(key, sizeof(key), value, sizeof(value)), "corrupt seed");
  cache.Reset();
  const unsigned char *guid = reinterpret_cast<const unsigned char *>(&d.Identifier);
  wchar_t token[33];
  for (unsigned i = 0; i < 16; ++i) swprintf(token + i * 2, 3, L"%02x", guid[i]);
  const std::wstring pattern = L"dxmt-sc-*-" + std::wstring(token) + L".val";
  WIN32_FIND_DATAW info{};
  HANDLE search = FindFirstFileW(pattern.c_str(), &info);
  if (search == INVALID_HANDLE_VALUE) fail("corrupt file locate");
  FindClose(search);
  HANDLE file = CreateFileW(info.cFileName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail("corrupt file open");
  SetFilePointer(file, 16 + 48 + sizeof(key), nullptr, FILE_BEGIN);
  const BYTE changed = 0;
  DWORD written = 0;
  if (!WriteFile(file, &changed, 1, &written, nullptr) || written != 1) fail("corrupt write");
  CloseHandle(file);
  void *out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&d, __uuidof(ID3D12ShaderCacheSession), &out), E_FAIL, "corrupt checksum");
  if (out) fail("corrupt output");
  d.Version++;
  cache = session(dev, d); missing(cache.Get());
  check(cache->StoreValue(key, sizeof(key), value, sizeof(value)), "length corrupt seed");
  cache.Reset();
  file = CreateFileW(info.cFileName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail("length corrupt open");
  SetFilePointer(file, 16, nullptr, FILE_BEGIN);
  const UINT impossible_length = 0xffffffffu;
  if (!WriteFile(file, &impossible_length, sizeof(impossible_length), &written, nullptr) ||
      written != sizeof(impossible_length)) fail("length corrupt write");
  CloseHandle(file);
  out = reinterpret_cast<void *>(1);
  expect(dev->CreateShaderCacheSession(&d, __uuidof(ID3D12ShaderCacheSession), &out), E_FAIL, "corrupt length");
  if (out) fail("corrupt length output");
  d.Version++; cache = session(dev, d); missing(cache.Get());
  cache->SetDeleteOnDestroy(); cache.Reset();
  passed("corrupt-disk-rejected-version-recovery");
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    auto dev = device();
    using BackendId = const char *(WINAPI *)();
    auto identity = reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
    if (!identity || strcmp(identity(), "owned-graphics-d3d12")) fail("backend identity");
    std::printf("OWNED_D3D12_SHADER_CACHE_BACKEND id=%s\n", identity());
    if (argc > 1) {
      auto d = desc(10, true);
      if (!wcscmp(argv[1], L"--contend")) {
        void *out = reinterpret_cast<void *>(1);
        expect(dev->CreateShaderCacheSession(&d, __uuidof(ID3D12ShaderCacheSession), &out),
               DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "child contention");
        if (out) fail("contention output");
      } else if (!wcscmp(argv[1], L"--restart")) {
        auto cache = session(dev.Get(), d); retrieve(cache.Get());
        const UINT marker = 0x12345678;
        check(cache->StoreValue("child", 5, &marker, sizeof(marker)), "child write");
      } else if (!wcscmp(argv[1], L"--deleted")) {
        auto cache = session(dev.Get(), d); missing(cache.Get()); cache->SetDeleteOnDestroy();
      } else if (!wcscmp(argv[1], L"--isolated")) {
        auto cache = session(dev.Get(), d); missing(cache.Get()); cache->SetDeleteOnDestroy();
      } else if (!wcscmp(argv[1], L"--control-inactive")) {
        auto working = session(dev.Get(), desc(22, true)); retrieve(working.Get());
        auto temporary = session(dev.Get(), desc(23, true, true)); missing(temporary.Get());
      } else fail("unknown child mode");
      std::printf("OWNED_D3D12_SHADER_CACHE_CHILD_PASS\n");
      return 0;
    }
    memory(dev.Get()); disk(dev.Get()); control(dev.Get()); corruption(dev.Get());
    std::printf("OWNED_D3D12_SHADER_CACHE_PASS\n");
    return 0;
  } catch (const std::exception &) { return 1; }
}
