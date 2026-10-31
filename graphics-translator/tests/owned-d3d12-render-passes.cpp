// ID3D12GraphicsCommandList4 render passes, D3D12_HEAP_TYPE_GPU_UPLOAD, and ID3D12Device11
// CreateSampler2 UINT border colors on the owned D3D12 backend. Every case reads back exact values.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---- ID3D12Device11 (microsoft/DirectX-Headers adbd6f3b), declared over ID3D12Device10 ----
enum : UINT { kSamplerFlagUintBorderColor = 0x1 };
struct SamplerDesc2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union {
    FLOAT FloatBorderColor[4];
    UINT UintBorderColor[4];
  };
  FLOAT MinLOD, MaxLOD;
  UINT Flags;
};
MIDL_INTERFACE("5405c344-d457-444e-b4dd-2366e45aee39")
ID3D12Device11 : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(const SamplerDesc2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) = 0;
};
__CRT_UUID_DECL(ID3D12Device11, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39)

namespace {

constexpr char kPrefix[] = "OWNED_D3D12_RENDER_PASSES";
constexpr DWORD kTimeoutMs = 15000;
constexpr D3D12_HEAP_TYPE kHeapTypeGpuUpload = static_cast<D3D12_HEAP_TYPE>(5);
constexpr D3D12_FEATURE kFeatureOptions16 = static_cast<D3D12_FEATURE>(45);
constexpr D3D12_FEATURE kFeatureOptions18 = static_cast<D3D12_FEATURE>(47);
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

struct Context {
  ComPtr<ID3D12Device11> dev;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 0;
  Context() {
    ComPtr<ID3D12Device> base;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&base)), "D3D12CreateDevice");
    check(base.As(&dev), "QI ID3D12Device11");
    D3D12_COMMAND_QUEUE_DESC qd{};
    check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  }
  struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
  };
  List list() {
    List value;
    check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&value.allocator)), "allocator");
    check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, value.allocator.Get(), nullptr,
                                 IID_PPV_ARGS(&value.list)),
          "CreateCommandList");
    return value;
  }
  void execute(std::initializer_list<ID3D12GraphicsCommandList *> lists) {
    std::vector<ID3D12CommandList *> batch;
    for (auto *l : lists) {
      check(l->Close(), "Close");
      batch.push_back(l);
    }
    executeSealed(batch);
  }
  void executeSealed(const std::vector<ID3D12CommandList *> &batch) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(fence->SetEventOnCompletion(++value, event), "SetEventOnCompletion");
    queue->ExecuteCommandLists(UINT(batch.size()), batch.data());
    check(queue->Signal(fence.Get(), value), "Signal");
    if (WaitForSingleObject(event, kTimeoutMs) != WAIT_OBJECT_0)
      fail("fence-wait");
    CloseHandle(event);
    expect(dev->GetDeviceRemovedReason(), S_OK, "device-healthy");
  }
};

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES value{};
  value.Type = type;
  value.CreationNodeMask = value.VisibleNodeMask = 1;
  return value;
}
D3D12_RESOURCE_DESC texDesc(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags,
                            UINT samples = 1) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.Flags = flags;
  return desc;
}
D3D12_RESOURCE_DESC bufDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}
ComPtr<ID3D12Resource> committed(ID3D12Device *dev, D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC &desc,
                                 D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear = nullptr) {
  auto props = heap(type);
  ComPtr<ID3D12Resource> value;
  check(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&value)),
        "CreateCommittedResource");
  return value;
}
void transition(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  l->ResourceBarrier(1, &b);
}
ComPtr<ID3DBlob> compile(const char *source, const char *entry, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "render-passes.hlsl", nullptr, nullptr, entry, profile,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
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

// Copy one subresource of a texture (in COPY_SOURCE) into a readback buffer.
struct Readback {
  ComPtr<ID3D12Resource> buffer;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  template <typename T> T at(UINT x, UINT y = 0) const {
    void *mapped = nullptr;
    check(buffer->Map(0, nullptr, &mapped), "Map(readback)");
    T value;
    std::memcpy(&value, static_cast<char *>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch +
                            x * sizeof(T),
                sizeof(T));
    buffer->Unmap(0, nullptr);
    return value;
  }
};
Readback copyOut(ID3D12Device *dev, ID3D12GraphicsCommandList *l, ID3D12Resource *resource, UINT subresource = 0) {
  Readback out;
  const auto desc = resource->GetDesc();
  UINT64 total = 0;
  dev->GetCopyableFootprints(&desc, subresource, 1, 0, &out.footprint, nullptr, nullptr, &total);
  out.buffer = committed(dev, D3D12_HEAP_TYPE_READBACK, bufDesc(total), D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION src{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = subresource;
  D3D12_TEXTURE_COPY_LOCATION dst{out.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = out.footprint;
  l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  return out;
}

// ---- shaders ----

const char kSource[] = R"hlsl(
cbuffer Constants : register(b0) { float4 color; float z; uint mode; };
float4 vsMain(uint id : SV_VertexID) : SV_Position {
  // mode 0: full-screen triangle; mode 1: left half only (x < 0).
  float2 p = id == 0 ? float2(-1, -1) : id == 1 ? float2(-1, 3) : float2(mode == 1 ? 0 : 3, -1);
  if (mode == 1 && id == 1) p = float2(-1, 3);
  if (mode == 1 && id == 2) p = float2(0, -1);
  if (mode == 1 && id == 0) p = float2(-1, -1);
  return float4(p, z, 1);
}
// Blends additively, so a lost PRESERVE shows up as a missing term.
float4 psMain() : SV_Target { return color; }
float4 psGreen() : SV_Target { return float4(0, color.b, 0, 0); }
ByteAddressBuffer GraphRead : register(t2);
cbuffer GraphValues : register(b2) { uint graphWord; };
float4 psGraphRead() : SV_Target { return color + float4(graphWord / 255.0, 0, 0, 0); }
float4 psGraphSrv() : SV_Target { return color + float4(GraphRead.Load(0) / 255.0, 0, 0, 0); }
struct GraphMrt { float4 first : SV_Target0; float4 second : SV_Target1; };
GraphMrt psGraphMrt() { GraphMrt result; result.first = color; result.second = float4(0, color.r, 0, 0); return result; }
Texture2D<uint> GraphTexture : register(t3);
RWTexture2D<uint> GraphWrite : register(u2);
float4 psGraphTexture() : SV_Target { return color + float4(GraphTexture.Load(int3(0, 0, 0)) / 255.0, 0, 0, 0); }
[numthreads(1, 1, 1)] void csGraphMip() { GraphWrite[uint2(0, 0)] = 64; }
Texture2D<float> DepthRead : register(t4);
RWStructuredBuffer<float> DepthOut : register(u3);
[numthreads(4, 4, 1)] void csDepthSrv(uint2 id : SV_DispatchThreadID) { DepthOut[id.y * 4 + id.x] = DepthRead.Load(int3(id, 0)); }
// Per-sample distinct values for resolve tests: sample i writes (i + 1) / 8 in red.
float4 psSamples(uint sample : SV_SampleIndex) : SV_Target { return float4((sample + 1) / 8.0, 0, 0, 1); }

Texture2D<uint4> IntTex : register(t0);
SamplerState Border : register(s0);
RWStructuredBuffer<uint4> Out : register(u0);
RWStructuredBuffer<uint> GroupOut : register(u1);
float4 vsUavAtomicAdd(uint id : SV_VertexID) : SV_Position {
  uint old; InterlockedAdd(GroupOut[0], 1, old); return vsMain(id);
}
[numthreads(1, 1, 1)] void csGraphIndependent() { GroupOut[0] = 0x13579bdf; }
float4 psUavProducer() : SV_Target { uint old; InterlockedExchange(GroupOut[0], 32, old); return color; }
float4 psUavProducer64() : SV_Target { uint old; InterlockedExchange(GroupOut[0], 64, old); return color; }
float4 psUavAtomicAdd() : SV_Target { uint old; InterlockedAdd(GroupOut[0], 1, old); return color; }
float4 psUavConsumer() : SV_Target { return float4(0, GroupOut[0] / 255.0, 0, 0); }
Texture2D<float4> Upload : register(t1);
[numthreads(1, 1, 1)] void csBorder() {
  // Sample far outside [0,1] with a point filter: every result is the border color.
  Out[0] = IntTex.SampleLevel(Border, float2(-4, -4), 0);
}
[numthreads(4, 1, 1)] void csUploadTexture(uint3 id : SV_DispatchThreadID) {
  Out[id.x] = uint4(Upload.Load(int3(id.x, 0, 0)) * 255 + 0.5);
}
)hlsl";

struct Draw {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso;
  Draw(ID3D12Device *dev, DXGI_FORMAT rt, DXGI_FORMAT ds, UINT samples, const char *ps, bool additive,
       bool depth_write = false, UINT rootConstants = 6, bool uav = false, bool srv = false, UINT rtCount = 1, bool table = false, bool cbv = false, bool vertex_uav = false) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = rootConstants;
    D3D12_ROOT_PARAMETER params[2] = {param, {}};
    params[1].ParameterType = cbv ? D3D12_ROOT_PARAMETER_TYPE_CBV : srv ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = srv || cbv ? 2 : 1;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0};
    if (table) { params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = {1, &range}; }
    root = rootSignature(dev, {uav || srv || table || cbv ? 2u : 1u, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    auto vs = compile(kSource, vertex_uav ? "vsUavAtomicAdd" : "vsMain", vertex_uav ? "vs_5_1" : "vs_5_0");
    auto pixel = compile(kSource, ps, "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = samples;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rt == DXGI_FORMAT_UNKNOWN ? 0 : rtCount;
    for (UINT i = 0; i < pd.NumRenderTargets; ++i) pd.RTVFormats[i] = rt;
    pd.DSVFormat = ds;
    pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, 0,
                          D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
    auto &blend = pd.BlendState.RenderTarget[0];
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (additive) {
      blend.BlendEnable = TRUE;
      blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
      blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (ds != DXGI_FORMAT_UNKNOWN) {
      pd.DepthStencilState.DepthEnable = TRUE;
      pd.DepthStencilState.DepthWriteMask = depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    }
    check(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");
  }
  void draw(ID3D12GraphicsCommandList *l, UINT w, UINT h, const float color[4], float z = 0.5f, UINT mode = 0,
            const D3D12_VIEWPORT *viewport = nullptr, const D3D12_RECT *scissor = nullptr,
            D3D12_GPU_VIRTUAL_ADDRESS uav = 0, D3D12_GPU_VIRTUAL_ADDRESS srv = 0, D3D12_GPU_DESCRIPTOR_HANDLE table = {}, D3D12_GPU_VIRTUAL_ADDRESS cbv = 0) {
    l->SetPipelineState(pso.Get());
    l->SetGraphicsRootSignature(root.Get());
    const UINT constants[6] = {std::bit_cast<UINT>(color[0]), std::bit_cast<UINT>(color[1]),
                               std::bit_cast<UINT>(color[2]), std::bit_cast<UINT>(color[3]),
                               std::bit_cast<UINT>(z), mode};
    l->SetGraphicsRoot32BitConstants(0, 6, constants, 0);
    if (uav) l->SetGraphicsRootUnorderedAccessView(1, uav);
    if (srv) l->SetGraphicsRootShaderResourceView(1, srv);
    if (table.ptr) l->SetGraphicsRootDescriptorTable(1, table);
    if (cbv) l->SetGraphicsRootConstantBufferView(1, cbv);
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    l->RSSetViewports(1, viewport ? viewport : &vp);
    l->RSSetScissorRects(1, scissor ? scissor : &sc);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);
  }
};

D3D12_CPU_DESCRIPTOR_HANDLE descriptor(ID3D12Device *dev, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                       ComPtr<ID3D12DescriptorHeap> &heapOut, bool visible = false) {
  D3D12_DESCRIPTOR_HEAP_DESC hd{type, 1, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                 : D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
  check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapOut)), "CreateDescriptorHeap");
  return heapOut->GetCPUDescriptorHandleForHeapStart();
}
D3D12_RENDER_PASS_RENDER_TARGET_DESC rtAccess(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                              D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE begin,
                                              D3D12_RENDER_PASS_ENDING_ACCESS_TYPE end, const float *clear = nullptr) {
  D3D12_RENDER_PASS_RENDER_TARGET_DESC d{};
  d.cpuDescriptor = rtv;
  d.BeginningAccess.Type = begin;
  if (clear) {
    d.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::memcpy(d.BeginningAccess.Clear.ClearValue.Color, clear, 16);
  }
  d.EndingAccess.Type = end;
  return d;
}

// Real root CBV reads commute with unrelated copy/compute work, including
// read/read on the copy source. Three sealed recordings replay without edits.
void submissionAccessScheduling(UINT control) {
  Context c;
  constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto input = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(256), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(4), D3D12_RESOURCE_STATE_COPY_DEST);
  auto computeData = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      bufDesc(4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const auto unusedReservedDesc = bufDesc(65536);
  ComPtr<ID3D12Resource> unusedReserved;
  check(c.dev->CreateReservedResource(&unusedReservedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      IID_PPV_ARGS(&unusedReserved)), "CreateReservedResource(graph-unused)");
  void *mapped = nullptr;
  check(input->Map(0, nullptr, &mapped), "Map(graph-input)");
  std::memset(mapped, 0, 256); *static_cast<uint32_t *>(mapped) = 16; input->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  auto initialize = c.list(); const float black[4] = {0, 0, 0, 1};
  initialize.list->ClearRenderTargetView(rtv, black, 0, nullptr); c.execute({initialize.list.Get()});
  Draw draw(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, control == 3 ? "psGraphSrv" : "psGraphRead",
      true, false, 6, false, control == 3, 1, false, control != 3);
  D3D12_ROOT_PARAMETER rootParam{}; rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  rootParam.Descriptor.ShaderRegister = 1;
  auto computeRoot = rootSignature(c.dev.Get(), {1, &rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  auto cs = compile(kSource, "csGraphIndependent", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = computeRoot.Get();
  pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> compute;
  check(c.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&compute)), "Create(graph-compute)");
  auto a = c.list(), b = c.list(), d = c.list(), read = c.list();
  auto *middle = control == 0 ? a.list.Get() : b.list.Get();
  auto *last = control == 0 ? a.list.Get() : d.list.Get();
  const float first[4] = {32 / 255.0f, 0, 0, 0}, second[4] = {16 / 255.0f, 0, 0, 0};
  a.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  draw.draw(a.list.Get(), 4, 4, first, 0.5f, 0, nullptr, nullptr, 0, control == 3 ? input->GetGPUVirtualAddress() : 0, {},
      control == 3 ? 0 : input->GetGPUVirtualAddress());
  if (control == 2) {
    middle->SetPipelineState(compute.Get()); middle->SetComputeRootSignature(computeRoot.Get());
    middle->SetComputeRootUnorderedAccessView(0, computeData->GetGPUVirtualAddress()); middle->Dispatch(1, 1, 1);
  } else middle->CopyBufferRegion(output.Get(), 0, input.Get(), 0, 4);
  last->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  draw.draw(last, 4, 4, second, 0.5f, 0, nullptr, nullptr, 0, control == 3 ? input->GetGPUVirtualAddress() : 0, {},
      control == 3 ? 0 : input->GetGPUVirtualAddress());
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto color = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (control == 2) {
    transition(read.list.Get(), computeData.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    read.list->CopyBufferRegion(output.Get(), 0, computeData.Get(), 0, 4);
    transition(read.list.Get(), computeData.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  check(a.list->Close(), "Close(graph-A)"); check(b.list->Close(), "Close(graph-B)");
  check(d.list->Close(), "Close(graph-C)"); check(read.list->Close(), "Close(graph-read)");
  for (UINT replay = 0; replay < 2; ++replay) {
    c.executeSealed({initialize.list.Get()});
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-access-%u replay=%u\n", kPrefix, control, replay); std::fflush(stderr);
    if (control == 0) c.executeSealed({a.list.Get()});
    else c.executeSealed({a.list.Get(), b.list.Get(), d.list.Get()});
    std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-access-%u replay=%u\n", kPrefix, control, replay); std::fflush(stderr);
    c.executeSealed({read.list.Get()});
    for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x)
      if (color.at<uint32_t>(x, y) != 0xff000050u) fail("graph-resource-read-pixels");
    check(output->Map(0, nullptr, &mapped), "Map(graph-copy)");
    const auto word = *static_cast<const uint32_t *>(mapped); output->Unmap(0, nullptr);
    if (word != (control == 2 ? 0x13579bdfu : 16u)) fail("graph-unrelated-work");
    std::printf("%s_GRAPH_ACCESS control=%u replay=%u rgba=%08x unrelated=%08x render_encoders=1 members=2 exact=1\n",
        kPrefix, control, replay, color.at<uint32_t>(0), word);
  }
  pass(control == 0 ? "graph-same-list-read-copy-read" : control == 1 ? "graph-cross-list-read-read-copy" :
       control == 2 ? "graph-cross-list-independent-compute" : "graph-root-srv-buffer-read");
}

// A large copy into a COMMON buffer in one ExecuteCommandLists and a copy of
// its last word in the next, queued back to back with no CPU wait and no
// barrier: buffer state decays and promotes implicitly across Execute
// boundaries. Each Execute starts with an empty in-plan history, so only queue
// tracking orders them; the long producer leaves a missed dependency visible.
void submissionCrossExecuteProducer() {
  Context c;
  constexpr UINT64 kBytes = 64ull << 20;
  auto input = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(kBytes), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto data = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(kBytes), D3D12_RESOURCE_STATE_COMMON);
  auto output = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(4), D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr;
  check(input->Map(0, nullptr, &mapped), "Map(cross-execute-input)");
  auto *words = static_cast<uint32_t *>(mapped);
  for (UINT64 i = 0; i < kBytes / 4; ++i) words[i] = 0x13579bdfu;
  input->Unmap(0, nullptr);
  auto zeros = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(4), D3D12_RESOURCE_STATE_GENERIC_READ);
  check(zeros->Map(0, nullptr, &mapped), "Map(cross-execute-zero)");
  *static_cast<uint32_t *>(mapped) = 0; zeros->Unmap(0, nullptr);
  auto producer = c.list(), consumer = c.list();
  producer.list->CopyBufferRegion(data.Get(), 0, input.Get(), 0, kBytes);
  consumer.list->CopyBufferRegion(output.Get(), 0, data.Get(), kBytes - 4, 4);
  check(producer.list->Close(), "Close(cross-execute-producer)");
  check(consumer.list->Close(), "Close(cross-execute-consumer)");
  ID3D12CommandList *first[] = {producer.list.Get()}, *second[] = {consumer.list.Get()};
  for (UINT replay = 0; replay < 2; ++replay) {
    // The producer must refill the last word every replay.
    auto reset = c.list();
    reset.list->CopyBufferRegion(data.Get(), kBytes - 4, zeros.Get(), 0, 4);
    c.execute({reset.list.Get()});
    check(output->Map(0, nullptr, &mapped), "Map(cross-execute-reset)");
    *static_cast<uint32_t *>(mapped) = 0; output->Unmap(0, nullptr);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(c.fence->SetEventOnCompletion(++c.value, event), "SetEventOnCompletion(cross-execute)");
    c.queue->ExecuteCommandLists(1, first);
    c.queue->ExecuteCommandLists(1, second);
    check(c.queue->Signal(c.fence.Get(), c.value), "Signal(cross-execute)");
    if (WaitForSingleObject(event, kTimeoutMs) != WAIT_OBJECT_0) fail("cross-execute-wait");
    CloseHandle(event);
    check(output->Map(0, nullptr, &mapped), "Map(cross-execute)");
    const auto word = *static_cast<const uint32_t *>(mapped); output->Unmap(0, nullptr);
    if (word != 0x13579bdfu) {
      std::fprintf(stderr, "%s_FAIL case=cross-execute-producer replay=%u got=%08x expected=13579bdf\n", kPrefix, replay, word);
      std::exit(1);
    }
    std::printf("%s_CROSS_EXECUTE replay=%u word=%08x exact=1\n", kPrefix, replay, word);
  }
  pass("graph-cross-execute-producer");
}

// Each closed list contributes one small, disjoint writer to the same buffer.
// Doubling the sequence must double bounded frontier work, not quadruple it.
void submissionLongFrontier(UINT count) {
  Context c;
  auto input = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(4), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(4 * count), D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr;
  check(input->Map(0, nullptr, &mapped), "Map(frontier-input)");
  *static_cast<UINT *>(mapped) = 0x13579bdfu; input->Unmap(0, nullptr);
  std::vector<Context::List> lists; lists.reserve(count);
  std::vector<ID3D12CommandList *> batch; batch.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    lists.push_back(c.list());
    lists.back().list->CopyBufferRegion(output.Get(), 4 * i, input.Get(), 0, 4);
    check(lists.back().list->Close(), "Close(frontier-copy)");
    batch.push_back(lists.back().list.Get());
  }
  for (UINT replay = 0; replay < 2; ++replay) {
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-frontier-%u replay=%u\n", kPrefix, count, replay); std::fflush(stderr);
    c.executeSealed(batch);
    std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-frontier-%u replay=%u\n", kPrefix, count, replay); std::fflush(stderr);
    check(output->Map(0, nullptr, &mapped), "Map(frontier-output)");
    for (UINT i = 0; i < count; ++i)
      if (static_cast<const UINT *>(mapped)[i] != 0x13579bdfu) fail("frontier-disjoint-copy-values");
    output->Unmap(0, nullptr);
    std::printf("%s_GRAPH_FRONTIER count=%u replay=%u word=13579bdf exact=1 sealed=1\n", kPrefix, count, replay);
  }
  pass(count == 256 ? "graph-frontier-256" : "graph-frontier-512");
}

// Attachment dependencies span node 64. An existing clear-promoted group at
// 62/63 must stay separate from 64/65. The second control instead places the
// pending clear at 63: it must materialize before history is cleared at 64.
void submissionFrontierBoundary(bool delayedClear) {
  Context c; constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto input = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(4), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(256), D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr;
  check(input->Map(0, nullptr, &mapped), "Map(frontier-boundary-input)");
  *static_cast<UINT *>(mapped) = 0x13579bdfu; input->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  Draw draw(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  const float black[4] = {0, 0, 0, 1}, red[4] = {16 / 255.0f, 0, 0, 0};
  std::vector<Context::List> lists; lists.reserve(66);
  std::vector<ID3D12CommandList *> batch; batch.reserve(66);
  for (UINT i = 0; i < 66; ++i) {
    lists.push_back(c.list()); auto *list = lists.back().list.Get();
    if (i == (delayedClear ? 63u : 0u)) list->ClearRenderTargetView(rtv, black, 0, nullptr);
    else if (i >= (delayedClear ? 64u : 62u)) {
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      draw.draw(list, 4, 4, red);
    } else list->CopyBufferRegion(output.Get(), 4 * i, input.Get(), 0, 4);
    check(list->Close(), "Close(frontier-boundary)"); batch.push_back(list);
  }
  auto read = c.list();
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto color = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  check(read.list->Close(), "Close(frontier-boundary-read)");
  for (UINT replay = 0; replay < 2; ++replay) {
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-frontier-boundary-%u replay=%u\n", kPrefix, unsigned(delayedClear), replay); std::fflush(stderr);
    c.executeSealed(batch);
    std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-frontier-boundary-%u replay=%u\n", kPrefix, unsigned(delayedClear), replay); std::fflush(stderr);
    c.executeSealed({read.list.Get()});
    const UINT expected = delayedClear ? 0xff000020u : 0xff000040u;
    for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x)
      if (color.at<UINT>(x, y) != expected) fail("frontier-boundary-overlap-pixels");
    check(output->Map(0, nullptr, &mapped), "Map(frontier-boundary-output)");
    for (UINT i = delayedClear ? 0 : 1; i < (delayedClear ? 63u : 62u); ++i)
      if (static_cast<const UINT *>(mapped)[i] != 0x13579bdfu) fail("frontier-boundary-copy-values");
    output->Unmap(0, nullptr);
    std::printf("%s_GRAPH_FRONTIER_BOUNDARY delayed_clear=%u replay=%u rgba=%08x render_encoders=%u clear_encoders=%u exact=1 sealed=1\n",
        kPrefix, unsigned(delayedClear), replay, color.at<UINT>(0), delayedClear ? 1u : 2u, delayedClear ? 1u : 0u);
  }
  pass(delayedClear ? "graph-frontier-delayed-clear-boundary" : "graph-frontier-overlap-group-boundary");
}

void submissionDisjointMips() {
  Context c; constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto td = texDesc(DXGI_FORMAT_R32_UINT, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); td.MipLevels = 2;
  auto texture = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComPtr<ID3D12DescriptorHeap> rtvHeap, heap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  check(c.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Create(mip-heap)");
  const auto cpu = heap->GetCPUDescriptorHandleForHeapStart(); const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
  const UINT stride = c.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
  c.dev->CreateShaderResourceView(texture.Get(), &srv, cpu);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  for (UINT mip = 0; mip < 2; ++mip) { uav.Texture2D.MipSlice = mip;
    c.dev->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, {cpu.ptr + stride * (mip + 1)}); }
  auto initialize = c.list(), a = c.list(), b = c.list(), d = c.list(), read = c.list();
  ID3D12DescriptorHeap *bound[] = {heap.Get()}; initialize.list->SetDescriptorHeaps(1, bound);
  const UINT initial[4] = {16, 16, 16, 16};
  for (UINT mip = 0; mip < 2; ++mip) initialize.list->ClearUnorderedAccessViewUint(
      {gpu.ptr + stride * (mip + 1)}, {cpu.ptr + stride * (mip + 1)}, texture.Get(), initial, 0, nullptr);
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {texture.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
  initialize.list->ResourceBarrier(1, &barrier);
  const float black[4] = {0, 0, 0, 1}; initialize.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  c.execute({initialize.list.Get()});
  Draw draw(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psGraphTexture", true, false, 6, false, false, 1, true);
  const float first[4] = {32 / 255.0f, 0, 0, 0}, second[4] = {16 / 255.0f, 0, 0, 0};
  a.list->SetDescriptorHeaps(1, bound); a.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  draw.draw(a.list.Get(), 4, 4, first, 0.5f, 0, nullptr, nullptr, 0, 0, gpu);
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0, 0};
  D3D12_ROOT_PARAMETER param{}; param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; param.DescriptorTable = {1, &range};
  auto root = rootSignature(c.dev.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}); auto cs = compile(kSource, "csGraphMip", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get(); pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> compute; check(c.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&compute)), "Create(mip-compute)");
  b.list->SetDescriptorHeaps(1, bound); b.list->SetPipelineState(compute.Get()); b.list->SetComputeRootSignature(root.Get());
  b.list->SetComputeRootDescriptorTable(0, {gpu.ptr + stride * 2}); b.list->Dispatch(1, 1, 1);
  d.list->SetDescriptorHeaps(1, bound); d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  draw.draw(d.list.Get(), 4, 4, second, 0.5f, 0, nullptr, nullptr, 0, 0, gpu);
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto color = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  for (UINT mip = 0; mip < 2; ++mip) {
    barrier.Transition.Subresource = mip; barrier.Transition.StateBefore = mip ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; read.list->ResourceBarrier(1, &barrier);
  }
  auto mip0 = copyOut(c.dev.Get(), read.list.Get(), texture.Get(), 0), mip1 = copyOut(c.dev.Get(), read.list.Get(), texture.Get(), 1);
  std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-mips replay=0\n", kPrefix); std::fflush(stderr);
  c.execute({a.list.Get(), b.list.Get(), d.list.Get()});
  std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-mips replay=0\n", kPrefix); std::fflush(stderr);
  c.execute({read.list.Get()});
  for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x)
    if (color.at<uint32_t>(x, y) != 0xff000050u || mip0.at<uint32_t>(x, y) != 16) fail("graph-disjoint-mip0-read");
  for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x)
    if (mip1.at<uint32_t>(x, y) != ((!x && !y) ? 64u : 16u)) fail("graph-disjoint-mip1-write");
  std::printf("%s_GRAPH_MIPS rgba=%08x mip0=%u mip1=%u mip1_untouched=%u render_encoders=1 members=2 exact=1\n",
      kPrefix, color.at<uint32_t>(0), mip0.at<uint32_t>(0), mip1.at<uint32_t>(0), mip1.at<uint32_t>(1));
  pass("graph-descriptor-disjoint-mip-compute");
}

