#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace {

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
  clear.Format = DXGI_FORMAT_R32_FLOAT;
  expect(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &color,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                         __uuidof(ID3D12Resource), nullptr),
         E_INVALIDARG, "incompatible optimized clear");

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
}

void testPlacedAlignment(ID3D12Device* device) {
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = 4ull * 1024ull * 1024ull;
  heapDesc.Properties = defaultHeap();
  heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
  ComPtr<ID3D12Heap> heap;
  check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateHeap");

  const auto buffer = bufferDesc(64 * 1024);
  ComPtr<ID3D12Resource> resource;
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

  std::printf("OWNED_D3D12_RESOURCES_ALIASING_GPU_PASS heap_bytes=%llu overlap_offset=0 write_bytes=%llu ordered_snapshots=4 barriers=first_to_second,second_to_first inheritance=bidirectional\n",
              static_cast<unsigned long long>(allocation.SizeInBytes),
              static_cast<unsigned long long>(bytes));
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

} // namespace

int main() {
  requireOwnedBackend();
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
        "D3D12CreateDevice");
  testResourceContracts(device.Get());
  testPlacedAlignment(device.Get());
  testTypelessDepthViews(device.Get());
  testBufferDescriptors(device.Get());
  testOverlappingPlacedResourceAliasing(device.Get());
  testBlockCompressedTailCopy(device.Get());
  std::puts("OWNED_D3D12_RESOURCES_PASS owned_backend=1 resource_contracts=1 placed_alignment=1 depth_planes=1 mip_array_views=1 typed_buffer_descriptors=1 null_descriptors=1 bc_tail_copy=1");
  return 0;
}
