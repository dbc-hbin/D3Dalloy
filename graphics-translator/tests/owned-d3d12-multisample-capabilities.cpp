#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace {
constexpr char marker[] = "OWNED_D3D12_MULTISAMPLE_CAPABILITIES";
constexpr UINT standardPattern = 0xffffffffu;
constexpr UINT centerPattern = 0xfffffffeu;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=%08lx\n", marker, step, static_cast<unsigned long>(hr));
  std::exit(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool ok, const char *step) { if (!ok) fail(step); }

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = p.VisibleNodeMask = 1;
  return p;
}
D3D12_RESOURCE_DESC texture(DXGI_FORMAT format, UINT samples, UINT quality,
                            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = d.Height = 4;
  d.DepthOrArraySize = d.MipLevels = 1;
  d.Format = format;
  d.SampleDesc = {samples, quality};
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Flags = flags;
  return d;
}
D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
ComPtr<ID3DBlob> compile(const char *source, const char *entry, const char *profile) {
  ComPtr<ID3DBlob> code, errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile,
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                          0, &code, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("D3DCompile", hr);
  }
  return code;
}
UINT qualityLevels(ID3D12Device *device, DXGI_FORMAT format, UINT samples,
                   D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags) {
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data{format, samples, flags, 0xdeadbeefu};
  HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &data, sizeof(data));
  require(data.Flags == flags, "quality query changed input Flags");
  require(SUCCEEDED(hr) || data.NumQualityLevels == 0, "failed query retained quality levels");
  std::printf("%s_QUERY format=%u count=%u flags=%u hr=%08lx levels=%u\n", marker,
              static_cast<UINT>(format), samples, static_cast<UINT>(flags),
              static_cast<unsigned long>(hr), data.NumQualityLevels);
  return SUCCEEDED(hr) ? data.NumQualityLevels : 0;
}
void rejectedDescriptor(ID3D12Device *device, ID3D12Heap *placedHeap,
                        const D3D12_RESOURCE_DESC &d, const char *step) {
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  void *committed = reinterpret_cast<void *>(UINT_PTR(0x1234));
  HRESULT committedResult = device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d,
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             __uuidof(ID3D12Resource), &committed);
  require(FAILED(committedResult) && !committed, step);
  HRESULT preflight = device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       __uuidof(ID3D12Resource), nullptr);
  require(FAILED(preflight), step);
  void *placed = reinterpret_cast<void *>(UINT_PTR(0x1234));
  HRESULT placedResult = device->CreatePlacedResource(placedHeap, 0, &d,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      __uuidof(ID3D12Resource), &placed);
  require(FAILED(placedResult) && !placed, "invalid placed resource returned object");
  require(FAILED(device->CreatePlacedResource(placedHeap, 0, &d,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             __uuidof(ID3D12Resource), nullptr)),
          "invalid placed resource passed null-output preflight");
  auto allocation = device->GetResourceAllocationInfo(0, 1, &d);
  require(allocation.SizeInBytes == UINT64_MAX, "invalid allocation info succeeded");
  std::printf("%s_REJECT format=%u count=%u quality=%u hr=%08lx\n", marker,
              static_cast<UINT>(d.Format), d.SampleDesc.Count, d.SampleDesc.Quality,
              static_cast<unsigned long>(preflight));
}
ComPtr<ID3D12Resource> validTexture(ID3D12Device *device, const D3D12_RESOURCE_DESC &d,
                                    D3D12_RESOURCE_STATES state) {
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT preflight = device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d,
                                                       state, nullptr, __uuidof(ID3D12Resource), nullptr);
  require(preflight == S_FALSE, "valid null-output preflight");
  auto allocation = device->GetResourceAllocationInfo(0, 1, &d);
  require(allocation.SizeInBytes != UINT64_MAX && allocation.SizeInBytes != 0,
          "valid allocation info rejected");
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                        IID_PPV_ARGS(&result)), "valid texture creation");
  return result;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  list->ResourceBarrier(1, &b);
}

