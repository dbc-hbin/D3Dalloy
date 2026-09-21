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

using Microsoft::WRL::ComPtr;

namespace {

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "COPY_RESOLVE_BARRIER_FAIL step=%s hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
  std::exit(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  if (!module)
    fail("GetModuleHandleW(d3d12)", HRESULT_FROM_WIN32(GetLastError()));
  FARPROC raw = GetProcAddress(module, "OwnedGraphicsBackendId");
  if (!raw)
    fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
  using BackendId = const char *(WINAPI *)();
  const auto backendId = reinterpret_cast<BackendId>(raw);
  const char *id = backendId();
  if (!id || std::strcmp(id, "owned-graphics-d3d12") != 0)
    fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_COPY_RESOLVE_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES value{};
  value.Type = type;
  value.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  value.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  value.CreationNodeMask = 1;
  value.VisibleNodeMask = 1;
  return value;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size) {
  D3D12_RESOURCE_DESC value{};
  value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  value.Width = size;
  value.Height = 1;
  value.DepthOrArraySize = 1;
  value.MipLevels = 1;
  value.Format = DXGI_FORMAT_UNKNOWN;
  value.SampleDesc.Count = 1;
  value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return value;
}

D3D12_RESOURCE_DESC textureDesc(UINT width, UINT height, UINT samples, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC value{};
  value.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  value.Width = width;
  value.Height = height;
  value.DepthOrArraySize = 1;
  value.MipLevels = 1;
  value.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  value.SampleDesc.Count = samples;
  value.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  value.Flags = flags;
  return value;
}

D3D12_RESOURCE_DESC depthStencilDesc(UINT width, UINT height, UINT samples) {
  D3D12_RESOURCE_DESC value = textureDesc(width, height, samples,
                                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
  value.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  return value;
}

ComPtr<ID3DBlob> compile(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("D3DCompile", hr);
  }
  return code;
}

void transition(ID3D12GraphicsCommandList1 *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}

} // namespace

