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
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr char kPrefix[] = "OWNED_D3D12_INDEXED_RESTART";
constexpr UINT kWidth = 16, kHeight = 16, kPitch = 256;
struct Point { float x, y; };
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_INDEXED_RESTART_CASE_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool ok, const char *step) { if (!ok) fail(step); }
std::vector<uint8_t> readFile(const char *name) {
  HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail("open-input", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) fail("size-input");
  std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
  DWORD count = 0;
  if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
    fail("read-input");
  CloseHandle(file);
  return bytes;
}
ComPtr<ID3DBlob> compile(const std::vector<uint8_t> &source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> result, errors;
  HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("compile-dxbc", hr);
  }
  return result;
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h = {}; h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1; return h;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE type,
                              D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
  d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  auto h = heap(type); ComPtr<ID3D12Resource> r;
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
  return r;
}
void upload(ID3D12Resource *buffer, const void *data, size_t size) {
  void *mapped = nullptr; check(buffer->Map(0, nullptr, &mapped), "map-upload");
  std::memcpy(mapped, data, size); buffer->Unmap(0, nullptr);
}
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before; b.Transition.StateAfter = after; return b;
}
struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 fenceValue = 0;
  HANDLE event = nullptr;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12DescriptorHeap> rtvs;
  ComPtr<ID3D12CommandSignature> simple, mutating;
  ~Context() { if (event) CloseHandle(event); }
  void submit(ID3D12GraphicsCommandList *list) {
    check(list->Close(), "close"); ID3D12CommandList *lists[] = {list}; queue->ExecuteCommandLists(1, lists);
    UINT64 value = ++fenceValue; check(queue->Signal(fence.Get(), value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "event");
      require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "wait");
    }
    check(device->GetDeviceRemovedReason(), "device-removed");
  }
};
void init(Context &c) {
  HMODULE module = GetModuleHandleW(L"d3d12.dll"); require(module != nullptr, "module");
  using Backend = const char *(WINAPI *)();
  auto backend = reinterpret_cast<Backend>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "backend");
  std::printf("%s_BACKEND id=%s\n", kPrefix, backend());
  ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d = {}; check(adapter->GetDesc1(&d), "adapter-desc");
    if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)))) break;
    adapter.Reset();
  }
  require(c.device != nullptr, "device");
  D3D12_COMMAND_QUEUE_DESC q = {}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(c.device->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)), "queue");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");
  c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(c.event != nullptr, "event-create");
  D3D12_ROOT_PARAMETER constant = {}; constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  constant.Constants.ShaderRegister = 0; constant.Constants.Num32BitValues = 2;
  constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_ROOT_SIGNATURE_DESC root = {}; root.NumParameters = 1; root.pParameters = &constant;
  root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> code, errors;
  check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors), "root-serialize");
  check(c.device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&c.root)), "root");
  D3D12_DESCRIPTOR_HEAP_DESC rtv = {}; rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = 1;
  check(c.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&c.rtvs)), "rtv-heap");
  D3D12_INDIRECT_ARGUMENT_DESC draw = {}; draw.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  D3D12_COMMAND_SIGNATURE_DESC signature = {}; signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  signature.NumArgumentDescs = 1; signature.pArgumentDescs = &draw;
  check(c.device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&c.simple)), "simple-signature");
  D3D12_INDIRECT_ARGUMENT_DESC args[4] = {};
  args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  args[0].Constant.RootParameterIndex = 0; args[0].Constant.Num32BitValuesToSet = 2;
  args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW; args[1].VertexBuffer.Slot = 0;
  args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
  args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  signature.ByteStride = 8 + sizeof(D3D12_VERTEX_BUFFER_VIEW) + sizeof(D3D12_INDEX_BUFFER_VIEW) +
                         sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  signature.NumArgumentDescs = 4; signature.pArgumentDescs = args;
  check(c.device->CreateCommandSignature(&signature, c.root.Get(), IID_PPV_ARGS(&c.mutating)), "mutating-signature");
}
Point procedural(uint32_t id) {
  if (id == 0xffffffffu) return {12.5f, 13.5f};
  if (id == 0xffffu) return {3.5f, 13.5f};
  constexpr Point points[] = {{3.5f, 3.5f}, {12.5f, 3.5f}, {3.5f, 12.5f}, {12.5f, 12.5f}};
  return points[id % 4];
}
enum class Mode { Direct, Predicated, Simple, Mutating };
struct Draw {
  UINT count = 0, instances = 1, start = 0;
  INT base = 0;
  UINT startInstance = 0;
  Point offset = {0, 0};
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct Case {
  const char *name;
  DXGI_FORMAT format;
  D3D_PRIMITIVE_TOPOLOGY topology;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut;
  bool attributes = false;
  Mode mode = Mode::Direct;
  std::vector<uint32_t> indices;
  std::vector<Draw> draws;
  UINT viewOffset = 0;
  bool predicatePass = true;
  bool usePredicate = false, mixedFormats = false;
  UINT maxCommands = 0, executeCount = 0;
  Case(const char *n, DXGI_FORMAT f, D3D_PRIMITIVE_TOPOLOGY t, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE v)
      : name(n), format(f), topology(t), cut(v) {}
};
bool isStrip(D3D_PRIMITIVE_TOPOLOGY t) {
  return t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || t == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
}
UINT primitiveSize(D3D_PRIMITIVE_TOPOLOGY t) {
  return t == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? 1 :
         (t == D3D_PRIMITIVE_TOPOLOGY_LINELIST || t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ? 2 : 3);
}
using Primitive = std::array<Point, 3>;
std::vector<Primitive> expected(const Case &c) {
  std::vector<Primitive> result;
  const UINT size = primitiveSize(c.topology);
  for (const Draw &draw : c.draws) {
    DXGI_FORMAT format = draw.format == DXGI_FORMAT_UNKNOWN ? c.format : draw.format;
    const uint32_t maximum = format == DXGI_FORMAT_R16_UINT ? 0xffffu : 0xffffffffu;
    bool cut = isStrip(c.topology) &&
        ((maximum == 0xffffu && c.cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
         (maximum == 0xffffffffu && c.cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF));
    for (UINT instance = 0; instance < draw.instances; ++instance) {
      std::vector<Point> run;
      for (UINT n = 0; n < draw.count; ++n) {
        const uint32_t index = c.indices.at(draw.start + n);
        if (cut && index == maximum) { run.clear(); continue; }
        Point p;
        if (c.attributes) {
          const uint32_t address = index + static_cast<uint32_t>(draw.base);
          require(address < 65536, "oracle-vb-address");
          p = procedural(address);
          constexpr Point instanceOffsets[] = {{0, 0}, {0, 0}, {0, 0}, {1, 0}, {-1, 0}, {0, 1}};
          require(draw.startInstance + instance < std::size(instanceOffsets), "oracle-instance-address");
          p.x += instanceOffsets[draw.startInstance + instance].x;
          p.y += instanceOffsets[draw.startInstance + instance].y;
        } else p = procedural(index);
        p.x += draw.offset.x; p.y += draw.offset.y;
        run.push_back(p);
        if (run.size() < size) continue;
        if (isStrip(c.topology) || run.size() % size == 0) {
          Primitive prim = {run[run.size() - size], run[run.size() - size + 1], run.back()};
          result.push_back(prim);
        }
      }
    }
  }
  return result;
}
// Ignore subpixel edge ties rather than making this fixture a rasterizer conformance test.
// Every compared pixel still has an exact, integer expected blend result.
int classification(const Primitive &p, UINT n, float x, float y) {
  if (n == 1) return (x == p[0].x && y == p[0].y) ? 1 : 0;
  if (n == 2) {
    float dx = p[1].x - p[0].x, dy = p[1].y - p[0].y;
    float len2 = dx * dx + dy * dy;
    if (len2 == 0) return 0;
    float t = ((x - p[0].x) * dx + (y - p[0].y) * dy) / len2;
    float dist2 = std::pow(x - p[0].x - t * dx, 2) + std::pow(y - p[0].y - t * dy, 2);
    if (t > 0.1f && t < 0.9f && dist2 < 0.0001f) return 1;
    if (t < -0.2f || t > 1.2f || dist2 > 2.25f) return 0;
    return -1;
  }
  auto edge = [](Point a, Point b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
  };
  float area = edge(p[0], p[1], p[2].x, p[2].y);
  if (area == 0) return 0;
  float e0 = edge(p[0], p[1], x, y) * (area > 0 ? 1 : -1);
  float e1 = edge(p[1], p[2], x, y) * (area > 0 ? 1 : -1);
  float e2 = edge(p[2], p[0], x, y) * (area > 0 ? 1 : -1);
  if (e0 > 0.25f && e1 > 0.25f && e2 > 0.25f) return 1;
  if (e0 < -0.25f || e1 < -0.25f || e2 < -0.25f) return 0;
  return -1;
}
D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(D3D_PRIMITIVE_TOPOLOGY topology) {
  if (topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST) return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  if (topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST || topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP)
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}
struct Shaders { D3D12_SHADER_BYTECODE proceduralVS, attributeVS, ps; };
ComPtr<ID3D12PipelineState> pipeline(Context &ctx, const Case &c, const Shaders &shaders) {
  D3D12_INPUT_ELEMENT_DESC inputs[2] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"INSTANCEOFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = ctx.root.Get(); d.VS = c.attributes ? shaders.attributeVS : shaders.proceduralVS;
  d.PS = shaders.ps; d.InputLayout = c.attributes ? D3D12_INPUT_LAYOUT_DESC{inputs, 2} : D3D12_INPUT_LAYOUT_DESC{};
  d.BlendState.RenderTarget[0].BlendEnable = TRUE;
  auto &b = d.BlendState.RenderTarget[0]; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ONE;
  b.BlendOp = D3D12_BLEND_OP_ADD; b.SrcBlendAlpha = b.DestBlendAlpha = D3D12_BLEND_ONE;
  b.BlendOpAlpha = D3D12_BLEND_OP_ADD; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
  d.SampleMask = UINT_MAX; d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
  d.DepthStencilState.DepthEnable = FALSE; d.DepthStencilState.StencilEnable = FALSE;
  d.PrimitiveTopologyType = topologyType(c.topology); d.IBStripCutValue = c.cut;
  d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT; d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  check(ctx.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "pipeline");
  return pso;
}
#pragma pack(push, 4)
struct MutableRecord {
  float offset[2];
  D3D12_VERTEX_BUFFER_VIEW vertex;
  D3D12_INDEX_BUFFER_VIEW index;
  D3D12_DRAW_INDEXED_ARGUMENTS draw;
};
#pragma pack(pop)
static_assert(sizeof(MutableRecord) == 8 + 16 + 16 + 20);
void runCase(Context &ctx, const Case &c, const Shaders &shaders, const char *shaderName) {
  auto pso = pipeline(ctx, c, shaders);
  D3D12_RESOURCE_DESC texture = {}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = kWidth; texture.Height = kHeight; texture.DepthOrArraySize = texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R32_FLOAT; texture.SampleDesc.Count = 1;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {}; clear.Format = texture.Format;
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT); ComPtr<ID3D12Resource> target;
  check(ctx.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &clear, IID_PPV_ARGS(&target)), "target");
  ctx.device->CreateRenderTargetView(target.Get(), nullptr, ctx.rtvs->GetCPUDescriptorHandleForHeapStart());
  auto readback = buffer(ctx.device.Get(), kPitch * kHeight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT step = c.format == DXGI_FORMAT_R16_UINT ? 2 : 4;
  const UINT viewSize = c.mixedFormats ? 42 : static_cast<UINT>(c.indices.size()) * step;
  std::vector<uint8_t> raw(c.viewOffset + viewSize);
  std::fill(raw.begin(), raw.end(), 0xab);
  if (c.mixedFormats) {
    require(c.indices.size() == 15 && c.draws.size() == 3, "mixed-layout");
    // Pad R16's five indices to a DWORD boundary before the R32 view.
    const UINT starts[] = {0, 12, 32};
    const UINT strides[] = {2, 4, 2};
    for (UINT command = 0; command < 3; ++command)
      for (UINT n = 0; n < 5; ++n)
        std::memcpy(raw.data() + c.viewOffset + starts[command] + n * strides[command],
                    &c.indices[command * 5 + n], strides[command]);
  } else for (size_t i = 0; i < c.indices.size(); ++i)
    std::memcpy(raw.data() + c.viewOffset + i * step, &c.indices[i], step);
  auto source = buffer(ctx.device.Get(), raw.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto index = buffer(ctx.device.Get(), raw.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  upload(source.Get(), raw.data(), raw.size());
  D3D12_INDEX_BUFFER_VIEW ibv = {index->GetGPUVirtualAddress() + c.viewOffset, viewSize, c.format};
  std::vector<Point> vertices;
  if (c.attributes) {
    vertices.reserve(65536);
    for (UINT i = 0; i < 65536; ++i) vertices.push_back(procedural(i));
  }
  auto vb = buffer(ctx.device.Get(), c.attributes ? vertices.size() * sizeof(Point) : sizeof(Point),
                   D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (c.attributes) upload(vb.Get(), vertices.data(), vertices.size() * sizeof(Point));
  D3D12_VERTEX_BUFFER_VIEW vbv = {vb->GetGPUVirtualAddress(),
                                 static_cast<UINT>(c.attributes ? vertices.size() * sizeof(Point) : sizeof(Point)),
                                 sizeof(Point)};
  constexpr Point offsets[] = {{0, 0}, {0, 0}, {0, 0}, {1, 0}, {-1, 0}, {0, 1}};
  auto instances = buffer(ctx.device.Get(), sizeof(offsets), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(instances.Get(), offsets, sizeof(offsets));
  D3D12_VERTEX_BUFFER_VIEW instanceView = {instances->GetGPUVirtualAddress(), sizeof(offsets), sizeof(Point)};
  std::vector<uint8_t> argumentBytes;
  UINT commandCount = c.maxCommands ? c.maxCommands : static_cast<UINT>(c.draws.size());
  if (c.mode == Mode::Simple || c.mode == Mode::Mutating) {
    for (UINT i = 0; i < commandCount; ++i) {
      Draw draw = i < c.draws.size() ? c.draws[i] : Draw{1, 1, 0, 0, 0, {6, 0}};
      D3D12_DRAW_INDEXED_ARGUMENTS args = {draw.count, draw.instances, draw.start, draw.base, draw.startInstance};
      if (c.mode == Mode::Simple) {
        auto *p = reinterpret_cast<const uint8_t *>(&args);
        argumentBytes.insert(argumentBytes.end(), p, p + sizeof(args));
      } else {
        // Each command binds a distinct IB view beginning at its own range; StartIndexLocation
        // is then relative to that view, not the IB bound before ExecuteIndirect.
        D3D12_INDEX_BUFFER_VIEW commandView = ibv;
        const UINT mixedStart[] = {0, 12, 32};
        const UINT offset = c.mixedFormats ? mixedStart[i] : draw.start * step;
        commandView.BufferLocation += offset;
        commandView.SizeInBytes = c.mixedFormats ? draw.count * (draw.format == DXGI_FORMAT_R16_UINT ? 2 : 4)
                                                : commandView.SizeInBytes - offset;
        if (c.mixedFormats) commandView.Format = draw.format;
        args.StartIndexLocation = 0;
        MutableRecord record = {{draw.offset.x, draw.offset.y}, vbv, commandView, args};
        auto *p = reinterpret_cast<const uint8_t *>(&record);
        argumentBytes.insert(argumentBytes.end(), p, p + sizeof(record));
      }
    }
  }
  ComPtr<ID3D12Resource> argSource, argBuffer, countSource, countBuffer;
  if (!argumentBytes.empty()) {
    argSource = buffer(ctx.device.Get(), argumentBytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    argBuffer = buffer(ctx.device.Get(), argumentBytes.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    upload(argSource.Get(), argumentBytes.data(), argumentBytes.size());
    UINT count = c.executeCount ? c.executeCount : static_cast<UINT>(c.draws.size());
    countSource = buffer(ctx.device.Get(), 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    countBuffer = buffer(ctx.device.Get(), 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    upload(countSource.Get(), &count, sizeof(count));
  }
  uint64_t predicate = c.predicatePass ? 1 : 0;
  ComPtr<ID3D12Resource> predicateSource, predicateBuffer;
  if (c.mode == Mode::Predicated || c.usePredicate || !c.predicatePass) {
    predicateSource = buffer(ctx.device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    predicateBuffer = buffer(ctx.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    upload(predicateSource.Get(), &predicate, sizeof(predicate));
  }
  ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
  check(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  check(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                      IID_PPV_ARGS(&list)), "list");
  list->CopyBufferRegion(index.Get(), 0, source.Get(), 0, raw.size());
  auto ibBarrier = barrier(index.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
  list->ResourceBarrier(1, &ibBarrier);
  if (argBuffer) {
    list->CopyBufferRegion(argBuffer.Get(), 0, argSource.Get(), 0, argumentBytes.size());
    list->CopyBufferRegion(countBuffer.Get(), 0, countSource.Get(), 0, 4);
    D3D12_RESOURCE_BARRIER bs[] = {
        barrier(argBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        barrier(countBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
    list->ResourceBarrier(2, bs);
  }
  if (predicateBuffer) {
    list->CopyBufferRegion(predicateBuffer.Get(), 0, predicateSource.Get(), 0, 8);
    auto b = barrier(predicateBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
    list->ResourceBarrier(1, &b);
  }
  auto handle = ctx.rtvs->GetCPUDescriptorHandleForHeapStart();
  const float black[4] = {}; list->ClearRenderTargetView(handle, black, 0, nullptr);
  list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0, 0, float(kWidth), float(kHeight), 0, 1};
  D3D12_RECT scissor = {0, 0, LONG(kWidth), LONG(kHeight)};
  list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
  list->SetGraphicsRootSignature(ctx.root.Get()); list->IASetPrimitiveTopology(c.topology);
  list->IASetIndexBuffer(&ibv);
  D3D12_VERTEX_BUFFER_VIEW views[] = {vbv, instanceView};
  if (c.attributes) list->IASetVertexBuffers(0, 2, views);
  else list->IASetVertexBuffers(0, 1, views);
  // D3D12_PREDICATION_OP names the skip condition: EQUAL_ZERO skips zero, executes nonzero.
  if (predicateBuffer) list->SetPredication(predicateBuffer.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  if (c.mode == Mode::Simple || c.mode == Mode::Mutating) {
    float offset[] = {0, 0}; list->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
    list->ExecuteIndirect(c.mode == Mode::Simple ? ctx.simple.Get() : ctx.mutating.Get(), commandCount,
                          argBuffer.Get(), 0, countBuffer.Get(), 0);
  } else for (const Draw &d : c.draws) {
    float offset[] = {d.offset.x, d.offset.y}; list->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
    list->DrawIndexedInstanced(d.count, d.instances, d.start, d.base, d.startInstance);
  }
  if (predicateBuffer) list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  auto toCopy = barrier(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &toCopy);
  D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
  dst.PlacedFootprint.Footprint.Width = kWidth; dst.PlacedFootprint.Footprint.Height = kHeight;
  dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = kPitch;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  ctx.submit(list.Get());
  const auto primitives = c.predicatePass ? expected(c) : std::vector<Primitive>{};
  UINT compared = 0, nonzero = 0;
  uint8_t *pixels = nullptr; check(readback->Map(0, nullptr, reinterpret_cast<void **>(&pixels)), "map-readback");
  for (UINT y = 0; y < kHeight; ++y) for (UINT x = 0; x < kWidth; ++x) {
    UINT exact = 0; bool ambiguous = false;
    for (const auto &p : primitives) {
      int v = classification(p, primitiveSize(c.topology), x + 0.5f, y + 0.5f);
      if (v < 0) ambiguous = true;
      else exact += static_cast<UINT>(v);
    }
    if (ambiguous) continue;
    float actual; std::memcpy(&actual, pixels + y * kPitch + x * sizeof(float), sizeof(actual));
    if (actual != static_cast<float>(exact)) {
      std::fprintf(stderr, "%s_CASE_FAIL shader=%s case=%s pixel=(%u,%u) expected=%u actual=%g\n",
                   kPrefix, shaderName, c.name, x, y, exact, actual);
      ExitProcess(1);
    }
    ++compared; if (exact) ++nonzero;
  }
  readback->Unmap(0, nullptr);
  require(compared > 100, "too-few-unambiguous-pixels");
  require(nonzero || primitives.empty(), "no-observed-primitive");
  std::printf("%s_CASE_PASS shader=%s case=%s compared=%u nonzero=%u primitives=%zu\n",
              kPrefix, shaderName, c.name, compared, nonzero, primitives.size());
}
std::vector<Case> cases() {
  using C = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE;
  const C off = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  const C cut16 = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
  const C cut32 = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
  std::vector<Case> out;
  for (DXGI_FORMAT format : {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT}) {
    const uint32_t maximum = format == DXGI_FORMAT_R16_UINT ? 0xffffu : 0xffffffffu;
    const C match = format == DXGI_FORMAT_R16_UINT ? cut16 : cut32;
    const C mismatch = format == DXGI_FORMAT_R16_UINT ? cut32 : cut16;
    for (auto topology : {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                          D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}) {
      const bool sixteen = format == DXGI_FORMAT_R16_UINT;
      const char *name = topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ?
          (sixteen ? "point-list-16-disabled" : "point-list-32-disabled") :
          topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ?
          (sixteen ? "line-list-16-disabled" : "line-list-32-disabled") :
          (sixteen ? "triangle-list-16-disabled" : "triangle-list-32-disabled");
      Case c = {name, format, topology, off};
      c.indices = {0, 1, maximum, 2, maximum, 3, 0, 1, 2, maximum, maximum, maximum};
      c.draws = {{static_cast<UINT>(c.indices.size() - 2), 1, 2, 0, 0, {0, 0}}};
      c.viewOffset = format == DXGI_FORMAT_R16_UINT ? 6 : 12;
      out.push_back(c);
    }
    for (auto topology : {D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP}) {
      for (C cut : {off, match, mismatch}) {
        const bool sixteen = format == DXGI_FORMAT_R16_UINT;
        const char *name = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ?
            (sixteen ? (cut == off ? "line-strip-16-disabled" :
                        cut == match ? "line-strip-16-matching-cut" : "line-strip-16-mismatched-cut") :
                       (cut == off ? "line-strip-32-disabled" :
                        cut == match ? "line-strip-32-matching-cut" : "line-strip-32-mismatched-cut")) :
            (sixteen ? (cut == off ? "triangle-strip-16-disabled" :
                        cut == match ? "triangle-strip-16-matching-cut" : "triangle-strip-16-mismatched-cut") :
                       (cut == off ? "triangle-strip-32-disabled" :
                        cut == match ? "triangle-strip-32-matching-cut" : "triangle-strip-32-mismatched-cut"));
        Case c = {name, format, topology, cut};
        c.indices = {3, 0, 1, 2, maximum, 0, 1, 2, 3, 0};
        c.draws = {{8, 1, 1, 0, 0, {0, 0}}}; c.viewOffset = format == DXGI_FORMAT_R16_UINT ? 4 : 8;
        out.push_back(c);
        if (topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP && sixteen && cut == off) {
          // A second affected draw using the same already-created PSO tests warm CachedPSO binding.
          c.name = "line-strip-16-disabled-warm-pso";
          c.draws.push_back({8, 1, 1, 0, 0, {1, 0}});
          out.push_back(c);
        }
      }
    }
  }
  {
    Case c = {"repeated-and-instanced", DXGI_FORMAT_R16_UINT, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, off};
    c.indices = {0xffffu, 0xffffu, 1, 1, 2}; c.draws = {{5, 2, 0, 0, 3, {0, 0}}}; out.push_back(c);
  }
  {
    Case c = {"base-negative-instance", DXGI_FORMAT_R16_UINT, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, off};
    c.attributes = true; c.indices = {65532, 65533, 65535, 65532, 65535, 65534};
    c.draws = {{6, 2, 0, -65532, 3, {0, 0}}}; out.push_back(c);
  }
  {
    Case c = {"base-positive-instance", DXGI_FORMAT_R32_UINT, D3D_PRIMITIVE_TOPOLOGY_LINELIST, off};
    c.attributes = true; c.indices = {0, 1, 0, 2, 1, 3}; c.draws = {{6, 2, 0, 2, 2, {0, 0}}}; out.push_back(c);
  }
  {
    Case c = {"attribute-cut", DXGI_FORMAT_R16_UINT, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, cut16};
    c.attributes = true; c.indices = {65532, 65533, 65534, 65535, 65532, 65533, 65534};
    c.draws = {{7, 1, 0, -65532, 2, {0, 0}}}; out.push_back(c);
  }
  {
    Case c = {"gpu-index-and-predicated-direct", DXGI_FORMAT_R32_UINT, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, off};
    c.mode = Mode::Predicated; c.indices = {0, 1, 2, 0xffffffffu, 0, 1, 2};
    c.draws = {{7, 1, 0, 0, 0, {0, 0}}}; out.push_back(c);
    c.name = "predicated-skip"; c.predicatePass = false; out.push_back(c);
  }
  {
    Case c = {"gpu-args-count-simple", DXGI_FORMAT_R16_UINT, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, off};
    c.mode = Mode::Simple; c.indices = {0, 1, 65535, 2, 3, 65535, 3, 0, 2};
    c.draws = {{3, 1, 0, 0, 0, {0, 0}}, {3, 1, 3, 0, 0, {0, 0}}};
    c.maxCommands = 3; c.executeCount = 2; out.push_back(c);
  }
  for (DXGI_FORMAT format : {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT}) {
    const uint32_t maximum = format == DXGI_FORMAT_R16_UINT ? 0xffffu : 0xffffffffu;
    Case c = {format == DXGI_FORMAT_R16_UINT ? "gpu-simple-strip-16-count-predicate" :
              "gpu-simple-strip-32-count-predicate", format, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, off};
    c.mode = Mode::Simple; c.usePredicate = true; c.viewOffset = format == DXGI_FORMAT_R16_UINT ? 8 : 16;
    c.indices = {0, 1, maximum, 2, 3, 0, 1, maximum, 2, 3};
    c.draws = {{5, 1, 0, 0, 0, {0, 0}}, {5, 2, 5, 0, 3, {0, 0}}};
    c.maxCommands = 3; c.executeCount = 2; out.push_back(c);
  }
  {
    Case c = {"gpu-icb-switch-r16-r32-r16", DXGI_FORMAT_R16_UINT,
              D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, cut16};
    c.mode = Mode::Mutating; c.mixedFormats = true; c.viewOffset = 8;
    c.indices = {0, 1, 0xffffu, 2, 3,
                 0, 1, 0xffffffffu, 2, 3,
                 1, 2, 0xffffu, 3, 4};
    c.draws = {{5, 1, 0, 2, 0, {0, 0}, DXGI_FORMAT_R16_UINT},
               {5, 2, 5, 0, 2, {1, 0}, DXGI_FORMAT_R32_UINT},
               {5, 1, 10, -1, 4, {-1, 0}, DXGI_FORMAT_R16_UINT}};
    out.push_back(c);
  }
  {
    Case c = {"gpu-state-mutating-icb", DXGI_FORMAT_R32_UINT, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, off};
    c.mode = Mode::Mutating; c.indices = {0, 1, 2, 0xffffffffu, 1, 2, 3};
    c.draws = {{4, 1, 0, 0, 0, {0, 0}}, {4, 1, 3, 0, 0, {-1, 0}}}; out.push_back(c);
    c.name = "gpu-state-mutating-icb-predicated-skip"; c.predicatePass = false; out.push_back(c);
  }
  return out;
}
} // namespace
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  require(argc == 5, "usage: fixture indexed-restart.hlsl procedural-vs.dxil attribute-vs.dxil ps.dxil");
  auto source = readFile(argv[1]);
  auto dxbcProcedural = compile(source, "proceduralVS", "vs_5_0");
  auto dxbcAttribute = compile(source, "attributeVS", "vs_5_0");
  auto dxbcPS = compile(source, "pixelPS", "ps_5_0");
  auto dxilProcedural = readFile(argv[2]), dxilAttribute = readFile(argv[3]), dxilPS = readFile(argv[4]);
  auto blob = [](ID3DBlob *b) { return D3D12_SHADER_BYTECODE{b->GetBufferPointer(), b->GetBufferSize()}; };
  auto bytes = [](const std::vector<uint8_t> &b) { return D3D12_SHADER_BYTECODE{b.data(), b.size()}; };
  Shaders variants[] = {{blob(dxbcProcedural.Get()), blob(dxbcAttribute.Get()), blob(dxbcPS.Get())},
                        {bytes(dxilProcedural), bytes(dxilAttribute), bytes(dxilPS)}};
  Context ctx; init(ctx);
  const auto matrix = cases();
  for (size_t shader = 0; shader < 2; ++shader)
    for (const auto &c : matrix) runCase(ctx, c, variants[shader], shader == 0 ? "dxbc" : "dxil");
  std::printf("OWNED_D3D12_INDEXED_RESTART_MATRIX_PASS shaders=dxbc,dxil cases=%zu\n", matrix.size());
  std::puts("OWNED_D3D12_INDEXED_RESTART_PASS");
  return 0;
}
