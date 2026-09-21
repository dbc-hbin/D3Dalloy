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
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kTimeoutMs = 15000;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_PREDICATION_INDIRECT_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void require(bool condition, const char *step) {
  if (!condition)
    fail(step);
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using BackendId = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backendId != nullptr, "owned-backend-id-export");
  const char *id = backendId();
  require(id && std::strcmp(id, "owned-graphics-d3d12") == 0, "owned-backend-id");
  std::printf("OWNED_D3D12_PREDICATION_INDIRECT_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties = {};
  properties.Type = type;
  properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  properties.CreationNodeMask = 1;
  properties.VisibleNodeMask = 1;
  return properties;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

struct TestContext {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;

  ~TestContext() {
    if (event)
      CloseHandle(event);
  }

  void submit(ID3D12GraphicsCommandList *list) {
    check(list->Close(), "close-list");
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "set-event");
      require(WaitForSingleObject(event, kTimeoutMs) == WAIT_OBJECT_0, "wait");
    }
  }
};

void createContext(TestContext &context) {
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; index++) {
    DXGI_ADAPTER_DESC1 desc = {};
    check(adapter->GetDesc1(&desc), "adapter-desc");
    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&context.device))))
      break;
    adapter.Reset();
  }
  if (!context.device)
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&context.device)), "device");

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(context.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context.queue)), "queue");
  check(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context.fence)), "fence");
  context.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(context.event != nullptr, "event");
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  ComPtr<ID3D12Resource> resource;
  const auto heap = heapProperties(heapType);
  const auto desc = bufferDesc(bytes, flags);
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)), "create-buffer");
  return resource;
}

