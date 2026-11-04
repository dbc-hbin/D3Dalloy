// D3D12 geometry-shader fixture: GS pipelines (DXBC gs_5_0 and frozen DXIL gs_6_0) against a CPU oracle.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "owned-d3d12-early-depth-oracle.inc"

using Microsoft::WRL::ComPtr;
namespace {
constexpr char kPrefix[] = "OWNED_D3D12_GEOMETRY";
constexpr UINT kSize = 32;
constexpr UINT kPitch = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_CASE_FAIL step=%s hr=0x%08lx\n", kPrefix, step, static_cast<unsigned long>(hr));
  std::fflush(stderr);
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}
void require(bool ok, const char *step) {
  if (!ok)
    fail(step);
}

std::vector<uint8_t> readFile(const std::string &name) {
  HANDLE file = CreateFileA(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("open-input", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
    fail("size-input");
  std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
  DWORD count = 0;
  if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
    fail("read-input");
  CloseHandle(file);
  return bytes;
}

std::vector<uint8_t> hlslSource;
bool useDxil = false;

// Inserts `dcl_gsinstances count` (D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT = 206, 2 tokens) after the SHEX header:
// the declaration fxc emits for [instance(count)]. Chunk offsets, the SHEX token count and the container size are
// updated; the container hash is not recomputed (the runtime does not validate it).
std::vector<uint8_t> addGSInstances(std::vector<uint8_t> dxbc, uint32_t count) {
  auto word = [&](size_t at) -> uint32_t & { return *reinterpret_cast<uint32_t *>(dxbc.data() + at); };
  const uint32_t chunks = word(28);
  size_t shex = 0;
  uint32_t shexIndex = 0;
  for (uint32_t i = 0; i < chunks; ++i) {
    const size_t at = word(32 + 4 * i);
    if (!std::memcmp(dxbc.data() + at, "SHEX", 4) || !std::memcmp(dxbc.data() + at, "SHDR", 4)) {
      shex = at;
      shexIndex = i;
    }
  }
  require(shex != 0, "gs-instances-shex");
  const uint32_t tokens[2] = {206u | (2u << 24), count};
  dxbc.insert(dxbc.begin() + shex + 16, reinterpret_cast<const uint8_t *>(tokens),
              reinterpret_cast<const uint8_t *>(tokens) + sizeof(tokens));
  word(shex + 4) += sizeof(tokens);  // chunk byte size
  word(shex + 12) += 2;              // SHEX length in tokens
  word(24) += sizeof(tokens);        // container size
  for (uint32_t i = shexIndex + 1; i < chunks; ++i)
    word(32 + 4 * i) += sizeof(tokens);
  return dxbc;
}

// DXBC: compile the entry from the shared HLSL. DXIL: load the frozen <entry>.dxil beside the exe.
std::vector<uint8_t> shader(const char *entry, const char *dxbcTarget) {
  if (useDxil)
    return readFile(std::string("geometry-") + entry + ".dxil");
  const D3D_SHADER_MACRO defines[] = {{"VKD3D_NO_GS_INSTANCES", "1"}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> result, errors;
  HRESULT hr = D3DCompile(hlslSource.data(), hlslSource.size(), "geometry.hlsl", defines, nullptr, entry,
                          dxbcTarget, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail(entry, hr);
  }
  auto *bytes = static_cast<const uint8_t *>(result->GetBufferPointer());
  std::vector<uint8_t> code(bytes, bytes + result->GetBufferSize());
  return std::strcmp(entry, "gs_tri") ? code : addGSInstances(std::move(code), 2);
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h = {};
  h.Type = type;
  h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  auto h = heap(type);
  ComPtr<ID3D12Resource> r;
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
  return r;
}
void upload(ID3D12Resource *resource, const void *data, size_t size) {
  void *mapped = nullptr;
  check(resource->Map(0, nullptr, &mapped), "map-upload");
  std::memcpy(mapped, data, size);
  resource->Unmap(0, nullptr);
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 fenceValue = 0;
  HANDLE event = nullptr;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12DescriptorHeap> rtvs;
  ComPtr<ID3D12Resource> target, resolved, readback;
  UINT samples = 1;
  ~Context() {
    if (event)
      CloseHandle(event);
  }
  void begin() {
    check(allocator->Reset(), "allocator-reset");
    check(list->Reset(allocator.Get(), nullptr), "list-reset");
  }
  void submit() {
    check(list->Close(), "close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "event");
      require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "wait");
    }
    check(device->GetDeviceRemovedReason(), "device-removed");
  }
};

void init(Context &c) {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "module");
  using Backend = const char *(WINAPI *)();
  auto backend = reinterpret_cast<Backend>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "backend");
  std::printf("%s_BACKEND id=%s\n", kPrefix, backend());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d = {};
    check(adapter->GetDesc1(&d), "adapter-desc");
    if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device))))
      break;
    adapter.Reset();
  }
  require(c.device != nullptr, "device");
  D3D12_COMMAND_QUEUE_DESC q = {};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(c.device->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)), "queue");
  check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator)), "allocator");
  check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&c.list)),
        "list");
  check(c.list->Close(), "list-close");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");
  c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(c.event != nullptr, "event-create");

  // b0: 4 constants visible to the GS only (proves GEOMETRY visibility routing).
  D3D12_ROOT_PARAMETER constant = {};
  constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  constant.Constants.ShaderRegister = 0;
  constant.Constants.Num32BitValues = 4;
  constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
  D3D12_ROOT_SIGNATURE_DESC root = {};
  root.NumParameters = 1;
  root.pParameters = &constant;
  root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
  ComPtr<ID3DBlob> code, errors;
  check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors), "root-serialize");
  check(c.device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&c.root)),
        "root");
  D3D12_DESCRIPTOR_HEAP_DESC rtv = {};
  rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv.NumDescriptors = 1;
  check(c.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&c.rtvs)), "rtv-heap");
  c.readback = buffer(c.device.Get(), kPitch * kSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
}

void createTarget(Context &c, UINT samples) {
  c.samples = samples;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = d.Height = kSize;
  d.DepthOrArraySize = d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = samples;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = d.Format;
  c.target.Reset();
  c.resolved.Reset();
  check(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                          IID_PPV_ARGS(&c.target)),
        "target");
  if (samples > 1) {
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                            IID_PPV_ARGS(&c.resolved)),
          "resolved");
  }
  c.device->CreateRenderTargetView(c.target.Get(), nullptr, c.rtvs->GetCPUDescriptorHandleForHeapStart());
}

struct Image {
  std::array<uint32_t, kSize * kSize> texels{};
  // 2x2 quad whose top-left pixel is (x-1, y-1): a GS quad centred on pixel corner (x, y).
  void quad(int x, int y, uint32_t color) {
    for (int dy = -1; dy <= 0; ++dy)
      for (int dx = -1; dx <= 0; ++dx)
        texels[(y + dy) * kSize + x + dx] = color;
  }
};

void setupPass(Context &c) {
  auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
  const float zero[4] = {};
  c.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
  D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
  c.list->RSSetViewports(1, &viewport);
  c.list->RSSetScissorRects(1, &scissor);
  c.list->SetGraphicsRootSignature(c.root.Get());
}