// A depth clear followed by a compute read of the same depth through an SRV.
// The SRV and the DSV name the same depth plane, so the dispatch must be ordered
// after the clear even though the transition rides on the clear's encoder.
void submissionDepthClearSrvRead() {
  Context c;
  auto ds = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(DXGI_FORMAT_R32_TYPELESS, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_RESOURCE_STATE_DEPTH_WRITE);
  auto out = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      bufDesc(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(64), D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> dsvHeap, heap;
  const auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  D3D12_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  c.dev->CreateDepthStencilView(ds.Get(), &dd, dsv);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  check(c.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Create(depth-srv-heap)");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
  c.dev->CreateShaderResourceView(ds.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 0};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[1].Descriptor.ShaderRegister = 3;
  auto root = rootSignature(c.dev.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  auto cs = compile(kSource, "csDepthSrv", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get(); pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> compute; check(c.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&compute)), "Create(depth-srv)");
  auto initial = c.list(); initial.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
  c.execute({initial.list.Get()});
  auto l = c.list(), read = c.list();
  ID3D12DescriptorHeap *bound[] = {heap.Get()};
  l.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, nullptr);
  transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  l.list->SetDescriptorHeaps(1, bound); l.list->SetPipelineState(compute.Get()); l.list->SetComputeRootSignature(root.Get());
  l.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  l.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress()); l.list->Dispatch(1, 1, 1);
  transition(read.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  read.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 64);
  std::fprintf(stderr, "%s_DEPTH_SRV_PHASE begin\n", kPrefix); std::fflush(stderr);
  c.execute({l.list.Get()});
  std::fprintf(stderr, "%s_DEPTH_SRV_PHASE end\n", kPrefix); std::fflush(stderr);
  c.execute({read.list.Get()});
  void *mapped = nullptr; check(readback->Map(0, nullptr, &mapped), "Map(depth-srv)");
  float values[16]; std::memcpy(values, mapped, sizeof(values)); readback->Unmap(0, nullptr);
  for (float value : values) if (value != 0.75f) fail("depth-clear-srv-read");
  std::printf("%s_DEPTH_SRV depth=%08x exact=1\n", kPrefix, std::bit_cast<uint32_t>(values[0]));
  pass("depth-clear-srv-compute-read");
}

