#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kTimeoutMs = 15000;
constexpr UINT kSmallElements = 64;
constexpr UINT kDrawElements = 8;
constexpr UINT kFirstPrimary = 8;
constexpr UINT kFirstSecondary = 5;
constexpr UINT kBoundaryFirst = 16;
constexpr UINT kBoundaryElements = D3D12_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH;
constexpr UINT kBoundaryTotal = kBoundaryFirst + kBoundaryElements + 16;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_BUFFER_RTV_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void require(bool condition, const char *step) {
  if (!condition)
    fail(step);
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using BackendId = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backendId != nullptr, "owned-backend-id-export");
  const char *id = backendId();
  require(id && std::strcmp(id, "owned-graphics-d3d12") == 0, "owned-backend-id");
  std::printf("OWNED_D3D12_BUFFER_RTV_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties = {};
  properties.Type = type;
  properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  properties.CreationNodeMask = 1;
  properties.VisibleNodeMask = 1;
  return properties;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
  ComPtr<ID3D12Resource> resource;
  const auto heap = heapProperties(heapType);
  const auto desc = bufferDesc(bytes, flags);
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)), "create-buffer");
  return resource;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

ComPtr<ID3DBlob> compileShader(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
                                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                0, &code, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("compile-shader", hr);
  }
  return code;
}

ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device *device) {
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("serialize-root-signature", hr);
  }
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "create-root-signature");
  return root;
}

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;

  ~Context() {
    if (event)
      CloseHandle(event);
  }

  void submit(ID3D12GraphicsCommandList *list) {
    check(list->Close(), "close-list");
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "set-event");
      require(WaitForSingleObject(event, kTimeoutMs) == WAIT_OBJECT_0, "wait");
    }
  }
};

void createContext(Context &context) {
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
    DXGI_ADAPTER_DESC1 desc = {};
    check(adapter->GetDesc1(&desc), "adapter-desc");
    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&context.device))))
      break;
    adapter.Reset();
  }
  if (!context.device)
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&context.device)),
          "device");

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(context.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context.queue)), "queue");
  check(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context.fence)),
        "fence");
  context.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(context.event != nullptr, "event");
}

void uploadWords(ID3D12Resource *upload, const std::vector<std::uint32_t> &words) {
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "upload-map");
  std::memcpy(mapped, words.data(), words.size() * sizeof(words[0]));
  upload->Unmap(0, nullptr);
}

std::uint32_t initialSmall(UINT target, UINT index) {
  return 0x11000000u | (target << 16) | (index * 0x101u + 0x33u);
}

std::uint32_t initialBoundary(UINT index) {
  return 0x5a000000u ^ (index * 0x00010101u);
}

void validateSmall(ID3D12Resource *readback, UINT target) {
  const SIZE_T bytes = kSmallElements * sizeof(std::uint32_t);
  D3D12_RANGE range{0, bytes};
  std::uint32_t *mapped = nullptr;
  check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "small-readback-map");
  const UINT first = target == 0 ? kFirstPrimary : kFirstSecondary;
  for (UINT index = 0; index < kSmallElements; ++index) {
    std::uint32_t expected = initialSmall(target, index);
    if (index >= first && index < first + kDrawElements)
      expected = target == 0 ? 0xff0000ffu : 0xff00ff00u;
    if (target == 0 && index >= first + 10 && index < first + 12)
      expected = 0xffffff00u;
    if (mapped[index] != expected) {
      std::fprintf(stderr,
                   "OWNED_D3D12_BUFFER_RTV_MISMATCH target=%u index=%u expected=0x%08lx actual=0x%08lx\n",
                   target, index, static_cast<unsigned long>(expected),
                   static_cast<unsigned long>(mapped[index]));
      D3D12_RANGE written{0, 0};
      readback->Unmap(0, &written);
      fail("small-readback");
    }
  }
  D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
}

void validateBoundary(ID3D12Resource *readback) {
  const SIZE_T bytes = kBoundaryTotal * sizeof(std::uint32_t);
  D3D12_RANGE range{0, bytes};
  std::uint32_t *mapped = nullptr;
  check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "boundary-readback-map");
  for (UINT index = 0; index < kBoundaryTotal; ++index) {
    const bool inView = index >= kBoundaryFirst && index < kBoundaryFirst + kBoundaryElements;
    const std::uint32_t expected = inView ? 0xffff00ffu : initialBoundary(index);
    if (mapped[index] != expected) {
      std::fprintf(stderr,
                   "OWNED_D3D12_BUFFER_RTV_MISMATCH target=boundary index=%u expected=0x%08lx actual=0x%08lx\n",
                   index, static_cast<unsigned long>(expected),
                   static_cast<unsigned long>(mapped[index]));
      D3D12_RANGE written{0, 0};
      readback->Unmap(0, &written);
      fail("boundary-readback");
    }
  }
  D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
}