int main() {
  requireOwnedBackend();
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  ComPtr<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "CreateCommandAllocator");
  ComPtr<ID3D12GraphicsCommandList1> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)), "CreateCommandList");

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.NumDescriptors = 4;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap");
  const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE msaaRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE resolvedRtv{msaaRtv.ptr + rtvStride};
  D3D12_CPU_DESCRIPTOR_HANDLE predicateFalseRtv{msaaRtv.ptr + 2 * rtvStride};
  D3D12_CPU_DESCRIPTOR_HANDLE predicateTrueRtv{msaaRtv.ptr + 3 * rtvStride};

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.NumDescriptors = 2;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)),
        "Create depth descriptor heap");
  const UINT dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  D3D12_CPU_DESCRIPTOR_HANDLE sourceDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE destinationDsv{sourceDsv.ptr + dsvStride};

  const D3D12_HEAP_PROPERTIES defaultHeap = heap(D3D12_HEAP_TYPE_DEFAULT);
  const D3D12_RESOURCE_DESC msaaDesc = textureDesc(4, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  const D3D12_RESOURCE_DESC resolvedDesc = textureDesc(8, 8, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  D3D12_CLEAR_VALUE clearValue{};
  clearValue.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  clearValue.Color[0] = 99.0f;
  clearValue.Color[1] = 98.0f;
  clearValue.Color[2] = 97.0f;
  clearValue.Color[3] = 96.0f;
  ComPtr<ID3D12Resource> msaa;
  ComPtr<ID3D12Resource> predicateFalseMsaa;
  ComPtr<ID3D12Resource> predicateTrueMsaa;
  ComPtr<ID3D12Resource> predicateSnapshotMsaa;
  ComPtr<ID3D12Resource> resolved;
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &msaaDesc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&msaa)), "Create MSAA texture");
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &msaaDesc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&predicateFalseMsaa)), "Create false-predicate MSAA texture");
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &msaaDesc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&predicateTrueMsaa)), "Create true-predicate MSAA texture");
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &msaaDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&predicateSnapshotMsaa)), "Create snapshot MSAA texture");
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resolvedDesc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                                        IID_PPV_ARGS(&resolved)), "Create resolve texture");
  device->CreateRenderTargetView(msaa.Get(), nullptr, msaaRtv);
  device->CreateRenderTargetView(resolved.Get(), nullptr, resolvedRtv);
  device->CreateRenderTargetView(predicateFalseMsaa.Get(), nullptr, predicateFalseRtv);
  device->CreateRenderTargetView(predicateTrueMsaa.Get(), nullptr, predicateTrueRtv);
  const FLOAT guard[4] = {99.0f, 98.0f, 97.0f, 96.0f};
  const FLOAT falseGuard[4] = {7.0f, 17.0f, 27.0f, 37.0f};
  const FLOAT trueGuard[4] = {8.0f, 18.0f, 28.0f, 38.0f};
  list->ClearRenderTargetView(resolvedRtv, guard, 0, nullptr);
  list->ClearRenderTargetView(predicateFalseRtv, falseGuard, 0, nullptr);
  list->ClearRenderTargetView(predicateTrueRtv, trueGuard, 0, nullptr);

  const D3D12_RESOURCE_DESC depthMsaaDesc = depthStencilDesc(4, 4, 4);
  const D3D12_RESOURCE_DESC depthResolvedDesc = depthStencilDesc(8, 8, 1);
  D3D12_CLEAR_VALUE depthSourceClear{};
  depthSourceClear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  depthSourceClear.DepthStencil.Depth = 1.0f;
  depthSourceClear.DepthStencil.Stencil = 0;
  D3D12_CLEAR_VALUE depthDestinationClear{};
  depthDestinationClear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  depthDestinationClear.DepthStencil.Depth = 0.75f;
  depthDestinationClear.DepthStencil.Stencil = 0xa5;
  ComPtr<ID3D12Resource> depthMsaa;
  ComPtr<ID3D12Resource> depthResolved;
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthMsaaDesc,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthSourceClear,
                                        IID_PPV_ARGS(&depthMsaa)), "Create depth MSAA texture");
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthResolvedDesc,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthDestinationClear,
                                        IID_PPV_ARGS(&depthResolved)), "Create depth resolve texture");
  D3D12_DEPTH_STENCIL_VIEW_DESC sourceDsvDesc{};
  sourceDsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  sourceDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
  device->CreateDepthStencilView(depthMsaa.Get(), &sourceDsvDesc, sourceDsv);
  D3D12_DEPTH_STENCIL_VIEW_DESC destinationDsvDesc{};
  destinationDsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  destinationDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(depthResolved.Get(), &destinationDsvDesc, destinationDsv);
  list->ClearDepthStencilView(sourceDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              1.0f, 0, 0, nullptr);
  list->ClearDepthStencilView(destinationDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                              0.75f, 0xa5, 0, nullptr);

  const D3D12_HEAP_PROPERTIES uploadHeap = heap(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC predicateUploadDesc = bufferDesc(2 * sizeof(UINT64));
  ComPtr<ID3D12Resource> predicateUpload;
  check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &predicateUploadDesc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(&predicateUpload)), "Create predicate upload");
  UINT64 *predicateValues = nullptr;
  D3D12_RANGE noRead{0, 0};
  check(predicateUpload->Map(0, &noRead, reinterpret_cast<void **>(&predicateValues)), "Map predicate upload");
  predicateValues[0] = 0;
  predicateValues[1] = 1;
  D3D12_RANGE predicateWrite{0, 2 * sizeof(UINT64)};
  predicateUpload->Unmap(0, &predicateWrite);

  const D3D12_RESOURCE_DESC predicateDesc = bufferDesc(sizeof(UINT64));
  ComPtr<ID3D12Resource> predicate;
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &predicateDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&predicate)), "Create predicate buffer");

  const char *shader = R"(