Image finishPass(Context &c) {
  ID3D12Resource *source = c.target.Get();
  if (c.samples > 1) {
    D3D12_RESOURCE_BARRIER before[] = {
        transition(c.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE)};
    c.list->ResourceBarrier(1, before);
    c.list->ResolveSubresource(c.resolved.Get(), 0, c.target.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_BARRIER after[] = {
        transition(c.target.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(c.resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    c.list->ResourceBarrier(2, after);
    source = c.resolved.Get();
  } else {
    D3D12_RESOURCE_BARRIER b = transition(c.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->ResourceBarrier(1, &b);
  }
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = c.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch};
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER back = transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           c.samples > 1 ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                                                         : D3D12_RESOURCE_STATE_RENDER_TARGET);
  c.list->ResourceBarrier(1, &back);
  c.submit();
  Image image;
  void *mapped = nullptr;
  D3D12_RANGE range = {0, kPitch * kSize};
  check(c.readback->Map(0, &range, &mapped), "map-readback");
  for (UINT y = 0; y < kSize; ++y)
    std::memcpy(image.texels.data() + y * kSize, static_cast<uint8_t *>(mapped) + y * kPitch, kSize * 4);
  D3D12_RANGE none = {};
  c.readback->Unmap(0, &none);
  return image;
}

ComPtr<ID3D12PipelineState> pipeline(Context &c, D3D12_PRIMITIVE_TOPOLOGY_TYPE type, const std::vector<uint8_t> &vs,
                                     const std::vector<uint8_t> &gs, const std::vector<uint8_t> &ps,
                                     HRESULT expect = S_OK) {
  D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = c.root.Get();
  d.VS = {vs.data(), vs.size()};
  d.GS = {gs.data(), gs.size()};
  d.PS = {ps.data(), ps.size()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {&element, 1};
  d.PrimitiveTopologyType = type;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = c.samples;
  ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  if (hr != expect) {
    std::fprintf(stderr, "%s_PSO hr=0x%08lx expected=0x%08lx\n", kPrefix, static_cast<unsigned long>(hr),
                 static_cast<unsigned long>(expect));
    fail("pso");
  }
  return pso;
}

struct Float2 {
  float x, y;
};

ComPtr<ID3D12Resource> vertexBuffer(Context &c, const std::vector<Float2> &points, D3D12_VERTEX_BUFFER_VIEW &view) {
  auto vb = buffer(c.device.Get(), points.size() * sizeof(Float2), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(vb.Get(), points.data(), points.size() * sizeof(Float2));
  view = {vb->GetGPUVirtualAddress(), UINT(points.size() * sizeof(Float2)), sizeof(Float2)};
  return vb;
}

// Pixel corner (x, y) of the kSize target in clip space: a 2x2 quad centred there covers pixels x-1..x, y-1..y.
Float2 corner(float x, float y) {
  return {x / kSize * 2.0f - 1.0f, 1.0f - y / kSize * 2.0f};
}
// Pixel centre of (x, y).
Float2 centre(float x, float y) {
  return corner(x + 0.5f, y + 0.5f);
}

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  return r | g << 8 | b << 16 | a << 24;
}
// geometry.hlsl encode(): id n -> UNORM8 n + 1.
uint32_t id8(uint32_t n) {
  return n + 1;
}

int compare(const char *name, const Image &image, const Image &expected) {
  int nonzero = 0, shown = 0;
  bool ok = true;
  for (UINT i = 0; i < kSize * kSize; ++i) {
    if (image.texels[i] != expected.texels[i]) {
      ok = false;
      if (shown++ < 24)
        std::fprintf(stderr, "%s_MISMATCH shader=%s case=%s x=%u y=%u got=%08x want=%08x\n", kPrefix,
                     useDxil ? "dxil" : "dxbc", name, i % kSize, i / kSize, image.texels[i], expected.texels[i]);
    }
    nonzero += expected.texels[i] != 0;
  }
  if (!ok)
    fail(name);
  require(nonzero > 0, "oracle-empty");
  std::printf("%s_CASE_PASS shader=%s case=%s nonzero=%d\n", kPrefix, useDxil ? "dxil" : "dxbc", name, nonzero);
  return 1;
}

struct Shaders {
  std::vector<uint8_t> vs, ps, early, quad, tri, linein, lineadj, triadj, line, points, streams2, streams4;
};

struct Draw {
  D3D12_PRIMITIVE_TOPOLOGY topology;
  std::vector<Float2> vertices;
  std::vector<uint16_t> indices; // empty: non-indexed
  UINT count = 0, instances = 1;
  bool indirect = false;
  bool statistics = false;
  bool predicate = false, predicateValue = false; // predicate: SetPredication(EQUAL_ZERO) over a buffer holding value
  std::array<float, 4> color = {1, 0, 0.5f, 1};
};

struct DrawResult {
  Image image;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
};

DrawResult run(Context &c, ID3D12PipelineState *pso, const Draw &draw, HRESULT expectClose = S_OK) {
  DrawResult result;
  D3D12_VERTEX_BUFFER_VIEW view;
  auto vb = vertexBuffer(c, draw.vertices, view);
  ComPtr<ID3D12Resource> ib, args, predicate;
  if (!draw.indices.empty()) {
    ib = buffer(c.device.Get(), draw.indices.size() * 2 + 2, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ);
    upload(ib.Get(), draw.indices.data(), draw.indices.size() * 2);
  }
  const bool indexed = !draw.indices.empty();
  if (draw.indirect) {
    // Two commands: the real draw followed by an empty one (count 0 must draw nothing).
    D3D12_DRAW_INDEXED_ARGUMENTS words[2] = {{draw.count, draw.instances, 0, 0, 0}, {}};
    D3D12_DRAW_ARGUMENTS plain[2] = {{draw.count, draw.instances, 0, 0}, {}};
    args = buffer(c.device.Get(), sizeof(words), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (indexed)
      upload(args.Get(), words, sizeof(words));
    else
      upload(args.Get(), plain, sizeof(plain));
  }
  if (draw.predicate) {
    predicate = buffer(c.device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const uint64_t value = draw.predicateValue;
    upload(predicate.Get(), &value, 8);
  }
  ComPtr<ID3D12CommandSignature> signature;
  if (draw.indirect) {
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    check(c.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "signature");
  }
  ComPtr<ID3D12QueryHeap> queries;
  ComPtr<ID3D12Resource> statistics;
  if (draw.statistics) {
    D3D12_QUERY_HEAP_DESC q = {};
    q.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    q.Count = 1;
    check(c.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "query-heap");
    statistics = buffer(c.device.Get(), sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), D3D12_HEAP_TYPE_READBACK,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  }

  c.begin();
  setupPass(c);
  c.list->SetPipelineState(pso);
  c.list->SetGraphicsRoot32BitConstants(0, 4, draw.color.data(), 0);
  c.list->IASetPrimitiveTopology(draw.topology);
  c.list->IASetVertexBuffers(0, 1, &view);
  if (indexed) {
    D3D12_INDEX_BUFFER_VIEW index = {ib->GetGPUVirtualAddress(), UINT(draw.indices.size() * 2 + 2),
                                     DXGI_FORMAT_R16_UINT};
    c.list->IASetIndexBuffer(&index);
  }
  if (draw.predicate)
    c.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  if (draw.statistics)
    c.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  if (draw.indirect)
    c.list->ExecuteIndirect(signature.Get(), 2, args.Get(), 0, nullptr, 0);
  else if (indexed)
    c.list->DrawIndexedInstanced(draw.count, draw.instances, 0, 0, 0);
  else
    c.list->DrawInstanced(draw.count, draw.instances, 0, 0);
  if (draw.statistics) {
    c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    c.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, statistics.Get(), 0);
  }
  if (draw.predicate)
    c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  if (expectClose != S_OK) {
    HRESULT hr = c.list->Close();
    if (hr != expectClose) {
      std::fprintf(stderr, "%s_CLOSE hr=0x%08lx expected=0x%08lx\n", kPrefix, static_cast<unsigned long>(hr),
                   static_cast<unsigned long>(expectClose));
      fail("close");
    }
    // A list whose Close failed cannot be reset; continue on a fresh one.
    c.list.Reset();
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&c.list)),
          "list-recreate");
    check(c.list->Close(), "list-recreate-close");
    return result;
  }
  result.image = finishPass(c);
  if (draw.statistics) {
    void *mapped = nullptr;
    D3D12_RANGE range = {0, sizeof(result.statistics)};
    check(statistics->Map(0, &range, &mapped), "map-statistics");
    std::memcpy(&result.statistics, mapped, sizeof(result.statistics));
    D3D12_RANGE none = {};
    statistics->Unmap(0, &none);
  }
  return result;
}

int expectStatistics(const char *name, const D3D12_QUERY_DATA_PIPELINE_STATISTICS &s, uint64_t iaVertices,
                     uint64_t iaPrimitives, uint64_t gsInvocations, uint64_t gsPrimitives, uint64_t psMin) {
  const bool ok = s.IAVertices == iaVertices && s.IAPrimitives == iaPrimitives && s.VSInvocations == iaVertices &&
                  s.GSInvocations == gsInvocations && s.GSPrimitives == gsPrimitives &&
                  s.CInvocations == gsPrimitives && s.CPrimitives == gsPrimitives && s.PSInvocations >= psMin &&
                  s.HSInvocations == 0 && s.DSInvocations == 0 && s.CSInvocations == 0;
  std::printf("%s_STATS shader=%s case=%s ia=%llu,%llu vs=%llu gs=%llu,%llu c=%llu,%llu ps=%llu hs=%llu ds=%llu "
              "cs=%llu\n",
              kPrefix, useDxil ? "dxil" : "dxbc", name, s.IAVertices, s.IAPrimitives, s.VSInvocations,
              s.GSInvocations, s.GSPrimitives, s.CInvocations, s.CPrimitives, s.PSInvocations, s.HSInvocations,
              s.DSInvocations, s.CSInvocations);
  if (!ok)
    fail(name);
  std::printf("%s_CASE_PASS shader=%s case=%s-statistics\n", kPrefix, useDxil ? "dxil" : "dxbc", name);
  return 1;
}

constexpr uint32_t kRed = 0xff8000ffu; // (1, 0, 0.5, 1)

// Points on a grid of 2x2 quads; used by the point/amplification cases.
std::vector<Float2> pointGrid(const std::vector<std::array<int, 2>> &corners) {
  std::vector<Float2> points;
  for (auto [x, y] : corners)
    points.push_back(corner(float(x), float(y)));
  return points;
}

/*
 * Point -> 2x2 quad amplification, GEOMETRY-visible root constants. Also: draw instancing (SV_InstanceID moves the
 * second instance 16 pixels right), statistics, predication (true -> drawn, false -> nothing) and ExecuteIndirect.
 */
int casesPoints(Context &c, const Shaders &s) {
  int cases = 0;
  auto pso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.quad, s.ps);
  const std::vector<std::array<int, 2>> corners = {{2, 2}, {10, 4}, {12, 20}, {6, 28}};
  Draw draw{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, pointGrid(corners)};
  draw.count = UINT(corners.size());
  Image expected;
  for (auto [x, y] : corners)
    expected.quad(x, y, kRed);
  cases += compare("amplify", run(c, pso.Get(), draw).image, expected);

  draw.statistics = true;
  auto stats = run(c, pso.Get(), draw);
  cases += compare("amplify-statistics-image", stats.image, expected);
  cases += expectStatistics("amplify", stats.statistics, 4, 4, 4, 8, 16);

  Draw instanced = draw;
  instanced.statistics = false;
  instanced.instances = 2;
  Image twice = expected;
  for (auto [x, y] : corners)
    twice.quad(x + 16, y, kRed);
  cases += compare("draw-instanced", run(c, pso.Get(), instanced).image, twice);

  Draw indexed = draw;
  indexed.statistics = false;
  indexed.indices = {3, 1, 0};
  indexed.count = 3;
  Image subset;
  for (int i : {3, 1, 0})
    subset.quad(corners[i][0], corners[i][1], kRed);
  cases += compare("indexed", run(c, pso.Get(), indexed).image, subset);

  Draw predicated = draw;
  predicated.statistics = false;
  predicated.predicate = true;
  predicated.predicateValue = true; // EQUAL_ZERO over 1: draw executes
  cases += compare("predicate-pass", run(c, pso.Get(), predicated).image, expected);
  predicated.predicateValue = false; // EQUAL_ZERO over 0: skipped
  predicated.statistics = true;
  auto skipped = run(c, pso.Get(), predicated);
  const bool empty = std::all_of(skipped.image.texels.begin(), skipped.image.texels.end(),
                                 [](uint32_t t) { return t == 0; });
  if (!empty || skipped.statistics.GSInvocations || skipped.statistics.IAVertices)
    fail("predicate-skip");
  std::printf("%s_CASE_PASS shader=%s case=predicate-skip\n", kPrefix, useDxil ? "dxil" : "dxbc");
  ++cases;

  Draw indirect = instanced;
  indirect.indirect = true;
  cases += compare("indirect", run(c, pso.Get(), indirect).image, twice);
  Draw indirectIndexed = indexed;
  indirectIndexed.indirect = true;
  cases += compare("indirect-indexed", run(c, pso.Get(), indirectIndexed).image, subset);
  indirect.predicate = true;
  indirect.predicateValue = false;
  auto indirectSkipped = run(c, pso.Get(), indirect);
  if (!std::all_of(indirectSkipped.image.texels.begin(), indirectSkipped.image.texels.end(),
                   [](uint32_t t) { return t == 0; }))
    fail("indirect-predicate-skip");
  std::printf("%s_CASE_PASS shader=%s case=indirect-predicate-skip\n", kPrefix, useDxil ? "dxil" : "dxbc");
  ++cases;

  // Many points: more than one object threadgroup (32 points per warp) and the full target.
  Draw many{D3D_PRIMITIVE_TOPOLOGY_POINTLIST};
  Image grid;
  for (int y = 2; y <= 30; y += 2)
    for (int x = 2; x <= 30; x += 2) {
      many.vertices.push_back(corner(float(x), float(y)));
      grid.quad(x, y, kRed);
    }
  many.count = UINT(many.vertices.size());
  many.statistics = true;
  auto full = run(c, pso.Get(), many);
  cases += compare("many-warps", full.image, grid);
  cases += expectStatistics("many-warps", full.statistics, 225, 225, 225, 450, 900);

  // LineStream and PointStream outputs.
  auto line = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.line, s.ps);
  Draw lines{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, {centre(4, 3), centre(20, 9)}};
  lines.count = 2;
  Image lineImage;
  for (auto [x, y] : {std::array<int, 2>{4, 3}, {20, 9}})
    for (int dx = -1; dx <= 1; ++dx)
      lineImage.texels[y * kSize + x + dx] = kRed;
  cases += compare("line-output", run(c, line.Get(), lines).image, lineImage);
  auto points = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.points, s.ps);
  Image pointImage;
  for (auto [x, y] : {std::array<int, 2>{4, 3}, {20, 9}})
    for (int dx = -2; dx <= 2; dx += 2)
      pointImage.texels[y * kSize + x + dx] = kRed;
  lines.statistics = true;
  auto pointResult = run(c, points.Get(), lines);
  cases += compare("point-output", pointResult.image, pointImage);
  cases += expectStatistics("point-output", pointResult.statistics, 2, 2, 2, 6, 6);
  return cases;
}

