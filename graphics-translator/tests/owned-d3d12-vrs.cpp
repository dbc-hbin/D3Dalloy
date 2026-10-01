// Owned D3D12 coarse-pixel and Tier2-source behavior, checked against exact references. Each case prints
// OWNED_D3D12_VRS_CASE_PASS <name> and the run ends with OWNED_D3D12_VRS_PASS cases=N.
// The DXIL shaders (dxil/vrs.hlsl, frozen by dxil/build-vrs.sh) are read beside the executable. Every PS
// invocation increments a UAV counter, so each case checks the invocation count too: one per coarse pixel.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 32, kHeight = 16;
constexpr float kClear = -7.0f;
int g_cases = 0;

[[noreturn]] void fail(const std::string &step, HRESULT actual = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_VRS_FAIL step=%s hr=%08lx\n", step.c_str(), static_cast<unsigned long>(actual));
  std::fflush(stderr);
  throw std::runtime_error(step);
}
void check(HRESULT actual, const char *step) {
  if (FAILED(actual)) fail(step, actual);
}
void passed(const std::string &name) {
  ++g_cases;
  std::printf("OWNED_D3D12_VRS_CASE_PASS %s\n", name.c_str());
  std::fflush(stdout);
}

std::vector<uint8_t> shader(const char *entry) {
  std::wstring name = L"vrs-";
  for (const char *c = entry; *c; ++c) name += static_cast<wchar_t>(*c);
  name += L".dxil";
  HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail(std::string("open ") + entry, HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 24)) {
    CloseHandle(file);
    fail(std::string("size ") + entry);
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != data.size()) fail(std::string("read ") + entry);
  return data;
}

using Float4 = std::array<float, 4>;

/* Rate axes of a D3D12_SHADING_RATE: log2 width in bits 2-3, log2 height in bits 0-1. */
UINT rateWidth(D3D12_SHADING_RATE rate) { return 1u << ((UINT(rate) >> 2) & 3); }
UINT rateHeight(D3D12_SHADING_RATE rate) { return 1u << (UINT(rate) & 3); }

struct Params {
  float depth_left = 0, depth_right = 0, color = 0;
  UINT unused = 0;
};

struct PsoDesc {
  const char *ps = "ps_main";
  UINT samples = 1;
  UINT sample_mask = UINT_MAX;
  bool depth = false;
  enum class StagePipeline { Vertex, Geometry, Tessellation, TessGeometry, Mesh, AmplificationMesh };
  StagePipeline stage = StagePipeline::Vertex;
  bool stream_output = false;
  bool view_instances = false;
  bool rate_viewport = false;
};
using StagePipeline = PsoDesc::StagePipeline;
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Kind, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind = Kind;
  T value{};
};

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12Resource> counter, zeros, counter_readback;
  UINT64 fence_value = 0;
  std::vector<uint8_t> vs = shader("vs_main");

  ComPtr<ID3D12PipelineState> pso(const PsoDesc &d) {
    const auto ps = shader(d.ps);
    const bool tess = d.stage == StagePipeline::Tessellation || d.stage == StagePipeline::TessGeometry;
    const bool geometry = d.stage == StagePipeline::Geometry || d.stage == StagePipeline::TessGeometry;
    const bool mesh = d.stage == StagePipeline::Mesh || d.stage == StagePipeline::AmplificationMesh;
    const bool amplification = d.stage == StagePipeline::AmplificationMesh;
    const bool rate = !std::strcmp(d.ps, "ps_rate");
    const auto gs = geometry ? shader(d.rate_viewport ? "gs_rate_view" : rate ? "gs_rate" : "gs_main") : std::vector<uint8_t>{};
    const auto hs = tess ? shader("hs_main") : std::vector<uint8_t>{};
    const auto ds = tess ? shader("ds_main") : std::vector<uint8_t>{};
    const auto ms = mesh ? shader(d.rate_viewport ? amplification ? "ms_as_rate_view" : "ms_rate_view" :
                                 amplification ? rate ? "ms_as_rate" : "ms_as" : rate ? "ms_rate" : "ms_main")
                         : std::vector<uint8_t>{};
    const auto as = amplification ? shader("as_main") : std::vector<uint8_t>{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.GS = {gs.data(), gs.size()};
    desc.HS = {hs.data(), hs.size()};
    desc.DS = {ds.data(), ds.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.IndependentBlendEnable = TRUE;
    desc.SampleMask = d.sample_mask;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = d.depth;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.PrimitiveTopologyType = tess ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = desc.RTVFormats[1] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.DSVFormat = d.depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {d.samples, 0};
    const D3D12_SO_DECLARATION_ENTRY so[] = {{0, "SV_Position", 0, 0, 4, 0},
        {0, "TEXCOORD", 0, 0, 2, 0}, {0, "FLATID", 0, 0, 1, 0}};
    const UINT so_stride = 28;
    if (d.stream_output) desc.StreamOutput = {so, 3, &so_stride, 1, 0};
    ComPtr<ID3D12PipelineState> result;
    if (mesh || d.view_instances) {
      struct Stream {
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE> hs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE> ds;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE> gs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
      } stream;
      stream.root.value = root.Get();
      stream.vs.value = mesh ? D3D12_SHADER_BYTECODE{} : desc.VS;
      stream.hs.value = desc.HS; stream.ds.value = desc.DS; stream.gs.value = desc.GS;
      stream.as.value = {as.data(), as.size()}; stream.ms.value = {ms.data(), ms.size()};
      stream.ps.value = desc.PS; stream.blend.value = desc.BlendState; stream.raster.value = desc.RasterizerState;
      stream.depth.value = desc.DepthStencilState; stream.topology.value = desc.PrimitiveTopologyType;
      stream.formats.value.NumRenderTargets = 2;
      stream.formats.value.RTFormats[0] = stream.formats.value.RTFormats[1] = desc.RTVFormats[0];
      stream.depth_format.value = desc.DSVFormat; stream.samples.value = desc.SampleDesc; stream.mask.value = desc.SampleMask;
      const D3D12_VIEW_INSTANCE_LOCATION locations[] = {{0, 0}, {0, 1}};
      if (d.view_instances) stream.views.value = {2, locations, D3D12_VIEW_INSTANCING_FLAG_NONE};
      ComPtr<ID3D12Device2> device2; check(device.As(&device2), "Device2");
      const D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(stream), &stream};
      check(device2->CreatePipelineState(&sd, IID_PPV_ARGS(&result)), "stage pipeline stream");
      return result;
    }
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&result)), d.ps);
    return result;
  }

  struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList6> list;
  };
  List begin(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT, ID3D12PipelineState *initial = nullptr) {
    List result;
    check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&result.allocator)), "allocator");
    check(device->CreateCommandList(0, type, result.allocator.Get(), initial, IID_PPV_ARGS(&result.list)), "list");
    return result;
  }
  void execute(List &list) {
    check(list.list->Close(), "Close");
    ID3D12CommandList *lists[] = {list.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), ++fence_value), "Signal");
    check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
    const DWORD waited = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0) fail("bounded GPU completion");
    check(device->GetDeviceRemovedReason(), "device removed");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&result)),
          "buffer");
    return result;
  }
};

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}

