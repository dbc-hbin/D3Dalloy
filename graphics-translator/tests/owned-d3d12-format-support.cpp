#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_FORMAT_SUPPORT_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool ok, const char *step) { if (!ok) fail(step); }
bool has1(const D3D12_FEATURE_DATA_FORMAT_SUPPORT &s, D3D12_FORMAT_SUPPORT1 bit) {
  return (static_cast<unsigned>(s.Support1) & static_cast<unsigned>(bit)) != 0;
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = 1;
  p.VisibleNodeMask = 1;
  return p;
}
D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_UNKNOWN;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
D3D12_RESOURCE_DESC textureDesc(DXGI_FORMAT format, UINT width, UINT height, UINT mips,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = static_cast<UINT16>(mips);
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Flags = flags;
  return d;
}
ComPtr<ID3D12Resource> resource(ID3D12Device *device, D3D12_HEAP_TYPE type,
                                const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state) {
  ComPtr<ID3D12Resource> result;
  const auto properties = heap(type);
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&result)), "create-resource");
  return result;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}
struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;

  Context() {
    HMODULE module = GetModuleHandleW(L"d3d12.dll");
    require(module != nullptr, "d3d12-module");
    using BackendId = const char *(WINAPI *)();
    const auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned-backend");
    std::printf("OWNED_D3D12_FORMAT_SUPPORT_BACKEND id=%s", backend());
    std::puts("");
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
  }
  ~Context() { if (event) CloseHandle(event); }
  void submit() {
    check(list->Close(), "close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "fence-event");
      require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "fence-wait");
    }
    check(allocator->Reset(), "allocator-reset");
    check(list->Reset(allocator.Get(), nullptr), "list-reset");
  }
  D3D12_FEATURE_DATA_FORMAT_SUPPORT query(DXGI_FORMAT format) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT result{};
    result.Format = format;
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &result, sizeof(result)), "format-query");
    return result;
  }
};

// These are precisely the currently mapped DXGI_FORMAT cases in MTLQueryDXGIFormat:
// enum values 1..99, excluding R1_UNORM (66) and XR_BIAS (89), plus B4G4R4A4 (115).
// Query all of them, even if the current Metal family gives an individual format a zero mask.
void checkMappedMatrix(Context &c) {
  unsigned count = 0, textureCount = 0, bufferCount = 0, bcCount = 0;
  for (unsigned value = 1; value <= 115; ++value) {
    if (value == 66 || value == 89 || (value > 99 && value < 115)) continue;
    const auto format = static_cast<DXGI_FORMAT>(value);
    const auto support = c.query(format);
    require(support.Support1 != static_cast<D3D12_FORMAT_SUPPORT1>(~0u) &&
            support.Support2 != static_cast<D3D12_FORMAT_SUPPORT2>(~0u), "uninitialized-support-mask");
    const bool bc = (value >= 70 && value <= 84) || (value >= 94 && value <= 99);
    if (bc) {
      require(!has1(support, D3D12_FORMAT_SUPPORT1_BUFFER) &&
              !has1(support, D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
              !has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
              !has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE1D), "compressed-incompatible-bits");
      ++bcCount;
    }
    if (has1(support, D3D12_FORMAT_SUPPORT1_TEXTURE2D)) ++textureCount;
    if (has1(support, D3D12_FORMAT_SUPPORT1_BUFFER)) ++bufferCount;
    ++count;
  }
  for (DXGI_FORMAT format : {DXGI_FORMAT_R1_UNORM, DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
                             DXGI_FORMAT_NV12, DXGI_FORMAT_FORCE_UINT}) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT bad{};
    bad.Format = format;
    bad.Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(~0u);
    bad.Support2 = static_cast<D3D12_FORMAT_SUPPORT2>(~0u);
    require(FAILED(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &bad, sizeof(bad))) &&
            bad.Support1 == D3D12_FORMAT_SUPPORT1_NONE && bad.Support2 == D3D12_FORMAT_SUPPORT2_NONE,
            "unmapped-format-result");
  }
  std::printf("OWNED_D3D12_FORMAT_SUPPORT_MATRIX formats=%u texture2d=%u buffer=%u compressed=%u\n",
              count, textureCount, bufferCount, bcCount);
}

