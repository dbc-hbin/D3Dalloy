// Enhanced Barriers (ID3D12GraphicsCommandList7::Barrier), ID3D12Device10 layout-based creation with
// castable formats, and the CommandList8/9 dynamic state (front/back stencil reference, dynamic
// depth bias, dynamic index-buffer strip cut) on the owned D3D12 backend.
//
// Every GPU case reads back exact values: a missing barrier lowering, a wrong layout model or a
// wrong dynamic state produces a different number, not only a validation message.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---- Interfaces newer than the llvm-mingw d3d12.h (microsoft/DirectX-Headers adbd6f3b) ----

MIDL_INTERFACE("ee936ef9-599d-4d28-938e-23c4ad05ce51")
ID3D12GraphicsCommandList8 : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT front, UINT back) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce,
                0x51)
MIDL_INTERFACE("34ed2808-ffe6-4c2b-b11a-cabd2b0c59e1")
ID3D12GraphicsCommandList9 : public ID3D12GraphicsCommandList8 {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59,
                0xe1)

MIDL_INTERFACE("14eecffc-4df8-40f7-a118-5c816f45695e")
ID3D12Device13 : public ID3D12Device10 {
  // ID3D12Device11::CreateSampler2 and ID3D12Device12::GetResourceAllocationInfo3 occupy the two
  // slots between Device10 and Device13; they are declared only to keep the vtable order.
  virtual void STDMETHODCALLTYPE CreateSampler2(const void *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) = 0;
  virtual D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo3(
      D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT count, const D3D12_RESOURCE_DESC1 *descs,
      const UINT32 *cast_counts, const DXGI_FORMAT *const *casts, D3D12_RESOURCE_ALLOCATION_INFO1 *infos) = 0;
  virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(const void *address, SIZE_T size, REFIID riid,
                                                                 void **heap) = 0;
};
__CRT_UUID_DECL(ID3D12Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e)
MIDL_INTERFACE("5f6e592d-d895-44c2-8e4a-88ad4926d323")
ID3D12Device14 : public ID3D12Device13 {
  virtual HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT node_mask, const void *blob, SIZE_T size, LPCWSTR name, REFIID riid, void **root) = 0;
};
__CRT_UUID_DECL(ID3D12Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23)

namespace {

constexpr char kPrefix[] = "OWNED_D3D12_ENHANCED_BARRIERS";
constexpr DWORD kTimeoutMs = 15000;
constexpr UINT kDynamicDepthBias = 0x4;       // D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS
constexpr UINT kDynamicStripCut = 0x8;        // D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT
constexpr UINT kSubobjectDepthStencil2 = 26;  // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2
constexpr UINT kSubobjectRasterizer1 = 27;    // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1
constexpr UINT kFeatureOptions12 = 41, kFeatureOptions14 = 43, kFeatureOptions15 = 44, kFeatureOptions16 = 45;
UINT gCases = 0;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", kPrefix, step, static_cast<unsigned long>(hr));
  std::fflush(stdout);
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}
void expect(HRESULT actual, HRESULT wanted, const char *step) {
  if (actual != wanted) {
    std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx expected=0x%08lx\n", kPrefix, step,
                 static_cast<unsigned long>(actual), static_cast<unsigned long>(wanted));
    ExitProcess(1);
  }
}
void pass(const char *name) {
  ++gCases;
  std::printf("%s_CASE_PASS case=%s\n", kPrefix, name);
}

ComPtr<ID3D12Device10> device() {
  ComPtr<ID3D12Device> base;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&base)), "D3D12CreateDevice");
  ComPtr<ID3D12Device10> value;
  check(base.As(&value), "QI ID3D12Device10");
  return value;
}
ComPtr<ID3D12CommandQueue> queue(ID3D12Device *dev, D3D12_COMMAND_LIST_TYPE type) {
  D3D12_COMMAND_QUEUE_DESC desc{};
  desc.Type = type;
  ComPtr<ID3D12CommandQueue> value;
  check(dev->CreateCommandQueue(&desc, IID_PPV_ARGS(&value)), "CreateCommandQueue");
  return value;
}
struct List {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList9> list;
};
List list(ID3D12Device *dev, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
  List value;
  check(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&value.allocator)), "CreateCommandAllocator");
  ComPtr<ID3D12GraphicsCommandList> base;
  check(dev->CreateCommandList(0, type, value.allocator.Get(), nullptr, IID_PPV_ARGS(&base)), "CreateCommandList");
  check(base.As(&value.list), "QI ID3D12GraphicsCommandList9");
  return value;
}
void wait(ID3D12Fence *fence, UINT64 value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event)
    fail("CreateEvent");
  check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
  const DWORD result = WaitForSingleObject(event, kTimeoutMs);
  CloseHandle(event);
  if (result != WAIT_OBJECT_0)
    fail("fence-wait", HRESULT_FROM_WIN32(result));
}
void submit(ID3D12Device *dev, ID3D12CommandQueue *q, ID3D12GraphicsCommandList *l) {
  check(l->Close(), "Close");
  ID3D12CommandList *lists[] = {l};
  q->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  check(q->Signal(fence.Get(), 1), "Signal");
  wait(fence.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "device-healthy");
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES value{};
  value.Type = type;
  value.CreationNodeMask = value.VisibleNodeMask = 1;
  return value;
}
D3D12_RESOURCE_DESC1 bufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC1 desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}
D3D12_RESOURCE_DESC1 textureDesc(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags,
                                 UINT16 mips = 1, UINT16 arrays = 1) {
  D3D12_RESOURCE_DESC1 desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = arrays;
  desc.MipLevels = mips;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  return desc;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device10 *dev, D3D12_HEAP_TYPE type, UINT64 size,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  auto props = heap(type);
  auto desc = bufferDesc(size, flags);
  ComPtr<ID3D12Resource> value;
  check(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr,
                                      nullptr, 0, nullptr, IID_PPV_ARGS(&value)),
        "CreateCommittedResource3(buffer)");
  return value;
}
ComPtr<ID3D12Resource> texture(ID3D12Device10 *dev, const D3D12_RESOURCE_DESC1 &desc, D3D12_BARRIER_LAYOUT layout,
                               const std::vector<DXGI_FORMAT> &casts = {}, const D3D12_CLEAR_VALUE *clear = nullptr) {
  auto props = heap(D3D12_HEAP_TYPE_DEFAULT);
  auto list = casts;
  ComPtr<ID3D12Resource> value;
  check(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &desc, layout, clear, nullptr,
                                      static_cast<UINT32>(list.size()), list.empty() ? nullptr : list.data(),
                                      IID_PPV_ARGS(&value)),
        "CreateCommittedResource3(texture)");
  return value;
}

// ---- barrier builders ----

D3D12_BUFFER_BARRIER bufferBarrier(ID3D12Resource *resource, D3D12_BARRIER_SYNC sb, D3D12_BARRIER_SYNC sa,
                                   D3D12_BARRIER_ACCESS ab, D3D12_BARRIER_ACCESS aa) {
  return {sb, sa, ab, aa, resource, 0, UINT64_MAX};
}
D3D12_TEXTURE_BARRIER textureBarrier(ID3D12Resource *resource, D3D12_BARRIER_SYNC sb, D3D12_BARRIER_SYNC sa,
                                     D3D12_BARRIER_ACCESS ab, D3D12_BARRIER_ACCESS aa, D3D12_BARRIER_LAYOUT lb,
                                     D3D12_BARRIER_LAYOUT la,
                                     D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) {
  D3D12_TEXTURE_BARRIER value{sb, sa, ab, aa, lb, la, resource, {}, flags};
  value.Subresources.IndexOrFirstMipLevel = 0xffffffffu;  // all subresources
  return value;
}
void barrier(ID3D12GraphicsCommandList7 *l, const D3D12_BUFFER_BARRIER &b) {
  D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
  group.pBufferBarriers = &b;
  l->Barrier(1, &group);
}
void barrier(ID3D12GraphicsCommandList7 *l, const D3D12_TEXTURE_BARRIER &b) {
  D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
  group.pTextureBarriers = &b;
  l->Barrier(1, &group);
}
void barrier(ID3D12GraphicsCommandList7 *l, const D3D12_GLOBAL_BARRIER &b) {
  D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_GLOBAL, 1};
  group.pGlobalBarriers = &b;
  l->Barrier(1, &group);
}