/*
 * Triangle input: lists, strips, strip cut, SV_PrimitiveID and GS instancing ([instance(2)]). Each triangle's first
 * vertex is on a pixel corner; GS instance i draws its quad 16*i pixels lower. The colour records the other two
 * vertices, the primitive id and the instance id.
 */
int casesTriangles(Context &c, const Shaders &s) {
  int cases = 0;
  auto pso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, s.vs, s.tri, s.ps);
  // Vertex k sits on corner (2 + 2k, 2); only vertex positions of the leading vertex matter for the oracle.
  std::vector<Float2> vertices;
  for (int k = 0; k < 12; ++k)
    vertices.push_back(corner(float(2 + 2 * (k % 12)), 2.0f + 2.0f * float(k / 12)));
  auto expect = [&](const std::vector<std::array<uint32_t, 3>> &primitives) {
    Image image;
    for (uint32_t p = 0; p < primitives.size(); ++p) {
      const auto [a, b, v] = primitives[p];
      for (uint32_t instance = 0; instance < 2; ++instance)
        image.quad(2 + 2 * int(a), 2 + 16 * int(instance), rgba(id8(b), id8(v), id8(p), id8(instance)));
    }
    return image;
  };
  Draw list{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, vertices};
  list.count = 12;
  list.statistics = true;
  auto listResult = run(c, pso.Get(), list);
  cases += compare("triangle-list-instanced", listResult.image, expect({{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {9, 10, 11}}));
  cases += expectStatistics("triangle-list-instanced", listResult.statistics, 12, 4, 8, 16, 32);

  // Strip: triangle i is (i, i+1, i+2) for even i and (i, i+2, i+1) for odd i (D3D11.3 8.10 winding rule).
  Draw strip{D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, vertices};
  strip.count = 7;
  std::vector<std::array<uint32_t, 3>> stripPrimitives;
  for (uint32_t i = 0; i + 2 < 7; ++i)
    stripPrimitives.push_back(i & 1 ? std::array<uint32_t, 3>{i, i + 2, i + 1} : std::array<uint32_t, 3>{i, i + 1, i + 2});
  strip.statistics = true;
  auto stripResult = run(c, pso.Get(), strip);
  cases += compare("triangle-strip", stripResult.image, expect(stripPrimitives));
  cases += expectStatistics("triangle-strip", stripResult.statistics, 7, 5, 10, 20, 40);

  // Strip with a 0xFFFF cut: {0,1,2,3} | {5,6,7}. SV_VertexID is the index value; primitive ids continue.
  auto cutPso = [&] {
    D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = c.root.Get();
    d.VS = {s.vs.data(), s.vs.size()};
    d.GS = {s.tri.data(), s.tri.size()};
    d.PS = {s.ps.data(), s.ps.size()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.InputLayout = {&element, 1};
    d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = c.samples;
    ComPtr<ID3D12PipelineState> result;
    check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&result)), "pso-cut");
    return result;
  }();
  Draw cut{D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, vertices};
  cut.indices = {0, 1, 2, 3, 0xffff, 5, 6, 7};
  cut.count = 8;
  cases += compare("triangle-strip-cut", run(c, cutPso.Get(), cut).image,
                   expect({{0, 1, 2}, {1, 3, 2}, {5, 6, 7}}));
  return cases;
}

/* Line lists and strips: a quad at the first vertex recording the second vertex and SV_PrimitiveID. */
int casesLines(Context &c, const Shaders &s) {
  int cases = 0;
  auto pso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, s.vs, s.linein, s.ps);
  std::vector<Float2> vertices;
  for (int k = 0; k < 12; ++k)
    vertices.push_back(corner(float(2 + 2 * (k % 8)), float(4 + 8 * (k / 8))));
  auto at = [&](uint32_t k) { return std::array<int, 2>{2 + 2 * int(k % 8), 4 + 8 * int(k / 8)}; };
  auto expect = [&](const std::vector<std::array<uint32_t, 2>> &primitives) {
    Image image;
    for (uint32_t p = 0; p < primitives.size(); ++p) {
      auto [x, y] = at(primitives[p][0]);
      image.quad(x, y, rgba(id8(primitives[p][1]), id8(p), 0, 255));
    }
    return image;
  };
  Draw list{D3D_PRIMITIVE_TOPOLOGY_LINELIST, vertices};
  list.count = 12;
  list.statistics = true;
  auto listResult = run(c, pso.Get(), list);
  cases += compare("line-list", listResult.image, expect({{0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}}));
  cases += expectStatistics("line-list", listResult.statistics, 12, 6, 6, 12, 24);
  Draw strip{D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, vertices};
  strip.count = 6;
  cases += compare("line-strip", run(c, pso.Get(), strip).image, expect({{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}}));
  return cases;
}

/*
 * Adjacency. Line adjacency (list: {4i..4i+3}; strip: {i..i+3}) draws a quad at vertex 1. Triangle adjacency list:
 * {6i..6i+5}. Triangle strip with adjacency (D3D11.3 8.10): for n triangles over 2n+4 vertices, triangle i has
 * vertices (2i, 2i+2, 2i+4) (odd i: (2i, 2i+4, 2i+2)) and adjacent vertices given by the table below.
 */
int casesAdjacency(Context &c, const Shaders &s) {
  int cases = 0;
  auto linePso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, s.vs, s.lineadj, s.ps);
  std::vector<Float2> vertices;
  for (int k = 0; k < 16; ++k)
    vertices.push_back(corner(float(2 + 2 * (k % 16)), 4.0f));
  auto quadAt = [](uint32_t k) { return 2 + 2 * int(k % 16); };
  auto lineExpect = [&](const std::vector<std::array<uint32_t, 4>> &primitives) {
    Image image;
    for (uint32_t p = 0; p < primitives.size(); ++p) {
      const auto v = primitives[p];
      image.quad(quadAt(v[1]), 4, rgba(id8(v[0]), id8(v[2]), id8(v[3]), id8(p)));
    }
    return image;
  };
  Draw list{D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ, vertices};
  list.count = 12;
  list.statistics = true;
  auto listResult = run(c, linePso.Get(), list);
  cases += compare("lineadj-list", listResult.image, lineExpect({{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}}));
  cases += expectStatistics("lineadj-list", listResult.statistics, 12, 3, 3, 6, 12);
  Draw strip{D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, vertices};
  strip.count = 7;
  std::vector<std::array<uint32_t, 4>> stripPrimitives;
  for (uint32_t i = 0; i + 3 < 7; ++i)
    stripPrimitives.push_back({i, i + 1, i + 2, i + 3});
  cases += compare("lineadj-strip", run(c, linePso.Get(), strip).image, lineExpect(stripPrimitives));

  auto triPso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, s.vs, s.triadj, s.ps);
  std::vector<Float2> triVertices;
  for (int k = 0; k < 16; ++k)
    triVertices.push_back(corner(float(2 + 4 * (k % 8)), float(4 + 8 * (k / 8))));
  auto triExpect = [&](const std::vector<std::array<uint32_t, 6>> &primitives) {
    Image image;
    for (uint32_t p = 0; p < primitives.size(); ++p) {
      const auto v = primitives[p];
      const int x = 2 + 4 * int(v[0] % 8), y = 4 + 8 * int(v[0] / 8);
      image.quad(x, y, rgba(id8(v[1]), id8(v[3]), id8(v[5]), 255));
      image.quad(x + 2, y, rgba(id8(v[2]), id8(v[4]), id8(p), 255));
    }
    return image;
  };
  Draw triList{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, triVertices};
  triList.count = 12;
  cases += compare("triadj-list", run(c, triPso.Get(), triList).image,
                   triExpect({{0, 1, 2, 3, 4, 5}, {6, 7, 8, 9, 10, 11}}));

  /* D3D11.3 8.10 triangle strip with adjacency, n triangles, vertices 0..2n+3 (GS input order v0..v5 =
   * vertex, adjacent, vertex, adjacent, vertex, adjacent):
   *   n == 1:            (0, 1, 2, 5, 4, 3)
   *   i == 0:            (0, 1, 2, 6, 4, 3)
   *   0 < i < n-1, even: (2i, 2i-2, 2i+2, 2i+6, 2i+4, 2i+3)
   *   0 < i < n-1, odd:  (2i, 2i+3, 2i+4, 2i+6, 2i+2, 2i-2)
   *   i == n-1, even:    (2i, 2i-2, 2i+2, 2i+5, 2i+4, 2i+3)
   *   i == n-1, odd:     (2i, 2i+3, 2i+4, 2i+5, 2i+2, 2i-2) */
  auto triStrip = [](uint32_t n) {
    std::vector<std::array<uint32_t, 6>> out;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t t = 2 * i;
      if (n == 1)
        out.push_back({0, 1, 2, 5, 4, 3});
      else if (i == 0)
        out.push_back({0, 1, 2, 6, 4, 3});
      else if (i + 1 < n)
        out.push_back(i & 1 ? std::array<uint32_t, 6>{t, t + 3, t + 4, t + 6, t + 2, t - 2}
                            : std::array<uint32_t, 6>{t, t - 2, t + 2, t + 6, t + 4, t + 3});
      else
        out.push_back(i & 1 ? std::array<uint32_t, 6>{t, t + 3, t + 4, t + 5, t + 2, t - 2}
                            : std::array<uint32_t, 6>{t, t - 2, t + 2, t + 5, t + 4, t + 3});
    }
    return out;
  };
  for (uint32_t n : {1u, 2u, 4u, 5u}) {
    Draw strip6{D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, triVertices};
    strip6.count = 2 * n + 4;
    const std::string name = "triadj-strip-" + std::to_string(n);
    cases += compare(name.c_str(), run(c, triPso.Get(), strip6).image, triExpect(triStrip(n)));
  }
  return cases;
}

/* PSO validation (D3D12 CreateGraphicsPipelineState rules for GS). */
int casesValidation(Context &c, const Shaders &s) {
  // GS input primitive must match the topology type.
  pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, s.vs, s.quad, s.ps, E_INVALIDARG);
  pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.tri, s.ps, E_INVALIDARG);
  pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH, s.vs, s.quad, s.ps, E_INVALIDARG);
  std::printf("%s_CASE_PASS shader=%s case=pso-topology-mismatch\n", kPrefix, useDxil ? "dxil" : "dxbc");
  // A draw topology the GS input primitive can't consume fails the list at Close.
  auto pso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.quad, s.ps);
  Draw wrong{D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, {corner(2, 2), corner(4, 4), corner(6, 6)}};
  wrong.count = 3;
  run(c, pso.Get(), wrong, E_INVALIDARG);
  std::printf("%s_CASE_PASS shader=%s case=draw-patch-topology\n", kPrefix, useDxil ? "dxil" : "dxbc");
  return 2;
}