/* Two float render targets (cleared to kClear) and an optional D32 depth buffer cleared to 1. */
struct Target {
  Context &c;
  UINT samples;
  UINT layers;
  std::array<ComPtr<ID3D12Resource>, 2> colors;
  ComPtr<ID3D12Resource> depth;
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs{};
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};

  Target(Context &ctx, UINT s, bool with_depth, UINT layer_count = 1) : c(ctx), samples(s), layers(layer_count) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
    const UINT increment = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 2; ++i) {
      colors[i] = make(DXGI_FORMAT_R32G32B32A32_FLOAT, samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
      rtvs[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtvs[i].ptr += i * increment;
      c.device->CreateRenderTargetView(colors[i].Get(), nullptr, rtvs[i]);
    }
    if (with_depth) {
      depth = make(DXGI_FORMAT_D32_FLOAT, samples, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
      D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      check(c.device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsv_heap)), "dsv heap");
      dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
      c.device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    }
  }
  ComPtr<ID3D12Resource> make(DXGI_FORMAT format, UINT s, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = layers;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = s;
    desc.Format = format;
    desc.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    if (format == DXGI_FORMAT_D32_FLOAT) clear.DepthStencil.Depth = 1.0f;
    else for (float &channel : clear.Color) channel = kClear;
    ComPtr<ID3D12Resource> result;
    check(c.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state,
                                            flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                     D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? &clear : nullptr,
                                            IID_PPV_ARGS(&result)),
          "texture");
    return result;
  }
  /* Clears and binds the targets, the full viewport and scissor, the root signature, the counter and `params`. */
  void bind(ID3D12GraphicsCommandList *list, const Params &params) {
    const float color[4] = {kClear, kClear, kClear, kClear};
    for (auto rtv : rtvs) list->ClearRenderTargetView(rtv, color, 0, nullptr);
    if (depth) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(2, rtvs.data(), FALSE, depth ? &dsv : nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kWidth), LONG(kHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(c.root.Get());
    setArguments(list, params);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
  void setArguments(ID3D12GraphicsCommandList *list, const Params &params) {
    list->SetGraphicsRoot32BitConstants(0, 4, &params, 0);
    list->SetGraphicsRootUnorderedAccessView(1, c.counter->GetGPUVirtualAddress());
  }

  /* Every texel of render target `index`, row-major; multisampled targets are resolved (sample average). */
  std::vector<Float4> read(UINT index, UINT layer = 0) {
    auto list = c.begin();
    ComPtr<ID3D12Resource> source = colors[index];
    if (samples > 1) {
      source = make(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
      transition(list.list.Get(), colors[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      list.list->ResolveSubresource(source.Get(), 0, colors[index].Get(), 0, DXGI_FORMAT_R32G32B32A32_FLOAT);
      transition(list.list.Get(), colors[index].Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
      transition(list.list.Get(), source.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    } else {
      transition(list.list.Get(), source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    const UINT pitch = (kWidth * 16 + 255) & ~255u;
    auto readback = c.buffer(UINT64(pitch) * kHeight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, kWidth, kHeight, 1, pitch}};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = source.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = layer;
    list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (samples == 1)
      transition(list.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.execute(list);
    void *mapped = nullptr;
    D3D12_RANGE range{0, SIZE_T(pitch) * kHeight};
    check(readback->Map(0, &range, &mapped), "Map");
    std::vector<Float4> result(size_t(kWidth) * kHeight);
    for (UINT y = 0; y < kHeight; ++y)
      std::memcpy(&result[size_t(y) * kWidth], static_cast<const uint8_t *>(mapped) + size_t(pitch) * y, kWidth * 16);
    D3D12_RANGE none{};
    readback->Unmap(0, &none);
    return result;
  }
};

/* Zeroes the invocation counter (recorded at the start of a case's list). */
void resetCounter(Context &c, ID3D12GraphicsCommandList *list) {
  transition(list, c.counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(c.counter.Get(), 0, c.zeros.Get(), 0, 4);
  transition(list, c.counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

UINT readCounter(Context &c) {
  auto list = c.begin();
  transition(list.list.Get(), c.counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list.list->CopyBufferRegion(c.counter_readback.Get(), 0, c.counter.Get(), 0, 4);
  transition(list.list.Get(), c.counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute(list);
  void *mapped = nullptr;
  D3D12_RANGE range{0, 4};
  check(c.counter_readback->Map(0, &range, &mapped), "Map counter");
  UINT value;
  std::memcpy(&value, mapped, 4);
  D3D12_RANGE none{};
  c.counter_readback->Unmap(0, &none);
  return value;
}

bool close_to(float got, float want) { return std::fabs(got - want) <= 1e-4f * std::max(1.0f, std::fabs(want)); }

void expectCount(const char *name, Context &c, UINT want) {
  const UINT got = readCounter(c);
  if (got != want) {
    std::fprintf(stderr, "OWNED_D3D12_VRS_MISMATCH case=%s invocations=%u expected=%u\n", name, got, want);
    fail(name);
  }
}

/* `expected(x, y, rt)` for every texel of both targets; prints the first mismatches and fails the case. */
template <typename Expected> void compare(const char *name, Target &t, Expected &&expected) {
  int mismatches = 0;
  for (UINT layer = 0; layer < t.layers; ++layer) for (UINT rt = 0; rt < 2; ++rt) {
    const auto texels = t.read(rt, layer);
    for (UINT y = 0; y < kHeight; ++y)
      for (UINT x = 0; x < kWidth; ++x) {
        const Float4 got = texels[size_t(y) * kWidth + x];
        const Float4 want = expected(x, y, rt);
        bool same = true;
        for (int i = 0; i < 4; ++i) same &= close_to(got[i], want[i]);
        if (!same && mismatches++ < 6)
          std::fprintf(stderr,
                       "OWNED_D3D12_VRS_MISMATCH case=%s rt=%u x=%u y=%u got=%g,%g,%g,%g expected=%g,%g,%g,%g\n",
                       name, rt, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
      }
  }
  if (mismatches) fail(name);
}

/* The coarse pixel center of fine pixel (x, y) at rate w x h; coarse pixels are aligned to the render target. */
std::array<float, 2> coarseCenter(UINT x, UINT y, UINT w, UINT h) {
  return {float(x / w * w) + 0.5f * w, float(y / h * h) + 0.5f * h};
}

/* ps_main's two outputs at coarse center s (vrs.hlsl reference). */
Float4 mainOutput(std::array<float, 2> s, UINT w, UINT h, UINT rt, const Params &p) {
  const float ndc_x = s[0] / kWidth * 2 - 1, ndc_y = 1 - s[1] / kHeight * 2;
  const float clip_w = 1.0f / (1.0f - s[0] / (4.0f * kWidth));
  if (rt == 0) return {s[0], s[1], 0.25f * ndc_x * clip_w, 0.25f * ndc_y * clip_w};
  const float z = p.depth_left + (p.depth_right - p.depth_left) * s[0] / kWidth;
  return {float(w), float(h), z, float(0x51)};
}

/* Draws the full-target triangle with ps_main at `rate` (combiners {c0, c1}) and checks every texel and the
 * invocation count against coarse pixels of `expected` size. */
void testRate(Context &c, const char *name, UINT samples, D3D12_SHADING_RATE rate, D3D12_SHADING_RATE expected,
              std::array<D3D12_SHADING_RATE_COMBINER, 2> combiners = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                                      D3D12_SHADING_RATE_COMBINER_PASSTHROUGH}) {
  auto pso = c.pso({"ps_main", samples});
  Target t(c, samples, false);
  const Params params{0.25f, 0.75f};
  auto list = c.begin();
  resetCounter(c, list.list.Get());
  t.bind(list.list.Get(), params);
  list.list->SetPipelineState(pso.Get());
  list.list->RSSetShadingRate(rate, combiners.data());
  list.list->DrawInstanced(3, 1, 0, 0);
  c.execute(list);
  const UINT w = rateWidth(expected), h = rateHeight(expected);
  compare(name, t, [&](UINT x, UINT y, UINT rt) { return mainOutput(coarseCenter(x, y, w, h), w, h, rt, params); });
  expectCount(name, c, kWidth / w * (kHeight / h));
  passed(name);
}

/* The captured implementation executes depth-export shaders at fine rate. */
void testFineFallback(Context &c, const char *name, const char *ps) {
  const bool depth = !std::strcmp(ps, "ps_depth");
  auto pso = c.pso({ps, 1, UINT_MAX, depth});
  Target t(c, 1, depth);
  auto list = c.begin();
  resetCounter(c, list.list.Get());
  t.bind(list.list.Get(), {});
  list.list->SetPipelineState(pso.Get());
  list.list->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
  list.list->DrawInstanced(3, 1, 0, 0);
  c.execute(list);
  const auto texels = t.read(0);
  for (UINT y = 0; y < kHeight; ++y)
    for (UINT x = 0; x < kWidth; ++x) {
      const Float4 got = texels[size_t(y) * kWidth + x];
      // SV_Position.xy is the fine pixel center in every fallback shader (ps_coverage stores it in .zw).
      const float px = std::strcmp(ps, "ps_coverage") ? got[0] : got[2];
      const float py = std::strcmp(ps, "ps_coverage") ? got[1] : got[3];
      if (!close_to(px, x + 0.5f) || !close_to(py, y + 0.5f)) {
        std::fprintf(stderr, "OWNED_D3D12_VRS_MISMATCH case=%s x=%u y=%u position=%g,%g\n", name, x, y, px, py);
        fail(name);
      }
    }
  expectCount(name, c, kWidth * kHeight);
  passed(name);
}

/* discard drops whole coarse pixels; every coarse pixel still invokes the PS once. */
void testDiscard(Context &c) {
  auto pso = c.pso({"ps_discard", 1});
  Target t(c, 1, false);
  auto list = c.begin();
  resetCounter(c, list.list.Get());
  t.bind(list.list.Get(), {});
  list.list->SetPipelineState(pso.Get());
  list.list->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
  list.list->DrawInstanced(3, 1, 0, 0);
  c.execute(list);
  compare("discard_whole_coarse_pixels", t, [](UINT x, UINT y, UINT rt) -> Float4 {
    const auto s = coarseCenter(x, y, 2, 2);
    if (std::fmod(s[0], 8.0f) < 4.0f) return {kClear, kClear, kClear, kClear};
    return rt ? Float4{0, 0, 0, 1} : Float4{s[0], s[1], 1, 1};
  });
  expectCount("discard_whole_coarse_pixels", c, kWidth / 2 * (kHeight / 2));
  passed("discard_whole_coarse_pixels");
}

/* 4x MSAA at 2x2: one invocation per coarse pixel, depth tested per fine sample. The second draw's depth crosses
 * 0.5 at x = 16.5, inside coarse pixel [16, 18): fine pixel 16 keeps its two samples left of center (4x pattern x
 * offsets -2/16 and -6/16), so its resolved color is the average. */
void testMultisampleDepth(Context &c) {
  auto first = c.pso({"ps_flat", 4, UINT_MAX, true});
  auto second = c.pso({"ps_flat", 4, UINT_MAX, true});
  Target t(c, 4, true);
  auto list = c.begin();
  resetCounter(c, list.list.Get());
  t.bind(list.list.Get(), {0.5f, 0.5f, 1.0f});
  list.list->SetPipelineState(first.Get());
  list.list->DrawInstanced(3, 1, 0, 0);
  t.setArguments(list.list.Get(), {0.0f, 16.0f / 16.5f, 2.0f});
  list.list->SetPipelineState(second.Get());
  list.list->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
  list.list->DrawInstanced(3, 1, 0, 0);
  c.execute(list);
  const auto color = t.read(0);
  for (UINT y = 0; y < kHeight; ++y)
    for (UINT x = 0; x < kWidth; ++x) {
      const Float4 got = color[size_t(y) * kWidth + x];
      const float want_color = x < 16 ? 2.0f : x == 16 ? 1.5f : 1.0f;
      // The covering draw's SV_Position: the fine center of the 1x1 draw, the coarse center of the 2x2 draw.
      const auto coarse = coarseCenter(x, y, 2, 2);
      const float fine_x = x + 0.5f, fine_y = y + 0.5f;
      const float want_x = x < 16 ? coarse[0] : x == 16 ? 0.5f * (coarse[0] + fine_x) : fine_x;
      const float want_y = x < 16 ? coarse[1] : x == 16 ? 0.5f * (coarse[1] + fine_y) : fine_y;
      if (!close_to(got[0], want_color) || !close_to(got[1], want_x) || !close_to(got[2], want_y)) {
        std::fprintf(stderr, "OWNED_D3D12_VRS_MISMATCH case=msaa4x x=%u y=%u got=%g,%g,%g expected=%g,%g,%g\n", x, y,
                     got[0], got[1], got[2], want_color, want_x, want_y);
        fail("msaa4x_2x2_per_sample_depth");
      }
    }
  expectCount("msaa4x_2x2_per_sample_depth", c, kWidth * kHeight + kWidth / 2 * (kHeight / 2));
  passed("msaa4x_2x2_per_sample_depth");
}

/* Tier 2 keeps coarse shading with a partial SampleMask, but writes only the selected fine samples. */
void testPartialSampleMask(Context &c) {
  auto pso = c.pso({"ps_main", 4, 0x7});
  Target t(c, 4, false);
  auto list = c.begin();
  resetCounter(c, list.list.Get());
  t.bind(list.list.Get(), {});
  list.list->SetPipelineState(pso.Get());
  list.list->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
  list.list->DrawInstanced(3, 1, 0, 0);
  c.execute(list);
  compare("msaa4x_partial_sample_mask_coarse", t, [&](UINT x, UINT y, UINT rt) {
    auto value = mainOutput(coarseCenter(x, y, 2, 2), 2, 2, rt, {});
    for (auto &channel : value) channel = 0.75f * channel + 0.25f * kClear;
    return value;
  });
  expectCount("msaa4x_partial_sample_mask_coarse", c, kWidth * kHeight / 4);
  passed("msaa4x_partial_sample_mask_coarse");
}

/* RSSetShadingRate recorded in a bundle applies inside it and persists into the caller; ClearState resets it. */
void testBundleAndClearState(Context &c) {
  auto pso = c.pso({"ps_main", 1});
  const Params params{0.25f, 0.75f};
  auto bundle = c.begin(D3D12_COMMAND_LIST_TYPE_BUNDLE, pso.Get());
  bundle.list->SetGraphicsRootSignature(c.root.Get());
  bundle.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  bundle.list->RSSetShadingRate(D3D12_SHADING_RATE_2X1, nullptr);
  bundle.list->DrawInstanced(3, 1, 0, 0);
  check(bundle.list->Close(), "bundle Close");
  for (int clear = 0; clear < 2; ++clear) {
    Target t(c, 1, false);
    auto list = c.begin();
    resetCounter(c, list.list.Get());
    t.bind(list.list.Get(), params);
    list.list->SetPipelineState(pso.Get());
    list.list->ExecuteBundle(bundle.list.Get());
    if (clear) {
      list.list->ClearState(pso.Get());
      t.bind(list.list.Get(), params);
    }
    list.list->DrawInstanced(3, 1, 0, 0);  // 2x1 inherited from the bundle, or 1x1 after ClearState
    c.execute(list);
    const char *name = clear ? "clear_state_resets_rate" : "bundle_rate_persists";
    const UINT w = clear ? 1 : 2;
    compare(name, t, [&](UINT x, UINT y, UINT rt) { return mainOutput(coarseCenter(x, y, w, 1), w, 1, rt, params); });
    expectCount(name, c, kWidth / 2 * kHeight + kWidth / w * kHeight);
    passed(name);
  }
}

using RateTiles = std::array<uint8_t, (kWidth / 8) * (kHeight / 8)>;

ComPtr<ID3D12Resource> rateImage(Context &c, const RateTiles &rates, UINT width = kWidth / 8,
                               UINT height = kHeight / 8, bool enhanced = false,
                               D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width; desc.Height = height; desc.Flags = flags;
  desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8_UINT;
  ComPtr<ID3D12Resource> image;
  if (enhanced) {
    ComPtr<ID3D12Device10> device10; check(c.device.As(&device10), "Device10");
    D3D12_RESOURCE_DESC1 desc1{}; desc1.Dimension = desc.Dimension; desc1.Width = width; desc1.Height = height;
    desc1.DepthOrArraySize = desc1.MipLevels = desc1.SampleDesc.Count = 1; desc1.Format = DXGI_FORMAT_R8_UINT;
    desc1.Flags = flags;
    check(device10->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_COPY_DEST,
      nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&image)), "enhanced rate image");
  } else {
    check(c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image)), "rate image");
  }
  auto upload = c.buffer(256 * height, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(upload->Map(0, nullptr, &mapped), "Map rate image");
  std::memset(mapped, 0, 256 * height);
  for (UINT y = 0; y < height; ++y)
    std::memcpy(static_cast<uint8_t *>(mapped) + y * 256, rates.data() + y * (kWidth / 8), width);
  upload->Unmap(0, nullptr);
  auto list = c.begin();
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = {0, {DXGI_FORMAT_R8_UINT, width, height, 1, 256}};
  dst.pResource = image.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (enhanced) {
    ComPtr<ID3D12GraphicsCommandList7> list7; check(list.list.As(&list7), "CommandList7");
    D3D12_TEXTURE_BARRIER barrier{}; barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
    barrier.SyncAfter = D3D12_BARRIER_SYNC_PIXEL_SHADING; barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
    barrier.AccessAfter = D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE; barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
    barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE; barrier.pResource = image.Get();
    barrier.Subresources.IndexOrFirstMipLevel = 0; barrier.Subresources.NumMipLevels = barrier.Subresources.NumArraySlices = barrier.Subresources.NumPlanes = 1;
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1}; group.pTextureBarriers = &barrier;
    list7->Barrier(1, &group);
  } else {
    transition(list.list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
  }
  c.execute(list);
  return image;
}

UINT combineRate(UINT a, UINT b, UINT combiner) {
  UINT result = 0;
  for (UINT shift : {2u, 0u}) {
    const UINT x = (a >> shift) & 3, y = (b >> shift) & 3;
    const UINT value = combiner == 1 ? y : combiner == 2 ? std::min(x, y) :
                       combiner == 3 ? std::max(x, y) : combiner == 4 ? std::min(x + y, 2u) : x;
    result |= value << shift;
  }
  return result;
}

void testTier2ImageShapes(Context &c) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC valid{}; valid.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  valid.Width = valid.Height = 8; valid.DepthOrArraySize = valid.MipLevels = valid.SampleDesc.Count = 1;
  valid.Format = DXGI_FORMAT_R8_UINT;
  for (UINT bad = 0; bad < 8; ++bad) {
    auto desc = valid;
    switch (bad) {
    case 0: desc.Format = DXGI_FORMAT_R8_UNORM; break;
    case 1: desc.DepthOrArraySize = 2; break;
    case 2: desc.MipLevels = 2; break;
    case 3: desc.SampleDesc.Count = 4; break;
    case 4: desc.SampleDesc.Quality = 1; break;
    case 5: desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; break;
    case 6: desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; break;
    case 7: desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS; break;
    }
    ComPtr<ID3D12Resource> image;
    const HRESULT hr = c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, nullptr, IID_PPV_ARGS(&image));
    if (hr != E_INVALIDARG || image) fail("invalid initial shading-rate image shape", hr);
  }
  passed("tier2_image_shapes");
}

void testTier2Sources(Context &c) {
  auto oldVs = std::move(c.vs); c.vs = shader("vs_rate");
  auto pso = c.pso({"ps_rate", 1});
  c.vs = std::move(oldVs);
  // SUM of 1x2 with 1x2 (or 2x1 with 2x1) requires implementation-defined
  // sanitization of 1x4/4x1. Use supported results throughout the exact combiner
  // matrix; the independent image cases below exercise all seven supported rates.
  const RateTiles rates{0, 5, 6, 9, 10, 0, 5, 10};
  auto image = rateImage(c, rates);
  for (UINT first = 0; first < 5; ++first)
    for (UINT second = 0; second < 5; ++second) {
      const std::string name = "tier2_combiners_" + std::to_string(first) + "_" + std::to_string(second);
      Target t(c, 1, false); auto list = c.begin(); resetCounter(c, list.list.Get());
      t.bind(list.list.Get(), {0.25f, 0.75f, 0, 4});
      list.list->SetPipelineState(pso.Get());
      const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER(first), D3D12_SHADING_RATE_COMBINER(second)};
      list.list->RSSetShadingRate(D3D12_SHADING_RATE_1X2, combiners);
      list.list->RSSetShadingRateImage(image.Get());
      list.list->DrawInstanced(3, 1, 0, 0); c.execute(list);
      const UINT stage0 = combineRate(1, 4, first);
      compare(name.c_str(), t, [&](UINT x, UINT y, UINT rt) -> Float4 {
        const UINT rate = combineRate(stage0, rates[(y / 8) * (kWidth / 8) + x / 8], second);
        const UINT w = 1u << ((rate >> 2) & 3), h = 1u << (rate & 3);
        const auto s = coarseCenter(x, y, w, h);
        const auto tex = mainOutput(s, w, h, 0, {});
        return rt ? Float4{float(w), float(h), tex[2], tex[3]} : Float4{s[0], s[1], float(rate), 81};
      });
      UINT count = 0;
      for (UINT tileRate : rates) {
        const UINT rate = combineRate(stage0, tileRate, second);
        count += 64u >> (((rate >> 2) & 3) + (rate & 3));
      }
      expectCount(name.c_str(), c, count); passed(name);
    }
  for (UINT mode = 0; mode < 3; ++mode) {
    const char *name = mode == 0 ? "tier2_null_image" : mode == 1 ? "tier2_unbind_image" : "tier2_clear_image";
    Target t(c, 1, false); auto list = c.begin(); resetCounter(c, list.list.Get());
    t.bind(list.list.Get(), {0, 0, 0, 4}); list.list->SetPipelineState(pso.Get());
    list.list->RSSetShadingRateImage(mode ? image.Get() : nullptr);
    if (mode == 2) { list.list->ClearState(pso.Get()); t.bind(list.list.Get(), {0, 0, 0, 4}); }
    else list.list->RSSetShadingRateImage(nullptr);
    const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_OVERRIDE, D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    list.list->RSSetShadingRate(D3D12_SHADING_RATE_4X4, combiners);
    list.list->DrawInstanced(3, 1, 0, 0); c.execute(list);
    compare(name, t, [&](UINT x, UINT y, UINT rt) -> Float4 {
      auto s = coarseCenter(x, y, 1, 1); auto tex = mainOutput(s, 1, 1, 0, {});
      return rt ? Float4{1, 1, tex[2], tex[3]} : Float4{s[0], s[1], 0, 81};
    });
    expectCount(name, c, kWidth * kHeight); passed(name);
  }
}

void testTier2ImageEdges(Context &c) {
  auto oldVs = std::move(c.vs); c.vs = shader("vs_rate"); auto pso = c.pso({"ps_rate", 1}); c.vs = std::move(oldVs);
  const RateTiles rates{10, 1, 4, 5, 6, 9, 10, 0};
  for (UINT mode = 0; mode < 3; ++mode) {
    const char *name = mode == 0 ? "tier2_small_image" : mode == 1 ? "tier2_enhanced_image" : "tier2_deny_srv_image";
    auto image = rateImage(c, rates, mode ? kWidth / 8 : 1, mode ? kHeight / 8 : 1, mode == 1,
      mode == 2 ? D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE : D3D12_RESOURCE_FLAG_NONE);
    Target t(c, 1, false); auto list = c.begin(); resetCounter(c, list.list.Get());
    t.bind(list.list.Get(), {0, 0, 0, 4}); list.list->SetPipelineState(pso.Get());
    const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    list.list->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners); list.list->RSSetShadingRateImage(image.Get());
    list.list->DrawInstanced(3, 1, 0, 0); c.execute(list);
    compare(name, t, [&](UINT x, UINT y, UINT rt) -> Float4 {
      const UINT rate = mode ? rates[(y / 8) * (kWidth / 8) + x / 8] : x < 8 && y < 8 ? 10 : 0;
      const UINT w = 1u << ((rate >> 2) & 3), h = 1u << (rate & 3);
      auto s = coarseCenter(x, y, w, h); auto tex = mainOutput(s, w, h, 0, {});
      return rt ? Float4{float(w), float(h), tex[2], tex[3]} : Float4{s[0], s[1], float(rate), 81};
    });
    UINT count = mode ? 0 : kWidth * kHeight - 64 + 4;
    if (mode) for (UINT rate : rates) count += 64u >> (((rate >> 2) & 3) + (rate & 3));
    expectCount(name, c, count); passed(name);
  }
}

void testTier2StageSources(Context &c) {
  const StagePipeline stages[]{StagePipeline::Geometry, StagePipeline::Tessellation, StagePipeline::TessGeometry,
    StagePipeline::Mesh, StagePipeline::AmplificationMesh};
  const char *names[]{"gs", "ts", "tsgs", "ms", "as_ms"};
  const RateTiles rates{0, 1, 4, 5, 6, 9, 10, 0};
  auto image = rateImage(c, rates);
  for (UINT i = 0; i < std::size(stages); ++i) {
    const std::string name = std::string("tier2_stage_") + names[i];
    auto pso = c.pso({"ps_rate", 1, UINT_MAX, false, stages[i]});
    Target t(c, 1, false); auto list = c.begin(); resetCounter(c, list.list.Get());
    t.bind(list.list.Get(), {0, 0, 0, 4}); list.list->SetPipelineState(pso.Get());
    if (i == 1 || i == 2) list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_SUM, D3D12_SHADING_RATE_COMBINER_MAX};
    list.list->RSSetShadingRate(D3D12_SHADING_RATE_1X2, combiners); list.list->RSSetShadingRateImage(image.Get());
    if (i >= 3) list.list->DispatchMesh(i == 4 ? 2 : 1, 1, 1); else list.list->DrawInstanced(3, 1, 0, 0);
    c.execute(list);
    const UINT stage0 = combineRate(1, i == 1 ? 0 : 4, D3D12_SHADING_RATE_COMBINER_SUM);
    compare(name.c_str(), t, [&](UINT x, UINT y, UINT rt) -> Float4 {
      const UINT rate = combineRate(stage0, rates[(y / 8) * (kWidth / 8) + x / 8], D3D12_SHADING_RATE_COMBINER_MAX);
      const UINT w = 1u << ((rate >> 2) & 3), h = 1u << (rate & 3);
      auto s = coarseCenter(x, y, w, h); auto tex = mainOutput(s, w, h, 0, {});
      return rt ? Float4{float(w), float(h), tex[2], tex[3]} : Float4{s[0], s[1], float(rate), i == 4 ? 82.0f : 81.0f};
    });
    UINT count = 0;
    for (UINT imageRate : rates) {
      const UINT rate = combineRate(stage0, imageRate, D3D12_SHADING_RATE_COMBINER_MAX);
      count += 64u >> (((rate >> 2) & 3) + (rate & 3));
    }
    expectCount(name.c_str(), c, count * (i == 4 ? 4 : 1)); passed(name);
  }
}

void testTier2ViewportSources(Context &c) {
  const StagePipeline stages[]{StagePipeline::Vertex, StagePipeline::Geometry, StagePipeline::Mesh, StagePipeline::AmplificationMesh};
  const char *names[]{"vs", "gs", "ms", "as_ms"};
  for (UINT i = 0; i < std::size(stages); ++i) {
    const std::string name = std::string("tier2_viewport_") + names[i];
    auto oldVs = std::move(c.vs); c.vs = shader(i == 0 ? "vs_rate_view" : "vs_main");
    auto pso = c.pso({"ps_rate", 1, UINT_MAX, false, stages[i], false, false, true});
    c.vs = std::move(oldVs);
    Target t(c, 1, false); auto list = c.begin(); resetCounter(c, list.list.Get());
    t.bind(list.list.Get(), {0, 0, 0, 4}); list.list->SetPipelineState(pso.Get());
    const D3D12_VIEWPORT views[]{{0, 0, 1, 1, 0, 1}, {0, 0, float(kWidth), float(kHeight), 0, 1}};
    const D3D12_RECT scissors[]{{0, 0, 0, 0}, {0, 0, LONG(kWidth), LONG(kHeight)}};
    list.list->RSSetViewports(2, views); list.list->RSSetScissorRects(2, scissors);
    const D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_OVERRIDE, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    list.list->RSSetShadingRate(D3D12_SHADING_RATE_4X4, combiners);
    if (i >= 2) list.list->DispatchMesh(1, 1, 1); else list.list->DrawInstanced(3, 1, 0, 0);
    c.execute(list);
    compare(name.c_str(), t, [&](UINT x, UINT y, UINT rt) -> Float4 {
      auto s = coarseCenter(x, y, 2, 1); auto tex = mainOutput(s, 2, 1, 0, {});
      return rt ? Float4{2, 1, tex[2], tex[3]} : Float4{s[0], s[1], 4, 81};
    });
    expectCount(name.c_str(), c, kWidth / 2 * kHeight * (i == 3 ? 2 : 1)); passed(name);
  }
}

// COMMON is not promotable to SRS. Hold the FIFO worker so API capture sees
// unknown state, then release it and require submission-time rejection.
void testTier2CommonSubmission(Context &c) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kWidth / 8; desc.Height = kHeight / 8;
  desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8_UINT;
  ComPtr<ID3D12Resource> image;
  check(c.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&image)), "COMMON rate image");
  auto pso = c.pso({"ps_rate", 1});
  Target target(c, 1, false); auto list = c.begin();
  target.bind(list.list.Get(), {}); list.list->SetPipelineState(pso.Get());
  list.list->RSSetShadingRateImage(image.Get());
  list.list->DrawInstanced(3, 1, 0, 0);
  check(list.list->Close(), "COMMON rate Close");
  ComPtr<ID3D12Fence> gate, removed;
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "COMMON gate fence");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed)), "COMMON removal fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) fail("COMMON removal event");
  check(removed->SetEventOnCompletion(1, event), "COMMON removal registration");
  check(c.queue->Wait(gate.Get(), 1), "COMMON FIFO gate");
  ID3D12CommandList *lists[]{list.list.Get()};
  c.queue->ExecuteCommandLists(1, lists);
  check(c.device->GetDeviceRemovedReason(), "COMMON capture remains healthy");
  check(gate->Signal(1), "COMMON release gate");
  const DWORD waited = WaitForSingleObject(event, 20000);
  CloseHandle(event);
  if (waited != WAIT_OBJECT_0 || c.device->GetDeviceRemovedReason() != DXGI_ERROR_DEVICE_REMOVED)
    fail("COMMON to SRS submission must reject");
  std::puts("OWNED_D3D12_VRS_COMMON_SUBMISSION_PASS capture=S_OK submission=887a0005 promotion=0");
}