ComPtr<ID3DBlob> compile(const char *source, const char *entry, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "enhanced-barriers.hlsl", nullptr, nullptr, entry, profile,
                                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail(entry, hr);
  }
  return code;
}
ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device *dev, const D3D12_ROOT_SIGNATURE_DESC &desc) {
  ComPtr<ID3DBlob> blob, errors;
  check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "SerializeRootSignature");
  ComPtr<ID3D12RootSignature> value;
  check(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&value)),
        "CreateRootSignature");
  return value;
}

// Readback of a buffer through a copy (the copy itself is ordered by an enhanced barrier).
std::vector<uint32_t> readBuffer(ID3D12Resource *readback, size_t words) {
  void *mapped = nullptr;
  D3D12_RANGE range{0, words * 4};
  check(readback->Map(0, &range, &mapped), "Map");
  std::vector<uint32_t> out(words);
  std::memcpy(out.data(), mapped, words * 4);
  readback->Unmap(0, nullptr);
  return out;
}

// ---- compute: UAV -> UAV / UAV -> SRV ordering across dispatches ----

const char kComputeSource[] = R"hlsl(
RWStructuredBuffer<uint> Data : register(u0);
StructuredBuffer<uint> Source : register(t0);
cbuffer Constants : register(b0) { uint mode; uint salt; };
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
  if (mode == 0)       // producer
    Data[id.x] = id.x * 3 + salt;
  else if (mode == 1)  // read-modify-write of the previous dispatch's result (UAV -> UAV)
    Data[id.x] = Data[id.x] * 2 + 1;
  else                 // consume through the SRV (UAV -> SRV)
    Data[id.x] = Source[id.x] + 7;
}
)hlsl";

