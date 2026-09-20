#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kTimeout = 15000;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_FENCE_EVENTS_FAIL step=%s hr=0x%08lx\n",
               step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) fail(step, hr);
}

struct Event {
  HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Event() { if (!handle) fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError())); }
  ~Event() { CloseHandle(handle); }
  Event(const Event &) = delete;
  Event &operator=(const Event &) = delete;
  void expect(DWORD expected, DWORD timeout, const char *step) const {
    const DWORD actual = WaitForSingleObject(handle, timeout);
    if (actual != expected) fail(step, actual == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL);
  }
};

ComPtr<ID3D12Device1> makeDevice() {
  ComPtr<ID3D12Device1> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "CreateDevice");
  return device;
}

ComPtr<ID3D12Fence> makeFence(ID3D12Device *device, UINT64 initial = 0) {
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  return fence;
}

void backend() {
  const auto module = GetModuleHandleW(L"d3d12.dll");
  using Id = const char *(WINAPI *)();
  const auto id = reinterpret_cast<Id>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  if (!id || std::strcmp(id(), "owned-graphics-d3d12")) fail("backend identity");
  std::printf("OWNED_D3D12_FENCE_EVENTS_BACKEND id=%s\n", id());
}

void asynchronous(ID3D12Device1 *device) {
  auto first = makeFence(device), second = makeFence(device);
  ID3D12Fence *fences[] = {first.Get(), second.Get()};
  const UINT64 values[] = {3, 7};
  Event all, any, secondDone;
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all.handle), "register ALL");
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, any.handle), "register ANY");
  all.expect(WAIT_TIMEOUT, 0, "ALL signaled before a producer");
  any.expect(WAIT_TIMEOUT, 0, "ANY signaled before a producer");
  check(first->Signal(3), "first CPU signal");
  any.expect(WAIT_OBJECT_0, kTimeout, "ANY did not observe first producer");
  all.expect(WAIT_TIMEOUT, 100, "ALL observed only one producer");
  check(second->SetEventOnCompletion(7, secondDone.handle), "register second fence control");
  check(second->Signal(7), "second CPU signal");
  all.expect(WAIT_OBJECT_0, kTimeout, "ALL did not observe both producers");
  secondDone.expect(WAIT_OBJECT_0, kTimeout, "single-fence callback control");
  any.expect(WAIT_TIMEOUT, 100, "ANY signaled a second time after auto-reset");

  Event completed;
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_NONE, completed.handle), "already completed ALL");
  completed.expect(WAIT_OBJECT_0, kTimeout, "already completed ALL notification");
  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=async_all_any exactly_once=1 already_completed=1");
}

void repeatedFence(ID3D12Device1 *device) {
  auto fence = makeFence(device);
  ID3D12Fence *fences[] = {fence.Get(), fence.Get(), fence.Get()};
  const UINT64 values[] = {2, 4, 4};
  Event done, firstDone;
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 3,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, done.handle), "repeated fence ALL");
  check(fence->SetEventOnCompletion(2, firstDone.handle), "repeated fence control");
  check(fence->Signal(2), "repeated fence signal first");
  firstDone.expect(WAIT_OBJECT_0, kTimeout, "repeated fence first callback");
  done.expect(WAIT_TIMEOUT, 100, "repeated fence ALL fired too early");
  check(fence->Signal(4), "repeated fence signal final");
  done.expect(WAIT_OBJECT_0, kTimeout, "repeated fence ALL final callback");
  done.expect(WAIT_TIMEOUT, 100, "repeated fence ALL fired twice");
  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=repeated_fence different_values=1 duplicate_values=1");
}