void runBufferRtv(Context &context) {
  const char *shader = R"(
struct VSOut { float4 position : SV_Position; };
VSOut vsMain(uint id : SV_VertexID) {
  const float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
  VSOut output;
  output.position = float4(positions[id], 0.0, 1.0);
  return output;
}
struct PSOut { float4 primary : SV_Target0; float4 secondary : SV_Target1; };
PSOut psMain() {
  PSOut output;
  output.primary = float4(1.0, 0.0, 0.0, 1.0);
  output.secondary = float4(0.0, 1.0, 0.0, 1.0);
  return output;
}
)";
  const auto vs = compileShader(shader, "vsMain", "vs_5_0");
  const auto ps = compileShader(shader, "psMain", "ps_5_0");
  const auto root = createRootSignature(context.device.Get());

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = root.Get();
  psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.SampleMask = UINT_MAX;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.RasterizerState.DepthClipEnable = TRUE;
  psoDesc.DepthStencilState.DepthEnable = FALSE;
  psoDesc.DepthStencilState.StencilEnable = FALSE;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 2;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psoDesc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psoDesc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  check(context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "create-pso");

  const UINT smallBytes = kSmallElements * sizeof(std::uint32_t);
  const UINT boundaryBytes = kBoundaryTotal * sizeof(std::uint32_t);
  auto primary = createBuffer(context.device.Get(), smallBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto secondary = createBuffer(context.device.Get(), smallBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto boundary = createBuffer(context.device.Get(), boundaryBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto primaryUpload = createBuffer(context.device.Get(), smallBytes, D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
  auto secondaryUpload = createBuffer(context.device.Get(), smallBytes, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
  auto boundaryUpload = createBuffer(context.device.Get(), boundaryBytes, D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
  auto primaryReadback = createBuffer(context.device.Get(), smallBytes, D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
  auto secondaryReadback = createBuffer(context.device.Get(), smallBytes, D3D12_HEAP_TYPE_READBACK,
                                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
  auto boundaryReadback = createBuffer(context.device.Get(), boundaryBytes, D3D12_HEAP_TYPE_READBACK,
                                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);

  std::vector<std::uint32_t> words(kSmallElements);
  for (UINT index = 0; index < kSmallElements; ++index)
    words[index] = initialSmall(0, index);
  uploadWords(primaryUpload.Get(), words);
  for (UINT index = 0; index < kSmallElements; ++index)
    words[index] = initialSmall(1, index);
  uploadWords(secondaryUpload.Get(), words);
  words.resize(kBoundaryTotal);
  for (UINT index = 0; index < kBoundaryTotal; ++index)
    words[index] = initialBoundary(index);
  uploadWords(boundaryUpload.Get(), words);

  const std::array<std::uint64_t, 2> predicates = {0, 1};
  auto predicateUpload = createBuffer(context.device.Get(), sizeof(predicates), D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
  void *predicateMapped = nullptr;
  check(predicateUpload->Map(0, nullptr, &predicateMapped), "predicate-upload-map");
  std::memcpy(predicateMapped, predicates.data(), sizeof(predicates));
  predicateUpload->Unmap(0, nullptr);
  auto predicate = createBuffer(context.device.Get(), sizeof(predicates), D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);

  D3D12_DESCRIPTOR_HEAP_DESC sourceHeapDesc = {};
  sourceHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  sourceHeapDesc.NumDescriptors = 3;
  ComPtr<ID3D12DescriptorHeap> sourceHeap;
  ComPtr<ID3D12DescriptorHeap> activeHeap;
  check(context.device->CreateDescriptorHeap(&sourceHeapDesc, IID_PPV_ARGS(&sourceHeap)),
        "source-rtv-heap");
  check(context.device->CreateDescriptorHeap(&sourceHeapDesc, IID_PPV_ARGS(&activeHeap)),
        "active-rtv-heap");
  const UINT descriptorSize =
      context.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const auto sourceStart = sourceHeap->GetCPUDescriptorHandleForHeapStart();
  const auto activeStart = activeHeap->GetCPUDescriptorHandleForHeapStart();
  auto offsetHandle = [descriptorSize](D3D12_CPU_DESCRIPTOR_HANDLE start, UINT index) {
    start.ptr += static_cast<SIZE_T>(descriptorSize) * index;
    return start;
  };
  D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
  rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rtv.ViewDimension = D3D12_RTV_DIMENSION_BUFFER;
  rtv.Buffer.FirstElement = kFirstPrimary;
  rtv.Buffer.NumElements = 32;
  context.device->CreateRenderTargetView(primary.Get(), &rtv, sourceStart);
  rtv.Buffer.FirstElement = kFirstSecondary;
  context.device->CreateRenderTargetView(secondary.Get(), &rtv, offsetHandle(sourceStart, 1));
  rtv.Buffer.FirstElement = kBoundaryFirst;
  rtv.Buffer.NumElements = kBoundaryElements;
  context.device->CreateRenderTargetView(boundary.Get(), &rtv, offsetHandle(sourceStart, 2));
  context.device->CopyDescriptorsSimple(3, activeStart, sourceStart, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&allocator)), "allocator");
  check(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                          pso.Get(), IID_PPV_ARGS(&list)), "command-list");
  list->CopyBufferRegion(primary.Get(), 0, primaryUpload.Get(), 0, smallBytes);
  list->CopyBufferRegion(secondary.Get(), 0, secondaryUpload.Get(), 0, smallBytes);
  list->CopyBufferRegion(boundary.Get(), 0, boundaryUpload.Get(), 0, boundaryBytes);
  list->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), 0, sizeof(predicates));
  std::array<D3D12_RESOURCE_BARRIER, 4> toRender = {
      transition(primary.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
      transition(secondary.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
      transition(boundary.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
      transition(predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION)};
  list->ResourceBarrier(static_cast<UINT>(toRender.size()), toRender.data());

  list->SetGraphicsRootSignature(root.Get());
  list->SetPipelineState(pso.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kDrawElements), 1.0f, 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(kDrawElements), 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(2, &activeStart, TRUE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);

  const float rejected[] = {0.0f, 0.0f, 1.0f, 1.0f};
  list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearRenderTargetView(activeStart, rejected, 0, nullptr);
  const float cyan[] = {0.0f, 1.0f, 1.0f, 1.0f};
  const D3D12_RECT twoElements = {10, 0, 12, 1};
  list->SetPredication(predicate.Get(), sizeof(std::uint64_t), D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearRenderTargetView(activeStart, cyan, 1, &twoElements);
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

  const float magenta[] = {1.0f, 0.0f, 1.0f, 1.0f};
  list->ClearRenderTargetView(offsetHandle(activeStart, 2), magenta, 0, nullptr);

  std::array<D3D12_RESOURCE_BARRIER, 3> toCopy = {
      transition(primary.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(secondary.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(boundary.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  list->ResourceBarrier(static_cast<UINT>(toCopy.size()), toCopy.data());
  list->CopyBufferRegion(primaryReadback.Get(), 0, primary.Get(), 0, smallBytes);
  list->CopyBufferRegion(secondaryReadback.Get(), 0, secondary.Get(), 0, smallBytes);
  list->CopyBufferRegion(boundaryReadback.Get(), 0, boundary.Get(), 0, boundaryBytes);
  context.submit(list.Get());

  validateSmall(primaryReadback.Get(), 0);
  validateSmall(secondaryReadback.Get(), 1);
  std::puts("OWNED_D3D12_BUFFER_RTV_DRAW_READBACK_PASS first_primary=8 first_secondary=5 pixels=8 mrt=2 descriptor_copy=1");
  std::puts("OWNED_D3D12_BUFFER_RTV_PREDICATED_CLEAR_PASS rejected_full=1 accepted_rect_elements=2");
  validateBoundary(boundaryReadback.Get());
  std::puts("OWNED_D3D12_BUFFER_RTV_BOUNDARY_PASS first_element=16 elements=16384 prefix=16 suffix=16");
}

} // namespace

int main() {
  requireOwnedBackend();
  Context context;
  createContext(context);
  runBufferRtv(context);
  std::puts("OWNED_D3D12_BUFFER_RTV_PASS draw=1 first_element=1 descriptor_copy=1 mrt=1 predicated_clear=1 boundary_16384=1");
  return 0;
}