struct ComputePipeline {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso;
  explicit ComputePipeline(ID3D12Device *dev) {
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].Constants.Num32BitValues = 2;
    D3D12_ROOT_SIGNATURE_DESC desc{3, params};
    root = rootSignature(dev, desc);
    auto cs = compile(kComputeSource, "csMain", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    check(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateComputePipelineState");
  }
  void dispatch(ID3D12GraphicsCommandList *l, ID3D12Resource *uav, ID3D12Resource *srv, UINT mode, UINT salt,
                UINT groups) {
    l->SetPipelineState(pso.Get());
    l->SetComputeRootSignature(root.Get());
    l->SetComputeRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(1, srv->GetGPUVirtualAddress());
    const UINT constants[] = {mode, salt};
    l->SetComputeRoot32BitConstants(2, 2, constants, 0);
    l->Dispatch(groups, 1, 1);
  }
};

void computeOrdering(bool split) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  ComputePipeline pipeline(dev.Get());
  constexpr UINT kWords = 4096, kGroups = kWords / 64;
  auto a = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, kWords * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto b = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, kWords * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, kWords * 4);
  auto l = list(dev.Get());
  constexpr UINT kSalt = 0x1234;
  pipeline.dispatch(l.list.Get(), a.Get(), b.Get(), 0, kSalt, kGroups);
  // UAV -> UAV on the same buffer.
  barrier(l.list.Get(), bufferBarrier(a.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                      D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
  pipeline.dispatch(l.list.Get(), a.Get(), b.Get(), 1, 0, kGroups);
  // UAV -> SRV, optionally as a split barrier with unrelated work in between.
  if (split) {
    barrier(l.list.Get(), bufferBarrier(a.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_SPLIT,
                                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
    barrier(l.list.Get(), bufferBarrier(a.Get(), D3D12_BARRIER_SYNC_SPLIT, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
  } else {
    barrier(l.list.Get(), bufferBarrier(a.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                        D3D12_BARRIER_SYNC_NON_PIXEL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
  }
  pipeline.dispatch(l.list.Get(), b.Get(), a.Get(), 2, 0, kGroups);
  // UAV -> copy source, then copy to readback.
  barrier(l.list.Get(), bufferBarrier(b.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COPY,
                                      D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE));
  l.list->CopyBufferRegion(readback.Get(), 0, b.Get(), 0, kWords * 4);
  submit(dev.Get(), q.Get(), l.list.Get());
  const auto data = readBuffer(readback.Get(), kWords);
  for (UINT i = 0; i < kWords; ++i) {
    const uint32_t expected = (i * 3 + kSalt) * 2 + 1 + 7;
    if (data[i] != expected) {
      std::fprintf(stderr, "%s_FAIL case=compute-ordering split=%d index=%u got=%u expected=%u\n", kPrefix, split, i,
                   data[i], expected);
      ExitProcess(1);
    }
  }
  pass(split ? "compute-uav-srv-split" : "compute-uav-uav-srv-copy");
}

// ---- textures: castable formats, layouts, DISCARD, subresource ranges ----

// Readback of one subresource of a texture that is already in a COPY_SOURCE-capable layout.
struct Readback {
  ComPtr<ID3D12Resource> buffer;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};
Readback copyOut(ID3D12Device10 *dev, ID3D12GraphicsCommandList *l, ID3D12Resource *resource, UINT subresource) {
  Readback out;
  const auto desc = resource->GetDesc();
  UINT64 total = 0;
  dev->GetCopyableFootprints(&desc, subresource, 1, 0, &out.footprint, nullptr, nullptr, &total);
  out.buffer = buffer(dev, D3D12_HEAP_TYPE_READBACK, total);
  D3D12_TEXTURE_COPY_LOCATION src{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = subresource;
  D3D12_TEXTURE_COPY_LOCATION dst{out.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = out.footprint;
  l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  return out;
}
template <typename T> T texel(const Readback &r, UINT x, UINT y = 0) {
  void *mapped = nullptr;
  check(r.buffer->Map(0, nullptr, &mapped), "Map(texture readback)");
  T value;
  std::memcpy(&value, static_cast<const char *>(mapped) + r.footprint.Offset + y * r.footprint.Footprint.RowPitch +
                          x * sizeof(T),
              sizeof(T));
  r.buffer->Unmap(0, nullptr);
  return value;
}
// Fill one subresource of a texture in a COPY_DEST-capable layout with a repeated 32-bit value.
ComPtr<ID3D12Resource> copyIn(ID3D12Device10 *dev, ID3D12GraphicsCommandList *l, ID3D12Resource *resource,
                              UINT subresource, uint32_t value) {
  const auto desc = resource->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 total = 0;
  dev->GetCopyableFootprints(&desc, subresource, 1, 0, &fp, &rows, nullptr, &total);
  auto upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, total);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "Map(upload)");
  for (UINT64 i = 0; i < total / 4; ++i)
    static_cast<uint32_t *>(mapped)[i] = value;
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  src.PlacedFootprint = fp;
  D3D12_TEXTURE_COPY_LOCATION dst{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.SubresourceIndex = subresource;
  l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  return upload;
}

const char kCastSource[] = R"hlsl(
Texture2D<uint> Cast : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
[numthreads(4, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) { Out[id.x] = Cast.Load(int3(id.x, 0, 0)); }
)hlsl";

// RGBA8_UNORM texture with castable R32_UINT: copy -> SRV through an enhanced layout transition,
// read back the exact bits through the cast view.
void castableView() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto tex = texture(dev.Get(), textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1, D3D12_RESOURCE_FLAG_NONE),
                     D3D12_BARRIER_LAYOUT_COPY_DEST, {DXGI_FORMAT_R32_UINT});
  auto out = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 16);
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto root = rootSignature(dev.Get(), {2, params});
  auto cs = compile(kCastSource, "csMain", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), {cs->GetBufferPointer(), cs->GetBufferSize()}};
  ComPtr<ID3D12PipelineState> pso;
  check(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "cast-pso");
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> heapSrv;
  check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapSrv)), "cast-heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = DXGI_FORMAT_R32_UINT;  // not in the RGBA8 family: valid only through the castable list
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  dev->CreateShaderResourceView(tex.Get(), &sd, heapSrv->GetCPUDescriptorHandleForHeapStart());
  auto l = list(dev.Get());
  constexpr uint32_t kBits = 0x80ff7f01u;
  auto upload = copyIn(dev.Get(), l.list.Get(), tex.Get(), 0, kBits);
  barrier(l.list.Get(), textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                       D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                       D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
  ID3D12DescriptorHeap *heaps[] = {heapSrv.Get()};
  l.list->SetDescriptorHeaps(1, heaps);
  l.list->SetPipelineState(pso.Get());
  l.list->SetComputeRootSignature(root.Get());
  l.list->SetComputeRootDescriptorTable(0, heapSrv->GetGPUDescriptorHandleForHeapStart());
  l.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  l.list->Dispatch(1, 1, 1);
  barrier(l.list.Get(), bufferBarrier(out.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COPY,
                                      D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE));
  l.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 16);
  submit(dev.Get(), q.Get(), l.list.Get());
  const auto data = readBuffer(readback.Get(), 4);
  for (UINT i = 0; i < 4; ++i)
    if (data[i] != kBits) {
      std::fprintf(stderr, "%s_FAIL case=castable-view index=%u got=%08x\n", kPrefix, i, data[i]);
      ExitProcess(1);
    }
  pass("castable-rgba8-as-r32uint-srv");
}

// Creation contracts of ID3D12Device10 (InitialLayout, castable format lists) and Device12.
void creationContracts() {
  auto dev = device();
  auto props = heap(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> r;
  auto buf = bufferDesc(256);
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &buf, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                       nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "buffer-layout-must-be-undefined");
  auto plain = textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, D3D12_RESOURCE_FLAG_NONE);
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &plain, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                                       nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "rt-layout-needs-allow-render-target");
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &plain, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                                       nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "uav-layout-needs-allow-uav");
  DXGI_FORMAT wrongSize[] = {DXGI_FORMAT_R16_UINT};
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &plain, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                       nullptr, 1, wrongSize, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "cast-unit-size-mismatch");
  DXGI_FORMAT toBlock[] = {DXGI_FORMAT_BC1_UNORM};
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &plain, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                       nullptr, 1, toBlock, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "cast-uncompressed-to-bc");
  auto bc1 = textureDesc(DXGI_FORMAT_BC1_UNORM, 8, 8, D3D12_RESOURCE_FLAG_NONE);
  DXGI_FORMAT bcSrgb[] = {DXGI_FORMAT_BC1_UNORM_SRGB};
  check(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &bc1, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                      nullptr, 1, bcSrgb, IID_PPV_ARGS(&r)),
        "cast-bc-srgb");
  DXGI_FORMAT sameSize[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM};
  check(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &plain, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                                      nullptr, nullptr, 3, sameSize, IID_PPV_ARGS(&r)),
        "cast-same-unit-size");
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &plain, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                       nullptr, 0, nullptr, __uuidof(ID3D12Resource), nullptr),
         S_FALSE, "null-output-validates");
  // Placed variant on a real heap.
  D3D12_HEAP_DESC hd{1 << 20, props, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
  ComPtr<ID3D12Heap> h;
  check(dev->CreateHeap(&hd, IID_PPV_ARGS(&h)), "CreateHeap");
  check(dev->CreatePlacedResource2(h.Get(), 0, &plain, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, 1, sameSize,
                                   IID_PPV_ARGS(&r)),
        "CreatePlacedResource2");
  expect(dev->CreatePlacedResource2(h.Get(), 0, &plain, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, nullptr, 0,
                                    nullptr, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "placed-ds-layout-needs-allow-ds");
  // Sampler feedback regions are not supported.
  auto feedback = plain;
  feedback.SamplerFeedbackMipRegion = {4, 4, 1};
  expect(dev->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &feedback, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                       nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
         E_INVALIDARG, "sampler-feedback-mip-region");
  // Device9 queue creation and Device6 background processing.
  D3D12_COMMAND_QUEUE_DESC qd{};
  ComPtr<ID3D12CommandQueue> q1;
  check(dev->CreateCommandQueue1(&qd, __uuidof(ID3D12Device), IID_PPV_ARGS(&q1)), "CreateCommandQueue1");
  WINBOOL further = TRUE;
  check(dev->SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE_ALLOWED,
                                         D3D12_MEASUREMENTS_ACTION_KEEP_ALL, nullptr, &further),
        "SetBackgroundProcessingMode");
  if (further)
    fail("background-further-measurements");
  expect(dev->SetBackgroundProcessingMode(static_cast<D3D12_BACKGROUND_PROCESSING_MODE>(9),
                                          D3D12_MEASUREMENTS_ACTION_KEEP_ALL, nullptr, nullptr),
         E_INVALIDARG, "background-invalid-mode");
  // Device8 footprints through DESC1 equal the DESC ones.
  auto mips = textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 32, D3D12_RESOURCE_FLAG_NONE, 4);
  D3D12_RESOURCE_DESC classic{mips.Dimension, mips.Alignment, mips.Width, mips.Height, mips.DepthOrArraySize,
                              mips.MipLevels, mips.Format, mips.SampleDesc, mips.Layout, mips.Flags};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT a[4]{}, b[4]{};
  UINT64 ta = 0, tb = 0;
  dev->GetCopyableFootprints(&classic, 0, 4, 0, a, nullptr, nullptr, &ta);
  dev->GetCopyableFootprints1(&mips, 0, 4, 0, b, nullptr, nullptr, &tb);
  if (ta != tb || std::memcmp(a, b, sizeof(a)))
    fail("footprints1-mismatch");
  const auto info = dev->GetResourceAllocationInfo(0, 1, &classic);
  const auto info2 = dev->GetResourceAllocationInfo2(0, 1, &mips, nullptr);
  if (info.SizeInBytes != info2.SizeInBytes || info.Alignment != info2.Alignment)
    fail("allocation-info2-mismatch");
  pass("device10-creation-contracts");
}

