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
    queue->ExecuteCommandLists(UINT(batch.size()), batch.data());
    check(queue->Signal(fence.Get(), ++value), "Signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
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
// Per-sample distinct values for resolve tests: sample i writes (i + 1) / 8 in red.
float4 psSamples(uint sample : SV_SampleIndex) : SV_Target { return float4((sample + 1) / 8.0, 0, 0, 1); }

Texture2D<uint4> IntTex : register(t0);
SamplerState Border : register(s0);
RWStructuredBuffer<uint4> Out : register(u0);
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
       bool depth_write = false) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 6;
    root = rootSignature(dev, {1, &param});
    auto vs = compile(kSource, "vsMain", "vs_5_0");
    auto pixel = compile(kSource, ps, "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = samples;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rt == DXGI_FORMAT_UNKNOWN ? 0 : 1;
    pd.RTVFormats[0] = rt;
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
  void draw(ID3D12GraphicsCommandList *l, UINT w, UINT h, const float color[4], float z = 0.5f, UINT mode = 0) {
    l->SetPipelineState(pso.Get());
    l->SetGraphicsRootSignature(root.Get());
    const UINT constants[6] = {std::bit_cast<UINT>(color[0]), std::bit_cast<UINT>(color[1]),
                               std::bit_cast<UINT>(color[2]), std::bit_cast<UINT>(color[3]),
                               std::bit_cast<UINT>(z), mode};
    l->SetGraphicsRoot32BitConstants(0, 6, constants, 0);
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
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

// 4x MSAA pass with ENDING_ACCESS_RESOLVE: AVERAGE into one target, MIN and MAX of the sample values
// (1..4)/8 into others via the subresource parameters, including a sub-rectangle with a DstX offset.
void resolveModes() {
  Context c;
  constexpr auto kFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto kFloat = DXGI_FORMAT_R32_FLOAT;
  auto run = [&](DXGI_FORMAT fmt, D3D12_RESOLVE_MODE mode, bool offset) {
    auto msaa = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                          texDesc(fmt, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 4),
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The translator's resolve renders into the destination, which therefore needs ALLOW_RENDER_TARGET
    // (a Metal render-target usage); see the report's constraint on ResolveSubresourceRegion.
    auto dst = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT,
                         texDesc(fmt, 8, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RESOLVE_DEST);
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    auto rtv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtvHeap);
    c.dev->CreateRenderTargetView(msaa.Get(), nullptr, rtv);
    Draw samples(c.dev.Get(), fmt, DXGI_FORMAT_UNKNOWN, 4, "psSamples", false);
    auto l = c.list();
    D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS region{0, 0, offset ? 4u : 0u, 0, {0, 0, 4, 4}};
    auto access = rtAccess(rtv, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD,
                           D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE);
    access.EndingAccess.Resolve = {msaa.Get(), dst.Get(), 1, &region, fmt, mode, FALSE};
    l.list->BeginRenderPass(1, &access, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    const float unused[4] = {};
    samples.draw(l.list.Get(), 4, 4, unused);
    l.list->EndRenderPass();
    transition(l.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto out = copyOut(c.dev.Get(), l.list.Get(), dst.Get());
    c.execute({l.list.Get()});
    return out;
  };
  auto average = run(kFmt, D3D12_RESOLVE_MODE_AVERAGE, false);
  auto minimum = run(kFloat, D3D12_RESOLVE_MODE_MIN, true);
  auto maximum = run(kFloat, D3D12_RESOLVE_MODE_MAX, false);
  // Samples write 1/8..4/8: average 2.5/8 = 0.3125 -> 80 (0x50); min 0.125; max 0.5.
  const uint32_t avg = average.at<uint32_t>(1, 1);
  const float mn = minimum.at<float>(5, 1), mx = maximum.at<float>(1, 1), untouched = minimum.at<float>(1, 1);
  std::printf("%s_RESOLVE average=%08x min=%.6f max=%.6f min_left=%.6f\n", kPrefix, avg, mn, mx, untouched);
  if ((avg & 0xff) < 0x4f || (avg & 0xff) > 0x51 || (avg >> 24) != 0xff || mn != 0.125f || mx != 0.5f)
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

}  // namespace

int main() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12"))
    return 3;
  std::printf("%s_BACKEND id=%s\n", kPrefix, backend());
  uintBorderColors();
  clearPreserve();
  discardAndDepthClear();
  resolveModes();
  suspendResume();
  validation();
  gpuUploadHeap();
  capabilities();
  std::printf("%s_PASS cases=%u\n", kPrefix, gCases);
  return 0;
}
