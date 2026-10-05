// Reserved 2D/array/volume resources with independent GPU readback oracles: tile mappings across
// heaps, NULL/SKIP/REUSE_SINGLE_TILE ranges, CopyTileMappings, CopyTiles, packed mips, mapping
// updates ordered at ExecuteCommandLists and cross-queue fence boundaries, residency status
// (CheckAccessFullyMapped) for texture and buffer accesses, min/max sampler reduction, and the
// creation/validation errors, per-slice full-chain array tails, standard-shape 3D addressing,
// filtering/LOD, UAV atomics and canonical RTV/alias coherence.
// Shaders: graphics-translator/tests/dxil/tiled.hlsl (frozen DXIL beside the executable).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kTile = 65536;
int cases = 0;

[[noreturn]] void
fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_TILED_FAIL step=%s hr=%08lx\n", step, static_cast<unsigned long>(hr));
  std::fflush(stderr);
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
void expect(const char *what, UINT actual, UINT expected) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_D3D12_TILED_FAIL value=%s actual=%08x expected=%08x\n", what, actual, expected);
    std::fflush(stderr);
    ExitProcess(1);
  }
}
void pass(const char *name) {
  std::printf("OWNED_D3D12_TILED_CASE %s status=PASS\n", name);
  std::fflush(stdout);
  cases++;
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CreationNodeMask = properties.VisibleNodeMask = 1;
  return properties;
}
D3D12_RESOURCE_DESC bufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}
D3D12_RESOURCE_DESC textureDesc(D3D12_RESOURCE_DIMENSION dimension, UINT width, UINT height, UINT16 depth, UINT16 mips,
                                DXGI_FORMAT format, D3D12_TEXTURE_LAYOUT layout) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = dimension;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = depth;
  desc.MipLevels = mips;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = layout;
  return desc;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}
void uavBarrier(ID3D12GraphicsCommandList *list) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  list->ResourceBarrier(1, &barrier);
}
std::vector<BYTE> readFile(const char *path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(file != INVALID_HANDLE_VALUE, "open DXIL");
  const DWORD size = GetFileSize(file, nullptr);
  require(size != INVALID_FILE_SIZE && size > 0, "DXIL size");
  std::vector<BYTE> bytes(size);
  DWORD read = 0;
  require(ReadFile(file, bytes.data(), size, &read, nullptr) && read == size, "read DXIL");
  CloseHandle(file);
  return bytes;
}
UINT packColor(UINT tile) { return (tile * 16) | ((255 - tile * 16) << 8) | (tile << 16) | (0xffu << 24); }

struct Context {
  ComPtr<ID3D12Device4> device;
  ComPtr<ID3D12CommandQueue> queue, queue2;
  ComPtr<ID3D12CommandAllocator> allocators[2];
  UINT64 allocatorFence[2] = {};
  UINT current = 0;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 100;
  HANDLE event = nullptr;

  ComPtr<ID3D12Resource> committed(D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state,
                                   const char *step) {
    const auto properties = heapProperties(type);
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)),
          step);
    return resource;
  }
  ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, UINT64 size, const char *step) {
    return committed(type, bufferDesc(size, type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                          : D3D12_RESOURCE_FLAG_NONE),
                     type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                     : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     step);
  }
  /* Alternates two allocators; an allocator is reset only after its last list retired. */
  void reset() {
    current ^= 1;
    if (fence->GetCompletedValue() < allocatorFence[current]) {
      check(fence->SetEventOnCompletion(allocatorFence[current], event), "allocator wait");
      require(WaitForSingleObject(event, 20000) == WAIT_OBJECT_0, "allocator wait");
    }
    check(allocators[current]->Reset(), "allocator reset");
    check(list->Reset(allocators[current].Get(), nullptr), "list reset");
  }
  void execute(const char *step) {
    check(list->Close(), step);
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++value), step);
    allocatorFence[current] = value;
  }
  void finish(const char *step) {
    check(queue->Signal(fence.Get(), ++value), step);
    check(fence->SetEventOnCompletion(value, event), step);
    require(WaitForSingleObject(event, 20000) == WAIT_OBJECT_0, step);
    check(device->GetDeviceRemovedReason(), step);
  }
};

struct Words {
  ComPtr<ID3D12Resource> resource;
  const UINT *data = nullptr;
  explicit Words(ComPtr<ID3D12Resource> readback) : resource(std::move(readback)) {
    check(resource->Map(0, nullptr, const_cast<void **>(reinterpret_cast<const void **>(&data))), "map readback");
  }
  ~Words() { resource->Unmap(0, nullptr); }
  UINT operator[](size_t index) const { return data[index]; }
};

ComPtr<ID3D12PipelineState> computePipeline(ID3D12Device *device, ID3D12RootSignature *root, const std::vector<BYTE> &cs) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.CS = {cs.data(), cs.size()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "compute pipeline");
  return pso;
}
ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC &desc) {
  ComPtr<ID3DBlob> bytes, error;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &error))) {
    if (error)
      std::fprintf(stderr, "%s\n", static_cast<const char *>(error->GetBufferPointer()));
    fail("root serialize");
  }
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&root)),
        "root signature");
  return root;
}
#include "owned-d3d12-tiled-volume.inc"
} // namespace

