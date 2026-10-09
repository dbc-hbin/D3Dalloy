#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <vector>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

struct HeapLifetimeMarker final : IUnknown {
  explicit HeapLifetimeMarker(bool* released) : released_(released) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
    *output = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = InterlockedDecrement(&references_);
    if (!remaining) {
      *released_ = true;
      delete this;
    }
    return remaining;
  }
private:
  ~HeapLifetimeMarker() = default;
  LONG references_ = 1;
  bool* released_;
};

constexpr GUID kHeapLifetimeMarker = {0x9ae5fdc8, 0x5d30, 0x4bf1,
                                      {0xa3, 0x38, 0xab, 0x31, 0x0f, 0x90, 0x59, 0xc2}};

[[noreturn]] void fail(const char* step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_RESOURCES_FAIL step=%s hr=0x%08lx\n",
               step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char* step) {
  if (FAILED(hr))
    fail(step, hr);
}

void expect(HRESULT actual, HRESULT expected, const char* step) {
  if (actual != expected) {
    std::fprintf(stderr,
                 "OWNED_D3D12_RESOURCES_HRESULT_MISMATCH step=%s expected=0x%08lx actual=0x%08lx\n",
                 step, static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    ExitProcess(1);
  }
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  if (!module)
    fail("GetModuleHandleW(d3d12)", HRESULT_FROM_WIN32(GetLastError()));
  FARPROC raw = GetProcAddress(module, "OwnedGraphicsBackendId");
  if (!raw)
    fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
  using BackendId = const char* (WINAPI*)();
  const auto backendId = reinterpret_cast<BackendId>(raw);
  const char* id = backendId();
  if (!id || std::strcmp(id, "owned-graphics-d3d12") != 0)
    fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_RESOURCES_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES defaultHeap() {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = D3D12_HEAP_TYPE_DEFAULT;
  props.CreationNodeMask = 1;
  props.VisibleNodeMask = 1;
  return props;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 width) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = width;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

D3D12_RESOURCE_DESC texture2DDesc(DXGI_FORMAT format, UINT width, UINT height, UINT16 arraySize,
                                  UINT16 mipLevels, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = arraySize;
  desc.MipLevels = mipLevels;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

void testResourceContracts(ID3D12Device* device) {
  const auto props = defaultHeap();
  auto buffer = bufferDesc(4096);

  // Validation-only creation must not allocate and must still reject malformed input.
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         S_FALSE, "validation-only buffer");
  auto badBuffer = buffer;
  badBuffer.Height = 2;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &badBuffer,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "invalid buffer shape");
  D3D12_CLEAR_VALUE bufferClear{};
  bufferClear.Format = DXGI_FORMAT_R32_UINT;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer,
                                         D3D12_RESOURCE_STATE_COMMON, &bufferClear,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "buffer optimized clear");

  auto color = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 3, 2, 1, 1,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &color,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                         __uuidof(ID3D12Resource), nullptr),
         S_FALSE, "validation-only optimized clear");
  auto invalidAlignment = color;
  invalidAlignment.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &invalidAlignment,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "non-MSAA RT rejects small alignment in validation-only create");
  clear.Format = DXGI_FORMAT_R32_FLOAT;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &color,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "incompatible optimized clear");

  auto plainTexture = color;
  plainTexture.Flags = D3D12_RESOURCE_FLAG_NONE;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &plainTexture,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "render-target state without capability");
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &plainTexture,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "depth-write state without capability");
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &plainTexture,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "UAV state without capability");
  auto invalidBufferState = bufferDesc(4096);
  ID3D12Resource* invalidStateResource = reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &invalidBufferState,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                         __uuidof(ID3D12Resource), reinterpret_cast<void**>(&invalidStateResource)),
         E_INVALIDARG, "buffer render-target state");
  if (invalidStateResource)
    fail("invalid initial state output not cleared");
  invalidBufferState.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &invalidBufferState,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         S_FALSE, "buffer RTV initial state with capability");

  auto overlimitBuffer = bufferDesc(~UINT64(0));
  if (device->GetResourceAllocationInfo(0, 1, &overlimitBuffer).SizeInBytes != ~UINT64(0))
    fail("overlimit buffer allocation accepted");
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &overlimitBuffer,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "overlimit buffer create");

  D3D12_HEAP_DESC invalidCustomHeap{};
  invalidCustomHeap.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  invalidCustomHeap.Properties = props;
  invalidCustomHeap.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
  invalidCustomHeap.Properties.CPUPageProperty = static_cast<D3D12_CPU_PAGE_PROPERTY>(0x7fff);
  invalidCustomHeap.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  expect(device->CreateHeap(&invalidCustomHeap, __uuidof(ID3D12Heap), nullptr),
         E_INVALIDARG, "invalid custom CPU page property");
  invalidCustomHeap.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  invalidCustomHeap.Properties.MemoryPoolPreference = static_cast<D3D12_MEMORY_POOL>(0x7fff);
  expect(device->CreateHeap(&invalidCustomHeap, __uuidof(ID3D12Heap), nullptr),
         E_INVALIDARG, "invalid custom memory pool");

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0;
  UINT64 totalBytes = 0;
  device->GetCopyableFootprints(&color, 0, 1, 7, &footprint, &rows, &rowBytes, &totalBytes);
  if (footprint.Offset != D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
      footprint.Footprint.RowPitch != D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ||
      rows != 2 || rowBytes != 12 || totalBytes != 773)
    fail("GetCopyableFootprints alignment");

  auto invalid = color;
  invalid.Width = 0;
  const auto invalidInfo = device->GetResourceAllocationInfo(0, 1, &invalid);
  if (invalidInfo.SizeInBytes != ~0ull || invalidInfo.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
    fail("GetResourceAllocationInfo invalid sentinel");
  const auto emptyInfo = device->GetResourceAllocationInfo(0, 0, nullptr);
  if (emptyInfo.SizeInBytes || emptyInfo.Alignment)
    fail("GetResourceAllocationInfo empty");

  auto excessiveMips = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 4,
                                     D3D12_RESOURCE_FLAG_NONE);
  if (device->GetResourceAllocationInfo(0, 1, &excessiveMips).SizeInBytes != ~UINT64(0))
    fail("excess mip allocation accepted");
  ID3D12Resource* impossible = reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
  if (device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &excessiveMips,
                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&impossible)) != E_INVALIDARG ||
      impossible)
    fail("excess mip texture accepted");
  excessiveMips.MipLevels = 0;
  ComPtr<ID3D12Resource> fullChain;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &excessiveMips,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&fullChain)), "full mip chain create");
  if (fullChain->GetDesc().MipLevels != 3)
    fail("full mip chain normalized incorrectly");

  auto oversized = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1,
                                 D3D12_RESOURCE_FLAG_NONE);
  auto rejectOversized = [&](const char* label) {
    if (device->GetResourceAllocationInfo(0, 1, &oversized).SizeInBytes != ~UINT64(0))
      fail(label);
    ID3D12Resource* unexpected = reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
    expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &oversized,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           __uuidof(ID3D12Resource), reinterpret_cast<void**>(&unexpected)),
           E_INVALIDARG, label);
    if (unexpected)
      fail("oversized resource output not cleared");
  };
  oversized.Width = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1;
  rejectOversized("oversized 2D width");
  oversized.Width = ~UINT64(0);
  rejectOversized("overflowing 2D width");
  oversized.Width = 4;
  oversized.Height = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1;
  rejectOversized("oversized 2D height");
  oversized.Height = 4;
  oversized.DepthOrArraySize = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION + 1;
  rejectOversized("oversized 2D array");
  oversized.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  rejectOversized("oversized 3D depth");
  oversized.DepthOrArraySize = 1;
  oversized.Width = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION + 1;
  rejectOversized("oversized 3D width");
  oversized.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
  oversized.Width = D3D12_REQ_TEXTURE1D_U_DIMENSION + 1;
  oversized.Height = 1;
  rejectOversized("oversized 1D width");
  oversized.Width = 4;
  oversized.DepthOrArraySize = D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION + 1;
  rejectOversized("oversized 1D array");

  auto crossAdapter = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1,
                                    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
  crossAdapter.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // Row-major cross-adapter textures are sized (linear, 256-byte pitch) but need a
  // SHARED_CROSS_ADAPTER heap; the sharing suite verifies the backed path.
  const auto crossInfo = device->GetResourceAllocationInfo(0, 1, &crossAdapter);
  if (crossInfo.SizeInBytes != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT ||
      crossInfo.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
    fail("row-major cross-adapter texture allocation info");
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &crossAdapter,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "unbacked row-major cross-adapter texture");
  crossAdapter.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &crossAdapter,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "unbacked cross-adapter texture");
}

