// SPDX-License-Identifier: LGPL-2.1-or-later
// Public CPU/GPU contract probe; link -ldxgi -ld3d12 -lole32 -luser32.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <array>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool condition, const char *step) {
  if (!condition) {
    std::fprintf(stderr, "DXGI_FACTORY_EVENTS_FAIL step=%s win32=%lu\n", step, GetLastError());
    std::exit(1);
  }
}
ULONG handles(HANDLE event) {
  struct Basic { ULONG attributes; ACCESS_MASK access; ULONG handles; ULONG pointers; ULONG reserved[10]; } info{};
  using Query = LONG(WINAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
  auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  require(query && query(event, 0, &info, sizeof(info), nullptr) >= 0, "query retained handles");
  return info.handles;
}
bool sameLuid(LUID a, LUID b) { return a.HighPart == b.HighPart && a.LowPart == b.LowPart; }
constexpr UINT notification = WM_APP + 37;
unsigned messages = 0;
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == notification) ++messages;
  return DefWindowProcW(window, message, wparam, lparam);
}
void pump(DWORD milliseconds) {
  const ULONGLONG end = GetTickCount64() + milliseconds;
  do {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= end) break;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(end - now), QS_ALLINPUT);
  } while (true);
}
}

int main(int argc, char **argv) {
  ComPtr<IDXGIFactory7> factory;
  require(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)) == S_OK, "create Factory7");
  require(factory->IsCurrent(), "new factory enumeration current");
  std::array<std::vector<LUID>, 3> identities;
  for (unsigned preference = 0; preference < 3; ++preference) {
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter4> adapter;
      const HRESULT hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE(preference),
                                                            IID_PPV_ARGS(&adapter));
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      require(hr == S_OK, "enumerate preference");
      DXGI_ADAPTER_DESC3 desc{};
      require(adapter->GetDesc3(&desc) == S_OK, "read adapter identity");
      require(!(desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE), "hardware never disguised as software");
      for (LUID old : identities[preference]) require(!sameLuid(old, desc.AdapterLuid), "unique adapters");
      identities[preference].push_back(desc.AdapterLuid);
      ComPtr<IDXGIAdapter4> byLuid;
      require(factory->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&byLuid)) == S_OK, "preference LUID roundtrip");
      DXGI_ADAPTER_DESC3 roundtrip{};
      require(byLuid->GetDesc3(&roundtrip) == S_OK && sameLuid(desc.AdapterLuid, roundtrip.AdapterLuid), "stable LUID");
      DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
      require(adapter->QueryVideoMemoryInfo(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory) == E_INVALIDARG,
              "no fake linked memory node");
      require(adapter->SetVideoMemoryReservation(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 0) == E_INVALIDARG,
              "no fake linked reservation node");
      if (preference == 0) {
        ComPtr<ID3D12Device> device;
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)) == S_OK,
                "create real adapter device");
        require(device->GetNodeCount() == 1, "one node per Metal adapter");
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        require(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)) == S_OK &&
                options.CrossNodeSharingTier == D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED,
                "optional cross-node support absent");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.NodeMask = 2;
        ComPtr<ID3D12CommandQueue> queue;
        require(FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && !queue,
                "requested nonexistent node rejected");
      }
      std::printf("DXGI_FACTORY_ADAPTER preference=%u index=%u luid=%08lx:%08lx\n", preference, index,
                  static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
    }
  }
  require(!identities[0].empty(), "real hardware adapters present");
  for (unsigned p = 1; p < 3; ++p) {
    require(identities[p].size() == identities[0].size(), "preference preserves adapter count");
    for (LUID expected : identities[0]) {
      bool found = false;
      for (LUID candidate : identities[p]) found |= sameLuid(candidate, expected);
      require(found, "preference is ordering not filtering");
    }
  }
  ComPtr<IDXGIAdapter1> first;
  require(factory->EnumAdapters1(0, &first) == S_OK, "legacy adapter order");
  DXGI_ADAPTER_DESC1 firstDesc{};
  require(first->GetDesc1(&firstDesc) == S_OK && sameLuid(firstDesc.AdapterLuid, identities[0][0]),
          "unspecified matches legacy order");
  first.Reset();
  void *invalid = reinterpret_cast<void *>(1);
  require(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE(99), __uuidof(IDXGIAdapter1), &invalid) ==
              DXGI_ERROR_INVALID_CALL && !invalid, "invalid preference clears output");
  invalid = reinterpret_cast<void *>(1);
  require(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), &invalid) == DXGI_ERROR_NOT_FOUND && !invalid,
          "no fake WARP renderer");

  WNDCLASSW cls{};
  cls.lpfnWndProc = windowProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"OwnedDxgiFactoryEvents";
  require(RegisterClassW(&cls) != 0, "register recipient class");
  HWND window = CreateWindowW(cls.lpszClassName, L"DXGI factory events", WS_OVERLAPPEDWINDOW,
                              50, 50, 300, 160, nullptr, nullptr, cls.hInstance, nullptr);
  require(window != nullptr, "create hidden recipient");
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE stereo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE adapters = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(event && stereo && adapters, "create event recipients");
  const ULONG baseline = handles(event);
  const ULONG stereoBaseline = handles(stereo);
  DWORD occlusionCookie = 0, windowCookie = 0, stereoCookie = 0, adapterCookie = 0;
  require(factory->RegisterOcclusionStatusEvent(event, &occlusionCookie) == S_OK && occlusionCookie,
          "register occlusion event");
  require(factory->RegisterOcclusionStatusWindow(window, notification, &windowCookie) == S_OK && windowCookie,
          "register occlusion window");
  require(factory->RegisterStereoStatusEvent(stereo, &stereoCookie) == S_OK && stereoCookie,
          "disabled stereo valid registration");
  require(factory->RegisterAdaptersChangedEvent(adapters, &adapterCookie) == S_OK && adapterCookie,
          "register real adapter observer");
  require(occlusionCookie != stereoCookie && stereoCookie != adapterCookie && windowCookie != occlusionCookie,
          "independent nonzero cookies");
  require(handles(event) == baseline + 1, "factory retains owned duplicate");
  DWORD duplicateCookie = 42;
  require(factory->RegisterOcclusionStatusEvent(event, &duplicateCookie) == DXGI_ERROR_INVALID_CALL &&
              !duplicateCookie, "duplicate registration rejected");
  HANDLE alias = nullptr;
  require(DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &alias, 0, FALSE,
                          DUPLICATE_SAME_ACCESS), "duplicate caller handle");
  require(factory->RegisterOcclusionStatusEvent(alias, &duplicateCookie) == DXGI_ERROR_INVALID_CALL &&
              !duplicateCookie, "alias of registered event rejected");
  CloseHandle(alias);
  HANDLE readOnly = nullptr;
  require(DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &readOnly, SYNCHRONIZE, FALSE, 0),
          "duplicate without event modify access");
  require(factory->RegisterOcclusionStatusEvent(readOnly, &duplicateCookie) == DXGI_ERROR_INVALID_CALL &&
              !duplicateCookie, "read-only event rejected");
  CloseHandle(readOnly);
  require(factory->RegisterOcclusionStatusEvent(event, nullptr) == DXGI_ERROR_INVALID_CALL, "null cookie rejected");
  require(factory->RegisterStereoStatusWindow(nullptr, notification, &duplicateCookie) == DXGI_ERROR_INVALID_CALL &&
              !duplicateCookie, "invalid notification window rejected");
  DWORD stereoWindowCookie = 0;
  require(factory->RegisterStereoStatusWindow(window, WM_APP + 38, &stereoWindowCookie) == S_OK && stereoWindowCookie,
          "disabled stereo window registration");
  HANDLE semaphore = CreateSemaphoreW(nullptr, 0, 1, nullptr);
  require(semaphore != nullptr, "create wrong object type");
  require(factory->RegisterOcclusionStatusEvent(semaphore, &duplicateCookie) == DXGI_ERROR_INVALID_CALL &&
              !duplicateCookie, "reject non-event object");
  CloseHandle(semaphore);
  ShowWindow(window, SW_SHOW);
  require(WaitForSingleObject(event, 3000) == WAIT_OBJECT_0, "unocclusion delivers event");
  pump(300);
  require(messages != 0, "unocclusion delivers window message");
  require(WaitForSingleObject(stereo, 0) == WAIT_TIMEOUT && !factory->IsWindowedStereoEnabled(),
          "disabled stereo has no false transition");
  require(WaitForSingleObject(adapters, 0) == WAIT_TIMEOUT, "no fabricated adapter changes");
  factory->UnregisterOcclusionStatus(stereoCookie);
  require(handles(stereo) == stereoBaseline + 1, "wrong registry unregister preserves stereo");
  factory->UnregisterStereoStatus(stereoCookie);
  factory->UnregisterStereoStatus(stereoWindowCookie);
  require(handles(stereo) == stereoBaseline, "stereo unregister releases duplicate");
  factory->UnregisterOcclusionStatus(occlusionCookie);
  factory->UnregisterOcclusionStatus(occlusionCookie);
  factory->UnregisterOcclusionStatus(windowCookie);
  require(handles(event) == baseline, "occlusion unregister releases duplicate");
  ResetEvent(event);
  ShowWindow(window, SW_HIDE);
  pump(300);
  require(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "no delivery after unregister");
  require(factory->UnregisterAdaptersChangedEvent(adapterCookie) == S_OK &&
              factory->UnregisterAdaptersChangedEvent(adapterCookie) == S_OK, "adapter unregister idempotent");

  HANDLE closedOriginal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE retainedWitness = nullptr;
  require(closedOriginal && DuplicateHandle(GetCurrentProcess(), closedOriginal, GetCurrentProcess(),
                                            &retainedWitness, 0, FALSE, DUPLICATE_SAME_ACCESS),
          "create closed-original witness");
  DWORD retainedCookie = 0;
  require(factory->RegisterOcclusionStatusEvent(closedOriginal, &retainedCookie) == S_OK,
          "register before closing original");
  CloseHandle(closedOriginal);
  HANDLE replacement = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD replacementCookie = 0;
  require(replacement && factory->RegisterOcclusionStatusEvent(replacement, &replacementCookie) == S_OK,
          "new event registers after caller closes original (including handle reuse)");
  ShowWindow(window, SW_SHOW);
  require(WaitForSingleObject(retainedWitness, 3000) == WAIT_OBJECT_0, "delivery survives closing original handle");
  require(WaitForSingleObject(replacement, 3000) == WAIT_OBJECT_0, "independent replacement event delivery");
  factory->UnregisterOcclusionStatus(replacementCookie);
  CloseHandle(replacement);
  factory->UnregisterOcclusionStatus(retainedCookie);
  require(handles(retainedWitness) == 1, "only witness remains after unregister");
  CloseHandle(retainedWitness);
  ShowWindow(window, SW_HIDE);

  // Optional real hotplug acceptance. No injected/pretend hardware event.
  if (argc > 1 && !std::strcmp(argv[1], "--hotplug")) {
    require(factory->RegisterAdaptersChangedEvent(adapters, &adapterCookie) == S_OK, "hotplug subscribe");
    std::puts("DXGI_FACTORY_HOTPLUG_WAIT connect or disconnect a real Metal GPU within 60 seconds");
    std::fflush(stdout);
    require(WaitForSingleObject(adapters, 60000) == WAIT_OBJECT_0, "native hotplug delivers event");
    require(!factory->IsCurrent(), "native hotplug invalidates old factory");
    ComPtr<IDXGIFactory7> fresh;
    require(CreateDXGIFactory2(0, IID_PPV_ARGS(&fresh)) == S_OK && fresh->IsCurrent(), "new hotplug snapshot current");
    factory->UnregisterAdaptersChangedEvent(adapterCookie);
  }
  require(factory->RegisterOcclusionStatusEvent(event, &occlusionCookie) == S_OK, "destruction subscriber");
  require(factory->RegisterStereoStatusEvent(stereo, &stereoCookie) == S_OK, "destruction stereo subscriber");
  CloseHandle(stereo); // The duplicate must keep the registration valid.
  factory.Reset();
  require(handles(event) == baseline, "factory destruction joins dispatch and closes duplicate");
  CloseHandle(event);
  CloseHandle(adapters);
  DestroyWindow(window);
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
  std::puts("DXGI_FACTORY_EVENTS_OK identity preference nodes occlusion stereo lifetime");
}
