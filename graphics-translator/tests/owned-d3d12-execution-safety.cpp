#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_EXECUTION_SAFETY_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr);
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) fail(step, hr);
}
void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) fail(step, actual);
}
ComPtr<ID3D12Device> device() {
  ComPtr<ID3D12Device> value;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&value)), "device");
  return value;
}
ComPtr<ID3D12CommandQueue> queue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
  D3D12_COMMAND_QUEUE_DESC desc{};
  desc.Type = type;
  ComPtr<ID3D12CommandQueue> value;
  check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&value)), "queue");
  return value;
}
ComPtr<ID3D12CommandAllocator> allocator(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
  ComPtr<ID3D12CommandAllocator> value;
  check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&value)), "allocator");
  return value;
}
ComPtr<ID3D12GraphicsCommandList> list(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type,
                                       ID3D12CommandAllocator *allocator) {
  ComPtr<ID3D12GraphicsCommandList> value;
  check(device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&value)), "list");
  return value;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, unsigned size) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> value;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&value)), "buffer");
  return value;
}
ComPtr<ID3D12Resource> texture(ID3D12Device *device, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COPY_DEST,
                               UINT16 mipLevels = 1) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 8;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = mipLevels;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> value;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        initial, nullptr, IID_PPV_ARGS(&value)), "texture");
  return value;
}
void wait(ID3D12Fence *fence, UINT64 value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) fail("event");
  check(fence->SetEventOnCompletion(value, event), "completion-event");
  const DWORD result = WaitForSingleObject(event, 15000);
  CloseHandle(event);
  if (result != WAIT_OBJECT_0) fail("completion-wait", HRESULT_FROM_WIN32(result));
}
void validReplay() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto first = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto second = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, first.Get());
  auto input = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 256);
  auto output = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256);
  void *mapped = nullptr;
  check(input->Map(0, nullptr, &mapped), "map-input");
  auto *words = static_cast<std::uint32_t *>(mapped);
  words[0] = 0x12345678;
  words[1] = 0xabcdef09;
  input->Unmap(0, nullptr);
  command->CopyBufferRegion(output.Get(), 0, input.Get(), 0, 4);
  check(command->Close(), "close-first");
  ComPtr<ID3D12Fence> gate, done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "done");
  check(q->Wait(gate.Get(), 1), "queue-wait");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  // Microsoft's HelloFrameBuffering reuses one command-list COM object with
  // per-frame allocators before the immediately preceding frame retires.
  check(command->Reset(second.Get(), nullptr), "reset-list-other-allocator");
  command->CopyBufferRegion(output.Get(), 4, input.Get(), 4, 4);
  check(command->Close(), "close-second");
  q->ExecuteCommandLists(1, submitted);
  check(q->Signal(done.Get(), 1), "signal-both");
  expect(first->Reset(), E_FAIL, "inflight-allocator-reset-must-fail");
  check(gate->Signal(1), "release-gate");
  wait(done.Get(), 1);
  // Fence publication must follow every preceding allocator lease release;
  // Reset must succeed immediately after each observed GPU completion.
  check(first->Reset(), "first-allocator-retired");
  for (UINT64 value = 2; value <= 17; ++value) {
    check(second->Reset(), "replay-allocator-retired");
    check(command->Reset(second.Get(), nullptr), "replay-list-reset");
    command->CopyBufferRegion(output.Get(), 4, input.Get(), 4, 4);
    check(command->Close(), "replay-list-close");
    q->ExecuteCommandLists(1, submitted);
    check(q->Signal(done.Get(), value), "replay-signal");
    wait(done.Get(), value);
  }
  check(output->Map(0, nullptr, &mapped), "map-output");
  words = static_cast<std::uint32_t *>(mapped);
  if (words[0] != 0x12345678 || words[1] != 0xabcdef09)
    fail("old-and-new-recording-replay");
  output->Unmap(0, nullptr);
  expect(dev->GetDeviceRemovedReason(), S_OK, "valid-device-preserved");
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_VALID_PASS old=12345678 new=abcdef09");
}
void commonHandoffRerecord() {
  auto dev = device();
  auto direct = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto copy = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  auto source = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 256);
  auto resource = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto result = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256);
  void *mapped = nullptr;
  check(source->Map(0, nullptr, &mapped), "common-source-map");
  *static_cast<UINT *>(mapped) = 0x51ab72cd;
  source->Unmap(0, nullptr);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  command->CopyBufferRegion(resource.Get(), 0, source.Get(), 0, 4);
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "common-first-close");
  ID3D12CommandList *submitted[] = {command.Get()};
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "common-fence");
  direct->ExecuteCommandLists(1, submitted);
  check(direct->Signal(done.Get(), 1), "common-first-signal");
  wait(done.Get(), 1);
  check(a->Reset(), "common-allocator-reset");
  check(command->Reset(a.Get(), nullptr), "common-same-list-reset");
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command->ResourceBarrier(1, &barrier);
  command->CopyBufferRegion(result.Get(), 0, resource.Get(), 0, 4);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "common-second-close");
  direct->ExecuteCommandLists(1, submitted);
  check(direct->Signal(done.Get(), 2), "common-second-signal");
  auto copyAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto copyCommand = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY, copyAllocator.Get());
  copyCommand->CopyBufferRegion(result.Get(), 4, resource.Get(), 0, 4);
  check(copyCommand->Close(), "common-copy-close");
  check(copy->Wait(done.Get(), 2), "common-copy-wait");
  ID3D12CommandList *copySubmitted[] = {copyCommand.Get()};
  copy->ExecuteCommandLists(1, copySubmitted);
  check(copy->Signal(done.Get(), 3), "common-copy-signal");
  wait(done.Get(), 3);
  expect(dev->GetDeviceRemovedReason(), S_OK, "common-device-healthy");
  check(result->Map(0, nullptr, &mapped), "common-result-map");
  auto *words = static_cast<const UINT *>(mapped);
  if (words[0] != 0x51ab72cd || words[1] != 0x51ab72cd)
    fail("common-handoff-readback");
  result->Unmap(0, nullptr);
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_COMMON_PASS rerecord=51ab72cd copy_queue=51ab72cd healthy=1");
}
void listTypes() {
  auto dev = device();
  for (auto type : {D3D12_COMMAND_LIST_TYPE_COPY, D3D12_COMMAND_LIST_TYPE_COMPUTE}) {
    auto a = allocator(dev.Get(), type);
    auto draw = list(dev.Get(), type, a.Get());
    draw->DrawInstanced(3, 1, 0, 0);
    expect(draw->Close(), E_FAIL, "draw-list-type");
    auto b = allocator(dev.Get(), type);
    auto indexed = list(dev.Get(), type, b.Get());
    indexed->DrawIndexedInstanced(3, 1, 0, 0, 0);
    expect(indexed->Close(), E_FAIL, "indexed-list-type");
    const FLOAT color[] = {1, 0, 0, 1};
    D3D12_CPU_DESCRIPTOR_HANDLE unused{};
    auto c = allocator(dev.Get(), type);
    auto rtv = list(dev.Get(), type, c.Get());
    rtv->ClearRenderTargetView(unused, color, 0, nullptr);
    expect(rtv->Close(), E_INVALIDARG, "rtv-clear-list-type");
    auto d = allocator(dev.Get(), type);
    auto dsv = list(dev.Get(), type, d.Get());
    dsv->ClearDepthStencilView(unused, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    expect(dsv->Close(), E_INVALIDARG, "dsv-clear-list-type");
    auto e = allocator(dev.Get(), type);
    auto targets = list(dev.Get(), type, e.Get());
    targets->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    expect(targets->Close(), E_INVALIDARG, "rtv-binding-list-type");
  }
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto dispatch = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY, a.Get());
  dispatch->Dispatch(1, 1, 1);
  expect(dispatch->Close(), E_FAIL, "dispatch-list-type");
  auto b = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto uav = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY, b.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE unusedCpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE unusedGpu{};
  const UINT values[] = {1, 0, 0, 0};
  uav->ClearUnorderedAccessViewUint(unusedGpu, unusedCpu, nullptr, values, 0, nullptr);
  expect(uav->Close(), E_INVALIDARG, "uav-clear-list-type");
  auto direct = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto overflow = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, direct.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE excessTargets[9]{};
  overflow->OMSetRenderTargets(9, excessTargets, FALSE, nullptr);
  expect(overflow->Close(), E_INVALIDARG, "rtv-binding-count");
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_TYPE_PASS draw_dispatch_clear=reject");
}
void barrierOwnership() {
  auto dev = device();
  auto foreignDev = device();
  auto local = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto foreign = buffer(foreignDev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = local.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "local-barrier-close");
  for (int kind = 0; kind < 3; ++kind) {
    auto invalidAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto invalid = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, invalidAllocator.Get());
    if (kind == 0) {
      barrier.Transition.pResource = foreign.Get();
    } else if (kind == 1) {
      barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      barrier.UAV.pResource = foreign.Get();
    } else {
      barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      barrier.Aliasing.pResourceBefore = local.Get();
      barrier.Aliasing.pResourceAfter = foreign.Get();
    }
    invalid->ResourceBarrier(1, &barrier);
    expect(invalid->Close(), E_INVALIDARG, "foreign-barrier-close");
  }
  for (auto heapType : {D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK}) {
    auto fixed = buffer(dev.Get(), heapType, 256);
    auto invalidAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto invalid = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, invalidAllocator.Get());
    D3D12_RESOURCE_BARRIER fixedBarrier{};
    fixedBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    fixedBarrier.Transition.pResource = fixed.Get();
    fixedBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    fixedBarrier.Transition.StateBefore = heapType == D3D12_HEAP_TYPE_UPLOAD
        ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    fixedBarrier.Transition.StateAfter = heapType == D3D12_HEAP_TYPE_UPLOAD
        ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
    invalid->ResourceBarrier(1, &fixedBarrier);
    expect(invalid->Close(), E_INVALIDARG, "fixed-heap-transition");
  }
  for (int kind = 0; kind < 3; ++kind) {
    auto invalidAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto invalid = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, invalidAllocator.Get());
    if (kind == 0) {
      invalid->CopyBufferRegion(local.Get(), 0, foreign.Get(), 0, 4);
    } else if (kind == 1) {
      invalid->CopyResource(local.Get(), foreign.Get());
    } else {
      auto localTexture = texture(dev.Get());
      auto foreignTexture = texture(foreignDev.Get());
      D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
      destination.pResource = localTexture.Get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source.pResource = foreignTexture.Get();
      source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      invalid->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    expect(invalid->Close(), E_INVALIDARG, "foreign-copy-close");
  }
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_BARRIER_PASS local=accepted foreign_barrier_copy=rejected fixed_heaps=rejected");
}
void barrierSequence(bool invalid, bool separateLists) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto firstAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto first = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, firstAllocator.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  first->ResourceBarrier(1, &barrier);
  ComPtr<ID3D12GraphicsCommandList> second;
  ComPtr<ID3D12CommandAllocator> secondAllocator;
  if (separateLists) {
    check(first->Close(), "barrier-first-close");
    secondAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    second = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, secondAllocator.Get());
  }
  if (!invalid) {
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  auto *target = separateLists ? second.Get() : first.Get();
  target->ResourceBarrier(1, &barrier);
  check(target->Close(), "barrier-final-close");
  ID3D12CommandList *batch[] = {first.Get(), second.Get()};
  q->ExecuteCommandLists(separateLists ? 2 : 1, batch);
  if (invalid) {
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "barrier-sequence-removal");
  } else {
    ComPtr<ID3D12Fence> done;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "barrier-valid-fence");
    check(q->Signal(done.Get(), 1), "barrier-valid-signal");
    wait(done.Get(), 1);
    expect(dev->GetDeviceRemovedReason(), S_OK, "barrier-valid-device");
  }
}
void splitBarrier(bool invalid) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command->ResourceBarrier(1, &barrier);
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
  if (invalid)
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "split-barrier-close");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  if (invalid) {
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "split-barrier-removal");
  } else {
    ComPtr<ID3D12Fence> done;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "split-barrier-fence");
    check(q->Signal(done.Get(), 1), "split-barrier-signal");
    wait(done.Get(), 1);
    expect(dev->GetDeviceRemovedReason(), S_OK, "split-barrier-valid");
  }
}
void barrierPromotion() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  command->ResourceBarrier(1, &barrier);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  command->ResourceBarrier(1, &barrier);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  command->ResourceBarrier(1, &barrier);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "barrier-promotion-close");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "barrier-promotion-fence");
  check(q->Signal(done.Get(), 1), "barrier-promotion-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "barrier-promotion-valid");
}
void textureReadPromotion() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = texture(dev.Get());
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  command->ResourceBarrier(1, &barrier);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "texture-read-promotion-close");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "texture-read-promotion-fence");
  check(q->Signal(done.Get(), 1), "texture-read-promotion-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "texture-read-promotion-valid");
}
void barrierAllOverride() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command->ResourceBarrier(1, &barrier);
  barrier.Transition.Subresource = 0;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  command->ResourceBarrier(1, &barrier);
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "barrier-all-override-close");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "barrier-all-override-fence");
  check(q->Signal(done.Get(), 1), "barrier-all-override-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "barrier-all-override-valid");
}
void barrierMultiMipOverride(bool incomplete) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = texture(dev.Get(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, 2);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command->ResourceBarrier(1, &barrier);
  for (UINT i = 0; i < (incomplete ? 1U : 2U); ++i) {
    barrier.Transition.Subresource = i;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command->ResourceBarrier(1, &barrier);
  }
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command->ResourceBarrier(1, &barrier);
  check(command->Close(), "barrier-multimip-close");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  if (incomplete) {
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "barrier-multimip-removal");
  } else {
    ComPtr<ID3D12Fence> done;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "barrier-multimip-fence");
    check(q->Signal(done.Get(), 1), "barrier-multimip-signal");
    wait(done.Get(), 1);
    expect(dev->GetDeviceRemovedReason(), S_OK, "barrier-multimip-valid");
  }
}
void barrierCrossCall(bool invalid, bool crossQueue) {
  auto dev = device();
  auto producer = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto consumer = crossQueue ? queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) : producer;
  auto resource = texture(dev.Get());
  auto firstAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto secondAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto first = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, firstAllocator.Get());
  auto second = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, secondAllocator.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  first->ResourceBarrier(1, &barrier);
  check(first->Close(), "barrier-cross-call-first-close");
  barrier.Transition.StateBefore = invalid ? D3D12_RESOURCE_STATE_COPY_DEST :
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  second->ResourceBarrier(1, &barrier);
  check(second->Close(), "barrier-cross-call-second-close");
  ComPtr<ID3D12Fence> gate, done, removed;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "barrier-cross-call-gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "barrier-cross-call-done");
  HANDLE event = nullptr;
  if (invalid) {
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed)), "barrier-cross-call-removed");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) fail("barrier-cross-call-event");
    check(removed->SetEventOnCompletion(1, event), "barrier-cross-call-register-removal");
  }
  ID3D12CommandList *one[] = {first.Get()}, *two[] = {second.Get()};
  if (crossQueue) {
    check(consumer->Wait(gate.Get(), 1), "barrier-cross-call-future-wait");
    consumer->ExecuteCommandLists(1, two);
    expect(dev->GetDeviceRemovedReason(), S_OK, "barrier-cross-call-no-premature-removal");
  }
  producer->ExecuteCommandLists(1, one);
  if (crossQueue) {
    check(producer->Signal(gate.Get(), 1), "barrier-cross-call-producer-signal");
  } else {
    consumer->ExecuteCommandLists(1, two);
  }
  if (invalid) {
    if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
      fail("barrier-cross-call-removal-wait");
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "barrier-cross-call-removal");
  } else {
    check(consumer->Signal(done.Get(), 1), "barrier-cross-call-consumer-signal");
    wait(done.Get(), 1);
    expect(dev->GetDeviceRemovedReason(), S_OK, "barrier-cross-call-valid");
  }
  if (event) CloseHandle(event);
}
void implicitCopyPromotion(bool invalid) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto source = texture(dev.Get(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
  auto destination = texture(dev.Get(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto b = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto first = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  auto second = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, b.Get());
  first->CopyResource(destination.Get(), source.Get());
  check(first->Close(), "implicit-copy-close");
  ID3D12CommandList *one[] = {first.Get()};
  q->ExecuteCommandLists(1, one);
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "implicit-copy-fence");
  check(q->Signal(done.Get(), 1), "implicit-copy-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "implicit-copy-executed");

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = destination.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = invalid ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE :
                                              D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  second->ResourceBarrier(1, &barrier);
  if (!invalid) {
    // The source was implicitly promoted to read-only COPY_SOURCE and must
    // decay to COMMON on DIRECT, unlike the write-promoted destination.
    barrier.Transition.pResource = source.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    second->ResourceBarrier(1, &barrier);
  }
  check(second->Close(), "implicit-copy-barrier-close");
  HANDLE removal = nullptr;
  ComPtr<ID3D12Fence> removed;
  if (invalid) {
    removal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!removal) fail("implicit-copy-removal-event");
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed)), "implicit-copy-removal-fence");
    check(removed->SetEventOnCompletion(1, removal), "implicit-copy-removal-registration");
  }
  ID3D12CommandList *two[] = {second.Get()};
  q->ExecuteCommandLists(1, two);
  if (invalid) {
    if (WaitForSingleObject(removal, 15000) != WAIT_OBJECT_0) fail("implicit-copy-removal-wait");
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "implicit-copy-before-mismatch");
  } else {
    check(q->Signal(done.Get(), 2), "implicit-copy-barrier-signal");
    wait(done.Get(), 2);
    expect(dev->GetDeviceRemovedReason(), S_OK, "implicit-copy-before-valid");
  }
  if (removal) CloseHandle(removal);
}
void copyQueueInitialStateMismatch() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto source = texture(dev.Get(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
  auto destination = texture(dev.Get(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY, a.Get());
  command->CopyResource(destination.Get(), source.Get());
  check(command->Close(), "copy-queue-class-close");
  ComPtr<ID3D12Fence> removed;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed)), "copy-queue-class-fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) fail("copy-queue-class-event");
  check(removed->SetEventOnCompletion(1, event), "copy-queue-class-register");
  ID3D12CommandList *submitted[] = {command.Get()};
  q->ExecuteCommandLists(1, submitted);
  if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0) fail("copy-queue-class-removal-wait");
  expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "copy-queue-class-mismatch");
  CloseHandle(event);
}
void bufferDecay() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  auto readback = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto b = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto first = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  auto second = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, b.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  first->ResourceBarrier(1, &barrier);
  first->CopyBufferRegion(readback.Get(), 0, resource.Get(), 0, 4);
  second->ResourceBarrier(1, &barrier); // Prior Execute decays a buffer to COMMON.
  check(first->Close(), "buffer-decay-first-close");
  check(second->Close(), "buffer-decay-second-close");
  ID3D12CommandList *one[] = {first.Get()}, *two[] = {second.Get()};
  q->ExecuteCommandLists(1, one);
  q->ExecuteCommandLists(1, two);
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "buffer-decay-fence");
  check(q->Signal(done.Get(), 1), "buffer-decay-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "buffer-decay-valid");
}
void splitCrossCall(bool invalid) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto resource = texture(dev.Get());
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto b = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto first = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  auto second = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, b.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  first->ResourceBarrier(1, &barrier);
  check(first->Close(), "split-cross-call-first-close");
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
  if (invalid)
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  second->ResourceBarrier(1, &barrier);
  check(second->Close(), "split-cross-call-second-close");
  ComPtr<ID3D12Fence> done, removed;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "split-cross-call-done");
  HANDLE event = nullptr;
  if (invalid) {
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed)), "split-cross-call-removed");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) fail("split-cross-call-event");
    check(removed->SetEventOnCompletion(1, event), "split-cross-call-register-removal");
  }
  ID3D12CommandList *one[] = {first.Get()}, *two[] = {second.Get()};
  q->ExecuteCommandLists(1, one);
  q->ExecuteCommandLists(1, two);
  if (invalid) {
    if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
      fail("split-cross-call-removal-wait");
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "split-cross-call-removal");
    CloseHandle(event);
  } else {
    check(q->Signal(done.Get(), 1), "split-cross-call-signal");
    wait(done.Get(), 1);
    expect(dev->GetDeviceRemovedReason(), S_OK, "split-cross-call-valid");
  }
}
void textureDecay(bool copyQueue) {
  auto dev = device();
  const auto type = copyQueue ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
  auto q = queue(dev.Get(), type);
  auto resource = texture(dev.Get(), copyQueue ? D3D12_RESOURCE_FLAG_NONE :
                                               D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
                          D3D12_RESOURCE_STATE_COMMON);
  auto source = texture(dev.Get(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
  auto a = allocator(dev.Get(), type);
  auto b = allocator(dev.Get(), type);
  auto first = list(dev.Get(), type, a.Get());
  auto second = list(dev.Get(), type, b.Get());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  first->ResourceBarrier(1, &barrier);
  first->CopyResource(resource.Get(), source.Get());
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  second->ResourceBarrier(1, &barrier);
  check(first->Close(), "texture-decay-first-close");
  check(second->Close(), "texture-decay-second-close");
  ID3D12CommandList *one[] = {first.Get()}, *two[] = {second.Get()};
  q->ExecuteCommandLists(1, one);
  q->ExecuteCommandLists(1, two);
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "texture-decay-fence");
  check(q->Signal(done.Get(), 1), "texture-decay-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "texture-decay-valid");
}
void queryCrossList(bool crossQueue) {
  auto dev = device();
  auto producer = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto consumer = crossQueue ? queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) : producer;
  auto producerAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto consumerAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto end = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, producerAllocator.Get());
  auto resolve = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, consumerAllocator.Get());
  D3D12_QUERY_HEAP_DESC queryDesc{};
  queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  queryDesc.Count = queryDesc.NodeMask = 1;
  ComPtr<ID3D12QueryHeap> heap;
  check(dev->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&heap)), "query-cross-list-heap");
  auto result = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256);
  end->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  check(end->Close(), "query-cross-list-end-close");
  resolve->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, result.Get(), 0);
  check(resolve->Close(), "query-cross-list-resolve-close");
  ComPtr<ID3D12Fence> gate, done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "query-cross-list-gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "query-cross-list-done");
  ID3D12CommandList *ending[] = {end.Get()}, *resolving[] = {resolve.Get()};
  if (crossQueue) {
    check(consumer->Wait(gate.Get(), 1), "query-cross-list-future-wait");
    consumer->ExecuteCommandLists(1, resolving);
    check(consumer->Signal(done.Get(), 1), "query-cross-list-consumer-signal");
  }
  producer->ExecuteCommandLists(1, ending);
  if (crossQueue) {
    check(producer->Signal(gate.Get(), 1), "query-cross-list-producer-signal");
  } else {
    producer->ExecuteCommandLists(1, resolving);
    check(producer->Signal(done.Get(), 1), "query-cross-list-signal");
  }
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "query-cross-list-device");
  void *mapped = nullptr;
  check(result->Map(0, nullptr, &mapped), "query-cross-list-map");
  const auto timestamp = *static_cast<const UINT64 *>(mapped);
  result->Unmap(0, nullptr);
  if (!timestamp) fail("query-cross-list-zero-timestamp");
}
void staleBatch() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto goodAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto staleAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto good = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, goodAllocator.Get());
  auto stale = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, staleAllocator.Get());
  auto resident = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256);
  ComPtr<ID3D12Device3> dev3;
  check(dev.As(&dev3), "removal-device3");
  ComPtr<ID3D12Fence> fence;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "removal-fence");
  check(good->Close(), "good-close");
  check(stale->Close(), "stale-close");
  check(staleAllocator->Reset(), "stale-allocator-reset");
  ID3D12CommandList *batch[] = {good.Get(), stale.Get()};
  q->ExecuteCommandLists(2, batch);
  expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "invalid-batch-removal");
  // A removed device must reject later queue publication, not signal completion.
  expect(q->Signal(fence.Get(), 1), DXGI_ERROR_DEVICE_REMOVED, "post-removal-signal");
  ID3D12Pageable *pageable[] = {resident.Get()};
  expect(dev->MakeResident(1, pageable), DXGI_ERROR_DEVICE_REMOVED, "post-removal-resident");
  expect(dev->Evict(1, pageable), DXGI_ERROR_DEVICE_REMOVED, "post-removal-evict");
  expect(dev3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, 1, pageable, fence.Get(), 2),
         DXGI_ERROR_DEVICE_REMOVED, "post-removal-enqueue-resident");
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_STALE_PASS dropped=2 removed=1 residency=reject");
}
void staleBundleAllocator() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto bundleAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_BUNDLE);
  auto bundle = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAllocator.Get());
  check(bundle->Close(), "stale-bundle-close");
  auto parentAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto parent = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, parentAllocator.Get());
  parent->ExecuteBundle(bundle.Get());
  check(parent->Close(), "stale-bundle-parent-close");
  check(bundleAllocator->Reset(), "stale-bundle-allocator-reset");
  ID3D12CommandList *submitted[] = {parent.Get()};
  q->ExecuteCommandLists(1, submitted);
  expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "stale-bundle-device-removal");
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_BUNDLE_STALE_PASS removed=1");
}
struct ReleaseState {
  ID3D12CommandQueue *queue;
  HANDLE started;
  HANDLE done;
};
DWORD WINAPI releaseQueue(void *context) {
  auto *state = static_cast<ReleaseState *>(context);
  SetEvent(state->started);
  state->queue->Release();
  SetEvent(state->done);
  return 0;
}
void shutdownWait() {
  auto dev = device();
  auto doomed = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto peer = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  ComPtr<ID3D12Fence> gate, before, after, peerDone;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "shutdown-gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&before)), "shutdown-before");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&after)), "shutdown-after");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&peerDone)), "shutdown-peer-done");
  check(doomed->Signal(before.Get(), 1), "shutdown-before-signal");
  check(doomed->Wait(gate.Get(), 1), "shutdown-doomed-wait");
  check(doomed->Signal(after.Get(), 1), "shutdown-postwait-signal");
  check(peer->Wait(gate.Get(), 1), "shutdown-peer-wait");
  check(peer->Signal(peerDone.Get(), 1), "shutdown-peer-signal");

  HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!started || !done) fail("shutdown-release-events");
  ReleaseState state{doomed.Detach(), started, done};
  HANDLE thread = CreateThread(nullptr, 0, releaseQueue, &state, 0, nullptr);
  if (!thread) fail("shutdown-release-thread");
  const DWORD start = WaitForSingleObject(started, 5000);
  const DWORD released = WaitForSingleObject(done, 5000);
  if (start != WAIT_OBJECT_0 || released != WAIT_OBJECT_0) {
    // An external CPU signal guarantees cleanup on a broken implementation.
    gate->Signal(1);
    WaitForSingleObject(done, 15000);
    fail("shutdown-stuck-on-foreign-fence");
  }
  CloseHandle(thread);
  CloseHandle(started);
  CloseHandle(done);
  expect(before->GetCompletedValue(), 1, "shutdown-prior-work-retired");
  expect(after->GetCompletedValue(), 0, "shutdown-postwait-unpublished");
  expect(peerDone->GetCompletedValue(), 0, "shutdown-peer-still-blocked");
  expect(dev->GetDeviceRemovedReason(), S_OK, "shutdown-device-healthy");
  check(gate->Signal(1), "shutdown-release-peer");
  wait(peerDone.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "shutdown-peer-healthy");
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_SHUTDOWN_PASS local_cancel=1 peer_preserved=1");
}
void completedRecordingReplay() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  auto input = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 256);
  auto output = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256);
  void *mapped = nullptr;
  check(input->Map(0, nullptr, &mapped), "completed-replay-map-input");
  auto *words = static_cast<std::uint32_t *>(mapped);
  words[0] = 0x7293ab1c;
  words[1] = 0x192ab713;
  input->Unmap(0, nullptr);
  check(output->Map(0, nullptr, &mapped), "completed-replay-map-output-before");
  words = static_cast<std::uint32_t *>(mapped);
  words[1] = 0xfeedbeef;
  output->Unmap(0, nullptr);
  command->CopyBufferRegion(output.Get(), 0, input.Get(), 0, 4);
  check(command->Close(), "completed-replay-close");
  command->CopyBufferRegion(output.Get(), 4, input.Get(), 4, 4); // A closed list cannot gain another command.
  ComPtr<ID3D12Fence> done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "completed-replay-fence");
  ID3D12CommandList *submitted[] = {command.Get()};
  for (UINT64 value = 1; value <= 2; ++value) {
    q->ExecuteCommandLists(1, submitted); // Same closed recording, after its prior GPU completion.
    check(q->Signal(done.Get(), value), "completed-replay-signal");
    wait(done.Get(), value);
    expect(dev->GetDeviceRemovedReason(), S_OK, "completed-replay-device");
  }
  check(output->Map(0, nullptr, &mapped), "completed-replay-map-output");
  words = static_cast<std::uint32_t *>(mapped);
  if (words[0] != 0x7293ab1c || words[1] != 0xfeedbeef)
    fail("completed-replay-output");
  output->Unmap(0, nullptr);
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_REPLAY_COMPLETED_PASS epochs=1 executions=2 late_command=ignored");
}
void invalidRecordingReplay(bool sameArray, bool secondQueue) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto peer = secondQueue ? queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) : nullptr;
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  check(command->Close(), "duplicate-close");
  ComPtr<ID3D12Fence> gate, removedEventFence;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "duplicate-gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removedEventFence)), "duplicate-event-fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) fail("duplicate-event");
  check(removedEventFence->SetEventOnCompletion(1, event), "duplicate-register-event");
  check(q->Wait(gate.Get(), 1), "duplicate-queue-wait");
  ID3D12CommandList *one[] = {command.Get()};
  ComPtr<ID3D12Resource> untouched;
  void *mappedUntouched = nullptr;
  if (sameArray) {
    ID3D12CommandList *twice[] = {command.Get(), command.Get()};
    q->ExecuteCommandLists(2, twice);
  } else {
    q->ExecuteCommandLists(1, one);
    if (secondQueue)
      peer->ExecuteCommandLists(1, one);
    else {
      // A valid first list in the rejected batch must not be submitted.
      auto goodAllocator = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
      auto good = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, goodAllocator.Get());
      auto upload = buffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 256);
      untouched = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256);
      void *mappedUpload = nullptr;
      check(upload->Map(0, nullptr, &mappedUpload), "duplicate-upload-map");
      *static_cast<std::uint32_t *>(mappedUpload) = 0x3278a64f;
      upload->Unmap(0, nullptr);
      check(untouched->Map(0, nullptr, &mappedUntouched), "duplicate-readback-map");
      *static_cast<std::uint32_t *>(mappedUntouched) = 0xfeedbeef;
      good->CopyBufferRegion(untouched.Get(), 0, upload.Get(), 0, 4);
      check(good->Close(), "duplicate-good-close");
      ID3D12CommandList *batch[] = {good.Get(), command.Get()};
      q->ExecuteCommandLists(2, batch);
    }
  }
  expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "duplicate-device-removal");
  if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
    fail("duplicate-peer-event-wake");
  CloseHandle(event);
  expect(q->Signal(removedEventFence.Get(), 2), DXGI_ERROR_DEVICE_REMOVED, "duplicate-no-false-signal");
  q.Reset(); // Ensure no submitted GPU copy can arrive after the observation.
  if (untouched) {
    if (*static_cast<std::uint32_t *>(mappedUntouched) != 0xfeedbeef)
      fail("duplicate-whole-batch-dropped");
    untouched->Unmap(0, nullptr);
  }
}
struct ConcurrentReplay {
  ID3D12CommandQueue *queue;
  ID3D12CommandList *command;
  HANDLE start;
};
DWORD WINAPI submitConcurrentReplay(void *context) {
  auto *replay = static_cast<ConcurrentReplay *>(context);
  WaitForSingleObject(replay->start, INFINITE);
  ID3D12CommandList *one[] = {replay->command};
  replay->queue->ExecuteCommandLists(1, one);
  return 0;
}
void invalidConcurrentReplay() {
  auto dev = device();
  auto first = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto second = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto a = allocator(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto command = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get());
  check(command->Close(), "concurrent-replay-close");
  ComPtr<ID3D12Fence> gate, removed;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "concurrent-replay-gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed)), "concurrent-replay-removed");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event || !start) fail("concurrent-replay-events");
  check(removed->SetEventOnCompletion(1, event), "concurrent-replay-removal-event");
  check(first->Wait(gate.Get(), 1), "concurrent-replay-first-wait");
  check(second->Wait(gate.Get(), 1), "concurrent-replay-second-wait");
  ConcurrentReplay jobs[] = {{first.Get(), command.Get(), start}, {second.Get(), command.Get(), start}};
  HANDLE threads[] = {CreateThread(nullptr, 0, submitConcurrentReplay, &jobs[0], 0, nullptr),
                      CreateThread(nullptr, 0, submitConcurrentReplay, &jobs[1], 0, nullptr)};
  if (!threads[0] || !threads[1]) fail("concurrent-replay-threads");
  SetEvent(start);
  if (WaitForMultipleObjects(2, threads, TRUE, 5000) != WAIT_OBJECT_0)
    fail("concurrent-replay-submit-wait");
  CloseHandle(threads[0]);
  CloseHandle(threads[1]);
  CloseHandle(start);
  expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "concurrent-replay-device-removal");
  if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
    fail("concurrent-replay-event-wake");
  CloseHandle(event);
  first.Reset();
  second.Reset();
}
} // namespace