// A and B emit one output into an unbound slot; C restores identity. Native
// indices are 0..7, with the spare physical slot kept truly unbound.
void submissionAttachmentUnion(bool unused, bool lifetime = false) {
  Context c; constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  std::array<ComPtr<ID3D12Resource>, 2> targets;
  for (auto &target : targets) target = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> heap;
  check(c.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Create(union-heap)");
  const auto start = heap->GetCPUDescriptorHandleForHeapStart();
  const UINT stride = c.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {start, {start.ptr + stride}, {start.ptr + stride * 2}};
  c.dev->CreateRenderTargetView(targets[0].Get(), nullptr, handles[0]);
  c.dev->CreateRenderTargetView(targets[1].Get(), nullptr, handles[1]);
  D3D12_RENDER_TARGET_VIEW_DESC nullDesc{}; nullDesc.Format = fmt; nullDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  c.dev->CreateRenderTargetView(nullptr, &nullDesc, handles[2]);
  Draw mrt(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psGraphMrt", true, false, 6, false, false, 2);
  std::unique_ptr<Draw> alternate, identity_pipeline;
  if (lifetime) {
    alternate = std::make_unique<Draw>(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psGraphMrt", true, false, 8, false, false, 2);
    identity_pipeline = std::make_unique<Draw>(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psGraphMrt", true, false, 10, false, false, 2);
  }
  auto initialize = c.list(), a = c.list(), b = c.list(), d = c.list(), read = c.list();
  const float black[4] = {0, 0, 0, 1};
  for (UINT i = 0; i < 2; ++i) initialize.list->ClearRenderTargetView(handles[i], black, 0, nullptr);
  const D3D12_CPU_DESCRIPTOR_HANDLE first[] = {handles[0], unused ? handles[2] : handles[1]};
  const D3D12_CPU_DESCRIPTOR_HANDLE second[] = {unused ? handles[2] : handles[1], handles[0 + unsigned(unused)]};
  const D3D12_CPU_DESCRIPTOR_HANDLE identity[] = {handles[0], handles[1]};
  const float values[3][4] = {{32 / 255.0f, 0, 0, 0}, {64 / 255.0f, 0, 0, 0}, {16 / 255.0f, 0, 0, 0}};
  a.list->OMSetRenderTargets(2, first, FALSE, nullptr);
  if (lifetime) {
    const float first_term[4] = {4 / 255.0f, 0, 0, 0}, second_term[4] = {28 / 255.0f, 0, 0, 0};
    mrt.draw(a.list.Get(), 4, 4, first_term); alternate->draw(a.list.Get(), 4, 4, second_term);
  } else mrt.draw(a.list.Get(), 4, 4, values[0]);
  b.list->OMSetRenderTargets(2, second, FALSE, nullptr); mrt.draw(b.list.Get(), 4, 4, values[1]);
  d.list->OMSetRenderTargets(2, identity, FALSE, nullptr);
  (lifetime ? *identity_pipeline : mrt).draw(d.list.Get(), 4, 4, values[2]);
  std::array<Readback, 2> out;
  for (UINT i = 0; i < 2; ++i) {
    transition(read.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    out[i] = copyOut(c.dev.Get(), read.list.Get(), targets[i].Get());
    transition(read.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  check(initialize.list->Close(), "Close(union-init)"); check(a.list->Close(), "Close(union-A)");
  check(b.list->Close(), "Close(union-B)"); check(d.list->Close(), "Close(union-C)"); check(read.list->Close(), "Close(union-read)");
  for (UINT replay = 0; replay < (lifetime ? 1u : 2u); ++replay) {
    c.executeSealed({initialize.list.Get()});
    const char *phase = lifetime ? "graph-union-lifetime" : unused ? "graph-union-1" : "graph-union-0";
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=%s replay=%u\n", kPrefix, phase, replay); std::fflush(stderr);
    if (lifetime) {
      ComPtr<ID3D12Fence> gate; check(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "Create(union-gate)");
      check(c.queue->Wait(gate.Get(), 1), "Wait(union-gate)");
      ID3D12CommandList *records[] = {a.list.Get(), b.list.Get(), d.list.Get()}; c.queue->ExecuteCommandLists(3, records);
      ComPtr<ID3D12CommandAllocator> replacement_a, replacement_b;
      check(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement_a)), "Create(union-reset-allocator-A)");
      check(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement_b)), "Create(union-reset-allocator-B)");
      expect(a.allocator->Reset(), E_FAIL, "Reset(union-live-allocator)");
      check(a.list->Reset(replacement_a.Get(), nullptr), "Reset(union-list-A)");
      check(b.list->Reset(replacement_b.Get(), nullptr), "Reset(union-list-B)");
      check(a.list->Close(), "Close(union-reset-A)"); check(b.list->Close(), "Close(union-reset-B)");
      mrt.pso.Reset(); mrt.root.Reset(); alternate.reset();
      a.list.Reset(); b.list.Reset(); a.allocator.Reset(); b.allocator.Reset();
      check(gate->Signal(1), "Signal(union-gate)"); c.executeSealed({});
    } else c.executeSealed({a.list.Get(), b.list.Get(), d.list.Get()});
    std::fprintf(stderr, "%s_GRAPH_PHASE end=%s replay=%u\n", kPrefix, phase, replay); std::fflush(stderr);
    c.executeSealed({read.list.Get()});
    const uint32_t expected[] = {unused ? 0xff000030u : 0xff004030u, unused ? 0xff005000u : 0xff003040u};
    for (UINT i = 0; i < 2; ++i) for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x)
      if (out[i].at<uint32_t>(x, y) != expected[i]) fail("graph-MRT-union-pixels");
    if (lifetime)
      std::printf("%s_GRAPH_UNION_LIFETIME first=%08x second=%08x render_encoders=1 members=3 multi_pso=1 owner_released=1 reset_before_gate=1 exact=1\n",
          kPrefix, out[0].at<uint32_t>(0), out[1].at<uint32_t>(0));
    else std::printf("%s_GRAPH_UNION unused=%u replay=%u first=%08x second=%08x render_encoders=1 members=3 identity_reset=1 exact=1\n",
        kPrefix, unsigned(unused), replay, out[0].at<uint32_t>(0), out[1].at<uint32_t>(0));
  }
  pass(lifetime ? "graph-union-masked-PSO-reset-release" : unused ? "graph-union-single-unused-output" : "graph-union-MRT-permutation-identity");
}

void submissionPendingClears(bool observation) {
  Context c; constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  std::array<ComPtr<ID3D12Resource>, 2> targets;
  for (auto &target : targets) target = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto ds = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_RESOURCE_STATE_DEPTH_WRITE);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
  check(c.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "Create(pending-RTVs)");
  const auto start = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = {start, {start.ptr + c.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)}};
  for (UINT i = 0; i < 2; ++i) c.dev->CreateRenderTargetView(targets[i].Get(), nullptr, rtvs[i]);
  const auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  c.dev->CreateDepthStencilView(ds.Get(), nullptr, dsv);
  auto clear = c.list(), read = c.list();
  const float first[4] = {16 / 255.0f, 0, 0, 1}, final[4] = {0, 0, 32 / 255.0f, 1}, green[4] = {0, 64 / 255.0f, 0, 1};
  clear.list->ClearRenderTargetView(rtvs[0], first, 0, nullptr);
  Readback intermediate;
  if (observation) {
    transition(clear.list.Get(), targets[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    intermediate = copyOut(c.dev.Get(), clear.list.Get(), targets[0].Get());
    transition(clear.list.Get(), targets[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  clear.list->ClearRenderTargetView(rtvs[0], final, 0, nullptr);
  clear.list->ClearRenderTargetView(rtvs[1], green, 0, nullptr);
  clear.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25f, 0x11, 0, nullptr);
  clear.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0x99, 0, nullptr);
  clear.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 0.875f, 0x22, 0, nullptr);
  std::array<Readback, 2> color;
  for (UINT i = 0; i < 2; ++i) {
    transition(read.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    color[i] = copyOut(c.dev.Get(), read.list.Get(), targets[i].Get());
    transition(read.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  transition(read.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto depth = copyOut(c.dev.Get(), read.list.Get(), ds.Get(), 0), stencil = copyOut(c.dev.Get(), read.list.Get(), ds.Get(), 1);
  transition(read.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  check(clear.list->Close(), "Close(pending-no-draw)"); check(read.list->Close(), "Close(pending-read)");
  for (UINT replay = 0; replay < 2; ++replay) {
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-clears-%u replay=%u\n", kPrefix, unsigned(observation), replay); std::fflush(stderr);
    c.executeSealed({clear.list.Get()});
    std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-clears-%u replay=%u\n", kPrefix, unsigned(observation), replay); std::fflush(stderr);
    c.executeSealed({read.list.Get()});
    for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x)
      if (color[0].at<uint32_t>(x, y) != 0xff200000u || color[1].at<uint32_t>(x, y) != 0xff004000u ||
          depth.at<uint32_t>(x, y) != 0x3f000000u || stencil.at<uint8_t>(x, y) != 0x22 ||
          (observation && intermediate.at<uint32_t>(x, y) != 0xff000010u)) fail("graph-pending-clear-values");
    std::printf("%s_GRAPH_CLEARS observation=%u replay=%u color0=%08x color1=%08x depth=%08x stencil=%02x "
        "clear_encoders=%u clear_loads=%u superseded=%u no_draw_final_flush=1 exact=1\n", kPrefix, unsigned(observation), replay,
        color[0].at<uint32_t>(0), color[1].at<uint32_t>(0), depth.at<uint32_t>(0), stencil.at<uint8_t>(0),
        observation ? 2u : 1u, observation ? 5u : 4u, observation ? 2u : 3u);
  }
  pass(observation ? "graph-pending-clear-observation" : "graph-pending-clear-MRT-DS-final-flush");
}
// Re-execute the same sealed records after retirement. B changes its shader,
// root layout, viewport and scissor; all controls keep the same attachment.
void crossListRenderGrouping(UINT control) {
  Context c;
  constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr const char *names[] = {"cross-list-ordinary-group", "cross-list-tail-uav-boundary",
      "cross-list-prefix-uav-boundary", "cross-list-transition-copy-boundary", "cross-list-incoming-clear"};
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  Draw red(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  Draw green(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1, "psGreen", true, false, 8);
  auto a = c.list(), b = c.list(), read = c.list();
  const float clear[4] = {0, 0, 64 / 255.0f, 1};
  const float redColor[4] = {128 / 255.0f, 0, 0, 0};
  const float greenInput[4] = {0, 0, 32 / 255.0f, 0};
  a.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  a.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  red.draw(a.list.Get(), 4, 4, redColor);
  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  if (control == 1) a.list->ResourceBarrier(1, &uav);
  if (control == 2) b.list->ResourceBarrier(1, &uav);
  Readback intermediate;
  if (control == 3) {
    transition(a.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    intermediate = copyOut(c.dev.Get(), a.list.Get(), rt.Get());
    transition(a.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  if (control == 4) {
    const float replacement[4] = {0, 0, 16 / 255.0f, 1};
    b.list->ClearRenderTargetView(rtv, replacement, 0, nullptr);
  }
  b.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0, 0, 2, 4, 0, 1};
  const D3D12_RECT scissor{0, 0, 1, 4};
  green.draw(b.list.Get(), 4, 4, greenInput, 0.5f, 0, &viewport, &scissor);
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto out = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  check(a.list->Close(), "Close(group-A)");
  check(b.list->Close(), "Close(group-B)");
  check(read.list->Close(), "Close(group-readback)");
  for (UINT replay = 0; replay < 2; ++replay) {
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-boundary-%u replay=%u\n", kPrefix, control, replay); std::fflush(stderr);
    c.executeSealed({a.list.Get(), b.list.Get()});
    std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-boundary-%u replay=%u\n", kPrefix, control, replay); std::fflush(stderr);
    c.executeSealed({read.list.Get()});
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x) {
        const uint32_t expected = (control == 4 ? 0xff100000u : 0xff400080u) | (x == 0 ? 0x00002000u : 0);
        if (out.at<uint32_t>(x, y) != expected) fail(names[control]);
        if (control == 3 && intermediate.at<uint32_t>(x, y) != 0xff400080u) fail("cross-list-intermediate-copy");
      }
    std::printf("%s_CROSS_LIST control=%u replay=%u left=%08x right=%08x exact=1 sealed=1\n", kPrefix,
                control, replay, out.at<uint32_t>(0), out.at<uint32_t>(3));
  }
  pass(names[control]);
}

void crossListUavDependency(UINT control = 0) {
  Context c;
  constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto data = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                        D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(4), D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(zero->Map(0, nullptr, &mapped), "Map(UAV-zero)");
  std::memset(mapped, 0, 4);
  zero->Unmap(0, nullptr);
  auto initialize = c.list();
  initialize.list->CopyBufferRegion(data.Get(), 0, zero.Get(), 0, 4);
  transition(initialize.list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute({initialize.list.Get()});
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  Draw producer(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1,
      control == 1 ? "psUavConsumer" : control == 3 ? "psUavAtomicAdd" : control == 4 ? "psMain" : "psUavProducer", true, false, 6, true, false, 1, false, false, control == 4);
  Draw consumer(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 1,
      control == 1 ? "psUavProducer" : control == 2 ? "psUavProducer64" : control == 3 ? "psUavAtomicAdd" : control == 4 ? "psMain" : "psUavConsumer", true, false, 8, true, false, 1, false, false, control == 4);
  auto wordOut = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(4), D3D12_RESOURCE_STATE_COPY_DEST);
  auto a = c.list(), b = c.list(), read = c.list();
  const float clear[4] = {0, 0, 64 / 255.0f, 1};
  const float red[4] = {128 / 255.0f, 0, 0, 0};
  const float unused[4] = {};
  a.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  a.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  producer.draw(a.list.Get(), 4, 4, red, 0.5f, 0, nullptr, nullptr, data->GetGPUVirtualAddress());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = data.Get();
  if (control < 3) b.list->ResourceBarrier(1, &barrier);
  b.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  consumer.draw(b.list.Get(), 4, 4, control == 1 ? red : unused, 0.5f, 0, nullptr, nullptr, data->GetGPUVirtualAddress());
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto out = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  transition(read.list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  read.list->CopyBufferRegion(wordOut.Get(), 0, data.Get(), 0, 4);
  const char *phase = control == 0 ? "graph-uav-dependency" : control == 1 ? "graph-uav-war" :
      control == 2 ? "graph-uav-waw" : control == 3 ? "graph-uav-atomic" : "graph-uav-vertex";
  std::fprintf(stderr, "%s_GRAPH_PHASE begin=%s replay=0\n", kPrefix, phase); std::fflush(stderr);
  c.execute({a.list.Get(), b.list.Get()});
  std::fprintf(stderr, "%s_GRAPH_PHASE end=%s replay=0\n", kPrefix, phase); std::fflush(stderr);
  c.execute({read.list.Get()});
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 4; ++x)
      if (out.at<uint32_t>(x, y) != (control == 0 ? 0xff402080u : 0xff400080u)) fail("cross-list-UAV-dependent-pixels");
  check(wordOut->Map(0, nullptr, &mapped), "Map(UAV-word)");
  const UINT word = *static_cast<const UINT *>(mapped); wordOut->Unmap(0, nullptr);
  if (word != (control == 2 ? 64u : control == 4 ? 6u : 32u)) fail("cross-list-UAV-dependent-word");
  std::printf("%s_CROSS_LIST_UAV rgba=%08x exact=1 dependency=1\n", kPrefix, out.at<uint32_t>(0));
  std::printf("%s_GRAPH_UAV control=%u word=%u render_encoders=%u members=2 inline_barriers=%u exact=1\n",
      kPrefix, control, word, control == 4 ? 1u : 2u, control == 4 ? 1u : 0u);
  pass(control == 0 ? "cross-list-uav-producer-consumer" : phase);
}

// CLEAR + draw + PRESERVE, then a second pass PRESERVE + additive draw: both contributions survive.
void clearPreserve() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  Draw add(c.dev.Get(), kFmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  auto l = c.list();
  const float clear[4] = {0.0f, 0.0f, 64 / 255.0f, 1.0f};
  auto first = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
                        clear);
  l.list->BeginRenderPass(1, &first, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  const float red[4] = {128 / 255.0f, 0, 0, 0};
  add.draw(l.list.Get(), 4, 4, red);
  l.list->EndRenderPass();
  auto second = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
                         D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
  l.list->BeginRenderPass(1, &second, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  const float green[4] = {0, 32 / 255.0f, 0, 0};
  add.draw(l.list.Get(), 4, 4, green);
  l.list->EndRenderPass();
  transition(l.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto out = copyOut(c.dev.Get(), l.list.Get(), rt.Get());
  c.execute({l.list.Get()});
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 4; ++x)
      if (out.at<uint32_t>(x, y) != 0xff402080u) {
        std::fprintf(stderr, "%s_FAIL case=clear-preserve pixel=(%u,%u) got=%08x\n", kPrefix, x, y,
                     out.at<uint32_t>(x, y));
        ExitProcess(1);
      }
  std::printf("%s_CLEAR_PRESERVE pixel=%08x\n", kPrefix, out.at<uint32_t>(0, 0));
  pass("clear-then-preserve");
}

// Identical RTVs reuse a native pass, but overwriting the same CPU descriptor slot must not.
void redundantRenderTargetBindings() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto desc = texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  desc.DepthOrArraySize = 2;
  auto first = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
  desc.DepthOrArraySize = 1;
  auto second = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtvHeap, aliasHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  const auto alias = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, aliasHeap);
  D3D12_RENDER_TARGET_VIEW_DESC view{};
  view.Format = kFmt;
  view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
  view.Texture2DArray.ArraySize = 1;
  c.dev->CreateRenderTargetView(first.Get(), &view, alias);
  Draw add(c.dev.Get(), kFmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  auto l = c.list();
  const float clear[4] = {0, 0, 0, 1};
  l.list->ClearRenderTargetView(alias, clear, 0, nullptr);
  view.Texture2DArray.FirstArraySlice = 1;
  c.dev->CreateRenderTargetView(first.Get(), &view, rtv);
  l.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  c.dev->CreateRenderTargetView(second.Get(), nullptr, rtv);
  l.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  view.Texture2DArray.FirstArraySlice = 0;
  c.dev->CreateRenderTargetView(first.Get(), &view, rtv);
  const float red[4] = {64 / 255.0f, 0, 0, 0};
  const float green[4] = {0, 32 / 255.0f, 0, 0};
  const float blue[4] = {0, 0, 16 / 255.0f, 0};
  const float otherBlue[4] = {0, 0, 128 / 255.0f, 0};
  l.list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  add.draw(l.list.Get(), 4, 4, red);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  add.draw(l.list.Get(), 4, 4, green);
  // A different CPU handle resolving to the same native view is also redundant.
  l.list->OMSetRenderTargets(1, &alias, FALSE, nullptr);
  add.draw(l.list.Get(), 4, 4, blue);
  view.Texture2DArray.FirstArraySlice = 1;
  c.dev->CreateRenderTargetView(first.Get(), &view, rtv);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  add.draw(l.list.Get(), 4, 4, otherBlue);
  // The same handle now names a different resource, not just a different array view.
  c.dev->CreateRenderTargetView(second.Get(), nullptr, rtv);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  add.draw(l.list.Get(), 4, 4, red);
  l.list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  add.draw(l.list.Get(), 4, 4, green);
  transition(l.list.Get(), first.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(l.list.Get(), second.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const std::array<Readback, 3> out = {copyOut(c.dev.Get(), l.list.Get(), first.Get(), 0),
      copyOut(c.dev.Get(), l.list.Get(), first.Get(), 1), copyOut(c.dev.Get(), l.list.Get(), second.Get())};
  c.execute({l.list.Get()});
  const uint32_t expected[] = {0xff102040u, 0xff800000u, 0xff002040u};
  for (UINT target = 0; target < out.size(); ++target)
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        if (out[target].at<uint32_t>(x, y) != expected[target]) {
          std::fprintf(stderr, "%s_FAIL case=redundant-rtv-bindings target=%u pixel=(%u,%u) got=%08x expected=%08x\n",
              kPrefix, target, x, y, out[target].at<uint32_t>(x, y), expected[target]);
          ExitProcess(1);
        }
  std::printf("%s_RTV_REBIND first=%08x view=%08x resource=%08x draws=6 redundant=2 unbind=1\n",
      kPrefix, out[0].at<uint32_t>(0), out[1].at<uint32_t>(0), out[2].at<uint32_t>(0));
  pass("redundant-rtv-bindings");
}

// Exact DSV aliases reuse a pass; flags and overwritten views/resources remain boundaries.
void redundantDepthStencilBindings() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto kDepth = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto desc = texDesc(kDepth, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
  desc.DepthOrArraySize = 2;
  auto first = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  desc.DepthOrArraySize = 1;
  auto second = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap, aliasHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  const auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  const auto alias = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, aliasHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  D3D12_DEPTH_STENCIL_VIEW_DESC view{};
  view.Format = kDepth;
  view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
  view.Texture2DArray.ArraySize = 1;
  c.dev->CreateDepthStencilView(first.Get(), &view, dsv);
  Draw add(c.dev.Get(), kFmt, kDepth, 1, "psMain", true, true);
  auto l = c.list();
  const float clear[4] = {0, 0, 0, 1};
  const auto both = D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL);
  l.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  view.Texture2DArray.FirstArraySlice = 1;
  c.dev->CreateDepthStencilView(first.Get(), &view, alias);
  l.list->ClearDepthStencilView(alias, both, 0.5f, 0x22, 0, nullptr);
  view.Texture2DArray.FirstArraySlice = 0;
  c.dev->CreateDepthStencilView(second.Get(), &view, alias);
  l.list->ClearDepthStencilView(alias, both, 0.125f, 0x33, 0, nullptr);
  c.dev->CreateDepthStencilView(first.Get(), &view, alias);
  l.list->ClearDepthStencilView(dsv, both, 0.75f, 0x11, 0, nullptr);
  const float red[4] = {64 / 255.0f, 0, 0, 0};
  const float green[4] = {0, 32 / 255.0f, 0, 0};
  const float blue[4] = {0, 0, 16 / 255.0f, 0};
  l.list->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
  add.draw(l.list.Get(), 4, 4, red, 0.5f);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
  add.draw(l.list.Get(), 4, 4, green, 0.25f);
  l.list->OMSetRenderTargets(1, &rtv, FALSE, &alias);
  add.draw(l.list.Get(), 4, 4, blue, 0.125f);
  // Stencil is disabled in this PSO: read-only stencil never conflicts with depth writes.
  // Both flag changes must split even though the native view and writable depth are identical.
  view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
  c.dev->CreateDepthStencilView(first.Get(), &view, alias);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, &alias);
  add.draw(l.list.Get(), 4, 4, red, 0.0625f);
  view.Flags = D3D12_DSV_FLAG_NONE;
  c.dev->CreateDepthStencilView(first.Get(), &view, alias);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, &alias);
  add.draw(l.list.Get(), 4, 4, green, 0.03125f);
  view.Texture2DArray.FirstArraySlice = 1;
  c.dev->CreateDepthStencilView(first.Get(), &view, alias);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, &alias);
  add.draw(l.list.Get(), 4, 4, blue, 0.25f);
  view.Texture2DArray.FirstArraySlice = 0;
  c.dev->CreateDepthStencilView(second.Get(), &view, alias);
  l.list->OMSetRenderTargets(1, &rtv, TRUE, &alias);
  add.draw(l.list.Get(), 4, 4, red, 0.0625f);
  // Trace grouping: draws [1,2,3], [4], [5], [6], [7]; before DSV reuse: seven encoders.
  transition(l.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(l.list.Get(), first.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(l.list.Get(), second.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto color = copyOut(c.dev.Get(), l.list.Get(), rt.Get());
  const std::array<Readback, 3> depths = {copyOut(c.dev.Get(), l.list.Get(), first.Get(), 0),
      copyOut(c.dev.Get(), l.list.Get(), first.Get(), 1), copyOut(c.dev.Get(), l.list.Get(), second.Get(), 0)};
  const std::array<Readback, 3> stencils = {copyOut(c.dev.Get(), l.list.Get(), first.Get(), 2),
      copyOut(c.dev.Get(), l.list.Get(), first.Get(), 3), copyOut(c.dev.Get(), l.list.Get(), second.Get(), 1)};
  c.execute({l.list.Get()});
  const uint32_t expectedDepth[] = {std::bit_cast<uint32_t>(0.03125f), std::bit_cast<uint32_t>(0.25f),
      std::bit_cast<uint32_t>(0.0625f)};
  const uint8_t expectedStencil[] = {0x11, 0x22, 0x33};
  for (UINT target = 0; target < depths.size(); ++target)
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        if (color.at<uint32_t>(x, y) != 0xff2040c0u ||
            depths[target].at<uint32_t>(x, y) != expectedDepth[target] ||
            stencils[target].at<uint8_t>(x, y) != expectedStencil[target])
          fail("redundant-dsv-bindings-readback");
  std::printf("%s_DSV_REBIND color=%08x depth=%08x,%08x,%08x stencil=%02x,%02x,%02x "
      "draws=7 redundant=2 flag_boundaries=2 view_boundaries=1 resource_boundaries=1 "
      "expected_render_encoders=5 exact=1\n", kPrefix, color.at<uint32_t>(0),
      depths[0].at<uint32_t>(0), depths[1].at<uint32_t>(0), depths[2].at<uint32_t>(0),
      stencils[0].at<uint8_t>(0), stencils[1].at<uint8_t>(0), stencils[2].at<uint8_t>(0));
  pass("redundant-dsv-bindings");
}

// A covering rectangle uses the selected mip/array view; partial clears retain pixels and DSV aspects.
void wholeSurfaceRectangleClears() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto desc = texDesc(kFmt, 8, 8, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  desc.DepthOrArraySize = 2;
  desc.MipLevels = 2;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto ds = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
      D3D12_RESOURCE_STATE_DEPTH_WRITE);
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  const auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  D3D12_RENDER_TARGET_VIEW_DESC view{};
  view.Format = kFmt;
  view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
  view.Texture2DArray.MipSlice = 1;
  view.Texture2DArray.ArraySize = 2;
  c.dev->CreateRenderTargetView(rt.Get(), &view, rtv);
  c.dev->CreateDepthStencilView(ds.Get(), nullptr, dsv);
  auto l = c.list();
  const float blue[4] = {0, 0, 1, 1}, green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};
  l.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
  view.Texture2DArray.FirstArraySlice = 1;
  view.Texture2DArray.ArraySize = 1;
  c.dev->CreateRenderTargetView(rt.Get(), &view, rtv);
  const D3D12_RECT covering[] = {{1, 1, 3, 3}, {-1, -2, 5, 6}, {0, 0, 0, 0}};
  const D3D12_RECT partial[] = {{1, 1, 3, 3}, {0, 0, 0, 0}, {5, 5, 6, 6}};
  l.list->ClearRenderTargetView(rtv, green, UINT(std::size(covering)), covering);
  l.list->ClearRenderTargetView(rtv, red, UINT(std::size(partial)), partial);
  const auto both = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
  l.list->ClearDepthStencilView(dsv, both, 0.25f, 0x11, 0, nullptr);
  l.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0x99, UINT(std::size(covering)), covering);
  // Read back between aspect clears, so a stencil-only clear cannot hide depth-only stencil corruption.
  transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto preservedStencil = copyOut(c.dev.Get(), l.list.Get(), ds.Get(), 1);
  transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  const D3D12_RECT exact = {0, 0, 4, 4};
  l.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 0.875f, 0x22, 1, &exact);
  l.list->ClearDepthStencilView(dsv, both, 0.75f, 0x33, UINT(std::size(partial)), partial);
  transition(l.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto untouched = copyOut(c.dev.Get(), l.list.Get(), rt.Get(), 1);
  auto color = copyOut(c.dev.Get(), l.list.Get(), rt.Get(), 3);
  auto depth = copyOut(c.dev.Get(), l.list.Get(), ds.Get(), 0);
  auto stencil = copyOut(c.dev.Get(), l.list.Get(), ds.Get(), 1);
  c.execute({l.list.Get()});
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 4; ++x) {
      const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
      const uint32_t expectedColor = inside ? 0xff0000ffu : 0xff00ff00u;
      const uint32_t expectedDepth = inside ? 0x3f400000u : 0x3f000000u;
      const uint8_t expectedStencil = inside ? 0x33 : 0x22;
      if (untouched.at<uint32_t>(x, y) != 0xffff0000u || color.at<uint32_t>(x, y) != expectedColor ||
          depth.at<uint32_t>(x, y) != expectedDepth || stencil.at<uint8_t>(x, y) != expectedStencil ||
          preservedStencil.at<uint8_t>(x, y) != 0x11) {
        std::fprintf(stderr, "%s_FAIL case=whole-surface-rectangle-clears pixel=(%u,%u) "
            "untouched=%08x color=%08x depth=%08x stencil=%02x preserved_stencil=%02x\n",
            kPrefix, x, y, untouched.at<uint32_t>(x, y), color.at<uint32_t>(x, y),
            depth.at<uint32_t>(x, y), stencil.at<uint8_t>(x, y), preservedStencil.at<uint8_t>(x, y));
        ExitProcess(1);
      }
    }
  std::printf("%s_RECT_CLEAR untouched=%08x outside=%08x inside=%08x depth=%08x,%08x "
      "stencil=%02x,%02x preserved_stencil=%02x exact=1\n", kPrefix, untouched.at<uint32_t>(0),
      color.at<uint32_t>(0), color.at<uint32_t>(1, 1), depth.at<uint32_t>(0), depth.at<uint32_t>(1, 1),
      stencil.at<uint8_t>(0), stencil.at<uint8_t>(1, 1), preservedStencil.at<uint8_t>(0));
  pass("whole-surface-rectangle-clears");
}

// Ordinary clears become attachment load actions without losing untouched pixels or DSV aspects.
// Replay the same sealed list after retirement: each clear must still execute exactly once.
void clearToRenderLoadActions() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto kDepth = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto ds = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(kDepth, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_RESOURCE_STATE_DEPTH_WRITE);
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
  const auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  const auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  c.dev->CreateDepthStencilView(ds.Get(), nullptr, dsv);
  Draw add(c.dev.Get(), kFmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  Draw depthDraw(c.dev.Get(), DXGI_FORMAT_UNKNOWN, kDepth, 1, "psMain", false, true);
  auto l = c.list();
  const float clear[4] = {16 / 255.0f, 32 / 255.0f, 64 / 255.0f, 1};
  const float term[4] = {32 / 255.0f, 16 / 255.0f, 8 / 255.0f, 0};
  l.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  l.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  add.draw(l.list.Get(), 2, 2, term);
  transition(l.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto color = copyOut(c.dev.Get(), l.list.Get(), rt.Get());
  transition(l.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const D3D12_CLEAR_FLAGS flags[] = {D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
      D3D12_CLEAR_FLAG_DEPTH, D3D12_CLEAR_FLAG_STENCIL};
  const float clearDepth[] = {0.75f, 0.5f, 0.875f};
  const UINT8 clearStencil[] = {0x11, 0x99, 0x22};
  const float drawDepth[] = {0.125f, 0.25f, 0.0625f};
  std::array<Readback, 3> depths, stencils;
  for (UINT stage = 0; stage < 3; ++stage) {
    l.list->ClearDepthStencilView(dsv, flags[stage], clearDepth[stage], clearStencil[stage], 0, nullptr);
    l.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    depthDraw.draw(l.list.Get(), 2, 2, term, drawDepth[stage]);
    // Rebinding the same depth-only view must keep the current native render pass.
    l.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    depthDraw.draw(l.list.Get(), 2, 2, term, drawDepth[stage]);
    // A farther draw must fail, leaving the near write and the outside clear intact.
    depthDraw.draw(l.list.Get(), 2, 2, term, 0.875f);
    transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    depths[stage] = copyOut(c.dev.Get(), l.list.Get(), ds.Get(), 0);
    stencils[stage] = copyOut(c.dev.Get(), l.list.Get(), ds.Get(), 1);
    transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  // Both resources end in their initial states, making the second execution legal without Reset.
  for (UINT execution = 0; execution < 2; ++execution) {
    if (execution == 0) {
      c.execute({l.list.Get()});
    } else {
      ID3D12CommandList *sealed = l.list.Get();
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event)
        fail("clear-load-replay-event");
      check(c.fence->SetEventOnCompletion(++c.value, event), "clear-load-replay-completion");
      c.queue->ExecuteCommandLists(1, &sealed);
      check(c.queue->Signal(c.fence.Get(), c.value), "clear-load-replay-signal");
      if (WaitForSingleObject(event, kTimeoutMs) != WAIT_OBJECT_0)
        fail("clear-load-replay-fence");
      CloseHandle(event);
      expect(c.dev->GetDeviceRemovedReason(), S_OK, "clear-load-replay-device-healthy");
    }
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x) {
        const bool inside = x < 2 && y < 2;
        const uint32_t expectedColor = inside ? 0xff483030u : 0xff402010u;
        const uint32_t gotColor = color.at<uint32_t>(x, y);
        if (gotColor != expectedColor) {
          std::fprintf(stderr, "%s_FAIL case=clear-to-render-load-actions execution=%u pixel=(%u,%u) "
              "color=%08x expected=%08x\n", kPrefix, execution, x, y, gotColor, expectedColor);
          ExitProcess(1);
        }
        for (UINT stage = 0; stage < 3; ++stage) {
          const float expectedDepth = inside ? drawDepth[stage] : stage == 0 ? 0.75f : 0.5f;
          const uint32_t expectedBits = std::bit_cast<uint32_t>(expectedDepth);
          const uint8_t expectedStencil = stage == 2 ? 0x22 : 0x11;
          const uint32_t gotDepth = depths[stage].at<uint32_t>(x, y);
          const uint8_t gotStencil = stencils[stage].at<uint8_t>(x, y);
          if (gotDepth != expectedBits || gotStencil != expectedStencil) {
            std::fprintf(stderr, "%s_FAIL case=clear-to-render-load-actions execution=%u stage=%u "
                "pixel=(%u,%u) depth=%08x expected_depth=%08x stencil=%02x expected_stencil=%02x\n",
                kPrefix, execution, stage, x, y, gotDepth, expectedBits, gotStencil, expectedStencil);
            ExitProcess(1);
          }
        }
      }
  }
  std::printf("%s_CLEAR_LOAD_FOLD outside=%08x inside=%08x executions=2 color_pixels=32 "
      "depth_pixels=96 stencil_pixels=96 depth_only_redundant=3 exact=1\n", kPrefix, color.at<uint32_t>(3, 3), color.at<uint32_t>(0, 0));
  pass("clear-to-render-load-actions");
}

// DISCARD beginning + a full-coverage draw defines every pixel; DISCARD ending leaves the next
// pass free to CLEAR. Depth CLEAR at begin must hold for the depth test inside the pass.
void discardAndDepthClear() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto kDepth = DXGI_FORMAT_D32_FLOAT;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CLEAR_VALUE dclear{kDepth};
  dclear.DepthStencil.Depth = 1.0f;
  auto ds = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                      texDesc(kDepth, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                      &dclear);
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
  auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  c.dev->CreateDepthStencilView(ds.Get(), nullptr, dsv);
  Draw opaque(c.dev.Get(), kFmt, kDepth, 1, "psMain", false, true);
  auto l = c.list();
  auto color = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD,
                        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
  D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
  depth.cpuDescriptor = dsv;
  depth.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
  depth.DepthBeginningAccess.Clear.ClearValue.Format = kDepth;
  depth.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = 0.25f;
  depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
  depth.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
  depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
  l.list->BeginRenderPass(1, &color, &depth, D3D12_RENDER_PASS_FLAG_NONE);
  const float blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
  // Nearer than the cleared 0.25: passes. Then farther (0.75): must be rejected by the cleared depth.
  opaque.draw(l.list.Get(), 4, 4, blue, 0.125f);
  opaque.draw(l.list.Get(), 4, 4, white, 0.75f);
  l.list->EndRenderPass();
  transition(l.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(l.list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto out = copyOut(c.dev.Get(), l.list.Get(), rt.Get());
  auto depthOut = copyOut(c.dev.Get(), l.list.Get(), ds.Get());
  c.execute({l.list.Get()});
  std::printf("%s_DISCARD_DEPTH color=%08x depth=%.6f\n", kPrefix, out.at<uint32_t>(2, 2),
              depthOut.at<float>(2, 2));
  if (out.at<uint32_t>(2, 2) != 0xffff0000u || depthOut.at<float>(2, 2) != 0.125f)
    fail("discard-depth-clear");
  pass("discard-begin-depth-clear");
}

// 4x native MSAA pass with ENDING_ACCESS_RESOLVE: the full-rect AVERAGE exercises
// resolve folding while MIN/MAX remain separate; preserve a source sample and read
// back both resolved destinations.
// (1..4)/8 into others via the subresource parameters, including a sub-rectangle with a DstX offset.
void resolveModes() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto kFloat = DXGI_FORMAT_R32_FLOAT;
  auto run = [&](DXGI_FORMAT fmt, D3D12_RESOLVE_MODE mode, bool offset, bool native = false) {
    const char *ps = native ? "psMain" : "psSamples";
    auto msaa = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                          texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 4),
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The translator's resolve renders into the destination, which therefore needs ALLOW_RENDER_TARGET
    // (a Metal render-target usage); see the report's constraint on ResolveSubresourceRegion.
    auto dst = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                         texDesc(fmt, native ? 4 : 8, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RESOLVE_DEST);
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
    c.dev->CreateRenderTargetView(msaa.Get(), nullptr, rtv);
    Draw samples(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 4, ps, false);
    auto l = c.list();
    D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS region{0, 0, offset ? 4u : 0u, 0, {0, 0, 4, 4}};
    auto access = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD,
                           D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE);
    access.EndingAccess.Resolve = {msaa.Get(), dst.Get(), 1, &region, fmt, mode, FALSE};
    l.list->BeginRenderPass(1, &access, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    const float color[4] = {0, 0x40 / 255.0f, 0x80 / 255.0f, 1};
    samples.draw(l.list.Get(), 4, 4, color);
    l.list->EndRenderPass();
    transition(l.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto out = copyOut(c.dev.Get(), l.list.Get(), dst.Get());
    c.execute({l.list.Get()});
    return out;
  };
  auto average = run(kFmt, D3D12_RESOLVE_MODE_AVERAGE, false);
  auto minimum = run(kFloat, D3D12_RESOLVE_MODE_MIN, true);
  auto maximum = run(kFloat, D3D12_RESOLVE_MODE_MAX, false);
  // A uniform-color native 4x pass folds its AVERAGE resolve into the pass store;
  // a lost resolve leaves the zero-initialized destination.
  auto folded = run(kFmt, D3D12_RESOLVE_MODE_AVERAGE, false, true);
  const uint32_t folded_pixel = folded.at<uint32_t>(1, 1);
  std::printf("%s_RESOLVE_FOLDED pixel=%08x\n", kPrefix, folded_pixel);
  if (folded_pixel != 0xff804000u) fail("resolve-folded");
  // Samples write 1/8..4/8: average 2.5/8 = 0.3125 -> 80 (0x50); min 0.125; max 0.5.
  const uint32_t avg = average.at<uint32_t>(1, 1);
  const float mn = minimum.at<float>(5, 1), mx = maximum.at<float>(1, 1), untouched = minimum.at<float>(1, 1);
  std::printf("%s_RESOLVE average=%08x min=%.6f max=%.6f min_left=%.6f\n", kPrefix, avg, mn, mx, untouched);
  if ((avg & 0xff) < 0x4f || (avg & 0xff) > 0x51 || (avg >> 24) != 0xff || mn != 0.125f || mx != 0.5f || untouched != 0.0f)
    fail("resolve-modes");
  pass("ending-resolve-average-min-max");
}

// A suspending pass in list A and the resuming pass in list B, executed together: B's CLEAR is not
// re-applied (resume), B's additive draw lands on A's content, and B's ending RESOLVE runs.
void suspendResume() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  Draw add(c.dev.Get(), kFmt, DXGI_FORMAT_UNKNOWN, 1, "psMain", true);
  const float clear[4] = {16 / 255.0f, 0, 0, 1};
  auto a = c.list(), b = c.list();
  auto begin = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
                        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, clear);
  a.list->BeginRenderPass(1, &begin, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
  const float green[4] = {0, 64 / 255.0f, 0, 0};
  add.draw(a.list.Get(), 4, 4, green);
  a.list->EndRenderPass();
  // The resuming pass repeats the same description; CLEAR must not wipe list A's draw.
  b.list->BeginRenderPass(1, &begin, nullptr, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
  const float blue[4] = {0, 0, 32 / 255.0f, 0};
  add.draw(b.list.Get(), 4, 4, blue);
  b.list->EndRenderPass();
  transition(b.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto out = copyOut(c.dev.Get(), b.list.Get(), rt.Get());
  c.execute({a.list.Get(), b.list.Get()});
  std::printf("%s_SUSPEND_RESUME pixel=%08x\n", kPrefix, out.at<uint32_t>(1, 1));
  if (out.at<uint32_t>(1, 1) != 0xff204010u)
    fail("suspend-resume");
  pass("suspend-resume-across-lists");
}

// Spec validation: nesting, End without Begin, Close inside a pass, disallowed APIs inside a pass,
// UAV barriers and write-state transitions of pass surfaces, and suspend followed by GPU work.
void validation() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(kFmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto buffer = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                          bufDesc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON);
  auto other = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(256), D3D12_RESOURCE_STATE_COMMON);
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
  c.dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  const auto access = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
                               D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
  struct Case {
    const char *name;
    void (*body)(ID3D12GraphicsCommandList4 *, const D3D12_RENDER_PASS_RENDER_TARGET_DESC &, ID3D12Resource *,
                 ID3D12Resource *, ID3D12Resource *, D3D12_CPU_DESCRIPTOR_HANDLE);
  };
  using L = ID3D12GraphicsCommandList4;
  using A = const D3D12_RENDER_PASS_RENDER_TARGET_DESC &;
  using R = ID3D12Resource *;
  using H = D3D12_CPU_DESCRIPTOR_HANDLE;
  const Case cases[] = {
      {"nested-begin", [](L *l, A a, R, R, R, H) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         l->EndRenderPass();
       }},
      {"end-without-begin", [](L *l, A, R, R, R, H) { l->EndRenderPass(); }},
      {"close-inside-pass", [](L *l, A a, R, R, R, H) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
       }},
      {"om-set-render-targets", [](L *l, A a, R, R, R, H rtv) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
         l->EndRenderPass();
       }},
      {"clear-rtv", [](L *l, A a, R, R, R, H rtv) {
         const float black[4] = {};
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         l->ClearRenderTargetView(rtv, black, 0, nullptr);
         l->EndRenderPass();
       }},
      {"copy-buffer", [](L *l, A a, R, R buffer, R other, H) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         l->CopyBufferRegion(other, 0, buffer, 0, 16);
         l->EndRenderPass();
       }},
      {"dispatch", [](L *l, A a, R, R, R, H) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         l->Dispatch(1, 1, 1);
         l->EndRenderPass();
       }},
      {"uav-barrier", [](L *l, A a, R, R buffer, R, H) {
         D3D12_RESOURCE_BARRIER b{};
         b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
         b.UAV.pResource = buffer;
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
         l->ResourceBarrier(1, &b);
         l->EndRenderPass();
       }},
      {"pass-surface-to-srv", [](L *l, A a, R rt, R, R, H) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
         transition(l, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
         l->EndRenderPass();
       }},
      {"work-after-suspend", [](L *l, A a, R, R buffer, R other, H) {
         l->BeginRenderPass(1, &a, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
         l->EndRenderPass();
         l->CopyBufferRegion(other, 0, buffer, 0, 16);
       }},
  };
  for (const auto &test : cases) {
    auto l = c.list();
    test.body(l.list.Get(), access, rt.Get(), buffer.Get(), other.Get(), rtv);
    const HRESULT hr = l.list->Close();
    if (hr != E_INVALIDARG) {
      std::fprintf(stderr, "%s_FAIL case=validation name=%s close=0x%08lx\n", kPrefix, test.name,
                   static_cast<unsigned long>(hr));
      ExitProcess(1);
    }
  }
  // Conformant inside a pass: a barrier on a resource written before the pass.
  auto l = c.list();
  l.list->BeginRenderPass(1, &access, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  transition(l.list.Get(), other.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  l.list->EndRenderPass();
  transition(l.list.Get(), other.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
  c.execute({l.list.Get()});
  std::printf("%s_VALIDATION_PASS rejected=%zu conformant_barrier=1\n", kPrefix, std::size(cases));
  pass("render-pass-validation");
}

// ---- GPU upload heap: CPU writes read by the GPU in place ----

void gpuUploadHeap() {
  Context c;
  D3D12_HEAP_PROPERTIES custom{};
  c.dev->GetCustomHeapProperties(&custom, 0, kHeapTypeGpuUpload);
  if (custom.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
      custom.MemoryPoolPreference != D3D12_MEMORY_POOL_L0)
    fail("gpu-upload-custom-properties");
  // Committed buffer: CPU writes, then a compute shader reads it as a root SRV (no copy).
  constexpr UINT kWords = 1024;
  auto upload = committed(c.dev.Get(), kHeapTypeGpuUpload,
                          bufDesc(kWords * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON);
  uint32_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "Map(gpu upload)");
  for (UINT i = 0; i < kWords; ++i)
    mapped[i] = i * 7 + 3;
  // Placed buffer in a GPU upload heap.
  D3D12_HEAP_DESC hd{1 << 16, heap(kHeapTypeGpuUpload), 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  ComPtr<ID3D12Heap> uploadHeap;
  check(c.dev->CreateHeap(&hd, IID_PPV_ARGS(&uploadHeap)), "CreateHeap(gpu upload)");
  ComPtr<ID3D12Resource> placed;
  auto pdesc = bufDesc(kWords * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  check(c.dev->CreatePlacedResource(uploadHeap.Get(), 0, &pdesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&placed)),
        "CreatePlacedResource(gpu upload)");
  uint32_t *placedMapped = nullptr;
  check(placed->Map(0, nullptr, reinterpret_cast<void **>(&placedMapped)), "Map(placed gpu upload)");
  for (UINT i = 0; i < kWords; ++i)
    placedMapped[i] = 0x10000u + i;
  // Texture in a GPU upload heap: WriteToSubresource from the CPU, sampled by the GPU.
  auto texture = committed(c.dev.Get(), kHeapTypeGpuUpload, texDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1,
                                                                      D3D12_RESOURCE_FLAG_NONE),
                           D3D12_RESOURCE_STATE_COMMON);
  const uint32_t texels[4] = {0x04030201u, 0x40302010u, 0xff00ff00u, 0x7f7f7f7fu};
  check(texture->WriteToSubresource(0, nullptr, texels, 16, 16), "WriteToSubresource(gpu upload)");
  // Shared GPU upload heaps are invalid.
  D3D12_HEAP_DESC shared{1 << 16, heap(kHeapTypeGpuUpload), 0, D3D12_HEAP_FLAG_SHARED};
  ComPtr<ID3D12Heap> rejected;
  expect(c.dev->CreateHeap(&shared, IID_PPV_ARGS(&rejected)), E_INVALIDARG, "gpu-upload-shared-heap");

  const char cs[] = R"hlsl(
StructuredBuffer<uint> A : register(t0);
StructuredBuffer<uint> B : register(t1);
RWStructuredBuffer<uint> Out : register(u0);
[numthreads(64, 1, 1)] void csMain(uint3 id : SV_DispatchThreadID) { Out[id.x] = A[id.x] + B[id.x]; }
)hlsl";
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister = 1;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto root = rootSignature(c.dev.Get(), {3, params});
  auto blob = compile(cs, "csMain", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), {blob->GetBufferPointer(), blob->GetBufferSize()}};
  ComPtr<ID3D12PipelineState> pso;
  check(c.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "gpu-upload-pso");
  auto result = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                          bufDesc(kWords * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON);
  auto readback = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(kWords * 4 + 64),
                            D3D12_RESOURCE_STATE_COPY_DEST);
  // Texture read-back through a compute Load.
  D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0};
  D3D12_ROOT_PARAMETER tparams[2]{};
  tparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  tparams[0].DescriptorTable = {1, &srvRange};
  tparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto troot = rootSignature(c.dev.Get(), {2, tparams});
  auto tblob = compile(kSource, "csUploadTexture", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC tpd{troot.Get(), {tblob->GetBufferPointer(), tblob->GetBufferSize()}};
  ComPtr<ID3D12PipelineState> tpso;
  check(c.dev->CreateComputePipelineState(&tpd, IID_PPV_ARGS(&tpso)), "gpu-upload-texture-pso");
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  auto srv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, srvHeap, true);
  c.dev->CreateShaderResourceView(texture.Get(), nullptr, srv);
  auto texOut = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                          bufDesc(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON);

  auto l = c.list();
  l.list->SetPipelineState(pso.Get());
  l.list->SetComputeRootSignature(root.Get());
  l.list->SetComputeRootShaderResourceView(0, upload->GetGPUVirtualAddress());
  l.list->SetComputeRootShaderResourceView(1, placed->GetGPUVirtualAddress());
  l.list->SetComputeRootUnorderedAccessView(2, result->GetGPUVirtualAddress());
  l.list->Dispatch(kWords / 64, 1, 1);
  ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
  l.list->SetDescriptorHeaps(1, heaps);
  l.list->SetPipelineState(tpso.Get());
  l.list->SetComputeRootSignature(troot.Get());
  l.list->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
  l.list->SetComputeRootUnorderedAccessView(1, texOut->GetGPUVirtualAddress());
  l.list->Dispatch(1, 1, 1);
  D3D12_RESOURCE_BARRIER uav[2]{};
  uav[0].Type = uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav[0].UAV.pResource = result.Get();
  uav[1].UAV.pResource = texOut.Get();
  l.list->ResourceBarrier(2, uav);
  l.list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, kWords * 4);
  l.list->CopyBufferRegion(readback.Get(), kWords * 4, texOut.Get(), 0, 64);
  c.execute({l.list.Get()});
  uint32_t *data = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&data)), "Map(readback)");
  for (UINT i = 0; i < kWords; ++i)
    if (data[i] != i * 7 + 3 + 0x10000u + i) {
      std::fprintf(stderr, "%s_FAIL case=gpu-upload index=%u got=%u\n", kPrefix, i, data[i]);
      ExitProcess(1);
    }
  const uint32_t *tex = data + kWords;
  for (UINT i = 0; i < 4; ++i)
    for (UINT ch = 0; ch < 4; ++ch)
      if (tex[i * 4 + ch] != ((texels[i] >> (8 * ch)) & 0xff))
        fail("gpu-upload-texture");
  readback->Unmap(0, nullptr);
  upload->Unmap(0, nullptr);
  placed->Unmap(0, nullptr);
  std::printf("%s_GPU_UPLOAD committed=1 placed=1 texture=1 words=%u\n", kPrefix, kWords);
  pass("gpu-upload-heap-cpu-write-gpu-read");
}