void testPlacedAlignment(ID3D12Device* device) {
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = 4ull * 1024ull * 1024ull;
  heapDesc.Properties = defaultHeap();
  heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  auto invalidDisplay = heapDesc;
  invalidDisplay.Flags = D3D12_HEAP_FLAG_ALLOW_DISPLAY;
  expect(device->CreateHeap(&invalidDisplay, __uuidof(ID3D12Heap), nullptr),
         E_INVALIDARG, "display heap capability probe");
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
  ComPtr<ID3D12Heap> heap;
  check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateHeap");

  ComPtr<ID3D12Device> otherDevice;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice)),
        "CreateDevice(other heap owner)");
  if (otherDevice.Get() == device)
    fail("distinct device instances");
  auto crossDeviceBuffer = bufferDesc(4096);
  ID3D12Resource* unexpected = reinterpret_cast<ID3D12Resource*>(static_cast<UINT_PTR>(1));
  expect(otherDevice->CreatePlacedResource(heap.Get(), 0, &crossDeviceBuffer,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           __uuidof(ID3D12Resource), reinterpret_cast<void**>(&unexpected)),
         E_INVALIDARG, "cross-device heap");
  if (unexpected)
    fail("cross-device heap output not cleared");
  expect(otherDevice->CreatePlacedResource(heap.Get(), 0, &crossDeviceBuffer,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "cross-device heap capability probe");

  const auto buffer = bufferDesc(64 * 1024);
  ComPtr<ID3D12Resource> resource;
  expect(device->CreatePlacedResource(heap.Get(), 4096, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                      nullptr, __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "misaligned placed buffer capability probe");
  expect(device->CreatePlacedResource(heap.Get(), 4096, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                      nullptr, IID_PPV_ARGS(&resource)),
         E_INVALIDARG, "misaligned placed buffer");
  check(device->CreatePlacedResource(heap.Get(), D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                                     &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&resource)),
        "aligned placed buffer");

  const auto texture = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1,
                                     D3D12_RESOURCE_FLAG_NONE);
  const auto info = device->GetResourceAllocationInfo(0, 1, &texture);
  if (!info.SizeInBytes || info.SizeInBytes == ~0ull || !info.Alignment)
    fail("GetResourceAllocationInfo(texture)");
  resource.Reset();
  expect(device->CreatePlacedResource(heap.Get(), 4096, &texture, D3D12_RESOURCE_STATE_COMMON,
                                      nullptr, IID_PPV_ARGS(&resource)),
         E_INVALIDARG, "misaligned placed texture");
  check(device->CreatePlacedResource(heap.Get(), 0, &texture, D3D12_RESOURCE_STATE_COMMON,
                                     nullptr, IID_PPV_ARGS(&resource)),
        "aligned placed texture");
}

void testAllocationInfoPlacement(ID3D12Device* device) {
  ComPtr<ID3D12Device4> device4;
  check(device->QueryInterface(IID_PPV_ARGS(&device4)), "ID3D12Device4 allocation query");
  const D3D12_RESOURCE_DESC descs[2] = {
      bufferDesc(4096), texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1,
                                      D3D12_RESOURCE_FLAG_NONE)};
  D3D12_RESOURCE_ALLOCATION_INFO1 offsets[2]{};
  const auto allocation = device4->GetResourceAllocationInfo1(0, 2, descs, offsets);
  if (allocation.SizeInBytes == ~UINT64(0) || !allocation.SizeInBytes ||
      !offsets[0].SizeInBytes || !offsets[1].SizeInBytes ||
      !offsets[0].Alignment || !offsets[1].Alignment ||
      offsets[0].Offset % offsets[0].Alignment ||
      offsets[1].Offset % offsets[1].Alignment ||
      offsets[1].Offset < offsets[0].Offset + offsets[0].SizeInBytes ||
      allocation.SizeInBytes < offsets[1].Offset + offsets[1].SizeInBytes)
    fail("GetResourceAllocationInfo1 invalid placement range");
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = allocation.SizeInBytes;
  heapDesc.Alignment = allocation.Alignment;
  heapDesc.Properties = defaultHeap();
  ComPtr<ID3D12Heap> heap;
  check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateHeap allocation range");
  ComPtr<ID3D12Resource> placedBuffer, placedTexture;
  check(device->CreatePlacedResource(heap.Get(), offsets[0].Offset, &descs[0],
                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&placedBuffer)), "allocation buffer placement");
  check(device->CreatePlacedResource(heap.Get(), offsets[1].Offset, &descs[1],
                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&placedTexture)), "allocation texture placement");
  UINT8 source[64]{};
  UINT8 destination[64]{};
  for (UINT i = 0; i < 64; ++i) source[i] = static_cast<UINT8>(i ^ 0xa5);
  check(placedTexture->WriteToSubresource(0, nullptr, source, 16, 64),
        "allocation placed texture write");
  check(placedTexture->ReadFromSubresource(destination, 16, 64, 0, nullptr),
        "allocation placed texture read");
  if (std::memcmp(source, destination, 64)) fail("allocation placed texture contents");
}

void testPlacedHeapLifetime(ID3D12Device* device) {
  const auto props = defaultHeap();
  const auto buffer = bufferDesc(4096);
  const auto texture = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1,
                                     D3D12_RESOURCE_FLAG_NONE);
  for (UINT kind = 0; kind < 2; ++kind) {
    const auto& desc = kind ? texture : buffer;
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = 4ull * 1024ull * 1024ull;
    heapDesc.Properties = props;
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateHeap(lifetime)");
    UINT size = 1, value = 0x12345678, readback = 0;
    expect(heap->GetPrivateData(kHeapLifetimeMarker, &size, nullptr), DXGI_ERROR_NOT_FOUND, "private missing");
    if (size != 0) fail("private missing size");
    expect(heap->SetPrivateData(kHeapLifetimeMarker, 0, nullptr), S_FALSE, "private erase missing");
    check(heap->SetPrivateData(kHeapLifetimeMarker, sizeof(value), &value), "private bytes");
    expect(heap->GetPrivateData(kHeapLifetimeMarker, nullptr, &readback), E_INVALIDARG, "private null size");
    size = 1;
    expect(heap->GetPrivateData(kHeapLifetimeMarker, &size, &readback), DXGI_ERROR_MORE_DATA, "private short buffer");
    if (size != sizeof(value) || readback != 0) fail("private short buffer contents");
    check(heap->GetPrivateData(kHeapLifetimeMarker, &size, &readback), "private readback");
    if (readback != value) fail("private bytes mismatch");
    bool queryReleased = false;
    auto* queryMarker = new HeapLifetimeMarker(&queryReleased);
    check(heap->SetPrivateDataInterface(kHeapLifetimeMarker, queryMarker), "private replace bytes");
    queryMarker->Release();
    size = 0;
    check(heap->GetPrivateData(kHeapLifetimeMarker, &size, nullptr), "private interface size");
    if (size != sizeof(IUnknown*)) fail("private interface size mismatch");
    IUnknown* queried = nullptr;
    size = sizeof(queried) - 1;
    expect(heap->GetPrivateData(kHeapLifetimeMarker, &size, &queried), DXGI_ERROR_MORE_DATA, "private short interface");
    if (queried || size != sizeof(queried)) fail("private short interface contents");
    check(heap->GetPrivateData(kHeapLifetimeMarker, &size, &queried), "private interface readback");
    if (queried != queryMarker) fail("private interface identity");
    check(heap->SetPrivateData(kHeapLifetimeMarker, sizeof(value), &value), "private replace interface");
    if (queryReleased) fail("private interface missing returned reference");
    queried->Release();
    if (!queryReleased) fail("private interface leaked reference");
    check(heap->SetPrivateData(kHeapLifetimeMarker, 0, nullptr), "private erase");
    bool released = false;
    auto* marker = new HeapLifetimeMarker(&released);
    const HRESULT markerHr = heap->SetPrivateDataInterface(kHeapLifetimeMarker, marker);
    marker->Release();
    check(markerHr, "SetPrivateDataInterface(lifetime)");
    ComPtr<ID3D12Resource> resource;
    check(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON,
                                       nullptr, IID_PPV_ARGS(&resource)),
          "CreatePlacedResource(lifetime)");
    heap.Reset();
    if (released)
      fail("placed resource did not retain heap");
    resource.Reset();
    if (!released)
      fail("placed resource leaked heap");
  }
}

