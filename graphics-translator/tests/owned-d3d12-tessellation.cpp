// D3D12 tessellation fixture: HS+DS pipelines (DXBC hs/ds_5_0 and frozen DXIL hs/ds_6_0) against an
// independent fixed-function tessellator oracle (owned-d3d12-tessellation-oracle.inc).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "owned-d3d12-tessellation-oracle.inc"

using Microsoft::WRL::ComPtr;
namespace {
constexpr char kPrefix[] = "OWNED_D3D12_TESSELLATION";
constexpr UINT kSize = 32;
constexpr UINT kPitch = 256;
constexpr UINT kLogBytes = 8 + 16383 * 8;
/* 16.16 units. The Metal tessellator places points with D3D's 16.16 edge placement, then interpolates in 1.15 fixed
 * point (2^-15), so points may differ from the 16.16 reference by a few units. The per-case maximum is printed. */
constexpr long kTolerance = 4;

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

std::vector<uint8_t> shader(const std::string &entry, const char *dxbcTarget) {
  if (useDxil)
    return readFile("tessellation-" + entry + ".dxil");
  ComPtr<ID3DBlob> result, errors;
  HRESULT hr = D3DCompile(hlslSource.data(), hlslSource.size(), "tessellation.hlsl", nullptr, nullptr, entry.c_str(),
                          dxbcTarget, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail(entry.c_str(), hr);
  }
  auto *bytes = static_cast<const uint8_t *>(result->GetBufferPointer());
  return {bytes, bytes + result->GetBufferSize()};
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
  ComPtr<ID3D12Resource> target, resolved, readback, log, logReadback, vertices, zero;
  ComPtr<ID3D12QueryHeap> statistics;
  ComPtr<ID3D12Resource> statisticsReadback;
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
      require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "wait");
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

