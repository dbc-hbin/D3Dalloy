#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT64 kBytes = 256;
constexpr DWORD kTimeoutMs = 15000;
// NT event query access is omitted by the public llvm-mingw winnt.h.
constexpr DWORD kEventQueryStateAccess = 0x0001;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_RESIDENCY_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) {
    std::fprintf(stderr,
                 "OWNED_D3D12_RESIDENCY_HRESULT_MISMATCH step=%s expected=0x%08lx actual=0x%08lx\n",
                 step, static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    ExitProcess(1);
  }
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  if (!module)
    fail("GetModuleHandleW", HRESULT_FROM_WIN32(GetLastError()));
  FARPROC raw = GetProcAddress(module, "OwnedGraphicsBackendId");
  if (!raw)
    fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
  using BackendId = const char *(WINAPI *)();
  const char *id = reinterpret_cast<BackendId>(raw)();
  if (!id || std::strcmp(id, "owned-graphics-d3d12") != 0)
    fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_RESIDENCY_BACKEND id=%s\n", id);
}

struct AdapterRelease {
  IDXGIAdapter3 *adapter;
  ULONG remainingReferences;
};

DWORD WINAPI releaseAdapter(void *context) {
  auto *release = static_cast<AdapterRelease *>(context);
  release->remainingReferences = release->adapter->Release();
  return 0;
}

std::array<DXGI_QUERY_VIDEO_MEMORY_INFO, 2> queryBudget(IDXGIAdapter3 *adapter) {
  std::array<DXGI_QUERY_VIDEO_MEMORY_INFO, 2> info{};
  for (UINT segment = 0; segment < info.size(); ++segment)
    check(adapter->QueryVideoMemoryInfo(0, static_cast<DXGI_MEMORY_SEGMENT_GROUP>(segment),
                                       &info[segment]),
          "QueryVideoMemoryInfo node0");
  return info;
}

void printBudget(const char *phase, const std::array<DXGI_QUERY_VIDEO_MEMORY_INFO, 2> &info) {
  for (UINT segment = 0; segment < info.size(); ++segment)
    std::printf("OWNED_D3D12_RESIDENCY_BUDGET_INFO phase=%s node=0 segment=%u "
                "budget_bytes=%llu usage_bytes=%llu available_reservation_bytes=%llu "
                "reservation_bytes=%llu\n",
                phase, segment, static_cast<unsigned long long>(info[segment].Budget),
                static_cast<unsigned long long>(info[segment].CurrentUsage),
                static_cast<unsigned long long>(info[segment].AvailableForReservation),
                static_cast<unsigned long long>(info[segment].CurrentReservation));
}

