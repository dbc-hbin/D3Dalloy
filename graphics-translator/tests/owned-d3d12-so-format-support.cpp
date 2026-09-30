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

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kTimeout = 15000;
constexpr UINT kBufferBytes = 64;
constexpr UINT kDataOffset = 16;
constexpr std::uint32_t kGuard = 0xa5a5a5a5;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_SO_FORMAT_SUPPORT_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) fail(step, hr);
}
void require(bool condition, const char *step) {
  if (!condition) fail(step);
}

ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE type,
                               D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)), "buffer");
  return resource;
}

ComPtr<ID3DBlob> compile(const char *entry) {
  // The first VS generates the payload. The second consumes exactly the same bytes
  // through a typed IA element before emitting another SO record.
  static constexpr char source[] = R"hlsl(
struct FloatOut { float4 position : SV_Position; float4 data : TEXCOORD0; };
struct IntOut   { float4 position : SV_Position; int4 data : TEXCOORD0; };
struct UintOut  { float4 position : SV_Position; uint4 data : TEXCOORD0; };
FloatOut emitFloat(uint id : SV_VertexID) {
  FloatOut o; o.position = float4(0, 0, 0, 1); o.data = float4(1.25, -2.5, 3.75, -4.5); return o;
}
IntOut emitInt(uint id : SV_VertexID) {
  IntOut o; o.position = float4(0, 0, 0, 1); o.data = int4(-7, 0x12345678, -99, 2147483647); return o;
}
UintOut emitUint(uint id : SV_VertexID) {
  UintOut o; o.position = float4(0, 0, 0, 1); o.data = uint4(0x89abcdef, 17, 0xfedcba98, 0x76543210); return o;
}
FloatOut readFloat(float4 input : TEXCOORD0) {
  FloatOut o; o.position = float4(0, 0, 0, 1); o.data = input; return o;
}
IntOut readInt(int4 input : TEXCOORD0) {
  IntOut o; o.position = float4(0, 0, 0, 1); o.data = input; return o;
}
UintOut readUint(uint4 input : TEXCOORD0) {
  UintOut o; o.position = float4(0, 0, 0, 1); o.data = input; return o;
}
)hlsl";
  ComPtr<ID3DBlob> shader, errors;
  HRESULT hr = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, entry, "vs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("compile", hr);
  }
  return shader;
}

void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, 0, before, after};
  list->ResourceBarrier(1, &b);
}