void testSmallMsaaAlignment(ID3D12Device* device) {
  const auto props = defaultHeap();
  auto desc = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 4, 1,
                            D3D12_RESOURCE_FLAG_NONE);
  desc.SampleDesc.Count = 4;
  desc.Alignment = D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
  auto info = device->GetResourceAllocationInfo(0, 1, &desc);
  if (info.SizeInBytes == ~UINT64(0) || info.Alignment != desc.Alignment)
    fail("eligible small MSAA array allocation");
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
        "eligible small MSAA array create");
  resource.Reset();

  desc.Width = desc.Height = 1024;
  info = device->GetResourceAllocationInfo(0, 1, &desc);
  if (info.SizeInBytes != ~UINT64(0))
    fail("oversized small MSAA allocation accepted");
  ID3D12Resource* invalid = reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
  if (device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&invalid)) != E_INVALIDARG || invalid)
    fail("oversized small MSAA create accepted");

  desc.Width = desc.Height = 4;
  desc.DepthOrArraySize = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  info = device->GetResourceAllocationInfo(0, 1, &desc);
  if (info.SizeInBytes == ~UINT64(0) || info.Alignment != desc.Alignment)
    fail("small MSAA render target allocation");
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
        "small MSAA render target create");

  desc.Width = desc.Height = 512;
  info = device->GetResourceAllocationInfo(0, 1, &desc);
  if (info.SizeInBytes == ~UINT64(0))
    fail("small MSAA tile boundary rejected");
  desc.Width = 513;
  if (device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes != ~UINT64(0))
    fail("small MSAA over tile boundary accepted");

  desc = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, 1, 1,
                       D3D12_RESOURCE_FLAG_NONE);
  desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  info = device->GetResourceAllocationInfo(0, 1, &desc);
  if (info.SizeInBytes == ~UINT64(0) || info.Alignment != desc.Alignment)
    fail("small 2D tile boundary rejected");
  desc.Width = 129;
  if (device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes != ~UINT64(0))
    fail("small 2D over tile boundary accepted");

  desc = texture2DDesc(DXGI_FORMAT_R8G8_B8G8_UNORM, 1024, 32, 1, 1,
                       D3D12_RESOURCE_FLAG_NONE);
  desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  info = device->GetResourceAllocationInfo(0, 1, &desc);
  if (info.SizeInBytes == ~UINT64(0) || info.Alignment != desc.Alignment)
    fail("small packed 422 tile boundary rejected");
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
        "small packed 422 create");
  resource.Reset();
  desc.Width = 1026;
  if (device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes != ~UINT64(0))
    fail("small packed 422 over tile boundary accepted");
}

void testCpuTextureTransfers(ID3D12Device* device) {
  const auto desc = texture2DDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1,
                                  D3D12_RESOURCE_FLAG_NONE);
  const auto props = defaultHeap();
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)),
        "CreateCommittedResource(CPU texture)");
  UINT8 source[64]{};
  for (UINT i = 0; i < sizeof(source); ++i)
    source[i] = static_cast<UINT8>(i ^ 0xa5);
  check(texture->WriteToSubresource(0, nullptr, source, 16, 64), "CPU texture write");
  UINT8 destination[64]{};
  check(texture->ReadFromSubresource(destination, 16, 64, 0, nullptr), "CPU texture read");
  if (std::memcmp(source, destination, sizeof(source)))
    fail("CPU texture round trip");

  expect(texture->WriteToSubresource(0, nullptr, source, 1, 4), E_INVALIDARG,
         "CPU texture undersized write pitch");
  expect(texture->ReadFromSubresource(destination, 1, 4, 0, nullptr), E_INVALIDARG,
         "CPU texture undersized read pitch");
  expect(texture->WriteToSubresource(0, nullptr, nullptr, 16, 64), E_INVALIDARG,
         "CPU texture null write data");
  expect(texture->ReadFromSubresource(nullptr, 16, 64, 0, nullptr), E_INVALIDARG,
         "CPU texture null read data");
  D3D12_BOX inverted{3, 0, 0, 2, 1, 1};
  expect(texture->WriteToSubresource(0, &inverted, nullptr, 0, 0), S_OK,
         "CPU texture empty write box");
  expect(texture->ReadFromSubresource(nullptr, 0, 0, 0, &inverted), S_OK,
         "CPU texture empty read box");
  expect(texture->ReadFromSubresource(destination, 16, 64, 1, nullptr), E_INVALIDARG,
         "CPU texture invalid subresource");

  UINT8 unalignedSource[68]{};
  UINT8 unalignedDestination[68]{};
  for (UINT row = 0; row < 4; ++row)
    std::memcpy(unalignedSource + row * 17, source + row * 16, 16);
  check(texture->WriteToSubresource(0, nullptr, unalignedSource, 17, 68),
        "CPU texture unaligned write pitch");
  check(texture->ReadFromSubresource(unalignedDestination, 17, 68, 0, nullptr),
        "CPU texture unaligned read pitch");
  for (UINT row = 0; row < 4; ++row)
    if (std::memcmp(unalignedSource + row * 17, unalignedDestination + row * 17, 16))
      fail("CPU texture unaligned pitch round trip");

  D3D12_RESOURCE_DESC volumeDesc = desc;
  volumeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  volumeDesc.Width = volumeDesc.Height = volumeDesc.DepthOrArraySize = 2;
  ComPtr<ID3D12Resource> volume;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &volumeDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&volume)),
        "CreateCommittedResource(CPU volume)");
  UINT8 volumeSource[40]{};
  UINT8 volumeDestination[40]{};
  for (UINT z = 0; z < 2; ++z)
    for (UINT row = 0; row < 2; ++row)
      for (UINT byte = 0; byte < 8; ++byte)
        volumeSource[z * 19 + row * 9 + byte] = static_cast<UINT8>(z * 32 + row * 8 + byte);
  check(volume->WriteToSubresource(0, nullptr, volumeSource, 9, 19),
        "CPU volume unaligned slice write");
  check(volume->ReadFromSubresource(volumeDestination, 9, 19, 0, nullptr),
        "CPU volume unaligned slice read");
  for (UINT z = 0; z < 2; ++z)
    for (UINT row = 0; row < 2; ++row)
      if (std::memcmp(volumeSource + z * 19 + row * 9,
                      volumeDestination + z * 19 + row * 9, 8))
        fail("CPU volume unaligned slice round trip");

  const auto bcDesc = texture2DDesc(DXGI_FORMAT_BC1_UNORM, 2, 2, 1, 1,
                                    D3D12_RESOURCE_FLAG_NONE);
  ComPtr<ID3D12Resource> bcTail;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &bcDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bcTail)),
        "CreateCommittedResource(BC CPU tail)");
  UINT8 block[8]{1, 2, 3, 4, 5, 6, 7, 8};
  check(bcTail->WriteToSubresource(0, nullptr, block, 8, 8), "BC CPU tail write");
  UINT8 copied[8]{};
  check(bcTail->ReadFromSubresource(copied, 8, 8, 0, nullptr), "BC CPU tail read");
  if (std::memcmp(block, copied, sizeof(block)))
    fail("BC CPU tail round trip");
}

void testTypelessDepthViews(ID3D12Device* device) {
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  clear.DepthStencil.Depth = 1.0f;
  const auto desc = texture2DDesc(DXGI_FORMAT_R32G8X24_TYPELESS, 64, 64, 2, 3,
                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
  ComPtr<ID3D12Resource> texture;
  const auto heapProps = defaultHeap();
  check(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                        IID_PPV_ARGS(&texture)),
        "CreateCommittedResource(typeless depth)");

  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
  srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvHeapDesc.NumDescriptors = 2;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)), "CreateDescriptorHeap(SRV)");
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();

  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Texture2DArray.MostDetailedMip = 1;
  srv.Texture2DArray.MipLevels = 2;
  srv.Texture2DArray.FirstArraySlice = 0;
  srv.Texture2DArray.ArraySize = 2;
  srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  srv.Texture2DArray.PlaneSlice = 0;
  device->CreateShaderResourceView(texture.Get(), &srv, handle);
  handle.ptr += stride;
  srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
  srv.Texture2DArray.PlaneSlice = 1;
  device->CreateShaderResourceView(texture.Get(), &srv, handle);

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap(DSV)");
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
  dsv.Texture2DArray.MipSlice = 1;
  dsv.Texture2DArray.FirstArraySlice = 1;
  dsv.Texture2DArray.ArraySize = 1;
  device->CreateDepthStencilView(texture.Get(), &dsv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void testBufferDescriptors(ID3D12Device* device) {
  const auto props = defaultHeap();
  auto desc = bufferDesc(64 * 1024);
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> buffer;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                        IID_PPV_ARGS(&buffer)),
        "CreateCommittedResource(descriptor buffer)");

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 6;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)),
        "CreateDescriptorHeap(buffer descriptors)");
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();

  D3D12_SHADER_RESOURCE_VIEW_DESC typed{};
  typed.Format = DXGI_FORMAT_R32_UINT;
  typed.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  typed.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  typed.Buffer.FirstElement = 0;
  typed.Buffer.NumElements = 16;
  device->CreateShaderResourceView(buffer.Get(), &typed, handle);

  handle.ptr += stride;
  D3D12_SHADER_RESOURCE_VIEW_DESC structured{};
  structured.Format = DXGI_FORMAT_UNKNOWN;
  structured.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  structured.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  structured.Buffer.FirstElement = 1;
  structured.Buffer.NumElements = 8;
  structured.Buffer.StructureByteStride = 16;
  device->CreateShaderResourceView(buffer.Get(), &structured, handle);

  handle.ptr += stride;
  D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
  raw.Format = DXGI_FORMAT_R32_TYPELESS;
  raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  raw.Buffer.FirstElement = 2;
  raw.Buffer.NumElements = 8;
  raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(buffer.Get(), &raw, handle);

  handle.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_UNKNOWN;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.FirstElement = 0;
  uav.Buffer.NumElements = 8;
  uav.Buffer.StructureByteStride = 16;
  device->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav, handle);

  handle.ptr += stride;
  D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
  nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nullSrv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nullSrv, handle);

  handle.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
  nullUav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, handle);
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}