void copyTextureToReadback(ID3D12GraphicsCommandList *list, ID3D12Resource *source, UINT subresource,
                           ID3D12Resource *destination, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = subresource;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = destination;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

void RunPredicatedClearTests(TestContext &context) {
  constexpr UINT width = 4;
  constexpr UINT height = 2;

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "clear-allocator");
  check(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)), "clear-list");

  D3D12_RESOURCE_DESC colorDesc = {};
  colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  colorDesc.Width = width;
  colorDesc.Height = height;
  colorDesc.DepthOrArraySize = 1;
  colorDesc.MipLevels = 1;
  colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  colorDesc.SampleDesc.Count = 1;
  colorDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE colorClear = {};
  colorClear.Format = colorDesc.Format;
  colorClear.Color[2] = 1.0f;
  colorClear.Color[3] = 1.0f;
  ComPtr<ID3D12Resource> color;
  auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  check(context.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
                                                 IID_PPV_ARGS(&color)), "color");

  D3D12_RESOURCE_DESC depthDesc = colorDesc;
  depthDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  depthClear.DepthStencil.Depth = 0.25f;
  depthClear.DepthStencil.Stencil = 0x33;
  ComPtr<ID3D12Resource> depth;
  check(context.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                 D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                 IID_PPV_ARGS(&depth)), "depth");

  auto predicateZero = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateOne = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateUpload = createBuffer(context.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  uint64_t *predicateValues = nullptr;
  check(predicateUpload->Map(0, nullptr, reinterpret_cast<void **>(&predicateValues)), "predicate-map");
  predicateValues[0] = 0;
  predicateValues[1] = 1;
  predicateUpload->Unmap(0, nullptr);

  auto uav = createBuffer(context.device.Get(), 16, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(context.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "rtv-heap");
  context.device->CreateRenderTargetView(color.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(context.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "dsv-heap");
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  context.device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
  uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  uavHeapDesc.NumDescriptors = 1;
  uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> uavHeap;
  check(context.device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&uavHeap)), "uav-gpu-heap");
  uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> uavCpuHeap;
  check(context.device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&uavCpuHeap)), "uav-cpu-heap");
  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uavDesc.Buffer.NumElements = 4;
  uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  const auto uavCpu = uavCpuHeap->GetCPUDescriptorHandleForHeapStart();
  const auto uavGpuCpu = uavHeap->GetCPUDescriptorHandleForHeapStart();
  const auto uavGpu = uavHeap->GetGPUDescriptorHandleForHeapStart();
  context.device->CreateUnorderedAccessView(uav.Get(), nullptr, &uavDesc, uavCpu);
  context.device->CreateUnorderedAccessView(uav.Get(), nullptr, &uavDesc, uavGpuCpu);
  ID3D12DescriptorHeap *descriptorHeaps[] = {uavHeap.Get()};
  list->SetDescriptorHeaps(1, descriptorHeaps);

  const float blue[] = {0.0f, 0.0f, 1.0f, 1.0f};
  list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), blue, 0, nullptr);
  list->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(),
                              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25f, 0x33, 0, nullptr);
  D3D12_RESOURCE_BARRIER uavOrdering = {};
  uavOrdering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavOrdering.UAV.pResource = uav.Get();
  const UINT initialUav[] = {7, 7, 7, 7};
  list->ClearUnorderedAccessViewUint(uavGpu, uavCpu, uav.Get(), initialUav, 0, nullptr);
  list->ResourceBarrier(1, &uavOrdering);

  list->CopyBufferRegion(predicateZero.Get(), 0, predicateUpload.Get(), 0, 8);
  list->CopyBufferRegion(predicateOne.Get(), 0, predicateUpload.Get(), 8, 8);
  std::array<D3D12_RESOURCE_BARRIER, 2> predicateBarriers = {
      transition(predicateZero.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
      transition(predicateOne.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
  };
  list->ResourceBarrier(static_cast<UINT>(predicateBarriers.size()), predicateBarriers.data());
  list->SetPredication(predicateZero.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  const float red[] = {1.0f, 0.0f, 0.0f, 1.0f};
  list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), red, 0, nullptr);
  list->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(),
                              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.9f, 0x99, 0, nullptr);
  const UINT rejectedUav[] = {99, 99, 99, 99};
  list->ClearUnorderedAccessViewUint(uavGpu, uavCpu, uav.Get(), rejectedUav, 0, nullptr);
  list->ResourceBarrier(1, &uavOrdering);

  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  auto predicateBarrier = transition(predicateOne.Get(), D3D12_RESOURCE_STATE_PREDICATION,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
  list->ResourceBarrier(1, &predicateBarrier);
  list->CopyBufferRegion(predicateOne.Get(), 0, predicateUpload.Get(), 0, 8);
  predicateBarrier = transition(predicateOne.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_PREDICATION);
  list->ResourceBarrier(1, &predicateBarrier);

  const D3D12_RECT leftHalf = {0, 0, 2, 2};
  const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
  list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), green, 1, &leftHalf);
  list->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(),
                              D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0xee, 0, nullptr);
  const UINT oneBits[] = {0x3f800000u, 0, 0, 0};
  const D3D12_RECT firstElement = {0, 0, 1, 1};
  list->ClearUnorderedAccessViewUint(uavGpu, uavCpu, uav.Get(), oneBits, 1, &firstElement);
  list->ResourceBarrier(1, &uavOrdering);

  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  const float rejectedAfterResnap[] = {1.0f, 1.0f, 0.0f, 1.0f};
  list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), rejectedAfterResnap, 0, nullptr);

  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  const UINT eleven[] = {11, 0, 0, 0};
  const D3D12_RECT secondElement = {1, 0, 2, 1};
  list->ClearUnorderedAccessViewUint(uavGpu, uavCpu, uav.Get(), eleven, 1, &secondElement);
  list->ResourceBarrier(1, &uavOrdering);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT colorFootprint = {};
  UINT colorRows = 0;
  UINT64 colorRowBytes = 0;
  UINT64 colorBytes = 0;
  context.device->GetCopyableFootprints(&colorDesc, 0, 1, 0, &colorFootprint, &colorRows,
                                        &colorRowBytes, &colorBytes);
  auto colorReadback = createBuffer(context.device.Get(), colorBytes, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_STATE_COPY_DEST);

  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> depthFootprints = {};
  std::array<UINT, 2> depthRows = {};
  std::array<UINT64, 2> depthRowBytes = {};
  UINT64 depthBytes = 0;
  context.device->GetCopyableFootprints(&depthDesc, 0, 2, 0, depthFootprints.data(), depthRows.data(),
                                        depthRowBytes.data(), &depthBytes);
  auto depthReadback = createBuffer(context.device.Get(), depthBytes, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto uavReadback = createBuffer(context.device.Get(), 16, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

  std::array<D3D12_RESOURCE_BARRIER, 3> copyBarriers = {
      transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
  };
  list->ResourceBarrier(static_cast<UINT>(copyBarriers.size()), copyBarriers.data());
  copyTextureToReadback(list.Get(), color.Get(), 0, colorReadback.Get(), colorFootprint);
  copyTextureToReadback(list.Get(), depth.Get(), 0, depthReadback.Get(), depthFootprints[0]);
  copyTextureToReadback(list.Get(), depth.Get(), 1, depthReadback.Get(), depthFootprints[1]);
  list->CopyBufferRegion(uavReadback.Get(), 0, uav.Get(), 0, 16);
  context.submit(list.Get());

  uint8_t *colorData = nullptr;
  check(colorReadback->Map(0, nullptr, reinterpret_cast<void **>(&colorData)), "color-map");
  for (UINT y = 0; y < height; y++) {
    const auto *row = colorData + colorFootprint.Offset + y * colorFootprint.Footprint.RowPitch;
    for (UINT x = 0; x < width; x++) {
      const uint8_t *pixel = row + x * 4;
      const uint8_t expected[4] = {0, static_cast<uint8_t>(x < 2 ? 255 : 0),
                                   static_cast<uint8_t>(x < 2 ? 0 : 255), 255};
      if (std::memcmp(pixel, expected, sizeof(expected)) != 0) {
        std::fprintf(stderr,
                     "OWNED_D3D12_PREDICATION_INDIRECT_RTV_MISMATCH x=%u y=%u expected=%u,%u,%u,%u observed=%u,%u,%u,%u\n",
                     x, y, expected[0], expected[1], expected[2], expected[3],
                     pixel[0], pixel[1], pixel[2], pixel[3]);
        fail(x < 2 ? "rtv-true-rect" : "rtv-false-preserve");
      }
    }
  }
  colorReadback->Unmap(0, nullptr);

  uint8_t *depthData = nullptr;
  check(depthReadback->Map(0, nullptr, reinterpret_cast<void **>(&depthData)), "depth-map");
  float depthValue = 0.0f;
  std::memcpy(&depthValue, depthData + depthFootprints[0].Offset, sizeof(depthValue));
  require(depthValue == 0.75f, "dsv-depth-true");
  require(depthData[depthFootprints[1].Offset] == 0x33, "dsv-stencil-preserved");
  depthReadback->Unmap(0, nullptr);

  uint32_t *uavData = nullptr;
  check(uavReadback->Map(0, nullptr, reinterpret_cast<void **>(&uavData)), "uav-map");
  const uint32_t expectedUav[4] = {0x3f800000u, 11u, 7u, 7u};
  for (UINT index = 0; index < 4; ++index) {
    if (uavData[index] != expectedUav[index]) {
      std::fprintf(stderr,
                   "OWNED_D3D12_PREDICATION_INDIRECT_UAV_MISMATCH index=%u expected=0x%08x observed=0x%08x all=%08x,%08x,%08x,%08x\n",
                   index, expectedUav[index], uavData[index], uavData[0], uavData[1], uavData[2], uavData[3]);
      fail(index == 0 ? "uav-float-true" : index == 1 ? "uav-disabled-unconditional"
                                                     : "uav-uint-false-preserve");
    }
  }
  uavReadback->Unmap(0, nullptr);

  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_PREDICATE_SNAPSHOT_PASS saved=1 source_updated=0 copy_predicated=1 resnap_skip_observed=1");
}


void RunTypedMultilayerClearTests(TestContext &context) {
  constexpr UINT width = 4;
  constexpr UINT height = 2;
  constexpr UINT layers = 2;
  const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "typed-clear-allocator");
  check(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)), "typed-clear-list");

  D3D12_RESOURCE_DESC colorDesc = {};
  colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  colorDesc.Width = width;
  colorDesc.Height = height;
  colorDesc.DepthOrArraySize = layers;
  colorDesc.MipLevels = 1;
  colorDesc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
  colorDesc.SampleDesc.Count = 1;
  colorDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> color;
  check(context.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                 IID_PPV_ARGS(&color)), "typed-color-array");

  D3D12_RESOURCE_DESC depthDesc = colorDesc;
  depthDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  ComPtr<ID3D12Resource> depth;
  check(context.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                 D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                                 IID_PPV_ARGS(&depth)), "typed-depth-array");

  D3D12_RESOURCE_DESC uavDescResource = colorDesc;
  uavDescResource.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  uavDescResource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> uav;
  check(context.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &uavDescResource,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&uav)), "typed-uav-array");

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.NumDescriptors = 2;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(context.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "typed-rtv-heap");
  const UINT rtvStride = context.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto allRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  auto selectedRtv = allRtv;
  selectedRtv.ptr += rtvStride;
  D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
  rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
  rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
  rtvDesc.Texture2DArray.ArraySize = layers;
  context.device->CreateRenderTargetView(color.Get(), &rtvDesc, allRtv);
  rtvDesc.Texture2DArray.FirstArraySlice = 1;
  rtvDesc.Texture2DArray.ArraySize = 1;
  context.device->CreateRenderTargetView(color.Get(), &rtvDesc, selectedRtv);

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.NumDescriptors = 2;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(context.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "typed-dsv-heap");
  const UINT dsvStride = context.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  auto allDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  auto selectedDsv = allDsv;
  selectedDsv.ptr += dsvStride;
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
  dsvDesc.Texture2DArray.ArraySize = layers;
  context.device->CreateDepthStencilView(depth.Get(), &dsvDesc, allDsv);
  dsvDesc.Texture2DArray.FirstArraySlice = 1;
  dsvDesc.Texture2DArray.ArraySize = 1;
  context.device->CreateDepthStencilView(depth.Get(), &dsvDesc, selectedDsv);

  D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
  uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  uavHeapDesc.NumDescriptors = 2;
  uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> uavHeap;
  check(context.device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&uavHeap)), "typed-uav-gpu-heap");
  uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> uavCpuHeap;
  check(context.device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&uavCpuHeap)), "typed-uav-cpu-heap");
  const UINT uavStride = context.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto allUavCpu = uavCpuHeap->GetCPUDescriptorHandleForHeapStart();
  auto selectedUavCpu = allUavCpu;
  selectedUavCpu.ptr += uavStride;
  auto allUavGpuCpu = uavHeap->GetCPUDescriptorHandleForHeapStart();
  auto selectedUavGpuCpu = allUavGpuCpu;
  selectedUavGpuCpu.ptr += uavStride;
  auto allUavGpu = uavHeap->GetGPUDescriptorHandleForHeapStart();
  auto selectedUavGpu = allUavGpu;
  selectedUavGpu.ptr += uavStride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC typedUavDesc = {};
  typedUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  typedUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  typedUavDesc.Texture2DArray.ArraySize = layers;
  context.device->CreateUnorderedAccessView(uav.Get(), nullptr, &typedUavDesc, allUavCpu);
  context.device->CreateUnorderedAccessView(uav.Get(), nullptr, &typedUavDesc, allUavGpuCpu);
  typedUavDesc.Texture2DArray.FirstArraySlice = 1;
  typedUavDesc.Texture2DArray.ArraySize = 1;
  context.device->CreateUnorderedAccessView(uav.Get(), nullptr, &typedUavDesc, selectedUavCpu);
  context.device->CreateUnorderedAccessView(uav.Get(), nullptr, &typedUavDesc, selectedUavGpuCpu);
  ID3D12DescriptorHeap *descriptorHeaps[] = {uavHeap.Get()};
  list->SetDescriptorHeaps(1, descriptorHeaps);

  const float initialColor[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  const UINT initialUav[4] = {7, 8, 9, 10};
  list->ClearRenderTargetView(allRtv, initialColor, 0, nullptr);
  list->ClearDepthStencilView(allDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              0.25f, 0x33, 0, nullptr);
  list->ClearUnorderedAccessViewUint(allUavGpu, allUavCpu, uav.Get(), initialUav, 0, nullptr);
  D3D12_RESOURCE_BARRIER uavOrdering = {};
  uavOrdering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavOrdering.UAV.pResource = uav.Get();
  list->ResourceBarrier(1, &uavOrdering);

  auto predicateZero = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateOne = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateUpload = createBuffer(context.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  uint64_t *predicateValues = nullptr;
  check(predicateUpload->Map(0, nullptr, reinterpret_cast<void **>(&predicateValues)), "typed-predicate-map");
  predicateValues[0] = 0;
  predicateValues[1] = 1;
  predicateUpload->Unmap(0, nullptr);
  list->CopyBufferRegion(predicateZero.Get(), 0, predicateUpload.Get(), 0, 8);
  list->CopyBufferRegion(predicateOne.Get(), 0, predicateUpload.Get(), 8, 8);
  std::array<D3D12_RESOURCE_BARRIER, 2> predicateBarriers = {
      transition(predicateZero.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
      transition(predicateOne.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
  };
  list->ResourceBarrier(static_cast<UINT>(predicateBarriers.size()), predicateBarriers.data());

  const float rejectedColor[4] = {901.0f, 902.0f, 903.0f, 904.0f};
  const UINT rejectedUav[4] = {991, 992, 993, 994};
  list->SetPredication(predicateZero.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearRenderTargetView(selectedRtv, rejectedColor, 0, nullptr);
  list->ClearDepthStencilView(selectedDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              0.9f, 0x99, 0, nullptr);
  list->ClearUnorderedAccessViewUint(selectedUavGpu, selectedUavCpu, uav.Get(), rejectedUav, 0, nullptr);
  list->ResourceBarrier(1, &uavOrdering);

  const D3D12_RECT colorRect = {1, 0, 3, 2};
  const D3D12_RECT uavRect = {0, 1, 2, 2};
  const float selectedColor[4] = {101.0f, 202.0f, 303.0f, 404.0f};
  const UINT selectedUav[4] = {11, 12, 13, 14};
  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearRenderTargetView(selectedRtv, selectedColor, 1, &colorRect);
  list->ClearDepthStencilView(selectedDsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0xee, 1, &colorRect);
  list->ClearUnorderedAccessViewUint(selectedUavGpu, selectedUavCpu, uav.Get(), selectedUav, 1, &uavRect);
  list->ResourceBarrier(1, &uavOrdering);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, layers> colorFootprints = {};
  std::array<UINT, layers> colorRows = {};
  std::array<UINT64, layers> colorRowBytes = {};
  UINT64 colorBytes = 0;
  context.device->GetCopyableFootprints(&colorDesc, 0, layers, 0, colorFootprints.data(), colorRows.data(),
                                        colorRowBytes.data(), &colorBytes);
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, layers * 2> depthFootprints = {};
  std::array<UINT, layers * 2> depthRows = {};
  std::array<UINT64, layers * 2> depthRowBytes = {};
  UINT64 depthBytes = 0;
  context.device->GetCopyableFootprints(&depthDesc, 0, layers * 2, 0, depthFootprints.data(), depthRows.data(),
                                        depthRowBytes.data(), &depthBytes);
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, layers> uavFootprints = {};
  std::array<UINT, layers> uavRows = {};
  std::array<UINT64, layers> uavRowBytes = {};
  UINT64 uavBytes = 0;
  context.device->GetCopyableFootprints(&uavDescResource, 0, layers, 0, uavFootprints.data(), uavRows.data(),
                                        uavRowBytes.data(), &uavBytes);
  auto colorReadback = createBuffer(context.device.Get(), colorBytes, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto depthReadback = createBuffer(context.device.Get(), depthBytes, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto uavReadback = createBuffer(context.device.Get(), uavBytes, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<D3D12_RESOURCE_BARRIER, 3> copyBarriers = {
      transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
  };
  list->ResourceBarrier(static_cast<UINT>(copyBarriers.size()), copyBarriers.data());
  for (UINT layer = 0; layer < layers; ++layer) {
    copyTextureToReadback(list.Get(), color.Get(), layer, colorReadback.Get(), colorFootprints[layer]);
    copyTextureToReadback(list.Get(), uav.Get(), layer, uavReadback.Get(), uavFootprints[layer]);
  }
  for (UINT subresource = 0; subresource < layers * 2; ++subresource)
    copyTextureToReadback(list.Get(), depth.Get(), subresource, depthReadback.Get(), depthFootprints[subresource]);
  context.submit(list.Get());

  uint8_t *colorData = nullptr;
  check(colorReadback->Map(0, nullptr, reinterpret_cast<void **>(&colorData)), "typed-color-readback-map");
  for (UINT layer = 0; layer < layers; ++layer) {
    for (UINT y = 0; y < height; ++y) {
      const auto *row = reinterpret_cast<const uint16_t *>(colorData + colorFootprints[layer].Offset +
                                                           y * colorFootprints[layer].Footprint.RowPitch);
      for (UINT x = 0; x < width; ++x) {
        const bool selected = layer == 1 && x >= 1 && x < 3;
        const uint16_t expected[4] = {static_cast<uint16_t>(selected ? 101 : 1),
                                      static_cast<uint16_t>(selected ? 202 : 2),
                                      static_cast<uint16_t>(selected ? 303 : 3),
                                      static_cast<uint16_t>(selected ? 404 : 4)};
        require(std::memcmp(row + x * 4, expected, sizeof(expected)) == 0, "typed-rtv-array-rect-predicate");
      }
    }
  }
  colorReadback->Unmap(0, nullptr);

  uint8_t *depthData = nullptr;
  check(depthReadback->Map(0, nullptr, reinterpret_cast<void **>(&depthData)), "typed-depth-readback-map");
  for (UINT layer = 0; layer < layers; ++layer) {
    for (UINT y = 0; y < height; ++y) {
      const auto *depthRow = reinterpret_cast<const float *>(depthData + depthFootprints[layer].Offset +
                                                              y * depthFootprints[layer].Footprint.RowPitch);
      const auto *stencilRow = depthData + depthFootprints[layers + layer].Offset +
                               y * depthFootprints[layers + layer].Footprint.RowPitch;
      for (UINT x = 0; x < width; ++x) {
        const float expectedDepth = layer == 1 && x >= 1 && x < 3 ? 0.75f : 0.25f;
        require(depthRow[x] == expectedDepth, "typed-dsv-array-depth-rect-predicate");
        require(stencilRow[x] == 0x33, "typed-dsv-depth-only-preserves-stencil");
      }
    }
  }
  depthReadback->Unmap(0, nullptr);

  uint8_t *uavData = nullptr;
  check(uavReadback->Map(0, nullptr, reinterpret_cast<void **>(&uavData)), "typed-uav-readback-map");
  for (UINT layer = 0; layer < layers; ++layer) {
    for (UINT y = 0; y < height; ++y) {
      const auto *row = reinterpret_cast<const uint32_t *>(uavData + uavFootprints[layer].Offset +
                                                           y * uavFootprints[layer].Footprint.RowPitch);
      for (UINT x = 0; x < width; ++x) {
        const bool selected = layer == 1 && y == 1 && x < 2;
        const uint32_t expected[4] = {selected ? 11u : 7u, selected ? 12u : 8u,
                                      selected ? 13u : 9u, selected ? 14u : 10u};
        require(std::memcmp(row + x * 4, expected, sizeof(expected)) == 0, "typed-uav-array-rect-predicate");
      }
    }
  }
  uavReadback->Unmap(0, nullptr);

  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_TYPED_MULTILAYER_CLEARS_PASS rtv=R16G16B16A16_UINT dsv=D32_FLOAT_S8X24_UINT uav=R32G32B32A32_UINT layers=2 rect_preservation=1 component_preservation=1 predicate_preservation=1");
}


void RunTypedFloatUavClearTest(TestContext &context) {
  constexpr UINT width = 4;
  constexpr UINT height = 2;
  constexpr UINT layers = 2;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = layers;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  check(context.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&texture)), "float-uav-array");

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 2;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> gpuHeap;
  check(context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gpuHeap)), "float-uav-gpu-heap");
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> cpuHeap;
  check(context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cpuHeap)), "float-uav-cpu-heap");
  const UINT stride = context.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto allCpu = cpuHeap->GetCPUDescriptorHandleForHeapStart();
  auto selectedCpu = allCpu;
  selectedCpu.ptr += stride;
  auto allGpuCpu = gpuHeap->GetCPUDescriptorHandleForHeapStart();
  auto selectedGpuCpu = allGpuCpu;
  selectedGpuCpu.ptr += stride;
  auto allGpu = gpuHeap->GetGPUDescriptorHandleForHeapStart();
  auto selectedGpu = allGpu;
  selectedGpu.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  uavDesc.Texture2DArray.ArraySize = layers;
  context.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uavDesc, allCpu);
  context.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uavDesc, allGpuCpu);
  uavDesc.Texture2DArray.FirstArraySlice = 1;
  uavDesc.Texture2DArray.ArraySize = 1;
  context.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uavDesc, selectedCpu);
  context.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uavDesc, selectedGpuCpu);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "float-uav-allocator");
  check(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)), "float-uav-list");
  ID3D12DescriptorHeap *heaps[] = {gpuHeap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  D3D12_RESOURCE_BARRIER uavBarrier = {};
  uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = texture.Get();
  const float initial[4] = {0.5f, 1.5f, -2.0f, 4.0f};
  list->ClearUnorderedAccessViewFloat(allGpu, allCpu, texture.Get(), initial, 0, nullptr);
  list->ResourceBarrier(1, &uavBarrier);

  auto predicateZero = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateOne = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = createBuffer(context.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  uint64_t *values = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&values)), "float-uav-predicate-map");
  values[0] = 0;
  values[1] = 1;
  upload->Unmap(0, nullptr);
  list->CopyBufferRegion(predicateZero.Get(), 0, upload.Get(), 0, 8);
  list->CopyBufferRegion(predicateOne.Get(), 0, upload.Get(), 8, 8);
  std::array<D3D12_RESOURCE_BARRIER, 2> predicateBarriers = {
      transition(predicateZero.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
      transition(predicateOne.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
  };
  list->ResourceBarrier(static_cast<UINT>(predicateBarriers.size()), predicateBarriers.data());
  const float rejected[4] = {99.0f, 98.0f, 97.0f, 96.0f};
  list->SetPredication(predicateZero.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearUnorderedAccessViewFloat(selectedGpu, selectedCpu, texture.Get(), rejected, 0, nullptr);
  list->ResourceBarrier(1, &uavBarrier);
  const D3D12_RECT rect = {1, 0, 3, 2};
  const float accepted[4] = {8.0f, -4.0f, 2.0f, 0.25f};
  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearUnorderedAccessViewFloat(selectedGpu, selectedCpu, texture.Get(), accepted, 1, &rect);
  list->ResourceBarrier(1, &uavBarrier);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, layers> footprints = {};
  std::array<UINT, layers> rows = {};
  std::array<UINT64, layers> rowBytes = {};
  UINT64 bytes = 0;
  context.device->GetCopyableFootprints(&desc, 0, layers, 0, footprints.data(), rows.data(), rowBytes.data(), &bytes);
  auto readback = createBuffer(context.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  auto toCopy = transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &toCopy);
  for (UINT layer = 0; layer < layers; ++layer)
    copyTextureToReadback(list.Get(), texture.Get(), layer, readback.Get(), footprints[layer]);
  context.submit(list.Get());

  uint8_t *data = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&data)), "float-uav-readback-map");
  for (UINT layer = 0; layer < layers; ++layer) {
    for (UINT y = 0; y < height; ++y) {
      const auto *row = reinterpret_cast<const float *>(data + footprints[layer].Offset +
                                                        y * footprints[layer].Footprint.RowPitch);
      for (UINT x = 0; x < width; ++x) {
        const bool selected = layer == 1 && x >= 1 && x < 3;
        const float *expected = selected ? accepted : initial;
        require(std::memcmp(row + x * 4, expected, sizeof(initial)) == 0,
                "typed-float-uav-array-rect-predicate");
      }
    }
  }
  readback->Unmap(0, nullptr);
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_TYPED_FLOAT_UAV_PASS format=R32G32B32A32_FLOAT layers=2 rect_preservation=1 predicate_preservation=1");
}