// ---- CreateSampler2 UINT border colors, sampled from an integer texture ----

void uintBorderColors() {
  Context c;
  auto tex = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, texDesc(DXGI_FORMAT_R32G32B32A32_UINT, 2, 2,
                                                                       D3D12_RESOURCE_FLAG_NONE),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_DESCRIPTOR_RANGE smp{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &srv};
  params[1].DescriptorTable = {1, &smp};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto root = rootSignature(c.dev.Get(), {3, params});
  auto blob = compile(kSource, "csBorder", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), {blob->GetBufferPointer(), blob->GetBufferSize()}};
  ComPtr<ID3D12PipelineState> pso;
  check(c.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "border-pso");
  ComPtr<ID3D12DescriptorHeap> srvHeap, samplerHeap;
  c.dev->CreateShaderResourceView(tex.Get(), nullptr,
                                  descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, srvHeap, true));
  auto samplerHandle = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, samplerHeap, true);
  const std::array<std::array<UINT, 4>, 4> borders = {{{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1},
      {0x01000001u, 0x81000013u, 0xfedcba97u, 0xffffffffu}}};
  std::array<std::array<UINT, 4>, 4> got{};
  for (size_t b = 0; b < borders.size(); ++b) {
    SamplerDesc2 sd{};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    std::memcpy(sd.UintBorderColor, borders[b].data(), 16);
    sd.Flags = kSamplerFlagUintBorderColor;
    c.dev->CreateSampler2(&sd, samplerHandle);
    auto out = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bufDesc(16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                         D3D12_RESOURCE_STATE_COMMON);
    auto readback = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(16), D3D12_RESOURCE_STATE_COPY_DEST);
    auto l = c.list();
    ID3D12DescriptorHeap *heaps[] = {srvHeap.Get(), samplerHeap.Get()};
    l.list->SetDescriptorHeaps(2, heaps);
    l.list->SetPipelineState(pso.Get());
    l.list->SetComputeRootSignature(root.Get());
    l.list->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    l.list->SetComputeRootDescriptorTable(1, samplerHeap->GetGPUDescriptorHandleForHeapStart());
    l.list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    l.list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = out.Get();
    l.list->ResourceBarrier(1, &uav);
    l.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 16);
    c.execute({l.list.Get()});
    void *mapped = nullptr;
    check(readback->Map(0, nullptr, &mapped), "Map(border)");
    std::memcpy(got[b].data(), mapped, 16);
    readback->Unmap(0, nullptr);
    if (got[b] != borders[b]) {
      std::fprintf(stderr, "%s_FAIL case=uint-border index=%zu got=%u,%u,%u,%u\n", kPrefix, b, got[b][0], got[b][1],
                   got[b][2], got[b][3]);
      ExitProcess(1);
    }
  }
  std::printf("%s_UINT_BORDER b0000=%u%u%u%u b0001=%u%u%u%u b1111=%u%u%u%u\n", kPrefix, got[0][0], got[0][1],
              got[0][2], got[0][3], got[1][0], got[1][1], got[1][2], got[1][3], got[2][0], got[2][1], got[2][2],
              got[2][3]);
  pass("create-sampler2-uint-border-colors");
  std::printf("%s_UINT_BORDER_ARBITRARY rgba=%08x,%08x,%08x,%08x exact=1\n", kPrefix,
      got[3][0], got[3][1], got[3][2], got[3][3]);
}