struct CopyContext {
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 value = 0;

  explicit CopyContext(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue(BC1)");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator(BC1)");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&list)),
          "CreateCommandList(BC1)");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence(BC1)");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      fail("CreateEventW(BC1)", HRESULT_FROM_WIN32(GetLastError()));
  }

  ~CopyContext() {
    if (event)
      CloseHandle(event);
  }

  void submit() {
    check(list->Close(), "Close(BC1)");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 signal = ++value;
    check(queue->Signal(fence.Get(), signal), "Signal(BC1)");
    if (fence->GetCompletedValue() < signal) {
      check(fence->SetEventOnCompletion(signal, event), "SetEventOnCompletion(BC1)");
      if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
        fail("BC1 copy fence timeout");
    }
    check(allocator->Reset(), "Reset(BC1 allocator)");
    check(list->Reset(allocator.Get(), nullptr), "Reset(BC1 list)");
  }
};

ComPtr<ID3D12Resource> createHostBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heap,
                                       D3D12_RESOURCE_STATES state, const char* step) {
  auto props = defaultHeap();
  props.Type = heap;
  const auto desc = bufferDesc(bytes);
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                       IID_PPV_ARGS(&resource)),
        step);
  return resource;
}


void testOverlappingPlacedResourceAliasing(ID3D12Device* device) {
  constexpr UINT64 bytes = 64 * 1024;
  const auto description = bufferDesc(bytes);
  const auto allocation = device->GetResourceAllocationInfo(0, 1, &description);
  if (!allocation.SizeInBytes || allocation.SizeInBytes == ~0ull || !allocation.Alignment)
    fail("GetResourceAllocationInfo(alias buffers)");

  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = allocation.SizeInBytes;
  heapDesc.Properties = defaultHeap();
  heapDesc.Alignment = allocation.Alignment;
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ComPtr<ID3D12Heap> heap;
  check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateHeap(alias buffers)");

  ComPtr<ID3D12Resource> first;
  ComPtr<ID3D12Resource> second;
  check(device->CreatePlacedResource(heap.Get(), 0, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&first)), "CreatePlacedResource(alias first)");
  check(device->CreatePlacedResource(heap.Get(), 0, &description, D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&second)), "CreatePlacedResource(alias second)");
  bool heapReleased = false;
  auto* marker = new HeapLifetimeMarker(&heapReleased);
  const HRESULT markerHr = heap->SetPrivateDataInterface(kHeapLifetimeMarker, marker);
  marker->Release();
  check(markerHr, "SetPrivateDataInterface(alias heap)");
  heap.Reset();
  if (heapReleased) fail("alias heap destroyed before GPU use");

  auto firstUpload = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ,
                                      "CreateCommittedResource(alias first upload)");
  auto secondUpload = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                       "CreateCommittedResource(alias second upload)");
  auto readback = createHostBuffer(device, bytes * 4, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   "CreateCommittedResource(alias readback)");
  UINT8* mapped = nullptr;
  check(firstUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map(alias first upload)");
  for (UINT64 i = 0; i < bytes; ++i)
    mapped[i] = static_cast<UINT8>((i * 17 + 0x21) & 0xff);
  firstUpload->Unmap(0, nullptr);
  check(secondUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map(alias second upload)");
  for (UINT64 i = 0; i < bytes; ++i)
    mapped[i] = static_cast<UINT8>((i * 29 + 0x93) & 0xff);
  secondUpload->Unmap(0, nullptr);

  CopyContext commands(device);
  commands.list->CopyBufferRegion(first.Get(), 0, firstUpload.Get(), 0, bytes);
  transition(commands.list.Get(), first.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(readback.Get(), 0, first.Get(), 0, bytes);

  D3D12_RESOURCE_BARRIER aliasBarrier{};
  aliasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  aliasBarrier.Aliasing.pResourceBefore = first.Get();
  aliasBarrier.Aliasing.pResourceAfter = second.Get();
  commands.list->ResourceBarrier(1, &aliasBarrier);
  transition(commands.list.Get(), second.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(readback.Get(), bytes, second.Get(), 0, bytes);

  transition(commands.list.Get(), second.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_COPY_DEST);
  commands.list->CopyBufferRegion(second.Get(), 0, secondUpload.Get(), 0, bytes);
  transition(commands.list.Get(), second.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(readback.Get(), bytes * 2, second.Get(), 0, bytes);

  aliasBarrier.Aliasing.pResourceBefore = second.Get();
  aliasBarrier.Aliasing.pResourceAfter = first.Get();
  commands.list->ResourceBarrier(1, &aliasBarrier);
  commands.list->CopyBufferRegion(readback.Get(), bytes * 3, first.Get(), 0, bytes);
  commands.submit();

  D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes * 4)};
  mapped = nullptr;
  check(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Map(alias readback)");
  for (UINT64 i = 0; i < bytes; ++i) {
    const UINT8 expectedFirst = static_cast<UINT8>((i * 17 + 0x21) & 0xff);
    const UINT8 expectedSecond = static_cast<UINT8>((i * 29 + 0x93) & 0xff);
    if (mapped[i] != expectedFirst || mapped[bytes + i] != expectedFirst ||
        mapped[bytes * 2 + i] != expectedSecond || mapped[bytes * 3 + i] != expectedSecond)
      fail("overlapping placed-resource alias inheritance mismatch");
  }
  D3D12_RANGE writtenRange{0, 0};
  readback->Unmap(0, &writtenRange);
  first.Reset();
  if (heapReleased) fail("alias heap destroyed with second placement alive");
  second.Reset();
  if (!heapReleased) fail("alias heap leaked after GPU use");

  std::printf("OWNED_D3D12_RESOURCES_ALIASING_GPU_PASS heap_bytes=%llu overlap_offset=0 write_bytes=%llu ordered_snapshots=4 barriers=first_to_second,second_to_first inheritance=bidirectional\n",
              static_cast<unsigned long long>(allocation.SizeInBytes),
              static_cast<unsigned long long>(bytes));
}

void testCustomHeapGpuCoherence(ID3D12Device* device) {
  constexpr UINT64 bytes = 64 * 1024;
  const auto desc = bufferDesc(bytes);
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = bytes;
  heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  heapDesc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
  heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  heapDesc.Properties.CreationNodeMask = heapDesc.Properties.VisibleNodeMask = 1;
  heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
  ComPtr<ID3D12Heap> uploadHeap;
  check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&uploadHeap)), "CreateHeap(custom upload)");
  heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  ComPtr<ID3D12Heap> readbackHeap;
  check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&readbackHeap)), "CreateHeap(custom readback)");
  ComPtr<ID3D12Resource> upload, readback;
  check(device->CreatePlacedResource(uploadHeap.Get(), 0, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                     nullptr, IID_PPV_ARGS(&upload)), "CreatePlacedResource(custom upload)");
  check(device->CreatePlacedResource(readbackHeap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&readback)), "CreatePlacedResource(custom readback)");
  expect(upload->Map(1, nullptr, nullptr), E_INVALIDARG, "Map(custom invalid subresource)");
  check(upload->Map(0, nullptr, nullptr), "Map(custom null output)");
  upload->Unmap(0, nullptr);
  UINT8* mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map(custom upload)");
  if (!mapped) fail("custom upload Map null");
  for (UINT64 i = 0; i < bytes; ++i)
    mapped[i] = static_cast<UINT8>((i * 13 + 0x67) & 0xff);
  upload->Unmap(0, nullptr);
  uploadHeap.Reset();
  readbackHeap.Reset();
  CopyContext commands(device);
  commands.list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, bytes);
  commands.submit();
  D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
  check(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Map(custom readback)");
  if (!mapped) fail("custom readback Map null");
  for (UINT64 i = 0; i < bytes; ++i)
    if (mapped[i] != static_cast<UINT8>((i * 13 + 0x67) & 0xff))
      fail("custom placed heap GPU coherence");
  D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
}