constexpr char shaders[] = R"(
struct VSOut { float4 position : SV_Position; };
VSOut vsMain(uint id : SV_VertexID) {
  VSOut o;
  o.position = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, .375, 1);
  return o;
}
VSOut vsEdge(uint id : SV_VertexID) {
  float2 positions[6] = {
    float2(-1, 1), float2(-.25, 1), float2(-1, -1),
    float2(-.25, 1), float2(-.25, -1), float2(-1, -1)
  };
  VSOut o;
  o.position = float4(positions[id], .375, 1);
  return o;
}
struct CenterOut { float4 position : SV_Position; float4 uv : TEXCOORD0; };
CenterOut vsCenterEdge(uint id : SV_VertexID) {
  float2 positions[6] = {
    float2(-1, 1), float2(-.1875, 1), float2(-1, -1),
    float2(-.1875, 1), float2(-.1875, -1), float2(-1, -1)
  };
  CenterOut o;
  o.position = float4(positions[id], .375, 1);
  o.uv = float4((positions[id].x + 1) * 2, (1 - positions[id].y) * 2, 0, 0);
  return o;
}
float4 psCenter(centroid float4 uv : TEXCOORD0) : SV_Target0 {
  return float4(1, uv.x / 4.0, uv.y / 4.0, 1);
}
float psCenterDepth() : SV_Depth { return .375; }
struct SemanticOut { float4 position : SV_Position; float4 color : CoLoR0; float4 uv : TEXCOORD1; };
SemanticOut vsSemantic(uint id : SV_VertexID) {
  SemanticOut o;
  o.position = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, .375, 1);
  o.color = float4(.25, .5, .75, 1);
  o.uv = float4(.75, .125, 0, 0);
  return o;
}
float4 psSemantic(float4 uv : texcoord1, float4 color : COLOR0) : SV_Target0 {
  return float4(uv.x, color.y, uv.y, color.x);
}
float4 psFloat(uint sample : SV_SampleIndex) : SV_Target0 {
  return float4((sample + 1) / 16.0, .25, .5, 1);
}
uint4 psUint(uint sample : SV_SampleIndex) : SV_Target0 {
  return uint4(sample + 1, 7, 11, 13);
}
struct DepthResult { float depth : SV_Depth; };
DepthResult psDepth(uint sample : SV_SampleIndex) {
  DepthResult o;
  o.depth = .375 + sample / 32.0;
  return o;
}
Texture2DMS<float4> colorMS : register(t0);
Texture2DMS<uint4> integerMS : register(t0);
Texture2DMS<float> depthMS : register(t0);
Texture2D<float4> colorResolved : register(t0);
Texture2D<uint4> integerResolved : register(t0);
Texture2D<float> depthResolved : register(t0);
RWStructuredBuffer<uint> result : register(u0);
[numthreads(1,1,1)] void readFloat(uint3 id : SV_DispatchThreadID) {
  uint width, height, samples;
  colorMS.GetDimensions(width, height, samples);
  for (uint i = 0; i < samples; ++i)
    result[i] = asuint(colorMS.Load(int2(1,1), i).x);
}
[numthreads(1,1,1)] void readCenter(uint3 id : SV_DispatchThreadID) {
  uint width, height, samples;
  colorMS.GetDimensions(width, height, samples);
  for (uint i = 0; i < samples; ++i) {
    float4 sample = colorMS.Load(int2(1,1), i);
    result[i] = asuint(sample.x);
    result[16 + i] = asuint(sample.y);
  }
}
[numthreads(1,1,1)] void readUint(uint3 id : SV_DispatchThreadID) {
  uint width, height, samples;
  integerMS.GetDimensions(width, height, samples);
  for (uint i = 0; i < samples; ++i)
    result[i] = integerMS.Load(int2(1,1), i).x;
}
[numthreads(1,1,1)] void readDepth(uint3 id : SV_DispatchThreadID) {
  uint width, height, samples;
  depthMS.GetDimensions(width, height, samples);
  for (uint i = 0; i < samples; ++i)
    result[i] = asuint(depthMS.Load(int2(1,1), i));
}
[numthreads(1,1,1)] void readFloatResolved(uint3 id : SV_DispatchThreadID) {
  result[8] = asuint(colorResolved.Load(int3(1,1,0)).x);
}
[numthreads(1,1,1)] void readUintResolved(uint3 id : SV_DispatchThreadID) {
  result[8] = integerResolved.Load(int3(1,1,0)).x;
}
[numthreads(1,1,1)] void readDepthResolved(uint3 id : SV_DispatchThreadID) {
  result[8] = asuint(depthResolved.Load(int3(1,1,0)));
}
)";