void testAdapterBudget() {
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "budget factory");
  ComPtr<IDXGIAdapter1> adapter1;
  check(factory->EnumAdapters1(0, &adapter1), "budget adapter");
  ComPtr<IDXGIAdapter3> adapter;
  check(adapter1.As(&adapter), "QueryInterface(IDXGIAdapter3)");

  // This gate deliberately precedes null-cookie validation: the old no-op stub
  // returns the same cookie twice and would dereference a null cookie afterward.
  std::array<HANDLE, 2> events{};
  std::array<DWORD, 2> cookies{};
  for (UINT i = 0; i < events.size(); ++i) {
    events[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!events[i])
      fail("budget CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    check(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(events[i], &cookies[i]),
          "budget register distinct events");
  }
  // Cookies are opaque: zero is allowed; only active uniqueness is required.
  if (cookies[0] == cookies[1])
    fail("budget distinct active cookies");

  auto rejectRegistration = [&](HANDLE event, DWORD *cookie, const char *step) {
    const HRESULT hr = adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(event, cookie);
    if (SUCCEEDED(hr))
      fail(step, hr);
  };
  DWORD rejectedCookie = 0;
  rejectRegistration(events[0], nullptr, "budget null cookie rejected");
  rejectRegistration(nullptr, &rejectedCookie, "budget null handle rejected");
  rejectRegistration(INVALID_HANDLE_VALUE, &rejectedCookie, "budget invalid handle rejected");
  HANDLE semaphore = CreateSemaphoreW(nullptr, 0, 1, nullptr);
  if (!semaphore)
    fail("budget CreateSemaphore", HRESULT_FROM_WIN32(GetLastError()));
  // Semaphore modify access overlaps EVENT_MODIFY_STATE: rights alone are not type validation.
  rejectRegistration(semaphore, &rejectedCookie, "budget non-event rejected");
  if (WaitForSingleObject(semaphore, 0) != WAIT_TIMEOUT ||
      !ReleaseSemaphore(semaphore, 1, nullptr) ||
      WaitForSingleObject(semaphore, 0) != WAIT_OBJECT_0)
    fail("budget rejected semaphore state preserved");
  CloseHandle(semaphore);

  HANDLE witness = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!witness)
    fail("budget witness CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
  for (DWORD access : {static_cast<DWORD>(SYNCHRONIZE), kEventQueryStateAccess}) {
    HANDLE restricted = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), witness, GetCurrentProcess(), &restricted,
                         access, FALSE, 0))
      fail("budget restricted DuplicateHandle", HRESULT_FROM_WIN32(GetLastError()));
    rejectRegistration(restricted, &rejectedCookie, "budget nonsignaling rights rejected");
    CloseHandle(restricted);
  }
  if (WaitForSingleObject(witness, 0) != WAIT_TIMEOUT)
    fail("budget rejected registration changed witness");
  HANDLE modifyOnly = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), witness, GetCurrentProcess(), &modifyOnly,
                       EVENT_MODIFY_STATE, FALSE, 0))
    fail("budget modify-only DuplicateHandle", HRESULT_FROM_WIN32(GetLastError()));
  DWORD witnessCookie = 0;
  check(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(modifyOnly, &witnessCookie),
        "budget modify-only registration");
  CloseHandle(modifyOnly); // The registration must own a duplicate, not borrow this handle.

  DWORD repeatedCookie = 0;
  check(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(events[0], &repeatedCookie),
        "budget repeated event registration");
  HANDLE initialEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
  if (!initialEvent)
    fail("budget initial CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
  DWORD initialCookie = 0;
  check(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(initialEvent, &initialCookie),
        "budget initially signaled auto event registration");
  if (WaitForSingleObject(initialEvent, 0) != WAIT_OBJECT_0)
    fail("budget registration consumed initial auto signal");
  // A second wake may be a real native transition. Never infer stability from equal endpoints.
  const DWORD initialSecondWait = WaitForSingleObject(initialEvent, 0);
  if (initialSecondWait != WAIT_OBJECT_0 && initialSecondWait != WAIT_TIMEOUT)
    fail("budget initial second wait", HRESULT_FROM_WIN32(GetLastError()));
  const std::array<DWORD, 5> activeCookies{cookies[0], cookies[1], witnessCookie,
                                          repeatedCookie, initialCookie};
  for (size_t i = 0; i < activeCookies.size(); ++i)
    for (size_t j = i + 1; j < activeCookies.size(); ++j)
      if (activeCookies[i] == activeCookies[j])
        fail("budget repeated/restricted distinct active cookies");

  const auto original = queryBudget(adapter.Get());
  printBudget("original", original);
  DXGI_QUERY_VIDEO_MEMORY_INFO invalidInfo{};
  expect(adapter->QueryVideoMemoryInfo(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &invalidInfo),
         E_INVALIDARG, "budget invalid node query");
  expect(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, nullptr),
         E_INVALIDARG, "budget null query output");
  const auto invalidSegment = static_cast<DXGI_MEMORY_SEGMENT_GROUP>(2);
  expect(adapter->QueryVideoMemoryInfo(0, invalidSegment, &invalidInfo),
         E_INVALIDARG, "budget invalid segment query");
  expect(adapter->SetVideoMemoryReservation(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 1),
         E_INVALIDARG, "budget invalid node reservation");
  expect(adapter->SetVideoMemoryReservation(0, invalidSegment, 1),
         E_INVALIDARG, "budget invalid segment reservation");
  for (UINT segment = 0; segment < original.size(); ++segment)
    if (!original[segment].Budget || original[segment].AvailableForReservation != 0 ||
        original[segment].CurrentReservation > original[segment].AvailableForReservation)
      fail("budget native query policy");
  // Stay within AvailableForReservation (currently zero). This exercises the
  // public setter/reporting contract, not a budget-change stimulus or negative control.
  for (UINT segment = 0; segment < original.size(); ++segment) {
    check(adapter->SetVideoMemoryReservation(0, static_cast<DXGI_MEMORY_SEGMENT_GROUP>(segment),
                                             0),
          "budget available reservation");
    const auto reserved = queryBudget(adapter.Get());
    for (UINT other = 0; other < reserved.size(); ++other)
      if (reserved[other].CurrentReservation !=
          (other <= segment ? 0 : original[other].CurrentReservation))
        fail("budget independent reservation report");
  }
  printBudget("reserved", queryBudget(adapter.Get()));
  for (UINT segment = 0; segment < original.size(); ++segment)
    check(adapter->SetVideoMemoryReservation(0, static_cast<DXGI_MEMORY_SEGMENT_GROUP>(segment),
                                             original[segment].CurrentReservation),
          "budget restore reservation");
  const auto restored = queryBudget(adapter.Get());
  for (UINT segment = 0; segment < restored.size(); ++segment)
    if (restored[segment].CurrentReservation != original[segment].CurrentReservation)
      fail("budget restored reservation mismatch");
  printBudget("restored", restored);

  // Observe real hardware without pressure, source overrides, or requiring a
  // physical transition. Notifications are coalesced hints, not a wake-per-cookie ledger.
  const HANDLE observedEvents[]{events[0], events[1], witness};
  const auto before = queryBudget(adapter.Get());
  auto previous = before;
  unsigned observedChanges = 0;
  std::array<unsigned, 3> wakes{};
  DWORD lastWait = WAIT_TIMEOUT;
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= 1500)
      break;
    const DWORD remaining = static_cast<DWORD>(1500 - elapsed);
    lastWait = WaitForMultipleObjects(3, observedEvents, FALSE, remaining < 100 ? remaining : 100);
    if (lastWait >= WAIT_OBJECT_0 && lastWait < WAIT_OBJECT_0 + 3)
      ++wakes[lastWait - WAIT_OBJECT_0];
    else if (lastWait != WAIT_TIMEOUT)
      fail("budget native observation wait", HRESULT_FROM_WIN32(GetLastError()));
    const auto current = queryBudget(adapter.Get());
    for (UINT segment = 0; segment < current.size(); ++segment) {
      if (current[segment].Budget != previous[segment].Budget) {
        ++observedChanges;
        std::printf("OWNED_D3D12_RESIDENCY_BUDGET_CHANGE segment=%u previous_bytes=%llu "
                    "current_bytes=%llu elapsed_ms=%llu\n", segment,
                    static_cast<unsigned long long>(previous[segment].Budget),
                    static_cast<unsigned long long>(current[segment].Budget),
                    static_cast<unsigned long long>(GetTickCount64() - started));
      }
    }
    previous = current;
  }
  printBudget("observed", previous);
  std::printf("OWNED_D3D12_RESIDENCY_BUDGET_MEASUREMENT elapsed_ms=%llu "
              "local_before_bytes=%llu local_after_bytes=%llu nonlocal_before_bytes=%llu "
              "nonlocal_after_bytes=%llu observed_segment_changes=%u event_wakes=%u,%u,%u "
              "last_wait=%lu initial_second_wait=%lu physical_change_required=0\n",
              static_cast<unsigned long long>(GetTickCount64() - started),
              static_cast<unsigned long long>(before[0].Budget),
              static_cast<unsigned long long>(previous[0].Budget),
              static_cast<unsigned long long>(before[1].Budget),
              static_cast<unsigned long long>(previous[1].Budget), observedChanges,
              wakes[0], wakes[1], wakes[2], static_cast<unsigned long>(lastWait),
              static_cast<unsigned long>(initialSecondWait));

  DWORD unknownCookie = 0;
  for (;;) {
    bool active = false;
    for (DWORD cookie : activeCookies)
      active = active || cookie == unknownCookie;
    if (!active)
      break;
    ++unknownCookie;
  }
  adapter->UnregisterVideoMemoryBudgetChangeNotification(unknownCookie); // Chosen policy: ignored.
  adapter->UnregisterVideoMemoryBudgetChangeNotification(cookies[0]);
  adapter->UnregisterVideoMemoryBudgetChangeNotification(cookies[0]); // Idempotent unknown cookie.
  adapter->UnregisterVideoMemoryBudgetChangeNotification(repeatedCookie);
  adapter->UnregisterVideoMemoryBudgetChangeNotification(cookies[1]);
  for (HANDLE event : events) {
    if (!ResetEvent(event) || !SetEvent(event) || WaitForSingleObject(event, 0) != WAIT_OBJECT_0 ||
        WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
      fail("budget caller handle usable after unregister");
  }
  // Leave witness/initial registrations live; drop every COM alias before the
  // final Release and check that joining the observer is bounded, with no self-reference.
  adapter1.Reset();
  factory.Reset();
  AdapterRelease release{adapter.Detach(), ~0u};
  const ULONGLONG releaseStarted = GetTickCount64();
  HANDLE thread = CreateThread(nullptr, 0, releaseAdapter, &release, 0, nullptr);
  if (!thread)
    fail("budget release CreateThread", HRESULT_FROM_WIN32(GetLastError()));
  if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0)
    fail("budget release with live registrations bounded");
  CloseHandle(thread);
  if (release.remainingReferences != 0)
    fail("budget release with live registrations refcycle");
  for (HANDLE event : {witness, initialEvent}) {
    if (!ResetEvent(event) || !SetEvent(event) || WaitForSingleObject(event, 0) != WAIT_OBJECT_0 ||
        WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
      fail("budget caller handle usable after adapter release");
    CloseHandle(event);
  }
  for (HANDLE event : events)
    CloseHandle(event);
  std::printf("OWNED_D3D12_RESIDENCY_BUDGET_TEARDOWN elapsed_ms=%llu remaining_refs=%lu\n",
              static_cast<unsigned long long>(GetTickCount64() - releaseStarted),
              static_cast<unsigned long>(release.remainingReferences));
  std::puts("OWNED_D3D12_RESIDENCY_BUDGET_PASS");
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CreationNodeMask = 1;
  properties.VisibleNodeMask = 1;
  return properties;
}