void capabilities() {
  Context c;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS18 o18{};
  check(c.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)), "OPTIONS5");
  check(c.dev->CheckFeatureSupport(kFeatureOptions16, &o16, sizeof(o16)), "OPTIONS16");
  check(c.dev->CheckFeatureSupport(kFeatureOptions18, &o18, sizeof(o18)), "OPTIONS18");
  std::printf("%s_CAPS render_passes_tier=%u render_passes_valid=%d gpu_upload_heap=%d\n", kPrefix,
              unsigned(o5.RenderPassesTier), o18.RenderPassesValid, o16.GPUUploadHeapSupported);
  if (o5.RenderPassesTier != D3D12_RENDER_PASS_TIER_0 || !o18.RenderPassesValid || !o16.GPUUploadHeapSupported)
    fail("capabilities");
  pass("capabilities");
}


void submissionOverlappingViewClears(UINT order) {
  Context c;
  auto rt = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
      texDesc(DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  check(c.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Create(alias-clear-heap)");
  const auto start = heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE views[] = {start, {start.ptr + c.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)}};
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
  for (UINT i = 0; i < 2; ++i) { D3D12_RENDER_TARGET_VIEW_DESC desc{}; desc.Format = formats[i];
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; c.dev->CreateRenderTargetView(rt.Get(), &desc, views[i]); }
  Draw draw(c.dev.Get(), formats[order], DXGI_FORMAT_UNKNOWN, 1, "psMain", false);
  auto work = c.list(), read = c.list();
  const float red[] = {1, 0, 0, 1}, blue[] = {0, 0, 1, 1}, green[] = {0, 1, 0, 1};
  work.list->ClearRenderTargetView(views[order], red, 0, nullptr);
  work.list->ClearRenderTargetView(views[1 - order], blue, 0, nullptr);
  work.list->OMSetRenderTargets(1, &views[order], FALSE, nullptr);
  const D3D12_RECT partial{0, 0, 2, 4};
  draw.draw(work.list.Get(), 4, 4, green, 0.5f, 0, nullptr, &partial);
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto out = copyOut(c.dev.Get(), read.list.Get(), rt.Get());
  transition(read.list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  check(work.list->Close(), "Close(alias-clear-work)"); check(read.list->Close(), "Close(alias-clear-read)");
  for (UINT replay = 0; replay < 2; ++replay) {
    std::fprintf(stderr, "%s_GRAPH_PHASE begin=graph-clear-view-%u replay=%u\n", kPrefix, order, replay); std::fflush(stderr);
    c.executeSealed({work.list.Get()});
    std::fprintf(stderr, "%s_GRAPH_PHASE end=graph-clear-view-%u replay=%u\n", kPrefix, order, replay); std::fflush(stderr);
    c.executeSealed({read.list.Get()});
    const auto untouched = out.at<uint32_t>(3, 0), drawn = out.at<uint32_t>(0, 0);
    std::printf("%s_GRAPH_CLEAR_VIEW order=%u replay=%u untouched=%08x drawn=%08x expected_untouched=ffff0000 expected_drawn=ff00ff00\n", kPrefix, order, replay, untouched, drawn); std::fflush(stdout);
    for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x)
      if (out.at<uint32_t>(x, y) != (x < 2 ? 0xff00ff00u : 0xffff0000u)) fail("graph-overlapping-view-clear-order");
  }
  pass(order ? "graph-overlap-sRGB-first-linear-last" : "graph-overlap-linear-first-sRGB-last");
}

}  // namespace

