// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12_sdk.hpp"
#include "d3d12_diagnostics.hpp"
#include "d3d12_core_generation.h"
#include "com/com_object.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

extern "C" const char OwnedD3D12CoreGeneration[33] = OWNED_D3D12_CORE_GENERATION;
namespace dxmt {
namespace {
// D3D12_ERROR_INVALID_REDIST is absent from the supported MinGW winerror.h.
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
constexpr HRESULT kD3D12ErrorInvalidRedist = static_cast<HRESULT>(0x887e0003u);
struct File {
  HANDLE handle = INVALID_HANDLE_VALUE;
  explicit File(const std::wstring &path) : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
  ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
};
HRESULT ModulePath(HMODULE module, std::wstring &out) {
  std::array<WCHAR, 32768> path;
  DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!length) return HRESULT_FROM_WIN32(GetLastError());
  if (length >= path.size()) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  out.assign(path.data(), length); return S_OK;
}
HRESULT FilePath(HANDLE file, std::wstring &out) {
  std::array<WCHAR, 32768> path;
  DWORD length = GetFinalPathNameByHandleW(file, path.data(), DWORD(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length) return HRESULT_FROM_WIN32(GetLastError());
  if (length >= path.size()) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  out.assign(path.data(), length); return S_OK;
}
HRESULT InstalledPath(std::wstring &out) {
  HMODULE main = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&OwnedD3D12MainGetBridge1), &main)) return HRESULT_FROM_WIN32(GetLastError());
  // Core imports this exact, already-loaded Main. Never let its import cause a
  // different d3d12.dll from the requested directory to execute.
  if (GetModuleHandleW(L"d3d12.dll") != main) return E_NOINTERFACE;
  static std::mutex path_mutex;
  static std::wstring installed;
  // Keep the verified backing file immutable for this Main image lifetime.
  static std::unique_ptr<File> backing_main;
  std::lock_guard lock(path_mutex);
  if (!installed.empty()) { out = installed; return S_OK; }
  HRESULT hr = ModulePath(main, out); if (FAILED(hr)) return hr;
  using DosFileName = WCHAR *(__cdecl *)(const char *);
  using SameMapping = LONG (WINAPI *)(void *, void *);
  auto dos_name = reinterpret_cast<DosFileName>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
  if (dos_name) {
    auto same = reinterpret_cast<SameMapping>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAreMappedFilesTheSame"));
    if (!same) return E_NOINTERFACE;
    const auto validate = [&](const std::wstring &path) -> HRESULT {
      auto file = std::make_unique<File>(path);
      if (file->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
      // A data mapping cannot redirect by basename or execute imports/DllMain.
      HANDLE mapping = CreateFileMappingW(file->handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
      if (!mapping) return HRESULT_FROM_WIN32(GetLastError());
      void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
      if (!view) { HRESULT error = HRESULT_FROM_WIN32(GetLastError()); CloseHandle(mapping); return error; }
      const LONG status = same(main, view);
      UnmapViewOfFile(view); CloseHandle(mapping);
      if (status) return DXGI_ERROR_UNSUPPORTED;
      HRESULT error = FilePath(file->handle, out); if (FAILED(error)) return error;
      backing_main = std::move(file); return S_OK;
    };
    hr = validate(out);
    if (FAILED(hr)) {
      // Wine reports the prefix facade as Main's module/mapped filename. Its
      // pinned DLL paths are hints only: require the exact loaded backing file,
      // not equal bytes, before trusting a directory as the installed location.
      std::array<WCHAR, 32768> environment;
      DWORD length = GetEnvironmentVariableW(L"WINEDLLPATH", environment.data(), DWORD(environment.size()));
      if (!length) return hr;
      if (length >= environment.size()) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
      int count = WideCharToMultiByte(CP_UTF8, 0, environment.data(), -1, nullptr, 0, nullptr, nullptr);
      if (!count) return HRESULT_FROM_WIN32(GetLastError());
      std::string paths(size_t(count), '\0');
      if (!WideCharToMultiByte(CP_UTF8, 0, environment.data(), -1, paths.data(), count, nullptr, nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
      const auto release = [](WCHAR *path) { HeapFree(GetProcessHeap(), 0, path); };
      for (char *entry = paths.data(); entry;) {
        char *next = std::strchr(entry, ':');
        if (next) *next++ = 0;
        if (entry[0] == '/') {
          std::string path = entry;
#if defined(__i386__) || defined(_M_IX86)
          // Wine's 64-bit host paths also locate the WoW64 PE sibling. The
          // wrapper need not list it, but its Main must match our loaded image.
          const auto slash = path.find_last_of('/');
          if (slash != std::string::npos &&
              (path.compare(slash + 1, std::string::npos, "x86_64-windows") == 0 ||
               path.compare(slash + 1, std::string::npos, "x86_64-unix") == 0)) {
            path.resize(slash + 1); path += "i386-windows";
          }
#endif
          path += "/d3d12.dll";
          std::unique_ptr<WCHAR, decltype(release)> dos(dos_name(path.c_str()), release);
          if (dos) {
            hr = validate(dos.get());
            if (SUCCEEDED(hr)) break;
          }
        }
        entry = next;
      }
      if (FAILED(hr)) return hr;
    }
  }
  auto slash = out.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return E_FAIL;
  out.resize(slash + 1); out += L"d3d12core.dll"; installed = out; return S_OK;
}
HRESULT RequestedPath(const char *directory, std::wstring &out) {
  if (!directory || !*directory) return E_INVALIDARG;
  int count = MultiByteToWideChar(CP_ACP, 0, directory, -1, nullptr, 0);
  if (!count) return HRESULT_FROM_WIN32(GetLastError());
  std::wstring path(size_t(count), L'\0');
  if (!MultiByteToWideChar(CP_ACP, 0, directory, -1, path.data(), count)) return HRESULT_FROM_WIN32(GetLastError());
  path.resize(size_t(count - 1));
  // Windows SDK paths are relative to the executable, never the current directory.
  // Root-relative and drive-relative paths have ambient state: reject both.
  const bool drive = path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
  const bool unc = path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
  if (!drive && !unc) {
    if (path[0] == L'\\' || path[0] == L'/' || path.find(L':') != std::wstring::npos) return E_INVALIDARG;
    HRESULT hr = ModulePath(nullptr, out); if (FAILED(hr)) return hr;
    auto slash = out.find_last_of(L"\\/"); if (slash == std::wstring::npos) return E_FAIL;
    out.resize(slash + 1); out += path;
  } else out = std::move(path);
  if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
  out += L"d3d12core.dll"; return S_OK;
}
HRESULT EqualImages(HANDLE installed, HANDLE requested) {
  LARGE_INTEGER a, b;
  if (!GetFileSizeEx(installed, &a) || !GetFileSizeEx(requested, &b)) return HRESULT_FROM_WIN32(GetLastError());
  if (!a.QuadPart || a.QuadPart != b.QuadPart) return DXGI_ERROR_UNSUPPORTED;
  std::array<unsigned char, 16384> left, right;
  for (LONGLONG offset = 0; offset < a.QuadPart;) {
    DWORD n = DWORD(std::min<LONGLONG>(left.size(), a.QuadPart - offset)), na = 0, nb = 0;
    if (!ReadFile(installed, left.data(), n, &na, nullptr) || !ReadFile(requested, right.data(), n, &nb, nullptr))
      return HRESULT_FROM_WIN32(GetLastError());
    if (na != n || nb != n || std::memcmp(left.data(), right.data(), n)) return DXGI_ERROR_UNSUPPORTED;
    offset += n;
  }
  return S_OK;
}
// Read-only file mapping, NOT SEC_IMAGE: no imports, TLS or DllMain execute.
// The installed image must itself belong to this Main generation and ABI.
HRESULT PreflightInstalled(HANDLE file, UINT version) {
  LARGE_INTEGER length;
  if (!GetFileSizeEx(file, &length)) return HRESULT_FROM_WIN32(GetLastError());
  if (length.QuadPart <= 0 || length.QuadPart > MAXDWORD) return DXGI_ERROR_UNSUPPORTED;
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) return HRESULT_FROM_WIN32(GetLastError());
  const auto *bytes = static_cast<const unsigned char *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
  if (!bytes) { HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); CloseHandle(mapping); return hr; }
  const auto check = [&]() -> HRESULT {
    const uint64_t size = uint64_t(length.QuadPart);
    const auto read = [&](uint64_t offset, void *out, size_t count) {
      if (offset > size || count > size - offset) return false;
      std::memcpy(out, bytes + offset, count); return true;
    };
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS nt;
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !read(uint64_t(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(nt.OptionalHeader) ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) return DXGI_ERROR_UNSUPPORTED;
    // Wine reads these DOS-stub signatures before deciding native versus builtin.
    // Reject redirectable/placeholder Core images before any executable mapping.
    char loader_marker[32];
    if (!read(sizeof(dos), loader_marker, sizeof(loader_marker)) ||
        !std::memcmp(loader_marker, "Wine builtin DLL", sizeof("Wine builtin DLL")) ||
        !std::memcmp(loader_marker, "Wine placeholder DLL", sizeof("Wine placeholder DLL")))
      return DXGI_ERROR_UNSUPPORTED;
#if defined(__arm64ec__) || defined(_M_ARM64EC)
    // The paired Core file is ARM64; Wine applies its ARM64EC view only to an image mapping.
    constexpr WORD machine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(__x86_64__) || defined(_M_X64)
    constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(__i386__) || defined(_M_IX86)
    constexpr WORD machine = IMAGE_FILE_MACHINE_I386;
#elif defined(__aarch64__) || defined(_M_ARM64)
    constexpr WORD machine = IMAGE_FILE_MACHINE_ARM64;
#else
#error Unsupported owned Core PE ABI
#endif
    if (nt.FileHeader.Machine != machine) return DXGI_ERROR_UNSUPPORTED;
    std::array<IMAGE_SECTION_HEADER, 96> sections;
    const uint64_t section_offset = uint64_t(dos.e_lfanew) + sizeof(nt);
    if (!read(section_offset, sections.data(), nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER)))
      return DXGI_ERROR_UNSUPPORTED;
    const auto address = [&](uint64_t rva, size_t count) -> const unsigned char * {
      for (UINT i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const auto &s = sections[i];
        if (rva < s.VirtualAddress || rva - s.VirtualAddress > s.SizeOfRawData) continue;
        const uint64_t delta = rva - s.VirtualAddress;
        const uint64_t offset = uint64_t(s.PointerToRawData) + delta;
        if (count > s.SizeOfRawData - delta || offset > size || count > size - offset) continue;
        return bytes + offset;
      }
      return nullptr;
    };
    const auto &directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto *raw = address(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (!raw) return DXGI_ERROR_UNSUPPORTED;
    IMAGE_EXPORT_DIRECTORY exports; std::memcpy(&exports, raw, sizeof(exports));
    if (!exports.NumberOfNames || exports.NumberOfNames > 65536 || !exports.NumberOfFunctions ||
        exports.NumberOfFunctions > 65536) return DXGI_ERROR_UNSUPPORTED;
    const auto *names = address(exports.AddressOfNames, size_t(exports.NumberOfNames) * sizeof(DWORD));
    const auto *ordinals = address(exports.AddressOfNameOrdinals, size_t(exports.NumberOfNames) * sizeof(WORD));
    const auto *functions = address(exports.AddressOfFunctions, size_t(exports.NumberOfFunctions) * sizeof(DWORD));
    if (!names || !ordinals || !functions) return DXGI_ERROR_UNSUPPORTED;
    const auto lookup = [&](const char *name, size_t count) -> const unsigned char * {
      const size_t name_size = std::strlen(name) + 1;
      for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD name_rva; std::memcpy(&name_rva, names + size_t(i) * sizeof(DWORD), sizeof(name_rva));
        const auto *candidate = address(name_rva, name_size);
        if (!candidate || std::memcmp(candidate, name, name_size)) continue;
        WORD ordinal; std::memcpy(&ordinal, ordinals + size_t(i) * sizeof(WORD), sizeof(ordinal));
        if (ordinal >= exports.NumberOfFunctions) return nullptr;
        DWORD rva; std::memcpy(&rva, functions + size_t(ordinal) * sizeof(DWORD), sizeof(rva));
        if (rva >= directory.VirtualAddress && uint64_t(rva) < uint64_t(directory.VirtualAddress) + directory.Size)
          return nullptr; // Export forwarders are never owned Core code/data.
        return address(rva, count);
      }
      return nullptr;
    };
    const auto *sdk = lookup("D3D12SDKVersion", sizeof(UINT));
    const auto *generation = lookup("OwnedD3D12CoreGeneration", 33);
    if (!sdk || !generation || !lookup("D3D12GetInterface", 1) || !lookup("OwnedD3D12CoreGetBridge1", 1))
      return DXGI_ERROR_UNSUPPORTED;
    UINT identity; std::memcpy(&identity, sdk, sizeof(identity));
    return identity == 618 && (!version || version == identity) &&
        !std::memcmp(generation, OwnedD3D12CoreGeneration, 33) ? S_OK : DXGI_ERROR_UNSUPPORTED;
  };
  HRESULT hr = check(); UnmapViewOfFile(bytes); CloseHandle(mapping); return hr;
}
}
struct OwnedCoreModule {
  std::shared_ptr<File> installed_file, requested_file;
  HMODULE module = nullptr;
  const OwnedD3D12CoreBridge1 *bridge = nullptr;
  ~OwnedCoreModule() { if (module) FreeLibrary(module); }
};
namespace {
// Only cache eviction unloads Core; leases protect every Core call and destructor.
std::mutex sdk_mutex;
std::map<std::wstring, std::shared_ptr<OwnedCoreModule>> cores;
std::shared_ptr<OwnedCoreModule> global_core;
bool global_device_load_started = false;
UINT global_device_creations = 0;

HRESULT LoadCoreLocked(UINT version, const std::wstring &requested, std::shared_ptr<OwnedCoreModule> &out) {
  out.reset();
  std::wstring installed;
  HRESULT hr = InstalledPath(installed); if (FAILED(hr)) return hr;
  auto baseline = std::make_shared<File>(installed);
  if (baseline->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
  if (FAILED(hr = PreflightInstalled(baseline->handle, version))) return hr;
  auto candidate = std::make_shared<File>(requested);
  if (candidate->handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
  std::wstring path;
  if (FAILED(hr = FilePath(candidate->handle, path))) return hr;
  // Deny writes/deletion from BEFORE preflight until AFTER unload, for both images.
  // Compare complete files before executing any code from the requested module.
  if (FAILED(hr = EqualImages(baseline->handle, candidate->handle))) return hr;
  if (auto found = cores.find(path); found != cores.end()) {
    if (version && version != found->second->bridge->sdk_version) return DXGI_ERROR_UNSUPPORTED;
    out = found->second; return S_OK;
  }
  auto core = std::make_shared<OwnedCoreModule>();
  core->installed_file = std::move(baseline); core->requested_file = std::move(candidate);
  core->module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!core->module) return HRESULT_FROM_WIN32(GetLastError());
  std::wstring live;
  if (FAILED(hr = ModulePath(core->module, live))) return hr;
  File live_file(live);
  if (live_file.handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
  if (FAILED(hr = FilePath(live_file.handle, live))) return hr;
  if (_wcsicmp(live.c_str(), path.c_str())) return DXGI_ERROR_UNSUPPORTED;
  auto sdk = reinterpret_cast<const UINT *>(GetProcAddress(core->module, "D3D12SDKVersion"));
  auto generation = reinterpret_cast<const char *>(GetProcAddress(core->module, "OwnedD3D12CoreGeneration"));
  auto get = reinterpret_cast<OwnedCoreGetBridge1>(GetProcAddress(core->module, "OwnedD3D12CoreGetBridge1"));
  if (!sdk || !generation || !get || std::memcmp(generation, OwnedD3D12CoreGeneration, 33) ||
      (version && version != *sdk)) return DXGI_ERROR_UNSUPPORTED;
  core->bridge = get(1, sizeof(OwnedD3D12CoreBridge1));
  const auto *b = core->bridge;
  if (!b || b->size != sizeof(*b) || b->version != 1 || b->configuration_size != sizeof(OwnedCoreConfiguration1) ||
      b->pointer_size != sizeof(void *) || b->sdk_version != *sdk || !b->generation ||
      std::strcmp(b->generation, OwnedD3D12CoreGeneration) || !b->create_factory || !b->destroy_factory ||
      !b->initialize || !b->apply || !b->get_configuration || !b->set_flags || !b->get_flags || !b->enable_debug || !b->set_dred ||
      !b->enable_experimental || !b->create_device) return E_NOINTERFACE;
  cores.emplace(path, core); out = std::move(core); return S_OK;
}
HRESULT SelectGlobalCoreLocked(UINT version, const char *directory, std::shared_ptr<OwnedCoreModule> &out) {
  std::wstring path;
  HRESULT hr = InstalledPath(path); if (FAILED(hr)) return hr;
  std::shared_ptr<OwnedCoreModule> installed;
  if (FAILED(hr = LoadCoreLocked(0, path, installed))) return hr;
  // Global redist selection upgrades inbox only. SDK1 factories remain strict
  // side-by-side selections; derive inbox precedence from the validated image.
  if (version <= installed->bridge->sdk_version) { out = std::move(installed); return S_OK; }
  if (FAILED(hr = RequestedPath(directory, path))) return hr;
  std::shared_ptr<OwnedCoreModule> selected;
  if (FAILED(hr = LoadCoreLocked(version, path, selected))) return hr;
  out = std::move(selected); return S_OK;
}
HRESULT LoadGlobalCoreLocked() {
  if (global_core) return S_OK;
  HMODULE exe = GetModuleHandleW(nullptr);
  const auto *version = reinterpret_cast<const UINT *>(GetProcAddress(exe, "D3D12SDKVersion"));
  const auto *directory = reinterpret_cast<const char *const *>(GetProcAddress(exe, "D3D12SDKPath"));
  return SelectGlobalCoreLocked(version ? *version : 0, directory ? *directory : nullptr, global_core);
}
RuntimeConfiguration FromCore(const OwnedCoreConfiguration1 &c) {
  RuntimeConfiguration value;
  value.debug = c.debug != 0; value.experimental_shader_models = c.experimental_shader_models != 0;
  value.breadcrumbs = D3D12_DRED_ENABLEMENT(c.breadcrumbs); value.contexts = D3D12_DRED_ENABLEMENT(c.contexts);
  value.factory_flags = c.factory_flags;
  return value;
}
HRESULT WINAPI GetGlobal(void *lease, OwnedCoreConfiguration1 *out) {
  try {
    const auto &factory = *static_cast<CoreFactoryConfiguration *>(lease);
    std::lock_guard lock(sdk_mutex);
    HRESULT hr = LoadGlobalCoreLocked(); if (FAILED(hr)) return hr;
    if (global_core->bridge->sdk_version < factory.bridge->sdk_version) return kD3D12ErrorInvalidRedist;
    const auto c = GetGlobalRuntimeConfiguration();
    *out = {UINT(c.debug), UINT(c.experimental_shader_models), UINT(c.breadcrumbs), UINT(c.contexts), c.factory_flags};
    return S_OK;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT WINAPI ApplyGlobal(void *lease, const OwnedCoreConfiguration1 *c) {
  auto &factory = *static_cast<CoreFactoryConfiguration *>(lease);
  std::lock_guard lock(sdk_mutex);
  if (global_core && (factory.bridge->sdk_version < global_core->bridge->sdk_version ||
      (factory.module != global_core && (global_device_creations || HasGlobalDevices())))) return kD3D12ErrorInvalidRedist;
  HRESULT hr = ApplyGlobalRuntimeConfiguration(FromCore(*c));
  if (SUCCEEDED(hr)) global_core = factory.module;
  return hr;
}
HRESULT WINAPI CreateDevice(void *lease, IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out,
                           const OwnedCoreConfiguration1 *c, BOOL independent) {
  auto &factory = *static_cast<CoreFactoryConfiguration *>(lease);
  auto value = FromCore(*c);
  value.sdk_version = factory.bridge->sdk_version;
  value.sdk_module_lifetime = factory.module;
  return CreateConfiguredDevice(adapter, level, iid, out, value, independent != FALSE);
}
class SDKConfiguration final : public ComObject<ID3D12SDKConfiguration1> {
  std::shared_ptr<OwnedCoreModule> module_;
public:
  explicit SDKConfiguration(std::shared_ptr<OwnedCoreModule> module = {}) : module_(std::move(module)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12SDKConfiguration) &&
        iid != __uuidof(ID3D12SDKConfiguration1)) return E_NOINTERFACE;
    *out = static_cast<ID3D12SDKConfiguration1 *>(this); AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT version, const char *directory) override {
    if (!version || !directory || !*directory) return E_INVALIDARG;
    if (directory[0] == '/' || directory[0] == '\\' || directory[1] == ':') return E_INVALIDARG;
    DWORD developer = 0, size = sizeof(developer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                    L"AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, nullptr, &developer, &size) != ERROR_SUCCESS ||
        !developer) return E_ACCESSDENIED;
    // Preserve the relative-path syntax contract even when inbox wins.
    if (std::strchr(directory, ':')) return E_INVALIDARG;
    try {
      std::unique_lock lock(sdk_mutex);
      if (global_device_load_started) {
        lock.unlock();
        RemoveDevicesForDebugLayer();
        return DXGI_ERROR_INVALID_CALL;
      }
      std::shared_ptr<OwnedCoreModule> selected;
      HRESULT hr = SelectGlobalCoreLocked(version, directory, selected); if (FAILED(hr)) return hr;
      global_core = std::move(selected); return S_OK;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT version, const char *directory, REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    std::shared_ptr<CoreFactoryConfiguration> configuration;
    HRESULT hr = SelectCoreFactory(version, directory, configuration);
    return FAILED(hr) ? hr : CreateCoreFactoryInterface(configuration, iid, out);
  }
  void STDMETHODCALLTYPE FreeUnusedSDKs() override {
    std::lock_guard lock(sdk_mutex);
    // Releases execute in Main, never in an unloadable Core vtable. No Core call
    // can be in flight with only the cache's reference remaining.
    std::erase_if(cores, [](const auto &entry) { return entry.second.use_count() == 1; });
  }
};
HRESULT InterfaceForCore(const std::shared_ptr<OwnedCoreModule> &core, REFCLSID clsid, REFIID iid, void **out) {
  if (clsid == CLSID_D3D12DeviceFactory)
    return CreateCoreFactoryInterface(std::make_shared<CoreFactoryConfiguration>(core), iid, out);
  if (clsid == CLSID_D3D12SDKConfiguration) {
    auto object = std::make_unique<SDKConfiguration>(core);
    HRESULT hr = object->QueryInterface(iid, out); if (SUCCEEDED(hr)) object.release(); return hr;
  }
  return GetDiagnosticsInterface(clsid, iid, out, core);
}
HRESULT WINAPI CoreInterface(HMODULE module, REFCLSID clsid, REFIID iid, void **out) {
  if (!out) return E_POINTER; *out = nullptr;
  try {
    std::wstring path;
    HRESULT hr = ModulePath(module, path); if (FAILED(hr)) return hr;
    std::shared_ptr<OwnedCoreModule> core;
    { std::lock_guard lock(sdk_mutex); if (FAILED(hr = LoadCoreLocked(0, path, core))) return hr; }
    return InterfaceForCore(core, clsid, iid, out);
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  catch (const std::runtime_error &) { return E_FAIL; }
}
const OwnedD3D12MainBridge1 main_bridge = {
  sizeof(OwnedD3D12MainBridge1), 1, sizeof(OwnedCoreConfiguration1), sizeof(void *),
  OwnedD3D12CoreGeneration, GetGlobal, ApplyGlobal, CreateDevice, CoreInterface
};
}
CoreFactoryConfiguration::CoreFactoryConfiguration(std::shared_ptr<OwnedCoreModule> core)
  : module(std::move(core)), bridge(module->bridge) {
  HRESULT hr = bridge->create_factory(&main_bridge, &state);
  if (hr == E_OUTOFMEMORY) throw std::bad_alloc();
  if (FAILED(hr)) throw std::runtime_error("owned Core bridge rejected factory");
}
CoreFactoryConfiguration::~CoreFactoryConfiguration() { if (state) bridge->destroy_factory(state); }
HRESULT SelectCoreFactory(UINT version, const char *directory, std::shared_ptr<CoreFactoryConfiguration> &out) {
  out.reset();
  if (version && (!directory || !*directory)) return E_INVALIDARG;
  try {
    std::wstring path;
    HRESULT hr = version ? RequestedPath(directory, path) : InstalledPath(path);
    if (FAILED(hr)) return hr;
    std::shared_ptr<OwnedCoreModule> core;
    { std::lock_guard lock(sdk_mutex); if (FAILED(hr = LoadCoreLocked(version, path, core))) return hr; }
    out = std::make_shared<CoreFactoryConfiguration>(std::move(core)); return S_OK;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  catch (const std::runtime_error &) { return E_FAIL; }
}
HRESULT GetSDKInterface(REFCLSID clsid, REFIID iid, void **out) {
  if (!out) return E_POINTER; *out = nullptr;
  try {
    if (clsid == CLSID_D3D12SDKConfiguration) {
      auto object = std::make_unique<SDKConfiguration>();
      HRESULT hr = object->QueryInterface(iid, out); if (SUCCEEDED(hr)) object.release(); return hr;
    }
    if (clsid == CLSID_D3D12DeviceFactory) {
      std::shared_ptr<OwnedCoreModule> core;
      {
        std::lock_guard lock(sdk_mutex);
        HRESULT hr = LoadGlobalCoreLocked(); if (FAILED(hr)) return hr;
        core = global_core;
      }
      return CreateCoreFactoryInterface(std::make_shared<CoreFactoryConfiguration>(std::move(core)), iid, out);
    }
    return E_NOINTERFACE;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  catch (const std::runtime_error &) { return E_FAIL; }
}
HRESULT CreateGlobalSDKDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out) {
  if (out) *out = nullptr;
  try {
    std::shared_ptr<OwnedCoreModule> core;
    {
      std::lock_guard lock(sdk_mutex);
      global_device_load_started = true;
      HRESULT hr = LoadGlobalCoreLocked(); if (FAILED(hr)) return hr;
      core = global_core;
      ++global_device_creations;
    }
    struct CreationGuard {
      ~CreationGuard() { std::lock_guard lock(sdk_mutex); --global_device_creations; }
    } creation_guard;
    CoreFactoryConfiguration configuration(std::move(core));
    HRESULT hr = configuration.bridge->initialize(configuration.state, &configuration); if (FAILED(hr)) return hr;
    return configuration.bridge->create_device(configuration.state, &configuration, adapter, level, iid, out, FALSE);
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  catch (const std::runtime_error &) { return E_FAIL; }
}
}
extern "C" const OwnedD3D12MainBridge1 *WINAPI OwnedD3D12MainGetBridge1(UINT version, UINT size) {
  return version == 1 && size == sizeof(OwnedD3D12MainBridge1) ? &dxmt::main_bridge : nullptr;
}