/* Stream output after a GS: stream 0 captures the GS output; declarations naming streams 1..3 are E_NOTIMPL. */
int casesStreamOutput(Context &c, const Shaders &s) {
  D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_SO_DECLARATION_ENTRY entries[2] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0}};
  UINT stride = 32;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = c.root.Get();
  d.VS = {s.vs.data(), s.vs.size()};
  d.GS = {s.quad.data(), s.quad.size()};
  d.PS = {s.ps.data(), s.ps.size()};
  d.StreamOutput = {entries, 2, &stride, 1, 0};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {&element, 1};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  int cases = 0;
  {
    /* D3D12 SO validation for a multi-stream GS: an output buffer belongs to exactly one stream, RasterizedStream is
     * 0..3 or D3D12_SO_NO_RASTERIZED_STREAM, a stream's entries must name that stream's outputs, and more than one
     * used stream requires point-list output (D3D11.3 13.5.2, 14.1). */
    auto multiDesc = d;
    multiDesc.GS = {s.streams2.data(), s.streams2.size()};
    UINT strides[2] = {16, 16};
    auto create = [&](std::vector<D3D12_SO_DECLARATION_ENTRY> so, UINT rasterized, HRESULT expect, const char *step,
                      const D3D12_GRAPHICS_PIPELINE_STATE_DESC &base) {
      auto desc = base;
      desc.StreamOutput = {so.data(), UINT(so.size()), strides, 2, rasterized};
      ComPtr<ID3D12PipelineState> pso;
      const HRESULT hr = c.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
      if (hr != expect) {
        std::fprintf(stderr, "%s_SO_VALIDATION step=%s hr=0x%08lx expected=0x%08lx\n", kPrefix, step,
                     static_cast<unsigned long>(hr), static_cast<unsigned long>(expect));
        fail(step);
      }
    };
    create({{0, "COLOR", 0, 0, 4, 0}, {1, "COLOR", 0, 0, 4, 1}}, 0, S_OK, "so-two-streams", multiDesc);
    create({{0, "COLOR", 0, 0, 4, 0}, {1, "COLOR", 0, 0, 4, 1}}, 1, S_OK, "so-raster-stream1", multiDesc);
    create({{0, "COLOR", 0, 0, 4, 0}, {1, "COLOR", 0, 0, 4, 1}}, D3D12_SO_NO_RASTERIZED_STREAM, S_OK,
           "so-no-raster-stream", multiDesc);
    create({{0, "COLOR", 0, 0, 4, 0}, {1, "COLOR", 0, 0, 4, 0}}, 0, E_INVALIDARG, "so-slot-two-streams", multiDesc);
    create({{0, "COLOR", 0, 0, 4, 0}, {1, "COLOR", 0, 0, 4, 1}}, 4, E_INVALIDARG, "so-raster-stream4", multiDesc);
    create({{0, "COLOR", 0, 0, 4, 0}, {4, "COLOR", 0, 0, 4, 1}}, 0, E_INVALIDARG, "so-stream4", multiDesc);
    // gs_quad writes stream 0 only: its signature has no stream 1 outputs.
    create({{0, "COLOR", 0, 0, 4, 0}, {1, "COLOR", 0, 0, 4, 1}}, 0, E_INVALIDARG, "so-undeclared-stream", d);
    // Streams 0 and 1 used with triangle-strip output (gs_quad): multi-stream output is point lists only.
    create({{0, "COLOR", 0, 0, 4, 0}}, 1, E_INVALIDARG, "so-multi-stream-strip", d);
    std::printf("%s_CASE_PASS shader=%s case=so-streams-validation\n", kPrefix, useDxil ? "dxil" : "dxbc");
    ++cases;
  }

  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "so-pso");
  // Two points -> two quads (triangle strips of 4 vertices) -> 4 triangles = 12 SO vertices.
  const std::vector<Float2> points = {corner(4, 4), corner(20, 12)};
  D3D12_VERTEX_BUFFER_VIEW view;
  auto vb = vertexBuffer(c, points, view);
  constexpr UINT kTargetBytes = 16 * 32;
  auto target = buffer(c.device.Get(), kTargetBytes + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
  auto readback = buffer(c.device.Get(), kTargetBytes + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = buffer(c.device.Get(), kTargetBytes + 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  std::vector<uint8_t> zeros(kTargetBytes + 256, 0);
  upload(zero.Get(), zeros.data(), zeros.size());
  c.begin();
  auto toCopy = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->ResourceBarrier(1, &toCopy);
  c.list->CopyBufferRegion(target.Get(), 0, zero.Get(), 0, kTargetBytes + 256);
  auto toSo = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  c.list->ResourceBarrier(1, &toSo);
  setupPass(c);
  c.list->SetGraphicsRootSignature(c.root.Get());
  c.list->SetPipelineState(pso.Get());
  const float color[4] = {1, 0, 0.5f, 1};
  c.list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
  c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  c.list->IASetVertexBuffers(0, 1, &view);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW so = {target->GetGPUVirtualAddress(), kTargetBytes,
                                        target->GetGPUVirtualAddress() + kTargetBytes};
  c.list->SOSetTargets(0, 1, &so);
  c.list->DrawInstanced(2, 1, 0, 0);
  c.list->SOSetTargets(0, 0, nullptr);
  auto toRead = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->ResourceBarrier(1, &toRead);
  c.list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, kTargetBytes + 256);
  auto back = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
  c.list->ResourceBarrier(1, &back);
  Image image = finishPass(c);
  Image expected;
  expected.quad(4, 4, kRed);
  expected.quad(20, 12, kRed);
  cases += compare("so-stream0-raster", image, expected);
  void *mapped = nullptr;
  D3D12_RANGE range = {0, kTargetBytes + 256};
  check(readback->Map(0, &range, &mapped), "map-so");
  const auto *floats = static_cast<const float *>(mapped);
  uint64_t filled = 0;
  std::memcpy(&filled, static_cast<uint8_t *>(mapped) + kTargetBytes, sizeof(filled));
  /* Oracle: per point, the strip (-1,+1),(+1,+1),(-1,-1),(+1,-1) pixels about the point becomes the list
   * triangles (0,1,2),(1,3,2) (D3D11.3 14.5: strips are written as lists, odd triangles in strip winding). */
  const float px = 2.0f / kSize;
  const float cornersX[4] = {-1, 1, -1, 1}, cornersY[4] = {1, 1, -1, -1};
  const int order[6] = {0, 1, 2, 1, 3, 2};
  bool ok = filled == 12 * 32;
  for (int p = 0; p < 2 && ok; ++p)
    for (int v = 0; v < 6 && ok; ++v) {
      const float *record = floats + (p * 6 + v) * 8;
      const float wantX = points[p].x + cornersX[order[v]] * px, wantY = points[p].y + cornersY[order[v]] * px;
      ok = std::fabs(record[0] - wantX) < 1e-6f && std::fabs(record[1] - wantY) < 1e-6f && record[2] == 0 &&
           record[3] == 1 && record[4] == 1 && record[5] == 0 && record[6] == 0.5f && record[7] == 1;
      if (!ok)
        std::fprintf(stderr, "%s_SO_RECORD p=%d v=%d got=%f,%f,%f,%f want=%f,%f filled=%llu\n", kPrefix, p, v,
                     record[0], record[1], record[2], record[3], wantX, wantY, (unsigned long long)filled);
    }
  D3D12_RANGE none = {};
  readback->Unmap(0, &none);
  if (!ok)
    fail("so-stream0");
  std::printf("%s_CASE_PASS shader=%s case=so-stream0 vertices=12\n", kPrefix, useDxil ? "dxil" : "dxbc");
  return cases + 1;
}

/*
 * GS stream output on streams 0-3 (gs_streams2 / gs_streams4, point lists): every stream s is declared to slot s as
 * COLOR.xyzw, so slot s receives, per input point p in IA order, the s + 1 (gs_streams4) or 1 / 2 (gs_streams2) points
 * the GS emitted to stream s, in emission order: COLOR = (encode(p), encode(s), encode(k), 1). Checked exactly: the
 * slot contents and BufferFilledSize, SO_STATISTICS_STREAM0..3 (NumPrimitivesWritten, PrimitivesStorageNeeded),
 * the rasterized stream's image (or none), and the pipeline statistics (GSPrimitives counts every stream, the clipper
 * only the rasterized one). `capacity[s]` is slot s's size in records; a smaller size than needed overflows that
 * stream alone (D3D11.3 14.5).
 */
struct MultiStreamCase {
  const char *name;
  bool four;
  UINT rasterized;
  std::array<uint32_t, 4> capacity;
  /* ExecuteIndirect with GPU-written arguments and count: {0 vertices}, {both points}, {both points}, then commands
   * past the count (count 3) far beyond any capture. The oracle is the direct draw issued twice: the second
   * command's records append after the first's on every stream, up to each slot's capacity (overflow). */
  bool indirect = false;
};

int runMultiStream(Context &c, const Shaders &s, const MultiStreamCase &m) {
  const uint32_t streams = m.four ? 4 : 2;
  auto emitted = [](uint32_t stream) { return stream + 1; }; // both shaders emit s + 1 points to stream s
  D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
  for (UINT stream = 0; stream < streams; ++stream)
    entries.push_back({stream, "COLOR", 0, 0, 4, BYTE(stream)});
  std::vector<UINT> strides(streams, 16);
  const auto &gs = m.four ? s.streams4 : s.streams2;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = c.root.Get();
  d.VS = {s.vs.data(), s.vs.size()};
  d.GS = {gs.data(), gs.size()};
  d.PS = {s.ps.data(), s.ps.size()};
  d.StreamOutput = {entries.data(), UINT(entries.size()), strides.data(), UINT(strides.size()), m.rasterized};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {&element, 1};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), m.name);

  const std::array<std::array<int, 2>, 2> pixels = {{{4, 4}, {16, 20}}};
  const std::vector<Float2> points = {centre(4, 4), centre(16, 20)};
  D3D12_VERTEX_BUFFER_VIEW view;
  auto vb = vertexBuffer(c, points, view);
  // Slot s: records at s * kSlotBytes, its BufferFilledSize at s * kSlotBytes + kFilledAt.
  constexpr UINT kSlotBytes = 1024, kFilledAt = 512, kBytes = 4 * kSlotBytes;
  auto target = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
  auto readback = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  std::vector<uint8_t> zeros(kBytes, 0);
  upload(zero.Get(), zeros.data(), zeros.size());
  constexpr UINT kCommands = 5, kArgBytes = kCommands * sizeof(D3D12_DRAW_ARGUMENTS) + 16;
  ComPtr<ID3D12Resource> argsUpload, args;
  ComPtr<ID3D12CommandSignature> signature;
  if (m.indirect) {
    struct {
      D3D12_DRAW_ARGUMENTS commands[kCommands];
      UINT count[4];
    } words = {{{0, 1, 0, 0}, {2, 1, 0, 0}, {2, 1, 0, 0}, {0x40000000u, 0x10000u, 0, 0}, {0x40000000u, 0x10000u, 0, 0}},
               {3, 0, 0, 0}};
    static_assert(sizeof(words) == kArgBytes);
    argsUpload = buffer(c.device.Get(), kArgBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    upload(argsUpload.Get(), &words, kArgBytes);
    args = buffer(c.device.Get(), kArgBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    check(c.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "so-multi-signature");
  }
  D3D12_QUERY_HEAP_DESC soHeapDesc = {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 4, 0};
  ComPtr<ID3D12QueryHeap> soQueries, statsQueries;
  check(c.device->CreateQueryHeap(&soHeapDesc, IID_PPV_ARGS(&soQueries)), "so-query-heap");
  D3D12_QUERY_HEAP_DESC statsHeapDesc = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0};
  check(c.device->CreateQueryHeap(&statsHeapDesc, IID_PPV_ARGS(&statsQueries)), "stats-query-heap");
  auto queryData = buffer(c.device.Get(), 4 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS) +
                                              sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                          D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

  c.begin();
  if (m.indirect) {
    c.list->CopyBufferRegion(args.Get(), 0, argsUpload.Get(), 0, kArgBytes);
    auto toArgs = transition(args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    c.list->ResourceBarrier(1, &toArgs);
  }
  auto toCopy = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->ResourceBarrier(1, &toCopy);
  c.list->CopyBufferRegion(target.Get(), 0, zero.Get(), 0, kBytes);
  auto toSo = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  c.list->ResourceBarrier(1, &toSo);
  setupPass(c);
  c.list->SetPipelineState(pso.Get());
  const float color[4] = {1, 0, 0.5f, 1};
  c.list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
  c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  c.list->IASetVertexBuffers(0, 1, &view);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW so[4] = {};
  for (UINT slot = 0; slot < streams; ++slot) {
    const auto base = target->GetGPUVirtualAddress() + slot * kSlotBytes;
    so[slot] = {base, 16ull * m.capacity[slot], base + kFilledAt};
  }
  c.list->SOSetTargets(0, streams, so);
  for (UINT stream = 0; stream < 4; ++stream)
    c.list->BeginQuery(soQueries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + stream), stream);
  c.list->BeginQuery(statsQueries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  if (m.indirect)
    c.list->ExecuteIndirect(signature.Get(), kCommands, args.Get(), 0, args.Get(),
                            kCommands * sizeof(D3D12_DRAW_ARGUMENTS));
  else
    c.list->DrawInstanced(2, 1, 0, 0);
  c.list->EndQuery(statsQueries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  for (UINT stream = 0; stream < 4; ++stream) {
    const auto type = D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + stream);
    c.list->EndQuery(soQueries.Get(), type, stream);
    c.list->ResolveQueryData(soQueries.Get(), type, stream, 1, queryData.Get(),
                             stream * sizeof(D3D12_QUERY_DATA_SO_STATISTICS));
  }
  c.list->ResolveQueryData(statsQueries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, queryData.Get(),
                           4 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS));
  c.list->SOSetTargets(0, 0, nullptr);
  auto toRead = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->ResourceBarrier(1, &toRead);
  c.list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, kBytes);
  auto back = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
  c.list->ResourceBarrier(1, &back);
  Image image = finishPass(c);

  // Oracle image: stream r's points (2k pixels right, 2r pixels down of the input pixel), or nothing.
  Image expected;
  const bool raster = m.rasterized != D3D12_SO_NO_RASTERIZED_STREAM;
  if (raster)
    for (uint32_t p = 0; p < 2; ++p)
      for (uint32_t k = 0; k < emitted(m.rasterized); ++k)
        expected.texels[(pixels[p][1] + 2 * m.rasterized) * kSize + pixels[p][0] + 2 * k] =
            rgba(id8(p), id8(m.rasterized), id8(k), 255);
  int cases = 0;
  if (raster) {
    cases += compare((std::string(m.name) + "-image").c_str(), image, expected);
  } else {
    for (auto texel : image.texels)
      if (texel)
        fail("so-multi-stream-no-raster-image");
  }

  void *mapped = nullptr;
  D3D12_RANGE range = {0, kBytes};
  check(readback->Map(0, &range, &mapped), "map-so-multi");
  const auto *bytes = static_cast<const uint8_t *>(mapped);
  bool ok = true;
  std::array<uint64_t, 4> written{}, needed{};
  const uint32_t draws = m.indirect ? 2 : 1;
  for (uint32_t stream = 0; stream < streams; ++stream) {
    needed[stream] = 2ull * draws * emitted(stream);
    written[stream] = std::min<uint64_t>(needed[stream], m.capacity[stream]);
    uint32_t filled = 0;
    std::memcpy(&filled, bytes + stream * kSlotBytes + kFilledAt, 4);
    if (filled != written[stream] * 16) {
      std::fprintf(stderr, "%s_SO_MULTI case=%s stream=%u filled=%u want=%llu\n", kPrefix, m.name, stream, filled,
                   (unsigned long long)written[stream] * 16);
      ok = false;
    }
    for (uint32_t r = 0; r < 512 / 16; ++r) {
      float record[4];
      std::memcpy(record, bytes + stream * kSlotBytes + r * 16, 16);
      float want[4] = {0, 0, 0, 0};
      if (r < written[stream]) {
        const uint32_t p = r / emitted(stream) % 2, k = r % emitted(stream);
        want[0] = (p + 1) / 255.0f, want[1] = (stream + 1) / 255.0f, want[2] = (k + 1) / 255.0f, want[3] = 1;
      }
      if (std::memcmp(record, want, 16)) {
        std::fprintf(stderr, "%s_SO_MULTI case=%s stream=%u record=%u got=%f,%f,%f,%f want=%f,%f,%f,%f\n", kPrefix,
                     m.name, stream, r, record[0], record[1], record[2], record[3], want[0], want[1], want[2], want[3]);
        ok = false;
      }
    }
  }
  D3D12_RANGE none = {};
  readback->Unmap(0, &none);
  D3D12_QUERY_DATA_SO_STATISTICS soStats[4];
  D3D12_QUERY_DATA_PIPELINE_STATISTICS stats;
  D3D12_RANGE queryRange = {0, sizeof(soStats) + sizeof(stats)};
  check(queryData->Map(0, &queryRange, &mapped), "map-so-multi-query");
  std::memcpy(soStats, mapped, sizeof(soStats));
  std::memcpy(&stats, static_cast<uint8_t *>(mapped) + sizeof(soStats), sizeof(stats));
  queryData->Unmap(0, &none);
  for (uint32_t stream = 0; stream < 4; ++stream) {
    if (soStats[stream].NumPrimitivesWritten != written[stream] ||
        soStats[stream].PrimitivesStorageNeeded != needed[stream]) {
      std::fprintf(stderr, "%s_SO_MULTI case=%s stream=%u query=(%llu,%llu) want=(%llu,%llu)\n", kPrefix, m.name,
                   stream, (unsigned long long)soStats[stream].NumPrimitivesWritten,
                   (unsigned long long)soStats[stream].PrimitivesStorageNeeded, (unsigned long long)written[stream],
                   (unsigned long long)needed[stream]);
      ok = false;
    }
  }
  uint64_t gsPrimitives = 0;
  for (uint32_t stream = 0; stream < streams; ++stream)
    gsPrimitives += 2ull * draws * emitted(stream);
  const uint64_t clipped = raster ? 2ull * draws * emitted(m.rasterized) : 0;
  if (stats.IAVertices != 2 * draws || stats.IAPrimitives != 2 * draws || stats.VSInvocations != 2 * draws ||
      stats.GSInvocations != 2 * draws ||
      stats.GSPrimitives != gsPrimitives || stats.CInvocations != clipped || stats.CPrimitives != clipped ||
      stats.PSInvocations != clipped) {
    std::fprintf(stderr, "%s_SO_MULTI case=%s stats ia=%llu,%llu gs=%llu,%llu c=%llu,%llu ps=%llu want gs=2,%llu "
                 "c=%llu ps=%llu\n", kPrefix, m.name, (unsigned long long)stats.IAVertices,
                 (unsigned long long)stats.IAPrimitives, (unsigned long long)stats.GSInvocations,
                 (unsigned long long)stats.GSPrimitives, (unsigned long long)stats.CInvocations,
                 (unsigned long long)stats.CPrimitives, (unsigned long long)stats.PSInvocations,
                 (unsigned long long)gsPrimitives, (unsigned long long)clipped, (unsigned long long)clipped);
    ok = false;
  }
  if (!ok)
    fail(m.name);
  std::printf("%s_CASE_PASS shader=%s case=%s written=%llu,%llu,%llu,%llu needed=%llu,%llu,%llu,%llu gs=%llu c=%llu\n",
              kPrefix, useDxil ? "dxil" : "dxbc", m.name, (unsigned long long)written[0],
              (unsigned long long)written[1], (unsigned long long)written[2], (unsigned long long)written[3],
              (unsigned long long)needed[0], (unsigned long long)needed[1], (unsigned long long)needed[2],
              (unsigned long long)needed[3], (unsigned long long)gsPrimitives, (unsigned long long)clipped);
  return cases + 1;
}