// Per-subresource layouts: a barrier on (mip 1, slice 1) must not change the others. A later
// barrier naming the wrong LayoutBefore for an untouched subresource removes the device.
void subresourceLayouts(bool invalid) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto tex = texture(dev.Get(), textureDesc(DXGI_FORMAT_R32_UINT, 8, 8, D3D12_RESOURCE_FLAG_NONE, 2, 2),
                     D3D12_BARRIER_LAYOUT_COPY_DEST);
  auto l = list(dev.Get());
  std::vector<ComPtr<ID3D12Resource>> uploads;
  for (UINT s = 0; s < 4; ++s)
    uploads.push_back(copyIn(dev.Get(), l.list.Get(), tex.Get(), s, 0x100u + s));
  auto b = textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST,
                          D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_DEST,
                          D3D12_BARRIER_LAYOUT_COPY_SOURCE);
  b.Subresources = {1, 1, 1, 1, 0, 1};  // mip 1 of slice 1 = subresource 3
  barrier(l.list.Get(), b);
  auto out = copyOut(dev.Get(), l.list.Get(), tex.Get(), 3);
  submit(dev.Get(), q.Get(), l.list.Get());
  if (texel<uint32_t>(out, 0) != 0x103u)
    fail("subresource-readback");
  // Second submission: subresource 0 is still COPY_DEST, subresource 3 is COPY_SOURCE.
  auto second = list(dev.Get());
  auto check0 = textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                               invalid ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_COPY_DEST,
                               D3D12_BARRIER_ACCESS_COPY_SOURCE,
                               invalid ? D3D12_BARRIER_LAYOUT_COPY_SOURCE : D3D12_BARRIER_LAYOUT_COPY_DEST,
                               D3D12_BARRIER_LAYOUT_COPY_SOURCE);
  check0.Subresources = {0, 1, 0, 1, 0, 1};
  barrier(second.list.Get(), check0);
  auto out0 = copyOut(dev.Get(), second.list.Get(), tex.Get(), 0);
  check(second.list->Close(), "subresource-second-close");
  // The layout left by the first submission is only known to the queue worker, so the mismatch
  // removes the device asynchronously; removal completes every fence.
  ComPtr<ID3D12Fence> removal;
  HANDLE removed = nullptr;
  if (invalid) {
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removal)), "subresource-removal-fence");
    removed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(removal->SetEventOnCompletion(1, removed), "subresource-removal-event");
  }
  ID3D12CommandList *lists[] = {second.list.Get()};
  q->ExecuteCommandLists(1, lists);
  if (invalid) {
    if (WaitForSingleObject(removed, kTimeoutMs) != WAIT_OBJECT_0)
      fail("subresource-removal-wait");
    CloseHandle(removed);
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "subresource-wrong-layout-removal");
    pass("subresource-wrong-layout-removes");
    return;
  }
  ComPtr<ID3D12Fence> fence;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  check(q->Signal(fence.Get(), 1), "signal");
  wait(fence.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "subresource-valid");
  if (texel<uint32_t>(out0, 0) != 0x100u)
    fail("subresource0-readback");
  pass("subresource-range-layouts");
}

// UNDEFINED + DISCARD into RENDER_TARGET, then a clear that must be the only visible content.
void discardTransition() {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto tex = texture(dev.Get(), textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4,
                                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                     D3D12_BARRIER_LAYOUT_UNDEFINED);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtv;
  check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)), "rtv-heap");
  const auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
  dev->CreateRenderTargetView(tex.Get(), nullptr, handle);
  auto l = list(dev.Get());
  barrier(l.list.Get(), textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                                       D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                                       D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                                       D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
  const float color[] = {0.0f, 1.0f, 0.0f, 1.0f};
  l.list->ClearRenderTargetView(handle, color, 0, nullptr);
  barrier(l.list.Get(), textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY,
                                       D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                       D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
  auto out = copyOut(dev.Get(), l.list.Get(), tex.Get(), 0);
  submit(dev.Get(), q.Get(), l.list.Get());
  if (texel<uint32_t>(out, 3, 3) != 0xff00ff00u)
    fail("discard-clear-readback");
  pass("undefined-discard-to-render-target");
}

// Direct queue writes a texture, returns it to COMMON and signals; the copy queue waits and copies
// it out (copy queues only accept COMMON). Without the COMMON transition the copy queue's implicit
// access to a COPY_DEST layout removes the device.
void crossQueueCommon(bool invalid) {
  auto dev = device();
  auto direct = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto copy = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto tex = texture(dev.Get(), textureDesc(DXGI_FORMAT_R32_UINT, 8, 8, D3D12_RESOURCE_FLAG_NONE),
                     D3D12_BARRIER_LAYOUT_COPY_DEST);
  auto producer = list(dev.Get());
  auto upload = copyIn(dev.Get(), producer.list.Get(), tex.Get(), 0, 0xc0ffee00u);
  if (!invalid)
    barrier(producer.list.Get(), textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE,
                                                D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_NO_ACCESS,
                                                D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_COMMON));
  check(producer.list->Close(), "cross-producer-close");
  auto consumer = list(dev.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
  auto out = copyOut(dev.Get(), consumer.list.Get(), tex.Get(), 0);
  check(consumer.list->Close(), "cross-consumer-close");
  ComPtr<ID3D12Fence> gate, done;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "done");
  HANDLE removed = nullptr;
  ComPtr<ID3D12Fence> removalFence;
  if (invalid) {
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removalFence)), "removal-fence");
    removed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(removalFence->SetEventOnCompletion(1, removed), "removal-event");
  }
  ID3D12CommandList *one[] = {producer.list.Get()}, *two[] = {consumer.list.Get()};
  check(copy->Wait(gate.Get(), 1), "copy-wait");
  copy->ExecuteCommandLists(1, two);
  direct->ExecuteCommandLists(1, one);
  check(direct->Signal(gate.Get(), 1), "direct-signal");
  if (invalid) {
    // Device removal signals every fence to UINT64_MAX.
    if (WaitForSingleObject(removed, kTimeoutMs) != WAIT_OBJECT_0)
      fail("cross-queue-removal-wait");
    CloseHandle(removed);
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "cross-queue-removal");
    pass("cross-queue-missing-common-removes");
    return;
  }
  check(copy->Signal(done.Get(), 1), "copy-signal");
  wait(done.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "cross-queue-healthy");
  if (texel<uint32_t>(out, 7, 7) != 0xc0ffee00u)
    fail("cross-queue-readback");
  pass("cross-queue-common-handoff");
}

// Legacy ResourceBarrier and enhanced Barrier on one texture: the hand-off must pass through COMMON.
// The invalid variant leaves the legacy state at PIXEL_SHADER_RESOURCE and still claims COMMON.
void legacyMixing(bool invalid) {
  auto dev = device();
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto props = heap(D3D12_HEAP_TYPE_DEFAULT);
  auto desc1 = textureDesc(DXGI_FORMAT_R32_UINT, 8, 8, D3D12_RESOURCE_FLAG_NONE);
  D3D12_RESOURCE_DESC desc{desc1.Dimension, 0, desc1.Width, desc1.Height, 1, 1, desc1.Format, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, desc1.Flags};
  ComPtr<ID3D12Resource> tex;
  check(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&tex)),
        "legacy-texture");
  auto l = list(dev.Get());
  auto upload = copyIn(dev.Get(), l.list.Get(), tex.Get(), 0, 0x5a5a5a5au);
  D3D12_RESOURCE_BARRIER legacy{};
  legacy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  legacy.Transition = {tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                       invalid ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON};
  l.list->ResourceBarrier(1, &legacy);
  barrier(l.list.Get(), textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                                       D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                       D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
  auto out = copyOut(dev.Get(), l.list.Get(), tex.Get(), 0);
  check(l.list->Close(), "legacy-mixing-close");
  ID3D12CommandList *lists[] = {l.list.Get()};
  q->ExecuteCommandLists(1, lists);
  if (invalid) {
    expect(dev->GetDeviceRemovedReason(), DXGI_ERROR_DEVICE_REMOVED, "legacy-non-common-removal");
    pass("legacy-to-enhanced-without-common-removes");
    return;
  }
  ComPtr<ID3D12Fence> fence;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  check(q->Signal(fence.Get(), 1), "signal");
  wait(fence.Get(), 1);
  expect(dev->GetDeviceRemovedReason(), S_OK, "legacy-mixing-valid");
  if (texel<uint32_t>(out, 0) != 0x5a5a5a5au)
    fail("legacy-mixing-readback");
  pass("legacy-common-to-enhanced");
}