void writeUpload(ID3D12Resource *resource, const void *data, size_t size) {
  void *mapped = nullptr;
  check(resource->Map(0, nullptr, &mapped), "map-upload");
  std::memcpy(mapped, data, size);
  resource->Unmap(0, nullptr);
}

std::vector<uint8_t> readFile(const char *path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("open-indirect-compute-dxil", HRESULT_FROM_WIN32(GetLastError()));

  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16ll * 1024 * 1024) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    fail("size-indirect-compute-dxil", error ? HRESULT_FROM_WIN32(error) : E_INVALIDARG);
  }

  std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    fail("read-indirect-compute-dxil", HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT));
  }
  CloseHandle(file);
  return bytes;
}

void RunIndirectTests(TestContext &context, const std::vector<uint8_t> &computeShader,
                      const std::vector<uint8_t> &vertexShader) {

  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[0].Descriptor.ShaderRegister = 0;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants.ShaderRegister = 0;
  parameters[1].Constants.Num32BitValues = 1;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = parameters;
  ComPtr<ID3DBlob> serialized;
  ComPtr<ID3DBlob> rootErrors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors),
        "serialize-indirect-root");
  ComPtr<ID3D12RootSignature> root;
  check(context.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&root)), "create-indirect-root");

  D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
  pipelineDesc.pRootSignature = root.Get();
  pipelineDesc.CS = {computeShader.data(), computeShader.size()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(context.device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)),
        "create-indirect-pipeline");

  D3D12_INDIRECT_ARGUMENT_DESC indirectArgs[2] = {};
  indirectArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  indirectArgs[0].Constant.RootParameterIndex = 1;
  indirectArgs[0].Constant.Num32BitValuesToSet = 1;
  indirectArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
  signatureDesc.ByteStride = sizeof(uint32_t) + sizeof(D3D12_DISPATCH_ARGUMENTS);
  signatureDesc.NumArgumentDescs = 2;
  signatureDesc.pArgumentDescs = indirectArgs;
  ComPtr<ID3D12CommandSignature> signature;
  check(context.device->CreateCommandSignature(&signatureDesc, root.Get(), IID_PPV_ARGS(&signature)),
        "create-indirect-signature");

  D3D12_INDIRECT_ARGUMENT_DESC graphicsArgs[3] = {};
  graphicsArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
  graphicsArgs[0].VertexBuffer.Slot = 3;
  graphicsArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
  graphicsArgs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  D3D12_COMMAND_SIGNATURE_DESC graphicsSignatureDesc = {};
  graphicsSignatureDesc.ByteStride = sizeof(D3D12_VERTEX_BUFFER_VIEW) + sizeof(D3D12_INDEX_BUFFER_VIEW) +
                                     sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  graphicsSignatureDesc.NumArgumentDescs = 3;
  graphicsSignatureDesc.pArgumentDescs = graphicsArgs;
  ComPtr<ID3D12CommandSignature> graphicsSignature;
  check(context.device->CreateCommandSignature(&graphicsSignatureDesc, nullptr,
                                                IID_PPV_ARGS(&graphicsSignature)),
        "create-vb-ib-indexed-signature");

  struct IndirectRecord {
    uint32_t amount;
    D3D12_DISPATCH_ARGUMENTS dispatch;
  };
  static_assert(sizeof(IndirectRecord) == 16);
  const IndirectRecord record = {3, {1, 1, 1}};
  const std::array<uint64_t, 2> predicateValues = {0, 1};
  const std::array<uint32_t, 2> countValues = {0, 1};
  const uint32_t zero = 0;

  auto output = createBuffer(context.device.Get(), 4, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto predicateZero = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateOne = createBuffer(context.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
  auto initializer = createBuffer(context.device.Get(), 4, D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
  auto predicateUpload = createBuffer(context.device.Get(), sizeof(predicateValues), D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  auto argumentBuffer = createBuffer(context.device.Get(), sizeof(record), D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
  auto countBuffer = createBuffer(context.device.Get(), sizeof(countValues), D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = createBuffer(context.device.Get(), 4, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  writeUpload(initializer.Get(), &zero, sizeof(zero));
  writeUpload(predicateUpload.Get(), predicateValues.data(), sizeof(predicateValues));
  writeUpload(argumentBuffer.Get(), &record, sizeof(record));
  writeUpload(countBuffer.Get(), countValues.data(), sizeof(countValues));

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "indirect-allocator");
  check(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                          IID_PPV_ARGS(&list)), "indirect-list");
  list->SetComputeRootSignature(root.Get());
  list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  list->CopyBufferRegion(output.Get(), 0, initializer.Get(), 0, 4);
  auto outputToUav = transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ResourceBarrier(1, &outputToUav);

  list->CopyBufferRegion(predicateZero.Get(), 0, predicateUpload.Get(), 0, 8);
  list->CopyBufferRegion(predicateOne.Get(), 0, predicateUpload.Get(), 8, 8);
  std::array<D3D12_RESOURCE_BARRIER, 2> predicateBarriers = {
      transition(predicateZero.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
      transition(predicateOne.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION),
  };
  list->ResourceBarrier(static_cast<UINT>(predicateBarriers.size()), predicateBarriers.data());

  list->SetPredication(predicateZero.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->SetComputeRoot32BitConstant(1, 100, 0);
  list->Dispatch(1, 1, 1);

  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->SetComputeRoot32BitConstant(1, 1, 0);
  list->Dispatch(1, 1, 1);

  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  list->ExecuteIndirect(signature.Get(), 1, argumentBuffer.Get(), 0, countBuffer.Get(), 4);

  list->SetPredication(predicateZero.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  list->ExecuteIndirect(signature.Get(), 1, argumentBuffer.Get(), 0, countBuffer.Get(), 4);

  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ExecuteIndirect(signature.Get(), 1, argumentBuffer.Get(), 0, countBuffer.Get(), 0);
  list->ExecuteIndirect(signature.Get(), 1, argumentBuffer.Get(), 0, countBuffer.Get(), 4);
  list->Dispatch(1, 1, 1);
  list->SetComputeRoot32BitConstant(1, 2, 0);
  list->Dispatch(1, 1, 1);

  D3D12_RESOURCE_BARRIER uavBarrier = {};
  uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = output.Get();
  list->ResourceBarrier(1, &uavBarrier);
  auto outputToCopy = transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &outputToCopy);
  list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4);
  context.submit(list.Get());

  uint32_t *result = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&result)), "map-indirect-result");
  require(*result == 9, "indirect-predicate-root-count-result");
  readback->Unmap(0, nullptr);

  D3D12_ROOT_PARAMETER graphicsRootParameter = {};
  graphicsRootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  graphicsRootParameter.Descriptor.ShaderRegister = 0;
  graphicsRootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC graphicsRootDesc = {};
  graphicsRootDesc.NumParameters = 1;
  graphicsRootDesc.pParameters = &graphicsRootParameter;
  ComPtr<ID3DBlob> graphicsRootBlob;
  check(D3D12SerializeRootSignature(&graphicsRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &graphicsRootBlob, &rootErrors), "serialize-graphics-root");
  ComPtr<ID3D12RootSignature> graphicsRoot;
  check(context.device->CreateRootSignature(0, graphicsRootBlob->GetBufferPointer(),
                                            graphicsRootBlob->GetBufferSize(), IID_PPV_ARGS(&graphicsRoot)),
        "create-graphics-root");
  D3D12_INPUT_ELEMENT_DESC input = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0,
                                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPipelineDesc = {};
  graphicsPipelineDesc.pRootSignature = graphicsRoot.Get();
  graphicsPipelineDesc.VS = {vertexShader.data(), vertexShader.size()};
  graphicsPipelineDesc.BlendState.AlphaToCoverageEnable = FALSE;
  graphicsPipelineDesc.BlendState.IndependentBlendEnable = FALSE;
  graphicsPipelineDesc.SampleMask = UINT_MAX;
  graphicsPipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  graphicsPipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  graphicsPipelineDesc.RasterizerState.DepthClipEnable = TRUE;
  graphicsPipelineDesc.DepthStencilState.DepthEnable = FALSE;
  graphicsPipelineDesc.DepthStencilState.StencilEnable = FALSE;
  graphicsPipelineDesc.InputLayout = {&input, 1};
  graphicsPipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  graphicsPipelineDesc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> graphicsPipeline;
  check(context.device->CreateGraphicsPipelineState(&graphicsPipelineDesc, IID_PPV_ARGS(&graphicsPipeline)),
        "create-graphics-pipeline");

  const std::array<float, 4> vertex = {2.0f, 2.0f, 0.0f, 1.0f};
  const uint16_t index = 0;
  auto vertexBuffer = createBuffer(context.device.Get(), sizeof(vertex), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indexBuffer = createBuffer(context.device.Get(), sizeof(index), D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
  writeUpload(vertexBuffer.Get(), vertex.data(), sizeof(vertex));
  writeUpload(indexBuffer.Get(), &index, sizeof(index));
  const D3D12_VERTEX_BUFFER_VIEW vertexView = {vertexBuffer->GetGPUVirtualAddress(), sizeof(vertex), sizeof(vertex)};
  const D3D12_INDEX_BUFFER_VIEW indexView = {indexBuffer->GetGPUVirtualAddress(), sizeof(index), DXGI_FORMAT_R16_UINT};
  struct GraphicsRecord {
    D3D12_VERTEX_BUFFER_VIEW vertex;
    D3D12_INDEX_BUFFER_VIEW index;
    D3D12_DRAW_INDEXED_ARGUMENTS draw;
  };
  const GraphicsRecord graphicsRecord = {vertexView, indexView, {1, 1, 0, 0, 0}};
  auto graphicsArguments = createBuffer(context.device.Get(), sizeof(graphicsRecord), D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
  writeUpload(graphicsArguments.Get(), &graphicsRecord, sizeof(graphicsRecord));

  check(allocator->Reset(), "graphics-allocator-reset");
  check(list->Reset(allocator.Get(), graphicsPipeline.Get()), "graphics-list-reset");
  auto outputToDestination = transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
  list->ResourceBarrier(1, &outputToDestination);
  list->CopyBufferRegion(output.Get(), 0, initializer.Get(), 0, 4);
  outputToUav = transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ResourceBarrier(1, &outputToUav);
  list->SetGraphicsRootSignature(graphicsRoot.Get());
  list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  list->IASetVertexBuffers(3, 1, &vertexView);
  list->IASetIndexBuffer(&indexView);

  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  list->DrawInstanced(1, 1, 0, 0);
  list->SetPredication(predicateOne.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->DrawInstanced(1, 1, 0, 0);
  list->DrawIndexedInstanced(1, 1, 0, 0, 0);
  list->ExecuteIndirect(graphicsSignature.Get(), 1, graphicsArguments.Get(), 0, countBuffer.Get(), 4);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ResourceBarrier(1, &uavBarrier);
  outputToCopy = transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &outputToCopy);
  list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4);
  context.submit(list.Get());
  result = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&result)), "map-graphics-result");
  require(*result == 3, "direct-indexed-vb-ib-indirect-result");
  readback->Unmap(0, nullptr);

}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  require(argc == 3, "usage-predication-indirect-dxil");
  const auto computeShader = readFile(argv[1]);
  const auto vertexShader = readFile(argv[2]);
  requireOwnedBackend();
  TestContext context;
  createContext(context);
  RunPredicatedClearTests(context);
  RunTypedMultilayerClearTests(context);
  RunTypedFloatUavClearTest(context);
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_CLEAR_CASES_PASS");
  RunIndirectTests(context, computeShader, vertexShader);
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_INDIRECT_CASES_PASS");
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_PASS");
  return 0;
}