struct Fixture {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 fenceValue = 0;
  HANDLE event = nullptr;
  ComPtr<ID3D12RootSignature> graphicsRoot, readRoot;
  ComPtr<ID3DBlob> vs, vsEdge, vsCenterEdge, vsSemantic, psFloat, psUint, psDepth, psCenter;
  ComPtr<ID3DBlob> psCenterDepth, psSemantic;
  ComPtr<ID3DBlob> csFloat, csUint, csDepth, csCenter;
  ComPtr<ID3DBlob> csFloatResolved, csUintResolved, csDepthResolved;
  ~Fixture() { if (event) CloseHandle(event); }

  void init() {
    HMODULE module = GetModuleHandleW(L"d3d12.dll");
    require(module != nullptr, "GetModuleHandle(d3d12)");
    using BackendId = const char *(WINAPI *)();
    auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned backend");
    std::printf("%s_BACKEND id=owned-graphics-d3d12\n", marker);
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
    D3D12_ROOT_SIGNATURE_DESC g{};
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&g, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &serialized, nullptr), "serialize graphics root");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&graphicsRoot)), "graphics root");
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC r{};
    r.NumParameters = 2;
    r.pParameters = parameters;
    check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &serialized, nullptr), "serialize read root");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&readRoot)), "read root");
    vs = compile(shaders, "vsMain", "vs_5_0");
    vsEdge = compile(shaders, "vsEdge", "vs_5_0");
    vsCenterEdge = compile(shaders, "vsCenterEdge", "vs_5_0");
    vsSemantic = compile(shaders, "vsSemantic", "vs_5_0");
    psFloat = compile(shaders, "psFloat", "ps_5_0");
    psUint = compile(shaders, "psUint", "ps_5_0");
    psDepth = compile(shaders, "psDepth", "ps_5_0");
    psCenter = compile(shaders, "psCenter", "ps_5_0");
    psCenterDepth = compile(shaders, "psCenterDepth", "ps_5_0");
    psSemantic = compile(shaders, "psSemantic", "ps_5_0");
    csFloat = compile(shaders, "readFloat", "cs_5_0");
    csUint = compile(shaders, "readUint", "cs_5_0");
    csDepth = compile(shaders, "readDepth", "cs_5_0");
    csCenter = compile(shaders, "readCenter", "cs_5_0");
    csFloatResolved = compile(shaders, "readFloatResolved", "cs_5_0");
    csUintResolved = compile(shaders, "readUintResolved", "cs_5_0");
    csDepthResolved = compile(shaders, "readDepthResolved", "cs_5_0");
  }
  void submit(ID3D12GraphicsCommandList *list) {
    check(list->Close(), "close");
    ID3D12CommandList *lists[]{list};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++fenceValue), "signal");
    if (fence->GetCompletedValue() < fenceValue) {
      check(fence->SetEventOnCompletion(fenceValue, event), "fence event");
      require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "GPU timeout");
    }
    check(device->GetDeviceRemovedReason(), "GPU removed");
  }
};