void testTier2Pulls(Context &c) {
  struct Case { const char *name, *ps; UINT samples, w, h, index; bool partial; };
  std::vector<Case> cases{
    {"tier2_centroid_full", "ps_centroid", 1, 2, 2, 0, false},
    {"tier2_centroid_partial", "ps_centroid", 4, 2, 2, 0, true},
    {"tier2_wide_snapped", "ps_wide_snapped", 1, 4, 2, 0, false},
    {"tier2_snapped_negative", "ps_snapped", 1, 2, 2, 0, false},
  };
  for (UINT sample = 0; sample < 16; ++sample)
    cases.push_back({"tier2_sample", "ps_sample", 4, 2, 2, sample, false});
  for (const auto &test : cases) {
    const std::string name = std::string(test.name) + (std::strcmp(test.ps, "ps_sample") ? "" : "_" + std::to_string(test.index));
    auto pso = c.pso({test.ps, test.samples}); Target t(c, test.samples, false); auto list = c.begin();
    resetCounter(c, list.list.Get()); t.bind(list.list.Get(), {0, 0, 0, test.index});
    if (test.partial) { const D3D12_RECT rect{1, 1, LONG(kWidth - 1), LONG(kHeight - 1)}; list.list->RSSetScissorRects(1, &rect); }
    list.list->SetPipelineState(pso.Get());
    const UINT rate = (UINT(std::countr_zero(test.w)) << 2) | UINT(std::countr_zero(test.h));
    list.list->RSSetShadingRate(D3D12_SHADING_RATE(rate), nullptr);
    list.list->DrawInstanced(3, 1, 0, 0); c.execute(list);
    compare(name.c_str(), t, [&](UINT x, UINT y, UINT rt) -> Float4 {
      if (test.partial && (x == 0 || y == 0 || x == kWidth - 1 || y == kHeight - 1))
        return {kClear, kClear, kClear, kClear};
      const auto center = coarseCenter(x, y, test.w, test.h); auto point = center;
      const bool sample = !std::strcmp(test.ps, "ps_sample");
      if (sample) {
        const UINT fine = test.index / test.samples, s = test.index % test.samples;
        static constexpr int sx[]{-2, 6, -6, 2}, sy[]{-6, -2, 2, 6};
        point = {float(x / test.w * test.w + fine % test.w) + 0.5f + sx[s] / 16.0f,
                 float(y / test.h * test.h + fine / test.w) + 0.5f + sy[s] / 16.0f};
      } else if (!std::strcmp(test.ps, "ps_centroid")) {
        const UINT left = x / test.w * test.w, top = y / test.h * test.h;
        point = {float(test.partial ? std::max(1u, left) : left) + 0.5f - (test.samples == 4 ? 2.0f / 16 : 0),
                 float(test.partial ? std::max(1u, top) : top) + 0.5f - (test.samples == 4 ? 6.0f / 16 : 0)};
      } else if (!std::strcmp(test.ps, "ps_wide_snapped")) {
        point[0] += 23.0f / 16; point[1] += 7.0f / 16;
      } else if (!std::strcmp(test.ps, "ps_snapped")) { point[0] -= 0.5f; point[1] -= 0.5f; }
      const auto tex = mainOutput(point, test.w, test.h, 0, {});
      const bool snapped = !std::strcmp(test.ps, "ps_snapped");
      return rt ? Float4{snapped ? 0.0f : float(test.w), snapped ? 0.0f : float(test.h), 0, 1}
                : Float4{center[0], center[1], tex[2], tex[3]};
    });
    expectCount(name.c_str(), c, kWidth / test.w * (kHeight / test.h)); passed(name);
  }
}