// Upload distinct bytes to each level. This proves shader reads of level 1 rather than
// merely checking whether the runtime accepted MipLevels=2 in a resource description.
ComPtr<ID3D12Resource> uploadRgbaOrR32(Context &c, DXGI_FORMAT format,
                                       const std::array<std::uint8_t, 4> &level0,
                                       const std::array<std::uint8_t, 4> &level1, UINT mipCount = 2) {
  const auto desc = textureDesc(format, 4, 4, mipCount);
  auto texture = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
  UINT64 bytes = 0;
  c.device->GetCopyableFootprints(&desc, 0, mipCount, 0, footprints.data(), nullptr, nullptr, &bytes);
  require(bytes && bytes != ~UINT64(0), "upload-footprints");
  auto upload = resource(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, bufferDesc(bytes),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  std::uint8_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "upload-map");
  std::memset(mapped, 0, static_cast<size_t>(bytes));
  for (UINT level = 0; level < mipCount; ++level) {
    const auto &f = footprints[level];
    const auto &pixel = level ? level1 : level0;
    for (UINT y = 0; y < f.Footprint.Height; ++y)
      for (UINT x = 0; x < f.Footprint.Width; ++x)
        std::memcpy(mapped + f.Offset + y * f.Footprint.RowPitch + x * 4, pixel.data(), 4);
  }
  upload->Unmap(0, nullptr);
  for (UINT level = 0; level < mipCount; ++level) {
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprints[level];
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = level;
    c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // The queued upload must remain alive until the fence completes.
  c.submit();
  return texture;
}

ComPtr<ID3D12DescriptorHeap> srvHeap(Context &c) {
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 2;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> result;
  check(c.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&result)), "srv-heap");
  return result;
}
void srv(Context &c, ID3D12DescriptorHeap *heap, UINT slot, ID3D12Resource *texture,
         DXGI_FORMAT format, UINT mipCount, UINT plane = 0) {
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Texture2D.MipLevels = mipCount;
  view.Texture2D.PlaneSlice = plane;
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += slot * c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  c.device->CreateShaderResourceView(texture, &view, cpu);
}

const char *kShader = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut vsMain(uint id : SV_VertexID) {
  float2 positions[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
  VSOut result; result.pos = float4(positions[id], 0, 1); return result;
}
#ifdef MIPS
Texture2D<uint4> integerMip : register(t0);
Texture2D<float4> normalizedMip : register(t1);
float4 psMain() : SV_Target { return float4(integerMip.Load(int3(0,0,1)).x,
                                      normalizedMip.Load(int3(0,0,1)).rgb); }
#endif
#ifdef DEPTH
Texture2D<float> depthPlane : register(t0);
Texture2D<uint4> stencilPlane : register(t1);
float4 psMain() : SV_Target { return float4(depthPlane.Load(int3(0,0,0)),
                                      stencilPlane.Load(int3(0,0,0)).g, 0, 1); }
#endif
#ifdef DEPTH_SCALAR
Texture2D<float> depth32 : register(t0);
Texture2D<float> depth16 : register(t1);
SamplerComparisonState comparison : register(s0);
float4 psMain() : SV_Target { return float4(depth32.Load(int3(0,0,0)),
                                      depth16.Load(int3(0,0,0)),
                                      depth32.SampleCmpLevelZero(comparison, float2(0.5,0.5), 0.5),
                                      depth16.SampleCmpLevelZero(comparison, float2(0.5,0.5), 0.5)); }
#endif
#ifdef CAST
Texture2D<float4> linearView : register(t0);
Texture2D<float4> srgbView : register(t1);
float4 psMain() : SV_Target { return float4(linearView.Load(int3(0,0,0)).r,
                                      srgbView.Load(int3(0,0,0)).r, 0, 1); }
#endif
#ifdef CAST_UINT
Texture2D<uint4> originalView : register(t0);
Texture2D<float4> reinterpretedView : register(t1);
float4 psMain() : SV_Target { return float4(originalView.Load(int3(0,0,0)).r,
                                      reinterpretedView.Load(int3(0,0,0)).r, 0, 1); }
#endif
)";

