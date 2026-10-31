#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
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

ComPtr<ID3D12CommandQueue> makeQueue(ID3D12Device *device) {
  D3D12_COMMAND_QUEUE_DESC desc = {};
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  return queue;
}

void requireValue(ID3D12Fence *fence, UINT64 expected, const char *step) {
  if (fence->GetCompletedValue() != expected) fail(step);
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
  // Already-reached single-fence values take the registration-free path.
  Event reached;
  check(second->SetEventOnCompletion(5, reached.handle), "already completed single");
  reached.expect(WAIT_OBJECT_0, 0, "already completed single notification");
  reached.expect(WAIT_TIMEOUT, 0, "already completed single fired twice");
  check(second->SetEventOnCompletion(7, nullptr), "already completed single null event");
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, nullptr), "already completed ALL null event");
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

void cpuRewindEvents(ID3D12Device1 *device) {
  auto fence = makeFence(device, 10), other = makeFence(device);
  check(fence->Signal(5), "CPU rewind 10 to 5");
  requireValue(fence.Get(), 5, "CPU rewind did not publish 5");

  ID3D12Fence *fences[] = {fence.Get(), other.Get()};
  const UINT64 values[] = {7, 1};
  Event single, any, all, nullEntered, nullReturned;
  check(fence->SetEventOnCompletion(7, single.handle), "rewind single registration");
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, any.handle), "rewind ANY registration");
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 2,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all.handle), "rewind ALL registration");
  single.expect(WAIT_TIMEOUT, 0, "rewind single observed stale 10");
  any.expect(WAIT_TIMEOUT, 0, "rewind ANY observed stale 10");
  all.expect(WAIT_TIMEOUT, 0, "rewind ALL observed stale 10");

  HRESULT nullResult = E_FAIL;
  std::thread waiter([&] {
    SetEvent(nullEntered.handle);
    nullResult = fence->SetEventOnCompletion(7, nullptr);
    SetEvent(nullReturned.handle);
  });
  nullEntered.expect(WAIT_OBJECT_0, kTimeout, "rewind null-event waiter startup");
  nullReturned.expect(WAIT_TIMEOUT, 0, "rewind null event observed stale 10");
  check(other->Signal(1), "rewind ALL second fence");
  all.expect(WAIT_TIMEOUT, 0, "rewind ALL ignored rewound fence");
  check(fence->Signal(5), "equal CPU signal");
  requireValue(fence.Get(), 5, "equal CPU signal changed value");
  single.expect(WAIT_TIMEOUT, 0, "equal CPU signal fired future event");
  check(fence->Signal(7), "rewind future CPU signal");
  single.expect(WAIT_OBJECT_0, kTimeout, "rewind single completion");
  any.expect(WAIT_OBJECT_0, kTimeout, "rewind ANY completion");
  all.expect(WAIT_OBJECT_0, kTimeout, "rewind ALL completion");
  nullReturned.expect(WAIT_OBJECT_0, kTimeout, "rewind null-event completion");
  waiter.join();
  check(nullResult, "rewind null-event result");
  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=cpu_rewind_events from=10 to=5 single=1 any=1 all=1 null=1 equal=1");
}