int main() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12"))
    return 3;
  std::printf("%s_BACKEND id=%s\n", kPrefix, backend());
  uintBorderColors();
  clearPreserve();
  for (UINT control = 0; control < 5; ++control) crossListRenderGrouping(control);
  for (UINT control = 0; control < 5; ++control) crossListUavDependency(control);
  for (UINT control = 0; control < 4; ++control) submissionAccessScheduling(control);
  submissionCrossExecuteProducer();
  submissionLongFrontier(256);
  submissionLongFrontier(512);
  submissionFrontierBoundary(false);
  submissionFrontierBoundary(true);
  submissionDisjointMips();
  submissionDepthClearSrvRead();
  submissionAttachmentUnion(false);
  submissionAttachmentUnion(true);
  submissionAttachmentUnion(true, true);
  submissionPendingClears(false);
  submissionPendingClears(true);
  submissionOverlappingViewClears(0);
  submissionOverlappingViewClears(1);
  redundantRenderTargetBindings();
  redundantDepthStencilBindings();
  wholeSurfaceRectangleClears();
  clearToRenderLoadActions();
  discardAndDepthClear();
  resolveModes();
  suspendResume();
  validation();
  gpuUploadHeap();
  capabilities();
  std::printf("%s_PASS cases=%u\n", kPrefix, gCases);
  return 0;
}
