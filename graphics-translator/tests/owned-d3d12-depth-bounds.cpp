// Real GPU oracles for DirectX-Specs DepthBoundsTest v1.3, both public consumer ABIs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
constexpr char marker[] = "OWNED_D3D12_DEPTH_BOUNDS";
constexpr UINT red = 0xff0000ff, blue = 0xffff0000, green = 0xff00ff00;
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=%08lx\n", marker, step, static_cast<unsigned long>(hr));
  std::exit(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void equal(UINT64 actual, UINT64 expected, const char *step, UINT cell = 0) {
  if (actual != expected) {
    std::fprintf(stderr, "%s_MISMATCH step=%s cell=%u actual=%llu expected=%llu\n", marker, step, cell,
                 static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
    fail(step);
  }
}
std::vector<unsigned char> shader(const char *entry) {
  std::string name = std::string("depth-bounds-") + entry + ".dxil";
  FILE *f = std::fopen(name.c_str(), "rb"); if (!f) fail(name.c_str());
  if (std::fseek(f, 0, SEEK_END)) fail("shader seek"); long n = std::ftell(f);
  if (n <= 0) fail("shader size"); std::rewind(f);
  std::vector<unsigned char> data(static_cast<size_t>(n));
  equal(std::fread(data.data(), 1, data.size(), f), data.size(), "shader read"); std::fclose(f); return data;
}
class Stream {
  std::vector<UINT64> words; size_t bytes = 0;
public:
  template<class T> void add(UINT type, const T &data) {
    struct alignas(void *) Object { UINT type; T data; } object{type, data};
    size_t offset = bytes; bytes += sizeof(object); words.resize((bytes + 7) / 8);
    std::memcpy(reinterpret_cast<char *>(words.data()) + offset, &object, sizeof(object));
  }
  D3D12_PIPELINE_STATE_STREAM_DESC desc() { return {bytes, words.data()}; }
};
// The selected llvm-mingw header may predate DESC2. This is its public SDK layout.
struct Face { D3D12_STENCIL_OP fail, depthFail, pass; D3D12_COMPARISON_FUNC func; UINT8 read, write; };
struct Desc2 { BOOL depth; D3D12_DEPTH_WRITE_MASK write; D3D12_COMPARISON_FUNC func; BOOL stencil; Face front, back; BOOL bounds; };
struct Args { float incoming = .875f, output = .75f; UINT color = red, live = 0, seed = 0, samples = 1, pad0 = 0, pad1 = 0; };
enum class Path { Direct, Indexed, Indirect, Bundle, BundlePersist, Replay, Reset, ClearState };
struct Case {
  const char *name;
  float stored = .5f, min = .4f, max = .6f;
  const char *ps = "plain";
  bool enabled = true, depth = true, noDSV = false, readonly = false, defaults = false, live = false;
  Path path = Path::Direct;
  UINT samples = 1;
  DXGI_FORMAT format = DXGI_FORMAT_D32_FLOAT;
  bool sampleSeed = false, arrayMip = false, desc2 = false;
};
void transition(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; l->ResourceBarrier(1, &b);
}
struct Recording { ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList1> list; };
struct Fixture {
  ComPtr<ID3D12Device> device; ComPtr<ID3D12Device2> device2;
  ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Fence> fence; UINT64 serial = 0;
  ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap, srvHeap;
  std::map<std::string, std::vector<unsigned char>> shaders;
  ComPtr<ID3DBlob> dxbcVS, dxbcPS;
  Fixture() {
    HMODULE module = GetModuleHandleW(L"d3d12.dll"); using Id = const char *(WINAPI *)();
    auto id = reinterpret_cast<Id>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    if (!id || std::strcmp(id(), "owned-graphics-d3d12")) fail("backend identity");
    std::printf("%s_BACKEND id=%s\n", marker, id());
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory"); check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    check(device.As(&device2), "device2");
    D3D12_COMMAND_QUEUE_DESC q{}; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[3]{}; p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants = {0, 0, 8};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor = {0, 0};
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC desc{3, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, error; check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "serialize root");
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    for (auto type : {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV}) {
      D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type = type; h.NumDescriptors = 2;
      if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      auto out = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? &rtvHeap : type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV ? &dsvHeap : &srvHeap;
      check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(out)), "descriptor heap");
    }
    for (const char *entry : {"vs", "plain", "seed", "late", "early", "lateDiscard", "earlyDiscard", "read", "read-msaa"}) shaders.emplace(entry, shader(entry));
    const char *source = "cbuffer A:register(b0){float z;float o;uint c;} float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==1?3:-1,i==2?-3:1,z,1);} float4 ps():SV_Target{return float4(c&255,(c>>8)&255,(c>>16)&255,c>>24)/255.;}";
    for (bool ps : {false, true}) {
      ComPtr<ID3DBlob> errors; auto out = ps ? &dxbcPS : &dxbcVS;
      HRESULT hr = D3DCompile(source, std::strlen(source), "depth-bounds", nullptr, nullptr, ps ? "ps" : "vs", ps ? "ps_5_0" : "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, out, &errors);
      if (FAILED(hr) && errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
      check(hr, "DXBC producer");
    }
  }
  Recording record(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
    Recording r; check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&r.allocator)), "allocator");
    check(device->CreateCommandList(0, type, r.allocator.Get(), nullptr, IID_PPV_ARGS(&r.list)), "list1"); return r;
  }
  void execute(ID3D12GraphicsCommandList1 *list) {
    ID3D12CommandList *lists[]{list}; queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++serial), "signal"); HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) fail("event"); check(fence->SetEventOnCompletion(serial, event), "completion");
    DWORD result = WaitForSingleObject(event, 30000); CloseHandle(event); if (result != WAIT_OBJECT_0) fail("GPU timeout");
  }
  ComPtr<ID3D12Resource> resource(D3D12_RESOURCE_DESC d, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES h{}; h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1; ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "resource"); return r;
  }
  ComPtr<ID3D12Resource> buffer(UINT bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav = false) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; return resource(d, type, state);
  }
  D3D12_SHADER_BYTECODE code(const char *entry) { auto &s = shaders.at(entry); return {s.data(), s.size()}; }
  ComPtr<ID3D12PipelineState> pipeline(const Case &c, bool probe = false, UINT mask = UINT_MAX) {
    Stream stream; stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root.Get());
    const bool dxbc = std::strcmp(c.ps, "dxbc") == 0;
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, dxbc ? D3D12_SHADER_BYTECODE{dxbcVS->GetBufferPointer(), dxbcVS->GetBufferSize()} : code("vs"));
    if (probe || std::strcmp(c.ps, "none"))
      stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, probe ? code("plain") : dxbc ? D3D12_SHADER_BYTECODE{dxbcPS->GetBufferPointer(), dxbcPS->GetBufferSize()} : code(c.ps));
    D3D12_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
    D3D12_RASTERIZER_DESC raster{}; raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_NONE; raster.DepthClipEnable = TRUE;
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, raster);
    D3D12_DEPTH_STENCIL_DESC1 depth{}; depth.DepthEnable = c.depth; depth.DepthWriteMask = c.readonly || probe ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = probe ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
    depth.StencilEnable = !c.noDSV && c.format == DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth.StencilReadMask = 255; depth.StencilWriteMask = probe || c.readonly ? 0 : 255;
    depth.FrontFace = depth.BackFace = {probe ? D3D12_STENCIL_OP_KEEP : D3D12_STENCIL_OP_REPLACE,
      probe ? D3D12_STENCIL_OP_KEEP : D3D12_STENCIL_OP_REPLACE, probe ? D3D12_STENCIL_OP_KEEP : D3D12_STENCIL_OP_REPLACE,
      probe ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS};
    depth.DepthBoundsTestEnable = !probe && c.enabled;
    if (c.desc2) {
      Face face{depth.FrontFace.StencilFailOp, depth.FrontFace.StencilDepthFailOp, depth.FrontFace.StencilPassOp, depth.FrontFace.StencilFunc, depth.StencilReadMask, depth.StencilWriteMask};
      Desc2 d{depth.DepthEnable, depth.DepthWriteMask, depth.DepthFunc, depth.StencilEnable, face, face, depth.DepthBoundsTestEnable}; stream.add(26, d);
    } else stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, depth);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, c.noDSV ? DXGI_FORMAT_UNKNOWN : c.format);
    D3D12_RT_FORMAT_ARRAY formats{}; formats.NumRenderTargets = 1; formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{c.samples, 0});
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, mask);
    auto desc = stream.desc(); ComPtr<ID3D12PipelineState> pso;
    check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "depth bounds PSO"); return pso;
  }
  ComPtr<ID3D12QueryHeap> query(D3D12_QUERY_HEAP_TYPE type, UINT count) {
    D3D12_QUERY_HEAP_DESC d{type, count, 1}; ComPtr<ID3D12QueryHeap> q; check(device->CreateQueryHeap(&d, IID_PPV_ARGS(&q)), "query heap"); return q;
  }
  void run(const Case &c) {
    std::printf("%s_BEGIN case=%s samples=%u format=%u\n", marker, c.name, c.samples, UINT(c.format));
    auto pso = pipeline(c); auto r = record(); auto *l = r.list.Get();
    if (c.path == Path::Reset) {
      l->OMSetDepthBounds(.9f, .1f); check(l->Close(), "pre-reset close"); check(r.allocator->Reset(), "allocator reset"); check(l->Reset(r.allocator.Get(), nullptr), "Reset");
    } else if (c.path == Path::ClearState) { l->OMSetDepthBounds(.9f, .1f); l->ClearState(nullptr); }
    D3D12_RESOURCE_DESC tex{}; tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; tex.Width = tex.Height = 4;
    tex.DepthOrArraySize = tex.MipLevels = 1; tex.SampleDesc.Count = c.samples; tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto target = resource(tex, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    tex.Format = c.format; tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    if (c.arrayMip) { tex.Width = tex.Height = 8; tex.MipLevels = tex.DepthOrArraySize = 2; }
    auto depth = resource(tex, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = c.format;
    dv.ViewDimension = c.samples == 1 ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMS;
    dv.Texture2DArray = {c.arrayMip ? 1u : 0u, c.arrayMip ? 1u : 0u, 1};
    auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart(); device->CreateDepthStencilView(depth.Get(), &dv, dsv);
    const float clear[4]{}; l->ClearRenderTargetView(rtv, clear, 0, nullptr);
    l->ClearDepthStencilView(dsv, c.format == DXGI_FORMAT_D24_UNORM_S8_UINT ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH, c.stored, 3, 0, nullptr);
    const UINT bytes = 16 * c.samples * 4;
    auto effects = buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
    auto zero = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr; D3D12_RANGE none{}; check(zero->Map(0, &none, &mapped), "zero map"); std::memset(mapped, 0, bytes); zero->Unmap(0, nullptr);
    l->CopyBufferRegion(effects.Get(), 0, zero.Get(), 0, bytes); transition(l, effects.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_VIEWPORT vp{0, 0, 4, 4, 0, 1}; D3D12_RECT scissor{0, 0, 4, 4};
    l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &scissor); l->SetGraphicsRootSignature(root.Get());
    l->SetGraphicsRootUnorderedAccessView(1, effects->GetGPUVirtualAddress()); l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Args args; args.samples = c.samples; args.live = c.live; args.seed = c.sampleSeed;
    if (!std::strcmp(c.name, "incoming_inside_stored_outside")) args.incoming = .5f;
    ComPtr<ID3D12PipelineState> seedPSO;
    if (c.sampleSeed) {
      Case seed = c; seed.enabled = false; seed.ps = "seed";
      seedPSO = pipeline(seed); l->SetPipelineState(seedPSO.Get()); l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      l->SetGraphicsRoot32BitConstants(0, 8, &args, 0); l->OMSetStencilRef(3); l->DrawInstanced(3, 1, 0, 0);
    }
    if (c.readonly) {
      transition(l, depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
      dv.Flags = c.format == DXGI_FORMAT_D24_UNORM_S8_UINT ? D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_READ_ONLY_DEPTH;
      dsv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); device->CreateDepthStencilView(depth.Get(), &dv, dsv);
    }
    l->OMSetRenderTargets(1, &rtv, FALSE, c.noDSV ? nullptr : &dsv); l->OMSetStencilRef(9);
    l->SetPipelineState(pso.Get()); l->SetGraphicsRoot32BitConstants(0, 8, &args, 0);
    if (!c.defaults && c.path != Path::Reset && c.path != Path::ClearState) l->OMSetDepthBounds(c.min, c.max);
    auto oq = query(D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2); auto sq = query(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1);
    l->BeginQuery(oq.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0); l->BeginQuery(oq.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1); l->BeginQuery(sq.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    const UINT vertices = c.live ? 6 : 3; Recording bundle;
    ComPtr<ID3D12Resource> indices, indirect; ComPtr<ID3D12CommandSignature> signature;
    if (c.path == Path::Indexed) {
      indices = buffer(6 * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      check(indices->Map(0, &none, &mapped), "index map"); const UINT data[]{0,1,2,3,4,5}; std::memcpy(mapped, data, sizeof(data)); indices->Unmap(0, nullptr);
      D3D12_INDEX_BUFFER_VIEW view{indices->GetGPUVirtualAddress(), 24, DXGI_FORMAT_R32_UINT}; l->IASetIndexBuffer(&view); l->DrawIndexedInstanced(vertices, 1, 0, 0, 0);
    } else if (c.path == Path::Indirect) {
      indirect = buffer(16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      D3D12_DRAW_ARGUMENTS draw{vertices, 1, 0, 0}; check(indirect->Map(0, &none, &mapped), "indirect map"); std::memcpy(mapped, &draw, sizeof(draw)); indirect->Unmap(0, nullptr);
      D3D12_INDIRECT_ARGUMENT_DESC argument{}; argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
      D3D12_COMMAND_SIGNATURE_DESC sd{16, 1, &argument, 0}; check(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)), "draw signature");
      l->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
    } else if (c.path == Path::Bundle || c.path == Path::BundlePersist) {
      bundle = record(D3D12_COMMAND_LIST_TYPE_BUNDLE); bundle.list->SetPipelineState(pso.Get()); bundle.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      bundle.list->DrawInstanced(vertices, 1, 0, 0);
      if (c.path == Path::BundlePersist) bundle.list->OMSetDepthBounds(.8f, .9f);
      check(bundle.list->Close(), "bundle close"); l->ExecuteBundle(bundle.list.Get());
      if (c.path == Path::BundlePersist) { args.color = blue; l->SetGraphicsRoot32BitConstants(0, 8, &args, 0); l->DrawInstanced(3, 1, 0, 0); }
    } else l->DrawInstanced(vertices, 1, 0, 0);
    l->EndQuery(sq.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0); l->EndQuery(oq.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1); l->EndQuery(oq.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    auto queries = buffer(104, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    l->ResolveQueryData(oq.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, queries.Get(), 0); l->ResolveQueryData(oq.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1, 1, queries.Get(), 8);
    l->ResolveQueryData(sq.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, queries.Get(), 16);
    auto effectReadback = buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(l, effects.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); l->CopyBufferRegion(effectReadback.Get(), 0, effects.Get(), 0, bytes);
    transition(l, effects.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = c.samples == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS; srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.Get(), &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{}; cd.pRootSignature = root.Get(); cd.CS = code(c.samples == 1 ? "read" : "read-msaa");
    ComPtr<ID3D12PipelineState> readPSO; check(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&readPSO)), "read PSO");
    auto colorOutput = buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    auto colorReadback = buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto probeReadback = buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto readColors = [&](ID3D12Resource *dst) {
      l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
      transition(l, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      l->SetComputeRootSignature(root.Get()); l->SetPipelineState(readPSO.Get());
      ID3D12DescriptorHeap *heaps[]{srvHeap.Get()}; l->SetDescriptorHeaps(1, heaps); l->SetComputeRoot32BitConstants(0, 8, &args, 0);
      l->SetComputeRootUnorderedAccessView(1, colorOutput->GetGPUVirtualAddress()); l->SetComputeRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart()); l->Dispatch(1, 1, 1);
      transition(l, colorOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); l->CopyBufferRegion(dst, 0, colorOutput.Get(), 0, bytes);
      transition(l, colorOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(l, target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    };
    readColors(colorReadback.Get());
    ComPtr<ID3D12Resource> depthReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint{};
    if (c.format == DXGI_FORMAT_D16_UNORM && c.samples == 1) {
      UINT64 size = 0; device->GetCopyableFootprints(&tex, 0, 1, 0, &depthFootprint, nullptr, nullptr, &size);
      depthReadback = buffer(UINT(size), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      const auto state = c.readonly ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE;
      transition(l, depth.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION source{}, dest{}; source.pResource = depth.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dest.pResource = depthReadback.Get(); dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dest.PlacedFootprint = depthFootprint;
      l->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr); transition(l, depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    std::vector<ComPtr<ID3D12PipelineState>> probes;
    float minimum = std::isnan(c.min) ? 0 : c.min, maximum = std::isnan(c.max) ? 0 : c.max;
    if (c.defaults || c.path == Path::Reset || c.path == Path::ClearState) { minimum = 0; maximum = 1; }
    UINT passedSamples = 0;
    for (UINT s = 0; s < c.samples; ++s) {
      float stored = c.sampleSeed ? .25f + .125f * s : c.stored;
      bool pass = !c.enabled || c.noDSV || (minimum <= stored && stored <= maximum);
      passedSamples += pass;
      if (!c.noDSV) {
        if (s == 0) {
          l->ClearRenderTargetView(rtv, clear, 0, nullptr); l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
          l->SetGraphicsRootSignature(root.Get()); l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
        Case probe = c; probe.depth = true; probe.live = false;
        probes.push_back(pipeline(probe, true, 1u << s)); l->SetPipelineState(probes.back().Get());
        Args expected; expected.samples = c.samples; expected.color = green;
        const bool writes = pass && !c.readonly && std::strcmp(c.ps, "lateDiscard");
        expected.incoming = writes && c.depth ? c.live ? .75f : !std::strcmp(c.ps, "late") ? .75f : .875f : stored;
        // D16 clears store a normalized integer, not the original float. Probe
        // that exact decoded value; raw CopyTextureRegion below independently
        // checks the integer, so EQUAL cannot hide a changed stored depth.
        if (c.format == DXGI_FORMAT_D16_UNORM && !(writes && c.depth))
          expected.incoming = std::round(expected.incoming * 65535.f) / 65535.f;
        l->SetGraphicsRoot32BitConstants(0, 8, &expected, 0); l->OMSetStencilRef(writes ? 9 : 3); l->DrawInstanced(3, 1, 0, 0);
      }
    }
    if (!c.noDSV) readColors(probeReadback.Get());
    check(l->Close(), "close");
    const UINT replays = c.path == Path::Replay ? 2 : 1;
    for (UINT replay = 0; replay < replays; ++replay) {
      execute(l);
      D3D12_RANGE range{0, bytes}; check(colorReadback->Map(0, &range, &mapped), "color map");
      for (UINT pixel = 0; pixel < 16; ++pixel) for (UINT s = 0; s < c.samples; ++s) {
        float stored = c.sampleSeed ? .25f + .125f * s : c.stored;
        bool pass = !c.enabled || c.noDSV || (minimum <= stored && stored <= maximum);
        UINT expected = !std::strcmp(c.ps, "none") || std::strstr(c.ps, "Discard") || !pass ? 0 : c.path == Path::BundlePersist ? blue : red;
        equal(static_cast<UINT *>(mapped)[pixel * c.samples + s], expected, "color", pixel * c.samples + s);
      }
      colorReadback->Unmap(0, &none);
      if (depthReadback) {
        check(depthReadback->Map(0, nullptr, &mapped), "D16 raw map");
        const bool pass = !c.enabled || (minimum <= c.stored && c.stored <= maximum);
        const float expectedDepth = pass && c.depth && !c.readonly ? !std::strcmp(c.ps, "late") ? .75f : .875f : c.stored;
        const UINT expectedWord = UINT(std::round(expectedDepth * 65535.f));
        for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) {
          UINT16 actual; std::memcpy(&actual, static_cast<const char *>(mapped) + depthFootprint.Offset + y * depthFootprint.Footprint.RowPitch + x * 2, 2);
          equal(actual, expectedWord, "D16 raw stored depth", y * 4 + x);
        }
        depthReadback->Unmap(0, &none);
      }
      if (!c.noDSV) {
        check(probeReadback->Map(0, &range, &mapped), "probe map");
        for (UINT cell = 0; cell < 16 * c.samples; ++cell) equal(static_cast<UINT *>(mapped)[cell], green, "depth stencil probe", cell);
        probeReadback->Unmap(0, &none);
      }
      check(effectReadback->Map(0, &range, &mapped), "effects map");
      for (UINT pixel = 0; pixel < 16; ++pixel) for (UINT s = 0; s < c.samples; ++s) {
        float stored = c.sampleSeed ? .25f + .125f * s : c.stored;
        bool pass = !c.enabled || c.noDSV || (minimum <= stored && stored <= maximum);
        UINT expected = !std::strcmp(c.ps, "late") ? c.live ? 2 : 1 : std::strstr(c.ps, "Discard") ? 1 : !std::strcmp(c.ps, "early") && pass ? 1 : 0;
        equal(static_cast<UINT *>(mapped)[pixel * c.samples + s], expected, "UAV effects", pixel * c.samples + s);
      }
      effectReadback->Unmap(0, &none);
      range.End = 104; check(queries->Map(0, &range, &mapped), "query map"); auto *q = static_cast<UINT64 *>(mapped);
      UINT multiplier = c.path == Path::BundlePersist ? 2 : 1;
      const UINT visible = !std::strcmp(c.ps, "lateDiscard") ? 0 : passedSamples;
      equal(q[0], UINT64(visible) * 16 * multiplier, "occlusion"); equal(q[1], visible ? 1 : 0, "binary occlusion");
      D3D12_QUERY_DATA_PIPELINE_STATISTICS stats; std::memcpy(&stats, q + 2, sizeof(stats));
      equal(stats.IAVertices, vertices * multiplier, "IA vertices"); equal(stats.IAPrimitives, vertices / 3 * multiplier, "IA primitives"); equal(stats.VSInvocations, vertices * multiplier, "VS invocations");
      equal(stats.CInvocations, vertices / 3 * multiplier, "clip invocations"); equal(stats.CPrimitives, vertices / 3 * multiplier, "clip primitives");
      equal(stats.GSInvocations + stats.GSPrimitives + stats.HSInvocations + stats.DSInvocations + stats.CSInvocations, 0, "unbound stages");
      UINT expectedPS = !std::strcmp(c.ps, "none") ? 0 : !std::strcmp(c.ps, "early") ? passedSamples * 16 :
                        !std::strcmp(c.ps, "late") ? c.samples * 16 * (c.live ? 2 : 1) : 16 * multiplier;
      if (!std::strcmp(c.ps, "early") || !std::strcmp(c.ps, "none") || passedSamples == c.samples)
        equal(stats.PSInvocations, expectedPS, "PS invocations");
      std::printf("%s_STATS case=%s replay=%u ps=%llu occlusion=%llu binary=%llu\n", marker, c.name, replay,
                  static_cast<unsigned long long>(stats.PSInvocations), static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]));
      queries->Unmap(0, &none);
    }
    std::printf("%s_CASE_PASS case=%s samples=%u format=%u cells=%u queries=3 replay=%u\n", marker, c.name, c.samples, UINT(c.format), 16 * c.samples, replays);
  }
};
}
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0); Fixture f;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{}; check(f.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)), "OPTIONS2");
  // Check the capability after the GPU cases so a reporting failure preserves rendering evidence.
  std::printf("%s_CAP supported=%u\n", marker, UINT(options.DepthBoundsTestSupported));
  std::vector<Case> cases;
  auto add = [&](const char *name) -> Case & { cases.push_back(Case{name}); return cases.back(); };
  add("stored_not_incoming").ps = "dxbc";
  add("incoming_inside_stored_outside").stored = .25f;
  add("odepth_not_stored").ps = "late";
  auto &min = add("inclusive_min"); min.stored = min.min = .5f;
  auto &max = add("inclusive_max"); max.stored = max.max = .5f;
  auto &inverted = add("inverted"); inverted.min = .8f; inverted.max = .2f;
  auto &invertedHigh = add("inverted_above_one"); invertedHigh.min = 2; invertedHigh.max = 1.5f;
  auto &invertedLow = add("inverted_below_zero"); invertedLow.min = -.25f; invertedLow.max = -.5f;
  auto &invertedLate = add("inverted_late_uav"); invertedLate.ps = "late"; invertedLate.min = .8f; invertedLate.max = .2f; invertedLate.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  auto &invertedEarly = add("inverted_early_uav"); invertedEarly.ps = "early"; invertedEarly.min = .8f; invertedEarly.max = .2f; invertedEarly.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  auto &nanMin = add("nan_min_zero"); nanMin.min = std::numeric_limits<float>::quiet_NaN(); nanMin.stored = 0;
  auto &nanMax = add("nan_max_zero"); nanMax.min = nanMax.stored = 0; nanMax.max = std::numeric_limits<float>::quiet_NaN();
  auto &nanReject = add("nan_max_reject"); nanReject.min = 0; nanReject.max = std::numeric_limits<float>::quiet_NaN();
  add("default_range").defaults = true;
  add("reset_defaults").path = Path::Reset; add("clearstate_defaults").path = Path::ClearState;
  auto &disabled = add("pso_disabled"); disabled.enabled = false; disabled.min = .9f; disabled.max = .1f;
  auto &noDSV = add("no_dsv"); noDSV.noDSV = true; noDSV.min = .9f; noDSV.max = .1f;
  auto &noDepth = add("depth_disabled"); noDepth.depth = false; noDepth.stored = .25f;
  auto &noDepthPass = add("depth_disabled_pass"); noDepthPass.depth = false;
  add("no_ps").ps = "none";
  auto &noPSFail = add("no_ps_fail"); noPSFail.ps = "none"; noPSFail.stored = .25f;
  add("readonly_pass").readonly = true;
  auto &readonlyFail = add("readonly_fail"); readonlyFail.readonly = true; readonlyFail.stored = .25f;
  auto &readonlyStencil = add("readonly_stencil"); readonlyStencil.readonly = true; readonlyStencil.format = DXGI_FORMAT_D24_UNORM_S8_UINT; readonlyStencil.ps = "late";
  auto &earlyFail = add("early_uav_fail"); earlyFail.ps = "early"; earlyFail.stored = .25f;
  add("early_uav_pass").ps = "early";
  auto &earlyDiscard = add("early_discard_commits_ds"); earlyDiscard.ps = "earlyDiscard"; earlyDiscard.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  auto &lateDiscard = add("late_discard_preserves_ds"); lateDiscard.ps = "lateDiscard"; lateDiscard.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  auto &lateFail = add("late_uav_fail"); lateFail.ps = "late"; lateFail.stored = .25f;
  add("bundle_inherit").path = Path::Bundle; add("bundle_persist").path = Path::BundlePersist;
  add("indexed").path = Path::Indexed; add("indirect").path = Path::Indirect;
  add("replay").path = Path::Replay; add("array_mip").arrayMip = true;
  add("desc2_enabled").desc2 = true;
  auto &desc2Off = add("desc2_disabled"); desc2Off.desc2 = true; desc2Off.enabled = false; desc2Off.min = .9f; desc2Off.max = .1f;
  auto &live = add("live_primitive_depth"); live.ps = "late"; live.live = true;
  for (auto format : {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT}) {
    auto &pass = add(format == DXGI_FORMAT_D16_UNORM ? "d16_updates" : "d24_updates"); pass.format = format; pass.ps = "late";
    auto &reject = add(format == DXGI_FORMAT_D16_UNORM ? "d16_no_updates" : "d24_no_updates"); reject.format = format; reject.ps = "late"; reject.stored = .25f;
    // 0x3333/0xffff lies on the D16 grid. This tests retained depth, not the
    // separate float-to-fixed rounding of a nonrepresentable .25 clear.
    if (format == DXGI_FORMAT_D16_UNORM) reject.stored = float(0x3333) / 65535.f;
  }
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{DXGI_FORMAT_D32_FLOAT, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
  check(f.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)), "4x support"); if (!quality.NumQualityLevels) fail("required 4x depth");
  for (const char *kind : {"late", "early"}) {
    auto &mixed = add(!std::strcmp(kind, "late") ? "sample4_late_mixed" : "sample4_early_mixed"); mixed.ps = kind; mixed.samples = 4; mixed.sampleSeed = true; mixed.min = .375f; mixed.max = .5f;
    auto &failed = add(!std::strcmp(kind, "late") ? "sample4_late_fail" : "sample4_early_fail"); failed.ps = kind; failed.samples = 4; failed.stored = .25f;
  }
  auto &live4 = add("sample4_live_primitive_depth"); live4.samples = 4; live4.ps = "late"; live4.live = true;
  auto &d16x4 = add("sample4_d16_updates"); d16x4.samples = 4; d16x4.ps = "late"; d16x4.format = DXGI_FORMAT_D16_UNORM;
  auto &d24x4 = add("sample4_d24_no_updates"); d24x4.samples = 4; d24x4.ps = "early"; d24x4.format = DXGI_FORMAT_D24_UNORM_S8_UINT; d24x4.stored = .25f;
  for (const auto &c : cases) f.run(c);
  equal(options.DepthBoundsTestSupported, TRUE, "DepthBoundsTestSupported");
  std::printf("%s_PASS cases=%u\n", marker, UINT(cases.size())); return 0;
}