  // b0: tess factors visible to the HULL only; u0: DS domain-point log visible to the DOMAIN only.
  D3D12_ROOT_PARAMETER parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.Num32BitValues = 8;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_HULL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor.ShaderRegister = 0;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
  D3D12_ROOT_SIGNATURE_DESC root = {};
  root.NumParameters = 2;
  root.pParameters = parameters;
  root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> code, errors;
  check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors), "root-serialize");
  check(c.device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&c.root)),
        "root");
  D3D12_DESCRIPTOR_HEAP_DESC rtv = {};
  rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv.NumDescriptors = 1;
  check(c.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&c.rtvs)), "rtv-heap");
  c.readback = buffer(c.device.Get(), kPitch * kSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  c.log = buffer(c.device.Get(), kLogBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  c.logReadback = buffer(c.device.Get(), kLogBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  c.zero = buffer(c.device.Get(), kLogBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(c.zero->Map(0, nullptr, &mapped), "map-zero");
  std::memset(mapped, 0, kLogBytes);
  c.zero->Unmap(0, nullptr);
  D3D12_QUERY_HEAP_DESC query = {};
  query.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
  query.Count = 1;
  check(c.device->CreateQueryHeap(&query, IID_PPV_ARGS(&c.statistics)), "query-heap");
  c.statisticsReadback = buffer(c.device.Get(), sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
                                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  // One patch per draw: the four corners of a centred square (tri patches use the first three).
  const float vertices[8] = {-0.75f, 0.75f, 0.75f, 0.75f, 0.75f, -0.75f, -0.75f, -0.75f};
  c.vertices = buffer(c.device.Get(), sizeof(vertices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  check(c.vertices->Map(0, nullptr, &mapped), "map-vertices");
  std::memcpy(mapped, vertices, sizeof(vertices));
  c.vertices->Unmap(0, nullptr);
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

ComPtr<ID3D12PipelineState> pipeline(Context &c, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &hs,
                                     const std::vector<uint8_t> &ds, const std::vector<uint8_t> &ps) {
  D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = c.root.Get();
  d.VS = {vs.data(), vs.size()};
  d.HS = {hs.data(), hs.size()};
  d.DS = {ds.data(), ds.size()};
  d.PS = {ps.data(), ps.size()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {&element, 1};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = c.samples;
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "pso");
  return pso;
}

using PointSet = std::multiset<std::pair<long, long>>;

struct DrawResult {
  PointSet points;
  uint32_t invocations = 0;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
  std::array<uint32_t, kSize * kSize> texels{};
};

DrawResult draw(Context &c, ID3D12PipelineState *pso, bool quad, const tess_oracle::Factors &factors) {
  DrawResult result;
  c.begin();
  c.list->CopyBufferRegion(c.log.Get(), 0, c.zero.Get(), 0, kLogBytes);
  D3D12_RESOURCE_BARRIER to_uav = transition(c.log.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.list->ResourceBarrier(1, &to_uav);
  auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
  const float zero[4] = {};
  c.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
  D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
  c.list->RSSetViewports(1, &viewport);
  c.list->RSSetScissorRects(1, &scissor);
  c.list->SetGraphicsRootSignature(c.root.Get());
  c.list->SetPipelineState(pso);
  float constants[8] = {factors.edge[0], factors.edge[1], factors.edge[2], factors.edge[3],
                        factors.inside[0], factors.inside[1], 0, 0};
  c.list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
  c.list->SetGraphicsRootUnorderedAccessView(1, c.log->GetGPUVirtualAddress());
  c.list->IASetPrimitiveTopology(quad ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                      : D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
  D3D12_VERTEX_BUFFER_VIEW view = {c.vertices->GetGPUVirtualAddress(), 32, 8};
  c.list->IASetVertexBuffers(0, 1, &view);
  c.list->BeginQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  c.list->DrawInstanced(quad ? 4 : 3, 1, 0, 0);
  c.list->EndQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  c.list->ResolveQueryData(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                           c.statisticsReadback.Get(), 0);
  D3D12_RESOURCE_BARRIER after[2] = {
      transition(c.log.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(c.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  c.list->ResourceBarrier(2, after);
  c.list->CopyBufferRegion(c.logReadback.Get(), 0, c.log.Get(), 0, kLogBytes);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = c.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch};
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = c.target.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER back[2] = {
      transition(c.log.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
      transition(c.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
  c.list->ResourceBarrier(2, back);
  c.submit();

  void *mapped = nullptr;
  D3D12_RANGE range = {0, kLogBytes};
  check(c.logReadback->Map(0, &range, &mapped), "map-log");
  const auto *words = static_cast<const uint32_t *>(mapped);
  result.invocations = words[0];
  const uint32_t stored = std::min<uint32_t>(words[0], 16383);
  for (uint32_t i = 0; i < stored; ++i) {
    float uv[2];
    std::memcpy(uv, words + 2 + i * 2, sizeof(uv));
    result.points.insert({std::lround(double(uv[0]) * 65536.0), std::lround(double(uv[1]) * 65536.0)});
  }
  D3D12_RANGE none = {};
  c.logReadback->Unmap(0, &none);
  range = {0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
  check(c.statisticsReadback->Map(0, &range, &mapped), "map-statistics");
  std::memcpy(&result.statistics, mapped, sizeof(result.statistics));
  c.statisticsReadback->Unmap(0, &none);
  range = {0, kPitch * kSize};
  check(c.readback->Map(0, &range, &mapped), "map-readback");
  for (UINT y = 0; y < kSize; ++y)
    std::memcpy(result.texels.data() + y * kSize, static_cast<uint8_t *>(mapped) + y * kPitch, kSize * 4);
  c.readback->Unmap(0, &none);
  return result;
}

const char *kPartitions[] = {"integer", "pow2", "fractional_odd", "fractional_even"};

int runCase(Context &c, ID3D12PipelineState *pso, bool quad, tess_oracle::Partitioning partition,
            const char *name, const tess_oracle::Factors &factors) {
  const auto expected = quad ? tess_oracle::quad(partition, factors) : tess_oracle::tri(partition, factors);
  PointSet want;
  for (const auto &p : expected)
    want.insert({std::lround(double(p.u) * 65536.0), std::lround(double(p.v) * 65536.0)});
  /* With triangle output D3D11.3 20.4 lets hardware repeat a domain point (each repeat is a DS invocation), so the
   * logged points must equal the oracle's unique set and DSInvocations the number of DS executions. Positions are
   * 16.16 fixed point; inner-ring placement may differ from the reference rounding by 1 ULP (both are exact to
   * the spec's fixed-point precision). */
  DrawResult got = draw(c, pso, quad, factors);
  PointSet unique;
  for (const auto &p : got.points) {
    auto close = std::find_if(unique.begin(), unique.end(), [&](const auto &q) {
      return std::abs(q.first - p.first) <= kTolerance && std::abs(q.second - p.second) <= kTolerance;
    });
    if (close == unique.end())
      unique.insert(p);
  }
  bool matched = unique.size() == want.size();
  long worst = 0;
  for (const auto &p : want) {
    long best = LONG_MAX;
    for (const auto &q : unique)
      best = std::min(best, std::max(std::abs(q.first - p.first), std::abs(q.second - p.second)));
    worst = std::max(worst, best);
  }
  matched = matched && worst <= kTolerance;
  const bool culled = expected.empty();
  int covered = 0;
  for (uint32_t texel : got.texels)
    covered += texel == 0xff00ff00u;
  const auto &s = got.statistics;
  const uint32_t vertices = quad ? 4 : 3;
  bool ok = matched && got.invocations >= want.size() && s.IAVertices == vertices &&
            s.IAPrimitives == 1 && s.VSInvocations == vertices && s.HSInvocations == 1 &&
            s.DSInvocations == got.invocations && s.GSInvocations == 0 && s.GSPrimitives == 0 &&
            s.CSInvocations == 0 && (culled ? covered == 0 && s.CInvocations == 0 : covered > 0);
  if (!ok) {
    std::fprintf(stderr,
                 "%s_MISMATCH shader=%s case=%s domain=%s partition=%s logged=%u unique=%zu want=%zu error=%ld covered=%d "
                 "stats=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                 kPrefix, useDxil ? "dxil" : "dxbc", name, quad ? "quad" : "tri", kPartitions[partition],
                 got.invocations, unique.size(), want.size(), worst, covered, s.IAVertices, s.IAPrimitives,
                 s.VSInvocations, s.GSInvocations, s.GSPrimitives, s.CInvocations, s.CPrimitives, s.PSInvocations,
                 s.HSInvocations, s.DSInvocations, s.CSInvocations);
    int shown = 0;
    auto contains = [](const PointSet &set, const std::pair<long, long> &p) {
      return std::any_of(set.begin(), set.end(), [&](const auto &q) {
        return std::abs(q.first - p.first) <= kTolerance && std::abs(q.second - p.second) <= kTolerance;
      });
    };
    for (const auto &p : want)
      if (!contains(unique, p) && shown++ < 64)
        std::fprintf(stderr, "%s_MISSING u=%ld v=%ld\n", kPrefix, p.first, p.second);
    shown = 0;
    for (const auto &p : unique)
      if (!contains(want, p) && shown++ < 64)
        std::fprintf(stderr, "%s_EXTRA u=%ld v=%ld\n", kPrefix, p.first, p.second);
    fail(name);
  }
  std::printf("%s_CASE_PASS shader=%s case=%s domain=%s partition=%s points=%zu covered=%d ds=%llu "
              "clip=%llu,%llu ps=%llu error=%ld\n",
              kPrefix, useDxil ? "dxil" : "dxbc", name, quad ? "quad" : "tri", kPartitions[partition], want.size(),
              covered, s.DSInvocations, s.CInvocations, s.CPrimitives, s.PSInvocations, worst);
  return 1;
}

/* Point output and the isoline domain. Every logged DS invocation is checked against the oracle's point set within
 * kTolerance; point output must visit each unique point exactly once (D3D11.3 11.7: only unique points within a
 * patch), and isoline line output may repeat points shared by adjacent segments. */
int runOutputCase(Context &c, ID3D12PipelineState *pso, const char *name, const char *domain, const char *partition,
                  const std::vector<tess_oracle::Point> &expected, const tess_oracle::Factors &factors, bool points,
                  bool quadPatch) {
  PointSet want;
  for (const auto &p : expected)
    want.insert({std::lround(double(p.u) * 65536.0), std::lround(double(p.v) * 65536.0)});
  DrawResult got = draw(c, pso, quadPatch, factors);
  auto close = [](const std::pair<long, long> &a, const std::pair<long, long> &b) {
    return std::abs(a.first - b.first) <= kTolerance && std::abs(a.second - b.second) <= kTolerance;
  };
  bool ok = true;
  long worst = 0;
  // Every logged point is an oracle point; every oracle point was logged (exactly once for point output).
  for (const auto &p : got.points)
    ok = ok && std::any_of(want.begin(), want.end(), [&](const auto &q) { return close(p, q); });
  for (const auto &q : want) {
    const auto hits = std::count_if(got.points.begin(), got.points.end(), [&](const auto &p) { return close(p, q); });
    ok = ok && hits >= 1 && (!points || hits == 1);
    long best = LONG_MAX;
    for (const auto &p : got.points)
      best = std::min(best, std::max(std::abs(p.first - q.first), std::abs(p.second - q.second)));
    worst = std::max(worst, want.empty() ? 0 : best);
  }
  const auto &s = got.statistics;
  const uint32_t vertices = quadPatch ? 4 : 3;
  ok = ok && (!points || got.invocations == want.size()) && s.DSInvocations == got.invocations &&
       s.IAVertices == vertices && s.HSInvocations == 1 && s.GSInvocations == 0;
  int covered = 0;
  for (uint32_t texel : got.texels)
    covered += texel == 0xff00ff00u;
  ok = ok && (want.empty() ? covered == 0 : covered > 0);
  if (!ok) {
    std::fprintf(stderr, "%s_MISMATCH shader=%s case=%s domain=%s partition=%s logged=%u want=%zu error=%ld "
                         "covered=%d ds=%llu\n",
                 kPrefix, useDxil ? "dxil" : "dxbc", name, domain, partition, got.invocations, want.size(), worst,
                 covered, s.DSInvocations);
    int shown = 0;
    for (const auto &q : want)
      if (!std::any_of(got.points.begin(), got.points.end(), [&](const auto &p) { return close(p, q); }) &&
          shown++ < 32)
        std::fprintf(stderr, "%s_MISSING u=%ld v=%ld\n", kPrefix, q.first, q.second);
    shown = 0;
    for (const auto &p : got.points)
      if (!std::any_of(want.begin(), want.end(), [&](const auto &q) { return close(p, q); }) && shown++ < 32)
        std::fprintf(stderr, "%s_EXTRA u=%ld v=%ld\n", kPrefix, p.first, p.second);
    fail(name);
  }
  std::printf("%s_CASE_PASS shader=%s case=%s domain=%s partition=%s points=%zu ds=%u covered=%d error=%ld\n",
              kPrefix, useDxil ? "dxil" : "dxbc", name, domain, partition, want.size(), got.invocations, covered,
              worst);
  return 1;
}

int runOutputMatrix(Context &c, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps,
                    const std::vector<uint8_t> &ds_tri, const std::vector<uint8_t> &ds_quad) {
  int cases = 0;
  auto ds_isoline = shader("ds_isoline", "ds_5_0");
  // Isoline factors: edge[0] = line density (SV_TessFactor[0]), edge[1] = line detail.
  const tess_oracle::Factors iso_sets[] = {
      {{1, 1, 0, 0}, {0, 0}}, {{3, 5, 0, 0}, {0, 0}}, {{4.25f, 6.5f, 0, 0}, {0, 0}}, {{64, 64, 0, 0}, {0, 0}},
      {{8, 0, 0, 0}, {0, 0}}};
  const char *iso_names[] = {"isoline-minimum", "isoline-integral", "isoline-fractional", "isoline-max64",
                             "isoline-cull"};
  const tess_oracle::Factors point_sets[] = {
      {{1, 1, 1, 1}, {1, 1}}, {{2, 3, 4, 5}, {3, 2}}, {{2.5f, 3.25f, 5.75f, 7.5f}, {4.5f, 6.25f}},
      {{64, 64, 64, 64}, {64, 64}}};
  const char *point_names[] = {"point-minimum", "point-integral", "point-fractional", "point-max64"};
  for (int partition = 0; partition < 4; ++partition) {
    const auto part = tess_oracle::Partitioning(partition);
    for (bool points : {false, true}) {
      auto hs = shader(std::string("hs_isoline_") + kPartitions[partition] + (points ? "_point" : "_line"),
                       "hs_5_0");
      auto pso = pipeline(c, vs, hs, ds_isoline, ps);
      for (size_t i = 0; i < std::size(iso_sets); ++i) {
        const auto expected = tess_oracle::isoline(part, iso_sets[i].edge[1], iso_sets[i].edge[0]);
        const std::string name = std::string(iso_names[i]) + (points ? "-points" : "-lines");
        cases += runOutputCase(c, pso.Get(), name.c_str(), "isoline", kPartitions[partition], expected, iso_sets[i],
                               points, true);
      }
    }
    auto tri = pipeline(c, vs, shader(std::string("hs_tri_") + kPartitions[partition] + "_point", "hs_5_0"), ds_tri,
                        ps);
    auto quad = pipeline(c, vs, shader(std::string("hs_quad_") + kPartitions[partition] + "_point", "hs_5_0"),
                         ds_quad, ps);
    for (size_t i = 0; i < std::size(point_sets); ++i) {
      cases += runOutputCase(c, tri.Get(), point_names[i], "tri", kPartitions[partition],
                             tess_oracle::tri(part, point_sets[i]), point_sets[i], true, false);
      cases += runOutputCase(c, quad.Get(), point_names[i], "quad", kPartitions[partition],
                             tess_oracle::quad(part, point_sets[i]), point_sets[i], true, true);
    }
  }
  return cases;
}

/* General tessellated draw for the patch-list, IA-variant, predication, indirect and MSAA cases. */
struct PatchDraw {
  D3D12_PRIMITIVE_TOPOLOGY topology;
  std::vector<float> vertices; // float2 positions
  std::vector<uint16_t> indices;
  UINT count = 0, instances = 1;
  bool indirect = false, predicate = false, predicateValue = false, statistics = true;
  std::array<float, 8> constants = {1, 1, 1, 1, 1, 1, 0, 0};
};

struct PatchResult {
  std::vector<std::pair<uint32_t, uint32_t>> log; // per DS invocation (word0, word1)
  uint32_t invocations = 0;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
  std::array<uint32_t, kSize * kSize> texels{};
};

PatchResult drawPatches(Context &c, ID3D12PipelineState *pso, const PatchDraw &d) {
  PatchResult result;
  auto upload = [&](const void *data, size_t size) {
    auto r = buffer(c.device.Get(), std::max<size_t>(size, 16), D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(r->Map(0, nullptr, &mapped), "map-upload");
    std::memcpy(mapped, data, size);
    r->Unmap(0, nullptr);
    return r;
  };
  auto vb = upload(d.vertices.data(), d.vertices.size() * sizeof(float));
  ComPtr<ID3D12Resource> ib, args, predicate;
  const bool indexed = !d.indices.empty();
  if (indexed)
    ib = upload(d.indices.data(), d.indices.size() * 2);
  ComPtr<ID3D12CommandSignature> signature;
  if (d.indirect) {
    D3D12_DRAW_INDEXED_ARGUMENTS words[2] = {{d.count, d.instances, 0, 0, 0}, {}};
    D3D12_DRAW_ARGUMENTS plain[2] = {{d.count, d.instances, 0, 0}, {}};
    args = indexed ? upload(words, sizeof(words)) : upload(plain, sizeof(plain));
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    check(c.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "signature");
  }
  if (d.predicate) {
    const uint64_t value = d.predicateValue;
    predicate = upload(&value, 8);
  }
  c.begin();
  c.list->CopyBufferRegion(c.log.Get(), 0, c.zero.Get(), 0, kLogBytes);
  D3D12_RESOURCE_BARRIER to_uav = transition(c.log.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.list->ResourceBarrier(1, &to_uav);
  auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
  const float zero[4] = {};
  c.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
  D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
  c.list->RSSetViewports(1, &viewport);
  c.list->RSSetScissorRects(1, &scissor);
  c.list->SetGraphicsRootSignature(c.root.Get());
  c.list->SetPipelineState(pso);
  c.list->SetGraphicsRoot32BitConstants(0, 8, d.constants.data(), 0);
  c.list->SetGraphicsRootUnorderedAccessView(1, c.log->GetGPUVirtualAddress());
  c.list->IASetPrimitiveTopology(d.topology);
  D3D12_VERTEX_BUFFER_VIEW view = {vb->GetGPUVirtualAddress(), UINT(d.vertices.size() * sizeof(float)), 8};
  c.list->IASetVertexBuffers(0, 1, &view);
  if (indexed) {
    D3D12_INDEX_BUFFER_VIEW index = {ib->GetGPUVirtualAddress(), UINT(std::max<size_t>(d.indices.size() * 2, 16)),
                                     DXGI_FORMAT_R16_UINT};
    c.list->IASetIndexBuffer(&index);
  }
  if (d.predicate)
    c.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  if (d.statistics)
    c.list->BeginQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  if (d.indirect)
    c.list->ExecuteIndirect(signature.Get(), 2, args.Get(), 0, nullptr, 0);
  else if (indexed)
    c.list->DrawIndexedInstanced(d.count, d.instances, 0, 0, 0);
  else
    c.list->DrawInstanced(d.count, d.instances, 0, 0);
  if (d.statistics) {
    c.list->EndQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    c.list->ResolveQueryData(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                             c.statisticsReadback.Get(), 0);
  }
  if (d.predicate)
    c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  ID3D12Resource *source = c.target.Get();
  if (c.samples > 1) {
    D3D12_RESOURCE_BARRIER before[2] = {
        transition(c.log.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(c.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE)};
    c.list->ResourceBarrier(2, before);
    c.list->ResolveSubresource(c.resolved.Get(), 0, c.target.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_BARRIER after[2] = {
        transition(c.target.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(c.resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    c.list->ResourceBarrier(2, after);
    source = c.resolved.Get();
  } else {
    D3D12_RESOURCE_BARRIER before[2] = {
        transition(c.log.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(c.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    c.list->ResourceBarrier(2, before);
  }
  c.list->CopyBufferRegion(c.logReadback.Get(), 0, c.log.Get(), 0, kLogBytes);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = c.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch};
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER back[2] = {
      transition(c.log.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
      transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                 c.samples > 1 ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET)};
  c.list->ResourceBarrier(2, back);
  c.submit();
  void *mapped = nullptr;
  D3D12_RANGE range = {0, kLogBytes};
  check(c.logReadback->Map(0, &range, &mapped), "map-log");
  const auto *words = static_cast<const uint32_t *>(mapped);
  result.invocations = words[0];
  for (uint32_t i = 0; i < std::min<uint32_t>(words[0], 16383); ++i)
    result.log.push_back({words[2 + 2 * i], words[3 + 2 * i]});
  D3D12_RANGE none = {};
  c.logReadback->Unmap(0, &none);
  if (d.statistics) {
    range = {0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
    check(c.statisticsReadback->Map(0, &range, &mapped), "map-statistics");
    std::memcpy(&result.statistics, mapped, sizeof(result.statistics));
    c.statisticsReadback->Unmap(0, &none);
  }
  range = {0, kPitch * kSize};
  check(c.readback->Map(0, &range, &mapped), "map-readback");
  for (UINT y = 0; y < kSize; ++y)
    std::memcpy(result.texels.data() + y * kSize, static_cast<uint8_t *>(mapped) + y * kPitch, kSize * 4);
  c.readback->Unmap(0, &none);
  return result;
}

void casePass(const char *name, const char *detail = "") {
  std::printf("%s_CASE_PASS shader=%s case=%s%s\n", kPrefix, useDxil ? "dxil" : "dxbc", name, detail);
}

[[noreturn]] void caseFail(const char *name, const PatchResult &r) {
  const auto &s = r.statistics;
  std::fprintf(stderr, "%s_MISMATCH shader=%s case=%s logged=%u stats=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                       "%llu,%llu\n",
               kPrefix, useDxil ? "dxil" : "dxbc", name, r.invocations, s.IAVertices, s.IAPrimitives, s.VSInvocations,
               s.GSInvocations, s.GSPrimitives, s.CInvocations, s.CPrimitives, s.PSInvocations, s.HSInvocations,
               s.DSInvocations, s.CSInvocations);
  for (size_t i = 0; i < r.log.size() && i < 16; ++i)
    std::fprintf(stderr, "%s_LOG i=%zu a=%u b=%u\n", kPrefix, i, r.log[i].first, r.log[i].second);
  fail(name);
}

/* Each patch lands on its own 2x2 quad; `centres[p]` is the pixel corner patch p's mean sits on. */
std::vector<float> patchVertices(uint32_t points, const std::vector<std::array<int, 2>> &centres) {
  std::vector<float> v;
  for (const auto &centre : centres)
    for (uint32_t k = 0; k < points; ++k) {
      v.push_back(float(centre[0]) / kSize * 2 - 1);
      v.push_back(1 - float(centre[1]) / kSize * 2);
    }
  return v;
}

int checkPatches(const char *name, const PatchResult &r, uint32_t points, uint32_t patches, uint32_t instances,
                 const std::vector<std::array<int, 2>> &drawn, bool expectStatistics = true) {
  // Every patch emits exactly its 4 quad corners (factor 1); the log is (primitive id, control-point count).
  std::map<uint32_t, uint32_t> perPatch;
  bool ok = r.invocations >= 4 * patches * instances;
  for (const auto &[primitive, count] : r.log) {
    ok = ok && count == points && primitive < patches;
    perPatch[primitive]++;
  }
  for (uint32_t p = 0; p < patches && ok; ++p)
    ok = perPatch[p] >= 4 * instances;
  int covered = 0;
  for (uint32_t texel : r.texels)
    covered += texel == 0xff00ff00u;
  ok = ok && covered == int(4 * drawn.size());
  for (const auto &[x, y] : drawn)
    for (int dy = -1; dy <= 0; ++dy)
      for (int dx = -1; dx <= 0; ++dx)
        ok = ok && r.texels[(y + dy) * kSize + x + dx] == 0xff00ff00u;
  const auto &s = r.statistics;
  if (expectStatistics)
    ok = ok && s.IAVertices == uint64_t(points) * patches * instances && s.IAPrimitives == uint64_t(patches) * instances &&
         s.VSInvocations == s.IAVertices && s.HSInvocations == uint64_t(patches) * instances &&
         s.DSInvocations == r.invocations && s.GSInvocations == 0 && s.CSInvocations == 0;
  if (!ok)
    caseFail(name, r);
  std::printf("%s_CASE_PASS shader=%s case=%s control_points=%u patches=%u instances=%u ds=%u covered=%d\n", kPrefix,
              useDxil ? "dxil" : "dxbc", name, points, patches, instances, r.invocations, covered);
  return 1;
}

int runPatchCases(Context &c, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps) {
  int cases = 0;
  auto ds = shader("ds_patch", "ds_5_0");
  const std::vector<std::array<int, 2>> centres = {{4, 4}, {12, 4}, {20, 4}, {28, 4}, {4, 12}, {12, 12}};
  for (uint32_t points : {1u, 2u, 5u, 8u, 11u, 16u, 17u, 32u}) {
    auto hs = shader((std::string("hs_patch") + std::to_string(points)).c_str(), "hs_5_0");
    auto pso = pipeline(c, vs, hs, ds, ps);
    PatchDraw d;
    d.topology = D3D_PRIMITIVE_TOPOLOGY(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1);
    d.vertices = patchVertices(points, centres);
    d.count = points * uint32_t(centres.size()) + (points > 1 ? points - 1 : 0); // trailing partial patch dropped
    d.vertices.resize(size_t(d.count) * 2, 0.0f);
    const std::string name = "patchlist-" + std::to_string(points);
    cases += checkPatches(name.c_str(), drawPatches(c, pso.Get(), d), points, uint32_t(centres.size()), 1, centres);
  }

  // IA variants on 5-point patches: indexed (patches 2, 0), instanced (instance count 2 draws each patch twice:
  // the DS positions do not depend on the instance, so coverage is unchanged), predication, ExecuteIndirect.
  auto hs = shader("hs_patch5", "hs_5_0");
  auto pso = pipeline(c, vs, hs, ds, ps);
  PatchDraw base;
  base.topology = D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST;
  base.vertices = patchVertices(5, {{4, 4}, {12, 4}, {20, 4}});
  base.count = 15;
  PatchDraw indexed = base;
  indexed.indices = {10, 11, 12, 13, 14, 0, 1, 2, 3, 4};
  indexed.count = 10;
  cases += checkPatches("indexed", drawPatches(c, pso.Get(), indexed), 5, 2, 1, {{20, 4}, {4, 4}});
  PatchDraw instanced = base;
  instanced.instances = 2;
  cases += checkPatches("instanced", drawPatches(c, pso.Get(), instanced), 5, 3, 2, {{4, 4}, {12, 4}, {20, 4}});
  PatchDraw predicated = base;
  predicated.predicate = true;
  predicated.predicateValue = true;
  cases += checkPatches("predicate-pass", drawPatches(c, pso.Get(), predicated), 5, 3, 1, {{4, 4}, {12, 4}, {20, 4}});
  predicated.predicateValue = false;
  auto skipped = drawPatches(c, pso.Get(), predicated);
  if (skipped.invocations || skipped.statistics.HSInvocations || skipped.statistics.IAVertices ||
      std::any_of(skipped.texels.begin(), skipped.texels.end(), [](uint32_t t) { return t != 0; }))
    caseFail("predicate-skip", skipped);
  casePass("predicate-skip");
  ++cases;
  PatchDraw indirect = base;
  indirect.indirect = true;
  cases += checkPatches("indirect", drawPatches(c, pso.Get(), indirect), 5, 3, 1, {{4, 4}, {12, 4}, {20, 4}});
  PatchDraw indirectIndexed = indexed;
  indirectIndexed.indirect = true;
  cases += checkPatches("indirect-indexed", drawPatches(c, pso.Get(), indirectIndexed), 5, 2, 1, {{20, 4}, {4, 4}});

  // 4x MSAA: pixel-aligned quads resolve to exactly the 1x image; statistics over an MSAA mesh draw: E_NOTIMPL.
  createTarget(c, 4);
  auto msaa = pipeline(c, vs, hs, ds, ps);
  PatchDraw resolved = base;
  resolved.statistics = false;
  cases += checkPatches("msaa4", drawPatches(c, msaa.Get(), resolved), 5, 3, 1, {{4, 4}, {12, 4}, {20, 4}}, false);
  {
    c.begin();
    c.list->SetGraphicsRootSignature(c.root.Get());
    c.list->SetPipelineState(msaa.Get());
    auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
    c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST);
    c.list->BeginQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    c.list->DrawInstanced(5, 1, 0, 0);
    c.list->EndQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    const HRESULT hr = c.list->Close();
    if (hr != E_NOTIMPL) {
      std::fprintf(stderr, "%s_CLOSE hr=0x%08lx expected E_NOTIMPL\n", kPrefix, static_cast<unsigned long>(hr));
      fail("msaa4-statistics");
    }
    c.list.Reset();
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&c.list)),
          "list-recreate");
    check(c.list->Close(), "list-recreate-close");
    casePass("msaa4-statistics-notimpl");
    ++cases;
  }
  createTarget(c, 1);
  return cases;
}

/*
 * GS after tessellation (VS+HS+DS+GS). Two patches per draw; gs_tsgs emits every DS triangle twice (two strips split by
 * a cut) with TAG = (SV_PrimitiveID, copy). The oracle: the DS domain points equal the tessellator oracle's, the image
 * equals the same HS/DS pipeline without the GS (1x and resolved 4x), the GS runs once per DS triangle (the no-GS
 * draw's CPrimitives) with SV_PrimitiveID = the patch, stream output holds the triangles in D3D order (patch order,
 * each triangle's two copies adjacent) and the copy-0 triangles tile each patch exactly (area sum).
 */
constexpr UINT kSoBytes = 64 * 1024;
constexpr UINT kSoRecordFloats = 6; // SV_Position.xyzw, TAG.xy

struct TsgsContext {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12Resource> gsLog, gsLogReadback, so, soReadback;
};

struct TsgsResult {
  PointSet points;
  uint32_t dsInvocations = 0;
  std::vector<uint32_t> ids; // SV_PrimitiveID per GS invocation
  uint32_t gsInvocations = 0;
  std::vector<float> records;
  uint64_t filled = 0;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS statistics{};
  std::array<uint32_t, kSize * kSize> texels{};
};

void initTsgs(Context &c, TsgsContext &t) {
  // b0: HULL factors, u0: DOMAIN point log, u1: GEOMETRY primitive-id log.
  D3D12_ROOT_PARAMETER parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 8;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_HULL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[2].Descriptor.ShaderRegister = 1;
  parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
  D3D12_ROOT_SIGNATURE_DESC root = {};
  root.NumParameters = 3;
  root.pParameters = parameters;
  root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
               D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
  ComPtr<ID3DBlob> code, errors;
  check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors), "tsgs-root-serialize");
  check(c.device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&t.root)),
        "tsgs-root");
  t.gsLog = buffer(c.device.Get(), kLogBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  t.gsLogReadback = buffer(c.device.Get(), kLogBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  t.so = buffer(c.device.Get(), kSoBytes + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
  t.soReadback = buffer(c.device.Get(), kSoBytes + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
}

HRESULT tsgsPipeline(Context &c, TsgsContext &t, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &hs,
                     const std::vector<uint8_t> &ds, const std::vector<uint8_t> *gs, const std::vector<uint8_t> &ps,
                     bool so, ComPtr<ID3D12PipelineState> &pso) {
  D3D12_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_SO_DECLARATION_ENTRY entries[2] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "TAG", 0, 0, 2, 0}};
  const UINT stride = kSoRecordFloats * 4;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = t.root.Get();
  d.VS = {vs.data(), vs.size()};
  d.HS = {hs.data(), hs.size()};
  d.DS = {ds.data(), ds.size()};
  if (gs)
    d.GS = {gs->data(), gs->size()};
  d.PS = {ps.data(), ps.size()};
  if (so)
    d.StreamOutput = {entries, 2, &stride, 1, 0};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {&element, 1};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = c.samples;
  return c.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
}

/* Two patches side by side (x < 0 and x > 0); tri patches use the first three corners of each square. */
std::vector<float> tsgsVertices(bool quad) {
  std::vector<float> v;
  for (float x0 : {-0.75f, 0.05f}) {
    const float corners[8] = {x0, 0.75f, x0 + 0.7f, 0.75f, x0 + 0.7f, -0.75f, x0, -0.75f};
    v.insert(v.end(), corners, corners + (quad ? 8 : 6));
  }
  return v;
}

TsgsResult drawTsgs(Context &c, TsgsContext &t, ID3D12PipelineState *pso, bool quad,
                    const tess_oracle::Factors &factors, bool so, bool statistics) {
  TsgsResult result;
  const auto vertices = tsgsVertices(quad);
  auto vb = buffer(c.device.Get(), vertices.size() * sizeof(float), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(vb->Map(0, nullptr, &mapped), "tsgs-map-vertices");
  std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(float));
  vb->Unmap(0, nullptr);
  c.begin();
  c.list->CopyBufferRegion(c.log.Get(), 0, c.zero.Get(), 0, kLogBytes);
  c.list->CopyBufferRegion(t.gsLog.Get(), 0, c.zero.Get(), 0, kLogBytes);
  D3D12_RESOURCE_BARRIER to_uav[3] = {
      transition(c.log.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
      transition(t.gsLog.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
      transition(t.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST)};
  c.list->ResourceBarrier(3, to_uav);
  c.list->CopyBufferRegion(t.so.Get(), 0, c.zero.Get(), 0, kSoBytes + 256);
  D3D12_RESOURCE_BARRIER to_so = transition(t.so.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_STREAM_OUT);
  c.list->ResourceBarrier(1, &to_so);
  auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
  const float zero[4] = {};
  c.list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
  D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
  c.list->RSSetViewports(1, &viewport);
  c.list->RSSetScissorRects(1, &scissor);
  c.list->SetGraphicsRootSignature(t.root.Get());
  c.list->SetPipelineState(pso);
  float constants[8] = {factors.edge[0], factors.edge[1], factors.edge[2], factors.edge[3],
                        factors.inside[0], factors.inside[1], 0, 0};
  c.list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
  c.list->SetGraphicsRootUnorderedAccessView(1, c.log->GetGPUVirtualAddress());
  c.list->SetGraphicsRootUnorderedAccessView(2, t.gsLog->GetGPUVirtualAddress());
  c.list->IASetPrimitiveTopology(quad ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                                      : D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
  D3D12_VERTEX_BUFFER_VIEW view = {vb->GetGPUVirtualAddress(), UINT(vertices.size() * sizeof(float)), 8};
  c.list->IASetVertexBuffers(0, 1, &view);
  D3D12_STREAM_OUTPUT_BUFFER_VIEW target = {t.so->GetGPUVirtualAddress(), kSoBytes,
                                            t.so->GetGPUVirtualAddress() + kSoBytes};
  if (so)
    c.list->SOSetTargets(0, 1, &target);
  if (statistics)
    c.list->BeginQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  c.list->DrawInstanced(UINT(vertices.size() / 2), 1, 0, 0);
  if (statistics) {
    c.list->EndQuery(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    c.list->ResolveQueryData(c.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                             c.statisticsReadback.Get(), 0);
  }
  if (so)
    c.list->SOSetTargets(0, 0, nullptr);
  ID3D12Resource *source = c.target.Get();
  D3D12_RESOURCE_BARRIER before[4] = {
      transition(c.log.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(t.gsLog.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(t.so.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(c.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 c.samples > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE)};
  c.list->ResourceBarrier(4, before);
  if (c.samples > 1) {
    c.list->ResolveSubresource(c.resolved.Get(), 0, c.target.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_BARRIER after[2] = {
        transition(c.target.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(c.resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    c.list->ResourceBarrier(2, after);
    source = c.resolved.Get();
  }
  c.list->CopyBufferRegion(c.logReadback.Get(), 0, c.log.Get(), 0, kLogBytes);
  c.list->CopyBufferRegion(t.gsLogReadback.Get(), 0, t.gsLog.Get(), 0, kLogBytes);
  c.list->CopyBufferRegion(t.soReadback.Get(), 0, t.so.Get(), 0, kSoBytes + 256);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = c.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, kPitch};
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER back[4] = {
      transition(c.log.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
      transition(t.gsLog.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
      transition(t.so.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT),
      transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                 c.samples > 1 ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET)};
  c.list->ResourceBarrier(4, back);
  c.submit();

  D3D12_RANGE range = {0, kLogBytes}, none = {};
  check(c.logReadback->Map(0, &range, &mapped), "tsgs-map-log");
  const auto *words = static_cast<const uint32_t *>(mapped);
  result.dsInvocations = words[0];
  for (uint32_t i = 0; i < std::min<uint32_t>(words[0], 16383); ++i) {
    float uv[2];
    std::memcpy(uv, words + 2 + i * 2, sizeof(uv));
    result.points.insert({std::lround(double(uv[0]) * 65536.0), std::lround(double(uv[1]) * 65536.0)});
  }
  c.logReadback->Unmap(0, &none);
  check(t.gsLogReadback->Map(0, &range, &mapped), "tsgs-map-gs-log");
  words = static_cast<const uint32_t *>(mapped);
  result.gsInvocations = words[0];
  result.ids.assign(words + 1, words + 1 + std::min<uint32_t>(words[0], 16383));
  t.gsLogReadback->Unmap(0, &none);
  range = {0, kSoBytes + 256};
  check(t.soReadback->Map(0, &range, &mapped), "tsgs-map-so");
  std::memcpy(&result.filled, static_cast<uint8_t *>(mapped) + kSoBytes, sizeof(result.filled));
  const auto *floats = static_cast<const float *>(mapped);
  result.records.assign(floats, floats + std::min<uint64_t>(result.filled, kSoBytes) / 4);
  t.soReadback->Unmap(0, &none);
  if (statistics) {
    range = {0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
    check(c.statisticsReadback->Map(0, &range, &mapped), "tsgs-map-statistics");
    std::memcpy(&result.statistics, mapped, sizeof(result.statistics));
    c.statisticsReadback->Unmap(0, &none);
  }
  range = {0, kPitch * kSize};
  check(c.readback->Map(0, &range, &mapped), "tsgs-map-readback");
  for (UINT y = 0; y < kSize; ++y)
    std::memcpy(result.texels.data() + y * kSize, static_cast<uint8_t *>(mapped) + y * kPitch, kSize * 4);
  c.readback->Unmap(0, &none);
  return result;
}

int runTsgsCase(Context &c, TsgsContext &t, const char *name, const std::vector<uint8_t> &vs,
                const std::vector<uint8_t> &hs, const std::vector<uint8_t> &ds, const std::vector<uint8_t> &gs,
                const std::vector<uint8_t> &ps, bool quad, tess_oracle::Partitioning partition,
                const tess_oracle::Factors &factors, bool so) {
  const bool statistics = c.samples == 1; // statistics over an MSAA mesh draw: E_NOTIMPL (msaa4-statistics-notimpl)
  ComPtr<ID3D12PipelineState> reference_pso, pso;
  check(tsgsPipeline(c, t, vs, hs, ds, nullptr, ps, false, reference_pso), "tsgs-reference-pso");
  check(tsgsPipeline(c, t, vs, hs, ds, &gs, ps, so, pso), "tsgs-pso");
  const TsgsResult reference = drawTsgs(c, t, reference_pso.Get(), quad, factors, false, statistics);
  const TsgsResult got = drawTsgs(c, t, pso.Get(), quad, factors, so, statistics);
  constexpr uint32_t patches = 2;
  const uint32_t controlPoints = quad ? 4 : 3;
  const auto expected = quad ? tess_oracle::quad(partition, factors) : tess_oracle::tri(partition, factors);
  auto close = [](const std::pair<long, long> &a, const std::pair<long, long> &b) {
    return std::abs(a.first - b.first) <= kTolerance && std::abs(a.second - b.second) <= kTolerance;
  };
  PointSet want;
  for (const auto &p : expected)
    want.insert({std::lround(double(p.u) * 65536.0), std::lround(double(p.v) * 65536.0)});
  bool points = !want.empty();
  for (const auto &p : got.points)
    points = points && std::any_of(want.begin(), want.end(), [&](const auto &q) { return close(p, q); });
  for (const auto &q : want)
    points = points && std::any_of(got.points.begin(), got.points.end(), [&](const auto &p) { return close(p, q); });
  const bool image = got.texels == reference.texels &&
                     std::any_of(got.texels.begin(), got.texels.end(), [](uint32_t t) { return t == 0xff00ff00u; });
  // One GS invocation per DS triangle, SV_PrimitiveID = the patch (half of the invocations each).
  const uint32_t invocations = got.gsInvocations;
  const auto perPatch0 = std::count(got.ids.begin(), got.ids.end(), 0u);
  const auto perPatch1 = std::count(got.ids.begin(), got.ids.end(), 1u);
  bool ids = invocations > 0 && got.ids.size() == invocations && perPatch0 == perPatch1 &&
             size_t(perPatch0 + perPatch1) == got.ids.size();
  bool stats = true;
  if (statistics) {
    const auto &r = reference.statistics, &s = got.statistics;
    ids = ids && invocations == r.CPrimitives;
    stats = s.IAVertices == controlPoints * patches && s.IAPrimitives == patches && s.VSInvocations == s.IAVertices &&
            s.HSInvocations == patches && s.DSInvocations == got.dsInvocations &&
            s.DSInvocations == r.DSInvocations && s.GSInvocations == invocations &&
            s.GSPrimitives == 2ull * invocations && s.CInvocations == 2ull * invocations &&
            s.CPrimitives == 2ull * invocations && s.PSInvocations > 0 && s.CSInvocations == 0;
  }
  bool output = true;
  double area[2] = {};
  if (so) {
    // Records in D3D order: patch-ordered triangles, each followed by its copy (TAG = (primitive id, copy)).
    constexpr uint32_t triangleFloats = 3 * kSoRecordFloats;
    const uint64_t triangles = got.records.size() / triangleFloats;
    output = got.filled == uint64_t(invocations) * 2 * 3 * kSoRecordFloats * 4 && triangles == 2ull * invocations;
    float previous = 0;
    for (uint64_t i = 0; i < triangles && output; ++i) {
      const float *tri = got.records.data() + i * triangleFloats;
      const float id = tri[4], copy = tri[5];
      output = copy == float(i & 1) && id >= previous && (id == 0 || id == 1);
      previous = id;
      for (int k = 0; k < 3 && output; ++k)
        output = tri[k * kSoRecordFloats + 4] == id && tri[k * kSoRecordFloats + 5] == copy &&
                 tri[k * kSoRecordFloats + 3] == 1.0f &&
                 (!(i & 1) || std::memcmp(tri + k * kSoRecordFloats, tri - triangleFloats + k * kSoRecordFloats,
                                          4 * sizeof(float)) == 0);
      if (!(i & 1)) {
        const double ax = tri[kSoRecordFloats] - tri[0], ay = tri[kSoRecordFloats + 1] - tri[1];
        const double bx = tri[2 * kSoRecordFloats] - tri[0], by = tri[2 * kSoRecordFloats + 1] - tri[1];
        area[id == 1] += std::fabs(ax * by - ay * bx) / 2;
      }
    }
    const double patchArea = quad ? 0.7 * 1.5 : 0.7 * 1.5 / 2;
    output = output && std::fabs(area[0] - patchArea) < 1e-4 && std::fabs(area[1] - patchArea) < 1e-4;
  }
  if (!(points && image && ids && stats && output)) {
    const auto &s = got.statistics;
    std::fprintf(stderr,
                 "%s_MISMATCH shader=%s case=%s domain=%s points=%d image=%d ids=%d stats=%d so=%d gs=%u ref_c=%llu "
                 "ids0=%lld ids1=%lld filled=%llu area=%f,%f stats=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                 "%llu,%llu\n",
                 kPrefix, useDxil ? "dxil" : "dxbc", name, quad ? "quad" : "tri", points, image, ids, stats, output,
                 invocations, reference.statistics.CPrimitives, (long long)perPatch0, (long long)perPatch1,
                 (unsigned long long)got.filled, area[0], area[1], s.IAVertices, s.IAPrimitives, s.VSInvocations,
                 s.GSInvocations, s.GSPrimitives, s.CInvocations, s.CPrimitives, s.PSInvocations, s.HSInvocations,
                 s.DSInvocations, s.CSInvocations);
    fail(name);
  }
  std::printf("%s_CASE_PASS shader=%s case=%s domain=%s partition=%s samples=%u gs=%u ds=%u so_vertices=%llu\n",
              kPrefix, useDxil ? "dxil" : "dxbc", name, quad ? "quad" : "tri", kPartitions[partition], c.samples,
              invocations, got.dsInvocations, (unsigned long long)(got.filled / (kSoRecordFloats * 4)));
  return 1;
}

int runTsgsCases(Context &c, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps,
                 const std::vector<uint8_t> &ds_tri, const std::vector<uint8_t> &ds_quad) {
  TsgsContext t;
  initTsgs(c, t);
  int cases = 0;
  auto gs = shader("gs_tsgs", "gs_5_0");
  auto hs_tri = shader("hs_tri_integer", "hs_5_0"), hs_quad = shader("hs_quad_integer", "hs_5_0");
  auto hs_tri_odd = shader("hs_tri_fractional_odd", "hs_5_0");
  auto hs_quad_odd = shader("hs_quad_fractional_odd", "hs_5_0");
  const tess_oracle::Factors integral = {{3, 3, 3, 3}, {3, 3}};
  const tess_oracle::Factors fractional = {{2.5f, 3.25f, 5.75f, 7.5f}, {4.5f, 6.25f}};
  using tess_oracle::Integer;
  using tess_oracle::FractionalOdd;
  cases += runTsgsCase(c, t, "tsgs-integer", vs, hs_tri, ds_tri, gs, ps, false, Integer, integral, false);
  cases += runTsgsCase(c, t, "tsgs-integer", vs, hs_quad, ds_quad, gs, ps, true, Integer, integral, false);
  cases += runTsgsCase(c, t, "tsgs-fractional", vs, hs_tri_odd, ds_tri, gs, ps, false, FractionalOdd, fractional,
                       false);
  cases += runTsgsCase(c, t, "tsgs-fractional", vs, hs_quad_odd, ds_quad, gs, ps, true, FractionalOdd, fractional,
                       false);
  cases += runTsgsCase(c, t, "tsgs-so", vs, hs_tri, ds_tri, gs, ps, false, Integer, integral, true);
  cases += runTsgsCase(c, t, "tsgs-so", vs, hs_quad_odd, ds_quad, gs, ps, true, FractionalOdd, fractional, true);
  createTarget(c, 4);
  cases += runTsgsCase(c, t, "tsgs-msaa4", vs, hs_tri, ds_tri, gs, ps, false, Integer, integral, false);
  cases += runTsgsCase(c, t, "tsgs-msaa4", vs, hs_quad, ds_quad, gs, ps, true, Integer, integral, true);
  createTarget(c, 1);

  // A GS whose input primitive is not the tessellator output (triangle GS after point output): E_INVALIDARG.
  {
    ComPtr<ID3D12PipelineState> invalid;
    const HRESULT hr = tsgsPipeline(c, t, vs, shader("hs_tri_integer_point", "hs_5_0"), ds_tri, &gs, ps, false,
                                    invalid);
    if (hr != E_INVALIDARG) {
      std::fprintf(stderr, "%s_PSO hr=0x%08lx expected E_INVALIDARG\n", kPrefix, static_cast<unsigned long>(hr));
      fail("tsgs-output-mismatch");
    }
    casePass("tsgs-output-mismatch");
    ++cases;
  }
  // ExecuteIndirect: the capture is sized from CPU-known patch counts, so an indirect TS+GS draw is E_NOTIMPL.
  {
    ComPtr<ID3D12PipelineState> pso;
    check(tsgsPipeline(c, t, vs, hs_tri, ds_tri, &gs, ps, false, pso), "tsgs-indirect-pso");
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    ComPtr<ID3D12CommandSignature> signature;
    check(c.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "tsgs-signature");
    const D3D12_DRAW_ARGUMENTS arguments = {6, 1, 0, 0};
    auto args = buffer(c.device.Get(), sizeof(arguments), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(args->Map(0, nullptr, &mapped), "tsgs-map-args");
    std::memcpy(mapped, &arguments, sizeof(arguments));
    args->Unmap(0, nullptr);
    c.begin();
    c.list->SetGraphicsRootSignature(t.root.Get());
    c.list->SetPipelineState(pso.Get());
    auto rtv = c.rtvs->GetCPUDescriptorHandleForHeapStart();
    c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    c.list->ExecuteIndirect(signature.Get(), 1, args.Get(), 0, nullptr, 0);
    const HRESULT hr = c.list->Close();
    if (hr != E_NOTIMPL) {
      std::fprintf(stderr, "%s_CLOSE hr=0x%08lx expected E_NOTIMPL\n", kPrefix, static_cast<unsigned long>(hr));
      fail("tsgs-indirect-notimpl");
    }
    c.list.Reset();
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&c.list)),
          "tsgs-list-recreate");
    check(c.list->Close(), "tsgs-list-recreate-close");
    casePass("tsgs-indirect-notimpl");
    ++cases;
  }
  return cases;
}

int runShaderSet(Context &c) {
  createTarget(c, 1);
  int cases = 0;
  auto vs = shader("vs_pass", "vs_5_0");
  auto ps = shader("ps_color", "ps_5_0");
  auto ds_tri = shader("ds_tri", "ds_5_0");
  auto ds_quad = shader("ds_quad", "ds_5_0");
  const tess_oracle::Factors factor_sets[] = {
      {{1, 1, 1, 1}, {1, 1}},             // minimum
      {{2, 3, 4, 5}, {3, 2}},             // integral, mixed parity
      {{2.5f, 3.25f, 5.75f, 7.5f}, {4.5f, 6.25f}}, // fractional values
      {{64, 64, 64, 64}, {64, 64}},       // max factor
      {{8, 0, 8, 8}, {8, 8}},             // edge factor 0 culls the patch
      {{8, -1, 8, 8}, {8, 8}},            // negative edge factor culls the patch
  };
  const char *names[] = {"minimum", "integral", "fractional", "max64", "cull-zero", "cull-negative"};
  for (int partition = 0; partition < 4; ++partition) {
    auto hs_tri = shader(std::string("hs_tri_") + kPartitions[partition], "hs_5_0");
    auto hs_quad = shader(std::string("hs_quad_") + kPartitions[partition], "hs_5_0");
    auto tri = pipeline(c, vs, hs_tri, ds_tri, ps);
    auto quad = pipeline(c, vs, hs_quad, ds_quad, ps);
    for (size_t i = 0; i < std::size(factor_sets); ++i) {
      cases += runCase(c, tri.Get(), false, tess_oracle::Partitioning(partition), names[i], factor_sets[i]);
      cases += runCase(c, quad.Get(), true, tess_oracle::Partitioning(partition), names[i], factor_sets[i]);
    }
  }
  cases += runOutputMatrix(c, vs, ps, ds_tri, ds_quad);
  cases += runPatchCases(c, vs, ps);
  cases += runTsgsCases(c, vs, ps, ds_tri, ds_quad);
  std::printf("%s_MATRIX_PASS shader=%s cases=%d\n", kPrefix, useDxil ? "dxil" : "dxbc", cases);
  return cases;
}
} // namespace

int main(int argc, char **argv) {
  Context c;
  init(c);
  hlslSource = readFile("tessellation.hlsl");
  useDxil = false;
  runShaderSet(c);
  if (GetFileAttributesA("tessellation-vs_pass.dxil") != INVALID_FILE_ATTRIBUTES) {
    useDxil = true;
    runShaderSet(c);
  } else {
    std::printf("%s_DXIL_SKIPPED reason=no-frozen-dxil\n", kPrefix);
  }
  std::printf("%s_PASS\n", kPrefix);
  return 0;
}