void testTier2Coverage(Context &c) {
  auto pso = c.pso({"ps_coverage", 1}); Target t(c, 1, false); auto list = c.begin();
  resetCounter(c, list.list.Get()); t.bind(list.list.Get(), {}); list.list->SetPipelineState(pso.Get());
  list.list->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
  list.list->DrawInstanced(3, 1, 0, 0); c.execute(list);
  compare("tier2_coverage_input", t, [](UINT x, UINT y, UINT rt) -> Float4 {
    auto s = coarseCenter(x, y, 2, 2); return rt ? Float4{0, 0, 0, 1} : Float4{4, 15, s[0], s[1]};
  });
  expectCount("tier2_coverage_input", c, kWidth * kHeight / 4); passed("tier2_coverage_input");
}

enum class StageExecution { Direct, Indexed, Indirect, View, Statistics, StreamOutput, IndirectZero };

void testStageRate(Context &c, StagePipeline stage, const char *stage_name, D3D12_SHADING_RATE rate,
                   const char *rate_name, StageExecution execution = StageExecution::Direct) {
  const bool tess = stage == StagePipeline::Tessellation || stage == StagePipeline::TessGeometry;
  const bool mesh = stage == StagePipeline::Mesh || stage == StagePipeline::AmplificationMesh;
  const bool amplification = stage == StagePipeline::AmplificationMesh;
  const bool indirect = execution == StageExecution::Indirect || execution == StageExecution::IndirectZero;
  const bool zero = execution == StageExecution::IndirectZero;
  const bool indexed = execution == StageExecution::Indexed;
  const bool view = execution == StageExecution::View;
  const bool statistics = execution == StageExecution::Statistics;
  const bool so = execution == StageExecution::StreamOutput;
  const std::string name = std::string("stage_") + stage_name + "_" + rate_name;
  auto pso = c.pso({"ps_main", 1, UINT_MAX, false, stage, so, view});
  Target target(c, 1, false, view ? 2 : 1);
  const Params params{0.25f, 0.75f};
  auto list = c.begin();
  resetCounter(c, list.list.Get());
  target.bind(list.list.Get(), params);
  list.list->SetPipelineState(pso.Get());
  list.list->IASetPrimitiveTopology(tess ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                       : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list.list->RSSetShadingRate(rate, nullptr);
  ComPtr<ID3D12Resource> indices, arguments, count, so_buffer, so_filled, so_readback;
  ComPtr<ID3D12CommandSignature> signature;
  ComPtr<ID3D12QueryHeap> queries;
  ComPtr<ID3D12Resource> statistics_readback;
  if (indexed) {
    indices = c.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr; check(indices->Map(0, nullptr, &mapped), "stage index map");
    const uint16_t data[] = {0xffff, 0, 1, 2}; std::memcpy(mapped, data, sizeof(data)); indices->Unmap(0, nullptr);
    const D3D12_INDEX_BUFFER_VIEW iv{indices->GetGPUVirtualAddress(), sizeof(data), DXGI_FORMAT_R16_UINT};
    list.list->IASetIndexBuffer(&iv);
  }
  if (statistics) {
    const D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, 1, 0};
    check(c.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "stage statistics query");
    statistics_readback = c.buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    list.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
  }
  if (so) {
    so_buffer = c.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
    so_filled = c.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    so_readback = c.buffer(512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    list.list->CopyBufferRegion(so_filled.Get(), 0, c.zeros.Get(), 0, 8);
    transition(list.list.Get(), so_filled.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW sv{so_buffer->GetGPUVirtualAddress(), 256, so_filled->GetGPUVirtualAddress()};
    list.list->SOSetTargets(0, 1, &sv);
  }
  if (indirect) {
    arguments = c.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    count = c.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = mesh ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    const UINT stride = mesh ? sizeof(D3D12_DISPATCH_MESH_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    const D3D12_COMMAND_SIGNATURE_DESC sd{stride, 1, &argument, 0};
    check(c.device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)), "stage indirect signature");
    void *mapped = nullptr; check(arguments->Map(0, nullptr, &mapped), "stage argument map");
    std::memset(mapped, 0, 256);
    if (mesh) {
      const D3D12_DISPATCH_MESH_ARGUMENTS data[] = {{0, 1, 1}, {amplification ? 2u : 1u, 1, 1}, {1, 1, 1}};
      std::memcpy(static_cast<uint8_t *>(mapped) + 16, data, sizeof(data));
    } else {
      const D3D12_DRAW_ARGUMENTS data[] = {{0, 1, 0, 0}, {3, 1, 0, 0}, {3, 1, 0, 0}};
      std::memcpy(static_cast<uint8_t *>(mapped) + 16, data, sizeof(data));
    }
    arguments->Unmap(0, nullptr);
    check(count->Map(0, nullptr, &mapped), "stage count map");
    const UINT effective_count = zero ? 0 : 2; std::memcpy(static_cast<uint8_t *>(mapped) + 4, &effective_count, 4);
    count->Unmap(0, nullptr);
    list.list->ExecuteIndirect(signature.Get(), 3, arguments.Get(), 16, count.Get(), 4);
  } else if (mesh) list.list->DispatchMesh(amplification ? 2 : 1, 1, 1);
  else if (indexed) list.list->DrawIndexedInstanced(3, 1, 1, 0, 0);
  else list.list->DrawInstanced(3, 1, 0, 0);
  if (statistics) {
    list.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    list.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0, 1, statistics_readback.Get(), 0);
  }
  if (so) {
    transition(list.list.Get(), so_buffer.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.list.Get(), so_filled.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list.list->CopyBufferRegion(so_readback.Get(), 0, so_filled.Get(), 0, 8);
    list.list->CopyBufferRegion(so_readback.Get(), 16, so_buffer.Get(), 0, 84);
  }
  c.execute(list);
  const UINT w = rateWidth(rate), h = rateHeight(rate);
  compare(name.c_str(), target, [&](UINT x, UINT y, UINT rt) -> Float4 {
    if (zero) return {kClear, kClear, kClear, kClear};
    auto result = mainOutput(coarseCenter(x, y, w, h), w, h, rt, params);
    if (amplification && rt == 1) result[3] = 0x52; // All children of the last AS parent retire last.
    return result;
  });
  const UINT invocations = zero ? 0 : kWidth / w * (kHeight / h) * (amplification ? 4 : 1) * target.layers;
  expectCount(name.c_str(), c, invocations);
  if (statistics) {
    void *mapped = nullptr; check(statistics_readback->Map(0, nullptr, &mapped), "stage statistics map");
    D3D12_QUERY_DATA_PIPELINE_STATISTICS1 result{}; std::memcpy(&result, mapped, sizeof(result));
    statistics_readback->Unmap(0, nullptr);
    if (result.PSInvocations != invocations) fail(name + " PSInvocations");
    if (mesh && (result.MSInvocations != (amplification ? 128u : 32u) ||
                 result.ASInvocations != (amplification ? 64u : 0u) || result.MSPrimitives != (amplification ? 4u : 1u)))
      fail(name + " stage capture must execute once");
  }
  if (so) {
    void *mapped = nullptr; check(so_readback->Map(0, nullptr, &mapped), "stage SO map");
    UINT64 bytes = 0; std::memcpy(&bytes, mapped, 8);
    if (bytes != 84) fail(name + " SO filled bytes");
    for (UINT v = 0; v < 3; ++v) {
      float result[6]; UINT flat = 0;
      auto *record = static_cast<uint8_t *>(mapped) + 16 + v * 28;
      std::memcpy(result, record, 24); std::memcpy(&flat, record + 24, 4);
      const float clip_w = v == 1 ? 2.f : 1.f;
      const float nx = v == 1 ? 3.f : -1.f, ny = v == 2 ? -3.f : 1.f;
      const float z = v == 1 ? 2 * params.depth_right - params.depth_left : params.depth_left;
      const float expected[] = {nx * clip_w, ny * clip_w, z * clip_w, clip_w, nx * clip_w * .25f, ny * clip_w * .25f};
      for (UINT lane = 0; lane < 6; ++lane) if (!close_to(result[lane], expected[lane])) fail(name + " SO vertex");
      if (flat != 0x51 + v) fail(name + " SO flat output");
    }
    so_readback->Unmap(0, nullptr);
  }
  passed(name);
}

void runStageRates(Context &c) {
  const std::array stages = {StagePipeline::Geometry, StagePipeline::Tessellation, StagePipeline::TessGeometry,
                             StagePipeline::Mesh, StagePipeline::AmplificationMesh};
  const char *names[] = {"gs", "ts", "tsgs", "ms", "as_ms"};
  const D3D12_SHADING_RATE rates[] = {D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_2X1,
      D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_4X2, D3D12_SHADING_RATE_4X4};
  const char *rate_names[] = {"rate_1x1", "rate_1x2", "rate_2x1", "rate_2x2", "rate_2x4", "rate_4x2", "rate_4x4"};
  for (size_t i = 0; i < stages.size(); ++i) {
    for (size_t r = 0; r < std::size(rates); ++r) testStageRate(c, stages[i], names[i], rates[r], rate_names[r]);
    testStageRate(c, stages[i], names[i], D3D12_SHADING_RATE_2X2, "stats_2x2", StageExecution::Statistics);
    if (i < 3) testStageRate(c, stages[i], names[i], D3D12_SHADING_RATE_2X2, "indexed_2x2", StageExecution::Indexed);
    testStageRate(c, stages[i], names[i], D3D12_SHADING_RATE_2X2, "indirect_2x2", StageExecution::Indirect);
    testStageRate(c, stages[i], names[i], D3D12_SHADING_RATE_2X2, "indirect_count_zero", StageExecution::IndirectZero);
    testStageRate(c, stages[i], names[i], D3D12_SHADING_RATE_2X2, "view_2x2", StageExecution::View);
  }
  testStageRate(c, StagePipeline::Geometry, "gs", D3D12_SHADING_RATE_2X2, "so_2x2", StageExecution::StreamOutput);
}

int run() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) {
    std::fprintf(stderr, "OWNED_D3D12_VRS_FAIL step=backend\n");
    return 3;
  }
  std::printf("OWNED_D3D12_VRS_BACKEND id=%s\n", backend());
  Context c;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)), "D3D12CreateDevice");
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)), "OPTIONS6");
  std::printf("OWNED_D3D12_VRS_CAPS tier=%d additional=%d tile=%u per_primitive_viewports=%d\n",
              int(options6.VariableShadingRateTier), int(options6.AdditionalShadingRatesSupported),
              options6.ShadingRateImageTileSize, int(options6.PerPrimitiveShadingRateSupportedWithViewportIndexing));
  if (options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_2 ||
      !options6.AdditionalShadingRatesSupported || options6.ShadingRateImageTileSize != 8 ||
      !options6.PerPrimitiveShadingRateSupportedWithViewportIndexing)
    fail("Tier2 OPTIONS6 contract");
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  check(c.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&c.queue)), "queue");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 4};
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor = {0, 0};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rd{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
  ComPtr<ID3DBlob> bytes, errors;
  check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "serialize root");
  check(c.device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&c.root)),
        "root");
  c.counter = c.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  c.zeros = c.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(c.zeros->Map(0, nullptr, &mapped), "Map zeros");
  std::memset(mapped, 0, 256);
  c.zeros->Unmap(0, nullptr);
  c.counter_readback = c.buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

  testRate(c, "rate_1x1", 1, D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_1X1);
  testRate(c, "rate_1x2", 1, D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_1X2);
  testRate(c, "rate_2x1", 1, D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_2X1);
  testRate(c, "rate_2x2", 1, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2);
  testRate(c, "rate_2x4", 1, D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_2X4);
  testRate(c, "rate_4x2", 1, D3D12_SHADING_RATE_4X2, D3D12_SHADING_RATE_4X2);
  testRate(c, "rate_4x4", 1, D3D12_SHADING_RATE_4X4, D3D12_SHADING_RATE_4X4);
  testRate(c, "msaa2x_rate_2x2", 2, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2);
  testRate(c, "msaa2x_rate_2x4", 2, D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_2X4);
  testRate(c, "msaa4x_rate_1x2", 4, D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_1X2);
  testRate(c, "msaa4x_rate_2x2", 4, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2);
  // Unsupported combinations are sanitized: the larger axis is reduced (x on ties) until supported.
  testRate(c, "msaa4x_rate_2x4_sanitized_2x2", 4, D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_2X2);
  testRate(c, "msaa4x_rate_4x4_sanitized_2x2", 4, D3D12_SHADING_RATE_4X4, D3D12_SHADING_RATE_2X2);
  testRate(c, "msaa2x_rate_4x4_sanitized_2x4", 2, D3D12_SHADING_RATE_4X4, D3D12_SHADING_RATE_2X4);
  testRate(c, "msaa8x_rate_2x2_fine", 8, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_1X1);
  // With no primitive output or image binding, both additional sources contribute 1x1.
  testRate(c, "combiner_override_1x1", 1, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_1X1,
           {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_OVERRIDE});
  testRate(c, "combiner_min_1x1", 1, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_1X1,
           {D3D12_SHADING_RATE_COMBINER_MIN, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH});
  testRate(c, "combiner_max_keeps_base", 1, D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_2X4,
           {D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX});
  testRate(c, "combiner_sum_keeps_base", 1, D3D12_SHADING_RATE_4X2, D3D12_SHADING_RATE_4X2,
           {D3D12_SHADING_RATE_COMBINER_SUM, D3D12_SHADING_RATE_COMBINER_SUM});
  testDiscard(c);
  testMultisampleDepth(c);
  testPartialSampleMask(c);
  testFineFallback(c, "depth_export_fine", "ps_depth");
  testBundleAndClearState(c);
  runStageRates(c);
  testTier2ImageShapes(c);
  testTier2Sources(c);
  testTier2ImageEdges(c);
  testTier2StageSources(c);
  testTier2Pulls(c);
  testTier2Coverage(c);
  testTier2ViewportSources(c);
  testTier2CommonSubmission(c);
  std::printf("OWNED_D3D12_VRS_PASS cases=%d\n", g_cases);
  return 0;
}

} // namespace

int main() {
  try {
    return run();
  } catch (const std::exception &) {
    return 1;
  }
}