D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc(Fixture &f, DXGI_FORMAT format,
                                                 UINT count, UINT quality, bool depth) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = f.graphicsRoot.Get();
  d.VS = {f.vs->GetBufferPointer(), f.vs->GetBufferSize()};
  auto &ps = depth ? f.psDepth : format == DXGI_FORMAT_R32_UINT ? f.psUint : f.psFloat;
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.DepthStencilState.DepthEnable = depth;
  d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  if (depth) d.DSVFormat = format;
  else { d.NumRenderTargets = 1; d.RTVFormats[0] = format; }
  d.SampleDesc = {count, quality};
  return d;
}
void semanticCase(Fixture &f) {
  auto imageDesc = texture(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto image = validTexture(f.device.Get(), imageDesc, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto desc = graphicsDesc(f, imageDesc.Format, 1, 0, false);
  desc.VS = {f.vsSemantic->GetBufferPointer(), f.vsSemantic->GetBufferSize()};
  desc.PS = {f.psSemantic->GetBufferPointer(), f.psSemantic->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "semantic PSO");
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> targets;
  check(f.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&targets)), "semantic RTV heap");
  auto target = targets->GetCPUDescriptorHandleForHeapStart();
  f.device->CreateRenderTargetView(image.Get(), nullptr, target);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  f.device->GetCopyableFootprints(&imageDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
  auto rbDesc = buffer(total);
  auto rbHeap = heap(D3D12_HEAP_TYPE_READBACK);
  ComPtr<ID3D12Resource> readback;
  check(f.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)), "semantic readback");
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(f.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&allocator)), "semantic allocator");
  check(f.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)), "semantic list");
  list->SetGraphicsRootSignature(f.graphicsRoot.Get());
  list->SetPipelineState(pso.Get());
  list->OMSetRenderTargets(1, &target, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
  D3D12_RECT scissor{0, 0, 4, 4};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = image.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  f.submit(list.Get());
  void *mapped = nullptr;
  D3D12_RANGE range{0, SIZE_T(total)};
  check(readback->Map(0, &range, &mapped), "semantic readback map");
  float rgba[4];
  std::memcpy(rgba, static_cast<const char *>(mapped) + footprint.Offset +
                    footprint.Footprint.RowPitch + 4 * sizeof(float), sizeof(rgba));
  D3D12_RANGE empty{0, 0};
  readback->Unmap(0, &empty);
  require(std::fabs(rgba[0] - .75f) < .0001f && std::fabs(rgba[1] - .5f) < .0001f &&
          std::fabs(rgba[2] - .125f) < .0001f && std::fabs(rgba[3] - .25f) < .0001f,
          "reordered case-insensitive semantic GPU output");
  std::printf("%s_SEMANTIC_GPU_PASS rgba=%.3f,%.3f,%.3f,%.3f\n", marker,
              rgba[0], rgba[1], rgba[2], rgba[3]);
}
void rejectedPSO(Fixture &f, DXGI_FORMAT format, UINT count, UINT quality, bool depth,
                 const char *step) {
  auto desc = graphicsDesc(f, format, count, quality, depth);
  ComPtr<ID3D12PipelineState> pso;
  require(FAILED(f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))), step);
  require(!pso, "invalid PSO returned an object");
}