/*
 * D3D12 has no SO overflow predicate query (DirectX-Specs CountersAndQueries: SO_STATISTICS_PREDICATE is not present;
 * applications emulate it). The API path is SetPredication over a 64-bit field of resolved SO_STATISTICS query data.
 * One list: a GS SO draw of 2 points (8 triangles) into a 3-triangle slot, ResolveQueryData into a DEFAULT buffer,
 * then two quads predicated on NumPrimitivesWritten (3) and on a zero word, with both ops. A predicate that passes
 * draws its quad, one that fails draws nothing; the resolve must be ordered before the predicate snapshot.
 */
int casesStreamOutputPredication(Context &c, const Shaders &s) {
  D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_SO_DECLARATION_ENTRY entry = {0, "COLOR", 0, 0, 4, 0};
  const UINT stride = 16;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = c.root.Get();
  d.VS = {s.vs.data(), s.vs.size()};
  d.GS = {s.quad.data(), s.quad.size()};
  d.PS = {s.ps.data(), s.ps.size()};
  d.StreamOutput = {&entry, 1, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {&element, 1};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> so;
  check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&so)), "so-predicate-pso");
  auto quads = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.quad, s.ps);
  // Point 0, 1: the SO draw. Points 2-5: four predicated quads at (8,8) (24,8) (8,24) (24,24).
  const std::vector<Float2> points = {corner(4, 4), corner(6, 6), corner(8, 8), corner(24, 8), corner(8, 24),
                                      corner(24, 24)};
  D3D12_VERTEX_BUFFER_VIEW view;
  auto vb = vertexBuffer(c, points, view);
  constexpr UINT kFilledAt = 256, kBytes = 512;
  auto target = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
  auto zero = buffer(c.device.Get(), kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  std::vector<uint8_t> zeros(kBytes, 0);
  upload(zero.Get(), zeros.data(), zeros.size());
  // [0] SO_STATISTICS {NumPrimitivesWritten, PrimitivesStorageNeeded}, [16] a zero word.
  auto predicates = buffer(c.device.Get(), 32, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto statsReadback = buffer(c.device.Get(), 32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_QUERY_HEAP_DESC heapDesc = {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 1, 0};
  ComPtr<ID3D12QueryHeap> heap;
  check(c.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap)), "so-predicate-heap");

  c.begin();
  auto toCopy = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
  c.list->ResourceBarrier(1, &toCopy);
  c.list->CopyBufferRegion(target.Get(), 0, zero.Get(), 0, kBytes);
  c.list->CopyBufferRegion(predicates.Get(), 0, zero.Get(), 0, 32);
  auto toSo = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  c.list->ResourceBarrier(1, &toSo);
  setupPass(c);
  const float color[4] = {1, 0, 0.5f, 1};
  c.list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
  c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  c.list->IASetVertexBuffers(0, 1, &view);
  c.list->SetPipelineState(so.Get());
  D3D12_STREAM_OUTPUT_BUFFER_VIEW slot = {target->GetGPUVirtualAddress(), 3 * 3 * 16,
                                          target->GetGPUVirtualAddress() + kFilledAt};
  c.list->SOSetTargets(0, 1, &slot);
  c.list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
  c.list->DrawInstanced(2, 1, 0, 0);
  c.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
  c.list->SOSetTargets(0, 0, nullptr);
  c.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, predicates.Get(), 0);
  D3D12_RESOURCE_BARRIER toPredicate[] = {
      transition(predicates.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION)};
  c.list->ResourceBarrier(1, toPredicate);
  c.list->SetPipelineState(quads.Get());
  struct Predicated {
    UINT64 offset;
    D3D12_PREDICATION_OP op;
    UINT vertex;
    bool drawn;
  };
  // NOT_EQUAL_ZERO skips when the value is non-zero (D3D12_PREDICATION_OP), EQUAL_ZERO skips when it is zero.
  const Predicated cases[] = {
      {0, D3D12_PREDICATION_OP_EQUAL_ZERO, 2, true},       // written 3 != 0: drawn
      {0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO, 3, false},  // written 3 != 0: skipped
      {16, D3D12_PREDICATION_OP_EQUAL_ZERO, 4, false},     // zero: skipped
      {16, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO, 5, true},  // zero: drawn
  };
  for (const auto &p : cases) {
    c.list->SetPredication(predicates.Get(), p.offset, p.op);
    c.list->DrawInstanced(1, 1, p.vertex, 0);
  }
  c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  D3D12_RESOURCE_BARRIER toRead[] = {
      transition(predicates.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  c.list->ResourceBarrier(1, toRead);
  c.list->CopyBufferRegion(statsReadback.Get(), 0, predicates.Get(), 0, 32);
  Image image = finishPass(c);

  void *mapped = nullptr;
  D3D12_RANGE range = {0, 16}, none = {};
  check(statsReadback->Map(0, &range, &mapped), "so-predicate-map");
  D3D12_QUERY_DATA_SO_STATISTICS stats;
  std::memcpy(&stats, mapped, sizeof(stats));
  statsReadback->Unmap(0, &none);
  // gs_quad: 2 triangles per point; the slot holds 3 of the 4.
  if (stats.NumPrimitivesWritten != 3 || stats.PrimitivesStorageNeeded != 4) {
    std::fprintf(stderr, "%s_SO_PREDICATE written=%llu needed=%llu want 3,4\n", kPrefix,
                 (unsigned long long)stats.NumPrimitivesWritten, (unsigned long long)stats.PrimitivesStorageNeeded);
    fail("so-statistics-predication");
  }
  Image expected;
  const int at[4][2] = {{8, 8}, {24, 8}, {8, 24}, {24, 24}};
  for (int i = 0; i < 4; ++i)
    if (cases[i].drawn)
      expected.quad(at[i][0], at[i][1], kRed);
  return compare("so-statistics-predication", image, expected);
}

int casesMultiStream(Context &c, const Shaders &s) {
  const MultiStreamCase multi[] = {
      {"so-streams2-raster0", false, 0, {8, 8, 8, 8}},
      {"so-streams2-raster1", false, 1, {8, 8, 8, 8}},
      {"so-streams4-raster2", true, 2, {8, 8, 8, 8}},
      {"so-streams4-no-raster", true, D3D12_SO_NO_RASTERIZED_STREAM, {8, 8, 8, 8}},
      // Stream 1 fits 3 of 4 points and stream 3 5 of 8; streams 0 and 2 are unaffected.
      {"so-streams4-overflow", true, 3, {8, 3, 8, 5}},
      {"so-streams2-indirect", false, 1, {8, 8, 8, 8}, true},
      {"so-streams4-indirect-overflow", true, 3, {8, 3, 8, 5}, true},
  };
  int cases = 0;
  for (const auto &m : multi)
    cases += runMultiStream(c, s, m);
  return cases;
}

/* 4x MSAA: the resolved GS output equals the single-sample image for axis-aligned, pixel-aligned quads, and an
 * edge through pixel centres resolves to the exact 2-of-4 average. */

/* D3D PSInvocations of a pixel-frequency PS under 4x MSAA: one invocation per (primitive, pixel) with at least one
 * covered sample. emit_quad's strip (c+(-1,-1), c+(1,-1), c+(-1,1), c+(1,1) in pixels, y down) is the triangles
 * (0,1,2) and (1,3,2); standard 4x sample offsets in 1/16 pixel. The oracle requires no sample on an edge, so no
 * fill-rule tie decides a count. */
uint64_t quadPixelInvocations4x(const std::vector<std::array<float, 2>> &centres) {
  const int offsets[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
  uint64_t invocations = 0;
  for (auto [cx, cy] : centres) {
    const double v[4][2] = {{cx - 1.0, cy - 1.0}, {cx + 1.0, cy - 1.0}, {cx - 1.0, cy + 1.0}, {cx + 1.0, cy + 1.0}};
    const int triangles[2][3] = {{0, 1, 2}, {1, 3, 2}};
    for (const auto &t : triangles) {
      for (int py = 0; py < int(kSize); ++py)
        for (int px = 0; px < int(kSize); ++px) {
          bool covered = false;
          for (const auto &o : offsets) {
            const double sx = px + 0.5 + o[0] / 16.0, sy = py + 0.5 + o[1] / 16.0;
            double e[3];
            for (int k = 0; k < 3; ++k) {
              const double *a = v[t[k]], *b = v[t[(k + 1) % 3]];
              e[k] = (b[0] - a[0]) * (sy - a[1]) - (b[1] - a[1]) * (sx - a[0]);
              if (e[k] == 0)
                fail("msaa4-oracle-tie");
            }
            covered = covered || (e[0] > 0 && e[1] > 0 && e[2] > 0) || (e[0] < 0 && e[1] < 0 && e[2] < 0);
          }
          invocations += covered;
        }
    }
  }
  return invocations;
}
int casesMultisample(Context &c, const Shaders &s) {
  int cases = 0;
  createTarget(c, 4);
  auto pso = pipeline(c, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, s.vs, s.quad, s.ps);
  const std::vector<std::array<int, 2>> corners = {{2, 2}, {10, 4}, {12, 20}, {6, 28}};
  Draw draw{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, pointGrid(corners)};
  draw.count = UINT(corners.size());
  Image expected;
  for (auto [x, y] : corners)
    expected.quad(x, y, kRed);
  cases += compare("msaa4-amplify", run(c, pso.Get(), draw).image, expected);
  /* A quad centred half a pixel right of a corner: its left/right columns are half covered. Standard 4x sample
   * positions (+-2/16, +-6/16) put exactly 2 of 4 samples on each side of a pixel's vertical centre line. */
  Draw half{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, {corner(8.5f, 8)}};
  half.count = 1;
  Image halfImage;
  const uint32_t halfRed = rgba(128, 0, 64, 128);
  for (int y = 7; y <= 8; ++y) {
    halfImage.texels[y * kSize + 7] = halfRed;
    halfImage.texels[y * kSize + 8] = kRed;
    halfImage.texels[y * kSize + 9] = halfRed;
  }
  auto halfResult = run(c, pso.Get(), half);
  // Resolve rounding: allow +-1 per channel on the half-covered texels.
  bool ok = true;
  for (UINT i = 0; i < kSize * kSize; ++i)
    for (int ch = 0; ch < 4; ++ch) {
      const int got = (halfResult.image.texels[i] >> (8 * ch)) & 0xff, want = (halfImage.texels[i] >> (8 * ch)) & 0xff;
      if (std::abs(got - want) > 1) {
        std::fprintf(stderr, "%s_MISMATCH case=msaa4-edge x=%u y=%u got=%08x want=%08x\n", kPrefix, i % kSize,
                     i / kSize, halfResult.image.texels[i], halfImage.texels[i]);
        ok = false;
        break;
      }
    }
  if (!ok)
    fail("msaa4-edge");
  std::printf("%s_CASE_PASS shader=%s case=msaa4-edge\n", kPrefix, useDxil ? "dxil" : "dxbc");
  ++cases;
  /* Statistics over a native 4x MSAA mesh draw: PSInvocations counts (primitive, pixel) pairs with coverage, not
   * covered samples. Each quad's diagonal splits two pixels between its triangles: 6 invocations per quad (the
   * 1x count is 4). A quad offset by 3/16 pixel is partially covered on every edge (no sample on an edge). */
  draw.statistics = true;
  std::vector<std::array<float, 2>> centres;
  for (auto [x, y] : corners)
    centres.push_back({float(x), float(y)});
  auto msaaStats = run(c, pso.Get(), draw);
  cases += compare("msaa4-statistics-image", msaaStats.image, expected);
  const uint64_t wantQuads = quadPixelInvocations4x(centres);
  cases += expectStatistics("msaa4", msaaStats.statistics, 4, 4, 4, 8, wantQuads);
  if (msaaStats.statistics.PSInvocations != wantQuads || wantQuads != 24)
    fail("msaa4-statistics-ps");
  Draw partial{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, {corner(8.1875f, 8.1875f)}};
  partial.count = 1;
  partial.statistics = true;
  auto partialStats = run(c, pso.Get(), partial).statistics;
  const uint64_t wantPartial = quadPixelInvocations4x({{8.1875f, 8.1875f}});
  cases += expectStatistics("msaa4-partial", partialStats, 1, 1, 1, 2, wantPartial);
  std::printf("%s_MSAA_STATISTICS shader=%s quads=%llu/%llu partial=%llu/%llu\n", kPrefix, useDxil ? "dxil" : "dxbc",
              (unsigned long long)msaaStats.statistics.PSInvocations, (unsigned long long)wantQuads,
              (unsigned long long)partialStats.PSInvocations, (unsigned long long)wantPartial);
  if (partialStats.PSInvocations != wantPartial)
    fail("msaa4-partial-statistics-ps");
  createTarget(c, 1);
  return cases;
}

/*
 * PSInvocations of a native MSAA GS draw whose PS forces early depth (ps_early) with a depth test: early tests run
 * an arbitrary subset of each pixel's covered samples, so the runtime counts once per (pixel, primitive) by a
 * raster-order claim buffer. gs_quad turns each point into a 2x2-pixel quad (two triangles); the points sit at
 * sub-pixel offsets so the quads' edges cut pixels and overlap one another. An occluder draw (no query, depth 0.25)
 * covers the left part of the target first; the counted draws are at depth 0.5 (viewport depth range), so the
 * occluder rejects their left samples and the later of two overlapping quads passes only where LESS_EQUAL holds.
 * Oracle: early_depth_oracle (API order, per-sample depth, any passing sample counts the pair once). Cases: direct
 * (2x, 4x, 8x, instanced), ExecuteIndirect with GPU-written arguments and count, and ExecuteIndirect with stream
 * output (the capture path), each image-equal to the same direct draw.
 */
int casesEarlyDepthStatistics(Context &c, const Shaders &s) {
  int cases = 0;
  /* No sample lies on a quad edge: edge coordinates and the diagonals' x + y sums avoid multiples of 1/16. */
  const std::vector<std::array<float, 2>> centres = {{6.3f, 6.1f}, {7.1f, 6.8f}, {20.4f, 9.7f}, {21.2f, 10.2f},
                                                     {12.6f, 22.1f}, {3.3f, 25.8f}, {27.9f, 26.4f}};
  for (UINT samples : {2u, 4u, 8u}) {
    createTarget(c, samples);
    D3D12_RESOURCE_DESC dd = c.target->GetDesc();
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1;
    ComPtr<ID3D12Resource> depth;
    check(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                            &depthClear, IID_PPV_ARGS(&depth)),
          "early-depth-dsv");
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsvs;
    check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvs)), "early-depth-dsv-heap");
    const auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    c.device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

    auto makePso = [&](const std::vector<uint8_t> &ps, bool so) {
      D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
      D3D12_SO_DECLARATION_ENTRY entry = {0, "COLOR", 0, 0, 4, 0};
      const UINT stride = 16;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = c.root.Get();
      d.VS = {s.vs.data(), s.vs.size()};
      d.GS = {s.quad.data(), s.quad.size()};
      d.PS = {ps.data(), ps.size()};
      if (so)
        d.StreamOutput = {&entry, 1, &stride, 1, 0};
      d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.DepthStencilState.DepthEnable = TRUE;
      d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
      d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      d.InputLayout = {&element, 1};
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = samples;
      ComPtr<ID3D12PipelineState> pso;
      check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "early-depth-pso");
      return pso;
    };
    auto early = makePso(s.early, false), earlySo = makePso(s.early, true), occluderPso = makePso(s.ps, false);

    // Occluder: a column of 2x2 quads covering x in [2, 7.7) (its right edge cuts through pixel 7, so some quads are
    // partially occluded). Its diagonal ties are irrelevant: everything it covers stays covered.
    std::vector<Float2> occluder;
    std::vector<early_depth_oracle::Triangle> occluderTris;
    auto quadTris = [](float cx, float cy, double z, std::vector<early_depth_oracle::Triangle> &out) {
      // gs_quad's strip around a pixel corner (cx, cy): pixel corners cx-1..cx+1, cy-1..cy+1.
      const early_depth_oracle::Vertex a{cx - 1.0, cy - 1.0, z}, b{cx + 1.0, cy - 1.0, z}, cc{cx - 1.0, cy + 1.0, z},
          dd{cx + 1.0, cy + 1.0, z};
      out.push_back({{a, b, cc}});
      out.push_back({{b, dd, cc}});
    };
    for (float y = 1.0f; y < 32.0f; y += 2.0f)
      for (float x : {3.0f, 5.0f, 6.7f}) {
        occluder.push_back(corner(x, y));
        quadTris(x, y, 0.25, occluderTris);
      }
    std::vector<Float2> points;
    std::vector<early_depth_oracle::Triangle> drawn;
    const UINT instances = 2; // vs_pass moves instance 1 by +16 pixels right (off target past x = 32 is clipped)
    for (UINT instance = 0; instance < instances; ++instance)
      for (auto [x, y] : centres)
        quadTris(x + 16.0f * instance, y, 0.5, drawn);
    for (auto [x, y] : centres)
      points.push_back(corner(x, y));
    const uint64_t want = early_depth_oracle::ps_invocations(kSize, kSize, samples, occluderTris, drawn);

    enum class Mode { Direct, Indirect, IndirectSo };
    auto record = [&](Mode mode, ID3D12PipelineState *pso, D3D12_QUERY_DATA_PIPELINE_STATISTICS &stats,
                      bool withOccluder = true) {
      D3D12_VERTEX_BUFFER_VIEW occluderView, view;
      auto occluderVb = vertexBuffer(c, occluder, occluderView);
      auto vb = vertexBuffer(c, points, view);
      D3D12_QUERY_HEAP_DESC q = {};
      q.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
      q.Count = 1;
      ComPtr<ID3D12QueryHeap> queries;
      check(c.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "early-depth-query");
      auto readback = buffer(c.device.Get(), sizeof(stats), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      // Indirect: {points, 2 instances}, {0 vertices}, then commands past the GPU-written count (2).
      struct {
        D3D12_DRAW_ARGUMENTS commands[4];
        UINT count[4];
      } words = {{{UINT(points.size()), instances, 0, 0}, {0, 1, 0, 0}, {0x4000000u, 4, 0, 0}, {0x4000000u, 4, 0, 0}},
                 {2, 0, 0, 0}};
      auto argsUpload = buffer(c.device.Get(), sizeof(words), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      upload(argsUpload.Get(), &words, sizeof(words));
      auto args = buffer(c.device.Get(), sizeof(words), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_INDIRECT_ARGUMENT_DESC argument = {};
      argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
      D3D12_COMMAND_SIGNATURE_DESC sd = {};
      sd.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
      sd.NumArgumentDescs = 1;
      sd.pArgumentDescs = &argument;
      ComPtr<ID3D12CommandSignature> signature;
      check(c.device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)), "early-depth-signature");
      constexpr UINT kSoBytes = 4096;
      auto so = buffer(c.device.Get(), kSoBytes + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
      c.begin();
      c.list->CopyBufferRegion(args.Get(), 0, argsUpload.Get(), 0, sizeof(words));
      auto toArgs = transition(args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
      c.list->ResourceBarrier(1, &toArgs);
      auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
      const float zero[4] = {};
      c.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
      c.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
      c.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
      c.list->RSSetScissorRects(1, &scissor);
      c.list->SetGraphicsRootSignature(c.root.Get());
      const float color[4] = {1, 0, 0.5f, 1};
      c.list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
      c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      D3D12_VIEWPORT occluderViewport = {0, 0, float(kSize), float(kSize), 0.25f, 0.25f};
      c.list->RSSetViewports(1, &occluderViewport);
      c.list->SetPipelineState(occluderPso.Get());
      c.list->IASetVertexBuffers(0, 1, &occluderView);
      if (withOccluder)
        c.list->DrawInstanced(UINT(occluder.size()), 1, 0, 0);
      D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0.5f, 0.5f};
      c.list->RSSetViewports(1, &viewport);
      c.list->SetPipelineState(pso);
      c.list->IASetVertexBuffers(0, 1, &view);
      D3D12_STREAM_OUTPUT_BUFFER_VIEW target = {so->GetGPUVirtualAddress(), kSoBytes,
                                                so->GetGPUVirtualAddress() + kSoBytes};
      if (mode == Mode::IndirectSo)
        c.list->SOSetTargets(0, 1, &target);
      c.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      if (mode == Mode::Direct)
        c.list->DrawInstanced(UINT(points.size()), instances, 0, 0);
      else
        c.list->ExecuteIndirect(signature.Get(), 4, args.Get(), 0, args.Get(), 4 * sizeof(D3D12_DRAW_ARGUMENTS));
      c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      c.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, readback.Get(), 0);
      if (mode == Mode::IndirectSo)
        c.list->SOSetTargets(0, 0, nullptr);
      Image image = finishPass(c);
      void *mapped = nullptr;
      D3D12_RANGE range = {0, sizeof(stats)}, none = {};
      check(readback->Map(0, &range, &mapped), "early-depth-map");
      std::memcpy(&stats, mapped, sizeof(stats));
      readback->Unmap(0, &none);
      return image;
    };
    D3D12_QUERY_DATA_PIPELINE_STATISTICS direct{}, indirect{}, indirectSo{}, open{};
    const Image directImage = record(Mode::Direct, early.Get(), direct);
    const Image indirectImage = record(Mode::Indirect, early.Get(), indirect);
    const Image soImage = record(Mode::IndirectSo, earlySo.Get(), indirectSo);
    // Without the occluder: only the counted quads' own overlaps (equal depth, LESS_EQUAL) are tested.
    record(Mode::Direct, early.Get(), open, false);
    const uint64_t wantOpen = early_depth_oracle::ps_invocations(kSize, kSize, samples, {}, drawn);
    const std::string base = "early-depth-msaa" + std::to_string(samples);
    const uint64_t prims = uint64_t(points.size()) * instances;
    cases += expectStatistics(base.c_str(), direct, prims, prims, prims, 2 * prims, want);
    cases += expectStatistics((base + "-indirect").c_str(), indirect, prims, prims, prims, 2 * prims, want);
    cases += expectStatistics((base + "-indirect-so").c_str(), indirectSo, prims, prims, prims, 2 * prims, want);
    cases += expectStatistics((base + "-open").c_str(), open, prims, prims, prims, 2 * prims, wantOpen);
    cases += compare((base + "-indirect-image").c_str(), indirectImage, directImage);
    cases += compare((base + "-indirect-so-image").c_str(), soImage, directImage);
    std::printf("%s_EARLY_DEPTH_STATISTICS shader=%s samples=%u ps=%llu,%llu,%llu,%llu oracle=%llu,%llu\n", kPrefix,
                useDxil ? "dxil" : "dxbc", samples, (unsigned long long)direct.PSInvocations,
                (unsigned long long)indirect.PSInvocations, (unsigned long long)indirectSo.PSInvocations,
                (unsigned long long)open.PSInvocations, (unsigned long long)want, (unsigned long long)wantOpen);
    if (direct.PSInvocations != want || indirect.PSInvocations != want || indirectSo.PSInvocations != want ||
        open.PSInvocations != wantOpen)
      fail("early-depth-ps-invocations");
  }
  createTarget(c, 1);
  return cases;
}