void synchronous(ID3D12Device1 *device, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags) {
  auto first = makeFence(device), second = makeFence(device);
  ID3D12Fence *fences[] = {first.Get(), second.Get()};
  const UINT64 values[] = {5, 9};
  Event entered, returned;
  HRESULT result = E_FAIL;
  std::thread waiter([&] {
    SetEvent(entered.handle);
    result = device->SetEventOnMultipleFenceCompletion(fences, values, 2, flags, nullptr);
    SetEvent(returned.handle);
  });
  entered.expect(WAIT_OBJECT_0, kTimeout, "null-event waiter startup");
  returned.expect(WAIT_TIMEOUT, 100, "null-event method returned before producers");
  check(second->Signal(9), "null-event second producer");
  if (flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL) {
    returned.expect(WAIT_TIMEOUT, 100, "null-event ALL returned after only one producer");
    check(first->Signal(5), "null-event first producer");
  }
  returned.expect(WAIT_OBJECT_0, kTimeout, "null-event wait did not return");
  waiter.join();
  check(result, "null-event wait result");
  if (flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY)
    check(first->Signal(5), "null-event ANY later producer");
  std::printf("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=null_event_%s synchronous=1\n",
              flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL ? "all" : "any");
}

void crossDevice(ID3D12Device1 *device) {
  auto other = makeDevice();
  auto first = makeFence(device), second = makeFence(other.Get());
  ID3D12Fence *fences[] = {first.Get(), second.Get()};
  const UINT64 values[] = {1, 1};
  Event done;
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, done.handle), "cross-owned-device ALL");
  check(first->Signal(1), "cross-device first producer");
  check(second->Signal(1), "cross-device second producer");
  done.expect(WAIT_OBJECT_0, kTimeout, "cross-device ALL notification");
  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=cross_owned_devices");
}

void invalidArguments(ID3D12Device1 *device) {
  auto fence = makeFence(device);
  ID3D12Fence *fences[] = {fence.Get(), nullptr};
  const UINT64 values[] = {1, 2};
  Event event;
  const std::array<HRESULT, 5> results = {
      device->SetEventOnMultipleFenceCompletion(nullptr, values, 1, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, event.handle),
      device->SetEventOnMultipleFenceCompletion(fences, nullptr, 1, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, event.handle),
      device->SetEventOnMultipleFenceCompletion(fences, values, 0, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, event.handle),
      device->SetEventOnMultipleFenceCompletion(fences, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, event.handle),
      device->SetEventOnMultipleFenceCompletion(fences, values, 1,
          static_cast<D3D12_MULTIPLE_FENCE_WAIT_FLAGS>(2), event.handle),
  };
  for (const HRESULT result : results)
    if (result != E_INVALIDARG) fail("invalid arguments exact HRESULT", result);
  check(fence->Signal(2), "invalid argument control producer");
  event.expect(WAIT_TIMEOUT, 100, "rejected registration signaled an event");
  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=invalid_arguments exact_hresult=1 no_partial_registration=1");
}

void teardown() {
  constexpr unsigned iterations = 64;
  // Keep handles alive through the entire stress case. A pending notification
  // is not permission for the application to close and reuse its event handle.
  std::vector<HANDLE> handles;
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    auto device = makeDevice();
    if ((iteration & 1) == 0) {
      auto fence = makeFence(device.Get());
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event) fail("teardown CreateEvent");
      handles.push_back(event);
      check(fence->SetEventOnCompletion(1, event), "teardown pending registration");
      if (iteration & 2) check(fence->Signal(1), "teardown racing notification");
    }
  }
  for (HANDLE event : handles) CloseHandle(event);
  std::printf("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=immediate_teardown iterations=%u pending_and_ready=1\n", iterations);
}
}

int main() {
  backend();
  auto device = makeDevice();
  asynchronous(device.Get());
  repeatedFence(device.Get());
  synchronous(device.Get(), D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL);
  synchronous(device.Get(), D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY);
  crossDevice(device.Get());
  invalidArguments(device.Get());
  teardown();
  std::puts("OWNED_D3D12_FENCE_EVENTS_PASS any_all=1 synchronous_null=1 callback_lifetime=1");
  return 0;
}