std::array<float, 4> drawAndRead(Context &c, ID3D12DescriptorHeap *views, const char *define) {
  const D3D_SHADER_MACRO macros[] = {{define, "1"}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> vs, ps, errors;
  check(D3DCompile(kShader, std::strlen(kShader), nullptr, macros, nullptr, "vsMain", "vs_5_0",
                   D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, &errors), "compile-vs");
  const HRESULT psHr = D3DCompile(kShader, std::strlen(kShader), nullptr, macros, nullptr, "psMain", "ps_5_0",
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, &errors);
  if (FAILED(psHr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("compile-ps", psHr);
  }
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 2;
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameter.DescriptorTable.NumDescriptorRanges = 1;
  parameter.DescriptorTable.pDescriptorRanges = &range;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 1;
  rootDesc.pParameters = &parameter;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  D3D12_STATIC_SAMPLER_DESC comparison{};
  comparison.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
  comparison.AddressU = comparison.AddressV = comparison.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  comparison.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  comparison.MaxAnisotropy = 1;
  comparison.MaxLOD = D3D12_FLOAT32_MAX;
  comparison.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  rootDesc.NumStaticSamplers = 1;
  rootDesc.pStaticSamplers = &comparison;
  ComPtr<ID3DBlob> serialized;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
        "serialize-root");
  ComPtr<ID3D12RootSignature> root;
  check(c.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "create-root");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = root.Get();
  psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.SampleMask = UINT_MAX;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.RasterizerState.DepthClipEnable = TRUE;
  psoDesc.DepthStencilState.DepthEnable = FALSE;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  psoDesc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "create-pso");

  const auto targetDesc = textureDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto target = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, targetDesc,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(c.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "rtv-heap");
  c.device->CreateRenderTargetView(target.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 totalBytes = 0;
  c.device->GetCopyableFootprints(&targetDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
  require(totalBytes >= 16 && totalBytes != ~UINT64(0), "target-footprint");
  auto readback = resource(c.device.Get(), D3D12_HEAP_TYPE_READBACK, bufferDesc(totalBytes),
                           D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12DescriptorHeap *heaps[] = {views};
  c.list->SetDescriptorHeaps(1, heaps);
  c.list->SetGraphicsRootSignature(root.Get());
  c.list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  c.list->SetPipelineState(pso.Get());
  D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  D3D12_RECT scissor{0, 0, 1, 1};
  c.list->RSSetViewports(1, &viewport);
  c.list->RSSetScissorRects(1, &scissor);
  auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c.list->DrawInstanced(3, 1, 0, 0);
  transition(c.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = target.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  c.submit();
  float *mapped = nullptr;
  D3D12_RANGE read{0, 16};
  check(readback->Map(0, &read, reinterpret_cast<void **>(&mapped)), "readback-map");
  std::array<float, 4> result{};
  std::memcpy(result.data(), mapped, 16);
  D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
  return result;
}
void expectNear(float actual, float expected, const char *step) {
  if (!std::isfinite(actual) || std::fabs(actual - expected) > 0.002f) {
    std::fprintf(stderr, "OWNED_D3D12_FORMAT_SUPPORT_MISMATCH step=%s expected=%g actual=%g\n",
                 step, double(expected), double(actual));
    fail(step);
  }
}


void testSrgbUavContract(Context &c) {
  for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                             DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
    const auto support = c.query(format);
    require(!has1(support, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
            !(static_cast<UINT>(support.Support2) &
              (D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)),
            "srgb-must-not-advertise-typed-uav");
  }
  const auto desc = textureDesc(DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 4, 1,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto texture = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc,
                          D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  c.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto upload = resource(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, bufferDesc(total),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  std::uint8_t *initial = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&initial)), "srgb-upload-map");
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 16; ++x)
      initial[footprint.Offset + y * footprint.Footprint.RowPitch + x] =
          static_cast<std::uint8_t>(17 + y * 23 + x);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION image{}, buffer{};
  image.pResource = texture.Get();
  image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  buffer.pResource = upload.Get();
  buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  buffer.PlacedFootprint = footprint;
  c.list->CopyTextureRegion(&image, 0, 0, 0, &buffer, nullptr);
  transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> cpuHeap, gpuHeap;
  check(c.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cpuHeap)), "srgb-cpu-heap");
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(c.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gpuHeap)), "srgb-gpu-heap");
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  c.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav,
                                      cpuHeap->GetCPUDescriptorHandleForHeapStart());
  c.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav,
                                      gpuHeap->GetCPUDescriptorHandleForHeapStart());
  ID3D12DescriptorHeap *heaps[] = {gpuHeap.Get()};
  c.list->SetDescriptorHeaps(1, heaps);
  const UINT color[4] = {255, 0, 0, 255};
  c.list->ClearUnorderedAccessViewUint(gpuHeap->GetGPUDescriptorHandleForHeapStart(),
                                      cpuHeap->GetCPUDescriptorHandleForHeapStart(),
                                      texture.Get(), color, 0, nullptr);
  transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto readback = resource(c.device.Get(), D3D12_HEAP_TYPE_READBACK, bufferDesc(total),
                           D3D12_RESOURCE_STATE_COPY_DEST);
  buffer.pResource = readback.Get();
  c.list->CopyTextureRegion(&buffer, 0, 0, 0, &image, nullptr);
  c.submit();
  std::uint8_t *actual = nullptr;
  D3D12_RANGE read{0, static_cast<SIZE_T>(total)};
  check(readback->Map(0, &read, reinterpret_cast<void **>(&actual)), "srgb-readback-map");
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 16; ++x)
      require(actual[footprint.Offset + y * footprint.Footprint.RowPitch + x] ==
              static_cast<std::uint8_t>(17 + y * 23 + x), "srgb-invalid-uav-must-preserve-data");
  D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
  require(c.device->GetDeviceRemovedReason() == S_OK, "srgb-uav-device-preserved");
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_SRGB_UAV_PASS formats=RGBA8,BGRA8 bytes=64 preserved=1");
}