void testBlockCompressedTailCopy(ID3D12Device* device) {
  // A 4x4 BC1 texture's tail mip is 2x2: one 8-byte block. GetCopyableFootprints
  // still pads that footprint to the 4x4 copy block, so a copy of the tail with a
  // null source box must clamp to the 2x2 region both locations share instead of
  // taking the padded 4x4 extent as the copy size.
  const UINT8 block[8] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
  const auto description = texture2DDesc(DXGI_FORMAT_BC1_UNORM, 4, 4, 1, 2, D3D12_RESOURCE_FLAG_NONE);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0;
  UINT64 totalBytes = 0;
  device->GetCopyableFootprints(&description, 1, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
  if (footprint.Footprint.Width != 4 || footprint.Footprint.Height != 4 ||
      footprint.Footprint.RowPitch != D3D12_TEXTURE_DATA_PITCH_ALIGNMENT || rows != 1 || rowBytes != sizeof(block) ||
      totalBytes != sizeof(block))
    fail("GetCopyableFootprints(BC1 tail)");
  if (footprint.Offset)
    fail("GetCopyableFootprints(BC1 tail) offset");

  const auto props = defaultHeap();
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &description,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
        "CreateCommittedResource(BC1 tail)");

  auto upload = createHostBuffer(device, totalBytes, D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, "CreateCommittedResource(BC1 upload)");
  auto readback = createHostBuffer(device, totalBytes, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST, "CreateCommittedResource(BC1 readback)");

  D3D12_RANGE wholeRange{0, static_cast<SIZE_T>(totalBytes)};
  UINT8* mapped = nullptr;
  check(upload->Map(0, &wholeRange, reinterpret_cast<void**>(&mapped)), "BC1 upload Map");
  std::memcpy(mapped, block, sizeof(block));
  D3D12_RANGE writtenRange{0, 0};
  upload->Unmap(0, &writtenRange);

  CopyContext commands(device);

  D3D12_TEXTURE_COPY_LOCATION textureTail{};
  textureTail.pResource = texture.Get();
  textureTail.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  textureTail.SubresourceIndex = 1;

  D3D12_TEXTURE_COPY_LOCATION uploadFootprint{};
  uploadFootprint.pResource = upload.Get();
  uploadFootprint.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  uploadFootprint.PlacedFootprint = footprint;

  D3D12_TEXTURE_COPY_LOCATION readbackFootprint = uploadFootprint;
  readbackFootprint.pResource = readback.Get();

  // Buffer -> 2x2 BC1 tail with a null source box: must be encoded, not dropped.
  commands.list->CopyTextureRegion(&textureTail, 0, 0, 0, &uploadFootprint, nullptr);
  transition(commands.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  // 2x2 BC1 tail -> buffer with a null source box: the readback direction has the
  // same padded-footprint problem, so it must be encoded too.
  commands.list->CopyTextureRegion(&readbackFootprint, 0, 0, 0, &textureTail, nullptr);
  commands.submit();

  mapped = nullptr;
  check(readback->Map(0, &wholeRange, reinterpret_cast<void**>(&mapped)), "BC1 readback Map");
  UINT8 tail[8] = {};
  std::memcpy(tail, mapped + footprint.Offset, sizeof(tail));
  readback->Unmap(0, &writtenRange);
  if (std::memcmp(tail, block, sizeof(tail)) != 0)
    fail("BC1 tail readback mismatch");

  std::printf("OWNED_D3D12_RESOURCES_BC_TAIL_GPU upload=%02x%02x%02x%02x%02x%02x%02x%02x "
              "readback=%02x%02x%02x%02x%02x%02x%02x%02x\n",
              block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7],
              tail[0], tail[1], tail[2], tail[3], tail[4], tail[5], tail[6], tail[7]);
}

struct BcFormat {
  DXGI_FORMAT resource, view;
  UINT family, bytes;
};

std::array<UINT8, 16> bcBlock(const BcFormat& format, UINT tag) {
  std::array<UINT8, 16> block{};
  const UINT color = tag % 4;
  constexpr UINT16 colors[] = {0xf800, 0x07e0, 0x001f, 0x8410};
  if (format.family <= 3) {
    UINT offset = format.family == 1 ? 0 : 8;
    if (format.family == 2) std::memset(block.data(), 0xff, 8);
    if (format.family == 3) block[0] = block[1] = 255;
    block[offset] = block[offset + 2] = static_cast<UINT8>(colors[color]);
    block[offset + 1] = block[offset + 3] = static_cast<UINT8>(colors[color] >> 8);
  } else if (format.family <= 5) {
    const bool signedFormat = format.view == DXGI_FORMAT_BC4_SNORM || format.view == DXGI_FORMAT_BC5_SNORM;
    constexpr UINT8 values[] = {127, 64, 32, 96};
    block[0] = block[1] = signedFormat ? values[color] : static_cast<UINT8>(values[color] * 2);
    if (format.family == 5) block[8] = block[9] = block[0];
  } else {
    // Valid BC6H mode 0 / BC7 mode 6, zero endpoints; vary only index bits.
    if (format.family == 7) block[0] = 0x40;
    block[12] = static_cast<UINT8>(tag * 37 + 3);
    block[13] = static_cast<UINT8>(tag * 19 + 7);
  }
  return block;
}