int main() {
  auto module = LoadLibraryW(L"d3d12.dll");
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) fail("owned-backend");
  std::printf("OWNED_D3D12_EXECUTION_SAFETY_BACKEND id=%s\n", backend());
  validReplay();
  commonHandoffRerecord();
  listTypes();
  barrierOwnership();
  barrierSequence(false, false);
  barrierSequence(false, true);
  barrierSequence(true, false);
  barrierSequence(true, true);
  splitBarrier(false);
  splitBarrier(true);
  barrierPromotion();
  textureReadPromotion();
  barrierAllOverride();
  barrierMultiMipOverride(false);
  barrierMultiMipOverride(true);
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_BARRIER_SEQUENCE_PASS valid=7 mismatched=4");
  barrierCrossCall(false, false);
  barrierCrossCall(true, false);
  barrierCrossCall(false, true);
  barrierCrossCall(true, true);
  implicitCopyPromotion(false);
  implicitCopyPromotion(true);
  copyQueueInitialStateMismatch();
  bufferDecay();
  splitCrossCall(false);
  splitCrossCall(true);
  textureDecay(false);
  textureDecay(true);
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_BARRIER_CROSS_CALL_PASS same_queue=valid+removed future_wait=valid+removed implicit_copy=valid+removed copy_queue_class=removed buffer_decay=valid sim_texture_decay=valid copy_queue_decay=valid split=valid+removed");
  queryCrossList(false);
  queryCrossList(true);
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_QUERY_CROSS_LIST_PASS same_queue=1 future_wait_cross_queue=1");
  staleBatch();
  staleBundleAllocator();
  shutdownWait();
  completedRecordingReplay();
  invalidRecordingReplay(false, false);
  invalidRecordingReplay(true, false);
  invalidRecordingReplay(false, true);
  invalidConcurrentReplay();
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_REPLAY_INVALID_PASS batch=removed same_array=removed two_queues=removed concurrent_queues=removed events=woke");
  std::puts("OWNED_D3D12_EXECUTION_SAFETY_PASS");
}