void testMipLoad(Context &c) {
  const auto integer = uploadRgbaOrR32(c, DXGI_FORMAT_R32_UINT, {77, 0, 0, 0}, {123, 0, 0, 0});
  const auto typeless = uploadRgbaOrR32(c, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                       {10, 20, 30, 40}, {31, 37, 41, 43});
  auto views = srvHeap(c);
  srv(c, views.Get(), 0, integer.Get(), DXGI_FORMAT_R32_UINT, 2);
  srv(c, views.Get(), 1, typeless.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 2);
  const auto result = drawAndRead(c, views.Get(), "MIPS");
  expectNear(result[0], 123, "mip-integer-shader-load");
  expectNear(result[1], 31.0f / 255.0f, "mip-typeless-r");
  expectNear(result[2], 37.0f / 255.0f, "mip-typeless-g");
  expectNear(result[3], 41.0f / 255.0f, "mip-typeless-b");
  require(has1(c.query(DXGI_FORMAT_R32_UINT), D3D12_FORMAT_SUPPORT1_MIP), "mip-integer-report");
  require(has1(c.query(DXGI_FORMAT_R8G8B8A8_TYPELESS), D3D12_FORMAT_SUPPORT1_MIP), "mip-typeless-report");
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_MIP_GPU_PASS formats=R32_UINT,R8G8B8A8_TYPELESS mip=1");
}

