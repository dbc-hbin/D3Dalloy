// D3D12 ConservativeRasterizationTier 3 against independent public numeric oracles.
//
// Every case draws through D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON and records, per pixel, the PS invocation
// count, the OR of SV_Coverage and SV_Position.z (conservative-raster.hlsl). The oracle snaps the projected
// vertices to the 16.8 fixed-point grid and covers a pixel when its square overlaps the snapped triangle with
// positive area; a pixel within 1/128 px of the boundary (float projection rounding) may go either way.
// Cases: the specification's bring-up, 1/2 pixel, TIR ForcedSampleCount 4 and 4x MSAA examples; random thin,
// sliver, sub-pixel and perspective triangles through VS, VS+GS and VS+HS+DS, DXBC and DXIL; back-face culling;
// SampleMask; per-sample depth extrapolation clamped to the viewport depth range at 4x; and PSO validation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT kSize = 32;
constexpr UINT kRecordBytes = kSize * kSize * 16;
constexpr UINT kDepthBytes = kSize * kSize * 4 * 4;
constexpr char kMarker[] = "OWNED_D3D12_CONSERVATIVE_RASTER";

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=%08lx\n", kMarker, step, static_cast<unsigned long>(hr));
  std::printf("%s_FAIL step=%s\n", kMarker, step);
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool condition, const char *step) { if (!condition) fail(step); }

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type,
                              D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  auto h = heap(type);
  ComPtr<ID3D12Resource> r;
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
  return r;
}
std::vector<uint8_t> file(const char *name) {
  FILE *f = std::fopen(name, "rb");
  if (!f) fail(name);
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> bytes(size > 0 ? size_t(size) : 0);
  require(size > 0 && std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size(), name);
  std::fclose(f);
  return bytes;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}

// One triangle: clip-space positions.
using Triangle = std::array<std::array<float, 4>, 3>;
struct Viewport { float x, y, width, height, min_depth, max_depth; };

/* ---- CPU oracle ---- */
struct Snapped { std::array<double, 8> xy; double area; UINT count = 3; };
Snapped snap(const Triangle &t, const Viewport &v, bool distance = false) {
  Snapped s{};
  std::array<std::array<float, 4>, 4> polygon{};
  if (distance) {
    if (std::all_of(t.begin(), t.end(), [](const auto &p) { return p[1] + 0.8f * p[3] < 0; })) {
      s.count = 0; return s;
    }
    s.count = 0;
    for (UINT i = 0; i < 3; ++i) {
      const auto &a = t[i], &b = t[(i + 1) % 3];
      const float da = a[0] + 0.1f * a[3], db = b[0] + 0.1f * b[3];
      if (da >= 0) polygon[s.count++] = a;
      if ((da >= 0) != (db >= 0)) {
        const float alpha = da / (da - db);
        for (UINT lane = 0; lane < 4; ++lane) polygon[s.count][lane] = a[lane] + alpha * (b[lane] - a[lane]);
        ++s.count;
      }
    }
    if (s.count < 3) { s.count = 0; return s; }
  } else std::copy(t.begin(), t.end(), polygon.begin());
  for (UINT i = 0; i < s.count; i++) {
    // D3D viewport transform and public 16.8 round-to-nearest grid.
    const float sx = v.x + (polygon[i][0] / polygon[i][3] + 1.0f) * v.width * 0.5f;
    const float sy = v.y + (1.0f - polygon[i][1] / polygon[i][3]) * v.height * 0.5f;
    s.xy[2 * i] = std::rint(double(sx) * 256.0) / 256.0;
    s.xy[2 * i + 1] = std::rint(double(sy) * 256.0) / 256.0;
  }
  for (UINT i = 0; i < s.count; ++i) {
    const UINT next = (i + 1) % s.count;
    s.area += s.xy[2 * i] * s.xy[2 * next + 1] - s.xy[2 * i + 1] * s.xy[2 * next];
  }
  return s;
}
// Square [px - grow, px + 1 + grow] overlaps the snapped triangle with positive area (separating axes).
bool overlaps(const Snapped &s, int px, int py, double grow) {
  if (s.count < 3) return false;
  const double lo_x = px - grow, hi_x = px + 1 + grow, lo_y = py - grow, hi_y = py + 1 + grow;
  if (s.area == 0) {
    // Tier2+ retains post-snap points/lines. The legacy floating-projection tests
    // retain their original rounding band; the exact-grid Tier3 oracle below does not.
    for (UINT i = 0; i < s.count; ++i) {
      double low = 0, high = 1;
      bool hit = true;
      for (UINT axis = 0; axis < 2; ++axis) {
        const double a = s.xy[i * 2 + axis], b = s.xy[((i + 1) % s.count) * 2 + axis];
        const double lo = axis ? lo_y : lo_x, hi = axis ? hi_y : hi_x;
        if (a == b) { if (a <= lo || a >= hi) hit = false; }
        else { low = std::max(low, std::min((lo - a) / (b - a), (hi - a) / (b - a)));
               high = std::min(high, std::max((lo - a) / (b - a), (hi - a) / (b - a))); }
      }
      if (hit && low < high) return true;
    }
    return false;
  }
  double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
  for (UINT i = 0; i < s.count; i++) {
    min_x = std::min(min_x, s.xy[2 * i]); max_x = std::max(max_x, s.xy[2 * i]);
    min_y = std::min(min_y, s.xy[2 * i + 1]); max_y = std::max(max_y, s.xy[2 * i + 1]);
  }
  if (!(max_x > lo_x && min_x < hi_x && max_y > lo_y && min_y < hi_y)) return false;
  const double orientation = s.area > 0 ? 1 : -1;
  for (UINT i = 0; i < s.count; i++) {
    const double ax = s.xy[2 * i], ay = s.xy[2 * i + 1];
    const double bx = s.xy[2 * ((i + 1) % s.count)], by = s.xy[2 * ((i + 1) % s.count) + 1];
    const double nx = (by - ay) * orientation, ny = (ax - bx) * orientation;
    // Clipping may create distinct homogeneous vertices that snap to the same
    // 16.8 point. A zero-length edge cannot separate the polygon from a square.
    if (nx == 0 && ny == 0) continue;
    // Corner of the square furthest against the outward normal.
    const double cx = nx > 0 ? lo_x : hi_x, cy = ny > 0 ? lo_y : hi_y;
    if (nx * (cx - ax) + ny * (cy - ay) >= 0) return false;
  }
  return true;
}
enum class Cull { None, Back, Front };
// Front faces are clockwise on the screen (FrontCounterClockwise = FALSE); y points down.
bool culled(const Snapped &s, Cull cull) {
  return !s.count || (cull == Cull::Back && s.area <= 0) || (cull == Cull::Front && s.area > 0);
}

