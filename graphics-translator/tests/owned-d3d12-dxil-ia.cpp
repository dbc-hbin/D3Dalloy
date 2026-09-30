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
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 8;
constexpr UINT kHeight = 8;
constexpr DWORD kFenceTimeoutMs = 15000;
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";

int fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_DXIL_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  return 1;
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) {
    fail(step, hr);
    ExitProcess(1);
  }
}

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    fail("CreateFileW(shader)", HRESULT_FROM_WIN32(GetLastError()));
    ExitProcess(1);
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
    CloseHandle(file);
    fail("GetFileSizeEx(shader)");
    ExitProcess(1);
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    fail("ReadFile(shader)", HRESULT_FROM_WIN32(error));
    ExitProcess(1);
  }
  CloseHandle(file);
  return data;
}

std::vector<uint8_t> compileDxbc(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), "mixed-stage-ia", nullptr, nullptr,
                                entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    check(hr, "D3DCompile(mixed stage)");
  }
  const auto *bytes = static_cast<const uint8_t *>(code->GetBufferPointer());
  return {bytes, bytes + code->GetBufferSize()};
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = type;
  props.CreationNodeMask = 1;
  props.VisibleNodeMask = 1;
  return props;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
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

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  const auto props = heapProperties(heap);
  const auto desc = bufferDesc(size, flags);
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)),
        "CreateCommittedResource(buffer)");
  return resource;
}

ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC &desc) {
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
  if (FAILED(hr)) {
    if (error)
      std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr);
    check(hr, "D3D12SerializeRootSignature");
  }
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "CreateRootSignature");
  return root;
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
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;

  explicit Context(ID3D12Device *device) {
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)),
          "CreateCommandList");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
      fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
      ExitProcess(1);
    }
  }

  ~Context() {
    if (event)
      CloseHandle(event);
  }

  void submitAndWait() {
    check(list->Close(), "CommandList::Close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "Queue::Signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "Fence::SetEventOnCompletion");
      const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
      if (wait != WAIT_OBJECT_0) {
        fail("GPU fence timeout", HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
        ExitProcess(1);
      }
    }
  }

  void reset() {
    check(allocator->Reset(), "Allocator::Reset");
    check(list->Reset(allocator.Get(), nullptr), "List::Reset");
  }
};

int testCompute(ID3D12Device *device, Context &ctx, const std::vector<uint8_t> &cs) {
  std::array<D3D12_ROOT_PARAMETER, 2> params{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 2;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].Descriptor.ShaderRegister = 0;

  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = static_cast<UINT>(params.size());
  rootDesc.pParameters = params.data();
  const auto root = createRootSignature(device, rootDesc);

  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = root.Get();
  psoDesc.CS = {cs.data(), cs.size()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "CreateComputePipelineState(DXIL)");

  auto output = createBuffer(device, 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = createBuffer(device, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);

  constexpr UINT constants[2] = {3, 0x13579bdfu};
  ctx.list->SetPipelineState(pso.Get());
  ctx.list->SetComputeRootSignature(root.Get());
  ctx.list->SetComputeRoot32BitConstants(0, 2, constants, 0);
  ctx.list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  ctx.list->Dispatch(1, 1, 1);
  transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 64);
  ctx.submitAndWait();

  void *mapped = nullptr;
  D3D12_RANGE range{0, 64};
  check(readback->Map(0, &range, &mapped), "Map(compute readback)");
  const UINT *words = static_cast<const UINT *>(mapped);
  const bool ok = words[constants[0]] == constants[1];
  std::printf("OWNED_D3D12_DXIL_COMPUTE index=%u expected=0x%08x actual=0x%08x\n",
              constants[0], constants[1], words[constants[0]]);
  readback->Unmap(0, nullptr);
  return ok ? 0 : fail("compute root repack mismatch");
}