D3D12_RESOURCE_DESC bufferDescription() {
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = kBytes;
  description.Height = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.SampleDesc.Count = 1;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return description;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, D3D12_HEAP_TYPE type,
                                    D3D12_HEAP_FLAGS flags, D3D12_RESOURCE_STATES state) {
  const auto properties = heapProperties(type);
  const auto description = bufferDescription();
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&properties, flags, &description, state, nullptr,
                                        IID_PPV_ARGS(&resource)),
        "CreateCommittedResource");
  return resource;
}

void waitFence(ID3D12Fence *fence, UINT64 value) {
  if (fence->GetCompletedValue() >= value)
    return;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event)
    fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
  check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
  const DWORD result = WaitForSingleObject(event, kTimeoutMs);
  CloseHandle(event);
  if (result != WAIT_OBJECT_0)
    fail("WaitForSingleObject", result == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                                        : HRESULT_FROM_WIN32(GetLastError()));
}

} // namespace

int main() {
  requireOwnedBackend();
  testAdapterBudget();

  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  ComPtr<ID3D12Device3> device3;
  check(device.As(&device3), "QueryInterface(ID3D12Device3)");

  D3D12_COMMAND_QUEUE_DESC queueDescription{};
  queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  ComPtr<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "CreateCommandAllocator");
  ComPtr<ID3D12Pageable> unsupported;
  check(allocator.As(&unsupported), "allocator pageable");

  auto target = createBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Pageable> targetPageable;
  check(target.As(&targetPageable), "target pageable");
  ID3D12Pageable *targetObject[] = {targetPageable.Get()};

  expect(device->MakeResident(1, nullptr), E_INVALIDARG, "MakeResident null array");
  ID3D12Pageable *unsupportedObject[] = {unsupported.Get()};
  expect(device->MakeResident(1, unsupportedObject), E_INVALIDARG, "MakeResident unsupported pageable");
  ID3D12Pageable *mixedObjects[] = {targetPageable.Get(), unsupported.Get()};
  expect(device->MakeResident(2, mixedObjects), E_INVALIDARG, "MakeResident atomic validation");
  check(device->Evict(1, targetObject), "Evict initial reference");
  expect(device->Evict(1, targetObject), E_INVALIDARG, "Evict reference underflow");

  D3D12_RESIDENCY_PRIORITY priority = static_cast<D3D12_RESIDENCY_PRIORITY>(
      static_cast<UINT>(D3D12_RESIDENCY_PRIORITY_NORMAL) + 17u);
  check(device3->SetResidencyPriority(1, targetObject, &priority), "SetResidencyPriority custom hint");
  expect(device3->SetResidencyPriority(1, targetObject, nullptr), E_INVALIDARG,
         "SetResidencyPriority null priorities");

  auto notResident = createBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Pageable> notResidentPageable;
  check(notResident.As(&notResidentPageable), "not-resident pageable");
  ID3D12Pageable *notResidentObject[] = {notResidentPageable.Get()};
  expect(device->Evict(1, notResidentObject), E_INVALIDARG, "CREATE_NOT_RESIDENT initial state");
  check(device->MakeResident(1, notResidentObject), "MakeResident CREATE_NOT_RESIDENT");
  check(device->Evict(1, notResidentObject), "Evict CREATE_NOT_RESIDENT");
  std::puts("OWNED_D3D12_RESIDENCY_CASE validation=1 refcounts=1 create_not_resident=1 priority=1");

  ComPtr<ID3D12Fence> residencyFence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&residencyFence)),
        "CreateFence residency");
  check(device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET, 1, targetObject,
                                     residencyFence.Get(), 6),
        "EnqueueMakeResident DENY_OVERBUDGET");
  waitFence(residencyFence.Get(), 6);
  check(device->Evict(1, targetObject), "Evict after budget-checked residency");
  check(device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, 1, targetObject,
                                     residencyFence.Get(), 7),
        "EnqueueMakeResident");
  waitFence(residencyFence.Get(), 7);
  if (residencyFence->GetCompletedValue() < 7)
    fail("residency fence not completed");
  std::printf("OWNED_D3D12_RESIDENCY_FENCE value=%llu completed=%llu\n",
              static_cast<unsigned long long>(7),
              static_cast<unsigned long long>(residencyFence->GetCompletedValue()));

  auto upload = createBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = createBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<std::uint8_t, kBytes> expected{};
  for (size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xffu);
  void *mapped = nullptr;
  const D3D12_RANGE noRead{0, 0};
  check(upload->Map(0, &noRead, &mapped), "upload Map");
  std::memcpy(mapped, expected.data(), expected.size());
  const D3D12_RANGE allWritten{0, kBytes};
  upload->Unmap(0, &allWritten);

  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)),
        "CreateCommandList");
  list->CopyBufferRegion(target.Get(), 0, upload.Get(), 0, kBytes);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = target.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &barrier);
  list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, kBytes);
  check(list->Close(), "command list Close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> gpuFence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpuFence)), "CreateFence GPU");
  check(queue->Signal(gpuFence.Get(), 1), "queue Signal");
  waitFence(gpuFence.Get(), 1);

  const D3D12_RANGE allRead{0, kBytes};
  check(readback->Map(0, &allRead, &mapped), "readback Map");
  if (std::memcmp(mapped, expected.data(), expected.size()) != 0)
    fail("readback mismatch");
  std::uint32_t checksum = 0;
  for (std::uint8_t value : expected)
    checksum = checksum * 16777619u ^ value;
  readback->Unmap(0, &noRead);
  std::printf("OWNED_D3D12_RESIDENCY_READBACK bytes=%llu checksum=0x%08x\n",
              static_cast<unsigned long long>(kBytes), checksum);
  std::puts("OWNED_D3D12_RESIDENCY_PASS validation=1 refcounts=1 create_not_resident=1 deny_overbudget=1 enqueue_fence=1 readback=1");
  return 0;
}