void gpuSignals(ID3D12Device1 *device) {
  auto queue = makeQueue(device);
  auto fence = makeFence(device), retired = makeFence(device);
  Event highObserved, retiredEvent;
  check(fence->SetEventOnCompletion(10, highObserved.handle), "GPU rewind high-value event");
  check(retired->SetEventOnCompletion(1, retiredEvent.handle), "GPU rewind retirement event");
  check(queue->Signal(fence.Get(), 10), "GPU signal 10");
  check(queue->Signal(fence.Get(), 5), "GPU rewind 10 to 5");
  check(queue->Signal(fence.Get(), 5), "equal GPU signal 5");
  check(queue->Signal(retired.Get(), 1), "GPU rewind retirement marker");
  retiredEvent.expect(WAIT_OBJECT_0, kTimeout, "GPU rewind retirement");
  requireValue(fence.Get(), 5, "GPU rewind did not publish 5");
  highObserved.expect(WAIT_OBJECT_0, kTimeout, "GPU high-value event unlatched after rewind");

  auto gate = makeFence(device), mixed = makeFence(device);
  Event mixedDone;
  check(queue->Wait(gate.Get(), 1), "mixed producer gate wait");
  check(queue->Signal(mixed.Get(), 10), "gated GPU signal 10");
  check(mixed->Signal(5), "CPU signal while GPU signal gated");
  requireValue(mixed.Get(), 5, "gated CPU signal did not publish 5");
  check(mixed->SetEventOnCompletion(10, mixedDone.handle), "mixed producer completion event");
  mixedDone.expect(WAIT_TIMEOUT, 0, "gated GPU signal completed before gate");
  check(gate->Signal(1), "release mixed producer gate");
  mixedDone.expect(WAIT_OBJECT_0, kTimeout, "gated GPU signal did not complete");
  requireValue(mixed.Get(), 10, "GPU completion after CPU signal did not publish 10");

  auto blockedQueue = makeQueue(device), producerQueue = makeQueue(device), waiterQueue = makeQueue(device);
  auto blockedGate = makeFence(device), competing = makeFence(device), waiterDone = makeFence(device);
  auto blockedDone = makeFence(device);
  Event waiterEvent, blockedEvent;
  check(blockedQueue->Wait(blockedGate.Get(), 1), "competing blocked queue gate");
  check(blockedQueue->Signal(competing.Get(), 10), "competing blocked first qualifying signal");
  check(blockedQueue->Signal(blockedDone.Get(), 1), "competing blocked signal marker");
  check(blockedDone->SetEventOnCompletion(1, blockedEvent.handle), "competing blocked marker event");
  check(producerQueue->Signal(competing.Get(), 10), "competing second producer signal");
  check(waiterQueue->Wait(competing.Get(), 10), "competing future wait");
  check(waiterQueue->Signal(waiterDone.Get(), 1), "competing future wait marker");
  check(waiterDone->SetEventOnCompletion(1, waiterEvent.handle), "competing future wait event");
  waiterEvent.expect(WAIT_OBJECT_0, kTimeout, "blocked first producer prevented second producer");
  requireValue(competing.Get(), 10, "second producer did not publish future wait value");
  check(blockedGate->Signal(1), "release competing blocked queue");
  blockedEvent.expect(WAIT_OBJECT_0, kTimeout, "competing blocked signal did not retire");
  requireValue(competing.Get(), 10, "late equal competing signal changed value");

  auto latchQueue = makeQueue(device);
  auto latchGate = makeFence(device), latched = makeFence(device), latchDone = makeFence(device);
  Event latchEvent;
  check(latchQueue->Wait(latchGate.Get(), 1), "API-time latch gate wait");
  check(latchQueue->Wait(latched.Get(), 10), "API-time pending queue wait");
  check(latchQueue->Signal(latchDone.Get(), 1), "API-time latch tail signal");
  check(latchDone->SetEventOnCompletion(1, latchEvent.handle), "API-time latch tail event");
  check(latched->Signal(10), "API-time threshold crossing while worker delayed");
  check(latched->Signal(5), "API-time latch rewind before worker");
  requireValue(latched.Get(), 5, "API-time latch rewind did not publish 5");
  check(latchGate->Signal(1), "release API-time latch worker");
  latchEvent.expect(WAIT_OBJECT_0, kTimeout, "queue wait lost threshold crossing while worker delayed");

  auto lifetimeGate = makeFence(device);
  Event lifetimeDone;
  {
    auto retained = makeFence(device);
    check(queue->Wait(lifetimeGate.Get(), 1), "retained signal gate wait");
    check(queue->Signal(retained.Get(), 1), "retained queued signal");
    check(retained->SetEventOnCompletion(1, lifetimeDone.handle), "retained completion event");
  }
  check(lifetimeGate->Signal(1), "release retained signal gate");
  lifetimeDone.expect(WAIT_OBJECT_0, kTimeout, "queued signal did not retain fence");
  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=gpu_rewind_mixed from=10 to=5 equal=1 gated_cpu_gpu=1 competing_queues=1 api_wait_latch=1 retained=1");
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

void explicitDeviceRemoval(ID3D12Device1 *device) {
  ComPtr<ID3D12Device5> device5;
  check(device->QueryInterface(IID_PPV_ARGS(&device5)), "removal QueryInterface Device5");
  if (device->GetDeviceRemovedReason() != S_OK) fail("removal subject is already removed");

  auto first = makeFence(device), second = makeFence(device, 7);
  auto unmonitored = makeFence(device, 19);
  ComPtr<ID3D12Fence> shared;
  check(device->CreateFence(11, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared)),
        "removal CreateFence shared");
  HANDLE sharedHandle = nullptr;
  check(device->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle),
        "removal CreateSharedHandle");
  ComPtr<ID3D12Fence> openedShared;
  const HRESULT openResult = device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&openedShared));
  if (!CloseHandle(sharedHandle)) fail("removal CloseHandle shared", HRESULT_FROM_WIN32(GetLastError()));
  check(openResult, "removal OpenSharedHandle");
  requireValue(second.Get(), 7, "removal initial nonzero fence");
  requireValue(shared.Get(), 11, "removal initial shared fence");
  requireValue(openedShared.Get(), 11, "removal initial opened shared fence");
  ID3D12Fence *fences[] = {first.Get(), second.Get(), shared.Get(), openedShared.Get()};
  const UINT64 values[] = {UINT64_MAX, 13, 17, UINT64_MAX};
  Event maximum, lower, sharedDone, any, all;
  check(first->SetEventOnCompletion(UINT64_MAX, maximum.handle), "removal maximum single registration");
  check(second->SetEventOnCompletion(13, lower.handle), "removal lower single registration");
  check(openedShared->SetEventOnCompletion(UINT64_MAX, sharedDone.handle), "removal opened shared registration");
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 4,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, any.handle), "removal ANY registration");
  check(device->SetEventOnMultipleFenceCompletion(fences, values, 4,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all.handle), "removal ALL registration");
  const std::array<Event *, 5> notifications = {&maximum, &lower, &sharedDone, &any, &all};
  for (const auto event : notifications)
    event->expect(WAIT_TIMEOUT, 0, "removal pending event initially ready");

  std::array<Event, 3> entered, returned;
  std::array<HRESULT, 3> results = {E_FAIL, E_FAIL, E_FAIL};
  std::array<std::thread, 3> waiters;
  for (std::size_t i = 0; i < waiters.size(); ++i) {
    waiters[i] = std::thread([&, i] {
      SetEvent(entered[i].handle);
      if (i == 0)
        results[i] = first->SetEventOnCompletion(UINT64_MAX, nullptr);
      else
        results[i] = device->SetEventOnMultipleFenceCompletion(fences, values, 4,
            i == 1 ? D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY : D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, nullptr);
      SetEvent(returned[i].handle);
    });
  }
  for (std::size_t i = 0; i < waiters.size(); ++i) {
    entered[i].expect(WAIT_OBJECT_0, kTimeout, "removal null-event waiter startup");
    returned[i].expect(WAIT_TIMEOUT, 100, "removal null-event method returned before removal");
  }

  std::array<Event, 4> raceGate, raceReturned;
  std::array<Event, 3> raceNotifications;
  std::array<HRESULT, 4> raceResults = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
  std::array<std::thread, 4> racers;
  ComPtr<ID3D12Fence> raceFence;
  for (std::size_t i = 0; i < racers.size(); ++i) {
    racers[i] = std::thread([&, i] {
      raceGate[i].expect(WAIT_OBJECT_0, kTimeout, "removal race start gate");
      if (i == 3)
        raceResults[i] = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&raceFence));
      else if (i == 0)
        raceResults[i] = openedShared->SetEventOnCompletion(UINT64_MAX, raceNotifications[i].handle);
      else
        raceResults[i] = device->SetEventOnMultipleFenceCompletion(fences, values, 4,
            i == 1 ? D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY : D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL,
            raceNotifications[i].handle);
      SetEvent(raceReturned[i].handle);
    });
  }
  // Release once around the same removal transition; either scheduling outcome
  // is valid, but successful calls must not leave fences or events stranded.
  for (const auto &gate : raceGate)
    if (!SetEvent(gate.handle)) fail("removal release race gate", HRESULT_FROM_WIN32(GetLastError()));
  device5->RemoveDevice();
  HRESULT reason = device->GetDeviceRemovedReason();
  if (reason != DXGI_ERROR_DEVICE_REMOVED) fail("explicit removal reason", reason);
  const std::array<ID3D12Fence *, 5> removedFences = {first.Get(), second.Get(), shared.Get(), openedShared.Get(), unmonitored.Get()};
  for (const auto fence : removedFences)
    requireValue(fence, UINT64_MAX, "removal did not terminally complete every fence");
  for (const auto event : notifications)
    event->expect(WAIT_OBJECT_0, kTimeout, "removal pending event did not fire");
  // A failed wait exits the process before any thread destructor or join can
  // hang on the broken removal path. Successful completion bounds every join.
  for (const auto &event : returned)
    event.expect(WAIT_OBJECT_0, kTimeout, "removal null-event wait did not return");
  for (auto &waiter : waiters) waiter.join();
  for (const HRESULT result : results)
    if (result != DXGI_ERROR_DEVICE_REMOVED) fail("removal null-event result", result);
  for (const auto &event : raceReturned)
    event.expect(WAIT_OBJECT_0, kTimeout, "removal racing call did not return");
  for (auto &racer : racers) racer.join();
  for (std::size_t i = 0; i < raceResults.size(); ++i) {
    const HRESULT result = raceResults[i];
    if (result != S_OK && result != DXGI_ERROR_DEVICE_REMOVED)
      fail("removal racing call result", result);
    if (i == 3) {
      if (result == S_OK) {
        if (!raceFence) fail("removal racing CreateFence omitted output");
        requireValue(raceFence.Get(), UINT64_MAX, "removal racing fence stranded");
      } else if (raceFence) {
        fail("removal rejected racing CreateFence populated output");
      }
    } else {
      raceNotifications[i].expect(result == S_OK ? WAIT_OBJECT_0 : WAIT_TIMEOUT,
          result == S_OK ? kTimeout : 0, "removal racing registration notification");
      raceNotifications[i].expect(WAIT_TIMEOUT, 0, "removal racing notification repeated");
    }
  }
  for (const auto event : notifications)
    event->expect(WAIT_TIMEOUT, 100, "removal event fired more than once");

  HRESULT hr = first->Signal(1);
  if (hr != DXGI_ERROR_DEVICE_REMOVED) fail("removed fence accepted CPU Signal", hr);
  Event lateSingle, lateAny, lateAll;
  hr = first->SetEventOnCompletion(UINT64_MAX, lateSingle.handle);
  if (hr != DXGI_ERROR_DEVICE_REMOVED) fail("removed fence accepted single registration", hr);
  hr = device->SetEventOnMultipleFenceCompletion(fences, values, 4,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, lateAny.handle);
  if (hr != DXGI_ERROR_DEVICE_REMOVED) fail("removed device accepted ANY registration", hr);
  hr = device->SetEventOnMultipleFenceCompletion(fences, values, 4,
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, lateAll.handle);
  if (hr != DXGI_ERROR_DEVICE_REMOVED) fail("removed device accepted ALL registration", hr);
  ComPtr<ID3D12Fence> lateFence;
  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lateFence));
  if (hr != DXGI_ERROR_DEVICE_REMOVED) fail("removed device accepted new fence", hr);
  if (lateFence) fail("removed CreateFence populated initialized output");
  lateSingle.expect(WAIT_TIMEOUT, 100, "rejected late single registration fired");
  lateAny.expect(WAIT_TIMEOUT, 100, "rejected late ANY registration fired");
  lateAll.expect(WAIT_TIMEOUT, 100, "rejected late ALL registration fired");

  device5->RemoveDevice();
  device5->RemoveDevice();
  reason = device->GetDeviceRemovedReason();
  if (reason != DXGI_ERROR_DEVICE_REMOVED) fail("repeated removal changed reason", reason);
  for (const auto fence : removedFences)
    requireValue(fence, UINT64_MAX, "repeated removal or rejected Signal changed terminal fence");
  for (const auto event : notifications)
    event->expect(WAIT_TIMEOUT, 100, "repeated removal duplicated notification");

  std::puts("OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=explicit_device_removal maximum=1 lower=1 shared_opened=1 any_all=1 null_single_any_all=1 all_fences_terminal=1 late_rejected=1 idempotent=1 registration_creation_race=1");
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
  cpuRewindEvents(device.Get());
  gpuSignals(device.Get());
  crossDevice(device.Get());
  invalidArguments(device.Get());
  teardown();
  explicitDeviceRemoval(device.Get());
  std::puts("OWNED_D3D12_FENCE_EVENTS_PASS any_all=1 synchronous_null=1 callback_lifetime=1 rewind=1 mixed_producers=1");
  return 0;
}