void testDepthAspects(Context &c) {
  const auto desc = textureDesc(DXGI_FORMAT_R32G8X24_TYPELESS, 4, 4, 1,
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
  auto texture = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(c.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap)), "dsv-heap");
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  c.device->CreateDepthStencilView(texture.Get(), &dsv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
  c.list->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(),
                                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.625f, 0x5a, 0, nullptr);
  transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  c.submit();
  auto views = srvHeap(c);
  srv(c, views.Get(), 0, texture.Get(), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 1, 0);
  srv(c, views.Get(), 1, texture.Get(), DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 1, 1);
  const auto result = drawAndRead(c, views.Get(), "DEPTH");
  expectNear(result[0], 0.625f, "depth-aspect-shader-read");
  expectNear(result[1], 90.0f, "stencil-aspect-shader-read");
  for (DXGI_FORMAT f : {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT})
    require(has1(c.query(f), D3D12_FORMAT_SUPPORT1_SHADER_LOAD) &&
            has1(c.query(f), D3D12_FORMAT_SUPPORT1_MIP), "depth-aspect-report");
  const auto combined = c.query(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
  require(!has1(combined, D3D12_FORMAT_SUPPORT1_SHADER_LOAD) &&
          has1(combined, D3D12_FORMAT_SUPPORT1_MIP), "combined-dsv-report");
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_DEPTH_GPU_PASS planes=depth,stencil typed_combined=dsv_only mip=1");
}