// Barriers that violate the spec fail the recording: Close returns E_INVALIDARG.
void invalidBarriers() {
  auto dev = device();
  auto buf = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto tex = texture(dev.Get(), textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, D3D12_RESOURCE_FLAG_NONE),
                     D3D12_BARRIER_LAYOUT_COMMON);
  struct Case {
    const char *name;
    D3D12_BARRIER_TYPE type;
    D3D12_GLOBAL_BARRIER global;
    D3D12_BUFFER_BARRIER buffer;
    D3D12_TEXTURE_BARRIER texture;
    D3D12_COMMAND_LIST_TYPE queue = D3D12_COMMAND_LIST_TYPE_DIRECT;
  };
  const auto none = D3D12_BARRIER_SYNC_NONE;
  const auto cs = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
  const auto uav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
  std::vector<Case> cases;
  cases.push_back({"sync-none-with-access", D3D12_BARRIER_TYPE_BUFFER, {}, bufferBarrier(buf.Get(), none, cs, uav, uav)});
  cases.push_back({"global-split", D3D12_BARRIER_TYPE_GLOBAL,
                   {cs, D3D12_BARRIER_SYNC_SPLIT, uav, uav}});
  auto partial = bufferBarrier(buf.Get(), cs, cs, uav, uav);
  partial.Offset = 64;
  cases.push_back({"buffer-partial-range", D3D12_BARRIER_TYPE_BUFFER, {}, partial});
  cases.push_back({"access-incompatible-sync", D3D12_BARRIER_TYPE_BUFFER, {},
                   bufferBarrier(buf.Get(), D3D12_BARRIER_SYNC_COPY, cs, uav, uav)});
  cases.push_back({"layout-access-mismatch", D3D12_BARRIER_TYPE_TEXTURE, {}, {},
                   textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, cs, D3D12_BARRIER_ACCESS_COPY_DEST,
                                  D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_COMMON,
                                  D3D12_BARRIER_LAYOUT_RENDER_TARGET)});
  cases.push_back({"discard-with-defined-before", D3D12_BARRIER_TYPE_TEXTURE, {}, {},
                   textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                                  D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                  D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                                  D3D12_TEXTURE_BARRIER_FLAG_DISCARD)});
  cases.push_back({"undefined-with-access", D3D12_BARRIER_TYPE_TEXTURE, {}, {},
                   textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                                  D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                  D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_COPY_SOURCE)});
  cases.push_back({"copy-queue-non-common-layout", D3D12_BARRIER_TYPE_TEXTURE, {}, {},
                   textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                                  D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                  D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_COPY_SOURCE),
                   D3D12_COMMAND_LIST_TYPE_COPY});
  cases.push_back({"compute-queue-render-target", D3D12_BARRIER_TYPE_TEXTURE, {}, {},
                   textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_NONE,
                                  D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_NO_ACCESS,
                                  D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_RENDER_TARGET),
                   D3D12_COMMAND_LIST_TYPE_COMPUTE});
  cases.back().texture.Subresources = {0, 1, 0, 1, 0, 1};
  auto range = textureBarrier(tex.Get(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                              D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                              D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_COPY_SOURCE);
  range.Subresources = {0, 2, 0, 1, 0, 1};  // two mips on a one-mip texture
  cases.push_back({"subresource-range-overflow", D3D12_BARRIER_TYPE_TEXTURE, {}, {}, range});
  for (const auto &c : cases) {
    auto l = list(dev.Get(), c.queue);
    D3D12_BARRIER_GROUP group{c.type, 1};
    if (c.type == D3D12_BARRIER_TYPE_GLOBAL)
      group.pGlobalBarriers = &c.global;
    else if (c.type == D3D12_BARRIER_TYPE_BUFFER)
      group.pBufferBarriers = &c.buffer;
    else
      group.pTextureBarriers = &c.texture;
    l.list->Barrier(1, &group);
    const HRESULT hr = l.list->Close();
    if (hr != E_INVALIDARG) {
      std::fprintf(stderr, "%s_FAIL case=invalid-barrier name=%s close=0x%08lx\n", kPrefix, c.name,
                   static_cast<unsigned long>(hr));
      ExitProcess(1);
    }
  }
  expect(dev->GetDeviceRemovedReason(), S_OK, "invalid-barriers-device-healthy");
  std::printf("%s_INVALID_BARRIERS_PASS rejected=%zu\n", kPrefix, cases.size());
  pass("invalid-barriers-fail-close");
}

// ---- graphics: RT -> SRV across draws, front/back stencil, dynamic depth bias, dynamic strip cut ----

const char kGraphicsSource[] = R"hlsl(
cbuffer Constants : register(b0) { uint mode; float z; };
Texture2D<float4> Previous : register(t0);
float4 vsMain(uint id : SV_VertexID) : SV_Position {
  float2 p;
  if (mode == 2) {  // strip-cut table (see stripCut)
    static const float2 strip[6] = {float2(-1, -1), float2(-1, 3), float2(-0.34, -1),
                                    float2(0.34, -1), float2(1, 3), float2(1, -1)};
    p = id < 6 ? strip[id] : float2(0, 3);
  } else {
    // Full-screen triangle, clockwise (front facing) for mode 0 and counter-clockwise for mode 1.
    uint v = (mode == 1 && id != 0) ? 3 - id : id;
    p = v == 0 ? float2(-1, -1) : v == 1 ? float2(-1, 3) : float2(3, -1);
  }
  return float4(p, z, 1);
}
float4 psColor() : SV_Target { return float4(1, 0, 0, 1); }
float4 psCopy(float4 position : SV_Position) : SV_Target {
  return Previous.Load(int3(position.xy, 0)) + float4(0, 1, 0, 0);
}
)hlsl";