struct VSOut { float4 position : SV_Position; };
VSOut vs(uint id : SV_VertexID) {
  float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
  VSOut result; result.position = float4(p[id], 0, 1); return result;
}
float4 ps(uint sample : SV_SampleIndex) : SV_Target0 {
  return float4(sample + 1, sample + 10, sample * 2 + 20, 40 - sample);
}
float psDepth(uint sample : SV_SampleIndex) : SV_Depth {
  const float depth[4] = {0.1, 0.3, 0.6, 0.9};
  return depth[sample];
}
)";
  ComPtr<ID3DBlob> vs = compile(shader, "vs", "vs_5_0");
  ComPtr<ID3DBlob> ps = compile(shader, "ps", "ps_5_0");
  ComPtr<ID3DBlob> depthPs = compile(shader, "psDepth", "ps_5_0");
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> rootBlob;
  ComPtr<ID3DBlob> rootErrors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors),
        "D3D12SerializeRootSignature");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "CreateRootSignature");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = root.Get();
  psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.SampleMask = UINT_MAX;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.DepthStencilState.DepthEnable = FALSE;
  psoDesc.DepthStencilState.StencilEnable = FALSE;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  psoDesc.SampleDesc.Count = 4;
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC depthPsoDesc = psoDesc;
  depthPsoDesc.PS = {depthPs->GetBufferPointer(), depthPs->GetBufferSize()};
  depthPsoDesc.NumRenderTargets = 0;
  depthPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
  depthPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  depthPsoDesc.DepthStencilState.DepthEnable = TRUE;
  depthPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  depthPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  depthPsoDesc.DepthStencilState.StencilEnable = TRUE;
  depthPsoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  depthPsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  depthPsoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  depthPsoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  depthPsoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
  depthPsoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  depthPsoDesc.DepthStencilState.BackFace = depthPsoDesc.DepthStencilState.FrontFace;
  ComPtr<ID3D12PipelineState> depthPsos[4];
  for (UINT sample = 0; sample < 4; ++sample) {
    depthPsoDesc.SampleMask = 1u << sample;
    check(device->CreateGraphicsPipelineState(&depthPsoDesc, IID_PPV_ARGS(&depthPsos[sample])),
          "Create depth pipeline state");
  }

  list->SetGraphicsRootSignature(root.Get());
  list->SetPipelineState(pso.Get());
  list->OMSetRenderTargets(1, &msaaRtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
  D3D12_RECT scissor{0, 0, 4, 4};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), msaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST);

  D3D12_RECT sourceRect{0, 0, 2, 2};
  list->ResolveSubresourceRegion(resolved.Get(), 0, 1, 1, msaa.Get(), 0, &sourceRect,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOLVE_MODE_AVERAGE);
  list->ResolveSubresourceRegion(resolved.Get(), 0, 3, 1, msaa.Get(), 0, &sourceRect,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOLVE_MODE_MIN);
  list->ResolveSubresourceRegion(resolved.Get(), 0, 5, 1, msaa.Get(), 0, &sourceRect,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOLVE_MODE_MAX);

  transition(list.Get(), msaa.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), predicateFalseMsaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_DEST);
  transition(list.Get(), predicateTrueMsaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION msaaSource{};
  msaaSource.pResource = msaa.Get();
  msaaSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION predicateDestination{};
  predicateDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

  list->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), 0, sizeof(UINT64));
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
  list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  predicateDestination.pResource = predicateFalseMsaa.Get();
  list->CopyTextureRegion(&predicateDestination, 0, 0, 0, &msaaSource, nullptr);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), sizeof(UINT64), sizeof(UINT64));
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
  list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  predicateDestination.pResource = predicateTrueMsaa.Get();
  list->CopyTextureRegion(&predicateDestination, 0, 0, 0, &msaaSource, nullptr);

  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_PREDICATION,
             D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), 0, sizeof(UINT64));
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PREDICATION);
  predicateDestination.pResource = predicateSnapshotMsaa.Get();
  list->CopyTextureRegion(&predicateDestination, 0, 0, 0, &msaaSource, nullptr);
  transition(list.Get(), predicateSnapshotMsaa.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  D3D12_RECT snapshotPixel{0, 0, 1, 1};
  list->ResolveSubresourceRegion(resolved.Get(), 0, 7, 0, predicateSnapshotMsaa.Get(), 0,
                                 &snapshotPixel, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                 D3D12_RESOLVE_MODE_AVERAGE);
  list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ResolveSubresourceRegion(resolved.Get(), 0, 7, 2, predicateSnapshotMsaa.Get(), 0,
                                 &snapshotPixel, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                 D3D12_RESOLVE_MODE_AVERAGE);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  list->SetGraphicsRootSignature(root.Get());
  list->OMSetRenderTargets(0, nullptr, FALSE, &sourceDsv);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const UINT stencilValues[4] = {10, 20, 30, 40};
  for (UINT sample = 0; sample < 4; ++sample) {
    list->SetPipelineState(depthPsos[sample].Get());
    list->OMSetStencilRef(stencilValues[sample]);
    list->DrawInstanced(3, 1, 0, 0);
  }
  transition(list.Get(), depthMsaa.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
             D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  transition(list.Get(), depthResolved.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
             D3D12_RESOURCE_STATE_RESOLVE_DEST);
  D3D12_RECT depthRect{0, 0, 2, 2};
  list->ResolveSubresourceRegion(depthResolved.Get(), 0, 1, 1, depthMsaa.Get(), 0, &depthRect,
                                 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOLVE_MODE_MIN);
  list->ResolveSubresourceRegion(depthResolved.Get(), 0, 4, 1, depthMsaa.Get(), 0, &depthRect,
                                 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOLVE_MODE_MAX);
  list->ResolveSubresourceRegion(depthResolved.Get(), 1, 1, 4, depthMsaa.Get(), 1, &depthRect,
                                 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOLVE_MODE_MIN);
  list->ResolveSubresourceRegion(depthResolved.Get(), 1, 4, 4, depthMsaa.Get(), 1, &depthRect,
                                 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOLVE_MODE_MAX);

  D3D12_RECT onePixel{0, 0, 1, 1};
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_PREDICATION,
             D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), 0, sizeof(UINT64));
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PREDICATION);
  list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ResolveSubresourceRegion(depthResolved.Get(), 0, 6, 6, depthMsaa.Get(), 0, &onePixel,
                                 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOLVE_MODE_MIN);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_PREDICATION,
             D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), sizeof(UINT64), sizeof(UINT64));
  transition(list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PREDICATION);
  list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ResolveSubresourceRegion(depthResolved.Get(), 0, 6, 4, depthMsaa.Get(), 0, &onePixel,
                                 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOLVE_MODE_MIN);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  transition(list.Get(), predicateFalseMsaa.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  transition(list.Get(), predicateTrueMsaa.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  list->ResolveSubresourceRegion(resolved.Get(), 0, 0, 4, predicateFalseMsaa.Get(), 0, nullptr,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOLVE_MODE_AVERAGE);
  list->ResolveSubresourceRegion(resolved.Get(), 0, 4, 4, predicateTrueMsaa.Get(), 0, nullptr,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOLVE_MODE_AVERAGE);

  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = nullptr;
  list->ResourceBarrier(1, &uav);
  D3D12_RESOURCE_BARRIER aliasing{};
  aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  aliasing.Aliasing.pResourceBefore = msaa.Get();
  aliasing.Aliasing.pResourceAfter = resolved.Get();
  list->ResourceBarrier(1, &aliasing);

  transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowSize = 0;
  UINT64 total = 0;
  device->GetCopyableFootprints(&resolvedDesc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
  const D3D12_HEAP_PROPERTIES readbackHeap = heap(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC readbackDesc = bufferDesc(total);
  ComPtr<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&readback)), "Create readback");
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = resolved.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = footprint;
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

  transition(list.Get(), depthResolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprints[2]{};
  UINT depthRows[2]{};
  UINT64 depthRowSizes[2]{};
  UINT64 depthTotal = 0;
  device->GetCopyableFootprints(&depthResolvedDesc, 0, 2, 0, depthFootprints, depthRows,
                                depthRowSizes, &depthTotal);
  if (depthFootprints[0].Footprint.Format != DXGI_FORMAT_R32_TYPELESS ||
      depthFootprints[1].Footprint.Format != DXGI_FORMAT_R8_TYPELESS)
    fail("depth stencil footprint formats");
  const D3D12_RESOURCE_DESC depthReadbackDesc = bufferDesc(depthTotal);
  ComPtr<ID3D12Resource> depthReadback;
  check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &depthReadbackDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&depthReadback)), "Create depth readback");
  for (UINT plane = 0; plane < 2; ++plane) {
    D3D12_TEXTURE_COPY_LOCATION depthSource{};
    depthSource.pResource = depthResolved.Get();
    depthSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    depthSource.SubresourceIndex = plane;
    D3D12_TEXTURE_COPY_LOCATION depthDestination{};
    depthDestination.pResource = depthReadback.Get();
    depthDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    depthDestination.PlacedFootprint = depthFootprints[plane];
    list->CopyTextureRegion(&depthDestination, 0, 0, 0, &depthSource, nullptr);
  }
  check(list->Close(), "Close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event)
    fail("CreateEvent");
  check(queue->Signal(fence.Get(), 1), "Signal");
  check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
  WaitForSingleObject(event, INFINITE);
  CloseHandle(event);

  void *mappedData = nullptr;
  D3D12_RANGE readRange{0, static_cast<SIZE_T>(total)};
  check(readback->Map(0, &readRange, &mappedData), "Map");
  const BYTE *mapped = static_cast<const BYTE *>(mappedData);
  auto color = [&](UINT x, UINT y) {
    return reinterpret_cast<const float *>(mapped +
                                           y * footprint.Footprint.RowPitch + x * 4 * sizeof(float));
  };
  auto requireColor = [&](UINT x, UINT y, const float expected[4], const char *step) {
    const float *actual = color(x, y);
    for (UINT channel = 0; channel < 4; ++channel) {
      if (std::fabs(actual[channel] - expected[channel]) > 0.001f)
        fail(step);
    }
  };
  const float average[4] = {2.5f, 11.5f, 23.0f, 38.5f};
  const float minimum[4] = {1.0f, 10.0f, 20.0f, 37.0f};
  const float maximum[4] = {4.0f, 13.0f, 26.0f, 40.0f};
  requireColor(1, 1, average, "RGBA average resolve mismatch");
  requireColor(3, 1, minimum, "RGBA minimum resolve mismatch");
  requireColor(5, 1, maximum, "RGBA maximum resolve mismatch");
  requireColor(0, 0, guard, "RGBA resolve guard mismatch");
  requireColor(7, 0, average, "predicate snapshot resolve mismatch");
  requireColor(7, 2, guard, "rebound zero predicate resolve mismatch");
  requireColor(1, 5, falseGuard, "predicated false RGBA copy mismatch");
  requireColor(5, 5, average, "predicated true RGBA copy mismatch");
  D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);

  void *depthMappedData = nullptr;
  D3D12_RANGE depthReadRange{0, static_cast<SIZE_T>(depthTotal)};
  check(depthReadback->Map(0, &depthReadRange, &depthMappedData), "Map depth readback");
  const BYTE *depthMapped = static_cast<const BYTE *>(depthMappedData);
  auto depthValue = [&](UINT x, UINT y) {
    return *reinterpret_cast<const float *>(depthMapped + depthFootprints[0].Offset +
                                            y * depthFootprints[0].Footprint.RowPitch +
                                            x * sizeof(float));
  };
  auto stencilValue = [&](UINT x, UINT y) {
    return *(depthMapped + depthFootprints[1].Offset +
             y * depthFootprints[1].Footprint.RowPitch + x);
  };
  auto requireDepth = [&](UINT x, UINT y, float expected, const char *step) {
    const float actual = depthValue(x, y);
    if (std::fabs(actual - expected) > 0.001f) {
      std::fprintf(stderr, "COPY_RESOLVE_DEPTH_DIAG x=%u y=%u expected=%.6f actual=%.6f "
                           "format=%u row_pitch=%u offset=%llu\n",
                   x, y, expected, actual, UINT(depthFootprints[0].Footprint.Format),
                   depthFootprints[0].Footprint.RowPitch,
                   static_cast<unsigned long long>(depthFootprints[0].Offset));
      fail(step);
    }
  };
  auto requireStencil = [&](UINT x, UINT y, BYTE expected, const char *step) {
    const BYTE actual = stencilValue(x, y);
    if (actual != expected) {
      std::fprintf(stderr, "COPY_RESOLVE_STENCIL_DIAG x=%u y=%u expected=%u actual=%u "
                           "format=%u row_pitch=%u offset=%llu\n",
                   x, y, UINT(expected), UINT(actual),
                   UINT(depthFootprints[1].Footprint.Format),
                   depthFootprints[1].Footprint.RowPitch,
                   static_cast<unsigned long long>(depthFootprints[1].Offset));
      fail(step);
    }
  };
  requireDepth(1, 1, 0.1f, "depth MIN resolve mismatch");
  requireDepth(2, 2, 0.1f, "depth MIN partial rect mismatch");
  requireDepth(4, 1, 0.9f, "depth MAX resolve mismatch");
  requireDepth(5, 2, 0.9f, "depth MAX partial rect mismatch");
  requireStencil(1, 1, 0xa5, "depth resolve changed stencil plane");
  requireStencil(4, 1, 0xa5, "depth resolve changed stencil plane");
  requireStencil(1, 4, 10, "stencil MIN resolve mismatch");
  requireStencil(2, 5, 10, "stencil MIN partial rect mismatch");
  requireStencil(4, 4, 40, "stencil MAX resolve mismatch");
  requireStencil(5, 5, 40, "stencil MAX partial rect mismatch");
  requireDepth(1, 4, 0.75f, "stencil resolve changed depth plane");
  requireDepth(4, 4, 0.75f, "stencil resolve changed depth plane");
  requireDepth(0, 0, 0.75f, "depth stencil outside guard mismatch");
  requireDepth(3, 1, 0.75f, "depth resolve destination offset guard mismatch");
  requireDepth(1, 3, 0.75f, "depth resolve partial rect guard mismatch");
  requireStencil(0, 0, 0xa5, "stencil resolve outside guard mismatch");
  requireStencil(3, 4, 0xa5, "stencil resolve destination offset guard mismatch");
  requireDepth(6, 6, 0.75f, "false-predicate depth resolve mismatch");
  requireDepth(6, 4, 0.1f, "true-predicate depth resolve mismatch");
  requireStencil(6, 4, 0xa5, "true-predicate depth resolve changed stencil plane");
  requireStencil(6, 6, 0xa5, "false-predicate depth resolve changed stencil plane");
  D3D12_RANGE depthWritten{0, 0};
  depthReadback->Unmap(0, &depthWritten);

  std::printf("OWNED_D3D12_COPY_RESOLVE_MSAA_PREDICATION_PASS false=7 true=2.5 source_update_unpredicated=1\n");
  std::printf("OWNED_D3D12_COPY_RESOLVE_PREDICATE_SNAPSHOT_PASS initial_one=1 predicated_write_zero=1 saved_one_executes=1 rebound_zero_skips=1\n");
  std::printf("OWNED_D3D12_COPY_RESOLVE_PASS average=2.5 min=1 max=4 guard=99 uav=1 aliasing=1\n");
  std::printf("OWNED_D3D12_COPY_RESOLVE_RGBA_PASS average=2.5,11.5,23,38.5 min=1,10,20,37 max=4,13,26,40 guard=99,98,97,96\n");
  std::printf("OWNED_D3D12_COPY_RESOLVE_DEPTH_STENCIL_PASS depth_min=0.1 depth_max=0.9 stencil_min=10 stencil_max=40 partial=1 plane_preservation=1 predicate_false=1 predicate_true=1\n");
  return 0;
}