int testGraphics(ID3D12Device *device, Context &ctx,
                 const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps) {
  std::array<D3D12_ROOT_PARAMETER, 2> params{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 20;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].Descriptor.ShaderRegister = 0;

  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = static_cast<UINT>(params.size());
  rootDesc.pParameters = params.data();
  const auto root = createRootSignature(device, rootDesc);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = root.Get();
  psoDesc.VS = {vs.data(), vs.size()};
  psoDesc.PS = {ps.data(), ps.size()};
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.SampleMask = 0xffffffffu;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.RasterizerState.DepthClipEnable = TRUE;
  psoDesc.DepthStencilState.DepthEnable = TRUE;
  psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 0;
  psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  psoDesc.SampleDesc.Count = 1;

  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(DXIL)");

  auto output = createBuffer(device, sizeof(float) * 16, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto outputReadback = createBuffer(device, sizeof(float) * 16, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

  D3D12_RESOURCE_DESC depthDesc{};
  depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDesc.Width = kWidth;
  depthDesc.Height = kHeight;
  depthDesc.DepthOrArraySize = 1;
  depthDesc.MipLevels = 1;
  depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
  depthDesc.SampleDesc.Count = 1;
  depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  const auto defaultProps = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_CLEAR_VALUE depthClear{};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 0.5f;
  depthClear.DepthStencil.Stencil = 0;
  ComPtr<ID3D12Resource> depth;
  check(device->CreateCommittedResource(&defaultProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&depth)),
        "CreateCommittedResource(depth)");

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap(DSV)");
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  const auto dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsvHandle);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint{};
  UINT depthRows = 0;
  UINT64 depthRowBytes = 0, depthTotalBytes = 0;
  device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &depthFootprint, &depthRows, &depthRowBytes, &depthTotalBytes);
  auto depthReadback = createBuffer(device, depthTotalBytes, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

  std::array<float, 20> constants{};
  constants[0] = constants[1] = constants[2] = constants[3] = 1.0f;
  std::array<UINT, 20> constantBits{};
  std::memcpy(constantBits.data(), constants.data(), sizeof(constants));

  ctx.list->SetPipelineState(pso.Get());
  ctx.list->SetGraphicsRootSignature(root.Get());
  ctx.list->SetGraphicsRoot32BitConstants(0, 20, constantBits.data(), 0);
  ctx.list->SetGraphicsRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  ctx.list->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
  ctx.list->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH,
                                  0.5f, 0, 0, nullptr);
  D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
  ctx.list->RSSetViewports(1, &viewport);
  ctx.list->RSSetScissorRects(1, &scissor);
  ctx.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx.list->DrawInstanced(3, 1, 0, 0);

  transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, sizeof(float) * 16);
  transition(ctx.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION depthSrc{};
  depthSrc.pResource = depth.Get();
  depthSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION depthDst{};
  depthDst.pResource = depthReadback.Get();
  depthDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  depthDst.PlacedFootprint = depthFootprint;
  ctx.list->CopyTextureRegion(&depthDst, 0, 0, 0, &depthSrc, nullptr);
  ctx.submitAndWait();

  D3D12_RANGE outputRange{0, sizeof(float) * 16};
  void *mapped = nullptr;
  check(outputReadback->Map(0, &outputRange, &mapped), "Map(graphics UAV readback)");
  const float *values = static_cast<const float *>(mapped);
  const float expectedDx = 1.0f + 20.0f / static_cast<float>(kWidth);
  const float expectedDy = 1.0f - 30.0f / static_cast<float>(kHeight);
  bool varyingOk = true;
  for (UINT i = 0; i < 4; i++) {
    const float *v = values + i * 4;
    std::printf("OWNED_D3D12_DXIL_VARYING slot=%u value=(%.5f,%.5f,%.5f,%.5f) expected=(%.5f,%.5f,1,1)\n",
                i, v[0], v[1], v[2], v[3], expectedDx, expectedDy);
    varyingOk &= std::fabs(v[0] - expectedDx) < 0.05f && std::fabs(v[1] - expectedDy) < 0.05f &&
                 std::fabs(v[2] - 1.0f) < 0.01f && std::fabs(v[3] - 1.0f) < 0.01f;
  }
  outputReadback->Unmap(0, nullptr);

  D3D12_RANGE depthRange{0, static_cast<SIZE_T>(depthTotalBytes)};
  mapped = nullptr;
  check(depthReadback->Map(0, &depthRange, &mapped), "Map(depth readback)");
  const auto *depthBytes = static_cast<const uint8_t *>(mapped);
  const float *depthCenter = reinterpret_cast<const float *>(
      depthBytes + (kHeight / 2) * depthFootprint.Footprint.RowPitch + (kWidth / 2) * sizeof(float));
  const float depthValue = *depthCenter;
  std::printf("OWNED_D3D12_DXIL_DEPTH center=%.6f expected=0.0 rows=%u row_bytes=%llu\n",
              depthValue, depthRows, static_cast<unsigned long long>(depthRowBytes));
  const bool depthOk = std::fabs(depthValue) < 0.0001f;
  depthReadback->Unmap(0, nullptr);

  if (!varyingOk)
    return fail("graphics varying derivative mismatch");
  if (!depthOk)
    return fail("graphics depth mismatch");
  return 0;
}