void gpuCase(Fixture &f, DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat,
             UINT count, bool depth, UINT quality = 0, UINT sampleMask = UINT_MAX) {
  const bool integer = viewFormat == DXGI_FORMAT_R32_UINT;
  const auto flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL :
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto d = texture(resourceFormat, count, quality, flags);
  auto image = validTexture(f.device.Get(), d, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto resolvedDesc = texture(viewFormat, 1, 0, flags);
  auto resolved = validTexture(f.device.Get(), resolvedDesc, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  auto psoDesc = graphicsDesc(f, viewFormat, count, quality, depth);
  psoDesc.SampleMask = sampleMask;
  if (quality == standardPattern)
    psoDesc.VS = {f.vsEdge->GetBufferPointer(), f.vsEdge->GetBufferSize()};
  if (quality == centerPattern) {
    psoDesc.VS = {f.vsCenterEdge->GetBufferPointer(), f.vsCenterEdge->GetBufferSize()};
    auto &pixel = depth ? f.psCenterDepth : f.psCenter;
    psoDesc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
  }
  ComPtr<ID3D12PipelineState> graphics;
  check(f.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&graphics)), "graphics PSO");
  D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};
  computeDesc.pRootSignature = f.readRoot.Get();
  auto &cs = quality == centerPattern && !depth ? f.csCenter :
             depth ? f.csDepth : integer ? f.csUint : f.csFloat;
  computeDesc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> compute;
  check(f.device->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&compute)), "read PSO");
  auto &resolvedCS = depth ? f.csDepthResolved : integer ? f.csUintResolved : f.csFloatResolved;
  computeDesc.CS = {resolvedCS->GetBufferPointer(), resolvedCS->GetBufferSize()};
  ComPtr<ID3D12PipelineState> readResolved;
  check(f.device->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&readResolved)), "resolved read PSO");
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  auto readbackHeap = heap(D3D12_HEAP_TYPE_READBACK);
  auto outputDesc = buffer(quality == centerPattern && !depth ? 128 : 64);
  outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> output, readback;
  check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &outputDesc,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                          IID_PPV_ARGS(&output)), "read output");
  outputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
  check(f.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &outputDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)), "readback");
  ComPtr<ID3D12QueryHeap> centerQuery;
  ComPtr<ID3D12Resource> centerQueryReadback;
  if (quality == centerPattern) {
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    queryDesc.Count = 1;
    check(f.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&centerQuery)), "center query heap");
    auto queryBuffer = buffer(sizeof(UINT64));
    check(f.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &queryBuffer,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&centerQueryReadback)), "center query readback");
  }
  D3D12_DESCRIPTOR_HEAP_DESC viewHeapDesc{};
  viewHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  viewHeapDesc.NumDescriptors = 2;
  viewHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> views;
  check(f.device->CreateDescriptorHeap(&viewHeapDesc, IID_PPV_ARGS(&views)), "SRV heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = depth ? DXGI_FORMAT_R32_FLOAT : viewFormat;
  if (viewFormat == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  f.device->CreateShaderResourceView(image.Get(), &srv, views->GetCPUDescriptorHandleForHeapStart());
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = 1;
  auto resolvedSrv = views->GetCPUDescriptorHandleForHeapStart();
  resolvedSrv.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  f.device->CreateShaderResourceView(resolved.Get(), &srv, resolvedSrv);
  D3D12_DESCRIPTOR_HEAP_DESC targetHeapDesc{};
  targetHeapDesc.Type = depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  targetHeapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> targets;
  check(f.device->CreateDescriptorHeap(&targetHeapDesc, IID_PPV_ARGS(&targets)), "target heap");
  auto target = targets->GetCPUDescriptorHandleForHeapStart();
  if (depth) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = viewFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    f.device->CreateDepthStencilView(image.Get(), &dsv, target);
  } else {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = viewFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    f.device->CreateRenderTargetView(image.Get(), &rtv, target);
  }
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  check(f.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&allocator)), "allocator");
  check(f.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&list)), "list");
  list->SetGraphicsRootSignature(f.graphicsRoot.Get());
  list->SetPipelineState(graphics.Get());
  if (depth) {
    list->OMSetRenderTargets(0, nullptr, FALSE, &target);
    list->ClearDepthStencilView(target, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
  } else {
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const FLOAT clear[4]{0, 0, 0, 0};
    list->ClearRenderTargetView(target, clear, 0, nullptr);
  }
  D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
  D3D12_RECT scissor{0, 0, 4, 4};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (centerQuery) list->BeginQuery(centerQuery.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  list->DrawInstanced(quality == standardPattern || quality == centerPattern ? 6 : 3, 1, 0, 0);
  if (centerQuery) {
    list->EndQuery(centerQuery.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->ResolveQueryData(centerQuery.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1,
                           centerQueryReadback.Get(), 0);
  }
  transition(list.Get(), image.Get(), depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
                                          D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  list->ResolveSubresourceRegion(resolved.Get(), 0, 0, 0, image.Get(), 0, nullptr,
                                 viewFormat, D3D12_RESOLVE_MODE_MAX);
  transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->SetPipelineState(compute.Get());
  list->SetComputeRootSignature(f.readRoot.Get());
  ID3D12DescriptorHeap *heaps[]{views.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  list->SetPipelineState(readResolved.Get());
  auto resolvedGpu = views->GetGPUDescriptorHandleForHeapStart();
  resolvedGpu.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  list->SetComputeRootDescriptorTable(0, resolvedGpu);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), output.Get());
  f.submit(list.Get());
  void *mapped = nullptr;
  D3D12_RANGE range{0, quality == centerPattern && !depth ? SIZE_T(128) : SIZE_T(64)};
  check(readback->Map(0, &range, &mapped), "map");
  // The edge is exactly at pixel (1,1)'s x center. Standard 2/4/8 sample
  // positions have negative-x masks 0b10/0b0101/0b00111010 respectively.
  const UINT standardCoverage = quality == standardPattern ?
      count == 2 ? 0x2u : count == 4 ? 0x5u : 0x3au : (1u << count) - 1;
  if (quality == centerPattern) {
    void *queryMapped = nullptr;
    D3D12_RANGE queryRange{0, sizeof(UINT64)};
    check(centerQueryReadback->Map(0, &queryRange, &queryMapped), "center query map");
    const UINT64 samplesPassed = *static_cast<const UINT64 *>(queryMapped);
    centerQueryReadback->Unmap(0, nullptr);
    const UINT coveredSamples = sampleMask & ((1u << count) - 1u);
    require(samplesPassed == 8ull * std::popcount(coveredSamples), "center occlusion sample count");
    if (depth) {
      const auto *words = static_cast<const UINT *>(mapped);
      for (UINT sample = 0; sample < count; ++sample) {
        float sampleDepth;
        std::memcpy(&sampleDepth, &words[sample], sizeof(sampleDepth));
        require(std::fabs(sampleDepth - .375f) < .00001f, "center DSV sample depth");
      }
      float resolvedDepth;
      std::memcpy(&resolvedDepth, &words[8], sizeof(resolvedDepth));
      require(std::fabs(resolvedDepth - .375f) < .00001f, "center DSV MAX resolve");
      D3D12_RANGE empty{0, 0};
      readback->Unmap(0, &empty);
      std::printf("%s_CENTER_DEPTH_GPU_PASS count=%u\n", marker, count);
      return;
    }
    for (UINT sample = 0; sample < count; ++sample) {
      float coverage, centroid;
      const auto *words = static_cast<const UINT *>(mapped);
      std::memcpy(&coverage, &words[sample], sizeof(float));
      std::memcpy(&centroid, &words[16 + sample], sizeof(float));
      const bool covered = coveredSamples & (1u << sample);
      require(std::fabs(coverage - (covered ? 1.f : 0.f)) < .005f,
              "center masked sample Load");
      require(std::fabs(centroid - (covered ? .375f : 0.f)) < .005f,
              "center centroid interpolation");
    }
    float resolvedCoverage;
    std::memcpy(&resolvedCoverage, &static_cast<const UINT *>(mapped)[8], sizeof(float));
    require(std::fabs(resolvedCoverage - 1.f) < .005f,
            "center resolve MAX");
    D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);
    if (sampleMask == UINT_MAX)
      std::printf("%s_CENTER_GPU_PASS count=%u coverage=%u\n", marker, count, coveredSamples);
    else
      std::printf("%s_CENTER_MASK_GPU_PASS count=%u mask=%u\n", marker, count, coveredSamples);
    return;
  }
  for (UINT sample = 0; sample < count; ++sample) {
    UINT word = static_cast<const UINT *>(mapped)[sample];
    if (integer) require(word == sample + 1, "integer sample Load");
    else {
      float actual;
      std::memcpy(&actual, &word, sizeof(actual));
      const float expected = !(standardCoverage & (1u << sample)) ? 0.f :
                             depth ? .375f + sample / 32.f : (sample + 1) / 16.f;
      require(std::fabs(actual - expected) < (depth ? 0.00001f : 0.005f), "float/depth sample Load");
    }
  }
  UINT resolvedWord = static_cast<const UINT *>(mapped)[8];
  if (integer) require(resolvedWord == count, "integer MAX resolve");
  else {
    float actual;
    std::memcpy(&actual, &resolvedWord, sizeof(actual));
    UINT lastCovered = count - 1;
    while (!(standardCoverage & (1u << lastCovered))) --lastCovered;
    const float expected = depth ? .375f + lastCovered / 32.f : (lastCovered + 1) / 16.f;
    require(std::fabs(actual - expected) < (depth ? 0.00001f : 0.005f),
            "float/depth MAX resolve");
  }
  D3D12_RANGE empty{0, 0};
  readback->Unmap(0, &empty);
  std::printf("%s_GPU_CASE_PASS format=%u view=%u count=%u quality=%u depth=%u\n", marker,
              static_cast<UINT>(resourceFormat), static_cast<UINT>(viewFormat), count, quality, depth);
}
} // namespace