/* ---- GPU harness ---- */
struct Shaders { D3D12_SHADER_BYTECODE vs{}, ps{}, gs{}, hs{}, ds{}, ms{}, as{}; };
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Kind, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind = Kind;
  T value{};
};
struct Draw {
  const Shaders *shaders = nullptr;
  std::vector<Triangle> triangles;
  Viewport viewport{0, 0, float(kSize), float(kSize), 0, 1};
  UINT samples = 1, forced = 0, sample_mask = UINT_MAX;
  bool conservative = true, depth = false, rtv = true, depth_clip = true;
  bool indirect = false, statistics = false, stream_output = false;
  bool indexed = false, bundle = false, replay = false, front_ccw = false, occlusion = false;
  bool stencil = false;
  D3D12_COMPARISON_FUNC depth_func = D3D12_COMPARISON_FUNC_ALWAYS;
  D3D12_COMPARISON_FUNC stencil_func = D3D12_COMPARISON_FUNC_ALWAYS;
  D3D12_STENCIL_OP stencil_pass = D3D12_STENCIL_OP_KEEP;
  UINT stencil_ref = 3;
  UINT views = 0, view_mask = 0;
  bool distance = false;
  Cull cull = Cull::None;
};
struct Result {
  std::vector<uint32_t> records;   // 4 dwords per pixel: invocations, coverage, z, 0
  std::vector<float> depth;        // 4 samples per pixel when Draw::depth
  std::vector<float> stencil;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
  std::vector<float> stream_output;
  UINT64 stream_bytes = 0;
  UINT64 occlusion = 0;
};

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> depth_reader, stencil_reader;
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
  HANDLE event = nullptr;
  UINT64 fence_value = 0;

  Context() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0};
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // StructuredBuffer<float4> vertices
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &range};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC root_desc{4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
    ComPtr<ID3DBlob> bytes, errors;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "root serialize");
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&root)),
          "root");
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&rtv_heap)), "RTV heap");
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&dsv_heap)), "DSV heap");
    h = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&srv_heap)), "SRV heap");
  }
  ~Context() { if (event) CloseHandle(event); }
  void execute(ID3D12GraphicsCommandList *list) {
    check(list->Close(), "close");
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++fence_value), "signal");
    check(fence->SetEventOnCompletion(fence_value, event), "fence event");
    require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU completion");
  }
  template <typename T> std::vector<T> read(ID3D12Resource *resource, size_t bytes) {
    std::vector<T> out(bytes / sizeof(T));
    void *mapped = nullptr;
    check(resource->Map(0, nullptr, &mapped), "map readback");
    std::memcpy(out.data(), mapped, bytes);
    resource->Unmap(0, nullptr);
    return out;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso(const Draw &d) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.Get();
    p.VS = d.shaders->vs; p.PS = d.shaders->ps; p.GS = d.shaders->gs; p.HS = d.shaders->hs; p.DS = d.shaders->ds;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = d.cull == Cull::Back ? D3D12_CULL_MODE_BACK :
                                 d.cull == Cull::Front ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    p.RasterizerState.FrontCounterClockwise = d.front_ccw;
    p.RasterizerState.DepthClipEnable = d.depth_clip;
    p.RasterizerState.ForcedSampleCount = d.forced;
    p.RasterizerState.ConservativeRaster = d.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                                          : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = d.sample_mask;
    p.SampleDesc.Count = d.samples;
    if (d.stream_output) {
      static const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "SV_Position", 0, 0, 4, 0},
                                                         {0, "TEXCOORD", 0, 0, 1, 0}};
      static const UINT stride = 20;
      p.StreamOutput = {entries, 2, &stride, 1, 0};
    }
    if (d.rtv) {
      p.NumRenderTargets = 1;
      p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (d.depth) {
      p.DSVFormat = d.stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
      p.DepthStencilState.DepthEnable = TRUE;
      p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      p.DepthStencilState.DepthFunc = d.depth_func;
      p.DepthStencilState.StencilEnable = d.stencil;
      p.DepthStencilState.StencilReadMask = p.DepthStencilState.StencilWriteMask = 0xff;
      p.DepthStencilState.FrontFace = p.DepthStencilState.BackFace =
          {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, d.stencil_pass, d.stencil_func};
    }
    p.PrimitiveTopologyType = d.shaders->hs.pShaderBytecode ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                                            : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    return p;
  }

  HRESULT createPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, const Shaders &shaders,
                         ID3D12PipelineState **pipeline, UINT views = 0) {
    if (!shaders.ms.pShaderBytecode && !views)
      return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline));
    struct Stream {
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE> gs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE> hs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE> ds;
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
    stream.root.value = root.Get(); stream.as.value = shaders.as; stream.ms.value = shaders.ms;
    stream.ps.value = desc.PS; stream.blend.value = desc.BlendState; stream.raster.value = desc.RasterizerState;
    stream.vs.value = desc.VS; stream.gs.value = desc.GS; stream.hs.value = desc.HS; stream.ds.value = desc.DS;
    const D3D12_VIEW_INSTANCE_LOCATION locations[2] = {{0, 0}, {0, 0}};
    stream.views.value = {views, locations, D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};
    stream.depth.value = desc.DepthStencilState; stream.topology.value = desc.PrimitiveTopologyType;
    stream.formats.value.NumRenderTargets = desc.NumRenderTargets;
    for (UINT i = 0; i < desc.NumRenderTargets; i++) stream.formats.value.RTFormats[i] = desc.RTVFormats[i];
    stream.depth_format.value = desc.DSVFormat; stream.samples.value = desc.SampleDesc; stream.mask.value = desc.SampleMask;
    ComPtr<ID3D12Device2> device2;
    const HRESULT hr = device.As(&device2);
    if (FAILED(hr)) return hr;
    const D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(stream), &stream};
    return device2->CreatePipelineState(&sd, IID_PPV_ARGS(pipeline));
  }

  Result run(const Draw &d) {
    auto desc = pso(d);
    ComPtr<ID3D12PipelineState> pipeline;
    check(createPipeline(desc, *d.shaders, &pipeline, d.views), "conservative PSO");
    const UINT64 vertex_bytes = std::max<size_t>(16, d.triangles.size() * sizeof(Triangle));
    auto vertices = buffer(device.Get(), vertex_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(vertices->Map(0, nullptr, &mapped), "map vertices");
    std::memcpy(mapped, d.triangles.data(), d.triangles.size() * sizeof(Triangle));
    vertices->Unmap(0, nullptr);
    auto records = buffer(device.Get(), kRecordBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto zero = buffer(device.Get(), kRecordBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    check(zero->Map(0, nullptr, &mapped), "map zero");
    std::memset(mapped, 0, kRecordBytes);
    zero->Unmap(0, nullptr);
    auto readback = buffer(device.Get(), kRecordBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target, depth, depth_out, depth_readback, stencil_readback;
    if (d.rtv) {
      D3D12_RESOURCE_DESC tex{};
      tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      tex.Width = tex.Height = kSize;
      tex.DepthOrArraySize = tex.MipLevels = 1;
      tex.SampleDesc.Count = d.samples;
      tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "target");
      device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }
    if (d.depth) {
      require(d.samples == 4, "depth cases run at 4x");
      D3D12_RESOURCE_DESC tex{};
      tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      tex.Width = tex.Height = kSize;
      tex.DepthOrArraySize = tex.MipLevels = 1;
      tex.SampleDesc.Count = d.samples;
      tex.Format = d.stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
      tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      D3D12_CLEAR_VALUE clear{};
      clear.Format = d.stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
      clear.DepthStencil.Depth = 0.5f;
      clear.DepthStencil.Stencil = 3;
      check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)), "depth");
      D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
      dsv.Format = clear.Format;
      dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      device->CreateDepthStencilView(depth.Get(), &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = d.stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      device->CreateShaderResourceView(depth.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
      if (d.stencil) {
        srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(depth.Get(), &srv, handle);
        stencil_readback = buffer(device.Get(), kDepthBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      }
      depth_out = buffer(device.Get(), kDepthBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      depth_readback = buffer(device.Get(), kDepthBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                    IID_PPV_ARGS(&list)), "list");
    list->CopyBufferRegion(records.Get(), 0, zero.Get(), 0, kRecordBytes);
    transition(list.Get(), records.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetGraphicsRootSignature(root.Get());
    list->SetGraphicsRootShaderResourceView(0, vertices->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(1, records->GetGPUVirtualAddress());
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(d.rtv ? 1 : 0, d.rtv ? &rtv : nullptr, FALSE, d.depth ? &dsv : nullptr);
    if (d.rtv) {
      const float clear[4] = {0, 0, 0, 0};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    }
    if (d.depth) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH |
        (d.stencil ? D3D12_CLEAR_FLAG_STENCIL : 0)), 0.5f, 3, 0, nullptr);
    list->OMSetStencilRef(d.stencil_ref);
    const D3D12_VIEWPORT viewport{d.viewport.x, d.viewport.y, d.viewport.width, d.viewport.height,
                                  d.viewport.min_depth, d.viewport.max_depth};
    const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    if (d.views) {
      ComPtr<ID3D12GraphicsCommandList1> view_list;
      check(list.As(&view_list), "view command list");
      view_list->SetViewInstanceMask(d.view_mask);
    }
    ComPtr<ID3D12Resource> stream, stream_counter, stream_readback, counter_readback;
    const UINT64 stream_size = std::max<UINT64>(20, d.triangles.size() * 3 * 20);
    if (d.stream_output) {
      stream = buffer(device.Get(), stream_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
      stream_counter = buffer(device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      stream_readback = buffer(device.Get(), stream_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      counter_readback = buffer(device.Get(), 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(stream_counter.Get(), 0, zero.Get(), 0, 8);
      transition(list.Get(), stream_counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      const D3D12_STREAM_OUTPUT_BUFFER_VIEW view{stream->GetGPUVirtualAddress(), stream_size,
                                                stream_counter->GetGPUVirtualAddress()};
      list->SOSetTargets(0, 1, &view);
    }
    ComPtr<ID3D12QueryHeap> query;
    ComPtr<ID3D12Resource> query_readback;
    if (d.statistics) {
      const D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0};
      check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&query)), "statistics heap");
      query_readback = buffer(device.Get(), sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                              D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      list->BeginQuery(query.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    }
    ComPtr<ID3D12QueryHeap> occlusion_query;
    ComPtr<ID3D12Resource> occlusion_readback;
    if (d.occlusion) {
      const D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
      check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&occlusion_query)), "occlusion heap");
      occlusion_readback = buffer(device.Get(), 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      list->BeginQuery(occlusion_query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    }
    ComPtr<ID3D12Resource> indices;
    D3D12_INDEX_BUFFER_VIEW index_view{};
    if (d.indexed) {
      indices = buffer(device.Get(), d.triangles.size() * 12, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      check(indices->Map(0, nullptr, &mapped), "index map");
      auto *data = static_cast<UINT *>(mapped);
      for (UINT i = 0; i < d.triangles.size(); ++i) {
        data[i * 3] = i * 3 + 2; data[i * 3 + 1] = i * 3; data[i * 3 + 2] = i * 3 + 1;
      }
      indices->Unmap(0, nullptr);
      index_view = {indices->GetGPUVirtualAddress(), UINT(d.triangles.size() * 12), DXGI_FORMAT_R32_UINT};
      list->IASetIndexBuffer(&index_view);
    }
    list->IASetPrimitiveTopology(d.shaders->hs.pShaderBytecode ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                               : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ComPtr<ID3D12CommandSignature> signature;
    ComPtr<ID3D12Resource> arguments;
    ComPtr<ID3D12CommandAllocator> bundle_allocator;
    ComPtr<ID3D12GraphicsCommandList> bundle;
    if (d.indirect) {
      D3D12_INDIRECT_ARGUMENT_DESC arg{};
      arg.Type = d.shaders->ms.pShaderBytecode ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH
               : d.indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
      const UINT stride = d.shaders->ms.pShaderBytecode ? sizeof(D3D12_DISPATCH_MESH_ARGUMENTS)
                         : d.indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
      const D3D12_COMMAND_SIGNATURE_DESC sig{stride, 1, &arg, 0};
      check(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&signature)), "indirect signature");
      arguments = buffer(device.Get(), stride, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      check(arguments->Map(0, nullptr, &mapped), "indirect map");
      if (d.shaders->ms.pShaderBytecode) {
        const D3D12_DISPATCH_MESH_ARGUMENTS dispatch{UINT(d.triangles.size()), 1, 1};
        std::memcpy(mapped, &dispatch, sizeof(dispatch));
      } else if (d.indexed) {
        const D3D12_DRAW_INDEXED_ARGUMENTS draw{UINT(d.triangles.size() * 3), 1, 0, 0, 0};
        std::memcpy(mapped, &draw, sizeof(draw));
      } else {
        const D3D12_DRAW_ARGUMENTS draw{UINT(d.triangles.size() * 3), 1, 0, 0};
        std::memcpy(mapped, &draw, sizeof(draw));
      }
      arguments->Unmap(0, nullptr);
      list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
    } else if (d.bundle) {
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)), "bundle allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pipeline.Get(),
                                     IID_PPV_ARGS(&bundle)), "bundle list");
      bundle->SetGraphicsRootSignature(root.Get());
      bundle->SetGraphicsRootShaderResourceView(0, vertices->GetGPUVirtualAddress());
      bundle->SetGraphicsRootUnorderedAccessView(1, records->GetGPUVirtualAddress());
      bundle->IASetPrimitiveTopology(d.shaders->hs.pShaderBytecode ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                                 : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      if (d.indexed) bundle->IASetIndexBuffer(&index_view);
      if (d.shaders->ms.pShaderBytecode) {
        ComPtr<ID3D12GraphicsCommandList6> mesh; check(bundle.As(&mesh), "bundle mesh list");
        mesh->DispatchMesh(UINT(d.triangles.size()), 1, 1);
      } else if (d.indexed) bundle->DrawIndexedInstanced(UINT(d.triangles.size() * 3), 1, 0, 0, 0);
      else bundle->DrawInstanced(UINT(d.triangles.size() * 3), 1, 0, 0);
      check(bundle->Close(), "bundle close");
      list->ExecuteBundle(bundle.Get());
    } else if (d.shaders->ms.pShaderBytecode) {
      ComPtr<ID3D12GraphicsCommandList6> mesh;
      check(list.As(&mesh), "mesh command list");
      mesh->DispatchMesh(UINT(d.triangles.size()), 1, 1);
    } else if (d.indexed) list->DrawIndexedInstanced(UINT(d.triangles.size() * 3), 1, 0, 0, 0);
    else list->DrawInstanced(UINT(d.triangles.size() * 3), 1, 0, 0);
    if (d.occlusion) {
      list->EndQuery(occlusion_query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      list->ResolveQueryData(occlusion_query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, occlusion_readback.Get(), 0);
    }
    if (d.statistics) {
      list->EndQuery(query.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, query_readback.Get(), 0);
    }
    if (d.stream_output) {
      transition(list.Get(), stream.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.Get(), stream_counter.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(stream_readback.Get(), 0, stream.Get(), 0, stream_size);
      list->CopyBufferRegion(counter_readback.Get(), 0, stream_counter.Get(), 0, 8);
    }
    transition(list.Get(), records.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, records.Get(), 0, kRecordBytes);
    if (d.depth) {
      transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      list->SetPipelineState(depth_reader.Get());
      list->SetComputeRootSignature(root.Get());
      ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootDescriptorTable(2, srv_heap->GetGPUDescriptorHandleForHeapStart());
      list->SetComputeRootUnorderedAccessView(3, depth_out->GetGPUVirtualAddress());
      list->Dispatch(kSize / 8, kSize / 8, 1);
      transition(list.Get(), depth_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(depth_readback.Get(), 0, depth_out.Get(), 0, kDepthBytes);
      if (d.stencil) {
        transition(list.Get(), depth_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetPipelineState(stencil_reader.Get());
        auto handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetComputeRootDescriptorTable(2, handle);
        list->Dispatch(kSize / 8, kSize / 8, 1);
        transition(list.Get(), depth_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(stencil_readback.Get(), 0, depth_out.Get(), 0, kDepthBytes);
      }
    }
    execute(list.Get());
    Result result;
    result.records = read<uint32_t>(readback.Get(), kRecordBytes);
    if (d.replay) {
      require(!d.depth && !d.stream_output, "replay case uses UAV-only readback");
      ComPtr<ID3D12CommandAllocator> reset_allocator;
      ComPtr<ID3D12GraphicsCommandList> reset_list;
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&reset_allocator)), "replay reset allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, reset_allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&reset_list)), "replay reset list");
      transition(reset_list.Get(), records.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      execute(reset_list.Get());
      ID3D12CommandList *lists[] = {list.Get()};
      queue->ExecuteCommandLists(1, lists);
      check(queue->Signal(fence.Get(), ++fence_value), "replay signal");
      check(fence->SetEventOnCompletion(fence_value, event), "replay fence");
      require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "replay completion");
      const auto replayed = read<uint32_t>(readback.Get(), kRecordBytes);
      for (UINT p = 0; p < kSize * kSize; ++p) {
        for (UINT lane = 0; lane < 3; ++lane) require(result.records[p * 4 + lane] == replayed[p * 4 + lane], "closed-list replay exact UAV records");
        require((result.records[p * 4 + 3] & 1) == (replayed[p * 4 + 3] & 1), "replay InnerCoverage Boolean");
      }
    }
    if (d.occlusion) result.occlusion = read<UINT64>(occlusion_readback.Get(), 8)[0];
    if (d.depth) result.depth = read<float>(depth_readback.Get(), kDepthBytes);
    if (d.stencil) result.stencil = read<float>(stencil_readback.Get(), kDepthBytes);
    if (d.statistics) result.statistics = read<D3D12_QUERY_DATA_PIPELINE_STATISTICS>(
        query_readback.Get(), sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS))[0];
    if (d.stream_output) {
      result.stream_output = read<float>(stream_readback.Get(), stream_size);
      result.stream_bytes = read<UINT64>(counter_readback.Get(), 8)[0];
    }
    return result;
  }
};

/* Random thin, sliver, sub-pixel, large and perspective triangles (deterministic). */
std::vector<Triangle> randomTriangles(UINT count, bool perspective) {
  uint32_t seed = 12345;
  auto next = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
  std::vector<Triangle> out;
  while (out.size() < count) {
    const UINT kind = UINT(out.size()) % 4;
    float sx[3], sy[3];
    const float cx = -2.0f + next() * 36.0f, cy = -2.0f + next() * 36.0f;
    if (kind == 0) {  // sliver: three almost collinear points
      const float angle = next() * 6.2831853f, length = 4.0f + next() * 20.0f;
      for (int i = 0; i < 3; i++) {
        const float t = (i - 1) * 0.5f * length, off = i == 1 ? (next() - 0.5f) * 0.3f : 0.0f;
        sx[i] = cx + std::cos(angle) * t - std::sin(angle) * off;
        sy[i] = cy + std::sin(angle) * t + std::cos(angle) * off;
      }
    } else if (kind == 1) {  // sub-pixel
      for (int i = 0; i < 3; i++) { sx[i] = cx + next() * 0.8f; sy[i] = cy + next() * 0.8f; }
    } else if (kind == 2) {  // thin
      for (int i = 0; i < 3; i++) { sx[i] = cx + (next() - 0.5f) * 1.5f; sy[i] = cy + (next() - 0.5f) * 18.0f; }
    } else {  // general
      for (int i = 0; i < 3; i++) { sx[i] = cx + (next() - 0.5f) * 16.0f; sy[i] = cy + (next() - 0.5f) * 16.0f; }
    }
    Triangle t;
    for (int i = 0; i < 3; i++) {
      const float w = perspective ? 0.5f + next() * 1.5f : 1.0f;
      const float nx = sx[i] / kSize * 2.0f - 1.0f, ny = 1.0f - sy[i] / kSize * 2.0f;
      t[i] = {nx * w, ny * w, (0.1f + next() * 0.8f) * w, w};
    }
    out.push_back(t);
  }
  return out;
}

Triangle ndcTriangle(float x0, float y0, float x1, float y1, float x2, float y2) {
  return Triangle{{{x0, y0, 0.5f, 1}, {x1, y1, 0.5f, 1}, {x2, y2, 0.5f, 1}}};
}

/* Compares invocations and coverage with the oracle. `full`: SV_Coverage of a covered pixel. */
struct Stats { UINT covered = 0, ambiguous = 0, invocations = 0; };
/* `isolate` redraws one triangle alone (failure diagnostics). */
template <typename Isolate, typename IsolateMany>
Stats compare(const Result &r, const Draw &d, uint32_t full, const char *name, Isolate &&isolate,
              IsolateMany &&isolate_many) {
  std::vector<Snapped> snapped;
  for (const auto &t : d.triangles) snapped.push_back(snap(t, d.viewport, d.distance));
  Stats stats;
  for (int py = 0; py < int(kSize); py++)
    for (int px = 0; px < int(kSize); px++) {
      // Pixels outside the viewport are never rasterized.
      const bool in_viewport = px + 0.5f >= d.viewport.x && px + 0.5f < d.viewport.x + d.viewport.width &&
                               py + 0.5f >= d.viewport.y && py + 0.5f < d.viewport.y + d.viewport.height;
      UINT strict = 0, loose = 0;
      for (const auto &s : snapped) {
        if (culled(s, d.cull) || !in_viewport) continue;
        strict += overlaps(s, px, py, -1.0 / 128);
        loose += overlaps(s, px, py, 1.0 / 128);
      }
      if (d.views) {
        const UINT enabled = UINT(bool(d.view_mask & 1)) + UINT(bool(d.view_mask & 2));
        strict *= enabled; loose *= enabled;
      }
      const uint32_t *record = &r.records[(py * kSize + px) * 4];
      const UINT got = record[0];
      if (got < strict || got > loose || (got && record[1] != full) || (!got && record[1])) {
        std::fprintf(stderr, "%s pixel=(%d,%d) invocations=%u coverage=0x%x expected=%u..%u full=0x%x\n", name,
                     px, py, got, record[1], strict, loose, full);
        for (size_t i = 0; i < snapped.size(); i++)
          if (overlaps(snapped[i], px, py, 1.0 / 128)) {
            const auto &t = d.triangles[i];
            std::fprintf(stderr, "  triangle %zu screen=(%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f) area=%.4f clip:", i,
                         snapped[i].xy[0], snapped[i].xy[1], snapped[i].xy[2], snapped[i].xy[3], snapped[i].xy[4],
                         snapped[i].xy[5], snapped[i].area);
            for (const auto &v : t) std::fprintf(stderr, " (%.9g,%.9g,%.9g,%.9g)", v[0], v[1], v[2], v[3]);
            const Result alone = isolate(t);
            std::fprintf(stderr, " alone=%u,0x%x\n", alone.records[(py * kSize + px) * 4],
                         alone.records[(py * kSize + px) * 4 + 1]);
          }
        // Diagnostics: the contributing triangles drawn together, then each prefix of the draw.
        std::vector<Triangle> together;
        for (size_t i = 0; i < snapped.size(); i++)
          if (overlaps(snapped[i], px, py, 1.0 / 128)) together.push_back(d.triangles[i]);
        const Result both = isolate_many(together);
        std::fprintf(stderr, "  together=%u\n", both.records[(py * kSize + px) * 4]);
        for (size_t i = 0; i < snapped.size(); ++i) {
          if (overlaps(snapped[i], px, py, 1.0 / 128)) continue;
          const Result alone = isolate(d.triangles[i]);
          const UINT count = alone.records[(py * kSize + px) * 4];
          if (count) {
            std::fprintf(stderr, "  unexpected triangle %zu alone=%u clip:", i, count);
            for (const auto &v : d.triangles[i]) std::fprintf(stderr, " (%.9g,%.9g,%.9g,%.9g)", v[0], v[1], v[2], v[3]);
            std::fprintf(stderr, "\n");
          }
        }

        fail(name);
      }
      stats.covered += strict > 0;
      stats.ambiguous += strict != loose;
      stats.invocations += got;
    }
  return stats;
}
Stats compare(Context &c, const Result &r, const Draw &d, uint32_t full, const char *name) {
  return compare(r, d, full, name, [&](const Triangle &t) {
    Draw alone = d;
    alone.triangles = {t};
    return c.run(alone);
  }, [&](const std::vector<Triangle> &t) {
    Draw some = d;
    some.triangles = t;
    return c.run(some);
  });
}

// Independent public oracle. Inputs are exact dyadic screen coordinates (1/4096 pixel);
// GPU projection is their exact power-of-two inverse. No translator header or helper is used.
// Segment/box clipping compares integer rational bounds; containment uses signed integer
// orientations. Public minimum/maximum uncertainty radii are 8/16 units (1/512, 1/256).
struct GridPoint { int64_t x, y; bool operator==(const GridPoint &) const = default; };
using GridTriangle = std::array<GridPoint, 3>;
struct GridBox { int64_t x0, y0, x1, y1; };
int64_t orient(GridPoint a, GridPoint b, GridPoint p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
bool segmentBox(GridPoint a, GridPoint b, GridBox box, bool closed) {
  int64_t ln = 0, ld = 1, hn = 1, hd = 1;
  for (UINT axis = 0; axis < 2; ++axis) {
    const int64_t p = axis ? a.y : a.x, q = axis ? b.y : b.x;
    const int64_t lo = axis ? box.y0 : box.x0, hi = axis ? box.y1 : box.x1;
    if (p == q) {
      if (closed ? (p < lo || p > hi) : (p <= lo || p >= hi)) return false;
      continue;
    }
    const int64_t den = std::abs(q - p);
    const int64_t low = q > p ? lo - p : p - hi, high = q > p ? hi - p : p - lo;
    if (low * ld > ln * den) { ln = low; ld = den; }
    if (high * hd < hn * den) { hn = high; hd = den; }
  }
  return closed ? ln * hd <= hn * ld : ln * hd < hn * ld;
}
bool contains(const GridTriangle &t, GridBox b, bool closed) {
  const int64_t area = orient(t[0], t[1], t[2]);
  if (!area) return false;
  for (GridPoint p : {GridPoint{b.x0, b.y0}, GridPoint{b.x1, b.y0},
                      GridPoint{b.x1, b.y1}, GridPoint{b.x0, b.y1}})
    for (UINT i = 0; i < 3; ++i) {
      const int64_t cross = orient(t[i], t[(i + 1) % 3], p) * (area > 0 ? 1 : -1);
      if (closed ? cross < 0 : cross <= 0) return false;
    }
  return true;
}
bool intersects(const GridTriangle &t, GridBox b, bool closed) {
  for (UINT i = 0; i < 3; ++i) if (segmentBox(t[i], t[(i + 1) % 3], b, closed)) return true;
  // If no triangle edge crosses the box, intersection can only be a box inside the triangle.
  return contains(t, b, closed);
}
GridTriangle quantize(GridTriangle t) {
  for (auto &p : t) {
    // Test inputs are never halfway between adjacent fixed-grid points.
    require(std::abs(p.x) % 16 != 8 && std::abs(p.y) % 16 != 8, "oracle no rounding ties");
    p.x = p.x < 0 ? -((-p.x + 8) / 16) * 16 : ((p.x + 8) / 16) * 16;
    p.y = p.y < 0 ? -((-p.y + 8) / 16) * 16 : ((p.y + 8) / 16) * 16;
  }
  return t;
}
Triangle clipTriangle(const GridTriangle &t, bool varying_depth = false) {
  Triangle clip;
  for (UINT i = 0; i < 3; ++i)
    clip[i] = {float(t[i].x) / 65536 - 1, 1 - float(t[i].y) / 65536,
               varying_depth ? 0.25f * (i + 1) : 0.25f, 1};
  return clip;
}
struct GridStats { UINT invocations = 0, mandatory = 0, interior = 0, edge = 0, uncertainty = 0; };
GridStats compareGrid(const Result &r, const GridTriangle &original, bool inner, const char *name,
                      bool degenerate_attributes = false, float first_value = 0, float first_depth = 0.25f) {
  const auto snapped = quantize(original);
  const bool degenerate = orient(snapped[0], snapped[1], snapped[2]) == 0;
  GridStats stats;
  for (UINT y = 0; y < kSize; ++y) for (UINT x = 0; x < kSize; ++x) {
    const GridBox pixel{int64_t(x) * 4096, int64_t(y) * 4096, int64_t(x + 1) * 4096, int64_t(y + 1) * 4096};
    const GridBox min_outer{pixel.x0 - 8, pixel.y0 - 8, pixel.x1 + 8, pixel.y1 + 8};
    const GridBox max_outer{pixel.x0 - 16, pixel.y0 - 16, pixel.x1 + 16, pixel.y1 + 16};
    // Degenerate boundary-touching points/lines may choose neighboring pixels;
    // require interior crossings and at least one invocation, not a specific neighbor.
    const bool mandatory = degenerate ? intersects(snapped, pixel, false) :
                           intersects(snapped, min_outer, false) || intersects(original, pixel, false);
    const bool allowed = intersects(snapped, max_outer, true);
    const uint32_t *record = &r.records[(y * kSize + x) * 4];
    const UINT got = record[0];
    if (got > 1 || (mandatory && !got) || (got && !allowed)) {
      std::fprintf(stderr, "%s pixel=(%u,%u) got=%u mandatory=%u allowed=%u\n", name, x, y, got, mandatory, allowed);
      fail("Tier3 exact outer uncertainty envelope");
    }
    stats.invocations += got; stats.mandatory += mandatory;
    if (!got) { require(record[3] == 0, "no UAV write outside rasterized pixels"); continue; }
    float depth; std::memcpy(&depth, record + 2, 4);
    require(std::isfinite(depth), "Tier3 finite SV_Position.z");
    if (inner) {
      const GridBox min_inner{pixel.x0 - 8, pixel.y0 - 8, pixel.x1 + 8, pixel.y1 + 8};
      const GridBox max_inner{pixel.x0 - 16, pixel.y0 - 16, pixel.x1 + 16, pixel.y1 + 16};
      const bool must_inner = contains(snapped, max_inner, false) && contains(original, pixel, true);
      const bool may_inner = contains(snapped, min_inner, true) && contains(original, pixel, true);
      const bool value = record[3] & 1;
      if ((!value && record[3] != 0) || (must_inner && !value) || (value && !may_inner)) {
        std::fprintf(stderr, "%s inner pixel=(%u,%u) raw=%u required=%u allowed=%u\n", name, x, y, record[3], must_inner, may_inner);
        fail("Tier3 no false positives or missing mandatory interior");
      }
      stats.interior += must_inner; stats.edge += !may_inner; stats.uncertainty += may_inner != must_inner;
      if (degenerate && degenerate_attributes) {
        float attribute; std::memcpy(&attribute, record + 1, 4);
        require(attribute == first_value, "post-snap degenerate back-facing Vertex0 attribute");
        require(depth == first_depth, "post-snap degenerate Vertex0 depth");
      }
    }
  }
  require(stats.invocations > 0, "Tier3 primitive must not be discarded");
  return stats;
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12 module");
  using BackendId = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backend && backend() && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned backend identity");
  std::printf("%s_BACKEND id=%s\n", kMarker, backend());
  Context c;

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "options");
  std::printf("%s_CAPS tier=%u\n", kMarker, unsigned(options.ConservativeRasterizationTier));

  // Shaders: DXIL frozen by build-conservative-raster.sh, DXBC compiled from the same source.
  std::vector<uint8_t> dxil[8];
  std::vector<uint8_t> distance_dxil[8];
  const char *entries[8] = {"vs_main", "ps_main", "gs_main", "hs_main", "ds_main", "ms_main", "ms_as", "as_main"};
  for (int i = 0; i < 8; i++) dxil[i] = file((std::string("conservative-raster-") + entries[i] + ".dxil").c_str());
  for (int i = 0; i < 8; i++) distance_dxil[i] = file((std::string("conservative-distance-") + entries[i] + ".dxil").c_str());
  std::vector<uint8_t> inner_dxil[4], inner_dxbc[4];
  const char *inner_entries[4] = {"ps_inner", "ps_inner_early", "ps_inner_late", "ps_helper"};
  for (UINT i = 0; i < 4; ++i) {
    inner_dxil[i] = file((std::string("conservative-raster-") + inner_entries[i] + ".dxil").c_str());
    inner_dxbc[i] = file((std::string("conservative-raster-") + inner_entries[i] + ".dxbc").c_str());
  }
  const auto coverage_dxbc = file("conservative-raster-ps_main.dxbc");
  const auto source = file("conservative-raster.hlsl");
  const auto distance_source = file("conservative-distance.hlsl");
  const D3D_SHADER_MACRO legacy_defines[] = {{"CONSERVATIVE_LEGACY", "1"}, {nullptr, nullptr}};
  // vkd3d's D3DCompile has no SV_Coverage input: the DXBC paths pair DXBC geometry stages with the DXIL PS.
  const char *profiles[6] = {"vs_5_0", nullptr, "gs_5_0", "hs_5_0", "ds_5_0", "cs_5_0"};
  const char *dxbc_entries[6] = {"vs_main", nullptr, "gs_main", "hs_main", "ds_main", "cs_depth"};
  ComPtr<ID3DBlob> dxbc[6];
  ComPtr<ID3DBlob> distance_dxbc[5];
  for (int i = 0; i < 6; i++) {
    if (!profiles[i]) continue;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "conservative-raster.hlsl", legacy_defines, nullptr,
                                  dxbc_entries[i], profiles[i], D3DCOMPILE_ENABLE_STRICTNESS, 0, &dxbc[i], &errors);
    if (FAILED(hr)) {
      if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
      std::printf("%s_COMPILE_ERROR entry=%s\n", kMarker, dxbc_entries[i]);
      fail("D3DCompile", hr);
    }
    if (i < 5) {
      const HRESULT distance_hr = D3DCompile(distance_source.data(), distance_source.size(),
          "conservative-distance.hlsl", nullptr, nullptr, dxbc_entries[i], profiles[i],
          D3DCOMPILE_ENABLE_STRICTNESS, 0, &distance_dxbc[i], &errors);
      if (FAILED(distance_hr)) {
        if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        fail("D3DCompile distance", distance_hr);
      }
    }
  }
  auto blob = [](const std::vector<uint8_t> &b) { return D3D12_SHADER_BYTECODE{b.data(), b.size()}; };
  auto code = [](const ComPtr<ID3DBlob> &b) {
    return D3D12_SHADER_BYTECODE{b->GetBufferPointer(), b->GetBufferSize()};
  };
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
  compute.pRootSignature = c.root.Get();
  compute.CS = code(dxbc[5]);
  check(c.device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&c.depth_reader)), "depth reader");
  const auto stencil_code = file("conservative-raster-cs_stencil.dxil");
  compute.CS = blob(stencil_code);
  check(c.device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&c.stencil_reader)), "stencil reader");

  struct Path { const char *name; Shaders shaders; };
  Path paths[] = {
      {"vs_dxil", {blob(dxil[0]), blob(dxil[1]), {}, {}, {}}},
      {"vs_dxbc", {code(dxbc[0]), blob(dxil[1]), {}, {}, {}}},
      {"vs_gs_dxil", {blob(dxil[0]), blob(dxil[1]), blob(dxil[2]), {}, {}}},
      {"vs_gs_dxbc", {code(dxbc[0]), blob(dxil[1]), code(dxbc[2]), {}, {}}},
      {"vs_hs_ds_dxil", {blob(dxil[0]), blob(dxil[1]), {}, blob(dxil[3]), blob(dxil[4])}},
      {"vs_hs_ds_dxbc", {code(dxbc[0]), blob(dxil[1]), {}, code(dxbc[3]), code(dxbc[4])}},
      {"vs_hs_ds_gs_dxil", {blob(dxil[0]), blob(dxil[1]), blob(dxil[2]), blob(dxil[3]), blob(dxil[4])}},
      {"vs_hs_ds_gs_dxbc", {code(dxbc[0]), blob(dxil[1]), code(dxbc[2]), code(dxbc[3]), code(dxbc[4])}},
      {"ms_dxil", {{}, blob(dxil[1]), {}, {}, {}, blob(dxil[5]), {}}},
      {"as_ms_dxil", {{}, blob(dxil[1]), {}, {}, {}, blob(dxil[6]), blob(dxil[7])}},
  };
  const Shaders &vs = paths[0].shaders;
  const Shaders distance_paths[] = {
      {blob(distance_dxil[0]), blob(distance_dxil[1]), {}, {}, {}},
      {code(distance_dxbc[0]), blob(distance_dxil[1]), {}, {}, {}},
      {blob(distance_dxil[0]), blob(distance_dxil[1]), blob(distance_dxil[2]), {}, {}},
      {code(distance_dxbc[0]), blob(distance_dxil[1]), code(distance_dxbc[2]), {}, {}},
      {blob(distance_dxil[0]), blob(distance_dxil[1]), {}, blob(distance_dxil[3]), blob(distance_dxil[4])},
      {code(distance_dxbc[0]), blob(distance_dxil[1]), {}, code(distance_dxbc[3]), code(distance_dxbc[4])},
      {blob(distance_dxil[0]), blob(distance_dxil[1]), blob(distance_dxil[2]), blob(distance_dxil[3]), blob(distance_dxil[4])},
      {code(distance_dxbc[0]), blob(distance_dxil[1]), code(distance_dxbc[2]), code(distance_dxbc[3]), code(distance_dxbc[4])},
      {{}, blob(distance_dxil[1]), {}, {}, {}, blob(distance_dxil[5]), {}},
      {{}, blob(distance_dxil[1]), {}, {}, {}, blob(distance_dxil[6]), blob(distance_dxil[7])},
  };
  // A synthetic IA capture must not overwrite bounds parameters when the draw
  // contains enough vertices to reach their position-offset word.
  for (const auto *path : {&paths[0], &paths[1]}) {
    Draw d; d.shaders = &path->shaders; d.samples = 4; d.conservative = false;
    const Triangle visible = ndcTriangle(-0.8f, -0.8f, -0.8f, 0.8f, 0.8f, -0.8f);
    d.triangles = {visible};
    const Result alone = c.run(d);
    d.triangles.assign(5, ndcTriangle(0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f));
    d.triangles.push_back(visible);
    const Result padded = c.run(d);
    for (UINT pixel = 0; pixel < kSize * kSize; ++pixel)
      require(alone.records[pixel * 4] == padded.records[pixel * 4] &&
              alone.records[pixel * 4 + 1] == padded.records[pixel * 4 + 1],
              "MSAA coverage independent of degenerate IA prefix");
  }
  std::printf("%s_CASE_PASS ia_capture_parameter_integrity\n", kMarker);

  // PSO validation: wireframe, non-triangle topologies and undefined modes are invalid.
  {
    Draw d; d.shaders = &vs;
    auto desc = c.pso(d);
    ComPtr<ID3D12PipelineState> pso;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    require(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)) == E_INVALIDARG, "wireframe rejected");
    desc = c.pso(d);
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    require(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)) == E_INVALIDARG, "lines rejected");
    desc = c.pso(d);
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE(2);
    require(c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)) == E_INVALIDARG, "mode 2 rejected");
    std::printf("%s_CASE_PASS pso_validation\n", kMarker);
  }

  // Specification bring-up: 2x2 viewport, (-1,0), (1,0), (0,0.5): the two top pixels conservatively, none when off.
  {
    Draw d; d.shaders = &vs;
    d.viewport = {0, 0, 2, 2, 0, 1};
    d.triangles = {ndcTriangle(-1, 0, 1, 0, 0, 0.5f)};
    d.conservative = false;
    const auto off = c.run(d);
    for (UINT i = 0; i < kSize * kSize; i++) require(off.records[i * 4] == 0, "bring-up off draws nothing");
    d.conservative = true;
    const auto on = c.run(d);
    const auto stats = compare(c, on, d, 1, "bring-up");
    require(stats.invocations >= 2 && stats.invocations <= 4 && on.records[0] == 1 && on.records[4] == 1, "bring-up mandatory top pixels and legal boundary neighbors");
    std::printf("%s_CASE bring-up off=0 on=%u\n", kMarker, stats.invocations);
    std::printf("%s_CASE_PASS spec_bring_up\n", kMarker);
  }
  // Specification 1/2 pixel: 4x4 viewport, only the four center pixels.
  {
    Draw d; d.shaders = &vs;
    d.viewport = {0, 0, 4, 4, 0, 1};
    const float e = 0.01f;
    d.triangles = {ndcTriangle(-0.25f + e, 0.25f - e, -0.25f + e, -0.25f + e, 0.25f - e, 0.25f - e)};
    const auto r = c.run(d);
    compare(c, r, d, 1, "half-pixel");
    // Three center pixels have positive-area intersection. Pixel (2,2) touches
    // the diagonal at (2,2); uncertainty may include it. No ring pixel is legal.
    for (int py = 0; py < 4; py++)
      for (int px = 0; px < 4; px++) {
        const bool center = (px == 1 || px == 2) && (py == 1 || py == 2);
        const UINT got = r.records[(py * kSize + px) * 4];
        if (!(px == 2 && py == 2) && got != (center ? 1u : 0u)) {
          std::fprintf(stderr, "half-pixel pixel=(%d,%d) got=%u coverage=0x%x expected=%u center_mask=0x%x\n",
                       px, py, got, r.records[(py * kSize + px) * 4 + 1], center ? 1u : 0u,
                       (r.records[(1 * kSize + 1) * 4] ? 1u : 0u) |
                       (r.records[(1 * kSize + 2) * 4] ? 2u : 0u) |
                       (r.records[(2 * kSize + 1) * 4] ? 4u : 0u) |
                       (r.records[(2 * kSize + 2) * 4] ? 8u : 0u));
          fail("half-pixel mandatory center and no ring");
        }
      }
    std::printf("%s_CASE_PASS spec_half_pixel\n", kMarker);
  }
  // Specification TIR: ForcedSampleCount 4 without RTVs, SV_Coverage 0xF on the bring-up pixels.
  {
    Draw d; d.shaders = &vs;
    d.viewport = {0, 0, 2, 2, 0, 1};
    d.triangles = {ndcTriangle(-1, 0, 1, 0, 0, 0.5f)};
    d.forced = 4; d.rtv = false; d.sample_mask = 0x5;  // SampleMask does not apply to UAV-only rendering
    const auto r = c.run(d);
    const auto stats = compare(c, r, d, 0xF, "tir-forced-4");
    require(stats.invocations >= 2 && stats.invocations <= 4 && r.records[0] == 1 && r.records[4] == 1, "TIR mandatory top pixels and legal boundary neighbors");
    std::printf("%s_CASE_PASS spec_tir_forced4_coverage_0xf\n", kMarker);
  }
  // Specification 4x MSAA: SV_Coverage 0xF, then SampleMask 0x5 restricts it to 0x5.
  {
    Draw d; d.shaders = &vs;
    d.viewport = {0, 0, 2, 2, 0, 1};
    d.triangles = {ndcTriangle(-1, 0, 1, 0, 0, 0.5f)};
    d.samples = 4;
    compare(c, c.run(d), d, 0xF, "msaa4");
    d.sample_mask = 0x5;
    compare(c, c.run(d), d, 0x5, "msaa4-samplemask");
    std::printf("%s_CASE_PASS spec_msaa4_coverage_0xf_samplemask_0x5\n", kMarker);
  }
  // Random thin/sliver/sub-pixel/perspective triangles on every path, 1x and 8x; back-face culling on VS.
  const auto triangles = randomTriangles(64, true);
  UINT path_passes = 0, path_rejects = 0;
  for (const auto &path : paths) {
    {
      // A path the implementation cannot rasterize exactly is rejected at PSO creation; that blocks the tier.
      Draw probe; probe.shaders = &path.shaders;
      auto desc = c.pso(probe);
      ComPtr<ID3D12PipelineState> pso;
      const HRESULT hr = c.createPipeline(desc, path.shaders, &pso);
      if (hr == E_NOTIMPL) {
        std::printf("%s_PATH_REJECTED path=%s\n", kMarker, path.name);
        ++path_rejects;
        continue;
      }
      check(hr, "path PSO");
    }
    for (UINT samples : {1u, 8u}) {
      Draw d; d.shaders = &path.shaders;
      d.triangles = triangles;
      d.samples = samples;
      const std::string name = std::string(path.name) + (samples == 1 ? "-1x" : "-8x");
      const auto stats = compare(c, c.run(d), d, samples == 1 ? 1u : 0xFFu, name.c_str());
      require(stats.covered > 100, "oracle covers pixels");
      std::printf("%s_ORACLE path=%s samples=%u triangles=%zu covered=%u ambiguous=%u invocations=%u\n", kMarker,
                  path.name, samples, triangles.size(), stats.covered, stats.ambiguous, stats.invocations);
    }
    std::printf("%s_CASE_PASS oracle_%s\n", kMarker, path.name);
    for (UINT samples : {1u, 8u}) {
      Draw d; d.shaders = &distance_paths[&path - paths]; d.triangles = triangles;
      d.samples = samples; d.distance = true;
      const std::string name = std::string(path.name) + "-distances-" + std::to_string(samples);
      const auto stats = compare(c, c.run(d), d, samples == 1 ? 1u : 0xFFu, name.c_str());
      std::printf("%s_DISTANCE path=%s samples=%u covered=%u invocations=%u\n",
                  kMarker, path.name, samples, stats.covered, stats.invocations);
    }
    {
      Draw d; d.shaders = &distance_paths[&path - paths]; d.distance = true; d.samples = 8;
      d.indirect = true; d.views = 2; d.view_mask = 2;
      d.triangles = {{{{-0.9f, -0.7f, 0.5f, 1}, {1.4f, -0.7f, 1, 2}, {-0.1f, 1.4f, 1, 2}}}};
      const Result r = c.run(d);
      const std::string name = std::string(path.name) + "-distance-perspective-indirect-view";
      const auto stats = compare(c, r, d, 0xFFu, name.c_str());
      std::array<double, 3> x{}, y{};
      for (UINT v = 0; v < 3; ++v) {
        const auto &p = d.triangles[0][v];
        x[v] = std::rint(double(float((p[0] / p[3] + 1) * kSize * 0.5f)) * 256.0) / 256.0;
        y[v] = std::rint(double(float((1 - p[1] / p[3]) * kSize * 0.5f)) * 256.0) / 256.0;
      }
      const double determinant = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
      for (UINT py = 0; py < kSize; ++py) for (UINT px = 0; px < kSize; ++px) {
        const auto *record = &r.records[(py * kSize + px) * 4];
        if (!record[0]) continue;
        const double a = ((y[1] - y[2]) * (px + 0.5 - x[2]) + (x[2] - x[1]) * (py + 0.5 - y[2])) / determinant;
        const double b = ((y[2] - y[0]) * (px + 0.5 - x[2]) + (x[0] - x[2]) * (py + 0.5 - y[2])) / determinant;
        const std::array<double, 3> barycentric{a, b, 1 - a - b};
        double numerator = 0, reciprocal_w = 0;
        for (UINT v = 0; v < 3; ++v) {
          const auto &p = d.triangles[0][v];
          numerator += barycentric[v] * (p[0] + 0.1f * p[3]) / p[3];
          reciprocal_w += barycentric[v] / p[3];
        }
        float actual; std::memcpy(&actual, record + 3, sizeof(actual));
        require(std::isfinite(actual) && std::fabs(actual - numerator / reciprocal_w) < 5e-4,
                "perspective SV_ClipDistance pixel input");
      }
      require(r.records[(16 * kSize + 16) * 4] == 1, "perspective distance interior");
      std::printf("%s_DISTANCE_INTERPOLATION path=%s samples=8 indirect=1 mask=2 invocations=%u\n",
                  kMarker, path.name, stats.invocations);
    }
    for (UINT mask : {0u, 2u, 3u}) {
      Draw d; d.shaders = &path.shaders; d.triangles = triangles; d.samples = 8; d.views = 2; d.view_mask = mask;
      const std::string name = std::string(path.name) + "-view-mask-" + std::to_string(mask);
      const auto stats = compare(c, c.run(d), d, 0xFF, name.c_str());
      std::printf("%s_VIEW path=%s mask=%u samples=8 invocations=%u\n", kMarker, path.name, mask, stats.invocations);
    }
    for (bool indirect : {false, true}) {
      Draw d; d.shaders = &path.shaders; d.triangles = triangles;
      d.samples = 8; d.indirect = indirect; d.statistics = true;
      d.stream_output = !path.shaders.ms.pShaderBytecode;
      const std::string name = std::string(path.name) + (indirect ? "-indirect-stats" : "-direct-stats");
      const auto result = c.run(d);
      const auto stats = compare(c, result, d, 0xFF, name.c_str());
      const auto &q = result.statistics;
      if (d.stream_output) {
        require(result.stream_bytes == triangles.size() * 3 * 20, "SO captures application vertices once");
        std::vector<bool> seen(triangles.size() * 3);
        for (size_t vertex = 0; vertex < seen.size(); ++vertex) {
          const float *record = result.stream_output.data() + vertex * 5;
          const auto id = UINT(record[4]);
          require(record[4] == float(id) && id < seen.size() && !seen[id], "SO vertex identity");
          seen[id] = true;
          for (unsigned lane = 0; lane < 4; ++lane)
            require(std::fabs(record[lane] - triangles[id / 3][id % 3][lane]) <= 1e-5f,
                    "SO preserves application clip-space output");
        }
      }
      if (!path.shaders.ms.pShaderBytecode) {
        require(q.IAVertices == triangles.size() * 3 && q.IAPrimitives == triangles.size() &&
                q.VSInvocations == triangles.size() * 3, "statistics IA/VS logical input");
        require(q.GSInvocations == (path.shaders.gs.pShaderBytecode ? triangles.size() : 0),
                "statistics GS input primitives");
        require(q.HSInvocations == (path.shaders.hs.pShaderBytecode ? triangles.size() : 0) &&
                q.DSInvocations == (path.shaders.ds.pShaderBytecode ? triangles.size() * 3 : 0),
                "statistics HS/DS tessellated outputs");
      }
      require(q.PSInvocations >= stats.invocations, "statistics includes every visible pixel invocation");
      std::printf("%s_COMBINATION path=%s indirect=%u samples=8 ia_vertices=%llu ia_primitives=%llu vs=%llu "
                  "gs=%llu hs=%llu ds=%llu ps=%llu visible=%u\n", kMarker, path.name, indirect,
                  q.IAVertices, q.IAPrimitives, q.VSInvocations, q.GSInvocations, q.HSInvocations, q.DSInvocations,
                  q.PSInvocations, stats.invocations);
    }
    ++path_passes;
  }
  {
    Draw d; d.shaders = &vs;
    d.triangles = triangles;
    d.cull = Cull::Back;
    const auto stats = compare(c, c.run(d), d, 1, "cull-back");
    std::printf("%s_ORACLE path=vs_dxil_cull_back samples=1 covered=%u ambiguous=%u invocations=%u\n", kMarker,
                stats.covered, stats.ambiguous, stats.invocations);
    std::printf("%s_CASE_PASS cull_back\n", kMarker);
  }
  // Depth: the plane extrapolated to every sample of every covered pixel, clamped to [0.25, 0.75].
  {
    Draw d; d.shaders = &vs;
    d.viewport = {0, 0, float(kSize), float(kSize), 0.25f, 0.75f};
    const float sx[3] = {4.3f, 27.6f, 9.1f}, sy[3] = {4.7f, 6.2f, 26.4f}, z[3] = {0.0f, 1.0f, 0.5f};
    Triangle t;
    for (int i = 0; i < 3; i++) t[i] = {sx[i] / kSize * 2 - 1, 1 - sy[i] / kSize * 2, z[i], 1};
    d.triangles = {t};
    d.samples = 4; d.depth = true;
    const auto r = c.run(d);
    const auto stats = compare(c, r, d, 0xF, "depth");
    // Plane through the snapped vertices in window space, depth = min + z * (max - min).
    const auto s = snap(t, d.viewport);
    const double ax = s.xy[2] - s.xy[0], ay = s.xy[3] - s.xy[1], bx = s.xy[4] - s.xy[0], by = s.xy[5] - s.xy[1];
    const double det = ax * by - ay * bx;
    auto plane = [&](double x, double y) {
      const double u = ((x - s.xy[0]) * by - (y - s.xy[1]) * bx) / det;
      const double v = (ax * (y - s.xy[1]) - ay * (x - s.xy[0])) / det;
      const double ndc = z[0] + u * (z[1] - z[0]) + v * (z[2] - z[0]);
      return std::clamp(0.25 + ndc * 0.5, 0.25, 0.75);
    };
    const double offsets[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
    UINT clamped = 0, checked = 0;
    double worst = 0;
    for (int py = 0; py < int(kSize); py++)
      for (int px = 0; px < int(kSize); px++) {
        const bool covered = r.records[(py * kSize + px) * 4] != 0;
        for (int sample = 0; sample < 4; sample++) {
          const float got = r.depth[(py * kSize + px) * 4 + sample];
          const double want = covered ? plane(px + 0.5 + offsets[sample][0] / 16, py + 0.5 + offsets[sample][1] / 16)
                                      : 0.5;
          worst = std::max(worst, std::fabs(got - want));
          if (std::fabs(got - want) > 2e-5) {
            std::fprintf(stderr, "depth pixel=(%d,%d) sample=%d got=%.7f want=%.7f covered=%d\n", px, py, sample,
                         got, want, covered);
            fail("depth extrapolation");
          }
          if (covered) {
            ++checked;
            clamped += want == 0.25 || want == 0.75;
          }
        }
        if (covered) {
          float position_z;
          std::memcpy(&position_z, &r.records[(py * kSize + px) * 4 + 2], 4);
          const double want = plane(px + 0.5, py + 0.5);
          if (std::fabs(position_z - want) > 2e-5) {
            std::fprintf(stderr, "SV_Position.z pixel=(%d,%d) got=%.7f want=%.7f\n", px, py, position_z, want);
            fail("SV_Position.z extrapolation");
          }
        }
      }
    require(clamped > 0 && checked > clamped, "depth exercises extrapolation and clamping");
    std::printf("%s_DEPTH samples=4 checked=%u clamped=%u covered=%u max_error=%.3g\n", kMarker, checked, clamped,
                stats.covered, worst);
    std::printf("%s_CASE_PASS depth_extrapolated_per_sample_clamped\n", kMarker);
  }
  // Tier3 proof is independent of the advertised tier until every GPU case has run.
  const GridTriangle interior{{{4 * 4096, 4 * 4096}, {28 * 4096, 4 * 4096}, {4 * 4096, 28 * 4096}}};
  const GridTriangle outer_gap{{{10 * 4096 + 32, 4 * 4096}, {28 * 4096, 4 * 4096}, {10 * 4096 + 32, 28 * 4096}}};
  const GridTriangle inner_boundary{{{10 * 4096 - 16, 4 * 4096}, {28 * 4096, 4 * 4096}, {10 * 4096 - 16, 28 * 4096}}};
  const GridTriangle point{{{10 * 4096 + 2048, 10 * 4096 + 2048}, {10 * 4096 + 2049, 10 * 4096 + 2048}, {10 * 4096 + 2048, 10 * 4096 + 2049}}};
  const GridTriangle line{{{5 * 4096 + 2048, 12 * 4096 + 2048}, {25 * 4096 + 2048, 12 * 4096 + 2048}, {15 * 4096 + 2048, 12 * 4096 + 2049}}};
  const GridTriangle corner{{{16 * 4096, 16 * 4096}, {16 * 4096 + 1, 16 * 4096}, {16 * 4096, 16 * 4096 + 1}}};
  struct GridCase { const char *name; GridTriangle triangle; UINT samples; bool indexed, indirect, bundle, replay; };
  const GridCase grid_cases[] = {
      {"interior_direct", interior, 1, false, false, false, false},
      {"outer_gap_indirect", outer_gap, 4, false, true, false, false},
      {"inner_boundary_indexed_mesh", inner_boundary, 4, true, false, false, false},
      {"point_direct", point, 1, false, false, false, false},
      {"line_indirect_indexed_mesh", line, 4, true, true, false, false},
      {"corner_bundle", corner, 1, false, false, true, false},
      {"interior_replay", interior, 4, false, false, false, true},
  };
  for (const auto &path : paths) {
    const bool is_dxbc = std::strstr(path.name, "dxbc") != nullptr;
    Shaders shader = path.shaders; shader.ps = blob(is_dxbc ? inner_dxbc[0] : inner_dxil[0]);
    for (const auto &test : grid_cases) {
      Draw d; d.shaders = &shader; d.samples = test.samples;
      const bool degenerate = orient(quantize(test.triangle)[0], quantize(test.triangle)[1], quantize(test.triangle)[2]) == 0;
      require(orient(test.triangle[0], test.triangle[1], test.triangle[2]) != 0, "no true pre-snap zero-area in Tier3 proof");
      d.triangles = {clipTriangle(test.triangle, degenerate)};
      d.indexed = test.indexed && !shader.ms.pShaderBytecode;
      d.indirect = test.indirect; d.bundle = test.bundle; d.replay = test.replay;
      d.stream_output = degenerate && !shader.ms.pShaderBytecode;
      d.depth = degenerate && d.samples == 4;
      const Result r = c.run(d);
      float first_value = d.indexed ? 2 : 0, first_depth = d.indexed ? 0.75f : 0.25f;
      if (degenerate && shader.hs.pShaderBytecode) {
        require(r.stream_bytes == 60, "tessellation produces one degenerate triangle");
        first_value = r.stream_output[4]; first_depth = r.stream_output[2] / r.stream_output[3];
      }
      const std::string name = std::string(path.name) + "-" + test.name;
      const auto s = compareGrid(r, test.triangle, true, name.c_str(), degenerate, first_value, first_depth);
      if (d.depth) for (UINT p = 0; p < kSize * kSize; ++p) for (UINT sample = 0; sample < 4; ++sample)
        require(std::isfinite(r.depth[p * 4 + sample]) &&
                r.depth[p * 4 + sample] == (r.records[p * 4] ? first_depth : 0.5f),
                "post-snap degenerate all samples finite Vertex0 depth");
      if (!degenerate) require(s.interior > 0 && s.edge > 0, "inner oracle exercises full and partial pixels");
      if (test.triangle == inner_boundary) require(s.uncertainty > 0, "inner oracle exercises legal uncertainty boundary");
      std::printf("%s_TIER3 path=%s case=%s samples=%u covered=%u mandatory=%u inner=%u edge=%u uncertain=%u\n",
                  kMarker, path.name, test.name, d.samples, s.invocations, s.mandatory, s.interior, s.edge, s.uncertainty);
    }
  }
  std::printf("%s_CASE_PASS tier3_exact_grid_all_families\n", kMarker);
  // Large coordinates and a long diagonal put cross-product cancellation under pressure,
  // while the target remains 32x32. All transforms are exact powers of two.
  {
    const GridTriangle huge{{{-8192ll * 4096, -8182ll * 4096 + 32},
                              {8192ll * 4096, 8202ll * 4096 + 32}, {8192ll * 4096, -8192ll * 4096}}};
    Shaders shader = vs; shader.ps = blob(inner_dxil[0]);
    Draw d; d.shaders = &shader; d.viewport = {-16384, -16384, 32768, 32768, 0, 1};
    Triangle t;
    for (UINT i = 0; i < 3; ++i) t[i] = {float(huge[i].x) / 67108864, -float(huge[i].y) / 67108864, 0.25f, 1};
    d.triangles = {t};
    const auto s = compareGrid(c.run(d), huge, true, "large-screen-diagonal");
    require(s.interior > 0 && s.edge > 0, "large diagonal full and partial pixels");
    std::printf("%s_TIER3_LARGE viewport=-16384,32768 covered=%u inner=%u edge=%u\n", kMarker, s.invocations, s.interior, s.edge);
    std::printf("%s_CASE_PASS tier3_large_coordinates\n", kMarker);
  }
  // Post-snap degenerates are back-facing even when original winding or FrontCCW changes.
  for (const auto *path : {&paths[0], &paths[9]}) {
  for (bool dxbc_ps : {false, true}) {
    if (path->shaders.ms.pShaderBytecode && dxbc_ps) continue;
    Shaders shader = path->shaders; shader.ps = blob(dxbc_ps ? inner_dxbc[0] : inner_dxil[0]);
    for (bool reverse : {false, true}) for (bool ccw : {false, true}) for (Cull cull : {Cull::Back, Cull::Front}) {
      GridTriangle shape = line; if (reverse) std::swap(shape[1], shape[2]);
      Draw d; d.shaders = &shader; d.triangles = {clipTriangle(shape, true)}; d.front_ccw = ccw; d.cull = cull;
      const Result r = c.run(d);
      if (cull == Cull::Back) {
        for (UINT p = 0; p < kSize * kSize; ++p) {
          const uint32_t *record = &r.records[p * 4];
          if (!record[0]) continue;
          const auto snapped_shape = quantize(shape);
          const auto desc = c.pso(d);
          float attribute, depth;
          std::memcpy(&attribute, record + 1, sizeof(attribute));
          std::memcpy(&depth, record + 2, sizeof(depth));
          const int64_t pre_area = orient(shape[0], shape[1], shape[2]);
          const int64_t post_area = orient(snapped_shape[0], snapped_shape[1], snapped_shape[2]);
          std::fprintf(stderr, "degenerate-cull path=%s producer=%s shape=line reverse=%u front_ccw=%u cull=back "
                       "pso_front_ccw=%u pso_cull=%u pixel=(%u,%u) invocations=%u "
                       "geometric_coverage=1 inner_raw=0x%x vertex0_actual=%.9g vertex0_expected=0 "
                       "depth_actual=%.9g depth_expected=0.25 pre_area2_units=%lld "
                       "pre_area2_pixels=%.17g post_area2_units=%lld\n",
                       path->name, dxbc_ps ? "dxbc" : "dxil", UINT(reverse), UINT(ccw),
                       UINT(desc.RasterizerState.FrontCounterClockwise), UINT(desc.RasterizerState.CullMode),
                       p % kSize, p / kSize, record[0], record[3], attribute, depth,
                       static_cast<long long>(pre_area), double(pre_area) / (4096.0 * 4096.0),
                       static_cast<long long>(post_area));
          // SV_Coverage cannot coexist with this shader's InnerCoverage input.
          // Geometric coverage above means the observed nonzero UAV invocation count;
          // the attribute adds 16 for front-facing, so a correct back-facing Vertex0 is exactly 0.
          for (UINT vertex = 0; vertex < 3; ++vertex) {
            uint32_t bits[4];
            std::memcpy(bits, d.triangles[0][vertex].data(), sizeof(bits));
            std::fprintf(stderr, "  vertex=%u pre_screen=(%.17g,%.17g) post_screen=(%.17g,%.17g) "
                         "clip=(%.9g,%.9g,%.9g,%.9g) bits=(%08x,%08x,%08x,%08x)\n", vertex,
                         double(shape[vertex].x) / 4096, double(shape[vertex].y) / 4096,
                         double(snapped_shape[vertex].x) / 4096, double(snapped_shape[vertex].y) / 4096,
                         d.triangles[0][vertex][0], d.triangles[0][vertex][1],
                         d.triangles[0][vertex][2], d.triangles[0][vertex][3], bits[0], bits[1], bits[2], bits[3]);
          }
          fail("post-snap degenerate back-face cull");
        }
      } else compareGrid(r, shape, true, "degenerate-front-cull", true);
    }
  }
  std::printf("%s_CASE_PASS %s\n", kMarker, path->shaders.ms.pShaderBytecode ?
              "tier3_mesh_degenerate_winding_cull_vertex0" : "tier3_degenerate_winding_cull_vertex0");
  }
  // Near/far clipping turns an originally nondegenerate triangle into a snapped line.
  // SV_ClipDistance is a captured PS input: ON must use original Vertex0, whereas
  // OFF remains nondegenerate. Probe at Vertex1's exact pixel center, whose depth
  // is inside [0,1], so no undefined exterior attribute interpolation is asserted.
  {
    const GridTriangle full{{{16 * 4096 + 2048, 1 * 4096 + 2048},
                              {25 * 4096 + 2048, 1 * 4096 + 2048}, {16 * 4096 + 2048, 10 * 4096 + 2048}}};
    const GridTriangle clipped_line{{full[1], full[2], full[1]}};
    // Clipped endpoint shifts are exactly 9/8193 pixel, below 1/512;
    // both near and far intersections round to the original Vertex1/Vertex2 endpoints.
    require(9.0 / 8193 < 1.0 / 512 && orient(full[0], full[1], full[2]) != 0,
            "depth clip independent post-snap line derivation");
    for (bool far_plane : {false, true}) for (bool enabled : {true, false}) {
      Draw d; d.shaders = &distance_paths[0]; d.samples = 4; d.depth = true; d.depth_clip = enabled;
      d.triangles = {clipTriangle(full)};
      d.triangles[0][0][2] = far_plane ? 2.0f : -1.0f;
      d.triangles[0][1][2] = d.triangles[0][2][2] = far_plane ? 1.0f - 1.0f / 8192 : 1.0f / 8192;
      const auto r = c.run(d);
      const auto s = compareGrid(r, enabled ? clipped_line : full, false,
                                enabled ? "depth-clip ON snapped line" : "depth-clip OFF full triangle");
      constexpr UINT probe = 1 * kSize + 25;
      const auto *record = &r.records[probe * 4];
      const UINT vertex = enabled ? 0 : 1;
      const float expected_clip = d.triangles[0][vertex][0] + 0.1f * d.triangles[0][vertex][3];
      const float expected_depth = enabled ? (far_plane ? 1.0f : 0.0f) : d.triangles[0][1][2];
      uint32_t clip_bits, depth_bits;
      std::memcpy(&clip_bits, &expected_clip, 4); std::memcpy(&depth_bits, &expected_depth, 4);
      if (record[0] != 1 || record[1] != 15 || record[3] != clip_bits || record[2] != depth_bits) {
        std::fprintf(stderr, "depth-clip plane=%s enabled=%u probe=(25,1) invocations=%u coverage=0x%x "
                     "clip_actual=0x%08x clip_expected=0x%08x depth_actual=0x%08x depth_expected=0x%08x\n",
                     far_plane ? "far" : "near", UINT(enabled), record[0], record[1], record[3], clip_bits, record[2], depth_bits);
        fail("captured ClipDistance follows post-depth-clip degeneracy");
      }
      for (UINT p = 0; p < kSize * kSize; ++p) {
        if (r.records[p * 4]) {
          require(r.records[p * 4 + 1] == 15, "depth clip conservative all-sample coverage");
          if (enabled) require(r.records[p * 4 + 3] == clip_bits && r.records[p * 4 + 2] == depth_bits,
                               "depth-clipped snapped line original Vertex0 attributes and depth");
        }
        for (UINT sample = 0; sample < 4; ++sample) {
          const float actual = r.depth[p * 4 + sample];
          require(std::isfinite(actual) && actual >= 0 && actual <= 1, "depth clip finite clamped sample depth");
          if (enabled) require(actual == (r.records[p * 4] ? expected_depth : 0.5f), "depth-clipped line all samples Vertex0 depth");
        }
      }
      std::printf("%s_DEPTH_CLIP path=vs_dxil plane=%s enabled=%u covered=%u mandatory=%u "
                  "probe_clip=%08x expected_clip=%08x probe_depth=%08x expected_depth=%08x\n", kMarker,
                  far_plane ? "far" : "near", UINT(enabled), s.invocations, s.mandatory,
                  record[3], clip_bits, record[2], depth_bits);
    }
    std::printf("%s_CASE_PASS tier3_depth_clip_degenerate_near_far\n", kMarker);
  }
  for (bool dxbc_ps : {false, true}) {
    Shaders shader = vs; shader.ps = blob(dxbc_ps ? inner_dxbc[0] : inner_dxil[0]);
    Draw d; d.shaders = &shader; d.triangles = {clipTriangle(inner_boundary)}; d.samples = 4;
    const auto full = c.run(d); compareGrid(full, inner_boundary, true, "inner-samplemask-full");
    d.sample_mask = 5;
    const auto masked = c.run(d); compareGrid(masked, inner_boundary, true, "inner-samplemask-5");
    for (UINT p = 0; p < kSize * kSize; ++p) require(full.records[p * 4] == masked.records[p * 4] &&
        (full.records[p * 4 + 3] & 1) == (masked.records[p * 4 + 3] & 1), "InnerCoverage independent of SampleMask");
    d.samples = 1; d.forced = 4; d.rtv = false; d.sample_mask = 0;
    compareGrid(c.run(d), inner_boundary, true, "inner-forced4-uav-samplemask0");
    shader.ps = dxbc_ps ? blob(coverage_dxbc) : blob(dxil[1]);
    const auto coverage = c.run(d);
    compareGrid(coverage, inner_boundary, false, "genuine-input-coverage-forced4");
    for (UINT p = 0; p < kSize * kSize; ++p) if (coverage.records[p * 4])
      require(coverage.records[p * 4 + 1] == 15, "genuine DXBC/DXIL InputCoverage forced4");
  }
  std::printf("%s_CASE_PASS tier3_inner_samplemask_forced4_dxbc_dxil\n", kMarker);
  // OFF has undefined InnerCoverage. Check ordinary geometry, never its undefined value.
  {
    const GridTriangle ordinary{{{4 * 4096 + 1024, 4 * 4096 + 1024}, {27 * 4096 + 1024, 4 * 4096 + 1024}, {4 * 4096 + 1024, 27 * 4096 + 1024}}};
    Shaders shader = vs; shader.ps = blob(inner_dxil[0]);
    Draw d; d.shaders = &shader; d.triangles = {clipTriangle(ordinary)}; d.conservative = false;
    const auto r = c.run(d);
    for (UINT y = 0; y < kSize; ++y) for (UINT x = 0; x < kSize; ++x) {
      const GridBox center{int64_t(x) * 4096 + 2048, int64_t(y) * 4096 + 2048,
                           int64_t(x) * 4096 + 2048, int64_t(y) * 4096 + 2048};
      require(r.records[(y * kSize + x) * 4] == UINT(contains(ordinary, center, false)), "ordinary mode center coverage");
    }
    d.conservative = true; compareGrid(c.run(d), ordinary, true, "conservative re-enabled");
    std::printf("%s_CASE_PASS tier3_off_geometry_on_inner_toggle\n", kMarker);
  }
  // A single partial pixel has three non-covered helpers. Its InnerCoverage
  // and both fine derivatives must be zero, exposing helper values without helper UAV writes.
  for (bool dxbc_ps : {false, true}) {
    const GridTriangle tiny{{{10 * 4096 + 1024, 10 * 4096 + 1024},
                              {10 * 4096 + 3072, 10 * 4096 + 1024}, {10 * 4096 + 1024, 10 * 4096 + 3072}}};
    Shaders shader = vs; shader.ps = blob(dxbc_ps ? inner_dxbc[3] : inner_dxil[3]);
    Draw d; d.shaders = &shader; d.triangles = {clipTriangle(tiny)};
    const auto r = c.run(d);
    const auto s = compareGrid(r, tiny, false, "helper InnerCoverage derivatives");
    require(s.invocations == 1, "one partial pixel and three helper neighbors");
    for (UINT p = 0; p < kSize * kSize; ++p) require(r.records[p * 4 + 3] == 0, "noncovered helpers report zero InnerCoverage");
  }
  std::printf("%s_CASE_PASS tier3_helper_inner_zero\n", kMarker);
  // Constant primitive depth avoids the specification's undefined extrapolated early-depth behavior.
  // Late variants explicitly write SV_Depth: failed tests must not erase preceding UAV side effects.
  struct Interaction { const char *name; bool early, depth_pass, stencil, stencil_pass, replace; };
  const Interaction interactions[] = {
      {"early_depth_pass", true, true, false, true, false},
      {"early_depth_fail", true, false, false, true, false},
      {"late_depth_pass_output", false, true, false, true, false},
      {"late_depth_fail_uav", false, false, false, true, false},
      {"early_stencil_pass", true, true, true, true, false},
      {"early_stencil_fail", true, true, true, false, false},
      {"late_stencil_pass", false, true, true, true, false},
      {"late_stencil_fail_uav", false, true, true, false, false},
      {"late_stencil_replace", false, true, true, true, true},
  };
  for (bool dxbc_ps : {false, true}) for (const auto &test : interactions) {
    Shaders shader = vs; shader.ps = blob(dxbc_ps ? inner_dxbc[test.early ? 1 : 2] : inner_dxil[test.early ? 1 : 2]);
    Draw d; d.shaders = &shader; d.triangles = {clipTriangle(interior)};
    d.samples = 4; d.depth = true; d.stencil = test.stencil; d.occlusion = true; d.statistics = true;
    d.depth_func = test.depth_pass ? D3D12_COMPARISON_FUNC_LESS : D3D12_COMPARISON_FUNC_NEVER;
    if (test.early && !test.depth_pass) {
      for (auto &p : d.triangles[0]) p[2] = 0.75f;
      d.depth_func = D3D12_COMPARISON_FUNC_LESS;
    }
    if (!test.early && test.depth_pass) for (auto &p : d.triangles[0]) p[2] = 0.75f;
    d.stencil_ref = test.stencil_pass && !test.replace ? 3 : 7;
    d.stencil_func = test.replace ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_EQUAL;
    d.stencil_pass = test.replace ? D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP;
    const bool accepted = test.depth_pass && test.stencil_pass;
    const bool shader_runs = !test.early || accepted;
    const auto r = c.run(d);
    GridStats stats;
    if (shader_runs) stats = compareGrid(r, interior, true, test.name);
    else for (UINT p = 0; p < kSize * kSize; ++p)
      require(r.records[p * 4] == 0 && r.records[p * 4 + 3] == 0, "early failed test suppresses UAV side effects");
    require(r.occlusion == (accepted ? UINT64(stats.invocations) * 4 : 0), "occlusion exact accepted conservative samples");
    require(r.statistics.PSInvocations >= stats.invocations, "statistics includes conservative UAV invocations");
    for (UINT p = 0; p < kSize * kSize; ++p) for (UINT s = 0; s < 4; ++s) {
      const bool updated = accepted && r.records[p * 4];
      require(std::isfinite(r.depth[p * 4 + s]) && r.depth[p * 4 + s] == (updated ? 0.25f : 0.5f),
              "early/late depth all samples exact");
      if (test.stencil) require(r.stencil[p * 4 + s] == (updated && test.replace ? 7.0f : 3.0f),
                               "early/late stencil all samples exact");
    }
    std::printf("%s_INTERACTION producer=%s case=%s uav=%u occlusion=%llu accepted=%u inner=%u\n", kMarker,
                dxbc_ps ? "dxbc" : "dxil", test.name, stats.invocations, r.occlusion, accepted, stats.interior);
  }
  std::printf("%s_CASE_PASS tier3_early_late_depth_stencil_uav_queries\n", kMarker);
  // Every shader family must execute the advertised conservative raster contract.
  require(path_rejects == 0 && path_passes == 10, "all ten conservative shader families");
  require(options.ConservativeRasterizationTier == D3D12_CONSERVATIVE_RASTERIZATION_TIER_3, "reported public Tier3 after GPU proof");
  std::printf("%s_PASS paths=%u rejected=%u tier=%u\n", kMarker, path_passes, path_rejects,
              UINT(options.ConservativeRasterizationTier));
  return 0;
}