// The IA shaders use POSITION.xy, TEXCOORD.xy, COLOR1.xyzw,
// SV_VertexID and SV_InstanceID. They output POSITION and
// COLOR1 + float4(TEXCOORD.xy, vertex_id * .01, instance_id * .01).
// D3D IDs are relative to the draw even though buffer fetches include the
// start/base vertex and start instance offsets.
int testInputAssembler(ID3D12Device *device, Context &ctx,
                       const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps,
                       const char *stagePair,
                       const char *marker = "OWNED_D3D12_DXIL_IA_PASS") {
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  const auto root = createRootSignature(device, rootDesc);
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get();
  desc.VS = {vs.data(), vs.size()};
  desc.PS = {ps.data(), ps.size()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = 0xffffffffu;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.InputLayout = {elements, static_cast<UINT>(std::size(elements))};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(IA)");

  // Upload allocations cover the entire vertex/instance fetch range, not just
  // the selected element. Initialize unused fields deterministically.
  auto plain = createBuffer(device, 100, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indexed = createBuffer(device, 120, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
  auto uv = createBuffer(device, 60, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  auto colors = createBuffer(device, 120, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indices = createBuffer(device, sizeof(uint16_t), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
  auto upload = [](ID3D12Resource *resource, size_t size, size_t offset, const void *data, size_t bytes) {
    void *mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    check(resource->Map(0, &noRead, &mapped), "Map(IA upload)");
    std::memset(mapped, 0, size);
    std::memcpy(static_cast<uint8_t *>(mapped) + offset, data, bytes);
    D3D12_RANGE written{0, size};
    resource->Unmap(0, &written);
  };
  const float plainPosition[] = {-0.625f, 0.625f};
  const float indexedPosition[] = {0.625f, -0.625f};
  const float uvPlain[] = {0.2f, 0.3f};
  const float uvIndexed[] = {0.4f, 0.1f};
  const float colorPlain[] = {0.1f, 0.2f, 0.3f, 1.0f};
  const float colorIndexed[] = {0.2f, 0.1f, 0.4f, 1.0f};
  const uint16_t indexValue = 1;
  upload(plain.Get(), 100, 2 * 20 + 4, plainPosition, sizeof(plainPosition));
  upload(indexed.Get(), 120, 3 * 24 + 4, indexedPosition, sizeof(indexedPosition));
  // Both UV and color records share their respective allocations.
  void *mapped = nullptr;
  D3D12_RANGE noRead{0, 0};
  check(uv->Map(0, &noRead, &mapped), "Map(IA uv)");
  std::memset(mapped, 0, 60);
  std::memcpy(static_cast<uint8_t *>(mapped) + 2 * 12, uvPlain, sizeof(uvPlain));
  std::memcpy(static_cast<uint8_t *>(mapped) + 3 * 12, uvIndexed, sizeof(uvIndexed));
  D3D12_RANGE uvWritten{0, 60};
  uv->Unmap(0, &uvWritten);
  check(colors->Map(0, &noRead, &mapped), "Map(IA colors)");
  std::memset(mapped, 0, 120);
  std::memcpy(static_cast<uint8_t *>(mapped) + 3 * 24 + 8, colorPlain, sizeof(colorPlain));
  std::memcpy(static_cast<uint8_t *>(mapped) + 2 * 24 + 8, colorIndexed, sizeof(colorIndexed));
  D3D12_RANGE colorsWritten{0, 120};
  colors->Unmap(0, &colorsWritten);
  upload(indices.Get(), sizeof(indexValue), 0, &indexValue, sizeof(indexValue));

  D3D12_RESOURCE_DESC targetDesc{};
  targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  targetDesc.Width = kWidth;
  targetDesc.Height = kHeight;
  targetDesc.DepthOrArraySize = 1;
  targetDesc.MipLevels = 1;
  targetDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  targetDesc.SampleDesc.Count = 1;
  targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = targetDesc.Format;
  const auto defaultProps = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> target;
  check(device->CreateCommittedResource(&defaultProps, D3D12_HEAP_FLAG_NONE, &targetDesc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)),
        "CreateCommittedResource(IA target)");
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap(IA RTV)");
  const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0, totalBytes = 0;
  device->GetCopyableFootprints(&targetDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
  auto readback = createBuffer(device, totalBytes, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

  ctx.list->SetPipelineState(pso.Get());
  ctx.list->SetGraphicsRootSignature(root.Get());
  ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const float black[] = {0, 0, 0, 0};
  ctx.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
  ctx.list->RSSetViewports(1, &viewport);
  ctx.list->RSSetScissorRects(1, &scissor);
  ctx.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  const D3D12_VERTEX_BUFFER_VIEW views[] = {
      {plain->GetGPUVirtualAddress(), 100, 20},
      {uv->GetGPUVirtualAddress(), 60, 12},
      {colors->GetGPUVirtualAddress(), 120, 24},
  };
  ctx.list->IASetVertexBuffers(0, static_cast<UINT>(std::size(views)), views);
  ctx.list->DrawInstanced(1, 1, 2, 3);
  const D3D12_VERTEX_BUFFER_VIEW indexedView{indexed->GetGPUVirtualAddress(), 120, 24};
  ctx.list->IASetVertexBuffers(0, 1, &indexedView);
  const D3D12_INDEX_BUFFER_VIEW indexView{indices->GetGPUVirtualAddress(), sizeof(indexValue), DXGI_FORMAT_R16_UINT};
  ctx.list->IASetIndexBuffer(&indexView);
  ctx.list->DrawIndexedInstanced(1, 1, 0, 2, 2);
  transition(ctx.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = target.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = footprint;
  ctx.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  ctx.submitAndWait();
  D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
  mapped = nullptr;
  check(readback->Map(0, &range, &mapped), "Map(IA readback)");
  const auto *bytes = static_cast<const uint8_t *>(mapped);
  const auto *nonindexed = reinterpret_cast<const float *>(bytes + footprint.Footprint.RowPitch + 16);
  const auto *indexedPixel = reinterpret_cast<const float *>(bytes + 6 * footprint.Footprint.RowPitch + 6 * 16);
  constexpr float expected[2][4] = {{0.3f, 0.5f, 0.3f, 1.0f}, {0.6f, 0.2f, 0.41f, 1.0f}};
  bool ok = true;
  for (UINT component = 0; component < 4; ++component) {
    ok &= std::fabs(nonindexed[component] - expected[0][component]) < 0.001f;
    ok &= std::fabs(indexedPixel[component] - expected[1][component]) < 0.001f;
  }
  if (!ok) {
    std::fprintf(stderr, "IA output mismatch pair=%s nonindexed=(%.3f,%.3f,%.3f,%.3f) "
                         "indexed=(%.3f,%.3f,%.3f,%.3f)\n", stagePair,
                 nonindexed[0], nonindexed[1], nonindexed[2], nonindexed[3],
                 indexedPixel[0], indexedPixel[1], indexedPixel[2], indexedPixel[3]);
    readback->Unmap(0, nullptr);
    return fail("IA output mismatch");
  }
  std::printf("%s nonindexed=(%.3f,%.3f,%.3f,%.3f) "
              "indexed=(%.3f,%.3f,%.3f,%.3f) strides=(20,24) drawparams=(2,3,2,2) pair=%s\n",
              marker, nonindexed[0], nonindexed[1], nonindexed[2], nonindexed[3],
              indexedPixel[0], indexedPixel[1], indexedPixel[2], indexedPixel[3], stagePair);
  readback->Unmap(0, nullptr);
  return 0;
}

// The PS consumes the user varying; the clip/cull builtin separately controls
// primitive rejection. Report only finite colors so readback is unambiguous.
int testNonfiniteDistances(ID3D12Device *device, Context &ctx, UINT samples) {
  constexpr char shader[] = R"(
    struct Clip { float4 position : SV_Position; float distance : SV_ClipDistance0; };
    struct Cull { float4 position : SV_Position; float distance : SV_CullDistance0; };
    float4 position(uint id) { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
    float4 pointPosition() { return float4(.125, -.125, 0, 1); }
    float4 linePosition(uint id) { return float4(id ? 1 : -1, -.125, 0, 1); }
    Clip clipInfVS(uint id : SV_VertexID) {
      Clip v; v.position = position(id); v.distance = asfloat(0x7f800000u); return v;
    }
    Clip clipNanVS(uint id : SV_VertexID) {
      Clip v; v.position = position(id); v.distance = asfloat(0x7fc00000u); return v;
    }
    Cull cullInfVS(uint id : SV_VertexID) {
      Cull v; v.position = position(id); v.distance = asfloat(0x7f800000u); return v;
    }
    Cull cullNanVS(uint id : SV_VertexID) {
      Cull v; v.position = position(id);
      v.distance = id == 0 ? asfloat(0x7fc00000u) : 1; return v;
    }
    Clip clipInfPointVS(uint id : SV_VertexID) {
      Clip v; v.position = pointPosition(); v.distance = asfloat(0x7f800000u); return v;
    }
    Cull cullInfLineVS(uint id : SV_VertexID) {
      Cull v; v.position = linePosition(id); v.distance = asfloat(0x7f800000u); return v;
    }
    Cull cullNanLineVS(uint id : SV_VertexID) {
      Cull v; v.position = linePosition(id);
      v.distance = id == 0 ? asfloat(0x7fc00000u) : 1; return v;
    }
    float4 clipPS(float distance : SV_ClipDistance0) : SV_Target0 {
      uint bits = asuint(distance) & 0x7fffffffu;
      return float4((bits & 0x7f800000u) == 0x7f800000u, 1,
                    bits > 0x7f800000u, 1);
    }
    float4 cullPS(float distance : SV_CullDistance0) : SV_Target0 {
      uint bits = asuint(distance) & 0x7fffffffu;
      return float4((bits & 0x7f800000u) == 0x7f800000u, 1,
                    bits > 0x7f800000u, 1);
    }
    float4 clipSamplePS(float distance : SV_ClipDistance0,
                        uint sample : SV_SampleIndex) : SV_Target0 {
      uint bits = asuint(distance) & 0x7fffffffu;
      return float4((bits & 0x7f800000u) == 0x7f800000u, 1, sample == 0, 1);
    }
  )";
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  const auto root = createRootSignature(device, rootDesc);
  D3D12_RESOURCE_DESC targetDesc{};
  targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  targetDesc.Width = kWidth;
  targetDesc.Height = kHeight;
  targetDesc.DepthOrArraySize = 1;
  targetDesc.MipLevels = 1;
  targetDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  targetDesc.SampleDesc.Count = samples;
  targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = targetDesc.Format;
  ComPtr<ID3D12Resource> target;
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &targetDesc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)),
        "CreateCommittedResource(nonfinite target)");
  ComPtr<ID3D12Resource> resolved;
  if (samples > 1) {
    auto resolveDesc = targetDesc;
    resolveDesc.SampleDesc.Count = 1;
    resolveDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &resolveDesc,
                                          D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)),
          "CreateCommittedResource(nonfinite resolve target)");
  }
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap(nonfinite RTV)");
  const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0, totalBytes = 0;
  auto footprintDesc = targetDesc;
  footprintDesc.SampleDesc.Count = 1;
  device->GetCopyableFootprints(&footprintDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
  auto readback = createBuffer(device, totalBytes, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

  struct Case {
    const char *name, *vs, *ps;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    D3D_PRIMITIVE_TOPOLOGY topology;
    UINT vertices;
    bool expectDraw, expectNonfinite, requireNan, sampleIndex;
  };
  constexpr Case cases[] = {
    {"clip-inf", "clipInfVS", "clipPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, true, true, false, false},
    {"cull-inf", "cullInfVS", "cullPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, true, true, false, false},
    {"clip-nan", "clipNanVS", "clipPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, false, false, false, false},
    {"cull-nan-survives", "cullNanVS", "cullPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, true, true, true, false},
    {"clip-point-inf", "clipInfPointVS", "clipPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,
      D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 1, true, true, false, false},
    {"cull-line-inf", "cullInfLineVS", "cullPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
      D3D_PRIMITIVE_TOPOLOGY_LINELIST, 2, true, true, false, false},
    {"cull-line-nan-survives", "cullNanLineVS", "cullPS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
      D3D_PRIMITIVE_TOPOLOGY_LINELIST, 2, true, true, true, false},
    {"clip-sample-inf", "clipInfVS", "clipSamplePS", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, true, true, false, true},
  };
  int failures = 0;
  for (const auto &c : cases) {
    const auto vs = compileDxbc(shader, c.vs, "vs_5_0");
    const auto ps = compileDxbc(shader, c.ps, "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = 0xffffffffu;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = c.type;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = targetDesc.Format;
    desc.SampleDesc.Count = samples;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)),
          "CreateGraphicsPipelineState(nonfinite distance)");
    ctx.list->SetPipelineState(pso.Get());
    ctx.list->SetGraphicsRootSignature(root.Get());
    const float background[] = {.25f, .25f, .25f, .25f};
    ctx.list->ClearRenderTargetView(rtv, background, 0, nullptr);
    ctx.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(kWidth), LONG(kHeight)};
    ctx.list->RSSetViewports(1, &viewport);
    ctx.list->RSSetScissorRects(1, &scissor);
    ctx.list->IASetPrimitiveTopology(c.topology);
    ctx.list->DrawInstanced(c.vertices, 1, 0, 0);
    transition(ctx.list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               samples > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (samples > 1) {
      ctx.list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, targetDesc.Format);
      transition(ctx.list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = samples > 1 ? resolved.Get() : target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(ctx.list.Get(), target.Get(),
               samples > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (samples > 1)
      transition(ctx.list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_RESOLVE_DEST);
    ctx.submitAndWait();
    void *mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
    check(readback->Map(0, &range, &mapped), "Map(nonfinite readback)");
    const auto *pixel = reinterpret_cast<const float *>(static_cast<const uint8_t *>(mapped) +
        (kHeight / 2) * footprint.Footprint.RowPitch + (kWidth / 2) * 16);
    const bool drawn = std::fabs(pixel[1] - 1) < .001f && std::fabs(pixel[3] - 1) < .001f;
    const bool untouched = std::fabs(pixel[0] - .25f) < .001f &&
                           std::fabs(pixel[1] - .25f) < .001f;
    const bool nonfinite = std::fabs(pixel[0] - 1) < .001f;
    const bool nan = std::fabs(pixel[2] - 1) < .001f;
    std::printf("OWNED_D3D12_DISTANCE samples=%u case=%s center=(%.3f,%.3f,%.3f,%.3f) drawn=%u nonfinite=%u nan=%u\n",
                samples, c.name, pixel[0], pixel[1], pixel[2], pixel[3], drawn, nonfinite, nan);
    const bool sampleCorrect = !c.sampleIndex || std::fabs(pixel[2] - 1.0f / samples) < .001f;
    const bool ok = c.expectDraw ? drawn && nonfinite == c.expectNonfinite &&
      (!c.requireNan || nan) && sampleCorrect : untouched;
    readback->Unmap(0, nullptr);
    if (!ok) failures += fail("nonfinite distance mismatch");
    ctx.reset();
  }
  if (!failures)
    std::printf("OWNED_D3D12_DISTANCE_PASS samples=%u\n", samples);
  return failures;
}

int run(int argc, wchar_t **argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: owned-d3d12-dxil-ia.exe <compute.dxil> <vertex.dxil> "
                         "<pixel.dxil> <dxil-ia-vs.dxil> <dxil-ia-ps.dxil>\n");
    return 2;
  }
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  if (!d3d12)
    return fail("GetModuleHandleW(d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));
  using BackendIdFn = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"));
  if (!backendId || !backendId() || std::strcmp(backendId(), kExpectedBackendId) != 0)
    return fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_DXIL_BACKEND id=%s\n", backendId());
  std::fflush(stdout);
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  const auto cs = readFile(argv[1]);
  const auto vs = readFile(argv[2]);
  const auto ps = readFile(argv[3]);
  const auto iaVS = readFile(argv[4]);
  const auto iaPS = readFile(argv[5]);
  constexpr char mixedSource[] = R"(
    struct Input {
      float2 position : POSITION;
      float2 uv : TEXCOORD0;
      float4 color : COLOR1;
      uint vertex : SV_VertexID;
      uint instance : SV_InstanceID;
    };
    struct Output {
      float4 color : CoLoR0;
      float4 position : SV_Position;
    };
    Output mixedVS(Input input) {
      Output output;
      output.position = float4(input.position, 0, 1);
      output.color = input.color + float4(input.uv, input.vertex * .01, input.instance * .01);
      return output;
    }
    float4 mixedPS(float4 color : cOlOr0) : SV_Target0 {
      return color;
    }
    struct ClipOutput {
      float4 color : CoLoR0;
      float4 position : SV_Position;
      float2 clip0 : SV_ClipDistance0;
      float2 clip1 : SV_ClipDistance1;
    };
    ClipOutput clipVS(Input input) {
      ClipOutput output;
      output.position = float4(input.position, 0, 1);
      output.color = input.color + float4(input.uv, input.vertex * .01, input.instance * .01);
      output.clip0 = float2(.75, 1);
      output.clip1 = float2(.5, 1);
      return output;
    }
    float4 clipPS(float2 clip1 : SV_ClipDistance1, float4 color : COLOR0,
                  float2 clip0 : SV_ClipDistance0) : SV_Target0 {
      return color + float4(clip0.x - .75, clip1.x - .5, 0, 0);
    }
    struct PackedOutput {
      float4 color : COLOR0;
      float4 position : SV_Position;
      float2 packed0 : TEXCOORD3;
      float2 packed1 : TEXCOORD4;
    };
    PackedOutput packedVS(Input input) {
      PackedOutput output;
      output.position = float4(input.position, 0, 1);
      output.color = input.color + float4(input.uv, input.vertex * .01, input.instance * .01);
      output.packed0 = float2(.25, .875);
      output.packed1 = float2(.5, .625);
      return output;
    }
    float4 packedPS(float2 packed1 : TEXCOORD4, float4 color : COLOR0,
                    float2 packed0 : TEXCOORD3) : SV_Target0 {
      return color + float4(packed0.x - .25, packed1.y - .625, 0, 0);
    }
  )";
  const auto dxbcVS = compileDxbc(mixedSource, "mixedVS", "vs_5_0");
  const auto dxbcPS = compileDxbc(mixedSource, "mixedPS", "ps_5_0");
  const auto clipVS = compileDxbc(mixedSource, "clipVS", "vs_5_0");
  const auto clipPS = compileDxbc(mixedSource, "clipPS", "ps_5_0");
  const auto packedVS = compileDxbc(mixedSource, "packedVS", "vs_5_0");
  const auto packedPS = compileDxbc(mixedSource, "packedPS", "ps_5_0");
  Context ctx(device.Get());
  int failures = testCompute(device.Get(), ctx, cs);
  ctx.reset();
  failures += testGraphics(device.Get(), ctx, vs, ps);
  ctx.reset();
  failures += testInputAssembler(device.Get(), ctx, iaVS, iaPS, "DXIL-DXIL");
  ctx.reset();
  failures += testInputAssembler(device.Get(), ctx, dxbcVS, iaPS, "DXBC-DXIL");
  ctx.reset();
  failures += testInputAssembler(device.Get(), ctx, iaVS, dxbcPS, "DXIL-DXBC");
  ctx.reset();
  failures += testInputAssembler(device.Get(), ctx, clipVS, clipPS, "DXBC-DXBC-CLIP",
                                 "OWNED_D3D12_DXIL_CLIP_LINK_PASS");
  ctx.reset();
  failures += testInputAssembler(device.Get(), ctx, packedVS, packedPS, "DXBC-DXBC-PACKED",
                                 "OWNED_D3D12_DXIL_PACKED_LINK_PASS");
  ctx.reset();
  failures += testNonfiniteDistances(device.Get(), ctx, 1);
  failures += testNonfiniteDistances(device.Get(), ctx, 4);
  if (!failures)
    std::puts("OWNED_D3D12_DXIL_PASS");
  return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  return run(argc, argv);
}