struct FormatCase {
  DXGI_FORMAT format;
  const char *name;
  UINT count;
  UINT kind;
};
constexpr FormatCase kCases[] = {
  {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", 1, 0},
  {DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT", 2, 0},
  {DXGI_FORMAT_R32G32B32_FLOAT, "R32G32B32_FLOAT", 3, 0},
  {DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT", 4, 0},
  {DXGI_FORMAT_R32_SINT, "R32_SINT", 1, 1},
  {DXGI_FORMAT_R32G32_SINT, "R32G32_SINT", 2, 1},
  {DXGI_FORMAT_R32G32B32_SINT, "R32G32B32_SINT", 3, 1},
  {DXGI_FORMAT_R32G32B32A32_SINT, "R32G32B32A32_SINT", 4, 1},
  {DXGI_FORMAT_R32_UINT, "R32_UINT", 1, 2},
  {DXGI_FORMAT_R32G32_UINT, "R32G32_UINT", 2, 2},
  {DXGI_FORMAT_R32G32B32_UINT, "R32G32B32_UINT", 3, 2},
  {DXGI_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", 4, 2},
};
constexpr std::uint32_t kExpected[3][4] = {
  {0x3fa00000, 0xc0200000, 0x40700000, 0xc0900000},
  {0xfffffff9, 0x12345678, 0xffffff9d, 0x7fffffff},
  {0x89abcdef, 17, 0xfedcba98, 0x76543210},
};

void capture(ID3D12Device *device, ID3D12RootSignature *root, ID3D12CommandQueue *queue,
             ID3D12Fence *fence, HANDLE event, UINT64 &fenceValue,
             const std::array<ComPtr<ID3DBlob>, 6> &shaders, const FormatCase &c) {
  const UINT size = c.count * 4;
  D3D12_SO_DECLARATION_ENTRY entry = {0, "TEXCOORD", 0, 0, static_cast<BYTE>(c.count), 0};
  D3D12_INPUT_ELEMENT_DESC input = {"TEXCOORD", 0, c.format, 0, 0,
                                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  ComPtr<ID3D12PipelineState> emit, read;
  for (UINT stage = 0; stage < 2; ++stage) {
    auto &shader = shaders[c.kind + 3 * stage];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    desc.StreamOutput = {&entry, 1, &size, 1, D3D12_SO_NO_RASTERIZED_STREAM};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.SampleDesc.Count = 1;
    if (stage) desc.InputLayout = {&input, 1};
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(stage ? &read : &emit)), "so-pso");
  }
  auto first = buffer(device, kBufferBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto second = buffer(device, kBufferBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto firstCount = buffer(device, 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto secondCount = buffer(device, 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto upload = buffer(device, kBufferBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto countUpload = buffer(device, 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto result = buffer(device, 2 * kBufferBytes + 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  std::uint32_t *initial = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&initial)), "upload-map");
  for (UINT i = 0; i < kBufferBytes / 4; ++i) initial[i] = kGuard;
  upload->Unmap(0, nullptr);
  std::uint32_t *initialCount = nullptr;
  check(countUpload->Map(0, nullptr, reinterpret_cast<void **>(&initialCount)), "counter-upload-map");
  initialCount[0] = initialCount[1] = 0;
  countUpload->Unmap(0, nullptr);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)), "list");
  // CopyBufferRegion implicitly promotes the COMMON buffers to COPY_DEST, a write state, so the
  // following transition must name COPY_DEST as its before-state.
  for (ID3D12Resource *target : {first.Get(), second.Get()}) {
    list->CopyBufferRegion(target, 0, upload.Get(), 0, kBufferBytes);
    barrier(list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  }
  for (ID3D12Resource *target : {firstCount.Get(), secondCount.Get()}) {
    list->CopyBufferRegion(target, 0, countUpload.Get(), 0, 8);
    barrier(list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  }
  list->SetGraphicsRootSignature(root);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW firstView = {first->GetGPUVirtualAddress() + kDataOffset,
                                                size, firstCount->GetGPUVirtualAddress()};
  list->SOSetTargets(0, 1, &firstView);
  list->SetPipelineState(emit.Get());
  list->DrawInstanced(1, 1, 0, 0);
  barrier(list.Get(), first.Get(), D3D12_RESOURCE_STATE_STREAM_OUT,
          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  D3D12_VERTEX_BUFFER_VIEW vertex = {first->GetGPUVirtualAddress() + kDataOffset, size, size};
  D3D12_STREAM_OUTPUT_BUFFER_VIEW secondView = {second->GetGPUVirtualAddress() + kDataOffset,
                                                 size, secondCount->GetGPUVirtualAddress()};
  list->IASetVertexBuffers(0, 1, &vertex);
  list->SOSetTargets(0, 1, &secondView);
  list->SetPipelineState(read.Get());
  list->DrawInstanced(1, 1, 0, 0);
  barrier(list.Get(), first.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(list.Get(), second.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(list.Get(), firstCount.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(list.Get(), secondCount.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(result.Get(), 0, first.Get(), 0, kBufferBytes);
  list->CopyBufferRegion(result.Get(), kBufferBytes, second.Get(), 0, kBufferBytes);
  list->CopyBufferRegion(result.Get(), 2 * kBufferBytes, firstCount.Get(), 0, 8);
  list->CopyBufferRegion(result.Get(), 2 * kBufferBytes + 8, secondCount.Get(), 0, 8);
  check(list->Close(), "close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  check(queue->Signal(fence, ++fenceValue), "signal");
  check(fence->SetEventOnCompletion(fenceValue, event), "set-event");
  require(WaitForSingleObject(event, kTimeout) == WAIT_OBJECT_0, "wait");
  std::uint32_t *words = nullptr;
  check(result->Map(0, nullptr, reinterpret_cast<void **>(&words)), "readback-map");
  for (UINT pass = 0; pass < 2; ++pass) {
    const std::uint32_t *data = words + pass * kBufferBytes / 4;
    for (UINT i = 0; i < kBufferBytes / 4; ++i) {
      const std::uint32_t expected = (i >= kDataOffset / 4 && i < kDataOffset / 4 + c.count)
          ? kExpected[c.kind][i - kDataOffset / 4] : kGuard;
      if (data[i] != expected) {
        std::fprintf(stderr, "OWNED_D3D12_SO_FORMAT_SUPPORT_MISMATCH format=%s pass=%u word=%u expected=%08x actual=%08x\n",
                     c.name, pass, i, expected, data[i]);
        fail("payload-or-guard");
      }
    }
    require(words[2 * kBufferBytes / 4 + 2 * pass] == size, "filled-size");
    require(words[2 * kBufferBytes / 4 + 2 * pass + 1] == 0, "filled-size-guard");
  }
  std::printf("OWNED_D3D12_SO_FORMAT_SUPPORT_GPU format=%s words=%u bits=", c.name, c.count);
  for (UINT i = 0; i < c.count; ++i)
    std::printf("%s%08x", i ? "," : "", words[kDataOffset / 4 + i]);
  std::printf(" bytes=%u counters=%u,%u guards=1 ia=1\n", size,
              words[2 * kBufferBytes / 4], words[2 * kBufferBytes / 4 + 2]);
  result->Unmap(0, nullptr);
}
} // namespace

int main() {
  auto module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using BackendId = const char *(WINAPI *)();
  auto id = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(id && std::strcmp(id(), "owned-graphics-d3d12") == 0, "backend");
  std::printf("OWNED_D3D12_SO_FORMAT_SUPPORT_BACKEND id=%s\n", id());
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT |
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> rootBlob, errors;
  ComPtr<ID3D12RootSignature> root;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors), "serialize");
  check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "root");
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "event");
  UINT64 fenceValue = 0;
  std::array<ComPtr<ID3DBlob>, 6> shaders = {compile("emitFloat"), compile("emitInt"), compile("emitUint"),
                                           compile("readFloat"), compile("readInt"), compile("readUint")};
  // GPU evidence deliberately precedes the capability query: the baseline must
  // show working SO and typed IA interpretation even when reporting omits SO_BUFFER.
  for (const auto &c : kCases)
    capture(device.Get(), root.Get(), queue.Get(), fence.Get(), event, fenceValue, shaders, c);
  const FormatCase negative[] = {
    {DXGI_FORMAT_UNKNOWN, "UNKNOWN", 0, 0},
    {DXGI_FORMAT_R32_TYPELESS, "R32_TYPELESS", 0, 0},
    {DXGI_FORMAT_R32G32B32_TYPELESS, "R32G32B32_TYPELESS", 0, 0},
    {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT", 0, 0},
    {DXGI_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", 0, 0},
    {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 0, 0},
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, "R32_FLOAT_X8X24_TYPELESS", 0, 0},
    {DXGI_FORMAT_D32_FLOAT, "D32_FLOAT", 0, 0},
  };
  for (const auto &c : negative) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {c.format};
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "negative-query");
    require(!(support.Support1 & D3D12_FORMAT_SUPPORT1_SO_BUFFER), "negative-so-buffer");
    std::printf("OWNED_D3D12_SO_FORMAT_SUPPORT_NEGATIVE format=%s so_buffer=0\n", c.name);
  }
  for (const auto &c : kCases) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {c.format};
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "positive-query");
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_SO_BUFFER)) {
      std::fprintf(stderr, "OWNED_D3D12_SO_FORMAT_SUPPORT_MISSING format=%s support1=0x%08x\n",
                   c.name, static_cast<unsigned>(support.Support1));
      fail("missing-so-buffer");
    }
    std::printf("OWNED_D3D12_SO_FORMAT_SUPPORT_CAP format=%s so_buffer=1\n", c.name);
  }
  CloseHandle(event);
  std::puts("OWNED_D3D12_SO_FORMAT_SUPPORT_PASS gpu=12 capabilities=12 negatives=8 ia=12");
  return 0;
}