void testBcOtherShape(ID3D12Device* device, ID3D12RootSignature* root, const BcFormat& format, bool volume) {
  auto desc = texture2DDesc(format.resource, 7, volume ? 5 : 7, volume ? 3 : 6,
                            volume ? 3 : 1, D3D12_RESOURCE_FLAG_NONE);
  if (volume) desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
  support.Format = format.view;
  check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "BC shape format support");
  if (!(support.Support1 & (volume ? D3D12_FORMAT_SUPPORT1_TEXTURE3D : D3D12_FORMAT_SUPPORT1_TEXTURECUBE)))
    fail("BC shape advertised support");
  const auto props = defaultHeap();
  ComPtr<ID3D12Resource> texture, copied;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "BC other shape create");
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copied)), "BC other shape copy create");
  const auto logical = texture->GetDesc();
  if (logical.Width != desc.Width || logical.Height != desc.Height || logical.MipLevels != desc.MipLevels ||
      logical.DepthOrArraySize != desc.DepthOrArraySize || logical.Dimension != desc.Dimension)
    fail("BC volume/cube GetDesc");
  const UINT subs = volume ? 3 : 6;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6]{};
  UINT rows[6]{};
  UINT64 rowBytes[6]{}, total = 0;
  device->GetCopyableFootprints(&desc, 0, subs, 512, footprints, rows, rowBytes, &total);
  const UINT64 bytes = total + 1024;
  std::vector<UINT8> input(bytes, 0x95), expected(bytes, 0x6d);
  for (UINT sub = 0; sub < subs; ++sub) {
    const UINT cols = volume && sub ? 1 : 2;
    const UINT depth = volume && !sub ? 3 : 1;
    const auto& fp = footprints[sub];
    if (fp.Footprint.Width != cols * 4 || fp.Footprint.Height != cols * 4 ||
        fp.Footprint.Depth != depth || rows[sub] != cols || rowBytes[sub] != cols * format.bytes ||
        fp.Footprint.RowPitch != 256 || (fp.Offset % 512)) fail("BC volume/cube footprint");
    for (UINT z = 0; z < depth; ++z) for (UINT y = 0; y < cols; ++y) for (UINT x = 0; x < cols; ++x) {
      const auto block = bcBlock(format, (volume ? sub + z : sub) + y * cols + x);
      const UINT64 offset = fp.Offset + z * cols * 256 + y * 256 + x * format.bytes;
      std::memcpy(input.data() + offset, block.data(), format.bytes);
      std::memcpy(expected.data() + offset, block.data(), format.bytes);
    }
  }
  auto upload = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, "BC shape upload");
  auto guard = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ, "BC shape guards");
  auto readback = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST, "BC shape readback");
  UINT8* mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC shape upload map");
  std::memcpy(mapped, input.data(), bytes); upload->Unmap(0, nullptr);
  check(guard->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC shape guards map");
  std::memset(mapped, 0x6d, bytes); guard->Unmap(0, nullptr);
  CopyContext commands(device);
  commands.list->CopyBufferRegion(readback.Get(), 0, guard.Get(), 0, bytes);
  for (UINT sub = 0; sub < subs; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture.Get(); dst.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[sub];
    const auto& fp = footprints[sub].Footprint;
    const D3D12_BOX physical{0,0,0,fp.Width,fp.Height,fp.Depth};
    commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, sub & 1 ? nullptr : &physical);
  }
  transition(commands.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyResource(copied.Get(), texture.Get());
  transition(commands.list.Get(), copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT sub = 0; sub < subs; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = copied.Get(); src.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprints[sub];
    const auto& fp = footprints[sub].Footprint;
    const D3D12_BOX physical{0,0,0,fp.Width,fp.Height,fp.Depth};
    commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, sub & 1 ? nullptr : &physical);
  }
  transition(commands.list.Get(), copied.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  constexpr char volumeSource[] =
    "Texture3D<float4> tex:register(t0);SamplerState pointSampler:register(s0);RWStructuredBuffer<uint> output:register(u0);"
    "[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){uint w,h,d,m;tex.GetDimensions(id.x,w,h,d,m);"
    "uint i=1+id.x*16;output[i]=w;output[i+1]=h;output[i+2]=d;output[i+3]=m;"
    "for(uint z=0;z<d;z++){float3 p=float3(w-0.5,h-0.5,z+0.5)/float3(w,h,d);"
    "uint4 v=asuint(tex.SampleLevel(pointSampler,p,id.x));uint k=i+4+z*4;"
    "output[k]=v.x;output[k+1]=v.y;output[k+2]=v.z;output[k+3]=v.w;}}";
  constexpr char cubeSource[] =
    "TextureCube<float4> tex:register(t0);SamplerState pointSampler:register(s0);RWStructuredBuffer<uint> output:register(u0);"
    "[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){uint w,h,m;tex.GetDimensions(0,w,h,m);"
    "uint i=1+id.x*8;output[i]=w;output[i+1]=h;output[i+2]=6;output[i+3]=m;"
    "float3 dirs[6]={float3(1,-0.9,-0.9),float3(-1,-0.9,0.9),float3(0.9,1,0.9),"
    "float3(0.9,-1,-0.9),float3(0.9,-0.9,1),float3(-0.9,-0.9,-1)};"
    "uint4 v=asuint(tex.SampleLevel(pointSampler,dirs[id.x],0));"
    "uint k=i+4;output[k]=v.x;output[k+1]=v.y;output[k+2]=v.z;output[k+3]=v.w;}";
  ComPtr<ID3DBlob> shader, errors;
  const char* source = volume ? volumeSource : cubeSource;
  const HRESULT compileHr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
  if (FAILED(compileHr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
  check(compileHr, "BC volume/cube shader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
  pipelineDesc.pRootSignature = root;
  pipelineDesc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)), "BC volume/cube pipeline");
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 1;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors)), "BC volume/cube descriptors");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format.view;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = volume ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURECUBE;
  if (volume) srv.Texture3D.MipLevels = 3; else srv.TextureCube.MipLevels = 1;
  device->CreateShaderResourceView(copied.Get(), &srv, descriptors->GetCPUDescriptorHandleForHeapStart());
  const UINT recordStride = volume ? 16 : 8;
  const UINT sampleBytes = (subs * recordStride + 2) * sizeof(UINT);
  auto resultDesc = bufferDesc(sampleBytes); resultDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &resultDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)), "BC shape result");
  auto sampleReadback = createHostBuffer(device, sampleBytes, D3D12_HEAP_TYPE_READBACK,
                                         D3D12_RESOURCE_STATE_COPY_DEST, "BC shape sample readback");
  commands.list->CopyBufferRegion(result.Get(), 0, guard.Get(), 0, sampleBytes);
  transition(commands.list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[] = {descriptors.Get()}; commands.list->SetDescriptorHeaps(1, heaps);
  commands.list->SetPipelineState(pipeline.Get()); commands.list->SetComputeRootSignature(root);
  commands.list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
  commands.list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
  commands.list->Dispatch(subs, 1, 1);
  transition(commands.list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(sampleReadback.Get(), 0, result.Get(), 0, sampleBytes);
  commands.submit();
  check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC shape readback map");
  if (std::memcmp(mapped, expected.data(), bytes)) fail("BC volume/cube GPU bytes/guards");
  readback->Unmap(0, nullptr);
  UINT* samples = nullptr;
  check(sampleReadback->Map(0, nullptr, reinterpret_cast<void**>(&samples)), "BC shape sample map");
  if (samples[0] != 0x6d6d6d6d || samples[subs * recordStride + 1] != 0x6d6d6d6d) fail("BC shape sample guards");
  for (UINT sub = 0; sub < subs; ++sub) {
    const UINT* record = samples + 1 + sub * recordStride;
    const UINT width = volume ? std::max(1u, 7u >> sub) : 7;
    const UINT height = volume ? std::max(1u, 5u >> sub) : 7;
    const UINT depth = volume ? std::max(1u, 3u >> sub) : 6;
    if (record[0] != width || record[1] != height || record[2] != depth || record[3] != desc.MipLevels)
      fail("BC volume/cube shader dimensions");
    for (UINT z = 0; z < (volume ? depth : 1); ++z) {
      const UINT tag = volume ? sub + std::min(z, depth - 1) + (sub ? 0 : 3) : sub + 3;
      for (UINT c = 0; c < 4; ++c) {
        constexpr UINT8 gray[] = {132, 130, 132};
        const float wanted = c == 3 ? 1.f : tag % 4 == 3 ? gray[c] / 255.f : tag % 4 == c ? 1.f : 0.f;
        float actual = 0; std::memcpy(&actual, record + 4 + z * 4 + c, sizeof(actual));
        if (!(actual >= wanted - 0.01f && actual <= wanted + 0.01f)) fail("BC volume/cube SRV sample");
      }
    }
    for (UINT i = 4 + (volume ? depth : 1) * 4; i < recordStride; ++i)
      if (record[i] != 0x6d6d6d6d) fail("BC shape unused sample guards");
  }
  sampleReadback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_RESOURCES_UNALIGNED_BC_SHAPE_PASS shape=%s format=%u logical=7x%ux%u copy=1 guards=1 sample=1 dimensions=1\n",
              volume ? "volume" : "cube", format.resource, desc.Height, desc.DepthOrArraySize);
}

