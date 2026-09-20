#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
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

int testDescriptorTable(ID3D12Device *device, Context &ctx, const std::vector<uint8_t> &cs) {
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  std::array<D3D12_ROOT_PARAMETER, 2> params{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 2;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;

  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = static_cast<UINT>(params.size());
  rootDesc.pParameters = params.data();
  const auto root = createRootSignature(device, rootDesc);
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get();
  desc.CS = {cs.data(), cs.size()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "CreateComputePipelineState(table)");

  auto output = createBuffer(device, 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = createBuffer(device, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 1;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap(table)");
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Format = DXGI_FORMAT_UNKNOWN;
  uav.Buffer.NumElements = 16;
  uav.Buffer.StructureByteStride = 4;
  device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());

  constexpr UINT constants[2] = {3, 0x2468ace0u};
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  ctx.list->SetDescriptorHeaps(1, heaps);
  ctx.list->SetPipelineState(pso.Get());
  ctx.list->SetComputeRootSignature(root.Get());
  ctx.list->SetComputeRoot32BitConstants(0, 2, constants, 0);
  ctx.list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
  ctx.list->Dispatch(1, 1, 1);
  transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 64);
  ctx.submitAndWait();

  void *mapped = nullptr;
  D3D12_RANGE mapRange{0, 64};
  check(readback->Map(0, &mapRange, &mapped), "Map(table readback)");
  const UINT actual = static_cast<const UINT *>(mapped)[constants[0]];
  std::printf("OWNED_D3D12_DXIL_DESCRIPTOR_TABLE_EXECUTED expected=0x%08x actual=0x%08x\n",
              constants[1], actual);
  readback->Unmap(0, nullptr);
  return actual == constants[1] ? 0 : fail("descriptor table readback mismatch");
}

int testRegisterSpaces(ID3D12Device *device, Context &ctx, const std::vector<uint8_t> &cs) {
  std::array<D3D12_ROOT_PARAMETER, 3> params{};
  for (UINT i = 0; i < 2; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[i].Constants.ShaderRegister = 0;
    params[i].Constants.RegisterSpace = i == 0 ? 2 : 9;
    params[i].Constants.Num32BitValues = 1;
  }
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].Descriptor.ShaderRegister = 0;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = static_cast<UINT>(params.size());
  rootDesc.pParameters = params.data();
  const auto root = createRootSignature(device, rootDesc);
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get();
  desc.CS = {cs.data(), cs.size()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "CreateComputePipelineState(register spaces)");
  auto output = createBuffer(device, 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = createBuffer(device, 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
  ctx.list->SetPipelineState(pso.Get());
  ctx.list->SetComputeRootSignature(root.Get());
  ctx.list->SetComputeRoot32BitConstant(0, 0x13579bdfu, 0);
  ctx.list->SetComputeRoot32BitConstant(1, 0x2468ace0u, 0);
  ctx.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
  ctx.list->Dispatch(1, 1, 1);
  transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ctx.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 8);
  ctx.submitAndWait();
  void *mapped = nullptr;
  D3D12_RANGE range{0, 8};
  check(readback->Map(0, &range, &mapped), "Map(register spaces)");
  const auto *words = static_cast<const UINT *>(mapped);
  const bool ok = words[0] == 0x13579bdfu && words[1] == 0x2468ace0u;
  std::printf("OWNED_D3D12_DXIL_REGISTER_SPACES b0_space2=0x%08x b0_space9=0x%08x\n", words[0], words[1]);
  readback->Unmap(0, nullptr);
  return ok ? 0 : fail("distinct register spaces aliased");
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
  psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
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

int run(int argc, wchar_t **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: owned-d3d12-dxil.exe <compute.dxil> <vertex.dxil> <pixel.dxil>\n");
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

  Context context(device.Get());
  int failures = 0;
  failures += testRegisterSpaces(device.Get(), context, readFile(L"register-space.dxil"));
  context.reset();
  failures += testDescriptorTable(device.Get(), context, cs);
  context.reset();
  failures += testCompute(device.Get(), context, cs);
  context.reset();
  failures += testGraphics(device.Get(), context, vs, ps);
  if (!failures)
    std::puts("OWNED_D3D12_DXIL_PASS");
  return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  return run(argc, argv);
}