int main() {
  Fixture f;
  f.init();
  semanticCase(f);
  D3D12_HEAP_DESC rejectionHeapDesc{};
  rejectionHeapDesc.SizeInBytes = 8 * 1024 * 1024;
  rejectionHeapDesc.Properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Heap> rejectionHeap;
  check(f.device->CreateHeap(&rejectionHeapDesc, IID_PPV_ARGS(&rejectionHeap)), "rejection heap");
  // Query-only negative phase: never hand a known-invalid descriptor to Metal
  // until every query rejects it, so an old runtime fails without a native assertion.
  for (UINT count : {0u, 3u, 16u, 32u, UINT_MAX})
    require(qualityLevels(f.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, count,
                          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) == 0,
            "unsupported sample count advertised");
  for (UINT count : {2u, 4u, 8u})
    require(qualityLevels(f.device.Get(), DXGI_FORMAT_BC1_UNORM, count,
                          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) == 0,
            "compressed multisampling advertised");
  require(qualityLevels(f.device.Get(), static_cast<DXGI_FORMAT>(0x7fffffffu), 4,
                        D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) == 0,
          "unmapped format advertised");
  require(qualityLevels(f.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                        static_cast<D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS>(2)) == 0,
          "unknown quality-query flag accepted");
  require(qualityLevels(f.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                        D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE) == 0,
          "zero-count tiled query advertised or altered input");
  for (UINT count : {1u, 2u, 4u, 8u}) {
    require(qualityLevels(f.device.Get(), DXGI_FORMAT_UNKNOWN, count,
                          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) == 0,
            "UNKNOWN format advertised as multisample texture");
    require(qualityLevels(f.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, count,
                          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE) == 0,
            "tiled multisampling advertised without tiled resources");
  }
  for (UINT count : {0u, 3u, 16u, 32u, UINT_MAX})
    rejectedDescriptor(f.device.Get(), rejectionHeap.Get(), texture(DXGI_FORMAT_R8G8B8A8_UNORM, count, 0), "unsupported count passed null-output preflight");
  for (UINT count : {0u, 3u, 16u, 32u, UINT_MAX})
    rejectedPSO(f, DXGI_FORMAT_R8G8B8A8_UNORM, count, 0, false,
                "unsupported count passed PSO creation");
  rejectedDescriptor(f.device.Get(), rejectionHeap.Get(), texture(DXGI_FORMAT_BC1_UNORM, 4, 0), "compressed MSAA passed null-output preflight");
  rejectedDescriptor(f.device.Get(), rejectionHeap.Get(), texture(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), "MSAA UAV resource passed null-output preflight");
  rejectedPSO(f, DXGI_FORMAT_BC1_UNORM, 4, 0, false, "compressed MSAA PSO created");
  auto msaaBits = [&](DXGI_FORMAT format, bool render, bool load, bool resolve) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    check(f.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)),
          "multisample format support query");
    const auto bits = static_cast<UINT>(support.Support1);
    require(bool(bits & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET) == render &&
            bool(bits & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD) == load &&
            bool(bits & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) == resolve,
            "multisample format bits disagree with executable roles");
  };
  msaaBits(DXGI_FORMAT_R32_UINT, true, true, false);
  msaaBits(DXGI_FORMAT_D32_FLOAT, true, false, false);
  msaaBits(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, true, false, false);
  msaaBits(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, false, true, false);
  for (UINT count : {1u, 2u, 4u, 8u}) {
    // A depthless/colorless pipeline has no format to query, but still needs
    // an executable sample count. Do not infer this from DXGI_FORMAT_UNKNOWN.
    auto noTarget = graphicsDesc(f, DXGI_FORMAT_UNKNOWN, count, 0, false);
    noTarget.NumRenderTargets = 0;
    noTarget.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState> noTargetPSO;
    check(f.device->CreateGraphicsPipelineState(&noTarget, IID_PPV_ARGS(&noTargetPSO)),
          "colorless/depthless graphics PSO");
    const UINT colorLevels = qualityLevels(f.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                            count, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE);
    require(colorLevels >= 1, "ordinary color quality level missing");
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R32_UINT,
                               DXGI_FORMAT_R32_TYPELESS,
                               DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
      require(qualityLevels(f.device.Get(), format, count,
                            D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) >= 1,
              "supported texture family has no quality level");
    if (count == 1) {
      validTexture(f.device.Get(), texture(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0),
                   D3D12_RESOURCE_STATE_COMMON);
      require(qualityLevels(f.device.Get(), DXGI_FORMAT_BC1_UNORM, 1,
                            D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) >= 1,
              "ordinary compressed texture rejected at count one");
      validTexture(f.device.Get(), texture(DXGI_FORMAT_BC1_UNORM, 1, 0),
                   D3D12_RESOURCE_STATE_COMMON);
      rejectedDescriptor(f.device.Get(), rejectionHeap.Get(), texture(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1), "single-sample nonzero quality accepted");
      rejectedDescriptor(f.device.Get(), rejectionHeap.Get(), texture(DXGI_FORMAT_R8G8B8A8_UNORM, 1, centerPattern),
                         "single-sample center quality accepted");
      continue;
    }
    rejectedDescriptor(f.device.Get(), rejectionHeap.Get(), texture(DXGI_FORMAT_R8G8B8A8_UNORM, count, 1), "unsupported ordinary quality accepted");
    rejectedPSO(f, DXGI_FORMAT_R8G8B8A8_UNORM, count, 1, false,
                "unsupported ordinary quality PSO created");
    rejectedPSO(f, DXGI_FORMAT_R8G8B8A8_UNORM, count, centerPattern, false,
                "center sample-frequency pixel shader accepted");
    auto aaLine = graphicsDesc(f, DXGI_FORMAT_R8G8B8A8_UNORM, count, centerPattern, false);
    aaLine.VS = {f.vsCenterEdge->GetBufferPointer(), f.vsCenterEdge->GetBufferSize()};
    aaLine.PS = {f.psCenter->GetBufferPointer(), f.psCenter->GetBufferSize()};
    aaLine.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    aaLine.RasterizerState.AntialiasedLineEnable = TRUE;
    ComPtr<ID3D12PipelineState> invalidLine;
    require(FAILED(f.device->CreateGraphicsPipelineState(&aaLine, IID_PPV_ARGS(&invalidLine))) && !invalidLine,
            "center anti-aliased line accepted");
    // The fixed center pattern is outside the ordinary quality-level range.
    auto centerDesc = texture(DXGI_FORMAT_R8G8B8A8_UNORM, count, centerPattern,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    validTexture(f.device.Get(), centerDesc, D3D12_RESOURCE_STATE_COMMON);
    require(f.device->CreatePlacedResource(rejectionHeap.Get(), 0, &centerDesc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          __uuidof(ID3D12Resource), nullptr) == S_FALSE,
            "center placed null-output preflight");
    ComPtr<ID3D12Resource> centerPlaced;
    check(f.device->CreatePlacedResource(rejectionHeap.Get(), 0, &centerDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&centerPlaced)), "center placed resource");
    gpuCase(f, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
            count, false, centerPattern);
    gpuCase(f, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
            count, false, centerPattern, 0x55555555u);
    gpuCase(f, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT,
            count, true, centerPattern);
    gpuCase(f, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
            count, false, standardPattern);
    gpuCase(f, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, count, false);
    gpuCase(f, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, count, false);
    gpuCase(f, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, count, false);
    gpuCase(f, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, count, true);
    gpuCase(f, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, count, true);
    gpuCase(f, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, count, true);
  }
  std::printf("%s_PASS\n", marker);
  return 0;
}