int
main(int argc, char **argv) {
  if (argc == 2 && !std::strcmp(argv[1], "--oracle-self-test")) {
    volumeOracleSelfTest();
    return 0;
  }
  require(argc == 21, "usage: tiled.exe tiled-write.dxil tiled-buffer-status.dxil tiled-texture-status.dxil "
                      "tiled-dxbc-status.dxbc tiled-fxc-cs.dxbc tiled-fxc-vs.dxbc tiled-fxc-ps.dxbc "
                      "tiled-root-status.dxil tiled-rgb32-status.dxil tiled-array-status.dxil "
                      "volume-read.dxil volume-uint.dxil volume-vs.dxil volume-sample.dxil volume-color.dxil "
                      "volume-read.dxbc volume-uint.dxbc volume-vs.dxbc volume-sample.dxbc volume-color.dxbc");
  const auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  require(backend && !std::strcmp(backend(), "owned-graphics-d3d12"), "backend");
  std::printf("OWNED_D3D12_TILED_BACKEND id=%s\n", backend());
  const auto writeCs = readFile(argv[1]), bufferCs = readFile(argv[2]), textureCs = readFile(argv[3]);
  const auto dxbcCs = readFile(argv[4]);
  const auto fxcCs = readFile(argv[5]), fxcVs = readFile(argv[6]), fxcPs = readFile(argv[7]);
  const auto rootStatusCs = readFile(argv[8]), rgb32StatusCs = readFile(argv[9]);
  const auto arrayStatusCs = readFile(argv[10]);

  Context c;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)), "device");
  auto *device = c.device.Get();
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&c.queue)), "queue");
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&c.queue2)), "compute queue");
  for (auto &allocator : c.allocators)
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocators[0].Get(), nullptr,
                                  IID_PPV_ARGS(&c.list)),
        "list");
  check(c.list->Close(), "initial close");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");
  c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(c.event != nullptr, "event");
  auto *list = c.list.Get();
  auto *queue = c.queue.Get();

  /* ---- Capabilities ---- */
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "options");
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)), "options5");
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &va, sizeof(va)), "va support");
  D3D12_FEATURE_DATA_FORMAT_SUPPORT rgba8{DXGI_FORMAT_R8G8B8A8_UNORM};
  check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &rgba8, sizeof(rgba8)), "format support");
  std::printf("OWNED_D3D12_TILED_CAPS tier=%d srv_only_tier3=%d va_bits_per_resource=%u rgba8_tiled=%d\n",
              int(options.TiledResourcesTier), int(options5.SRVOnlyTiledResourceTier3),
              va.MaxGPUVirtualAddressBitsPerResource, (rgba8.Support2 & D3D12_FORMAT_SUPPORT2_TILED) ? 1 : 0);

  /* ---- Pipelines ---- */
  D3D12_ROOT_PARAMETER writeParams[2]{};
  writeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  writeParams[0].Constants = {0, 0, 2};
  writeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  writeParams[1].Descriptor = {0, 0};
  const auto writeRoot = rootSignature(device, {2, writeParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  const auto writePso = computePipeline(device, writeRoot.Get(), writeCs);

  D3D12_DESCRIPTOR_RANGE bufferRanges[2]{};
  bufferRanges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
  bufferRanges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, 0, 4};
  D3D12_ROOT_PARAMETER bufferParams[2]{};
  bufferParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  bufferParams[0].DescriptorTable = {2, bufferRanges};
  bufferParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  bufferParams[1].Descriptor = {0, 0};
  const auto bufferRoot = rootSignature(device, {2, bufferParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  const auto bufferPso = computePipeline(device, bufferRoot.Get(), bufferCs);

  D3D12_DESCRIPTOR_RANGE textureRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER textureParams[3]{};
  textureParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  textureParams[0].DescriptorTable = {1, &textureRange};
  textureParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  textureParams[1].Constants = {0, 0, 1};
  textureParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  textureParams[2].Descriptor = {0, 0};
  D3D12_STATIC_SAMPLER_DESC samplers[4]{};
  const D3D12_FILTER filters[4] = {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR,
                                   D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_LINEAR};
  for (UINT i = 0; i < 4; i++) {
    samplers[i].Filter = filters[i];
    samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].MaxAnisotropy = 1;
    samplers[i].ShaderRegister = i;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  }
  const auto textureRoot = rootSignature(device, {3, textureParams, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  const auto texturePso = computePipeline(device, textureRoot.Get(), textureCs);

  D3D12_DESCRIPTOR_HEAP_DESC descriptorDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> descriptors;
  check(device->CreateDescriptorHeap(&descriptorDesc, IID_PPV_ARGS(&descriptors)), "descriptor heap");
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpuSlot = [&](UINT i) {
    auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(i) * increment;
    return handle;
  };
  auto gpuSlot = [&](UINT i) {
    auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64(i) * increment;
    return handle;
  };
  auto dispatchWrite = [&](ID3D12Resource *target, UINT groups, UINT value, UINT offset) {
    list->SetComputeRootSignature(writeRoot.Get());
    list->SetPipelineState(writePso.Get());
    const UINT constants[2] = {value, offset};
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
    list->Dispatch(groups, 1, 1);
    uavBarrier(list);
  };

  /* ---- Heaps ---- */
  auto makeHeap = [&](UINT tiles, D3D12_HEAP_FLAGS flags, const char *step) {
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = UINT64(tiles) * kTile;
    desc.Properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags = flags;
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&desc, IID_PPV_ARGS(&heap)), step);
    return heap;
  };
  const auto heapA = makeHeap(4, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, "heap A");
  const auto heapB = makeHeap(4, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, "heap B");
  const auto heapT = makeHeap(24, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES, "texture heap");

  /* ---- Reserved buffers ---- */
  const auto reservedBufferDesc = bufferDesc(4 * kTile, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ComPtr<ID3D12Resource> R, R2, R3, R4;
  check(device->CreateReservedResource(&reservedBufferDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                       IID_PPV_ARGS(&R)),
        "reserved buffer");
  check(device->CreateReservedResource(&reservedBufferDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                       IID_PPV_ARGS(&R2)),
        "reserved alias buffer");
  check(device->CreateReservedResource1(&reservedBufferDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                        IID_PPV_ARGS(&R3)),
        "CreateReservedResource1");
  const auto oneTileDesc = bufferDesc(kTile, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  check(device->CreateReservedResource(&oneTileDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&R4)),
        "reserved probe buffer");
  {
    UINT total = 0, subresources = 1;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE shape{};
    D3D12_SUBRESOURCE_TILING tiling{};
    device->GetResourceTiling(R.Get(), &total, &packed, &shape, &subresources, 0, &tiling);
    expect("buffer total tiles", total, 4);
    expect("buffer shape", shape.WidthInTexels, kTile);
    expect("buffer shape height", shape.HeightInTexels, 1);
    expect("buffer packed mips", packed.NumPackedMips, 0);
    expect("buffer subresource tiles", tiling.WidthInTiles, 4);
    require(R->GetGPUVirtualAddress() != 0 && R2->GetGPUVirtualAddress() != R->GetGPUVirtualAddress(),
            "reserved buffer GPU VA");
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    expect("reserved GetHeapProperties", UINT(R->GetHeapProperties(&properties, &flags)), UINT(E_INVALIDARG));
  }
  pass("buffer-tiling");

  /* Committed comparison buffers, their SRVs and the reserved buffer views. */
  const auto committedBuffer = c.committed(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(64), D3D12_RESOURCE_STATE_COPY_DEST,
                                           "committed buffer");
  const auto uploadSmall = c.buffer(D3D12_HEAP_TYPE_UPLOAD, 64, "committed upload");
  {
    UINT *data = nullptr;
    check(uploadSmall->Map(0, nullptr, reinterpret_cast<void **>(&data)), "map committed upload");
    for (UINT i = 0; i < 16; i++)
      data[i] = i * 0x11;
    uploadSmall->Unmap(0, nullptr);
  }
  auto rawSrv = [&](ID3D12Resource *resource, UINT elements, UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer = {0, elements, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    device->CreateShaderResourceView(resource, &srv, cpuSlot(slot));
  };
  auto typedSrv = [&](ID3D12Resource *resource, UINT elements, UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_UINT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer = {0, elements, 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(resource, &srv, cpuSlot(slot));
  };
  rawSrv(R.Get(), 4 * kTile / 4, 0);
  typedSrv(R.Get(), 4 * kTile / 4, 1);
  rawSrv(committedBuffer.Get(), 16, 2);
  typedSrv(committedBuffer.Get(), 16, 3);
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4 * kTile / 4;
    device->CreateUnorderedAccessView(R2.Get(), nullptr, &uav, cpuSlot(4));
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(R2.Get(), nullptr, &uav, cpuSlot(5));
  }

  /* ---- Phase 1: map across two heaps, NULL tile, CopyTileMappings alias, residency status ---- */
  {
    const D3D12_TILED_RESOURCE_COORDINATE start{};
    const D3D12_TILE_REGION_SIZE whole{4, FALSE, 0, 0, 0}, first3{3, FALSE, 0, 0, 0};
    const D3D12_TILE_RANGE_FLAGS flags[2] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NULL};
    const UINT offsets[2] = {0, 0}, counts[2] = {2, 1};
    // Tiles 0,1 -> heap A tiles 0,1; tile 2 NULL; tile 3 -> heap B tile 0 (two calls, two heaps).
    queue->UpdateTileMappings(R.Get(), 1, &start, &first3, heapA.Get(), 2, flags, offsets, counts,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    const D3D12_TILED_RESOURCE_COORDINATE tile3{3, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
    const UINT zero = 0;
    queue->UpdateTileMappings(R.Get(), 1, &tile3, &one, heapB.Get(), 1, nullptr, &zero, nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    queue->CopyTileMappings(R2.Get(), &start, R.Get(), &start, &whole, D3D12_TILE_MAPPING_FLAG_NONE);
  }
  const auto output = c.buffer(D3D12_HEAP_TYPE_DEFAULT, 8192, "output");
  const auto outputReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, 8192, "output readback");
  const auto tileReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, kTile, "tile readback");
  c.reset();
  list->CopyBufferRegion(committedBuffer.Get(), 0, uploadSmall.Get(), 0, 64);
  dispatchWrite(R.Get(), 4, 0x100, 16);
  dispatchWrite(R.Get(), 4, 0x200, 32);
  transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list, committedBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(bufferRoot.Get());
  list->SetPipelineState(bufferPso.Get());
  list->SetComputeRootDescriptorTable(0, gpuSlot(0));
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  list->Dispatch(5, 1, 1);
  uavBarrier(list);
  transition(list, R.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, 8192);
  {
    // CopyTiles: tiled buffer tile 0 -> linear buffer.
    const D3D12_TILED_RESOURCE_COORDINATE start{};
    const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
    list->CopyTiles(R.Get(), &start, &one, tileReadback.Get(), 0,
                    D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  }
  transition(list, R.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute("phase 1 close");
  c.finish("phase 1");
  {
    Words w(outputReadback);
    const UINT values[4] = {0x100, 0x101, 0, 0x103}, seconds[4] = {0x200, 0x201, 0, 0x203};
    const UINT mapped[4] = {1, 1, 0, 1};
    for (UINT t = 0; t < 4; t++) {
      const UINT *row = &w.data[t * 12];
      expect("raw SRV value", row[0], values[t]);
      expect("raw SRV status", row[1], mapped[t]);
      expect("typed SRV value", row[2], values[t]);
      expect("typed SRV status", row[3], mapped[t]);
      expect("typed UAV alias value", row[4], values[t]);
      expect("typed UAV alias status", row[5], mapped[t]);
      expect("raw UAV alias value", row[6], seconds[t]);
      expect("raw UAV alias status", row[7], mapped[t]);
      if (t < 3)
        expect("straddling load status", row[8], mapped[t] && mapped[t + 1]);
    }
    expect("committed raw value", w[48], 0x11);
    expect("committed raw status", w[49], 1);
    expect("committed typed value", w[50], 0x22);
    expect("committed typed status", w[51], 1);
  }
  pass("buffer-map-two-heaps");
  pass("buffer-null-tile");
  pass("copy-tile-mappings");
  pass("buffer-residency-status");
  {
    Words w(tileReadback);
    expect("CopyTiles tile->linear +16", w[4], 0x100);
    expect("CopyTiles tile->linear +32", w[8], 0x200);
  }
  pass("copy-tiles-buffer-to-linear");

  /* ---- Phase 2: mapping update exactly between two ExecuteCommandLists; invalid updates dropped ---- */
  const auto bufferReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, 4 * kTile, "buffer readback");
  c.reset();
  dispatchWrite(R.Get(), 4, 0x300, 48);
  c.execute("phase 2a close");
  {
    // Swap tiles 0 and 1 between heap A pages in one call (two regions, two ranges).
    const D3D12_TILED_RESOURCE_COORDINATE starts[2] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    const D3D12_TILE_REGION_SIZE sizes[2] = {{1, FALSE, 0, 0, 0}, {1, FALSE, 0, 0, 0}};
    const UINT offsets[2] = {1, 0}, counts[2] = {1, 1};
    queue->UpdateTileMappings(R.Get(), 2, starts, sizes, heapA.Get(), 2, nullptr, offsets, counts,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    // Invalid: region outside the resource, and a range outside the heap. Both are dropped.
    const D3D12_TILED_RESOURCE_COORDINATE outside{4, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
    const UINT zero = 0, beyond = 9;
    queue->UpdateTileMappings(R.Get(), 1, &outside, &one, heapA.Get(), 1, nullptr, &zero, nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    const D3D12_TILED_RESOURCE_COORDINATE tile3{3, 0, 0, 0};
    queue->UpdateTileMappings(R.Get(), 1, &tile3, &one, heapA.Get(), 1, nullptr, &beyond, nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    // Eviction and residency of the tile pool keep its contents.
    ID3D12Pageable *pageables[] = {heapA.Get(), heapB.Get(), R.Get()};
    check(device->Evict(3, pageables), "Evict tile heaps");
    check(device->MakeResident(3, pageables), "MakeResident tile heaps");
  }
  c.reset();
  transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(bufferReadback.Get(), 0, R.Get(), 0, 4 * kTile);
  transition(list, R.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute("phase 2b close");
  c.finish("phase 2");
  {
    Words w(bufferReadback);
    auto word = [&](UINT tile, UINT byte) { return w[(tile * kTile + byte) / 4]; };
    expect("swapped tile 0 +16", word(0, 16), 0x101);
    expect("swapped tile 0 +48", word(0, 48), 0x301);
    expect("swapped tile 1 +16", word(1, 16), 0x100);
    expect("swapped tile 1 +48", word(1, 48), 0x300);
    for (UINT byte : {16u, 32u, 48u})
      expect("NULL tile copy reads zero", word(2, byte), 0);
    expect("tile 3 unchanged by invalid updates +16", word(3, 16), 0x103);
    expect("tile 3 unchanged by invalid updates +48", word(3, 48), 0x303);
  }
  pass("mapping-update-at-execute-boundary");
  pass("invalid-update-dropped");
  pass("evict-make-resident");

  /* ---- Phase 3: REUSE_SINGLE_TILE aliases every tile to one heap page ---- */
  {
    const D3D12_TILE_RANGE_FLAGS reuse = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
    const UINT offset = 3, count = 4;
    queue->UpdateTileMappings(R3.Get(), 1, nullptr, nullptr, heapB.Get(), 1, &reuse, &offset, &count,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  c.reset();
  dispatchWrite(R3.Get(), 1, 0x500, 64);
  transition(list, R3.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(bufferReadback.Get(), 0, R3.Get(), 0, 4 * kTile);
  c.execute("phase 3 close");
  c.finish("phase 3");
  {
    Words w(bufferReadback);
    std::printf("OWNED_D3D12_TILED_REUSE words=%x,%x,%x,%x\n", w[64 / 4], w[(kTile + 64) / 4], w[(2 * kTile + 64) / 4],
                w[(3 * kTile + 64) / 4]);
    for (UINT t = 0; t < 4; t++)
      expect("REUSE_SINGLE_TILE alias", w[(t * kTile + 64) / 4], 0x500);
  }
  pass("reuse-single-tile");

  /* ---- Phase 4: mapping update on a second queue between fence Wait/Signal ---- */
  c.reset();
  dispatchWrite(R.Get(), 4, 0x600, 80);
  c.execute("phase 4a close");
  const UINT64 produced = ++c.value;
  check(queue->Signal(c.fence.Get(), produced), "phase 4 signal");
  check(c.queue2->Wait(c.fence.Get(), produced), "compute queue wait");
  {
    // Tile 2 -> heap B tile 1, tile 3 -> NULL: the first list must still see the old mapping.
    const D3D12_TILED_RESOURCE_COORDINATE start{2, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE two{2, FALSE, 0, 0, 0};
    const D3D12_TILE_RANGE_FLAGS flags[2] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NULL};
    const UINT offsets[2] = {1, 0}, counts[2] = {1, 1};
    c.queue2->UpdateTileMappings(R.Get(), 1, &start, &two, heapB.Get(), 2, flags, offsets, counts,
                                 D3D12_TILE_MAPPING_FLAG_NONE);
  }
  const UINT64 remapped = ++c.value;
  check(c.queue2->Signal(c.fence.Get(), remapped), "compute queue signal");
  check(queue->Wait(c.fence.Get(), remapped), "direct queue wait");
  {
    // The probe buffer views heap B tile 0, which backed R tile 3 before the update.
    const D3D12_TILED_RESOURCE_COORDINATE start{};
    const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
    const UINT zero = 0;
    queue->UpdateTileMappings(R4.Get(), 1, &start, &one, heapB.Get(), 1, nullptr, &zero, nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  c.reset();
  dispatchWrite(R.Get(), 4, 0x700, 96);
  transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(bufferReadback.Get(), 0, R.Get(), 0, 4 * kTile);
  list->CopyBufferRegion(tileReadback.Get(), 0, R4.Get(), 0, kTile);
  transition(list, R.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute("phase 4b close");
  c.finish("phase 4");
  {
    Words w(bufferReadback);
    auto word = [&](UINT tile, UINT byte) { return w[(tile * kTile + byte) / 4]; };
    expect("cross-queue tile 0 before", word(0, 80), 0x600);
    expect("cross-queue tile 0 after", word(0, 96), 0x700);
    expect("cross-queue tile 1 after", word(1, 96), 0x701);
    expect("cross-queue newly mapped tile 2", word(2, 96), 0x702);
    expect("cross-queue unmapped tile 3 +80", word(3, 80), 0);
    expect("cross-queue unmapped tile 3 +96", word(3, 96), 0);
    Words probe(tileReadback);
    expect("old page of tile 3 kept the pre-update write", probe[80 / 4], 0x603);
    expect("old page of tile 3 kept its earlier data", probe[16 / 4], 0x103);
  }
  pass("cross-queue-mapping-order");

  /* ---- Phase 5: reserved 2D texture, packed mips, texture residency status, sampler reduction ---- */
  const auto reservedTextureDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 1, 10,
                                               DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
  ComPtr<ID3D12Resource> T;
  check(device->CreateReservedResource(&reservedTextureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&T)),
        "reserved texture");
  UINT totalTiles = 0, subresourceCount = 10;
  D3D12_PACKED_MIP_INFO packed{};
  D3D12_TILE_SHAPE shape{};
  D3D12_SUBRESOURCE_TILING tilings[10]{};
  device->GetResourceTiling(T.Get(), &totalTiles, &packed, &shape, &subresourceCount, 0, tilings);
  std::printf("OWNED_D3D12_TILED_TEXTURE_TILING total=%u standard_mips=%u packed_mips=%u packed_tiles=%u "
              "packed_start=%u shape=%ux%ux%u\n",
              totalTiles, packed.NumStandardMips, packed.NumPackedMips, packed.NumTilesForPackedMips,
              packed.StartTileIndexInOverallResource, shape.WidthInTexels, shape.HeightInTexels, shape.DepthInTexels);
  {
    expect("texture shape width", shape.WidthInTexels, 128);
    expect("texture shape height", shape.HeightInTexels, 128);
    expect("texture shape depth", shape.DepthInTexels, 1);
    expect("mip count", packed.NumStandardMips + packed.NumPackedMips, 10);
    require(packed.NumStandardMips >= 3 && packed.NumPackedMips >= 1, "standard/packed split");
    require(packed.NumTilesForPackedMips >= 1, "packed tiles");
    expect("subresource count", subresourceCount, 10);
    UINT next = 0;
    for (UINT mip = 0; mip < 10; mip++) {
      if (mip < packed.NumStandardMips) {
        const UINT size = 512 >> mip, tiles = (size + 127) / 128;
        expect("mip tiles x", tilings[mip].WidthInTiles, tiles);
        expect("mip tiles y", tilings[mip].HeightInTiles, tiles);
        expect("mip tiles z", tilings[mip].DepthInTiles, 1);
        expect("mip first tile", tilings[mip].StartTileIndexInOverallResource, next);
        next += tiles * tiles;
      } else {
        expect("packed subresource", tilings[mip].StartTileIndexInOverallResource, D3D12_PACKED_TILE);
      }
    }
    expect("packed start", packed.StartTileIndexInOverallResource, next);
    expect("total tiles", totalTiles, next + packed.NumTilesForPackedMips);
    require(next + packed.NumTilesForPackedMips <= 24, "texture heap size");
  }
  pass("texture-tiling");
  const UINT packedMip = packed.NumStandardMips;
  const UINT packedHeapTile = 16;
  {
    // Mip 0: tiles 0-4 -> 0-4, tile 5 NULL, 6-9 -> 6-9, tile 10 skipped (never mapped), 11-15 -> 11-15.
    const D3D12_TILED_RESOURCE_COORDINATE start{};
    const D3D12_TILE_REGION_SIZE mip0{16, FALSE, 0, 0, 0};
    const D3D12_TILE_RANGE_FLAGS flags[5] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NULL,
                                             D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_SKIP,
                                             D3D12_TILE_RANGE_FLAG_NONE};
    const UINT offsets[5] = {0, 0, 6, 0, 11}, counts[5] = {5, 1, 4, 1, 5};
    queue->UpdateTileMappings(T.Get(), 1, &start, &mip0, heapT.Get(), 5, flags, offsets, counts,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    // The packed mips by subresource coordinate of the first packed mip.
    const D3D12_TILED_RESOURCE_COORDINATE tail{0, 0, 0, packedMip};
    const D3D12_TILE_REGION_SIZE tailSize{packed.NumTilesForPackedMips, FALSE, 0, 0, 0};
    queue->UpdateTileMappings(T.Get(), 1, &tail, &tailSize, heapT.Get(), 1, nullptr, &packedHeapTile, nullptr,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  // Upload mip 0 (color per tile), the first packed mip, a committed 4x4 texture and a 2x2 R16F texture.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT mip0Footprint{}, packedFootprint{};
  UINT64 mip0Bytes = 0, packedBytes = 0;
  device->GetCopyableFootprints(&reservedTextureDesc, 0, 1, 0, &mip0Footprint, nullptr, nullptr, &mip0Bytes);
  device->GetCopyableFootprints(&reservedTextureDesc, packedMip, 1, 0, &packedFootprint, nullptr, nullptr,
                                &packedBytes);
  const auto committedTextureDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 1,
                                                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_TEXTURE_LAYOUT_UNKNOWN);
  const auto reductionDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 2, 2, 1, 1, DXGI_FORMAT_R16_FLOAT,
                                         D3D12_TEXTURE_LAYOUT_UNKNOWN);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT committedFootprint{}, reductionFootprint{};
  const UINT64 packedOffset = (mip0Bytes + 511) & ~UINT64(511);
  const UINT64 committedOffset = (packedOffset + packedBytes + 511) & ~UINT64(511);
  UINT64 committedBytes = 0, reductionBytes = 0;
  device->GetCopyableFootprints(&committedTextureDesc, 0, 1, committedOffset, &committedFootprint, nullptr, nullptr,
                                &committedBytes);
  const UINT64 reductionOffset = (committedOffset + committedBytes + 511) & ~UINT64(511);
  device->GetCopyableFootprints(&reductionDesc, 0, 1, reductionOffset, &reductionFootprint, nullptr, nullptr,
                                &reductionBytes);
  packedFootprint.Offset = packedOffset;
  const auto textureUpload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, reductionOffset + reductionBytes, "texture upload");
  constexpr UINT kPackedColor = 0x11223344, kCommittedColor = 0x80402010;
  {
    BYTE *data = nullptr;
    check(textureUpload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "map texture upload");
    for (UINT y = 0; y < 512; y++)
      for (UINT x = 0; x < 512; x++)
        reinterpret_cast<UINT *>(data + mip0Footprint.Offset + y * mip0Footprint.Footprint.RowPitch)[x] =
            packColor((y / 128) * 4 + x / 128);
    for (UINT y = 0; y < packedFootprint.Footprint.Height; y++)
      for (UINT x = 0; x < packedFootprint.Footprint.Width; x++)
        reinterpret_cast<UINT *>(data + packedOffset + y * packedFootprint.Footprint.RowPitch)[x] = kPackedColor;
    for (UINT y = 0; y < 4; y++)
      for (UINT x = 0; x < 4; x++)
        reinterpret_cast<UINT *>(data + committedOffset + y * committedFootprint.Footprint.RowPitch)[x] =
            kCommittedColor;
    const uint16_t halves[4] = {0x3000 /* 0.125 */, 0x3b00 /* 0.875 */, 0x3800 /* 0.5 */, 0x3400 /* 0.25 */};
    for (UINT y = 0; y < 2; y++)
      std::memcpy(data + reductionOffset + y * reductionFootprint.Footprint.RowPitch, halves + y * 2, 4);
    textureUpload->Unmap(0, nullptr);
  }
  const auto committedTexture = c.committed(D3D12_HEAP_TYPE_DEFAULT, committedTextureDesc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, "committed texture");
  const auto reductionTexture = c.committed(D3D12_HEAP_TYPE_DEFAULT, reductionDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            "reduction texture");
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = UINT(-1);
    device->CreateShaderResourceView(T.Get(), &srv, cpuSlot(8));
    device->CreateShaderResourceView(committedTexture.Get(), &srv, cpuSlot(9));
    srv.Format = DXGI_FORMAT_R16_FLOAT;
    device->CreateShaderResourceView(reductionTexture.Get(), &srv, cpuSlot(10));
  }
  auto copyToTexture = [&](ID3D12Resource *texture, UINT subresource, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp) {
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = textureUpload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = fp;
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = subresource;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  };
  auto dispatchTextureStatus = [&](UINT64 outputOffset) {
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(textureRoot.Get());
    list->SetPipelineState(texturePso.Get());
    list->SetComputeRootDescriptorTable(0, gpuSlot(8));
    list->SetComputeRoot32BitConstants(1, 1, &packedMip, 0);
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress() + outputOffset);
    list->Dispatch(19, 1, 1);
    uavBarrier(list);
  };
  c.reset();
  copyToTexture(T.Get(), 0, mip0Footprint);
  copyToTexture(T.Get(), packedMip, packedFootprint);
  copyToTexture(committedTexture.Get(), 0, committedFootprint);
  copyToTexture(reductionTexture.Get(), 0, reductionFootprint);
  transition(list, T.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list, committedTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list, reductionTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  dispatchTextureStatus(0);
  c.execute("phase 5a close");
  {
    // Remap between lists: tile 0 -> NULL, tile 5 (box at 1,1) -> heap tile 5, packed mips -> NULL.
    const D3D12_TILED_RESOURCE_COORDINATE starts[3] = {{0, 0, 0, 0}, {1, 1, 0, 0}, {0, 0, 0, packedMip}};
    const D3D12_TILE_REGION_SIZE sizes[3] = {{1, FALSE, 0, 0, 0}, {1, TRUE, 1, 1, 1},
                                             {packed.NumTilesForPackedMips, FALSE, 0, 0, 0}};
    const D3D12_TILE_RANGE_FLAGS flags[3] = {D3D12_TILE_RANGE_FLAG_NULL, D3D12_TILE_RANGE_FLAG_NONE,
                                             D3D12_TILE_RANGE_FLAG_NULL};
    const UINT offsets[3] = {0, 5, 0}, counts[3] = {1, 1, packed.NumTilesForPackedMips};
    queue->UpdateTileMappings(T.Get(), 3, starts, sizes, heapT.Get(), 3, flags, offsets, counts,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  }
  c.reset();
  dispatchTextureStatus(4096);
  transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, 8192);
  transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute("phase 5b close");
  c.finish("phase 5");
  {
    Words w(outputReadback);
    for (UINT t = 0; t < 19; t++)
      std::printf("OWNED_D3D12_TILED_TEXTURE_ROW %u before=%08x,%08x,%08x,%08x after=%08x,%08x,%08x,%08x\n", t,
                  w[t * 4], w[t * 4 + 1], w[t * 4 + 2], w[t * 4 + 3], w[1024 + t * 4], w[1024 + t * 4 + 1],
                  w[1024 + t * 4 + 2], w[1024 + t * 4 + 3]);
    for (UINT t = 0; t < 16; t++) {
      const UINT *row = &w.data[t * 4];
      const bool mapped = t != 5 && t != 10;
      expect("texture Load value", row[0], mapped ? packColor(t) : 0);
      expect("texture Load status", row[1], mapped);
      expect("texture SampleLevel status|equal", row[2], mapped ? 3 : 2);
      expect("texture GatherRed status|red", row[3], mapped ? (1 | ((t * 16) << 8)) : 0);
    }
    expect("packed mip Load value", w[64], kPackedColor);
    expect("packed mip Load status", w[65], 1);
    expect("packed mip SampleLevel value", w[66], kPackedColor);
    expect("packed mip SampleLevel status", w[67], 1);
    expect("committed texture Load value", w[68], kCommittedColor);
    expect("committed texture Load status", w[69], 1);
    expect("committed texture SampleLevel value", w[70], kCommittedColor);
    expect("committed texture SampleLevel status", w[71], 1);
    float reduced[3];
    std::memcpy(reduced, &w.data[72], sizeof(reduced));
    std::printf("OWNED_D3D12_TILED_REDUCTION min=%.4f max=%.4f average=%.4f\n", reduced[0], reduced[1], reduced[2]);
    require(std::fabs(reduced[0] - 0.125f) < 1e-3f, "minimum reduction");
    require(std::fabs(reduced[1] - 0.875f) < 1e-3f, "maximum reduction");
    require(std::fabs(reduced[2] - 0.4375f) < 1e-3f, "average reduction");

    const UINT *after = &w.data[4096 / 4];
    expect("remapped-away tile 0 Load value", after[0], 0);
    expect("remapped-away tile 0 Load status", after[1], 0);
    expect("newly mapped tile 5 Load status", after[5 * 4 + 1], 1);
    expect("still unmapped tile 10 Load status", after[10 * 4 + 1], 0);
    expect("unchanged tile 15 Load value", after[15 * 4], packColor(15));
    expect("unmapped packed mip Load value", after[64], 0);
    expect("unmapped packed mip Load status", after[65], 0);
    expect("unmapped packed mip SampleLevel status", after[67], 0);
  }
  pass("texture-null-tile");
  pass("texture-packed-mips");
  pass("texture-residency-status");
  pass("texture-remap-at-execute-boundary");
  pass("min-max-reduction");

  /*
   * ---- Phase 5b: SM 5.0 DXBC ld_raw_s / ld_s (texture, typed buffer) / check_access_fully_mapped ----
   * Wine's d3dcompiler cannot emit the feedback opcodes, so tiled-dxbc-status.dxbc is FXC-style cs_5_0
   * bytecode with the loads rewritten to their *_FEEDBACK forms (see tiled.hlsl). Group g reads byte
   * g*64KB+16 of R, texel (g*128+5, 7) of T and element g*16384+4 of R as R32_UINT, and stores
   * uint4(raw, texel.x, typed, g) at g*32 and uint4(mapped raw, mapped texel, mapped typed, 0) at g*32+16.
   */
  {
    D3D12_DESCRIPTOR_RANGE dxbcRanges[2]{};
    dxbcRanges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    dxbcRanges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3};
    D3D12_ROOT_PARAMETER dxbcParam{};
    dxbcParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    dxbcParam.DescriptorTable = {2, dxbcRanges};
    const auto dxbcRoot = rootSignature(device, {1, &dxbcParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    const auto dxbcPso = computePipeline(device, dxbcRoot.Get(), dxbcCs);
    rawSrv(R.Get(), 4 * kTile / 4, 12);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = UINT(-1);
    device->CreateShaderResourceView(T.Get(), &srv, cpuSlot(13));
    typedSrv(R.Get(), 4 * kTile / 4, 14);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 8192 / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpuSlot(15));
    c.reset();
    transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(dxbcRoot.Get());
    list->SetPipelineState(dxbcPso.Get());
    list->SetComputeRootDescriptorTable(0, gpuSlot(12));
    list->Dispatch(4, 1, 1);
    uavBarrier(list);
    transition(list, R.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, 8192);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("DXBC close");
    c.finish("DXBC");
    Words w(outputReadback);
    // R after phase 4: tile 0 -> heap A page 1 (0x101 at +16), tile 1 -> page 0 (0x100), tile 2 -> heap B
    // page 1 (never written at +16, so its value is not checked), tile 3 NULL. T mip 0 row 0: tile 0 NULL.
    const UINT raw[4] = {0x101, 0x100, 0, 0}, rawMapped[4] = {1, 1, 1, 0}, texMapped[4] = {0, 1, 1, 1};
    for (UINT g = 0; g < 4; g++) {
      std::printf("OWNED_D3D12_TILED_DXBC group=%u words=%08x,%08x,%08x,%08x status=%u,%u,%u\n", g, w[g * 8],
                  w[g * 8 + 1], w[g * 8 + 2], w[g * 8 + 3], w[g * 8 + 4], w[g * 8 + 5], w[g * 8 + 6]);
      expect("DXBC group", w[g * 8 + 3], g);
      if (g != 2) {
        expect("DXBC ld_raw_s value", w[g * 8], raw[g]);
        expect("DXBC ld_s typed buffer value", w[g * 8 + 2], raw[g]);
      }
      expect("DXBC ld_raw_s status", w[g * 8 + 4], rawMapped[g] ? ~0u : 0);
      expect("DXBC ld_s typed buffer status", w[g * 8 + 6], rawMapped[g] ? ~0u : 0);
      // Red of packColor(g) is g*16/255: zero exactly for the NULL tile 0 and for no mapped tile.
      require((w[g * 8 + 1] != 0) == bool(texMapped[g]), "DXBC ld_s texture value");
      expect("DXBC ld_s texture status", w[g * 8 + 5], texMapped[g] ? ~0u : 0);
    }
  }
  pass("dxbc-feedback-status");

  /*
   * ---- Phase 5c: FXC-compiled SM 5.0 feedback (every *_FEEDBACK opcode) and root/RGB32 status ----
   * R (after phase 4): tiles 0,1,2 mapped, 3 NULL. R2 (phase 1 CopyTileMappings): tiles 0,1,3 mapped,
   * 2 NULL. T mip 0 row 0: tile 0 NULL (phase 5 remap), tiles 1-3 mapped.
   */
  {
    const bool rMapped[4] = {true, true, true, false}, r2Mapped[4] = {true, true, false, true};
    const bool tMapped[4] = {false, true, true, true};
    // Committed helpers: 4x MSAA RGBA8 (cleared), R32_FLOAT 512x512, a 512x8 render target.
    auto msaaDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D12_TEXTURE_LAYOUT_UNKNOWN);
    msaaDesc.SampleDesc.Count = 4;
    msaaDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto msaa = c.committed(D3D12_HEAP_TYPE_DEFAULT, msaaDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, "msaa");
    auto rtDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_TEXTURE_LAYOUT_UNKNOWN);
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto target = c.committed(D3D12_HEAP_TYPE_DEFAULT, rtDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, "target");
    const auto depthDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                       D3D12_TEXTURE_LAYOUT_UNKNOWN);
    const auto depth = c.committed(D3D12_HEAP_TYPE_DEFAULT, depthDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   "depth");
    const auto psOut = c.buffer(D3D12_HEAP_TYPE_DEFAULT, 256, "ps output");
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtvs;
    check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvs)), "rtv heap");
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto msaaRtv = rtv;
    msaaRtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(msaa.Get(), nullptr, msaaRtv);

    // Table at slot 16: t0 T, t1 structured(R), t2 MSAA, t3 (root), t4 R32F; u0 output, u1 typed UAV of R2.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = UINT(-1);
    device->CreateShaderResourceView(T.Get(), &srv, cpuSlot(16));
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Buffer = {0, 4 * kTile / 16, 16, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(R.Get(), &srv, cpuSlot(17));
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    device->CreateShaderResourceView(msaa.Get(), &srv, cpuSlot(18));
    rawSrv(committedBuffer.Get(), 16, 19); // t3 is a root SRV in the root signature; slot unused
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D = {0, 1, 0, 0};
    device->CreateShaderResourceView(depth.Get(), &srv, cpuSlot(20));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.NumElements = 8192 / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpuSlot(21));
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.Buffer.NumElements = 4 * kTile / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(R2.Get(), nullptr, &uav, cpuSlot(22));
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.NumElements = 256 / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(psOut.Get(), nullptr, &uav, cpuSlot(23));

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 4};
    ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 5};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {3, ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {3, 0};
    // Pixel shader: u1 is psOut (slot 23), the rest as above.
    D3D12_DESCRIPTOR_RANGE psRanges[3]{};
    psRanges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    psRanges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 4};
    psRanges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 7};
    D3D12_ROOT_PARAMETER psParams[3] = {params[0], params[1], {}};
    psParams[0].DescriptorTable = {3, psRanges};
    psParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC fxcSamplers[2]{};
    for (UINT i = 0; i < 2; i++) {
      fxcSamplers[i].Filter = i ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
      fxcSamplers[i].AddressU = fxcSamplers[i].AddressV = fxcSamplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      fxcSamplers[i].MaxLOD = D3D12_FLOAT32_MAX;
      fxcSamplers[i].MaxAnisotropy = 1;
      fxcSamplers[i].ShaderRegister = i;
      fxcSamplers[i].ComparisonFunc = i ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_NEVER;
    }
    const auto fxcRoot = rootSignature(device, {2, params, 2, fxcSamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    const auto fxcPso = computePipeline(device, fxcRoot.Get(), fxcCs);
    const auto psRoot = rootSignature(device, {2, psParams, 2, fxcSamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx{};
    gfx.pRootSignature = psRoot.Get();
    gfx.VS = {fxcVs.data(), fxcVs.size()};
    gfx.PS = {fxcPs.data(), fxcPs.size()};
    gfx.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gfx.SampleMask = UINT_MAX;
    gfx.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gfx.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gfx.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gfx.NumRenderTargets = 1;
    gfx.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gfx.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> psPso;
    check(device->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(&psPso)), "fxc graphics pipeline");

    c.reset();
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(msaaRtv, clear, 0, nullptr);
    transition(list, msaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    constexpr auto kAnyShader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kAnyShader);
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(fxcRoot.Get());
    list->SetPipelineState(fxcPso.Get());
    list->SetComputeRootDescriptorTable(0, gpuSlot(16));
    list->SetComputeRootShaderResourceView(1, R->GetGPUVirtualAddress());
    list->Dispatch(4, 1, 1);
    uavBarrier(list);
    // Pixel shader: implicit-LOD sample/sample_b/gather4/sample_c feedback on one pixel per tile.
    list->SetGraphicsRootSignature(psRoot.Get());
    list->SetPipelineState(psPso.Get());
    list->SetGraphicsRootDescriptorTable(0, gpuSlot(16));
    list->SetGraphicsRootShaderResourceView(1, R->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 512, 8, 0, 1};
    const D3D12_RECT scissor{0, 0, 512, 8};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    uavBarrier(list);
    transition(list, R.Get(), kAnyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, psOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, 4096);
    list->CopyBufferRegion(outputReadback.Get(), 4096, psOut.Get(), 0, 256);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("fxc close");
    c.finish("fxc");
    Words w(outputReadback);
    for (UINT g = 0; g < 4; g++) {
      const UINT *row = &w.data[g * 16];
      const UINT *ps = &w.data[1024 + g * 8];
      std::printf("OWNED_D3D12_TILED_FXC group=%u cs=%u%u%u%u,%u%u%u%u,%u%u%u%u ps=%u%u%u%u%u values=%08x,%08x,%08x\n",
                  g, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[12], row[13], row[14], row[15],
                  ps[0], ps[1], ps[2], ps[3], ps[4], row[8], row[9], row[10]);
      const UINT t = tMapped[g];
      expect("sample_l_s", row[0], t);
      expect("sample_d_cl_s", row[1], t);
      expect("gather4_s", row[2], t);
      expect("ld_structured_s", row[3], rMapped[g]);
      expect("ld_uav_typed_s (R2)", row[4], r2Mapped[g]);
      expect("ld_raw_s through a root SRV", row[5], rMapped[g]);
      expect("ld_ms_s (committed)", row[6], 1);
      expect("ld_s", row[7], t);
      expect("sample_c_lz_s (committed)", row[12], 1);
      expect("gather4_c_s (committed)", row[13], 1);
      expect("gather4_po_s", row[14], t);
      expect("gather4_po_c_s (committed)", row[15], 1);
      expect("sample_cl_s (ps)", ps[0], t);
      expect("sample_b_cl_s (ps)", ps[1], t);
      expect("gather4_s (ps)", ps[2], t);
      expect("sample_cl_s clamp 1 (ps): mip 1 is unmapped", ps[3], 0);
      expect("sample_c_cl_s (ps, committed)", ps[4], 1);
      if (g < 2)
        expect("root SRV value", row[10], g ? 0x100 : 0x101);
    }
  }
  pass("fxc-feedback-status");

  /*
   * ---- Phase 5d: DXIL status through root SRV/UAV descriptors and an RGB32 (emulated) typed SRV ----
   * Root: SRV = R (tiles 0,1,2 mapped, 3 NULL), UAV = R2 (tiles 0,1,3 mapped, 2 NULL); then a second
   * dispatch rebinds the SRV to a committed buffer (fully mapped) to prove the status is per binding.
   */
  {
    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[0].Descriptor = {0, 1};
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[1].Descriptor = {1, 1};
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[2].Descriptor = {0, 1};
    const auto rootViewRoot = rootSignature(device, {3, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    const auto rootViewPso = computePipeline(device, rootViewRoot.Get(), rootStatusCs);

    D3D12_DESCRIPTOR_RANGE rgbRanges[2]{};
    rgbRanges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2, 0};
    rgbRanges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2, 1};
    D3D12_ROOT_PARAMETER rgbParam{};
    rgbParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rgbParam.DescriptorTable = {2, rgbRanges};
    const auto rgbRoot = rootSignature(device, {1, &rgbParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    const auto rgbPso = computePipeline(device, rgbRoot.Get(), rgb32StatusCs);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32G32B32_UINT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer = {0, 4 * kTile / 12, 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(R.Get(), &srv, cpuSlot(24));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 256 / 4;
    uav.Buffer.FirstElement = 1024 / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpuSlot(25));

    c.reset();
    constexpr auto kAnyShader = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kAnyShader);
    list->SetComputeRootSignature(rootViewRoot.Get());
    list->SetPipelineState(rootViewPso.Get());
    list->SetComputeRootShaderResourceView(0, R->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, R2->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    list->Dispatch(4, 1, 1);
    uavBarrier(list);
    // Committed SRV: the same shader and UAV, output at byte 64 (rows 4..: after the 4 groups above).
    list->SetComputeRootShaderResourceView(0, committedBuffer->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress() + 64);
    list->Dispatch(1, 1, 1);
    uavBarrier(list);
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rgbRoot.Get());
    list->SetPipelineState(rgbPso.Get());
    list->SetComputeRootDescriptorTable(0, gpuSlot(24));
    list->Dispatch(4, 1, 1);
    uavBarrier(list);
    transition(list, R.Get(), kAnyShader, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, 2048);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("root status close");
    c.finish("root status");
    Words w(outputReadback);
    const bool rMapped[4] = {true, true, true, false}, r2Mapped[4] = {true, true, false, true};
    for (UINT g = 0; g < 4; g++) {
      const UINT *row = &w.data[g * 4];
      const UINT *rgb = &w.data[256 + g * 4];
      std::printf("OWNED_D3D12_TILED_ROOT_STATUS group=%u root_srv=%08x,%u root_uav=%08x,%u rgb32=%08x,%u\n", g,
                  row[0], row[1], row[2], row[3], rgb[0], rgb[3]);
      expect("root SRV status", row[1], rMapped[g]);
      expect("root UAV status", row[3], r2Mapped[g]);
      expect("RGB32 SRV status", rgb[3], rMapped[g]);
      if (!rMapped[g])
        expect("RGB32 SRV NULL tile value", rgb[0], 0);
    }
    const UINT *committedRow = &w.data[64 / 4]; // second dispatch, group 0: committed buffer byte 16
    std::printf("OWNED_D3D12_TILED_ROOT_STATUS committed root_srv=%08x,%u\n", committedRow[0], committedRow[1]);
    expect("committed root SRV value", committedRow[0], 0x44);
    expect("committed root SRV status", committedRow[1], 1);
  }
  pass("root-view-and-rgb32-status");

  /* ---- Phase 6: CopyTiles linear -> tiled -> linear round trip, unmapped tile copies as zero ---- */
  const auto patternUpload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, 2 * kTile, "pattern upload");
  const auto patternReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, 3 * kTile, "pattern readback");
  {
    UINT *data = nullptr;
    check(patternUpload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "map pattern");
    for (UINT i = 0; i < 2 * kTile / 4; i++)
      data[i] = 0x9e3779b9u * (i + 1);
    patternUpload->Unmap(0, nullptr);
  }
  c.reset();
  transition(list, T.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  transition(list, R.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  {
    const D3D12_TILED_RESOURCE_COORDINATE textureTile{2, 0, 0, 0}, bufferTile{1, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
    list->CopyTiles(T.Get(), &textureTile, &one, patternUpload.Get(), 0,
                    D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    list->CopyTiles(R.Get(), &bufferTile, &one, patternUpload.Get(), kTile,
                    D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    transition(list, T.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, R.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTiles(T.Get(), &textureTile, &one, patternReadback.Get(), 0,
                    D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    list->CopyTiles(R.Get(), &bufferTile, &one, patternReadback.Get(), kTile,
                    D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    const D3D12_TILED_RESOURCE_COORDINATE unmapped{2, 2, 0, 0}; // mip 0 tile 10
    list->CopyTiles(T.Get(), &unmapped, &one, patternReadback.Get(), 2 * kTile,
                    D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  }
  transition(list, R.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute("phase 6 close");
  c.finish("phase 6");
  {
    Words w(patternReadback);
    for (UINT i = 0; i < 2 * kTile / 4; i++)
      if (w[i] != 0x9e3779b9u * (i + 1))
        expect("CopyTiles round trip", w[i], 0x9e3779b9u * (i + 1));
    for (UINT i = 2 * kTile / 4; i < 3 * kTile / 4; i++)
      if (w[i])
        expect("CopyTiles from unmapped tile", w[i], 0);
  }
  pass("copy-tiles-round-trip");
  pass("copy-tiles-unmapped-reads-zero");

  /* Full mip-chain arrays: per-slice packed mappings, independent blit and shader readback. */
  {
    constexpr UINT arrayMips = 9, arraySlices = 3, rows = arrayMips * arraySlices;
    auto desc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, arraySlices, 0,
                            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    ComPtr<ID3D12Resource> array;
    check(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&array)),
          "full-chain reserved array");
    desc = array->GetDesc();
    expect("array normalized mip count", desc.MipLevels, arrayMips);
    UINT total = 0, count = rows;
    D3D12_PACKED_MIP_INFO tail{};
    D3D12_TILE_SHAPE arrayShape{};
    D3D12_SUBRESOURCE_TILING subs[rows]{};
    device->GetResourceTiling(array.Get(), &total, &tail, &arrayShape, &count, 0, subs);
    expect("array subresource count", count, rows);
    expect("array standard shape", arrayShape.WidthInTexels, 128);
    require(tail.NumPackedMips && tail.NumTilesForPackedMips, "array has packed tails");
    expect("array mip split", tail.NumStandardMips + tail.NumPackedMips, arrayMips);
    expect("array equal slice tiles", total % arraySlices, 0);
    const UINT stride = total / arraySlices;
    expect("array tail ends each slice", tail.StartTileIndexInOverallResource + tail.NumTilesForPackedMips, stride);
    for (UINT s = 0; s < arraySlices; s++) {
      UINT next = s * stride;
      for (UINT mip = 0; mip < arrayMips; mip++) {
        const auto &sub = subs[s * arrayMips + mip];
        if (mip < tail.NumStandardMips) {
          const UINT extent = ((256 >> mip) + 127) / 128;
          expect("array subresource start", sub.StartTileIndexInOverallResource, next);
          expect("array subresource width", sub.WidthInTiles, extent);
          expect("array subresource height", sub.HeightInTiles, extent);
          next += extent * extent;
        } else {
          expect("array packed sentinel", sub.StartTileIndexInOverallResource, D3D12_PACKED_TILE);
        }
      }
      expect("array tail offset", next, s * stride + tail.StartTileIndexInOverallResource);
    }
    const auto arrayHeap = makeHeap(total, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES, "array heap");
    const D3D12_TILED_RESOURCE_COORDINATE origin{};
    const D3D12_TILE_REGION_SIZE entire{total, FALSE, 0, 0, 0};
    const UINT zero = 0;
    queue->UpdateTileMappings(array.Get(), 1, &origin, &entire, arrayHeap.Get(), 1, nullptr, &zero, &total,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[rows]{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, rows, 0, footprints, nullptr, nullptr, &bytes);
    const auto upload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, bytes, "array upload");
    const auto blitReadback = c.buffer(D3D12_HEAP_TYPE_READBACK, bytes, "array independent readback");
    const UINT64 mutationOffset = (bytes + 511) & ~UINT64(511);
    const auto mutation = c.buffer(D3D12_HEAP_TYPE_UPLOAD, mutationOffset + bytes, "array null and alias write upload");
    BYTE *mutationData = nullptr;
    check(mutation->Map(0, nullptr, reinterpret_cast<void **>(&mutationData)), "array mutation map");
    constexpr UINT nullWriteColor = 0x55667788, aliasWriteColor = 0x87654321;
    for (UINT half = 0; half < 2; half++)
      for (const auto &fp : footprints)
        for (UINT y = 0; y < fp.Footprint.Height; y++)
          for (UINT x = 0; x < fp.Footprint.Width; x++)
            reinterpret_cast<UINT *>(mutationData + half * mutationOffset + fp.Offset + y * fp.Footprint.RowPitch)[x] =
                half ? aliasWriteColor : nullWriteColor;
    mutation->Unmap(0, nullptr);
    BYTE *data = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "array upload map");
    for (UINT row = 0; row < rows; row++) {
      const auto &fp = footprints[row];
      for (UINT y = 0; y < fp.Footprint.Height; y++)
        for (UINT x = 0; x < fp.Footprint.Width; x++)
          reinterpret_cast<UINT *>(data + fp.Offset + y * fp.Footprint.RowPitch)[x] =
              0xff000000u | ((row + 1) * 0x010101u);
    }
    upload->Unmap(0, nullptr);
    c.reset();
    for (UINT row = 0; row < rows; row++) {
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = footprints[row];
      dst.pResource = array.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = row;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list, array.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 4, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 4, 1};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 4};
    D3D12_STATIC_SAMPLER_DESC arraySamplers[2] = {samplers[0], samplers[3]};
    for (auto &sampler : arraySamplers) sampler.RegisterSpace = 4;
    const auto root = rootSignature(device, {3, params, 2, arraySamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    const auto pso = computePipeline(device, root.Get(), arrayStatusCs);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = arrayMips; srv.Texture2DArray.ArraySize = arraySlices;
    device->CreateShaderResourceView(array.Get(), &srv, cpuSlot(31));
    auto observe = [&](UINT phase) {
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get());
      list->SetComputeRootDescriptorTable(0, gpuSlot(31));
      list->SetComputeRoot32BitConstants(1, 1, &arrayMips, 0);
      list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress() + phase * rows * 32);
      list->Dispatch(arrayMips, arraySlices, 1); uavBarrier(list);
    };
    observe(0);
    c.execute("array initial consumers");
    const D3D12_TILED_RESOURCE_COORDINATE slice1{0, 0, 0, arrayMips + tail.NumStandardMips};
    const D3D12_TILED_RESOURCE_COORDINATE slice2{0, 0, 0, 2 * arrayMips + tail.NumStandardMips};
    const D3D12_TILE_REGION_SIZE tailSize{tail.NumTilesForPackedMips, FALSE, 0, 0, 0};
    const D3D12_TILE_RANGE_FLAGS nullFlag = D3D12_TILE_RANGE_FLAG_NULL;
    queue->UpdateTileMappings(array.Get(), 1, &slice1, &tailSize, nullptr, 1, &nullFlag, nullptr,
                              &tail.NumTilesForPackedMips, D3D12_TILE_MAPPING_FLAG_NONE);
    auto writeTail = [&](UINT64 offset) {
      transition(list, array.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      for (UINT mip = tail.NumStandardMips; mip < arrayMips; mip++) {
        const UINT row = arrayMips + mip;
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = mutation.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[row]; src.PlacedFootprint.Offset += offset;
        dst.pResource = array.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = row;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      transition(list, array.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    c.reset(); writeTail(0); observe(1); c.execute("array null tail consumers");
    check(queue->Signal(c.fence.Get(), ++c.value), "array cross queue producer");
    check(c.queue2->Wait(c.fence.Get(), c.value), "array cross queue wait");
    c.queue2->CopyTileMappings(array.Get(), &slice1, array.Get(), &slice2, &tailSize,
                               D3D12_TILE_MAPPING_FLAG_NONE);
    check(c.queue2->Signal(c.fence.Get(), ++c.value), "array cross queue mapped");
    check(queue->Wait(c.fence.Get(), c.value), "array direct queue wait");
    c.reset(); observe(2); c.execute("array copied mapping consumers");
    const D3D12_TILED_RESOURCE_COORDINATE slice0{0, 0, 0, tail.NumStandardMips};
    const UINT oldSlice1Heap = stride + tail.StartTileIndexInOverallResource;
    queue->UpdateTileMappings(array.Get(), 1, &slice0, &tailSize, arrayHeap.Get(), 1, nullptr, &oldSlice1Heap,
                              &tail.NumTilesForPackedMips, D3D12_TILE_MAPPING_FLAG_NONE);
    c.reset(); observe(3); writeTail(mutationOffset); observe(4);
    transition(list, array.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT row = 0; row < rows; row++) {
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = array.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = row;
      dst.pResource = blitReadback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprints[row];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, 5 * rows * 32);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute("array alias consumers"); c.finish("array all consumers");
    Words shader(outputReadback), blit(blitReadback);
    for (UINT phase = 0; phase < 5; phase++)
      for (UINT row = 0; row < rows; row++) {
        const UINT slice = row / arrayMips, mip = row % arrayMips;
        const bool packedSlice1 = slice == 1 && mip >= tail.NumStandardMips;
        const bool mapped = !(phase == 1 && packedSlice1);
        UINT source = phase >= 2 && packedSlice1 ? row + arrayMips : row;
        if (phase >= 3 && slice == 0 && mip >= tail.NumStandardMips)
          source = row + arrayMips;
        UINT color = mapped ? 0xff000000u | ((source + 1) * 0x010101u) : 0;
        if (phase == 4 && slice >= 1 && mip >= tail.NumStandardMips)
          color = aliasWriteColor;
        const UINT *result = &shader.data[(phase * rows + row) * 8];
        for (UINT access = 0; access < 3; access++) {
          expect("array shader color", result[access * 2], color);
          expect("array shader residency", result[access * 2 + 1], mapped);
        }
        if (phase == 4) {
          const auto &fp = footprints[row];
          for (UINT y = 0; y < fp.Footprint.Height; y++)
            for (UINT x = 0; x < fp.Footprint.Width; x++)
              expect("array independent blit color", blit[(fp.Offset + y * fp.Footprint.RowPitch) / 4 + x], color);
        }
      }
    pass("array-full-chain-independent-tail-readback");
    pass("array-null-tail-point-linear-residency");
    pass("array-tail-alias-crossqueue-order");
    pass("array-null-tail-writes-discarded-and-remap-retains-data");
    pass("array-tail-alias-write-visible-through-both-slices");
    for (UINT slice = 0; slice < arraySlices; slice++) {
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> invalid;
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "array packed copy allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&invalid)), "array packed copy list");
      const D3D12_TILED_RESOURCE_COORDINATE packedStart{0, 0, 0, slice * arrayMips + tail.NumStandardMips};
      const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
      invalid->CopyTiles(array.Get(), &packedStart, &one, tileReadback.Get(), 0,
                         D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
      expect("all array tails reject undefined CopyTiles", UINT(invalid->Close()), UINT(E_INVALIDARG));
    }
    pass("array-packed-copy-undefined-for-every-slice");
  }

  /* ---- Phase 7: validation (a list whose Close failed cannot be reset, so each uses a new list) ---- */
  {
    auto invalidCopy = [&](ID3D12Resource *resource, const D3D12_TILED_RESOURCE_COORDINATE &start,
                           D3D12_TILE_COPY_FLAGS flags, const char *what) {
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> invalid;
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), what);
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&invalid)),
            what);
      const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
      invalid->CopyTiles(resource, &start, &one, tileReadback.Get(), 0, flags);
      expect(what, UINT(invalid->Close()), UINT(E_INVALIDARG));
    };
    invalidCopy(R.Get(), {7, 0, 0, 0}, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER,
                "CopyTiles outside the resource: Close");
    invalidCopy(T.Get(), {0, 0, 0, 0},
                D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE |
                    D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER,
                "CopyTiles with both directions: Close");
  }
  pass("copy-tiles-invalid");
  {
    auto rejects = [&](D3D12_RESOURCE_DESC desc, const char *what) {
      ComPtr<ID3D12Resource> resource;
      expect(what, UINT(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&resource))),
             UINT(E_INVALIDARG));
    };
    rejects(textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE1D, 16384, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                        D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE),
            "1D reserved texture");
    auto msaa = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    msaa.SampleDesc.Count = 4;
    msaa.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rejects(msaa, "MSAA reserved texture");
    rejects(textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                        D3D12_TEXTURE_LAYOUT_UNKNOWN),
            "reserved texture without 64KB_UNDEFINED_SWIZZLE");
    ComPtr<ID3D12Resource> array;
    auto arrayDesc = textureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    check(device->CreateReservedResource(&arrayDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&array)),
          "reserved array without packed mips");
    UINT total = 0;
    device->GetResourceTiling(array.Get(), &total, nullptr, nullptr, nullptr, 0, nullptr);
    expect("reserved array tiles", total, 32);
    D3D12_HEAP_PROPERTIES properties{};
    expect("reserved texture GetHeapProperties", UINT(T->GetHeapProperties(&properties, nullptr)),
           UINT(E_INVALIDARG));
  }
  pass("reserved-creation-validation");
  runVolumeCases(c, argc - 11, argv + 11);
  check(device->GetDeviceRemovedReason(), "device removed at end");
  /* The reported capability is checked last so a functional failure is reported first. */
  require(unsigned(options.TiledResourcesTier) == 4, "TiledResourcesTier 4");
  require(!options5.SRVOnlyTiledResourceTier3, "SRVOnlyTiledResourceTier3 unreported");
  require(va.MaxGPUVirtualAddressBitsPerResource >= 40, "per-resource VA bits");
  require(rgba8.Support2 & D3D12_FORMAT_SUPPORT2_TILED, "RGBA8 FORMAT_SUPPORT2_TILED");
  pass("caps");
  CloseHandle(c.event);
  std::printf("OWNED_D3D12_TILED_PASS cases=%d tier=4\n", cases);
  return 0;
}