/*
 * ExecuteIndirect over GS strips the mesh GS cannot assemble natively: an indexed triangle strip with a 0xFFFF cut
 * (gs_tri) and a triangle strip with adjacency (gs_triadj). Direct draws expand them into the equivalent list per
 * draw; ExecuteIndirect expands each command from its GPU-written arguments. Every indirect case runs the commands
 * {strip, start 0}, {0 indices}, {strip again from a later start index, base vertex 1} under a GPU-written count of
 * 3, then commands past the count, with and without stream output, and must equal the same direct draws exactly:
 * image, SO bytes and filled size, and pipeline statistics.
 */
int casesIndirectStripExpansion(Context &c, const Shaders &s) {
  int cases = 0;
  struct Strip {
    const char *name;
    D3D_PRIMITIVE_TOPOLOGY topology;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    const std::vector<uint8_t> *gs;
    std::vector<uint16_t> indices;
    UINT count;   // indices of command 0 (from index 0)
    UINT start2;  // command 2: start index (count2 indices), base vertex 1
    UINT count2;
    bool frontier = false;
  };
  std::vector<Float2> vertices;
  for (int k = 0; k < 16; ++k)
    vertices.push_back(corner(float(2 + 4 * (k % 8)), float(4 + 8 * (k / 8))));
  std::vector<Strip> strips = {
      {"indirect-strip-cut", D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, &s.tri,
       {0, 1, 2, 3, 0xffff, 5, 6, 7, 0xffff, 0xffff, 8, 9, 10, 11, 12}, 8, 8, 7},
      {"indirect-triadj-strip", D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
       &s.triadj, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}, 10, 2, 8},
  };
  // The two live segments cross the 256/512 scan boundaries; cut padding also spans whole empty blocks.
  // Command 2 starts at an unaligned 16-bit index and restarts the same segments with a different base vertex.
  for (bool adjacency : {false, true}) {
    std::vector<uint16_t> indices(521, 0xffff);
    for (UINT first : {250u, 508u})
      for (UINT i = 0; i < (adjacency ? 10u : 7u); i++) indices[first + i] = uint16_t(i);
    strips.push_back({adjacency ? "indirect-triadj-strip-scan" : "indirect-strip-cut-scan",
                      adjacency ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, adjacency ? &s.triadj : &s.tri,
                      std::move(indices), 521, 247, 274});
  }
  // A raw-index scan is bounded to 256 positions per scheduler slot. Long empty cut runs
  // cross a 64-slot quantum, while live strips straddle scan boundaries and the final tail.
  // The second command must restart assembly and use its own base, even after the long cursor.
  for (bool adjacency : {false, true}) {
    std::vector<uint16_t> indices(65 * 256 + 17, 0xffff);
    for (UINT i = 0; i < 6; ++i) indices[250 + i] = uint16_t(i);
    for (UINT first : {257u, 510u, 16381u})
      for (UINT i = 0; i < (adjacency ? 10u : 7u); ++i) indices[first + i] = uint16_t(i);
    strips.push_back({adjacency ? "indirect-frontier-triadj-carry" : "indirect-frontier-strip-cut-carry",
                      adjacency ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, adjacency ? &s.triadj : &s.tri,
                      std::move(indices), 65 * 256 + 17, 16379, 20, true});
  }
  for (const auto &strip : strips)
    for (bool so : {false, true}) {
      if (strip.frontier && !so)
        continue;
      D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
      D3D12_SO_DECLARATION_ENTRY entries[2] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0}};
      const UINT stride = 32;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = c.root.Get();
      d.VS = {s.vs.data(), s.vs.size()};
      d.GS = {strip.gs->data(), strip.gs->size()};
      d.PS = {s.ps.data(), s.ps.size()};
      if (so)
        d.StreamOutput = {entries, 2, &stride, 1, 0};
      d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      d.RasterizerState.DepthClipEnable = TRUE;
      d.InputLayout = {&element, 1};
      d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      d.PrimitiveTopologyType = strip.type;
      d.NumRenderTargets = 1;
      d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> pso;
      check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), strip.name);

      struct Result {
        Image image;
        std::vector<uint8_t> so;
        uint64_t filled = 0;
        D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
      };
      auto record = [&](bool indirect) {
        Result result;
        if (strip.frontier && indirect) {
          std::fprintf(stderr, "OWNED_D3D12_CAPTURE_FRONTIER_PHASE suite=geometry shader=%s case=%s begin\n",
                       useDxil ? "dxil" : "dxbc", strip.name);
          std::fflush(stderr);
        }
        D3D12_VERTEX_BUFFER_VIEW view;
        auto vb = vertexBuffer(c, vertices, view);
        auto ib = buffer(c.device.Get(), strip.indices.size() * 2 + 16, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(ib.Get(), strip.indices.data(), strip.indices.size() * 2);
        struct {
          D3D12_DRAW_INDEXED_ARGUMENTS commands[5];
          UINT count[4];
        } words = {{{strip.count, 1, 0, 0, 0},
                    {0, 1, 0, 0, 0},
                    {strip.count2, 1, strip.start2, 1, 0},
                    {0x4000000u, 4, 0, 0, 0},
                    {0x4000000u, 4, 0, 0, 0}},
                   {3, 0, 0, 0}};
        auto argsUpload = buffer(c.device.Get(), sizeof(words), D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(argsUpload.Get(), &words, sizeof(words));
        auto args = buffer(c.device.Get(), sizeof(words), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_INDIRECT_ARGUMENT_DESC argument = {};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC sd = {};
        sd.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &argument;
        ComPtr<ID3D12CommandSignature> signature;
        check(c.device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&signature)), "strip-signature");
        const UINT kSoBytes = (strip.frontier ? 512u : 256u) * 32; // every record fits
        auto target = buffer(c.device.Get(), kSoBytes + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
        auto readback = buffer(c.device.Get(), kSoBytes + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto zero = buffer(c.device.Get(), kSoBytes + 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::vector<uint8_t> zeros(kSoBytes + 256, 0);
        upload(zero.Get(), zeros.data(), zeros.size());
        D3D12_QUERY_HEAP_DESC q = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0};
        ComPtr<ID3D12QueryHeap> queries;
        check(c.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "strip-query");
        auto stats = buffer(c.device.Get(), sizeof(result.statistics), D3D12_HEAP_TYPE_READBACK,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        c.begin();
        c.list->CopyBufferRegion(args.Get(), 0, argsUpload.Get(), 0, sizeof(words));
        auto toArgs = transition(args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto toCopy = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER barriers[] = {toArgs, toCopy};
        c.list->ResourceBarrier(2, barriers);
        c.list->CopyBufferRegion(target.Get(), 0, zero.Get(), 0, kSoBytes + 256);
        auto toSo = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        c.list->ResourceBarrier(1, &toSo);
        setupPass(c);
        c.list->SetPipelineState(pso.Get());
        const float color[4] = {1, 0, 0.5f, 1};
        c.list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
        c.list->IASetPrimitiveTopology(strip.topology);
        c.list->IASetVertexBuffers(0, 1, &view);
        D3D12_INDEX_BUFFER_VIEW index = {ib->GetGPUVirtualAddress(), UINT(strip.indices.size() * 2 + 16),
                                         DXGI_FORMAT_R16_UINT};
        c.list->IASetIndexBuffer(&index);
        D3D12_STREAM_OUTPUT_BUFFER_VIEW soView = {target->GetGPUVirtualAddress(), kSoBytes,
                                                  target->GetGPUVirtualAddress() + kSoBytes};
        if (so)
          c.list->SOSetTargets(0, 1, &soView);
        c.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        if (indirect) {
          c.list->ExecuteIndirect(signature.Get(), 5, args.Get(), 0, args.Get(),
                                  5 * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        } else {
          c.list->DrawIndexedInstanced(strip.count, 1, 0, 0, 0);
          c.list->DrawIndexedInstanced(strip.count2, 1, strip.start2, 1, 0);
        }
        c.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        c.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, stats.Get(), 0);
        if (so)
          c.list->SOSetTargets(0, 0, nullptr);
        auto toRead = transition(target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        c.list->ResourceBarrier(1, &toRead);
        c.list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, kSoBytes + 256);
        auto back = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
        c.list->ResourceBarrier(1, &back);
        result.image = finishPass(c);
        void *mapped = nullptr;
        D3D12_RANGE range = {0, kSoBytes + 256}, none = {};
        check(readback->Map(0, &range, &mapped), "strip-map-so");
        const auto *bytes = static_cast<const uint8_t *>(mapped);
        std::memcpy(&result.filled, bytes + kSoBytes, sizeof(result.filled));
        result.so.assign(bytes, bytes + kSoBytes);
        if (strip.frontier)
          require(std::all_of(bytes + kSoBytes + 8, bytes + kSoBytes + 256,
                              [](uint8_t value) { return value == 0; }), "frontier-strip-so-tail-guard");
        readback->Unmap(0, &none);
        range = {0, sizeof(result.statistics)};
        check(stats->Map(0, &range, &mapped), "strip-map-stats");
        std::memcpy(&result.statistics, mapped, sizeof(result.statistics));
        stats->Unmap(0, &none);
        if (strip.frontier) {
          const bool adjacency = strip.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
          const uint64_t wantedInvocations = adjacency ? 13u : 48u;
          const uint64_t wantedPrimitives = adjacency ? 52u : 96u;
          require(result.statistics.GSInvocations == wantedInvocations &&
                  result.statistics.GSPrimitives == wantedPrimitives &&
                  result.filled == wantedPrimitives * 3 * stride, "frontier-strip-complete-consumer-counts");
          if (indirect) {
            std::fprintf(stderr, "OWNED_D3D12_CAPTURE_FRONTIER_PHASE suite=geometry shader=%s case=%s end\n",
                         useDxil ? "dxil" : "dxbc", strip.name);
            std::fflush(stderr);
          }
        }
        return result;
      };
      const Result direct = record(false), indirect = record(true);
      const std::string name = std::string(strip.name) + (so ? "-so" : "");
      cases += compare(name.c_str(), indirect.image, direct.image);
      const auto &a = indirect.statistics, &b = direct.statistics;
      const bool statsEqual = a.IAVertices == b.IAVertices && a.IAPrimitives == b.IAPrimitives &&
                              a.VSInvocations == b.VSInvocations && a.GSInvocations == b.GSInvocations &&
                              a.GSPrimitives == b.GSPrimitives && a.CInvocations == b.CInvocations &&
                              a.CPrimitives == b.CPrimitives && a.PSInvocations == b.PSInvocations;
      const bool soEqual = indirect.filled == direct.filled && indirect.so == direct.so && (!so || direct.filled > 0);
      std::printf("%s_STRIP_EXPANSION shader=%s case=%s ia=%llu,%llu gs=%llu,%llu ps=%llu filled=%llu/%llu\n",
                  kPrefix, useDxil ? "dxil" : "dxbc", name.c_str(), a.IAVertices, a.IAPrimitives, a.GSInvocations,
                  a.GSPrimitives, a.PSInvocations, (unsigned long long)indirect.filled,
                  (unsigned long long)direct.filled);
      if (!statsEqual || !soEqual || !b.GSInvocations)
        fail((name + "-equal").c_str());
      if (strip.frontier)
        std::printf("%s_CAPTURE_FRONTIER_PASS shader=%s case=%s raw_positions=16657 carry=1 "
                    "commands=2 gs=%llu primitives=%llu bytes=%llu guards=1 direct=exact\n",
                    kPrefix, useDxil ? "dxil" : "dxbc", strip.name, a.GSInvocations, a.GSPrimitives,
                    (unsigned long long)indirect.filled);
      std::printf("%s_CASE_PASS shader=%s case=%s-equal\n", kPrefix, useDxil ? "dxil" : "dxbc", name.c_str());
      ++cases;
    }
  return cases;
}

#include "owned-d3d12-gsindirect-contract.inc"

int runShaderSet(Context &c) {
  Shaders s;
  s.vs = shader("vs_pass", "vs_5_0");
  s.ps = shader("ps_color", "ps_5_0");
  s.early = shader("ps_early", "ps_5_0");
  s.quad = shader("gs_quad", "gs_5_0");
  s.tri = shader("gs_tri", "gs_5_0");
  s.linein = shader("gs_linein", "gs_5_0");
  s.lineadj = shader("gs_lineadj", "gs_5_0");
  s.triadj = shader("gs_triadj", "gs_5_0");
  s.line = shader("gs_line", "gs_5_0");
  s.points = shader("gs_points", "gs_5_0");
  s.streams2 = shader("gs_streams2", "gs_5_0");
  s.streams4 = shader("gs_streams4", "gs_5_0");
  createTarget(c, 1);
  int cases = 0;
  cases += casesPoints(c, s);
  cases += casesTriangles(c, s);
  cases += casesLines(c, s);
  cases += casesAdjacency(c, s);
  cases += casesValidation(c, s);
  cases += casesMultisample(c, s);
  cases += casesEarlyDepthStatistics(c, s);
  cases += casesIndirectStripExpansion(c, s);
  cases += casesStreamOutput(c, s);
  cases += casesMultiStream(c, s);
  cases += casesStreamOutputPredication(c, s);
  cases += casesIndirectMutation(c, false);
  std::printf("%s_MATRIX_PASS shader=%s cases=%d\n", kPrefix, useDxil ? "dxil" : "dxbc", cases);
  return cases;
}

bool fileExists(const char *name) {
  return GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES;
}
} // namespace

int main(int argc, char **argv) {
  Context c;
  init(c);
  hlslSource = readFile("geometry.hlsl");
  useDxil = false;
  runShaderSet(c);
  if (fileExists("geometry-vs_pass.dxil")) {
    useDxil = true;
    runShaderSet(c);
  } else {
    std::printf("%s_DXIL_SKIPPED reason=no-frozen-dxil\n", kPrefix);
  }
  std::printf("%s_PASS\n", kPrefix);
  return 0;
}
