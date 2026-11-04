#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <bit>
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

template <typename... Args>
void writeIndirectPhase(const char *format, Args... args) {
  char line[256];
  const int length = std::snprintf(line, sizeof(line), format, args...);
  DWORD written = 0;
  if (length <= 0 || size_t(length) >= sizeof(line) ||
      !WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr) || written != DWORD(length))
    fail("indirect phase marker");
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
    submitClosed(list);
  }

  void submitClosed(ID3D12GraphicsCommandList *list) {
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


void RunBufferUavRectangleTests(TestContext &context) {
  constexpr UINT wordCount = 512;
  constexpr UINT viewElements = 37;
  constexpr UINT bufferBytes = wordCount * sizeof(UINT);
  struct ViewCase {
    DXGI_FORMAT format;
    UINT components;
    bool raw;
    bool floating;
  };
  const ViewCase views[] = {
      {DXGI_FORMAT_R32_TYPELESS, 1, true, false},
      {DXGI_FORMAT_R32_UINT, 1, false, false},
      {DXGI_FORMAT_R32_FLOAT, 1, false, true},
      {DXGI_FORMAT_R32G32B32A32_UINT, 4, false, false},
      {DXGI_FORMAT_R32G32B32A32_FLOAT, 4, false, true},
  };
  const UINT uintValues[] = {0xffffffff, 0x80000001, 0x01000001, 0x7fffffff};
  const float floatValues[] = {1.25f, -2.5f, 3.75f, -4.5f};
  // Disjoint visible ranges [0, 5), [12, 17), [35, 37); all other rectangles are empty after clipping.
  const D3D12_RECT rects[] = {
      {-3, -2, 5, 2}, {12, 0, 17, 1}, {35, 0, LONG_MAX, 1},
      {LONG_MIN, 0, -1, 1}, {39, 0, LONG_MAX, 1}, {7, 0, 3, 1},
      {5, 1, 12, 2}, {5, -2, 12, 0}, {5, 0, 5, 1},
  };
  std::array<UINT, wordCount> initial;
  for (UINT i = 0; i < wordCount; ++i)
    initial[i] = 0x12340000u + i;
  auto upload = createBuffer(context.device.Get(), bufferBytes + 16, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "buffer-uav-upload-map");
  std::memcpy(mapped, initial.data(), bufferBytes);
  const UINT64 predicates[] = {0, 1};
  std::memcpy(static_cast<uint8_t *>(mapped) + bufferBytes, predicates, sizeof(predicates));
  upload->Unmap(0, nullptr);
  auto predicate = createBuffer(context.device.Get(), sizeof(predicates), D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = createBuffer(context.device.Get(), bufferBytes, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "buffer-uav-allocator");
  check(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)), "buffer-uav-list");
  list->CopyBufferRegion(predicate.Get(), 0, upload.Get(), bufferBytes, sizeof(predicates));
  auto predicateBarrier = transition(predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_PREDICATION);
  list->ResourceBarrier(1, &predicateBarrier);
  context.submit(list.Get());

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 1;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> gpuHeap;
  check(context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gpuHeap)), "buffer-uav-gpu-heap");
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> cpuHeap;
  check(context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cpuHeap)), "buffer-uav-cpu-heap");
  const auto cpu = cpuHeap->GetCPUDescriptorHandleForHeapStart();
  const auto gpuCpu = gpuHeap->GetCPUDescriptorHandleForHeapStart();
  const auto gpu = gpuHeap->GetGPUDescriptorHandleForHeapStart();
  UINT cases = 0;
  for (const auto &view : views) {
    for (UINT first : {8u, 72u}) {
      for (UINT predication = 0; predication < 3; ++predication) {
        for (bool rectangular : {false, true}) {
          auto buffer = createBuffer(context.device.Get(), bufferBytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
          D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
          desc.Format = view.format;
          desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          desc.Buffer.FirstElement = first;
          desc.Buffer.NumElements = viewElements;
          desc.Buffer.Flags = view.raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
          context.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &desc, cpu);
          context.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &desc, gpuCpu);
          check(allocator->Reset(), "buffer-uav-reset-allocator");
          check(list->Reset(allocator.Get(), nullptr), "buffer-uav-reset-list");
          list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, bufferBytes);
          auto toUav = transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          list->ResourceBarrier(1, &toUav);
          ID3D12DescriptorHeap *heaps[] = {gpuHeap.Get()};
          list->SetDescriptorHeaps(1, heaps);
          // EQUAL_ZERO skips zero and executes nonzero. Test both, independently of the direct path.
          if (predication)
            list->SetPredication(predicate.Get(), predication == 1 ? 8 : 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
          const UINT rectCount = rectangular ? static_cast<UINT>(std::size(rects)) : 0;
          if (view.floating)
            list->ClearUnorderedAccessViewFloat(gpu, cpu, buffer.Get(), floatValues, rectCount,
                                                rectangular ? rects : nullptr);
          else
            list->ClearUnorderedAccessViewUint(gpu, cpu, buffer.Get(), uintValues, rectCount,
                                               rectangular ? rects : nullptr);
          list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
          auto toCopy = transition(buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->ResourceBarrier(1, &toCopy);
          list->CopyBufferRegion(readback.Get(), 0, buffer.Get(), 0, bufferBytes);
          context.submit(list.Get());
          UINT *actual = nullptr;
          check(readback->Map(0, nullptr, reinterpret_cast<void **>(&actual)), "buffer-uav-readback-map");
          for (UINT i = 0; i < wordCount; ++i) {
            UINT expected = initial[i];
            const UINT element = i / view.components;
            if (predication != 2 && element >= first && element < first + viewElements) {
              const UINT x = element - first;
              if (!rectangular || x < 5 || (x >= 12 && x < 17) || x >= 35) {
                const UINT component = i % view.components;
                expected = view.floating ? std::bit_cast<UINT>(floatValues[component]) : uintValues[component];
              }
            }
            if (actual[i] != expected) {
              std::fprintf(stderr, "buffer-uav-mismatch format=%u float=%u first=%u predicate=%u rect=%u "
                                   "word=%u expected=%08x actual=%08x\n",
                           UINT(view.format), view.floating, first, predication, rectangular, i, expected, actual[i]);
              fail("buffer-uav-view-relative-rect");
            }
          }
          readback->Unmap(0, nullptr);
          ++cases;
        }
      }
    }
  }
  std::printf("OWNED_D3D12_PREDICATION_INDIRECT_BUFFER_UAV_PASS cases=%u view_relative=1 "
              "clipping=1 sentinels=1 predicate_preservation=1\n", cases);
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