void testUnalignedBlockTextures(ID3D12Device* device) {
  // The flag removes top-level dimension alignment, not block-aligned copy rules.
  const BcFormat formats[] = {
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, 1, 8},
    {DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC1_UNORM_SRGB, 1, 8},
    {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB, 1, 8},
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, 2, 16},
    {DXGI_FORMAT_BC2_UNORM_SRGB, DXGI_FORMAT_BC2_UNORM_SRGB, 2, 16},
    {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, 2, 16},
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, 3, 16},
    {DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB, 3, 16},
    {DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, 3, 16},
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, 4, 8},
    {DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC4_SNORM, 4, 8},
    {DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, 4, 8},
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, 5, 16},
    {DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC5_SNORM, 5, 16},
    {DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, 5, 16},
    {DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_UF16, 6, 16},
    {DXGI_FORMAT_BC6H_SF16, DXGI_FORMAT_BC6H_SF16, 6, 16},
    {DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_UF16, 6, 16},
    {DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM, 7, 16},
    {DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM_SRGB, 7, 16},
    {DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM, 7, 16},
  };
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor.ShaderRegister = 0;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> serialized, errors, shader;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
        "BC serialize root");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(&root)), "BC root");
  constexpr char source[] =
    "Texture2DArray<float4> tex : register(t0); SamplerState pointSampler : register(s0);"
    "RWStructuredBuffer<uint> output : register(u0);"
    "[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {"
    "uint w,h,a,m; tex.GetDimensions(id.x,w,h,a,m); uint base=1+(id.y*3+id.x)*20;"
    "output[base]=w;output[base+1]=h;output[base+2]=a;output[base+3]=m;"
    "for(uint y=0;y<(h+3)/4;y++)for(uint x=0;x<(w+3)/4;x++){"
    "float2 p=float2(min(x*4+2,w-1),min(y*4+2,h-1));"
    "float4 v=tex.SampleLevel(pointSampler,float3((p+0.5)/float2(w,h),id.y),id.x);"
    "uint4 bits=asuint(v);uint i=base+4+(y*2+x)*4;"
    "output[i]=bits.x;output[i+1]=bits.y;output[i+2]=bits.z;output[i+3]=bits.w;}}";
  const HRESULT compileHr = D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "main", "cs_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
  if (FAILED(compileHr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
  check(compileHr, "BC compile shader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
  pipelineDesc.pRootSignature = root.Get();
  pipelineDesc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)), "BC sample pipeline");

  const auto props = defaultHeap();
  CopyContext commands(device);
  for (const auto& format : formats) {
    const auto desc = texture2DDesc(format.resource, 7, 5, 2, 3, D3D12_RESOURCE_FLAG_NONE);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format.view;
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "BC format support");
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) fail("BC advertised format support");
    ComPtr<ID3D12Resource> texture, whole, placed, cpu, alignedTexture;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "BC odd create");
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&whole)), "BC whole create");
    const auto alignedDesc = texture2DDesc(format.resource, 8, 8, 2, 3, D3D12_RESOURCE_FLAG_NONE);
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &alignedDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&alignedTexture)), "BC aligned create");
    auto cpuProps = props;
    cpuProps.Type = D3D12_HEAP_TYPE_CUSTOM;
    cpuProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    cpuProps.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    check(device->CreateCommittedResource(&cpuProps, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cpu)), "BC CPU create");
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
    if (!allocation.SizeInBytes || allocation.SizeInBytes == ~0ull) fail("BC odd allocation");
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = allocation.SizeInBytes + allocation.Alignment;
    heapDesc.Alignment = allocation.Alignment;
    heapDesc.Properties = props;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "BC placed heap");
    check(device->CreatePlacedResource(heap.Get(), allocation.Alignment, &desc,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&placed)), "BC placed create");
    for (auto* resource : {texture.Get(), whole.Get(), placed.Get(), cpu.Get()}) {
      const auto logical = resource->GetDesc();
      if (logical.Width != 7 || logical.Height != 5 || logical.DepthOrArraySize != 2 || logical.MipLevels != 3 ||
          logical.Format != format.resource) fail("BC logical GetDesc");
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6]{};
    UINT rows[6]{};
    UINT64 rowBytes[6]{}, total = 0;
    device->GetCopyableFootprints(&desc, 0, 6, 512, footprints, rows, rowBytes, &total);
    const UINT64 bytes = total + 512 + 512;
    std::vector<UINT8> payload(bytes, 0xa5), expected(bytes, 0x6d);
    UINT64 nextOffset = 512;
    for (UINT sub = 0; sub < 6; ++sub) {
      const UINT mip = sub % 3;
      const UINT columns = mip ? 1 : 2, countRows = mip ? 1 : 2;
      const auto& fp = footprints[sub];
      if (fp.Offset != nextOffset || fp.Footprint.Width != (mip ? 4u : 8u) ||
          fp.Footprint.Height != (mip ? 4u : 8u) || fp.Footprint.RowPitch != 256 ||
          rows[sub] != countRows || rowBytes[sub] != columns * format.bytes) fail("BC odd footprints");
      nextOffset = (fp.Offset + (countRows - 1) * 256 + rowBytes[sub] + 511) & ~UINT64(511);
      for (UINT y = 0; y < countRows; ++y) for (UINT x = 0; x < columns; ++x) {
        const auto block = bcBlock(format, sub + y * columns + x);
        std::memcpy(payload.data() + fp.Offset + y * 256 + x * format.bytes, block.data(), format.bytes);
        std::memcpy(expected.data() + fp.Offset + y * 256 + x * format.bytes, block.data(), format.bytes);
      }
      // Deliberately non-block-multiple CPU pitch; only active block bytes may change.
      const UINT pitch = columns * format.bytes + 3;
      std::vector<UINT8> cpuInput(countRows * pitch, 0x91), cpuOutput(countRows * pitch, 0x73);
      for (UINT y = 0; y < countRows; ++y)
        std::memcpy(cpuInput.data() + y * pitch, payload.data() + fp.Offset + y * 256, rowBytes[sub]);
      check(cpu->WriteToSubresource(sub, nullptr, cpuInput.data(), pitch, 0), "BC CPU odd write");
      check(cpu->ReadFromSubresource(cpuOutput.data(), pitch, 0, sub, nullptr), "BC CPU odd read");
      for (UINT y = 0; y < countRows; ++y) for (UINT b = 0; b < pitch; ++b)
        if (cpuOutput[y * pitch + b] != (b < rowBytes[sub] ? cpuInput[y * pitch + b] : 0x73))
          fail("BC CPU odd payload/padding");
    }
    if (total != footprints[5].Offset + rowBytes[5] - 512) fail("BC odd footprint total");
    const D3D12_BOX invalidBoxes[] = {{1,0,0,7,5,1}, {0,1,0,7,5,1}, {0,0,0,3,5,1}, {0,0,0,7,3,1}};
    UINT8 scratch[128]{};
    for (const auto& box : invalidBoxes) {
      expect(cpu->WriteToSubresource(0, &box, scratch, 64, 0), E_INVALIDARG, "BC CPU invalid interior write");
      expect(cpu->ReadFromSubresource(scratch, 64, 0, 0, &box), E_INVALIDARG, "BC CPU invalid interior read");
    }
    const D3D12_BOX cpuEdge{4,4,0,7,5,1};
    const auto cpuEdgeBlock = bcBlock(format, 12);
    check(cpu->WriteToSubresource(0, &cpuEdge, cpuEdgeBlock.data(), format.bytes, 0), "BC CPU logical edge write");
    std::array<UINT8, 16> cpuEdgeRead{};
    check(cpu->ReadFromSubresource(cpuEdgeRead.data(), format.bytes, 0, 0, &cpuEdge), "BC CPU logical edge read");
    if (std::memcmp(cpuEdgeRead.data(), cpuEdgeBlock.data(), format.bytes)) fail("BC CPU logical edge bytes");
    auto upload = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, "BC upload");
    auto guard = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, "BC guards");
    auto readback = createHostBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, "BC readback");
    UINT8* mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC upload map");
    std::memcpy(mapped, payload.data(), bytes);
    upload->Unmap(0, nullptr);
    check(guard->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC guard map");
    std::memset(mapped, 0x6d, bytes);
    guard->Unmap(0, nullptr);
    auto predicate = createHostBuffer(device, 16, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, "BC predicate");
    check(predicate->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC predicate map");
    // EQUAL_ZERO discards a zero predicate: offset 0 executes, offset 8 skips.
    const UINT64 predicateValues[] = {1, 0};
    std::memcpy(mapped, predicateValues, sizeof(predicateValues));
    predicate->Unmap(0, nullptr);
    commands.list->CopyBufferRegion(readback.Get(), 0, guard.Get(), 0, bytes);
    for (UINT sub = 0; sub < 6; ++sub) {
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture.Get(); dst.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[sub];
      const auto& fp = footprints[sub].Footprint;
      const D3D12_BOX physical{0,0,0,fp.Width,fp.Height,1};
      if (sub >= 3) commands.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, sub >= 3 ? nullptr : &physical);
      commands.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    transition(commands.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.list->CopyResource(whole.Get(), texture.Get());
    transition(commands.list.Get(), whole.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT sub = 0; sub < 6; ++sub) {
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = alignedTexture.Get(); dst.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = whole.Get(); src.SubresourceIndex = sub;
      const auto& fp = footprints[sub].Footprint;
      const D3D12_BOX physical{0,0,0,fp.Width,fp.Height,1};
      if (sub >= 3) commands.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &physical);
      commands.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    transition(commands.list.Get(), alignedTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT sub = 0; sub < 6; ++sub) {
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = placed.Get(); dst.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = alignedTexture.Get(); src.SubresourceIndex = sub;
      const auto& fp = footprints[sub].Footprint;
      const D3D12_BOX physical{0,0,0,fp.Width,fp.Height,1};
      if (sub >= 3) commands.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &physical);
      commands.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    D3D12_TEXTURE_COPY_LOCATION edgeDst{}; edgeDst.pResource = placed.Get(); edgeDst.SubresourceIndex = 3;
    D3D12_TEXTURE_COPY_LOCATION edgeSrc{}; edgeSrc.pResource = whole.Get();
    const D3D12_BOX edge{4,4,0,8,8,1};
    commands.list->CopyTextureRegion(&edgeDst, 4, 4, 0, &edgeSrc, &edge);
    const auto edgeBlock = bcBlock(format, 3);
    std::memcpy(expected.data() + footprints[3].Offset + 256 + format.bytes, edgeBlock.data(), format.bytes);
    D3D12_TEXTURE_COPY_LOCATION uploadEdge{}; uploadEdge.pResource = upload.Get();
    uploadEdge.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; uploadEdge.PlacedFootprint = footprints[3];
    D3D12_TEXTURE_COPY_LOCATION textureEdge{}; textureEdge.pResource = placed.Get();
    commands.list->CopyTextureRegion(&textureEdge, 4, 4, 0, &uploadEdge, &edge);
    const auto uploadedEdgeBlock = bcBlock(format, 6);
    std::memcpy(expected.data() + footprints[0].Offset + 256 + format.bytes, uploadedEdgeBlock.data(), format.bytes);
    // Invalid copies poison Close; check each rejection on a fresh, unsubmitted list.
    auto rejectCopy = [&](const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y,
                          const D3D12_TEXTURE_COPY_LOCATION& src, const D3D12_BOX& box) {
      ComPtr<ID3D12CommandAllocator> invalidAllocator;
      ComPtr<ID3D12GraphicsCommandList> invalidList;
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&invalidAllocator)),
            "BC invalid copy allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, invalidAllocator.Get(), nullptr,
                                      IID_PPV_ARGS(&invalidList)), "BC invalid copy list");
      invalidList->CopyTextureRegion(&dst, x, y, 0, &src, &box);
      expect(invalidList->Close(), E_INVALIDARG, "BC invalid copy Close");
    };
    const D3D12_BOX firstBlock{0,0,0,4,4,1};
    auto invalidUpload = uploadEdge;
    invalidUpload.PlacedFootprint.Offset += 1;
    rejectCopy(textureEdge, 0, 0, invalidUpload, firstBlock);
    invalidUpload = uploadEdge; invalidUpload.PlacedFootprint.Footprint.RowPitch = 255;
    rejectCopy(textureEdge, 0, 0, invalidUpload, firstBlock);
    const D3D12_BOX outside{4,4,0,12,8,1};
    rejectCopy(textureEdge, 0, 0, uploadEdge, outside);
    for (const auto& box : invalidBoxes) rejectCopy(edgeDst, 0, 0, edgeSrc, box);
    const D3D12_BOX aligned{0,0,0,4,4,1};
    commands.list->SetPredication(predicate.Get(), 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
    commands.list->CopyTextureRegion(&edgeDst, 4, 0, 0, &edgeSrc, &aligned);
    commands.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    rejectCopy(edgeDst, 1, 0, edgeSrc, aligned);
    rejectCopy(edgeDst, 0, 1, edgeSrc, aligned);
    transition(commands.list.Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION edgeReadback{}; edgeReadback.pResource = readback.Get();
    edgeReadback.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    edgeReadback.PlacedFootprint = {nextOffset, {format.resource, 4, 4, 1, 256}};
    commands.list->CopyTextureRegion(&edgeReadback, 0, 0, 0, &textureEdge, &edge);
    std::memcpy(expected.data() + nextOffset, uploadedEdgeBlock.data(), format.bytes);
    for (UINT sub = 0; sub < 6; ++sub) {
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = placed.Get(); src.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprints[sub];
      const auto& fp = footprints[sub].Footprint;
      const D3D12_BOX physical{0,0,0,fp.Width,fp.Height,1};
      if (sub >= 3) commands.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, sub >= 3 ? nullptr : &physical);
      commands.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    transition(commands.list.Get(), placed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC descriptorDesc{};
    descriptorDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorDesc.NumDescriptors = 1;
    descriptorDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    check(device->CreateDescriptorHeap(&descriptorDesc, IID_PPV_ARGS(&descriptors)), "BC SRV heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format.view;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = 3;
    srv.Texture2DArray.ArraySize = 2;
    device->CreateShaderResourceView(placed.Get(), &srv, descriptors->GetCPUDescriptorHandleForHeapStart());
    constexpr UINT sampleBytes = (6 * 20 + 2) * sizeof(UINT);
    auto resultDesc = bufferDesc(sampleBytes); resultDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &resultDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)), "BC sample output");
    auto sampleReadback = createHostBuffer(device, sampleBytes, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST, "BC sample readback");
    commands.list->CopyBufferRegion(result.Get(), 0, guard.Get(), 0, sampleBytes);
    transition(commands.list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {descriptors.Get()}; commands.list->SetDescriptorHeaps(1, heaps);
    commands.list->SetPipelineState(pipeline.Get());
    commands.list->SetComputeRootSignature(root.Get());
    commands.list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
    commands.list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
    commands.list->Dispatch(3, 2, 1);
    transition(commands.list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.list->CopyBufferRegion(sampleReadback.Get(), 0, result.Get(), 0, sampleBytes);
    commands.submit();
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "BC readback map");
    for (UINT64 b = 0; b < bytes; ++b) if (mapped[b] != expected[b]) {
      std::fprintf(stderr, "BC_BYTES_MISMATCH format=%u offset=%llu expected=%02x actual=%02x\n",
                   format.resource, static_cast<unsigned long long>(b), expected[b], mapped[b]);
      fail("BC GPU bytes or guard mismatch");
    }
    readback->Unmap(0, nullptr);
    UINT* samples = nullptr;
    check(sampleReadback->Map(0, nullptr, reinterpret_cast<void**>(&samples)), "BC sample map");
    if (samples[0] != 0x6d6d6d6d || samples[121] != 0x6d6d6d6d) fail("BC sample guards");
    for (UINT sub = 0; sub < 6; ++sub) {
      const UINT mip = sub % 3, w = std::max(1u, 7u >> mip), h = std::max(1u, 5u >> mip);
      const UINT* record = samples + 1 + sub * 20;
      if (record[0] != w || record[1] != h || record[2] != 2 || record[3] != 3) fail("BC shader logical dimensions");
      for (UINT b = 0; b < (mip ? 1u : 4u); ++b) {
        const UINT block = mip ? 0 : b;
        const UINT tag = sub == 3 && block == 3 ? 3 : sub == 0 && block == 3 ? 6 : sub + block;
        float expectedSample[4] = {0, 0, 0, 1};
        if (format.family <= 3) {
          const UINT color = tag % 4;
          constexpr UINT8 gray[] = {132, 130, 132};
          const bool srgb = format.view == DXGI_FORMAT_BC1_UNORM_SRGB || format.view == DXGI_FORMAT_BC2_UNORM_SRGB ||
                            format.view == DXGI_FORMAT_BC3_UNORM_SRGB;
          for (UINT c = 0; c < 3; ++c) {
            expectedSample[c] = color == 3 ? gray[c] / 255.f : color == c ? 1.f : 0.f;
            if (srgb && color == 3) expectedSample[c] = std::pow((expectedSample[c] + 0.055f) / 1.055f, 2.4f);
          }
        } else if (format.family <= 5) {
          constexpr UINT values[] = {127,64,32,96};
          const bool signedFormat = format.view == DXGI_FORMAT_BC4_SNORM || format.view == DXGI_FORMAT_BC5_SNORM;
          expectedSample[0] = float(values[tag % 4]) * (signedFormat ? 1.f / 127.f : 2.f / 255.f);
          if (format.family == 5) expectedSample[1] = expectedSample[0];
        } else if (format.family == 7) expectedSample[3] = 0;
        for (UINT c = 0; c < 4; ++c) {
          float actual = 0; std::memcpy(&actual, record + 4 + b * 4 + c, sizeof(actual));
          if (!(actual >= expectedSample[c] - 0.01f && actual <= expectedSample[c] + 0.01f)) {
            std::fprintf(stderr, "BC_SAMPLE_MISMATCH format=%u sub=%u block=%u channel=%u expected=%f actual=%f\n",
                         format.resource, sub, b, c, expectedSample[c], actual);
            fail("BC real SRV sample");
          }
        }
      }
    }
    for (UINT sub = 0; sub < 6; ++sub) if (sub % 3)
      for (UINT i = 8; i < 20; ++i)
        if (samples[1 + sub * 20 + i] != 0x6d6d6d6d) fail("BC unused sample guards");
    sampleReadback->Unmap(0, nullptr);
    std::printf("OWNED_D3D12_RESOURCES_UNALIGNED_BC_FORMAT_PASS format=%u view=%u block_bytes=%u logical=7x5 mips=3 slices=2 cpu=1 upload=1 wholecopy=1 regioncopy=1 placed=1 guards=1 sample=1 dimensions=1 invalid_interior=1 mixed_dimensions=1 predication=1 explicit_physical_boxes=1 buffer_edge=1\n",
                format.resource, format.view, format.bytes);
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options{};
  testBcOtherShape(device, root.Get(), formats[0], true);
  testBcOtherShape(device, root.Get(), formats[6], true);
  testBcOtherShape(device, root.Get(), formats[0], false);
  const HRESULT capHr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options, sizeof(options));
  std::printf("OWNED_D3D12_RESOURCES_UNALIGNED_BC_CAP hr=0x%08lx supported=%u\n",
              static_cast<unsigned long>(capHr), options.UnalignedBlockTexturesSupported);
  check(capHr, "OPTIONS8 unaligned BC query");
  if (!options.UnalignedBlockTexturesSupported) fail("OPTIONS8 unaligned BC supported");
  expect(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, nullptr, sizeof(options)), E_INVALIDARG,
         "OPTIONS8 null output");
  expect(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options, sizeof(options) - 1), E_INVALIDARG,
         "OPTIONS8 short size");
  expect(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options, sizeof(options) + 1), E_INVALIDARG,
         "OPTIONS8 long size");
}

} // namespace

int main() {
  requireOwnedBackend();
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
        "D3D12CreateDevice");
  testResourceContracts(device.Get());
  testPlacedAlignment(device.Get());
  testAllocationInfoPlacement(device.Get());
  testPlacedHeapLifetime(device.Get());
  testSmallMsaaAlignment(device.Get());
  testCpuTextureTransfers(device.Get());
  testTypelessDepthViews(device.Get());
  testBufferDescriptors(device.Get());
  testOverlappingPlacedResourceAliasing(device.Get());
  testCustomHeapGpuCoherence(device.Get());
  testBlockCompressedTailCopy(device.Get());
  testUnalignedBlockTextures(device.Get());
  std::puts("OWNED_D3D12_RESOURCES_PASS owned_backend=1 resource_contracts=1 placed_alignment=1 depth_planes=1 mip_array_views=1 typed_buffer_descriptors=1 null_descriptors=1 bc_tail_copy=1 unaligned_bc=1");
  return 0;
}
