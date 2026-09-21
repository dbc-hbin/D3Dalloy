#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT64 kBytes = 256;
constexpr DWORD kTimeoutMs = 15000;

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