struct StencilFace1 {
  D3D12_STENCIL_OP fail, depthFail, pass;
  D3D12_COMPARISON_FUNC func;
  UINT8 readMask, writeMask;
};
struct DepthStencil2 {
  BOOL depthEnable;
  D3D12_DEPTH_WRITE_MASK depthWrite;
  D3D12_COMPARISON_FUNC depthFunc;
  BOOL stencilEnable;
  StencilFace1 front, back;
  BOOL depthBoundsTestEnable;
};
struct Rasterizer1 {
  D3D12_FILL_MODE fill;
  D3D12_CULL_MODE cull;
  BOOL frontCounterClockwise;
  FLOAT depthBias, depthBiasClamp, slopeScaledDepthBias;
  BOOL depthClip, multisample, antialiasedLine;
  UINT forcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE conservative;
};
template <UINT Type, typename T> struct alignas(void *) Subobject {
  UINT type = Type;
  T data{};
  Subobject() = default;
  explicit Subobject(const T &value) : data(value) {}
};
struct GraphicsStream {
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology{
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
  Subobject<kSubobjectRasterizer1, Rasterizer1> raster;
  Subobject<kSubobjectDepthStencil2, DepthStencil2> depthStencil;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, UINT> flags;
};

struct Graphics {
  ComPtr<ID3D12Device10> dev;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3DBlob> vs, psColor, psCopy;
  explicit Graphics(ComPtr<ID3D12Device10> device) : dev(std::move(device)) {
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 2;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root = rootSignature(dev.Get(), {2, params});
    vs = compile(kGraphicsSource, "vsMain", "vs_5_0");
    psColor = compile(kGraphicsSource, "psColor", "ps_5_0");
    psCopy = compile(kGraphicsSource, "psCopy", "ps_5_0");
  }
  GraphicsStream stream(ID3DBlob *ps, DXGI_FORMAT rt, DXGI_FORMAT ds) const {
    GraphicsStream s;
    s.root.data = root.Get();
    s.vs.data = {vs->GetBufferPointer(), vs->GetBufferSize()};
    if (ps)
      s.ps.data = {ps->GetBufferPointer(), ps->GetBufferSize()};
    s.rtv.data.NumRenderTargets = rt == DXGI_FORMAT_UNKNOWN ? 0 : 1;
    s.rtv.data.RTFormats[0] = rt;
    s.dsv.data = ds;
    s.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, 0,
                     D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
    const StencilFace1 keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                            D3D12_COMPARISON_FUNC_ALWAYS, 0xff, 0xff};
    s.depthStencil.data = {FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, FALSE, keep, keep, FALSE};
    return s;
  }
  ComPtr<ID3D12PipelineState> create(GraphicsStream &s) const {
    ComPtr<ID3D12Device2> dev2;
    check(dev.As(&dev2), "QI ID3D12Device2");
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(s), &s};
    ComPtr<ID3D12PipelineState> pso;
    check(dev2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "CreatePipelineState(stream)");
    return pso;
  }
  void bind(ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso, UINT mode, float z, UINT width, UINT height) {
    l->SetPipelineState(pso);
    l->SetGraphicsRootSignature(root.Get());
    const UINT constants[] = {mode, std::bit_cast<UINT>(z)};
    l->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    D3D12_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    D3D12_RECT sc{0, 0, LONG(width), LONG(height)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
};

D3D12_CPU_DESCRIPTOR_HANDLE view(ID3D12Device *dev, D3D12_DESCRIPTOR_HEAP_TYPE type, ComPtr<ID3D12DescriptorHeap> &heapOut) {
  D3D12_DESCRIPTOR_HEAP_DESC hd{type, 1};
  check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapOut)), "view-heap");
  return heapOut->GetCPUDescriptorHandleForHeapStart();
}

