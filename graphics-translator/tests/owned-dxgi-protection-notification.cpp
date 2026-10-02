// SPDX-License-Identifier: LGPL-2.1-or-later
// Public DXGI CPU probe: clang++ -std=c++20 -static -Iinclude/native/directx
//   owned-dxgi-protection-notification.cpp -ldxgi -lole32 -o protection.exe
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace {
void require(bool condition, const char *step) {
  if (!condition) {
    std::fprintf(stderr, "DXGI_PROTECTION_NOTIFICATION_FAIL step=%s win32=%lu\n", step,
                 static_cast<unsigned long>(GetLastError()));
    std::exit(1);
  }
}

ULONG handleCount(HANDLE event) {
  // Public OBJECT_BASIC_INFORMATION layout, as in the budget notification probe.
  struct BasicInfo {
    ULONG attributes;
    ACCESS_MASK access;
    ULONG handles;
    ULONG pointers;
    ULONG reserved[10];
  } info{};
  using QueryObject = LONG(WINAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
  const auto query = reinterpret_cast<QueryObject>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
  require(query != nullptr, "resolve NtQueryObject");
  require(query(event, 0, &info, sizeof(info), nullptr) >= 0, "query event ownership");
  return info.handles;
}

void rejected(IDXGIAdapter3 *adapter, HANDLE event, HRESULT wanted, const char *step) {
  DWORD cookie = 0xabcdef01;
  std::printf("DXGI_PROTECTION_NOTIFICATION_CALL step=%s\n", step);
  std::fflush(stdout);
  const HRESULT hr = adapter->RegisterHardwareContentProtectionTeardownStatusEvent(event, &cookie);
  std::printf("DXGI_PROTECTION_NOTIFICATION_RESULT step=%s hr=0x%08lx cookie=%lu\n", step,
              static_cast<unsigned long>(hr), static_cast<unsigned long>(cookie));
  require(hr == wanted, step);
  require(cookie == 0, "failed registration clears cookie");
}
} // namespace

int main() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  ComPtr<IDXGIFactory1> factory;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)) == S_OK, "create public DXGI factory");
  ComPtr<IDXGIAdapter1> base;
  require(factory->EnumAdapters1(0, &base) == S_OK, "enumerate public adapter");
  ComPtr<IDXGIAdapter3> adapter;
  require(base.As(&adapter) == S_OK, "query public adapter3");
  base.Reset();

  for (BOOL manual : {TRUE, FALSE}) {
    for (BOOL signaled : {FALSE, TRUE}) {
      HANDLE event = CreateEventW(nullptr, manual, signaled, nullptr);
      require(event != nullptr, "create caller event");
      const ULONG handles = handleCount(event);
      rejected(adapter.Get(), event, DXGI_ERROR_UNSUPPORTED, "valid event unsupported");
      require(handleCount(event) == handles, "unsupported registration retains no event handle");
      require(WaitForSingleObject(event, 0) == (signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT),
              "registration preserves caller event state");
      require((signaled ? SetEvent(event) : ResetEvent(event)) != FALSE, "restore event state");
      require(adapter->RegisterHardwareContentProtectionTeardownStatusEvent(event, nullptr) == E_INVALIDARG,
              "null cookie rejected");
      adapter->UnregisterHardwareContentProtectionTeardownStatus(0);
      adapter->UnregisterHardwareContentProtectionTeardownStatus(0xabcdef01);
      adapter->UnregisterHardwareContentProtectionTeardownStatus(0xabcdef01);
      require(handleCount(event) == handles, "unregister preserves caller ownership");
      require(WaitForSingleObject(event, 0) == (signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT),
              "unregister preserves caller event state");
      require(SetEvent(event) != FALSE, "caller event remains writable");
      require(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "caller event remains waitable");
      require(CloseHandle(event) != FALSE, "caller can close own event");
    }
  }

  rejected(adapter.Get(), nullptr, E_INVALIDARG, "null event rejected");
  rejected(adapter.Get(), INVALID_HANDLE_VALUE, E_INVALIDARG, "invalid event sentinel rejected");

  HANDLE budgetEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(budgetEvent != nullptr, "create budget caller event");
  const ULONG initialHandles = handleCount(budgetEvent);
  DWORD budgetCookie = 0;
  require(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(budgetEvent, &budgetCookie) == S_OK,
          "existing budget registration succeeds");
  require(budgetCookie != 0, "budget registration supplies cookie");
  require(handleCount(budgetEvent) == initialHandles + 1, "budget registration owns duplicate");
  adapter->UnregisterHardwareContentProtectionTeardownStatus(budgetCookie);
  require(handleCount(budgetEvent) == initialHandles + 1, "protection unregister does not consume budget cookie");
  rejected(adapter.Get(), budgetEvent, DXGI_ERROR_UNSUPPORTED, "budget event protection registration unsupported");
  require(handleCount(budgetEvent) == initialHandles + 1, "protection failure preserves budget registration");
  adapter->UnregisterVideoMemoryBudgetChangeNotification(budgetCookie);
  require(handleCount(budgetEvent) == initialHandles, "budget unregister releases only owned duplicate");
  require(WaitForSingleObject(budgetEvent, 0) == WAIT_TIMEOUT, "budget event stays unsignaled");
  require(SetEvent(budgetEvent) != FALSE, "set event before adapter destruction");
  adapter.Reset();
  factory.Reset();
  require(handleCount(budgetEvent) == initialHandles, "adapter destruction preserves caller handle");
  require(WaitForSingleObject(budgetEvent, 0) == WAIT_OBJECT_0, "adapter destruction preserves caller state");
  require(CloseHandle(budgetEvent) != FALSE, "caller closes event after adapter destruction");
  std::puts("DXGI_PROTECTION_NOTIFICATION_OK unsupported cookie ownership state budget");
  return 0;
}