void RunPreparedComputeIndirect(TestContext &context, const std::vector<uint8_t> &computeShader) {
  // GPU writes the argument/count/predicate storage on every closed-list replay.
  // The existing supplied shader stays unchanged: one thread atomically adds b0.
  static constexpr char producerSource[] = R"(
    RWByteAddressBuffer arguments : register(u0);
    RWByteAddressBuffer controls : register(u1);
    ByteAddressBuffer input : register(t0);
    cbuffer Config : register(b0) { uint maximum; uint mutate; uint addressLow; uint addressHigh; };
    [numthreads(32,1,1)]
    void main(uint3 tid : SV_DispatchThreadID) {
      uint i = tid.x;
      if (i == 0) {
        controls.Store(0, input.Load(4)); controls.Store(4, 0);
        controls.Store(16, input.Load(0));
      }
      if (i >= maximum) return;
      uint offset = 16 + i * 32;
      arguments.Store4(offset, uint4(0,0,0,0));
      arguments.Store4(offset + 16, uint4(0,0,0,0));
      if (mutate != 0) { arguments.Store(offset, i + 1); offset += 4; }
      if (mutate == 2) {
        uint low = addressLow + i * 8 + 4;
        arguments.Store2(offset, uint2(low, addressHigh + (low < addressLow ? 1 : 0)));
        offset += 8;
      }
      arguments.Store(offset, mutate != 0 ? 1 : i % 3 + 1);
      arguments.Store(offset + 4, mutate != 0 || i % 5 != 4 ? 1 : 0);
      arguments.Store(offset + 8, 1);
    }
  )";
  ComPtr<ID3DBlob> producerShader, errors, serialized;
  check(D3DCompile(producerSource, sizeof(producerSource)-1, nullptr, nullptr, nullptr,
                   "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &producerShader, &errors),
        "prepared-compute-producer-compile");
  D3D12_ROOT_PARAMETER producerParameters[4] = {};
  producerParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  producerParameters[0].Descriptor.ShaderRegister = 0;
  producerParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  producerParameters[1].Descriptor.ShaderRegister = 1;
  producerParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  producerParameters[2].Descriptor.ShaderRegister = 0;
  producerParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  producerParameters[3].Constants.Num32BitValues = 4;
  D3D12_ROOT_SIGNATURE_DESC producerRootDesc = {};
  producerRootDesc.NumParameters = 4; producerRootDesc.pParameters = producerParameters;
  check(D3D12SerializeRootSignature(&producerRootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),
        "prepared-compute-producer-root-serialize");
  ComPtr<ID3D12RootSignature> producerRoot;
  check(context.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
        IID_PPV_ARGS(&producerRoot)),"prepared-compute-producer-root");
  D3D12_COMPUTE_PIPELINE_STATE_DESC producerDesc = {};
  producerDesc.pRootSignature = producerRoot.Get();
  producerDesc.CS = {producerShader->GetBufferPointer(),producerShader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> producer;
  check(context.device->CreateComputePipelineState(&producerDesc,IID_PPV_ARGS(&producer)),
        "prepared-compute-producer-pipeline");
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants.Num32BitValues = 1;
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 2; rootDesc.pParameters = parameters;
  check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),
        "prepared-compute-root-serialize");
  ComPtr<ID3D12RootSignature> root;
  check(context.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)),"prepared-compute-root");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
  pipelineDesc.pRootSignature = root.Get(); pipelineDesc.CS = {computeShader.data(),computeShader.size()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(context.device->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)),
        "prepared-compute-pipeline");
  const UINT maxima[] = {0,1,31,32,33,4097};
  for (UINT mode = 0; mode < 5; ++mode) {
    const bool mutate = mode == 2 || mode == 3;
    const bool counted = mode == 1 || mode == 3;
    const bool predicated = mode == 3 || mode == 4;
    D3D12_INDIRECT_ARGUMENT_DESC descriptions[3] = {};
    descriptions[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    descriptions[0].Constant.RootParameterIndex = 1;
    descriptions[0].Constant.Num32BitValuesToSet = 1;
    descriptions[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    descriptions[1].UnorderedAccessView.RootParameterIndex = 0;
    descriptions[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    if (mode == 2) descriptions[1] = descriptions[2];
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
    signatureDesc.ByteStride = 32; signatureDesc.NumArgumentDescs = mode == 3 ? 3 : mutate ? 2 : 1;
    signatureDesc.pArgumentDescs = descriptions + (mutate ? 0 : 2);
    ComPtr<ID3D12CommandSignature> signature;
    check(context.device->CreateCommandSignature(&signatureDesc,mutate ? root.Get() : nullptr,
          IID_PPV_ARGS(&signature)),"prepared-compute-signature");
    for (UINT maximum : maxima) {
      if ((mode == 2 || mode == 4) && maximum != 33)
        continue;
      const UINT64 argumentBytes = 16 + UINT64(maximum)*32 + 16;
      auto arguments = createBuffer(context.device.Get(),argumentBytes,D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto controls = createBuffer(context.device.Get(),32,D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto input = createBuffer(context.device.Get(),8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
      std::vector<uint32_t> initial(mode == 3 ? 2*maximum+2 : 4);
      for (UINT i = 0; i < initial.size(); ++i)
        initial[i] = mode == 3 ? (i%2 == 0 || i == initial.size()-1 ? 0xd00d0000+i : 0) :
                     i == 0 ? 0xa17e0011 : i == 1 ? 0 : i == 2 ? 0xb28f0022 : 0xc3900033;
      const UINT64 outputBytes = initial.size()*sizeof(uint32_t);
      auto initializer = createBuffer(context.device.Get(),outputBytes,D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
      writeUpload(initializer.Get(),initial.data(),outputBytes);
      auto output = createBuffer(context.device.Get(),outputBytes,D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto readback = createBuffer(context.device.Get(),outputBytes,D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),
            "prepared-compute-allocator");
      check(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),producer.Get(),
            IID_PPV_ARGS(&list)),"prepared-compute-list");
      list->CopyBufferRegion(output.Get(),0,initializer.Get(),0,outputBytes);
      auto outputBarrier = transition(output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->ResourceBarrier(1,&outputBarrier);
      list->SetComputeRootSignature(producerRoot.Get());
      list->SetComputeRootUnorderedAccessView(0,arguments->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(1,controls->GetGPUVirtualAddress());
      list->SetComputeRootShaderResourceView(2,input->GetGPUVirtualAddress());
      const UINT64 address = output->GetGPUVirtualAddress();
      const UINT config[] = {maximum,mode == 3 ? 2u : mutate ? 1u : 0u,
                             static_cast<UINT>(address),static_cast<UINT>(address >> 32)};
      list->SetComputeRoot32BitConstants(3,4,config,0);
      list->Dispatch(maximum ? maximum/32 + (maximum%32 != 0) : 1,1,1);
      D3D12_RESOURCE_BARRIER published[] = {
        transition(arguments.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        transition(controls.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
      list->ResourceBarrier(2,published);
      list->SetPipelineState(pipeline.Get());
      list->SetComputeRootSignature(root.Get());
      list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress()+4);
      list->SetComputeRoot32BitConstant(1,7,0);
      if (predicated)
        list->SetPredication(controls.Get(),0,D3D12_PREDICATION_OP_EQUAL_ZERO);
      list->ExecuteIndirect(signature.Get(),maximum,arguments.Get(),16,counted ? controls.Get() : nullptr,16);
      list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
      // End in the same states used by the first execution, so exact replay
      // reruns the producer and reuses the recorded resolver/ICB allocation.
      D3D12_RESOURCE_BARRIER finished[] = {
        transition(arguments.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(controls.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE)};
      list->ResourceBarrier(3,finished);
      list->CopyBufferRegion(readback.Get(),0,output.Get(),0,outputBytes);
      outputBarrier = transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
      list->ResourceBarrier(1,&outputBarrier);
      check(list->Close(),"prepared-compute-close");
      const UINT replayCounts[] = {maximum,1,maximum,maximum,maximum,0,maximum+7};
      const UINT executions = (counted || predicated) && maximum == 33 ? 7 : 1;
      for (UINT execution = 0; execution < executions; ++execution) {
        const UINT count = execution ? replayCounts[execution] : maximum;
        const bool predicate = execution != 3;
        const UINT values[] = {count,predicate ? 1u : 0u};
        writeUpload(input.Get(),values,sizeof(values));
        writeIndirectPhase("OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=compute mode=%u max=%u replay=%u phase=submit_begin\n",
                     mode,maximum,execution);
        context.submitClosed(list.Get());
        writeIndirectPhase("OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=compute mode=%u max=%u replay=%u phase=submit_end\n",
                     mode,maximum,execution);
        const UINT active = predicated && !predicate ? 0 :
                            counted && count < maximum ? count : maximum;
        UINT expected = 0;
        for (UINT i = 0; i < active; ++i)
          expected += mutate ? i+1 : (i%5 != 4 ? 7*(i%3+1) : 0);
        UINT *observed = nullptr;
        check(readback->Map(0,nullptr,reinterpret_cast<void **>(&observed)),"prepared-compute-map");
        UINT sum = 0;
        if (mode == 3) {
          for (UINT i = 0; i < initial.size(); ++i) {
            const bool lane = i%2 == 1 && i < 2*maximum;
            const UINT value = lane ? (i/2 < active ? i/2+1 : 0) : initial[i];
            require(observed[i] == value,"prepared-compute-exact-root-uav-lane-and-guard");
            if (lane) sum += observed[i];
          }
          require(sum == expected,"prepared-compute-root-uav-sum");
        } else {
          require(observed[0] == initial[0] && observed[1] == expected &&
                  observed[2] == initial[2] && observed[3] == initial[3],"prepared-compute-exact-value-and-guards");
          sum = observed[1];
        }
        std::printf("OWNED_D3D12_PREDICATION_INDIRECT_PREPARED_COMPUTE_VALUES mode=%u max=%u replay=%u count=%u predicate=%u result=%u guards=%08x,%08x,%08x\n",
                    mode,maximum,execution,count,predicate,sum,observed[0],observed[initial.size()-2],observed[initial.size()-1]);
        readback->Unmap(0,nullptr);
      }
    }
  }
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_PREPARED_COMPUTE_PASS");
}

void RunSimpleGraphicsIndirect(TestContext &context) {
  static constexpr char source[] = R"(
    struct VOut { float4 position : SV_Position; nointerpolation float4 color : COLOR0; };
    VOut vs(uint vertex : SV_VertexID, float4 instanceColor : COLOR0, float4 vertexColor : COLOR1) {
      VOut result;
      uint corner = vertex % 3;
      result.position = float4(corner == 2 ? 3 : -1, corner == 1 ? 3 : -1, 0, 1);
      result.color = float4(vertexColor.r, instanceColor.g, 0, 1);
      return result;
    }
    float4 ps(VOut input) : SV_Target0 { return input.color; }
  )";
  ComPtr<ID3DBlob> vs, ps, errors, serialized;
  check(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors),"simple-indirect-vs-compile");
  check(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors),"simple-indirect-ps-compile");
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),
        "simple-indirect-root-serialize");
  ComPtr<ID3D12RootSignature> root;
  check(context.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)),"simple-indirect-root");
  const D3D12_INPUT_ELEMENT_DESC inputs[] = {
    {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,2,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},
    {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root.Get(); desc.VS = {vs->GetBufferPointer(),vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(),ps->GetBufferSize()}; desc.InputLayout = {inputs,2};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX; desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  check(context.device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&pipeline)),"simple-indirect-pipeline");
  std::array<std::array<float,4>,16> colors = {};
  for (UINT i = 0; i < colors.size(); ++i)
    colors[i] = {0, i/255.0f, 0, 1};
  auto instanceBuffer = createBuffer(context.device.Get(),sizeof(colors),D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
  writeUpload(instanceBuffer.Get(),colors.data(),sizeof(colors));
  const D3D12_VERTEX_BUFFER_VIEW instanceView = {instanceBuffer->GetGPUVirtualAddress(),sizeof(colors),16};
  // SV_VertexID excludes start/baseVertex; fetched data observes those offsets.
  std::array<std::array<float,4>,16> vertexColors = {};
  for (UINT i = 0; i < vertexColors.size(); ++i)
    vertexColors[i] = {(i/3+1)/255.0f,0,0,1};
  auto vertexBuffer = createBuffer(context.device.Get(),sizeof(vertexColors),D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
  writeUpload(vertexBuffer.Get(),vertexColors.data(),sizeof(vertexColors));
  const D3D12_VERTEX_BUFFER_VIEW vertexView = {vertexBuffer->GetGPUVirtualAddress(),sizeof(vertexColors),16};
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(context.device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"simple-indirect-rtv-heap");
  D3D12_RESOURCE_DESC texture = {};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = 4; texture.Height = 2;
  texture.DepthOrArraySize = 1; texture.MipLevels = 1; texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const UINT maxima[] = {0,1,2,4,63,64,65,4096};
  for (UINT kind = 0; kind < 3; ++kind) {
    const bool indexed = kind != 0;
    const bool wide = kind == 2;
    D3D12_INDIRECT_ARGUMENT_DESC operation = {};
    operation.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
    signatureDesc.ByteStride = 32; signatureDesc.NumArgumentDescs = 1; signatureDesc.pArgumentDescs = &operation;
    ComPtr<ID3D12CommandSignature> signature;
    check(context.device->CreateCommandSignature(&signatureDesc,nullptr,IID_PPV_ARGS(&signature)),
          "simple-indirect-signature");
    const uint16_t indices16[] = {30,30,30,6,7,8,9,10,11};
    const uint32_t indices32[] = {30,30,30,6,7,8,9,10,11};
    const UINT width = wide ? 4 : 2;
    auto indices = createBuffer(context.device.Get(),9*width,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    writeUpload(indices.Get(),wide ? static_cast<const void *>(indices32) : indices16,9*width);
    // Bound IB offset excludes two poison elements; argument indexStart skips
    // the third. Valid backing remainder is a whole number of indices.
    const D3D12_INDEX_BUFFER_VIEW indexView = {indices->GetGPUVirtualAddress()+2*width,7*width,
                                               wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    for (UINT maximum : maxima) {
      std::vector<uint8_t> records(16 + (maximum < 4 ? 4 : maximum) * 32 + 16);
      for (UINT i = 0; i < (maximum < 4 ? 4 : maximum); ++i) {
        const D3D12_DRAW_ARGUMENTS draw = {i >= 3 ? 0u : 3u,i == 2 ? 0u : 1u,3,5};
        const D3D12_DRAW_INDEXED_ARGUMENTS drawIndexed = {i >= 3 ? 0u : 3u,i == 2 ? 0u : 1u,1,-3,5};
        std::memcpy(records.data()+16+i*32,indexed ? static_cast<const void *>(&drawIndexed) : &draw,
                    indexed ? sizeof(drawIndexed) : sizeof(draw));
      }
      auto upload = createBuffer(context.device.Get(),records.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
      writeUpload(upload.Get(),records.data(),records.size());
      auto arguments = createBuffer(context.device.Get(),records.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
      auto properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
      ComPtr<ID3D12Resource> color;
      check(context.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&texture,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&color)),"simple-indirect-color");
      const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
      context.device->CreateRenderTargetView(color.Get(),nullptr,rtv);
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
      UINT64 bytes = 0; context.device->GetCopyableFootprints(&texture,0,1,512,&footprint,nullptr,nullptr,&bytes);
      auto readback = createBuffer(context.device.Get(),bytes+512,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
      std::vector<uint8_t> guards(static_cast<size_t>(bytes+512),0xa5);
      auto guardUpload = createBuffer(context.device.Get(),guards.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
      writeUpload(guardUpload.Get(),guards.data(),guards.size());
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),
            "simple-indirect-allocator");
      check(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),
            IID_PPV_ARGS(&list)),"simple-indirect-list");
      list->CopyBufferRegion(arguments.Get(),0,upload.Get(),0,records.size());
      auto argumentBarrier = transition(arguments.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
      list->ResourceBarrier(1,&argumentBarrier);
      const float clear[] = {0,0,0,0}; list->ClearRenderTargetView(rtv,clear,0,nullptr);
      list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
      const D3D12_VIEWPORT viewport = {0,0,4,2,0,1}; const D3D12_RECT scissor = {1,0,3,2};
      list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
      list->SetGraphicsRootSignature(root.Get()); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->IASetVertexBuffers(0,1,&vertexView);
      list->IASetVertexBuffers(2,1,&instanceView);
      if (indexed) list->IASetIndexBuffer(&indexView);
      list->ExecuteIndirect(signature.Get(),maximum,arguments.Get(),16,nullptr,0);
      auto colorBarrier = transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->ResourceBarrier(1,&colorBarrier);
      list->CopyBufferRegion(readback.Get(),0,guardUpload.Get(),0,guards.size());
      copyTextureToReadback(list.Get(),color.Get(),0,readback.Get(),footprint);
      check(list->Close(),"simple-indirect-close");
      // The final live record changes after Close. The GPU copy and native
      // indirect command must observe it; last-command order determines color.
      const D3D12_DRAW_ARGUMENTS last = {3,1,6,7};
      const D3D12_DRAW_INDEXED_ARGUMENTS lastIndexed = {3,1,4,-3,7};
      std::memcpy(records.data()+16+32,indexed ? static_cast<const void *>(&lastIndexed) : &last,
                  indexed ? sizeof(lastIndexed) : sizeof(last));
      writeUpload(upload.Get(),records.data(),records.size());
      writeIndirectPhase("OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=graphics format=%u max=%u phase=submit_begin\n",kind,maximum);
      context.submitClosed(list.Get());
      writeIndirectPhase("OWNED_D3D12_INDIRECT_PREPARATION_PHASE kind=graphics format=%u max=%u phase=submit_end\n",kind,maximum);
      const uint8_t red = maximum >= 2 ? 3 : 2; const uint8_t green = maximum >= 2 ? 7 : 5;
      uint8_t *observed = nullptr;
      check(readback->Map(0,nullptr,reinterpret_cast<void **>(&observed)),"simple-indirect-image-map");
      for (UINT y = 0; y < 2; ++y)
        for (UINT x = 0; x < 4; ++x) {
          const uint8_t *pixel = observed+footprint.Offset+y*footprint.Footprint.RowPitch+x*4;
          const bool covered = maximum != 0 && x >= 1 && x < 3;
          if (pixel[0] != (covered ? red : 0) || pixel[1] != (covered ? green : 0) ||
              pixel[2] != 0 || pixel[3] != (covered ? 255 : 0)) {
            std::fprintf(stderr, "SIMPLE_INDIRECT_PIXEL format=%u max=%u x=%u y=%u actual=%u,%u,%u,%u expected=%u,%u,0,%u\n",
                kind, maximum, x, y, pixel[0], pixel[1], pixel[2], pixel[3], covered ? red : 0,
                covered ? green : 0, covered ? 255 : 0);
            require(false,"simple-indirect-exact-pixel");
          }
        }
      // Footprint row padding has unspecified contents; only bytes outside
      // the entire placed footprint are guards, never the copied row padding.
      const UINT64 end = footprint.Offset+UINT64(footprint.Footprint.RowPitch)*2;
      for (UINT64 i = 0; i < footprint.Offset; ++i)
        require(observed[i] == 0xa5,"simple-indirect-prefix-guard");
      for (UINT64 i = end; i < guards.size(); ++i)
        require(observed[i] == 0xa5,"simple-indirect-suffix-guard");
      std::printf("OWNED_D3D12_PREDICATION_INDIRECT_SIMPLE_GRAPHICS_VALUES kind=%u max=%u rgba=%u,%u,0,%u\n",
                  kind,maximum,maximum ? red : 0,maximum ? green : 0,maximum ? 255 : 0);
      readback->Unmap(0,nullptr);
    }
  }
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_SIMPLE_GRAPHICS_PASS");
}

void RunMSAAIndexedIndirectMutations(TestContext &context, UINT sampleCount) {
  // Only the ordinary single-sample ICB path records these large arrays.
  // MSAA resolves live commands at execution: keep its workload at two.
  const UINT commandCount = sampleCount == 1 ? 16385 : 2;
  constexpr UINT constantCount = 60;
  static constexpr char source[] = R"(
    RWStructuredBuffer<uint> counts : register(u1);
    cbuffer Tint : register(b0) { uint4 payload[15]; };
    Texture2D<float4> black : register(t0);
    SamplerState borderWhite : register(s0);
    struct VOut { float4 position : SV_Position; nointerpolation float tint : COLOR0; };
    VOut vs(float4 position : POSITION) {
      uint old; InterlockedAdd(counts[0], 1, old);
      VOut result; result.position = position;
      result.tint = payload[14].w == payload[0].x + 59 ? payload[0].x * .25 : 0;
      return result;
    }
    float4 ps(VOut input) : SV_Target0 {
      uint old; InterlockedAdd(counts[1], 1, old);
      return float4(input.tint * black.SampleLevel(borderWhite, float2(-2,-2), 0).r, 0, 0, 1);
    }
  )";
  ComPtr<ID3DBlob> vs, ps, errors;
  HRESULT shaderResult = D3DCompile(source, sizeof(source)-1, nullptr, nullptr, nullptr, "vs", "vs_5_0",
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, &errors);
  if (FAILED(shaderResult) && errors)
    std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
                 static_cast<const char *>(errors->GetBufferPointer()));
  check(shaderResult, "msaa-indirect-vs");
  errors.Reset();
  shaderResult = D3DCompile(source, sizeof(source)-1, nullptr, nullptr, nullptr, "ps", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, &errors);
  if (FAILED(shaderResult) && errors)
    std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
                 static_cast<const char *>(errors->GetBufferPointer()));
  check(shaderResult, "msaa-indirect-ps");
  D3D12_ROOT_PARAMETER roots[3] = {};
  roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  roots[0].Descriptor.ShaderRegister = 1;
  roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  roots[1].Constants.ShaderRegister = 0;
  roots[1].Constants.Num32BitValues = constantCount;
  roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_DESCRIPTOR_RANGE sampleRange = {};
  sampleRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  sampleRange.NumDescriptors = 1;
  roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  roots[2].DescriptorTable = {1, &sampleRange};
  roots[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 3;
  rootDesc.pParameters = roots;
  rootDesc.NumStaticSamplers = 1;
  rootDesc.pStaticSamplers = &sampler;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> serialized;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
        "msaa-indirect-root-serialize");
  ComPtr<ID3D12RootSignature> root;
  check(context.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&root)), "msaa-indirect-root");
  D3D12_INDIRECT_ARGUMENT_DESC args[5] = {};
  args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  args[0].Constant.RootParameterIndex = 1;
  args[0].Constant.Num32BitValuesToSet = constantCount;
  args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
  args[1].UnorderedAccessView.RootParameterIndex = 0;
  args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
  args[2].VertexBuffer.Slot = 31;
  args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
  args[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  constexpr UINT stride = constantCount * 4 + 8 + sizeof(D3D12_VERTEX_BUFFER_VIEW) +
                          sizeof(D3D12_INDEX_BUFFER_VIEW) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
  signatureDesc.ByteStride = stride;
  signatureDesc.NumArgumentDescs = 5;
  signatureDesc.pArgumentDescs = args;
  ComPtr<ID3D12CommandSignature> signature;
  check(context.device->CreateCommandSignature(&signatureDesc, root.Get(), IID_PPV_ARGS(&signature)),
        "msaa-indirect-signature");
  D3D12_INPUT_ELEMENT_DESC input = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 31, 0,
                                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = root.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.DepthStencilState.DepthEnable = FALSE;
  pso.InputLayout = {&input, 1};
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso.SampleDesc.Count = sampleCount;
  ComPtr<ID3D12PipelineState> pipeline;
  check(context.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)),
        "msaa-indirect-pipeline");
  ComPtr<ID3D12PipelineState> postGrowthPipeline;
  if (sampleCount == 1) {
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    check(context.device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&postGrowthPipeline)),
          "arena-post-growth-pipeline");
  }
  // Multisampled textures cannot be copied to a readback buffer directly.
  // Load every sample on the GPU, preserving the per-sample color evidence.
  static constexpr char readSource[] = R"(
    #if SAMPLE_COUNT == 1
    Texture2D<float4> color : register(t0);
    #else
    Texture2DMS<float4> color : register(t0);
    #endif
    RWStructuredBuffer<uint4> pixels : register(u0);
    [numthreads(4,2,1)] void main(uint3 id : SV_DispatchThreadID) {
      #if SAMPLE_COUNT == 1
        pixels[id.y*4 + id.x] = uint4(round(color.Load(int3(id.xy,0))*255));
      #else
      for (uint sample = 0; sample < SAMPLE_COUNT; ++sample)
        pixels[sample*8 + id.y*4 + id.x] = uint4(round(color.Load(int2(id.xy),sample)*255));
      #endif
    }
  )";
  ComPtr<ID3DBlob> cs;
  errors.Reset();
  const D3D_SHADER_MACRO macros[] = {{"SAMPLE_COUNT",sampleCount == 1 ? "1" : "4"},{nullptr,nullptr}};
  check(D3DCompile(readSource,sizeof(readSource)-1,nullptr,macros,nullptr,"main","cs_5_0",
                   D3DCOMPILE_ENABLE_STRICTNESS,0,&cs,&errors),"msaa-indirect-reader-compile");
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER readRoots[2] = {};
  readRoots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  readRoots[0].DescriptorTable.NumDescriptorRanges = 1;
  readRoots[0].DescriptorTable.pDescriptorRanges = &range;
  readRoots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  readRoots[1].Descriptor.ShaderRegister = 0;
  D3D12_ROOT_SIGNATURE_DESC readRootDesc = {};
  readRootDesc.NumParameters = 2;
  readRootDesc.pParameters = readRoots;
  ComPtr<ID3DBlob> readSerialized;
  check(D3D12SerializeRootSignature(&readRootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&readSerialized,&errors),
        "msaa-indirect-reader-root-serialize");
  ComPtr<ID3D12RootSignature> readRoot;
  check(context.device->CreateRootSignature(0,readSerialized->GetBufferPointer(),
        readSerialized->GetBufferSize(),IID_PPV_ARGS(&readRoot)),"msaa-indirect-reader-root");
  D3D12_COMPUTE_PIPELINE_STATE_DESC readPso = {};
  readPso.pRootSignature = readRoot.Get();
  readPso.CS = {cs->GetBufferPointer(),cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> readPipeline;
  check(context.device->CreateComputePipelineState(&readPso,IID_PPV_ARGS(&readPipeline)),
        "msaa-indirect-reader-pipeline");
  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
  srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvHeapDesc.NumDescriptors = 2;
  srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(context.device->CreateDescriptorHeap(&srvHeapDesc,IID_PPV_ARGS(&srvHeap)),
        "msaa-indirect-srv-heap");
  auto sampleCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
  auto sampleGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
  const UINT descriptorSize = context.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampleCpu.ptr += descriptorSize;
  sampleGpu.ptr += descriptorSize;
  auto blackUpload = createBuffer(context.device.Get(),256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  const std::array<uint8_t,256> blackBytes = {};
  writeUpload(blackUpload.Get(),blackBytes.data(),blackBytes.size());
  auto predicate = createBuffer(context.device.Get(),16,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  const UINT64 predicates[] = {1,0};
  writeUpload(predicate.Get(),predicates,sizeof(predicates));
  using Vertex = std::array<float,4>;
  // Both index buffers are valid. The opposite VB/IB combination selects an
  // offscreen triangle, so forwarding a stale view cannot masquerade as pass.
  const std::array<Vertex, 6> left = {{{{-1, -1, .5f, 1}}, {{-1, 3, .5f, 1}}, {{3, -1, .5f, 1}},
                                       {{3, -1, .5f, 1}}, {{3, 3, .5f, 1}}, {{4, -1, .5f, 1}}}};
  const std::array<Vertex, 6> right = {{{{3, -1, .5f, 1}}, {{3, 3, .5f, 1}}, {{4, -1, .5f, 1}},
                                        {{-1, -1, .5f, 1}}, {{-1, 3, .5f, 1}}, {{3, -1, .5f, 1}}}};
  const uint16_t indicesA[] = {0, 1, 2};
  const uint16_t indicesB[] = {3, 4, 5};
  auto vbA = createBuffer(context.device.Get(),sizeof(left),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto vbB = createBuffer(context.device.Get(),sizeof(right),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto ibA = createBuffer(context.device.Get(),sizeof(indicesA),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto ibB = createBuffer(context.device.Get(),sizeof(indicesB),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  writeUpload(vbA.Get(),left.data(),sizeof(left)); writeUpload(vbB.Get(),right.data(),sizeof(right));
  writeUpload(ibA.Get(),indicesA,sizeof(indicesA)); writeUpload(ibB.Get(),indicesB,sizeof(indicesB));
  const D3D12_VERTEX_BUFFER_VIEW viewA = {vbA->GetGPUVirtualAddress(),sizeof(left),sizeof(Vertex)};
  const D3D12_VERTEX_BUFFER_VIEW viewB = {vbB->GetGPUVirtualAddress(),sizeof(right),sizeof(Vertex)};
  const D3D12_INDEX_BUFFER_VIEW indexA = {ibA->GetGPUVirtualAddress(),sizeof(indicesA),DXGI_FORMAT_R16_UINT};
  const D3D12_INDEX_BUFFER_VIEW indexB = {ibB->GetGPUVirtualAddress(),sizeof(indicesB),DXGI_FORMAT_R16_UINT};
  D3D12_RESOURCE_DESC texture = {};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 4; texture.Height = 2; texture.DepthOrArraySize = 1; texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = sampleCount;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(context.device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"msaa-indirect-rtv-heap");
  for (uint32_t requested = 1; requested <= 2; ++requested) {
    ComPtr<ID3D12Resource> color;
    auto properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    check(context.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&texture,
          D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&color)),"msaa-indirect-color");
    auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    context.device->CreateRenderTargetView(color.Get(),nullptr,rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = texture.Format;
    srv.ViewDimension = sampleCount == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
    if (sampleCount == 1) srv.Texture2D.MipLevels = 1;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    context.device->CreateShaderResourceView(color.Get(),&srv,srvHeap->GetCPUDescriptorHandleForHeapStart());
    auto sampleDesc = texture;
    sampleDesc.Width = sampleDesc.Height = 1;
    sampleDesc.SampleDesc.Count = 1;
    sampleDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> sampleTexture;
    check(context.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&sampleDesc,
          D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&sampleTexture)),"msaa-indirect-sample-texture");
    D3D12_SHADER_RESOURCE_VIEW_DESC sampleSrv = {};
    sampleSrv.Format = sampleDesc.Format;
    sampleSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sampleSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sampleSrv.Texture2D.MipLevels = 1;
    context.device->CreateShaderResourceView(sampleTexture.Get(),&sampleSrv,sampleCpu);
    D3D12_QUERY_HEAP_DESC queryDesc = {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    queryDesc.Count = 1;
    ComPtr<ID3D12QueryHeap> query;
    check(context.device->CreateQueryHeap(&queryDesc,IID_PPV_ARGS(&query)),"msaa-indirect-query");
    auto statsReadback = createBuffer(context.device.Get(),sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                                     D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto image = createBuffer(context.device.Get(),32*16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto imageUav = createBuffer(context.device.Get(),32*16,D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto countReadback = createBuffer(context.device.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto first = createBuffer(context.device.Get(),8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto second = createBuffer(context.device.Get(),8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto zero = createBuffer(context.device.Get(),8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    const uint64_t zeros = 0; writeUpload(zero.Get(),&zeros,8);
    auto arguments = createBuffer(context.device.Get(),stride*commandCount,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto count = createBuffer(context.device.Get(),4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    std::vector<uint8_t> records(stride*commandCount);
    auto pack = [&](UINT n,uint32_t tint,D3D12_GPU_VIRTUAL_ADDRESS uav,
                    const D3D12_VERTEX_BUFFER_VIEW &vb,const D3D12_INDEX_BUFFER_VIEW &ib,UINT vertices) {
      uint8_t *p = records.data()+n*stride;
      const D3D12_DRAW_INDEXED_ARGUMENTS draw = {vertices,1,0,0,0};
      std::array<UINT,constantCount> constants;
      for (UINT i = 0; i < constantCount; ++i) constants[i] = tint+i;
      std::memcpy(p,constants.data(),sizeof(constants)); p+=sizeof(constants);
      std::memcpy(p,&uav,8); p+=8;
      std::memcpy(p,&vb,sizeof(vb)); p+=sizeof(vb);
      std::memcpy(p,&ib,sizeof(ib)); p+=sizeof(ib);
      std::memcpy(p,&draw,sizeof(draw));
    };
    for (UINT i = 0; i < commandCount; ++i)
      pack(i,1,first->GetGPUVirtualAddress(),viewA,indexA,i == 0 ? 3 : 0);
    writeUpload(arguments.Get(),records.data(),records.size());
    const UINT one = 1; writeUpload(count.Get(),&one,sizeof(one));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),
          "msaa-indirect-allocator");
    check(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),
                                            IID_PPV_ARGS(&list)),"msaa-indirect-list");
    D3D12_TEXTURE_COPY_LOCATION sampleSource = {};
    sampleSource.pResource = blackUpload.Get();
    sampleSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sampleSource.PlacedFootprint.Footprint = {sampleDesc.Format,1,1,1,256};
    D3D12_TEXTURE_COPY_LOCATION sampleDestination = {};
    sampleDestination.pResource = sampleTexture.Get();
    sampleDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&sampleDestination,0,0,0,&sampleSource,nullptr);
    auto sampleBarrier = transition(sampleTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1,&sampleBarrier);
    list->CopyBufferRegion(first.Get(),0,zero.Get(),0,8);
    list->CopyBufferRegion(second.Get(),0,zero.Get(),0,8);
    D3D12_RESOURCE_BARRIER begin[] = {
      transition(first.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
      transition(second.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2,begin);
    const float clear[4] = {0,0,0,0};
    list->ClearRenderTargetView(rtv,clear,0,nullptr);
    list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT viewport = {0,0,4,2,0,1}; D3D12_RECT scissor = {0,0,4,2};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
    list->SetDescriptorHeaps(1,heaps);
    list->SetGraphicsRootSignature(root.Get());
    list->SetGraphicsRootDescriptorTable(2,sampleGpu);
    list->SetGraphicsRootUnorderedAccessView(0,second->GetGPUVirtualAddress());
    list->SetGraphicsRoot32BitConstant(1,3,0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(31,1,&viewA); list->IASetIndexBuffer(&indexA);
    // Prime the cached counter allocation, but leave no query active on
    // the large ICB draw: active statistics select the execution-owned path.
    list->BeginQuery(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
    if (sampleCount == 1)
      list->EndQuery(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
    list->SetPredication(predicate.Get(),0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    list->ExecuteIndirect(signature.Get(),commandCount,arguments.Get(),0,count.Get(),0);
    list->SetGraphicsRootUnorderedAccessView(0,second->GetGPUVirtualAddress());
    std::array<UINT,constantCount> forbidden;
    for (UINT i = 0; i < constantCount; ++i) forbidden[i] = 3+i;
    list->SetGraphicsRoot32BitConstants(1,constantCount,forbidden.data(),0);
    list->IASetVertexBuffers(31,1,&viewB); list->IASetIndexBuffer(&indexB);
    if (sampleCount == 1) {
      list->SetPipelineState(postGrowthPipeline.Get());
      list->BeginQuery(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
      // The direct draw reuses the original counter chunk after root/VB
      // growth. Disable color writes so it cannot hide indirect root errors.
      list->DrawIndexedInstanced(3,1,0,0,0);
    }
    list->SetPredication(predicate.Get(),8,D3D12_PREDICATION_OP_EQUAL_ZERO);
    list->DrawIndexedInstanced(3,1,0,0,0);
    list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    list->EndQuery(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
    list->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,statsReadback.Get(),0);
    D3D12_RESOURCE_BARRIER end[] = {
      transition(first.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(second.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(3,end);
    list->CopyBufferRegion(countReadback.Get(),0,first.Get(),0,8);
    list->CopyBufferRegion(countReadback.Get(),8,second.Get(),0,8);
    list->SetPipelineState(readPipeline.Get());
    list->SetComputeRootSignature(readRoot.Get());
    list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootDescriptorTable(0,srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1,imageUav->GetGPUVirtualAddress());
    list->Dispatch(1,1,1);
    auto imageBarrier = transition(imageUav.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1,&imageBarrier);
    list->CopyBufferRegion(image.Get(),0,imageUav.Get(),0,32*16);
    check(list->Close(), "close-msaa-indirect-list");
    // The second queue submission must observe data changed after Close,
    // not the initial record/count copied by any recording-time resolver.
    if (requested == 2) {
      pack(commandCount-1,2,second->GetGPUVirtualAddress(),viewB,indexB,3);
      writeUpload(arguments.Get(),records.data(),records.size());
      writeUpload(count.Get(),&commandCount,sizeof(commandCount));
    }
    context.submitClosed(list.Get());
    D3D12_QUERY_DATA_PIPELINE_STATISTICS *stats = nullptr;
    check(statsReadback->Map(0,nullptr,reinterpret_cast<void **>(&stats)),"msaa-indirect-stats-map");
    const UINT queriedDraws = sampleCount == 1 ? 1 : requested;
    require(stats->IAVertices == queriedDraws*3 && stats->IAPrimitives == queriedDraws &&
            stats->VSInvocations == queriedDraws*3 && stats->PSInvocations == queriedDraws*8,
            "msaa-indirect-cross-chunk-cached-statistics");
    statsReadback->Unmap(0,nullptr);
    uint32_t *observed = nullptr;
    check(countReadback->Map(0,nullptr,reinterpret_cast<void **>(&observed)),"msaa-indirect-count-map");
    require(observed[0] == 3 && observed[1] == 8,"msaa-indirect-first-exactly-once");
    const UINT secondDraws = (requested == 2 ? 1u : 0u) + (sampleCount == 1 ? 1u : 0u);
    require(observed[2] == secondDraws*3 && observed[3] == secondDraws*8,
            "msaa-indirect-second-exactly-once");
    countReadback->Unmap(0,nullptr);
    uint32_t *pixels = nullptr;
    check(image->Map(0,nullptr,reinterpret_cast<void **>(&pixels)),"msaa-indirect-color-map");
    for (UINT sample = 0; sample < sampleCount; ++sample)
      for (UINT y = 0; y < 2; ++y)
        for (UINT x = 0; x < 4; ++x) {
          const uint32_t *pixel = pixels+4*(sample*8+y*4+x);
          uint8_t expected = requested == 2 ? 128 : 64;
          require(pixel[0] == expected && pixel[1] == 0 && pixel[2] == 0 &&
                  pixel[3] == (expected ? 255 : 0),"msaa-indirect-sample-color");
        }
    image->Unmap(0,nullptr);
  }
  if (sampleCount == 4)
    std::puts("OWNED_D3D12_PREDICATION_INDIRECT_MSAA_INDEXED_MUTATIONS_PASS counts=1,2 samples=4");
  else
    std::printf("OWNED_D3D12_PREDICATION_INDIRECT_ARENA_BINDINGS_PASS commands=%u "
                "first_last=1 static_sampler=1 vertex_slot=31 cached_statistics_after_growth=1 predicate_skip=1\n",commandCount);
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
  RunBufferUavRectangleTests(context);
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_CLEAR_CASES_PASS");
  RunIndirectTests(context, computeShader, vertexShader);
  RunPreparedComputeIndirect(context, computeShader);
  RunSimpleGraphicsIndirect(context);
  RunMSAAIndexedIndirectMutations(context,4);
  RunMSAAIndexedIndirectMutations(context,1);
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_INDIRECT_CASES_PASS");
  std::puts("OWNED_D3D12_PREDICATION_INDIRECT_PASS");
  return 0;
}