// Draw 1 renders A; an enhanced RENDER_TARGET -> SHADER_RESOURCE barrier orders draw 2, which reads A.
void renderTargetToShaderResource() {
  Graphics g(device());
  auto q = queue(g.dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  const auto rtDesc = textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto a = texture(g.dev.Get(), rtDesc, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
  auto b = texture(g.dev.Get(), rtDesc, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtvA, rtvB;
  auto ha = view(g.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvA);
  auto hb = view(g.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvB);
  g.dev->CreateRenderTargetView(a.Get(), nullptr, ha);
  g.dev->CreateRenderTargetView(b.Get(), nullptr, hb);
  D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> srv;
  check(g.dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&srv)), "srv-heap");
  g.dev->CreateShaderResourceView(a.Get(), nullptr, srv->GetCPUDescriptorHandleForHeapStart());
  auto colorStream = g.stream(g.psColor.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN);
  auto copyStream = g.stream(g.psCopy.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN);
  auto colorPso = g.create(colorStream), copyPso = g.create(copyStream);
  auto l = list(g.dev.Get());
  const float black[] = {0, 0, 0, 0};
  l.list->ClearRenderTargetView(ha, black, 0, nullptr);
  l.list->ClearRenderTargetView(hb, black, 0, nullptr);
  l.list->OMSetRenderTargets(1, &ha, FALSE, nullptr);
  g.bind(l.list.Get(), colorPso.Get(), 0, 0.5f, 4, 4);
  l.list->DrawInstanced(3, 1, 0, 0);
  barrier(l.list.Get(), textureBarrier(a.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                                       D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                       D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
  ID3D12DescriptorHeap *heaps[] = {srv.Get()};
  l.list->SetDescriptorHeaps(1, heaps);
  l.list->OMSetRenderTargets(1, &hb, FALSE, nullptr);
  g.bind(l.list.Get(), copyPso.Get(), 0, 0.5f, 4, 4);
  l.list->SetGraphicsRootDescriptorTable(1, srv->GetGPUDescriptorHandleForHeapStart());
  l.list->DrawInstanced(3, 1, 0, 0);
  barrier(l.list.Get(), textureBarrier(b.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY,
                                       D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                       D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
  auto out = copyOut(g.dev.Get(), l.list.Get(), b.Get(), 0);
  submit(g.dev.Get(), q.Get(), l.list.Get());
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 4; ++x)
      if (texel<uint32_t>(out, x, y) != 0xff00ffffu) {
        std::fprintf(stderr, "%s_FAIL case=rt-to-srv pixel=(%u,%u) got=%08x\n", kPrefix, x, y,
                     texel<uint32_t>(out, x, y));
        ExitProcess(1);
      }
  pass("draw-render-target-to-shader-resource");
}

// OMSetFrontAndBackStencilRef plus DEPTH_STENCIL2 per-face write masks: the front triangle writes
// 0x11 over 0xf0 with mask 0xff, the back triangle writes 0x22 with mask 0x0f -> 0xf2.
void frontBackStencil() {
  Graphics g(device());
  auto q = queue(g.dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  constexpr auto kFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  D3D12_CLEAR_VALUE clear{kFormat};
  clear.DepthStencil = {1.0f, 0};
  auto ds = texture(g.dev.Get(), textureDesc(kFormat, 2, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                    D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, {}, &clear);
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  auto hd = view(g.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  g.dev->CreateDepthStencilView(ds.Get(), nullptr, hd);
  auto s = g.stream(nullptr, DXGI_FORMAT_UNKNOWN, kFormat);
  s.depthStencil.data.stencilEnable = TRUE;
  s.depthStencil.data.front = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE,
                               D3D12_COMPARISON_FUNC_ALWAYS, 0xff, 0xff};
  s.depthStencil.data.back = s.depthStencil.data.front;
  s.depthStencil.data.back.writeMask = 0x0f;
  auto pso = g.create(s);
  auto l = list(g.dev.Get());
  l.list->ClearDepthStencilView(hd, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0xf0, 0, nullptr);
  l.list->OMSetRenderTargets(0, nullptr, FALSE, &hd);
  l.list->OMSetFrontAndBackStencilRef(0x11, 0x22);
  for (UINT side = 0; side < 2; ++side) {
    g.bind(l.list.Get(), pso.Get(), side, 0.5f, 2, 1);
    D3D12_RECT sc{LONG(side), 0, LONG(side + 1), 1};
    l.list->RSSetScissorRects(1, &sc);
    l.list->DrawInstanced(3, 1, 0, 0);
  }
  barrier(l.list.Get(), textureBarrier(ds.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_SYNC_COPY,
                                       D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                       D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
  auto out = copyOut(g.dev.Get(), l.list.Get(), ds.Get(), 1);
  submit(g.dev.Get(), q.Get(), l.list.Get());
  const auto front = texel<uint8_t>(out, 0), back = texel<uint8_t>(out, 1);
  std::printf("%s_STENCIL front=%02x back=%02x\n", kPrefix, front, back);
  if (front != 0x11 || back != 0xf2)
    fail("front-back-stencil");
  pass("front-back-stencil-ref-and-masks");
}

// Depth of one pixel drawn at z=0.25 with the pipeline's or the dynamic depth bias.
float depthWith(Graphics &g, ID3D12PipelineState *pso, const float *dynamic, bool reset) {
  auto q = queue(g.dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  constexpr auto kFormat = DXGI_FORMAT_D32_FLOAT;
  D3D12_CLEAR_VALUE clear{kFormat};
  clear.DepthStencil = {1.0f, 0};
  auto ds = texture(g.dev.Get(), textureDesc(kFormat, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                    D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, {}, &clear);
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  auto hd = view(g.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  g.dev->CreateDepthStencilView(ds.Get(), nullptr, hd);
  auto l = list(g.dev.Get());
  l.list->ClearDepthStencilView(hd, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  l.list->OMSetRenderTargets(0, nullptr, FALSE, &hd);
  g.bind(l.list.Get(), pso, 0, 0.25f, 1, 1);
  if (dynamic)
    l.list->RSSetDepthBias(dynamic[0], dynamic[1], dynamic[2]);
  if (reset)
    l.list->SetPipelineState(pso);  // restores the pipeline's depth bias
  l.list->DrawInstanced(3, 1, 0, 0);
  barrier(l.list.Get(), textureBarrier(ds.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_SYNC_COPY,
                                       D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                       D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
  auto out = copyOut(g.dev.Get(), l.list.Get(), ds.Get(), 0);
  submit(g.dev.Get(), q.Get(), l.list.Get());
  return texel<float>(out, 0);
}

// D3DConf_12_DynamicDepthBias SingleDynamic / PSOReset equivalents, with a fractional FLOAT bias
// from D3D12_RASTERIZER_DESC1.
void dynamicDepthBias() {
  Graphics g(device());
  auto make = [&](float bias, UINT flags) {
    auto s = g.stream(nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT);
    s.depthStencil.data.depthEnable = TRUE;
    s.depthStencil.data.depthWrite = D3D12_DEPTH_WRITE_MASK_ALL;
    s.raster.data.depthBias = bias;
    s.flags.data = flags;
    return g.create(s);
  };
  constexpr float kBias = 40960.5f;
  auto none = make(0, 0), fixed = make(kBias, 0), dynamic = make(0, kDynamicDepthBias),
       dynamicFixed = make(kBias, kDynamicDepthBias);
  const float zero = depthWith(g, none.Get(), nullptr, false);
  const float reference = depthWith(g, fixed.Get(), nullptr, false);
  const float values[] = {kBias, 0, 0};
  const float dynamicValue = depthWith(g, dynamic.Get(), values, false);
  const float clearValues[] = {0, 0, 0};
  const float resetValue = depthWith(g, dynamicFixed.Get(), clearValues, true);
  const float overrideValue = depthWith(g, dynamicFixed.Get(), clearValues, false);
  std::printf("%s_DEPTH_BIAS zero=%.9g reference=%.9g dynamic=%.9g reset=%.9g override=%.9g\n", kPrefix, zero,
              reference, dynamicValue, resetValue, overrideValue);
  if (zero != 0.25f || reference <= zero || dynamicValue != reference || resetValue != reference ||
      overrideValue != zero)
    fail("dynamic-depth-bias");
  // Without the pipeline flag the call fails the recording.
  auto l = list(g.dev.Get());
  l.list->SetPipelineState(fixed.Get());
  l.list->RSSetDepthBias(1, 0, 0);
  expect(l.list->Close(), E_INVALIDARG, "depth-bias-without-flag");
  pass("dynamic-depth-bias");
}

// Triangle strip {0,1,2,0xffff,3,4,5}: with a 0xFFFF cut the middle pixel is never covered; without
// it the strip's (2, 0xffff, 3) triangle covers it.
void stripCut() {
  Graphics g(device());
  auto q = queue(g.dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto s = g.stream(g.psColor.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN);
  s.flags.data = kDynamicStripCut;
  auto pso = g.create(s);  // pipeline IBStripCutValue stays DISABLED
  const uint16_t indices[] = {0, 1, 2, 0xffff, 3, 4, 5};
  auto ib = buffer(g.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(indices));
  void *mapped = nullptr;
  check(ib->Map(0, nullptr, &mapped), "Map(ib)");
  std::memcpy(mapped, indices, sizeof(indices));
  ib->Unmap(0, nullptr);
  D3D12_INDEX_BUFFER_VIEW ibv{ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
  auto run = [&](bool dynamicCut, bool reset) {
    auto rt = texture(g.dev.Get(), textureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 3, 1,
                                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    auto h = view(g.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, h);
    auto l = list(g.dev.Get());
    const float black[] = {0, 0, 0, 0};
    l.list->ClearRenderTargetView(h, black, 0, nullptr);
    l.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
    g.bind(l.list.Get(), pso.Get(), 2, 0.5f, 3, 1);
    l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    l.list->IASetIndexBuffer(&ibv);
    if (dynamicCut)
      l.list->IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    if (reset)
      l.list->SetPipelineState(pso.Get());
    l.list->DrawIndexedInstanced(7, 1, 0, 0, 0);
    barrier(l.list.Get(), textureBarrier(rt.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY,
                                         D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                         D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
    auto out = copyOut(g.dev.Get(), l.list.Get(), rt.Get(), 0);
    submit(g.dev.Get(), q.Get(), l.list.Get());
    return std::array<uint32_t, 3>{texel<uint32_t>(out, 0), texel<uint32_t>(out, 1), texel<uint32_t>(out, 2)};
  };
  const auto cut = run(true, false), none = run(false, false), reset = run(true, true);
  std::printf("%s_STRIP_CUT cut=%08x,%08x,%08x none=%08x reset=%08x\n", kPrefix, cut[0], cut[1], cut[2], none[1],
              reset[1]);
  constexpr uint32_t red = 0xff0000ffu;
  if (cut[0] != red || cut[1] != 0 || cut[2] != red || none[1] != red || reset[1] != red)
    fail("dynamic-strip-cut");
  // Without the pipeline flag the call fails the recording.
  auto plain = g.stream(g.psColor.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN);
  auto plainPso = g.create(plain);
  auto l = list(g.dev.Get());
  l.list->SetPipelineState(plainPso.Get());
  l.list->IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
  expect(l.list->Close(), E_INVALIDARG, "strip-cut-without-flag");
  pass("dynamic-index-buffer-strip-cut");
}

void capabilities() {
  auto dev = device();
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS14 o14{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS15 o15{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
  check(dev->CheckFeatureSupport(D3D12_FEATURE(kFeatureOptions12), &o12, sizeof(o12)), "OPTIONS12");
  check(dev->CheckFeatureSupport(D3D12_FEATURE(kFeatureOptions14), &o14, sizeof(o14)), "OPTIONS14");
  check(dev->CheckFeatureSupport(D3D12_FEATURE(kFeatureOptions15), &o15, sizeof(o15)), "OPTIONS15");
  check(dev->CheckFeatureSupport(D3D12_FEATURE(kFeatureOptions16), &o16, sizeof(o16)), "OPTIONS16");
  std::printf("%s_CAPS enhanced_barriers=%d relaxed_format_casting=%d independent_stencil=%d dynamic_strip_cut=%d "
              "triangle_fan=%d dynamic_depth_bias=%d gpu_upload_heap=%d\n",
              kPrefix, o12.EnhancedBarriersSupported, o12.RelaxedFormatCastingSupported,
              o14.IndependentFrontAndBackStencilRefMaskSupported, o15.DynamicIndexBufferStripCutSupported,
              o15.TriangleFanSupported, o16.DynamicDepthBiasSupported, o16.GPUUploadHeapSupported);
  if (!o12.EnhancedBarriersSupported || !o12.RelaxedFormatCastingSupported ||
      !o14.IndependentFrontAndBackStencilRefMaskSupported || !o15.DynamicIndexBufferStripCutSupported ||
      o15.TriangleFanSupported || !o16.DynamicDepthBiasSupported || !o16.GPUUploadHeapSupported)
    fail("capabilities");
  ComPtr<ID3D12Device14> device14;
  check(dev.As(&device14), "QI ID3D12Device14");
  std::printf("%s_INTERFACES device14=1 command_list9=1\n", kPrefix);
  pass("capabilities");
}

// ---- ID3D12Device13: a heap over application memory, written by the GPU and read by the CPU ----

void heapFromAddress() {
  auto dev = device();
  ComPtr<ID3D12Device13> dev13;
  check(dev.As(&dev13), "QI ID3D12Device13");
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  constexpr SIZE_T kBytes = 4 * 65536, kOffset = 65536;
  auto *memory = static_cast<uint32_t *>(VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!memory)
    fail("VirtualAlloc");
  for (UINT i = 0; i < kBytes / 4; ++i)
    memory[i] = 0xdeadbeefu;
  ComPtr<ID3D12Heap> heapObject;
  expect(dev13->OpenExistingHeapFromAddress1(memory, 0, IID_PPV_ARGS(&heapObject)), E_INVALIDARG,
         "heap-from-address1-zero-size");
  check(dev13->OpenExistingHeapFromAddress1(memory, kBytes, IID_PPV_ARGS(&heapObject)),
        "OpenExistingHeapFromAddress1");
  const auto hd = heapObject->GetDesc();
  if (hd.SizeInBytes != kBytes)
    fail("heap-from-address1-size");
  // A placed buffer at a nonzero offset; the compute producer writes it through the GPU and the
  // application reads the original allocation without any copy.
  // Buffers in an existing-memory heap are cross-adapter resources; placement is 64 KiB aligned.
  auto desc = bufferDesc(4096 * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                       D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
  ComPtr<ID3D12Resource> placed;
  check(dev->CreatePlacedResource2(heapObject.Get(), kOffset, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0,
                                   nullptr, IID_PPV_ARGS(&placed)),
        "CreatePlacedResource2(host heap)");
  ComputePipeline pipeline(dev.Get());
  auto l = list(dev.Get());
  pipeline.dispatch(l.list.Get(), placed.Get(), placed.Get(), 0, 0x77, 4096 / 64);
  submit(dev.Get(), q.Get(), l.list.Get());
  for (UINT i = 0; i < 4096; ++i)
    if (memory[kOffset / 4 + i] != i * 3 + 0x77)
      fail("heap-from-address1-gpu-write");
  if (memory[0] != 0xdeadbeefu || memory[kOffset / 4 - 1] != 0xdeadbeefu ||
      memory[(kOffset + 4096 * 4) / 4] != 0xdeadbeefu)
    fail("heap-from-address1-outside-range");
  placed.Reset();
  heapObject.Reset();
  VirtualFree(memory, 0, MEM_RELEASE);
  pass("device13-heap-from-address1");
}

// ---- ID3D12Device14: a root signature from a DXIL library subobject, used by a real dispatch ----

std::vector<char> readFile(const wchar_t *name) {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!length || length >= MAX_PATH)
    fail("GetModuleFileNameW");
  std::wstring file(path, path + length);
  file.resize(file.find_last_of(L"\\/") + 1);
  file += name;
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    fail("open-library", HRESULT_FROM_WIN32(GetLastError()));
  std::vector<char> data(GetFileSize(h, nullptr));
  DWORD read = 0;
  if (!ReadFile(h, data.data(), DWORD(data.size()), &read, nullptr) || read != data.size())
    fail("read-library");
  CloseHandle(h);
  return data;
}

const char kLibraryRootSource[] = R"hlsl(
cbuffer Constants : register(b0) { uint a; uint b; uint c; };
RWStructuredBuffer<uint> Out : register(u0);
[numthreads(1, 1, 1)] void csMain() { Out[0] = a; Out[1] = b; Out[2] = c; }
)hlsl";

void rootSignatureFromLibrary() {
  auto dev = device();
  ComPtr<ID3D12Device14> dev14;
  check(dev.As(&dev14), "QI ID3D12Device14");
  const auto library = readFile(L"enhanced-barriers-rs-library.dxil");
  ComPtr<ID3D12RootSignature> global, local, ambiguous, missing;
  check(dev14->CreateRootSignatureFromSubobjectInLibrary(0, library.data(), library.size(), L"GlobalRS",
                                                         IID_PPV_ARGS(&global)),
        "RootSignatureFromLibrary(GlobalRS)");
  check(dev14->CreateRootSignatureFromSubobjectInLibrary(0, library.data(), library.size(), L"LocalRS",
                                                         IID_PPV_ARGS(&local)),
        "RootSignatureFromLibrary(LocalRS)");
  expect(dev14->CreateRootSignatureFromSubobjectInLibrary(0, library.data(), library.size(), L"MissingRS",
                                                          IID_PPV_ARGS(&missing)),
         E_INVALIDARG, "root-signature-missing-name");
  expect(dev14->CreateRootSignatureFromSubobjectInLibrary(0, library.data(), library.size(), nullptr,
                                                          IID_PPV_ARGS(&ambiguous)),
         E_INVALIDARG, "root-signature-ambiguous-null-name");
  if (missing || ambiguous)
    fail("root-signature-output-not-cleared");
  // The global root signature (3 root constants at b0, root UAV at u0) drives a dispatch.
  auto q = queue(dev.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto cs = compile(kLibraryRootSource, "csMain", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{global.Get(), {cs->GetBufferPointer(), cs->GetBufferSize()}};
  ComPtr<ID3D12PipelineState> pso;
  check(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "library-root-pso");
  auto out = buffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, 16);
  auto l = list(dev.Get());
  l.list->SetPipelineState(pso.Get());
  l.list->SetComputeRootSignature(global.Get());
  const UINT constants[] = {11, 22, 33};
  l.list->SetComputeRoot32BitConstants(0, 3, constants, 0);
  l.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  l.list->Dispatch(1, 1, 1);
  barrier(l.list.Get(), bufferBarrier(out.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COPY,
                                      D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE));
  l.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 16);
  submit(dev.Get(), q.Get(), l.list.Get());
  const auto data = readBuffer(readback.Get(), 3);
  if (data[0] != 11 || data[1] != 22 || data[2] != 33)
    fail("library-root-dispatch");
  pass("device14-root-signature-from-library");
}

bool requireOwnedBackend() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12"))
    return false;
  std::printf("%s_BACKEND id=%s\n", kPrefix, backend());
  return true;
}

}  // namespace

int main() {
  if (!requireOwnedBackend())
    return 3;
  creationContracts();
  computeOrdering(false);
  computeOrdering(true);
  castableView();
  subresourceLayouts(false);
  subresourceLayouts(true);
  discardTransition();
  crossQueueCommon(false);
  crossQueueCommon(true);
  legacyMixing(false);
  legacyMixing(true);
  invalidBarriers();
  renderTargetToShaderResource();
  frontBackStencil();
  dynamicDepthBias();
  stripCut();
  heapFromAddress();
  rootSignatureFromLibrary();
  capabilities();
  std::printf("%s_PASS cases=%u\n", kPrefix, gCases);
  return 0;
}