void testSinglePlaneDepthViews(Context &c) {
  const UINT forbidden = D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                         D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                         D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                         D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
                         D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON |
                         D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
  for (DXGI_FORMAT format : {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM,
                             DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT})
    require(!(static_cast<UINT>(c.query(format).Support1) & forbidden), "typed-dsv-not-shader-readable");
  for (DXGI_FORMAT format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_UNORM,
                             DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS}) {
    const auto support = c.query(format);
    require(has1(support, D3D12_FORMAT_SUPPORT1_SHADER_LOAD) &&
            has1(support, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON),
            "depth-alias-shader-and-comparison-report");
  }

  const DXGI_FORMAT storageFormats[] = {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM};
  const DXGI_FORMAT typelessFormats[] = {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R16_TYPELESS};
  const DXGI_FORMAT shaderFormats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_UNORM};
  const float depths[] = {0.625f, 0.375f};
  ComPtr<ID3D12Resource> typeless[2];
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heapDesc.NumDescriptors = 2;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(c.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap)), "single-plane-dsv-heap");
  auto dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  const UINT dsvStride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  for (UINT i = 0; i < 2; ++i) {
    typeless[i] = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                           textureDesc(typelessFormats[i], 4, 4, 1,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                           D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = storageFormats[i];
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    c.device->CreateDepthStencilView(typeless[i].Get(), &dsv, dsvHandle);
    c.list->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, depths[i], 0, 0, nullptr);
    dsvHandle.ptr += dsvStride;
    transition(c.list.Get(), typeless[i].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  c.submit();

  auto views = srvHeap(c);
  for (UINT i = 0; i < 2; ++i)
    srv(c, views.Get(), i, typeless[i].Get(), shaderFormats[i], 1);
  const auto valid = drawAndRead(c, views.Get(), "DEPTH_SCALAR");
  expectNear(valid[0], depths[0], "r32-depth-load");
  expectNear(valid[1], depths[1], "r16-depth-load");
  expectNear(valid[2], 1, "r32-depth-comparison");
  expectNear(valid[3], 0, "r16-depth-comparison");
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_SINGLE_DEPTH_GPU_PASS typed=dsv_only aliases=R32_FLOAT,R16_UNORM load=1 comparison=1");
}

void testFullyTypedCast(Context &c) {
  auto texture = uploadRgbaOrR32(c, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 {128, 64, 32, 255}, {0, 0, 0, 0}, 1);
  auto views = srvHeap(c);
  srv(c, views.Get(), 0, texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
  srv(c, views.Get(), 1, texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1);
  const auto result = drawAndRead(c, views.Get(), "CAST");
  expectNear(result[0], 128.0f / 255.0f, "typed-linear-view");
  // sRGB decode of 128/255 in an otherwise identical typed resource.
  expectNear(result[1], 0.215861f, "typed-srgb-view");
  require(has1(c.query(DXGI_FORMAT_R8G8B8A8_UNORM), D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
          "fully-typed-cast-report");
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_CAST_GPU_PASS typed_resource=R8G8B8A8_UNORM views=linear,srgb");
}

ComPtr<ID3D12Resource> uploadCastPattern(Context &c, DXGI_FORMAT format,
                                         const std::uint8_t *pattern, UINT patternBytes) {
  const auto desc = textureDesc(format, 4, 4, 1);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  c.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
  require(patternBytes && rowBytes && rowBytes % patternBytes == 0, "cast-pattern-footprint");
  auto texture = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = resource(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, bufferDesc(total),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  std::uint8_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "cast-pattern-map");
  for (UINT y = 0; y < rows; ++y)
    for (UINT64 byte = 0; byte < rowBytes; byte += patternBytes)
      std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + byte,
                  pattern, patternBytes);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;
  dst.pResource = texture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(c.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  c.submit();
  return texture;
}

void testOtherCastGroups(Context &c) {
  const std::uint8_t halfOne[] = {0x00, 0x3c};
  auto r16 = uploadCastPattern(c, DXGI_FORMAT_R16_UINT, halfOne, sizeof(halfOne));
  auto views = srvHeap(c);
  srv(c, views.Get(), 0, r16.Get(), DXGI_FORMAT_R16_UINT, 1);
  srv(c, views.Get(), 1, r16.Get(), DXGI_FORMAT_R16_FLOAT, 1);
  auto result = drawAndRead(c, views.Get(), "CAST_UINT");
  expectNear(result[0], 15360, "r16-uint-source");
  expectNear(result[1], 1, "r16-half-cast");
  require(has1(c.query(DXGI_FORMAT_R16_UINT), D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
          "r16-cast-report");

  const std::uint8_t floatOne[] = {0, 0, 0x80, 0x3f};
  auto rgba32 = uploadCastPattern(c, DXGI_FORMAT_R32G32B32A32_UINT, floatOne, sizeof(floatOne));
  views = srvHeap(c);
  srv(c, views.Get(), 0, rgba32.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 1);
  srv(c, views.Get(), 1, rgba32.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 1);
  result = drawAndRead(c, views.Get(), "CAST_UINT");
  expectNear(result[0], 1065353216.0f, "rgba32-uint-source");
  expectNear(result[1], 1, "rgba32-float-cast");
  require(has1(c.query(DXGI_FORMAT_R32G32B32A32_UINT), D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
          "rgba32-cast-report");

  // A solid BC1 block with R5=16, G6=B5=0; both endpoints and all selectors equal.
  const std::uint8_t bc1Block[] = {0, 0x80, 0, 0x80, 0, 0, 0, 0};
  auto bc1 = uploadCastPattern(c, DXGI_FORMAT_BC1_UNORM, bc1Block, sizeof(bc1Block));
  views = srvHeap(c);
  srv(c, views.Get(), 0, bc1.Get(), DXGI_FORMAT_BC1_UNORM, 1);
  srv(c, views.Get(), 1, bc1.Get(), DXGI_FORMAT_BC1_UNORM_SRGB, 1);
  result = drawAndRead(c, views.Get(), "CAST");
  expectNear(result[0], 16.0f / 31.0f, "bc1-linear-source");
  const float srgb = (16.0f / 31.0f + 0.055f) / 1.055f;
  expectNear(result[1], std::pow(srgb, 2.4f), "bc1-srgb-cast");
  require(has1(c.query(DXGI_FORMAT_BC1_UNORM), D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
          "bc1-cast-report");
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_CAST_GROUPS_GPU_PASS r16=uint_to_half rgba32=uint_to_float bc1=linear_to_srgb");
}

} // namespace

int main() {
  Context c;
  checkMappedMatrix(c);
  testSrgbUavContract(c);
  testDepthAspects(c);
  testSinglePlaneDepthViews(c);
  testFullyTypedCast(c);
  testOtherCastGroups(c);
  testMipLoad(c);
  std::puts("OWNED_D3D12_FORMAT_SUPPORT_PASS");
  return 0;
}
