#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr char marker[] = "OWNED_D3D12_PIPELINE_STATISTICS";
constexpr UINT64 kTimeout = 15000;
constexpr UINT kWidth = 4, kHeight = 4;
using Stats = std::array<UINT64, 11>;
static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) == 88);
constexpr Stats kDraw{3, 1, 3, 0, 0, 1, 1, 16, 0, 0, 0};
constexpr Stats kCompute{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 48};
constexpr Stats kCombined{3, 1, 3, 0, 0, 1, 1, 16, 0, 0, 48};

[[noreturn]] void fail(const char *what, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=%08lx\n", marker, what, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *what) { if (FAILED(hr)) fail(what, hr); }
void require(bool condition, const char *what) { if (!condition) fail(what); }
void expected(HRESULT actual, HRESULT wanted, const char *what) {
  if (actual != wanted) fail(what, actual);
}
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type,
                               D3D12_RESOURCE_STATES state,
                               D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
  d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
  auto h = heap(type); ComPtr<ID3D12Resource> r;
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                         IID_PPV_ARGS(&r)), "buffer");
  return r;
}
ComPtr<ID3D12QueryHeap> queryHeap(ID3D12Device *device, UINT count) {
  D3D12_QUERY_HEAP_DESC d{};
  d.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; d.Count = count; d.NodeMask = 1;
  ComPtr<ID3D12QueryHeap> q;
  check(device->CreateQueryHeap(&d, IID_PPV_ARGS(&q)), "statistics heap");
  return q;
}
ComPtr<ID3DBlob> compile(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> code, errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                          entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                          0, &code, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("D3DCompile", hr);
  }
  return code;
}
std::vector<uint8_t> shaderFile(const char *name) {
  FILE *f = std::fopen(name, "rb");
  if (!f) fail(name);
  require(std::fseek(f, 0, SEEK_END) == 0, "shader seek");
  long bytes = std::ftell(f);
  require(bytes > 0 && std::fseek(f, 0, SEEK_SET) == 0, "shader size");
  std::vector<uint8_t> data(static_cast<size_t>(bytes));
  require(std::fread(data.data(), 1, data.size(), f) == data.size(), "shader read");
  std::fclose(f);
  return data;
}
struct Pipelines {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> graphics, graphicsNoDepthClip, graphicsStripCut, compute, stream, streamNoRaster;
};
Pipelines pipelines(ID3D12Device *device, const void *vs, size_t vsBytes,
                    const void *ps, size_t psBytes, const void *cs, size_t csBytes) {
  std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
  for (UINT i = 0; i < parameters.size(); ++i) {
    parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[i].Descriptor.ShaderRegister = i;
    parameters[i].ShaderVisibility = i == 0 ? D3D12_SHADER_VISIBILITY_VERTEX :
                                      i == 1 ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = static_cast<UINT>(parameters.size());
  desc.pParameters = parameters.data();
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
               D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
  ComPtr<ID3DBlob> blob, errors;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
  if (FAILED(hr)) {
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("serialize root signature", hr);
  }
  Pipelines p;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&p.root)), "root signature");
  D3D12_INPUT_ELEMENT_DESC input{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
  g.pRootSignature = p.root.Get(); g.VS = {vs, vsBytes}; g.PS = {ps, psBytes};
  g.InputLayout = {&input, 1};
  g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  g.SampleMask = UINT_MAX;
  g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  g.RasterizerState.DepthClipEnable = TRUE;
  g.DepthStencilState.DepthEnable = FALSE;
  g.DepthStencilState.StencilEnable = FALSE;
  g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  g.NumRenderTargets = 1; g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  g.SampleDesc.Count = 1;
  check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p.graphics)), "graphics PSO");
  ComPtr<ID3DBlob> graphicsCache;
  check(p.graphics->GetCachedBlob(&graphicsCache), "graphics cached blob");
  g.CachedPSO = {graphicsCache->GetBufferPointer(), graphicsCache->GetBufferSize()};
  p.graphics.Reset();
  check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p.graphics)), "warm graphics PSO");
  g.CachedPSO = {};
  g.RasterizerState.DepthClipEnable = FALSE;
  check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p.graphicsNoDepthClip)),
        "depth clip disabled PSO");
  g.RasterizerState.DepthClipEnable = TRUE;
  g.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
  check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p.graphicsStripCut)),
        "R16 restart PSO");
  g.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  D3D12_SO_DECLARATION_ENTRY position{0, "SV_Position", 0, 0, 4, 0};
  UINT stride = 16;
  g.StreamOutput = {&position, 1, &stride, 1, 0};
  check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p.stream)), "stream + raster PSO");
  g.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
  check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p.streamNoRaster)), "stream only PSO");
  D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
  c.pRootSignature = p.root.Get(); c.CS = {cs, csBytes};
  check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&p.compute)), "compute PSO");
  ComPtr<ID3DBlob> computeCache;
  check(p.compute->GetCachedBlob(&computeCache), "compute cached blob");
  c.CachedPSO = {computeCache->GetBufferPointer(), computeCache->GetBufferSize()};
  p.compute.Reset();
  check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&p.compute)), "warm compute PSO");
  return p;
}
struct Recording {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
};
Recording recording(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
  Recording r;
  check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&r.allocator)), "allocator");
  check(device->CreateCommandList(0, type, r.allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&r.list)), "command list");
  return r;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before; b.Transition.StateAfter = after;
  list->ResourceBarrier(1, &b);
}
struct Fixture {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 fenceValue = 0;
  Pipelines shader;
  ComPtr<ID3D12Resource> color, colorReadback, vertices, entries, entriesZero, entriesReadback;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  HANDLE event = nullptr;
  ~Fixture() { if (event) CloseHandle(event); }
  void submit(Recording &r) {
    check(r.list->Close(), "Close");
    ID3D12CommandList *lists[]{r.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
    if (fence->GetCompletedValue() < fenceValue) {
      check(fence->SetEventOnCompletion(fenceValue, event), "SetEventOnCompletion");
      require(WaitForSingleObject(event, kTimeout) == WAIT_OBJECT_0, "fence timeout");
    }
  }
  void init(const void *vs, size_t vsBytes, const void *ps, size_t psBytes,
            const void *cs, size_t csBytes) {
    HMODULE dll = GetModuleHandleW(L"d3d12.dll");
    require(dll != nullptr, "d3d12 module");
    auto id = reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(dll, "OwnedGraphicsBackendId"));
    require(id && std::strcmp(id(), "owned-graphics-d3d12") == 0, "owned backend");
    std::printf("%s_BACKEND id=owned-graphics-d3d12\n", marker);
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "direct queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event");
    shader = pipelines(device.Get(), vs, vsBytes, ps, psBytes, cs, csBytes);
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = kWidth; texture.Height = kHeight;
    texture.DepthOrArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{}; clear.Format = texture.Format; clear.Color[3] = 1;
    check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &texture,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                          IID_PPV_ARGS(&color)), "color");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
    rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    colorReadback = buffer(device.Get(), 1024, D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    // Two triangles, so indirect commands may consume either three or six vertices.
    const std::array<float, 24> geometry{
      -1, -1, .5f, 1, -1, 3, .5f, 1, 3, -1, .5f, 1,
      -1, -1, .5f, 1, -1, 3, .5f, 1, 3, -1, .5f, 1
    };
    vertices = buffer(device.Get(), sizeof(geometry), D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(vertices->Map(0, nullptr, &mapped), "map vertices");
    std::memcpy(mapped, geometry.data(), sizeof(geometry)); vertices->Unmap(0, nullptr);
    entries = buffer(device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    entriesZero = buffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
    entriesReadback = buffer(device.Get(), 256, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    check(entriesZero->Map(0, nullptr, &mapped), "map zero entries");
    std::memset(mapped, 0, 256); entriesZero->Unmap(0, nullptr);
  }
  void prepare(ID3D12GraphicsCommandList *list) {
    list->CopyBufferRegion(entries.Get(), 0, entriesZero.Get(), 0, 256);
    transition(list, entries.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  void bindGraphics(ID3D12GraphicsCommandList *list, bool raster = true) {
    list->SetGraphicsRootSignature(shader.root.Get());
    list->SetPipelineState(shader.graphics.Get());
    list->SetGraphicsRootUnorderedAccessView(0, entries->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(1, entries->GetGPUVirtualAddress() + 16);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(), 6 * 4 * sizeof(float), 4 * sizeof(float)};
    list->IASetVertexBuffers(0, 1, &vb);
    D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    D3D12_RECT scissor{0, 0, raster ? LONG(kWidth) : 0, raster ? LONG(kHeight) : 0};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  }
  void draw(ID3D12GraphicsCommandList *list, UINT verticesCount = 3, bool raster = true) {
    bindGraphics(list, raster); list->DrawInstanced(verticesCount, 1, 0, 0);
  }
  void dispatch(ID3D12GraphicsCommandList *list) {
    list->SetComputeRootSignature(shader.root.Get());
    list->SetPipelineState(shader.compute.Get());
    list->SetComputeRootUnorderedAccessView(2, entries->GetGPUVirtualAddress() + 32);
    list->Dispatch(2, 3, 1);
  }
  void finish(ID3D12GraphicsCommandList *list) {
    transition(list, entries.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(entriesReadback.Get(), 0, entries.Get(), 0, 256);
    transition(list, entries.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
  }
  void captureColor(ID3D12GraphicsCommandList *list) {
    transition(list, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = color.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = colorReadback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kWidth;
    dst.PlacedFootprint.Footprint.Height = kHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list, color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  void verifyColor() {
    D3D12_RANGE range{0, 1024}; void *mapped = nullptr;
    check(colorReadback->Map(0, &range, &mapped), "map rasterized image");
    const auto *bytes = static_cast<const uint8_t *>(mapped);
    for (UINT y = 0; y < kHeight; ++y)
      for (UINT x = 0; x < kWidth; ++x) {
        const auto *pixel = bytes + 256 * y + 4 * x;
        if (pixel[0] != 64 || pixel[1] != 128 || pixel[2] != 191 || pixel[3] != 255)
          fail("4x4 image disagrees with full-coverage PS statistic");
      }
    D3D12_RANGE written{0, 0}; colorReadback->Unmap(0, &written);
  }
  std::array<UINT, 3> oracle() {
    D3D12_RANGE range{0, 48}; void *mapped = nullptr;
    check(entriesReadback->Map(0, &range, &mapped), "map entry oracle");
    auto words = static_cast<const UINT *>(mapped);
    std::array<UINT, 3> values{words[0], words[4], words[8]};
    D3D12_RANGE written{0, 0}; entriesReadback->Unmap(0, &written);
    return values;
  }
};
void compare(const char *name, const Stats &actual, const Stats &wanted,
             const std::array<UINT, 3> &oracle) {
  for (UINT i = 0; i < actual.size(); ++i) {
    if (actual[i] != wanted[i]) {
      std::fprintf(stderr, "%s_VECTOR_FAIL case=%s field=%u actual=%llu expected=%llu oracle=%u,%u,%u vector=",
                   marker, name, i, static_cast<unsigned long long>(actual[i]),
                   static_cast<unsigned long long>(wanted[i]), oracle[0], oracle[1], oracle[2]);
      for (auto value : actual) std::fprintf(stderr, "%llu,", static_cast<unsigned long long>(value));
      std::fputc('\n', stderr);
      fail(name);
    }
  }
  require(actual[2] == oracle[0] && actual[7] == oracle[1] && actual[10] == oracle[2],
          "independent shader entry counters disagree");
  std::printf("%s_GPU_CASE_PASS case=%s ia=%llu,%llu vs=%llu clip=%llu,%llu ps=%llu cs=%llu oracle=%u,%u,%u\n",
              marker, name, static_cast<unsigned long long>(actual[0]),
              static_cast<unsigned long long>(actual[1]), static_cast<unsigned long long>(actual[2]),
              static_cast<unsigned long long>(actual[5]), static_cast<unsigned long long>(actual[6]),
              static_cast<unsigned long long>(actual[7]), static_cast<unsigned long long>(actual[10]),
              oracle[0], oracle[1], oracle[2]);
}
Stats readStats(ID3D12Resource *readback, UINT64 offset) {
  D3D12_RANGE range{SIZE_T(offset), SIZE_T(offset + 88)}; void *mapped = nullptr;
  check(readback->Map(0, &range, &mapped), "map stats");
  Stats result{}; std::memcpy(result.data(), static_cast<uint8_t *>(mapped) + offset, 88);
  D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
  return result;
}
void guard(ID3D12Resource *readback, UINT64 offset, UINT count) {
  D3D12_RANGE range{0, 512}; void *mapped = nullptr;
  check(readback->Map(0, &range, &mapped), "map guard");
  const uint8_t *bytes = static_cast<const uint8_t *>(mapped);
  for (UINT i = 0; i < 512; ++i) {
    if ((i < offset || i >= offset + count * 88) && bytes[i] != 0xA5)
      fail("query resolve guard changed");
  }
  D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
}
void validation(Fixture &f) {
  auto q = queryHeap(f.device.Get(), 2);
  auto out = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto invalid = [&](const char *name, auto record) {
    auto r = recording(f.device.Get()); record(r.list.Get());
    expected(r.list->Close(), E_INVALIDARG, name);
    std::printf("%s_VALIDATION_PASS case=%s\n", marker, name);
  };
  invalid("end_without_begin", [&](auto *l) { l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0); });
  invalid("duplicate_begin", [&](auto *l) {
    l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  });
  invalid("unclosed", [&](auto *l) { l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0); });
  invalid("active_resolve", [&](auto *l) {
    l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 0);
    l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  });
  invalid("wrong_type", [&](auto *l) {
    l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  });
  invalid("out_of_range", [&](auto *l) {
    l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 2);
  });
  invalid("misaligned_resolve", [&](auto *l) {
    l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 4);
  });
  invalid("past_destination", [&](auto *l) {
    l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, out.Get(), 88);
  });
  invalid("past_heap", [&](auto *l) {
    l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1, 2, out.Get(), 0);
  });
  invalid("wrong_resolve_type", [&](auto *l) {
    l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, out.Get(), 0);
  });
  invalid("end_out_of_range", [&](auto *l) {
    l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 2);
  });
  {
    ComPtr<ID3D12Device> other;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other)),
          "cross-device query validation device");
    require(other.Get() != f.device.Get(), "cross-device query distinct object");
    auto foreign = queryHeap(other.Get(), 1);
    auto foreignOut = buffer(other.Get(), 128, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    invalid("foreign_heap", [&](auto *l) {
      l->BeginQuery(foreign.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    });
    invalid("foreign_destination", [&](auto *l) {
      l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                          0, 1, foreignOut.Get(), 0);
    });
  }
  {
    auto r = recording(f.device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
    r.list->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    expected(r.list->Close(), E_INVALIDARG, "compute list statistics begin");
    check(r.allocator->Reset(), "failed-close allocator reusable");
    expected(r.list->Reset(r.allocator.Get(), nullptr), E_INVALIDARG,
             "failed query list stays poisoned");
  }
  {
    auto r = recording(f.device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
    r.list->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 0);
    expected(r.list->Close(), E_INVALIDARG, "compute list statistics resolve");
  }
  {
    auto r = recording(f.device.Get());
    r.list->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 2, 0, out.Get(), 256);
    check(r.list->Close(), "zero count boundary resolve");
  }
  {
    auto r = recording(f.device.Get()); check(r.list->Close(), "closed guard close");
    r.list->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    r.list->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    check(r.list->Reset(r.allocator.Get(), nullptr), "closed guard reset");
    check(r.list->Close(), "closed guard final close");
  }
}
struct StageVertex { float position[4], distances[4]; };
using StageTriangle = std::array<StageVertex, 3>;
struct StageOptions {
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE;
  bool depth = false, multisample = false, noTarget = false, unchangedColor = false;
  bool depthClip = true;
  UINT minInvocation = 1, maxInvocation = 1;
  UINT minClip = 1, maxClip = 1, minPixel = 0, maxPixel = 0;
  UINT sampleIndexCount = 0;
};
void stageCase(Fixture &f, const char *name, D3D12_SHADER_BYTECODE vs,
               D3D12_SHADER_BYTECODE ps, const StageVertex *vertices, UINT count,
               StageOptions options) {
  D3D12_INPUT_ELEMENT_DESC input[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = f.shader.root.Get(); desc.VS = vs; desc.PS = ps;
  desc.InputLayout = {input, 2};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = options.cull;
  desc.RasterizerState.DepthClipEnable = options.depthClip;
  desc.DepthStencilState.DepthEnable = options.depth;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  desc.PrimitiveTopologyType = options.topologyType;
  desc.NumRenderTargets = options.noTarget ? 0 : 1;
  if (!options.noTarget) desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.DSVFormat = options.depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = options.multisample ? 4 : 1;
  ComPtr<ID3D12PipelineState> pipeline;
  check(f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), name);
  auto vb = buffer(f.device.Get(), sizeof(StageVertex) * count, D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(vb->Map(0, nullptr, &mapped), "stage vertices map");
  std::memcpy(mapped, vertices, sizeof(StageVertex) * count); vb->Unmap(0, nullptr);
  ComPtr<ID3D12Resource> depth, msColor;
  ComPtr<ID3D12DescriptorHeap> stageRtv, stageDsv;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = f.rtv, dsv{};
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = kWidth; texture.Height = kHeight;
  texture.DepthOrArraySize = texture.MipLevels = 1;
  texture.SampleDesc.Count = desc.SampleDesc.Count;
  if (options.multisample) {
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &texture,
          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&msColor)),
          "multisample target");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&stageRtv)), "multisample RTV heap");
    rtv = stageRtv->GetCPUDescriptorHandleForHeapStart();
    f.device->CreateRenderTargetView(msColor.Get(), nullptr, rtv);
  }
  if (options.depth) {
    texture.Format = DXGI_FORMAT_D32_FLOAT;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{}; clear.Format = texture.Format; clear.DepthStencil.Depth = .25f;
    check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &texture,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)), "stage depth");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&stageDsv)), "stage DSV heap");
    dsv = stageDsv->GetCPUDescriptorHandleForHeapStart();
    f.device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  }
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  if (!options.noTarget) {
    const float clear[]{0, 0, 0, 1};
    l->ClearRenderTargetView(rtv, clear, 0, nullptr);
  }
  if (options.depth) l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, .25f, 0, 0, nullptr);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->SetGraphicsRootSignature(f.shader.root.Get());
  l->SetPipelineState(pipeline.Get());
  l->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
  l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
  l->IASetPrimitiveTopology(options.topology);
  D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), UINT(sizeof(StageVertex) * count),
                                sizeof(StageVertex)};
  l->IASetVertexBuffers(0, 1, &view);
  D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(kWidth), LONG(kHeight)};
  l->RSSetViewports(1, &viewport); l->RSSetScissorRects(1, &scissor);
  l->OMSetRenderTargets(options.noTarget ? 0 : 1, options.noTarget ? nullptr : &rtv,
                        FALSE, options.depth ? &dsv : nullptr);
  l->DrawInstanced(count, 1, 0, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  if (options.unchangedColor) f.captureColor(l);
  f.finish(l); f.submit(r);
  const Stats actual = readStats(out.Get(), 24);
  auto oracle = f.oracle();
  if (options.sampleIndexCount) {
    D3D12_RANGE range{16, 32};
    check(f.entriesReadback->Map(0, &range, &mapped), "sample oracle map");
    const auto *words = static_cast<const UINT *>(mapped);
    oracle[1] = 0;
    for (UINT sample = 0; sample < 4; ++sample) {
      require(words[4 + sample] == options.sampleIndexCount, "SV_SampleIndex coverage");
      oracle[1] += words[4 + sample];
    }
    D3D12_RANGE written{0, 0}; f.entriesReadback->Unmap(0, &written);
  }
  const bool expectedStats = actual[0] == count && actual[1] == 1 && actual[2] == oracle[0] &&
          oracle[0] == count && actual[3] == 0 && actual[4] == 0 &&
          actual[5] >= options.minInvocation && actual[5] <= options.maxInvocation &&
          actual[6] >= options.minClip &&
          actual[6] <= options.maxClip && actual[7] == oracle[1] &&
          actual[7] >= options.minPixel && actual[7] <= options.maxPixel &&
          actual[8] == 0 && actual[9] == 0 && actual[10] == 0 && oracle[2] == 0;
  if (!expectedStats) {
    std::fprintf(stderr, "%s_STAGE_FAIL case=%s vector=", marker, name);
    for (auto value : actual) std::fprintf(stderr, "%llu,", static_cast<unsigned long long>(value));
    std::fprintf(stderr, " oracle=%u,%u,%u expected clip-inv=%u..%u clip-prim=%u..%u ps=%u..%u\n",
                 oracle[0], oracle[1], oracle[2], options.minInvocation,
                 options.maxInvocation, options.minClip, options.maxClip,
                 options.minPixel, options.maxPixel);
    fail(name);
  }
  if (options.unchangedColor) {
    D3D12_RANGE range{0, 1024};
    check(f.colorReadback->Map(0, &range, &mapped), "stage unchanged color map");
    const auto *image = static_cast<const uint8_t *>(mapped);
    for (UINT y = 0; y < kHeight; ++y)
      for (UINT x = 0; x < kWidth; ++x)
        require(image[y * 256 + x * 4] == 0 && image[y * 256 + x * 4 + 3] == 255,
                "discard/depth rejected color writes");
    D3D12_RANGE written{0, 0}; f.colorReadback->Unmap(0, &written);
  }
  std::printf("%s_GPU_CASE_PASS case=%s ia=%llu,%llu vs=%llu clip=%llu,%llu ps=%llu oracle=%u,%u,0\n",
              marker, name, static_cast<unsigned long long>(actual[0]),
              static_cast<unsigned long long>(actual[1]), static_cast<unsigned long long>(actual[2]),
              static_cast<unsigned long long>(actual[5]), static_cast<unsigned long long>(actual[6]),
              static_cast<unsigned long long>(actual[7]), oracle[0], oracle[1]);
}
struct MSConfig {
  const char *name;
  UINT samples, mask, shader;
  bool edge = false, clipped = false, overlap = false, scissor = false;
  bool blend = false;
  // 1: forced early partial depth, 2: late fully occluded,
  // 3: SV_Depth pass, 4: SV_Depth reject, 5: forced early full reject,
  // 6: partial depth with stencil increment, 7: partial stencil rejection,
  // 8: forced early checkerboard depth leaving rejected quad neighbors.
  UINT depth = 0;
  // 1 indexed+instanced, 2/3 indirect count one/zero,
  // 4/5 predicate true/false, 6 bundle under parent query,
  // 7 ordinary -> MSAA -> ordinary in one recording,
  // 8 two primitives per instance with PrimitiveID reset for the next instance.
  UINT drawMode = 0;
  bool alphaToCoverage = false;
  bool viewportOffset = false;
};

// The shader reads every MSAA sample independently; resolving to one color
// would hide duplicated pixel-shader invocations and lost partial coverage.
struct MSReader {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pipeline;
};
MSReader makeMSReader(Fixture &f) {
  static constexpr char readSource[] = R"(
Texture2DMS<float4> image : register(t0);
Texture2DMS<float> depthImage : register(t1);
Texture2DMS<uint4> stencilImage : register(t2);
RWStructuredBuffer<float4> samples : register(u0);
cbuffer Count : register(b0) { uint sampleCount; uint readDepth; uint readStencil; }
[numthreads(4,4,1)] void main(uint3 id : SV_DispatchThreadID) {
  for (uint sample = 0; sample < sampleCount; ++sample) {
    samples[(id.y * 4 + id.x) * 4 + sample] = image.Load(int2(id.xy), sample);
    if (readDepth)
      samples[64 + (id.y * 4 + id.x) * 4 + sample] =
          depthImage.Load(int2(id.xy), sample).xxxx;
    if (readStencil)
      samples[128 + (id.y * 4 + id.x) * 4 + sample] =
          float4(stencilImage.Load(int2(id.xy), sample).y, 0, 0, 0);
  }
}
)";
  auto readShader = compile(readSource, "main", "cs_5_0");
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 3;
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].Descriptor.ShaderRegister = 0;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants.ShaderRegister = 0;
  params[2].Constants.Num32BitValues = 3;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{3, params};
  ComPtr<ID3DBlob> blob;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &blob, nullptr), "MSAA reader root");
  MSReader result;
  check(f.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&result.root)), "MSAA reader root signature");
  D3D12_COMPUTE_PIPELINE_STATE_DESC readDesc{};
  readDesc.pRootSignature = result.root.Get();
  readDesc.CS = {readShader->GetBufferPointer(), readShader->GetBufferSize()};
  check(f.device->CreateComputePipelineState(&readDesc, IID_PPV_ARGS(&result.pipeline)),
        "MSAA sample reader");
  return result;
}
void msaaBehavior(Fixture &f, const MSReader &reader,
                  const std::array<D3D12_SHADER_BYTECODE, 30> &code, MSConfig config) {
  const bool dxilPositions = code[14].pShaderBytecode != nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = 3;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(f.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap)), "MSAA reader descriptors");

  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = texture.Height = 4;
  texture.DepthOrArraySize = texture.MipLevels = 1;
  texture.SampleDesc.Count = config.samples;
  // Pull-model evaluations can legitimately be negative. A float target
  // preserves those values instead of making signed-offset bugs look like 0.
  texture.Format = config.shader == 13 || config.shader == 14 || config.shader == 17 ||
                   config.shader == 20 || config.shader == 21 ||
                   config.shader == 28 ?
                   DXGI_FORMAT_R32G32B32A32_FLOAT :
                   DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> color;
  check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)), "MSAA color");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = texture.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  f.device->CreateShaderResourceView(color.Get(), &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());
  ComPtr<ID3D12Resource> depth;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  const bool stencil = config.depth >= 6;
  if (config.depth) {
    auto depthDesc = texture;
    depthDesc.Format = stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = config.depth == 3 || config.depth == 7 ? 1 :
                               config.depth == 4 ? .5f : .25f;
    check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &depthDesc,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)), "MSAA depth");
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    check(f.device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap)), "MSAA DSV heap");
    dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = clear.Format;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    f.device->CreateDepthStencilView(depth.Get(), &view, dsv);
  }
  auto secondSrv = srvHeap->GetCPUDescriptorHandleForHeapStart();
  secondSrv.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.Format = stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS :
               config.depth ? DXGI_FORMAT_R32_FLOAT : texture.Format;
  f.device->CreateShaderResourceView(config.depth ? depth.Get() : color.Get(), &srv, secondSrv);
  auto thirdSrv = secondSrv;
  thirdSrv.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.Format = stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : texture.Format;
  f.device->CreateShaderResourceView(stencil ? depth.Get() : color.Get(), &srv, thirdSrv);
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(f.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "MSAA RTV descriptors");
  auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  f.device->CreateRenderTargetView(color.Get(), nullptr, rtv);
  auto samples = buffer(f.device.Get(), 3072, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto sampled = buffer(f.device.Get(), 3072, D3D12_HEAP_TYPE_READBACK,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_INPUT_ELEMENT_DESC input[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = f.shader.root.Get();
  pso.InputLayout = {input, 2};
  pso.VS = code[6];
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = texture.Format;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.SampleDesc.Count = config.samples;
  pso.SampleMask = config.mask;
  pso.PS = code[7];
  ComPtr<ID3D12PipelineState> samplePSO, pixelPSO;
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&samplePSO)), "MSAA coverage PSO");
  pso.PS = code[config.shader];
  pso.BlendState.AlphaToCoverageEnable = config.alphaToCoverage;
  if (config.depth) {
    pso.DSVFormat = stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    if (stencil) {
      auto &ds = pso.DepthStencilState;
      ds.StencilEnable = TRUE;
      ds.StencilReadMask = ds.StencilWriteMask = 0xff;
      ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
      ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP_INCR_SAT;
      ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
      ds.BackFace = ds.FrontFace;
    }
  }
  if (config.blend) {
    auto &blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pixelPSO)), "MSAA pixel PSO");
  ComPtr<ID3D12PipelineState> prepass;
  if (config.depth == 1 || config.depth == 8 || stencil) {
    pso.PS = config.depth == 8 ? code[29] : code[15];
    pso.NumRenderTargets = 0;
    pso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    if (config.depth == 7) pso.SampleMask = 0xau;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (stencil) {
      auto &ds = pso.DepthStencilState;
      ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
      ds.BackFace = ds.FrontFace;
    }
    check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&prepass)),
          "MSAA sample-depth prepass PSO");
  }
  StageTriangle triangle{{{{-1,1,.5f,1}, {0,0,1,1}},
                          {{.2f,1,.5f,1}, {1,0,1,1}},
                          {{.2f,-1,.5f,1}, {1,1,1,1}}}};
  if (!config.edge) {
    triangle = {{{{-1,-1,.5f,1}, {0,1,1,1}},
                 {{-1,3,.5f,1}, {0,-1,1,1}},
                 {{3,-1,.5f,1}, {2,1,1,1}}}};
  }
  if (config.clipped) triangle[0].position[2] = -.5f;
  if (config.shader == 20) {
    triangle[0].position[2] = .1f;
    triangle[1].position[2] = .9f;
    triangle[2].position[2] = .9f;
  }
  if (config.depth == 4)
    for (auto &vertex : triangle) vertex.position[2] = .1f;
  // Non-unit W exercises perspective interpolation without changing NDC.
  // The Position test uses a steeper reciprocal-W slope so center-vs-sample
  // mistakes remain visible independently of color quantization.
  for (UINT coordinate = 0; coordinate < 4; ++coordinate)
    triangle[2].position[coordinate] *= config.shader == 20 ? 4.f : 1.5f;
  const StageTriangle pair[2]{triangle, triangle};
  const UINT geometryBytes = config.drawMode == 8 ? sizeof(pair) : sizeof(triangle);
  auto vb = buffer(f.device.Get(), geometryBytes, D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(vb->Map(0, nullptr, &mapped), "MSAA geometry map");
  std::memcpy(mapped, pair, geometryBytes);
  vb->Unmap(0, nullptr);
  ComPtr<ID3D12Resource> indices, arguments, argumentUpload, predicate, predicateUpload;
  ComPtr<ID3D12CommandSignature> signature;
  Recording bundle;
  if (config.drawMode == 1) {
    const UINT16 values[]{0, 1, 2};
    indices = buffer(f.device.Get(), sizeof(values), D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
    check(indices->Map(0, nullptr, &mapped), "MSAA indices map");
    std::memcpy(mapped, values, sizeof(values)); indices->Unmap(0, nullptr);
  }
  if (config.drawMode == 2 || config.drawMode == 3) {
    argumentUpload = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
    arguments = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    check(argumentUpload->Map(0, nullptr, &mapped), "MSAA indirect map");
    std::memset(mapped, 0, 128);
    const D3D12_DRAW_ARGUMENTS draw{3, 1, 0, 0};
    std::memcpy(mapped, &draw, sizeof(draw));
    *reinterpret_cast<UINT *>(static_cast<uint8_t *>(mapped) + 64) =
        config.drawMode == 2 ? 1 : 0;
    argumentUpload->Unmap(0, nullptr);
    D3D12_INDIRECT_ARGUMENT_DESC command{};
    command.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &command;
    check(f.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)),
          "MSAA draw signature");
  }
  if (config.drawMode == 4 || config.drawMode == 5) {
    predicateUpload = buffer(f.device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
    predicate = buffer(f.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    check(predicateUpload->Map(0, nullptr, &mapped), "MSAA predicate map");
    *static_cast<UINT64 *>(mapped) = config.drawMode == 4 ? 1 : 0;
    predicateUpload->Unmap(0, nullptr);
  }
  if (config.drawMode == 6) {
    bundle = recording(f.device.Get(), D3D12_COMMAND_LIST_TYPE_BUNDLE);
    auto *b = bundle.list.Get();
    b->SetGraphicsRootSignature(f.shader.root.Get());
    b->SetPipelineState(pixelPSO.Get());
    b->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
    b->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
    b->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), sizeof(triangle), sizeof(StageVertex)};
    b->IASetVertexBuffers(0, 1, &view);
    b->DrawInstanced(3, 1, 0, 0);
    check(b->Close(), "MSAA bundle close");
  }
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12QueryHeap> occlusion;
  ComPtr<ID3D12Resource> occlusionReadback;
  if (config.depth) {
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    desc.Count = desc.NodeMask = 1;
    check(f.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&occlusion)), "MSAA occlusion heap");
    occlusionReadback = buffer(f.device.Get(), 8, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  }
  using SampleColors = std::array<float, 768>;
  auto drawAndRead = [&](bool reference, SampleColors &colors,
                         std::array<UINT, 16> &counters) {
    auto r = recording(f.device.Get()); auto *l = r.list.Get();
    f.prepare(l);
    if (!reference && arguments) {
      l->CopyBufferRegion(arguments.Get(), 0, argumentUpload.Get(), 0, 128);
      transition(l, arguments.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    if (!reference && predicate) {
      l->CopyBufferRegion(predicate.Get(), 0, predicateUpload.Get(), 0, 8);
      transition(l, predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_PREDICATION);
    }
    const float clear[]{0, 0, 0, 1};
    l->ClearRenderTargetView(rtv, clear, 0, nullptr);
    if (!reference && config.depth)
      l->ClearDepthStencilView(dsv, stencil ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL :
                               D3D12_CLEAR_FLAG_DEPTH,
                               config.depth == 3 || config.depth == 7 ? 1 :
                               config.depth == 4 ? .5f : .25f, 0, 0, nullptr);
    l->SetGraphicsRootSignature(f.shader.root.Get());
    l->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
    l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), geometryBytes, sizeof(StageVertex)};
    l->IASetVertexBuffers(0, 1, &view);
    D3D12_VIEWPORT viewport{config.viewportOffset ? 1.f : 0.f,
                            config.viewportOffset ? 1.f : 0.f,
                            config.viewportOffset ? 2.f : 4.f,
                            config.viewportOffset ? 2.f : 4.f, 0, 1};
    D3D12_RECT scissor{0, 0, config.scissor ? 2 : 4, 4};
    l->RSSetViewports(1, &viewport); l->RSSetScissorRects(1, &scissor);
    if (!reference && prepass) {
      l->SetPipelineState(prepass.Get());
      l->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
      if (stencil) l->OMSetStencilRef(1);
      l->DrawInstanced(3, 1, 0, 0);
      transition(l, f.entries.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COPY_DEST);
      f.prepare(l);
    }
    if (!reference) {
      l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      if (occlusion) l->BeginQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    }
    if (!reference && config.drawMode == 7) {
      l->ClearRenderTargetView(f.rtv, clear, 0, nullptr);
      f.bindGraphics(l);
      l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 192);
      l->DrawInstanced(3, 1, 0, 0);
      l->SetGraphicsRootSignature(f.shader.root.Get());
      l->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
      l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
      l->IASetVertexBuffers(0, 1, &view);
      l->RSSetViewports(1, &viewport); l->RSSetScissorRects(1, &scissor);
    }
    l->SetPipelineState(reference ? samplePSO.Get() : pixelPSO.Get());
    if (stencil) l->OMSetStencilRef(1);
    l->OMSetRenderTargets(1, &rtv, FALSE, !reference && config.depth ? &dsv : nullptr);
    if (!reference && indices) {
      D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(), 3 * sizeof(UINT16),
                                 DXGI_FORMAT_R16_UINT};
      l->IASetIndexBuffer(&ib);
      l->DrawIndexedInstanced(3, 2, 0, 0, 0);
    } else if (!reference && arguments) {
      l->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, arguments.Get(), 64);
    } else if (!reference && predicate) {
      l->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      l->DrawInstanced(3, 1, 0, 0);
      l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    } else if (!reference && bundle.list) {
      l->ExecuteBundle(bundle.list.Get());
    } else if (!reference && config.drawMode == 8) {
      l->DrawInstanced(6, 2, 0, 0);
    } else l->DrawInstanced(3, 1, 0, 0);
    if (!reference && config.overlap) l->DrawInstanced(3, 1, 0, 0);
    if (!reference && config.drawMode == 7) {
      f.bindGraphics(l);
      l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 192);
      l->DrawInstanced(3, 1, 0, 0);
      f.captureColor(l);
    }
    if (!reference) {
      if (occlusion) {
        l->EndQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        l->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1,
                            occlusionReadback.Get(), 0);
      }
      l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 0);
    }
    transition(l, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!reference && config.depth)
      transition(l, depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l->SetComputeRootSignature(reader.root.Get());
    l->SetPipelineState(reader.pipeline.Get());
    ID3D12DescriptorHeap *heaps[]{srvHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    l->SetComputeRootUnorderedAccessView(1, samples->GetGPUVirtualAddress());
    l->SetComputeRoot32BitConstant(2, config.samples, 0);
    l->SetComputeRoot32BitConstant(2, !reference && config.depth, 1);
    l->SetComputeRoot32BitConstant(2, !reference && stencil, 2);
    l->Dispatch(1, 1, 1);
    transition(l, samples.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyBufferRegion(sampled.Get(), 0, samples.Get(), 0, 3072);
    transition(l, samples.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(l, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!reference && config.depth)
      transition(l, depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
    f.finish(l); f.submit(r);
    if (!reference && config.drawMode == 7) f.verifyColor();
    D3D12_RANGE sampleRange{0, 3072};
    check(sampled->Map(0, &sampleRange, &mapped), "MSAA samples map");
    std::memcpy(colors.data(), mapped, 3072);
    D3D12_RANGE empty{0, 0}; sampled->Unmap(0, &empty);
    D3D12_RANGE counterRange{16, 80};
    check(f.entriesReadback->Map(0, &counterRange, &mapped), "MSAA counters map");
    const auto *words = static_cast<const UINT *>(mapped);
    for (UINT i = 0; i < 16; ++i) counters[i] = words[4 + i];
    f.entriesReadback->Unmap(0, &empty);
  };
  SampleColors referenceColors{}, colors{};
  std::array<UINT, 16> reference{}, counters{};
  drawAndRead(true, referenceColors, reference);
  std::array<UINT, 4> sampleBuckets{};
  D3D12_RANGE bucketRange{144, 160};
  check(f.entriesReadback->Map(0, &bucketRange, &mapped), "sample-frequency buckets map");
  std::memcpy(sampleBuckets.data(), static_cast<const uint8_t *>(mapped) + 144, 16);
  D3D12_RANGE empty{0, 0}; f.entriesReadback->Unmap(0, &empty);
  drawAndRead(false, colors, counters);
  const Stats stats = readStats(out.Get(), 0);
  std::array<UINT, 4> expectedBuckets{};
  UINT pixels = 0, coveredSamples = 0, partialPixels = 0, invocations = 0;
  UINT passingSamples = 0;
  for (UINT pixel = 0; pixel < 16; ++pixel) {
    UINT mask = reference[pixel];
    require((mask & ~config.mask) == 0 &&
            (mask & ~((1u << config.samples) - 1)) == 0,
            "raster coverage escaped the PSO sample mask");
    if (mask) ++pixels;
    coveredSamples += __builtin_popcount(mask);
    if (mask && mask != (1u << config.samples) - 1) ++partialPixels;
    const bool early = config.depth == 1 || config.depth == 5 ||
                       config.depth == 8 || stencil;
    const bool skipped = config.drawMode == 3 || config.drawMode == 5;
    const bool checkerboardPass = ((pixel / 4 + pixel % 4) & 1) == 0;
    const bool invoked = mask && !skipped && (!early ||
                          (config.depth != 5 && (config.depth == 8 ?
                           checkerboardPass : (mask & 0xau))));
    const UINT expectedCount = invoked ? (config.drawMode == 8 ? 4 :
                                         config.overlap || config.drawMode == 1 ? 2 : 1) : 0;
    invocations += expectedCount;
    const UINT expectedUav = config.drawMode == 8 ? (invoked ? 0x202u : 0u) : expectedCount;
    if (counters[pixel] != expectedUav) fail(config.name);
    for (UINT sample = 0; sample < config.samples; ++sample) {
      const UINT at = (pixel * 4 + sample) * 4;
      const bool covered = (mask & (1u << sample)) != 0;
      if (dxilPositions && covered)
        require(referenceColors[at] >= 0 && referenceColors[at] <= 1 &&
                referenceColors[at + 1] >= 0 && referenceColors[at + 1] <= 1 &&
                std::fabs(referenceColors[at + 2] - (sample + .5f) / 4) < .01f &&
                std::fabs(referenceColors[at + 3] - 1) < .01f,
                "sample-frequency position control disagrees with sample index");
      else
        require(std::fabs(referenceColors[at] - (covered && !dxilPositions ? .125f : 0)) < .01f,
                "sample-frequency control disagrees with per-sample RTV");
      const bool visible = covered && !skipped && config.depth != 2 && config.depth != 4 &&
                           config.depth != 5 && (!early || (config.depth == 8 ?
                           checkerboardPass : (sample & 1))) &&
                           !(config.alphaToCoverage && config.shader == 10) &&
                           !(config.shader == 16 && (pixel & 1) && sample != 0);
      if (config.depth && visible) ++passingSamples;
      const float expectedRed = !visible ? 0 :
          config.shader == 28 ? 0 :
          config.shader == 22 ? .5f :
          config.shader == 8 ? (config.blend ? .75f :
                                config.overlap || config.drawMode == 1 ? .5f : .25f) :
          config.shader == 9 && (pixel & 1) ? 0 : .25f;
      const float expectedAlpha = !visible || (config.shader == 9 && (pixel & 1)) ? 1 :
          config.shader == 10 ? 0 : 1;
      const bool writesColor = visible && !(config.shader == 9 && (pixel & 1));
      const float expectedGreen = writesColor ? (config.blend ? 1 : .5f) : 0;
      const float expectedBlue = writesColor ? (config.blend ? 1 : .75f) : 0;
      if (covered) ++expectedBuckets[sample];
      if (config.depth) {
        const float expectedDepth = config.depth == 8 ?
                                    (checkerboardPass ? .5f : .25f) :
                                    config.depth == 1 || config.depth == 6 ?
                                    (sample & 1 ? .5f : .25f) :
                                    config.depth == 7 ? (sample & 1 ? .5f : 1) :
                                    config.depth == 3 ? .875f :
                                    config.depth == 4 ? .5f : .25f;
        require(std::fabs(colors[256 + at] - expectedDepth) < .001f,
                "per-sample depth differs from early/late shader semantics");
        if (stencil) {
          const float expectedStencil = config.depth == 8 ? (checkerboardPass ? 2 : 1) :
                                        config.depth == 6 ? (sample & 1 ? 2 : 1) :
                                        (sample & 1 ? 2 : 0);
          require(std::fabs(colors[512 + at] - expectedStencil) < .01f,
                  "per-sample stencil updated more or fewer than once");
        }
      }
      if (covered && (config.shader == 13 || config.shader == 14 ||
                      config.shader == 17 || config.shader == 20 || config.shader == 21)) {
        const float x = float(pixel % 4) + .5f;
        const float y = float(pixel / 4) + .5f;
        const auto varyingX = [](float sx) { return (sx / 6) / (1 - sx / 24); };
        const auto varyingY = [](float sx, float sy) {
          return (sy / 4 - sx / 24) / (1 - sx / 24);
        };
        if (config.shader == 13 && pixel == 5) {
          const auto edgeX = [](float sx, float sy) {
            return (sx / 2.4f - sy / 12) / (1 - sy / 12);
          };
          const auto edgeY = [](float sy) { return (sy / 6) / (1 - sy / 12); };
          require(config.edge ?
                  (std::fabs(colors[at] - edgeX(x, y)) < .025f &&
                   std::fabs(colors[at + 1] - edgeY(y)) < .025f &&
                   std::fabs(colors[at + 2] - (edgeX(2.5f, y) - edgeX(1.5f, y))) < .025f &&
                   std::fabs(colors[at + 3] - (edgeY(1.5f) - edgeY(.5f))) < .025f) :
                  (std::fabs(colors[at] - varyingX(x)) < .025f &&
                   std::fabs(colors[at + 1] - varyingY(x, y)) < .025f &&
                   std::fabs(colors[at + 2] - (varyingX(1.5f) - varyingX(.5f))) < .025f &&
                   std::fabs(colors[at + 3] -
                             (varyingY(x, 1.5f) - varyingY(x, .5f))) < .025f),
                  "perspective centroid/implicit derivatives changed");
        }
        if (config.shader == 13 && !config.edge)
          require(std::fabs(colors[at] - varyingX(x)) < .003f &&
                  std::fabs(colors[at + 1] - varyingY(x, y)) < .003f,
                  "fully covered by-value centroid differed from center in another pixel");
        if (config.shader == 13 && config.edge && pixel == 6) {
          float minX = 1, maxX = 0;
          for (UINT coveredSample = 0; coveredSample < config.samples; ++coveredSample) {
            if (!(mask & (1u << coveredSample))) continue;
            const UINT sampleAt = (pixel * 4 + coveredSample) * 4;
            const float sx = dxilPositions ? referenceColors[sampleAt] - .5f : -.5f;
            const float sy = dxilPositions ? referenceColors[sampleAt + 1] - .5f : -.5f;
            const float value = ((x + sx) / 2.4f - (y + sy) / 12) /
                                (1 - (y + sy) / 12);
            if (value < minX) minX = value;
            if (value > maxX) maxX = value;
          }
          require(dxilPositions ?
                  (colors[at] >= minX - .025f && colors[at] <= maxX + .025f) :
                  (colors[at] > 0 && colors[at] < 1.01f),
                  "center-outside centroid interpolation escaped covered samples");
          if (dxilPositions && mask) {
            const UINT first = __builtin_ctz(mask);
            const UINT selected = (pixel * 4 + first) * 4;
            const float sx = referenceColors[selected] - .5f;
            const float sy = referenceColors[selected + 1] - .5f;
            const float expectedX = ((x + sx) / 2.4f - (y + sy) / 12) /
                                    (1 - (y + sy) / 12);
            const float expectedY = ((y + sy) / 6) / (1 - (y + sy) / 12);
            require(std::fabs(colors[at] - expectedX) < .003f &&
                    std::fabs(colors[at + 1] - expectedY) < .003f,
                    "partial primitive centroid ignored first geometric and PSO-mask covered sample");
          }
        }
        if (config.shader == 14) {
          const UINT selected = (pixel * 3 + 1) % config.samples;
          const UINT sampleAt = (pixel * 4 + selected) * 4;
          const float sx = referenceColors[sampleAt] - .5f;
          const float sy = referenceColors[sampleAt + 1] - .5f;
          const float snappedX = float(int(pixel % 4) - 2) / 16;
          const float snappedY = float(int(pixel / 4) - 2) / 16;
          require(std::fabs(colors[at] - varyingX(x + sx)) < .025f &&
                  std::fabs(colors[at + 1] - varyingY(x + snappedX, y + snappedY)) < .025f &&
                  std::fabs(colors[at + 2] - referenceColors[sampleAt]) < .025f &&
                  std::fabs(colors[at + 3] - referenceColors[sampleAt + 1]) < .025f,
                  "dynamic sample/snapped evaluation or target sample count/position changed");
        }
        if (config.shader == 17) {
          float expectedX = 0, expectedY = 0;
          for (UINT i = 0; i < 16; ++i) {
            const float offsetX = (int((pixel + i) & 15) - 8) / 16.f;
            const float offsetY = (int((4 * pixel + 5 * i) & 15) - 8) / 16.f;
            expectedX += varyingX(x + offsetX) / 32;
            expectedY += varyingY(x + offsetX, y + offsetY) / 32;
          }
          require(std::fabs(colors[at] - expectedX) < .025f &&
                  std::fabs(colors[at + 1] - expectedY) < .025f &&
                  std::fabs(colors[at + 2] - .25f) < .01f &&
                  std::fabs(colors[at + 3] - 1) < .01f,
                  "dynamic snapped offset matrix did not preserve interpolation");
        }
        if (config.shader == 20 && pixel == 5) {
          require(std::fabs(colors[at] - x / 4) < .025f &&
                  std::fabs(colors[at + 1] - y / 4) < .025f,
                  "fully covered centroid SV_Position.xy changed with PSO sample mask");
          const float centerDepth = .5f - .1f * y + .1f * x;
          require(std::fabs(colors[at + 2] - centerDepth) < .002f,
                  "fully covered centroid SV_Position.z lost center depth");
          require(std::fabs(colors[at + 3] - 1 / (1 - 3 * x / 32)) < .002f,
                  "fully covered centroid SV_Position.w lost center perspective W");
        }
        if (config.shader == 21 && pixel == 5) {
          const float expectedX = varyingX(x);
          const float expectedY = varyingY(x, y);
          require(std::fabs(colors[at] - expectedX) < .025f &&
                  std::fabs(colors[at + 1] - expectedY) < .025f &&
                  std::fabs(colors[at + 2] - expectedX) < .025f &&
                  std::fabs(colors[at + 3] - expectedY) < .025f,
                  "EvaluateAttributeCentroid differs from masked centroid input");
        }
        continue;
      }
      if (config.shader == 28) {
        if (visible) {
          const float x = float(pixel % 4) + .5f;
          const float y = float(pixel / 4) + .5f;
          const auto nonlinearX = [](float sx) {
            const float q = (sx / 6) / (1 - sx / 24);
            return q * q;
          };
          const auto nonlinearY = [](float sx, float sy) {
            const float q = (sy / 4 - sx / 24) / (1 - sx / 24);
            return q * q;
          };
          const float expectedDx = nonlinearX(float(pixel % 4 | 1) + .5f) -
                                   nonlinearX(float(pixel % 4 & ~1u) + .5f);
          const float expectedDy = nonlinearY(x, float(pixel / 4 | 1) + .5f) -
                                   nonlinearY(x, float(pixel / 4 & ~1u) + .5f);
          require(std::fabs(colors[at] - expectedDx) < .02f &&
                  std::fabs(colors[at + 1] - expectedDy) < .02f &&
                  std::fabs(colors[at + 2] - .5f) < .01f &&
                  std::fabs(colors[at + 3] - 1) < .01f,
                  "early-depth killed helper before nonlinear fine derivatives");
        } else require(colors[at] == 0 && colors[at + 1] == 0 &&
                       colors[at + 2] == 0 && colors[at + 3] == 1,
                       "early-depth helper wrote a rejected sample");
        continue;
      }
      if (std::fabs(colors[at] - expectedRed) > .01f ||
          std::fabs(colors[at + 1] - expectedGreen) > .01f ||
          std::fabs(colors[at + 2] - expectedBlue) > .01f ||
          std::fabs(colors[at + 3] - expectedAlpha) > .01f)
        fail(config.name);
    }
  }
  for (UINT sample = 0; sample < config.samples; ++sample)
    require(sampleBuckets[sample] == expectedBuckets[sample], "sample-frequency bucket");
  if (config.mask == 0) require(pixels == 0, "zero PSO sample mask covered pixels");
  else require(pixels > 0 && coveredSamples >= pixels, "MSAA coverage oracle was empty");
  if (!config.edge && !config.viewportOffset && config.mask == (1u << config.samples) - 1)
    require(pixels == 16 && coveredSamples == 16 * config.samples,
            "full geometry lost samples in independent control shader");
  if (config.clipped)
    require(reference[0] == 0 && reference[1] != 0,
            "near clipping did not trim the edge triangle");
  if (config.scissor)
    require(reference[6] == 0 && reference[5] != 0,
            "scissor did not trim partial MSAA coverage");
  if (config.viewportOffset)
    require(reference[0] == 0 && reference[3] == 0 && reference[12] == 0 &&
            reference[15] == 0 && pixels > 0,
            "offset viewport lost its MSAA bounds or guard pixels");
  if (config.mask && config.mask != (1u << config.samples) - 1 && !config.clipped)
    require(reference[1] == config.mask,
            "interior MSAA pixel did not retain selected PSO mask bits");
  require(stats[7] == invocations + (config.drawMode == 7 ? 32 : 0),
          "MSAA query disagrees with pixel coverage or adjacent ordinary draws");
  if (occlusionReadback) {
    D3D12_RANGE range{0, sizeof(UINT64)};
    check(occlusionReadback->Map(0, &range, &mapped), "MSAA occlusion readback map");
    const UINT64 passed = *static_cast<const UINT64 *>(mapped);
    D3D12_RANGE empty{0, 0}; occlusionReadback->Unmap(0, &empty);
    require(passed == passingSamples,
            "MSAA occlusion counted an internal pass or missed surviving samples");
  }
  const UINT expectedVertices = config.drawMode == 3 || config.drawMode == 5 ? 0 :
                                config.drawMode == 8 ? 12 :
                                config.drawMode == 7 ? 9 :
                                config.overlap || config.drawMode == 1 ? 6 : 3;
  require(stats[0] == expectedVertices && stats[2] == expectedVertices,
          "MSAA query counted internal or skipped vertex invocations");
  require(f.oracle()[0] == expectedVertices,
          "MSAA vertex shader reran during capture or replay");
  if (config.drawMode == 7) {
    D3D12_RANGE range{192, 196};
    check(f.entriesReadback->Map(0, &range, &mapped), "ordinary shader counter map");
    require(*reinterpret_cast<const UINT *>(static_cast<const uint8_t *>(mapped) + 192) == 32,
            "ordinary draws before/after MSAA did not each shade 16 pixels");
    D3D12_RANGE empty{0, 0}; f.entriesReadback->Unmap(0, &empty);
  }
  if (config.edge && !config.clipped && !config.scissor && !config.viewportOffset &&
      config.mask == (1u << config.samples) - 1)
    require(partialPixels && reference[6] && (reference[6] != ((1u << config.samples) - 1)),
            "center-outside edge pixel lost partial coverage");
  std::printf("%s_GPU_CASE_PASS case=%s samples=%u pixels=%u covered_samples=%u partial=%u ps=%llu\n",
              marker, config.name, config.samples, pixels, coveredSamples, partialPixels,
              static_cast<unsigned long long>(stats[7]));
}

#include "owned-d3d12-msaa-depth-interop.inc"
#include "owned-d3d12-msaa-eight.inc"
#include "owned-d3d12-msaa-early-sample.inc"
#include "owned-d3d12-msaa-viewport-depth.inc"

void msaaArrayMRT(Fixture &f, const MSReader &reader,
                  const std::array<D3D12_SHADER_BYTECODE, 30> &code) {
  static constexpr char readSource[] = R"(
Texture2DMSArray<float4> color : register(t0);
Texture2DMSArray<uint4> typed : register(t1);
RWStructuredBuffer<float4> output : register(u0);
[numthreads(4,4,1)] void main(uint3 id : SV_DispatchThreadID) {
  for (uint layer = 0; layer < 2; ++layer)
    for (uint sample = 0; sample < 4; ++sample) {
      uint at = ((layer * 16 + id.y * 4 + id.x) * 4 + sample) * 2;
      output[at] = color.Load(int3(id.xy, layer), sample);
      output[at + 1] = float4(typed.Load(int3(id.xy, layer), sample).x, 0, 0, 0);
    }
}
)";
  auto bytecode = compile(readSource, "main", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC readDesc{};
  readDesc.pRootSignature = reader.root.Get();
  readDesc.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
  ComPtr<ID3D12PipelineState> sampleReader;
  check(f.device->CreateComputePipelineState(&readDesc, IID_PPV_ARGS(&sampleReader)),
        "layered MSAA reader");
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = texture.Height = 4;
  texture.DepthOrArraySize = 2;
  texture.MipLevels = 1;
  texture.SampleDesc.Count = 4;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT};
  ComPtr<ID3D12Resource> targets[2];
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = 2;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(f.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)),
        "layered MRT descriptors");
  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = 3;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(f.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap)),
        "layered MRT SRVs");
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2]{};
  auto srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < 2; ++i) {
    texture.Format = formats[i];
    check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &texture,
          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])),
          "layered MSAA target");
    rtvs[i] = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvs[i].ptr += i * f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = formats[i];
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
    rtv.Texture2DMSArray.ArraySize = 2;
    f.device->CreateRenderTargetView(targets[i].Get(), &rtv, rtvs[i]);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = formats[i];
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DMSArray.ArraySize = 2;
    f.device->CreateShaderResourceView(targets[i].Get(), &srv, srvHandle);
    srvHandle.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC spare{};
  spare.Format = formats[0];
  spare.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
  spare.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  spare.Texture2DMSArray.ArraySize = 2;
  f.device->CreateShaderResourceView(targets[0].Get(), &spare, srvHandle);
  auto output = buffer(f.device.Get(), 4096, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(f.device.Get(), 4096, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_INPUT_ELEMENT_DESC input[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = f.shader.root.Get();
  pso.InputLayout = {input, 2};
  pso.VS = code[18];
  pso.PS = code[19];
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 2;
  pso.RTVFormats[0] = formats[0]; pso.RTVFormats[1] = formats[1];
  pso.SampleMask = UINT_MAX;
  pso.SampleDesc.Count = 4;
  ComPtr<ID3D12PipelineState> pipeline;
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)),
        "layered typed MRT PSO");
  const StageTriangle triangle{{{{-1,-1,.5f,1}, {0,0,1,1}},
                                {{-1,3,.5f,1}, {0,0,1,1}},
                                {{3,-1,.5f,1}, {0,0,1,1}}}};
  auto vb = buffer(f.device.Get(), sizeof(triangle), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(vb->Map(0, nullptr, &mapped), "layered vertices map");
  std::memcpy(mapped, triangle.data(), sizeof(triangle)); vb->Unmap(0, nullptr);
  auto q = queryHeap(f.device.Get(), 1);
  auto query = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  const float clear[]{0, 0, 0, 1};
  l->ClearRenderTargetView(rtvs[0], clear, 0, nullptr);
  l->ClearRenderTargetView(rtvs[1], clear, 0, nullptr);
  l->SetGraphicsRootSignature(f.shader.root.Get()); l->SetPipelineState(pipeline.Get());
  l->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
  l->SetGraphicsRootUnorderedAccessView(2, f.entries->GetGPUVirtualAddress() + 16);
  D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), sizeof(triangle), sizeof(StageVertex)};
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  l->IASetVertexBuffers(0, 1, &view);
  D3D12_VIEWPORT viewports[2]{{0, 0, 1, 1, 0, 1}, {0, 0, 4, 4, 0, 1}};
  D3D12_RECT scissors[2]{{0, 0, 1, 1}, {0, 0, 4, 4}};
  l->RSSetViewports(2, viewports); l->RSSetScissorRects(2, scissors);
  l->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->DrawInstanced(3, 1, 0, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, query.Get(), 0);
  for (const auto &target : targets)
    transition(l, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  l->SetComputeRootSignature(reader.root.Get()); l->SetPipelineState(sampleReader.Get());
  ID3D12DescriptorHeap *heaps[]{srvHeap.Get()};
  l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
  l->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  l->Dispatch(1, 1, 1);
  transition(l, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  l->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4096);
  f.finish(l); f.submit(r);
  const Stats stats = readStats(query.Get(), 0);
  require(stats[0] == 3 && stats[1] == 1 && stats[2] == 3 &&
          stats[5] == 1 && stats[6] == 1 && stats[7] == 16 &&
          f.oracle()[0] == 3, "layered MRT query or vertex invocations");
  D3D12_RANGE counterRange{16, 80};
  check(f.entriesReadback->Map(0, &counterRange, &mapped), "layered PS counter map");
  const auto *words = static_cast<const UINT *>(mapped);
  for (UINT pixel = 0; pixel < 16; ++pixel)
    require(words[4 + pixel] == 1, "layered MRT pixel shader ran per sample");
  D3D12_RANGE empty{0, 0}; f.entriesReadback->Unmap(0, &empty);
  D3D12_RANGE range{0, 4096};
  check(readback->Map(0, &range, &mapped), "layered MRT samples map");
  const float *values = static_cast<const float *>(mapped);
  for (UINT layer = 0; layer < 2; ++layer)
    for (UINT pixel = 0; pixel < 16; ++pixel)
      for (UINT sample = 0; sample < 4; ++sample) {
        const UINT at = ((layer * 16 + pixel) * 4 + sample) * 8;
        const float red = layer ? .25f : 0;
        const float green = layer ? .5f : 0;
        const float blue = layer ? .75f : 0;
        const float typed = layer ? float((pixel + 1) * 17) : 0;
        if (std::fabs(values[at] - red) > .01f ||
            std::fabs(values[at + 1] - green) > .01f ||
            std::fabs(values[at + 2] - blue) > .01f ||
            std::fabs(values[at + 3] - 1) > .01f ||
            std::fabs(values[at + 4] - typed) > .01f)
          fail("MSAA typed MRT array layer readback");
      }
  readback->Unmap(0, &empty);
  std::printf("%s_GPU_CASE_PASS case=msaa_4x_array_layer_typed_mrt ps=16 layers=0,1\n", marker);
}

void stageBoundaries(Fixture &f, bool dxil) {
  auto source = shaderFile("pipeline-statistics-stages.hlsl");
  source.push_back(0);
  const char *entries[]{"vsDistances", "psDiscard", "psEarly", "psDepth", "psPixel", "psSample",
                        "vsMSAA", "psMSCoverage", "psMSAtomic", "psMSDiscard", "psMSZeroAlpha",
                        "psMSEarly", "psMSDepth", "psMSCentroid", "psMSEvaluate",
                        "psMSPrepass", "psMSOutputMask", "psMSAllSnaps",
                        "vsMSArray", "psMSMRT", "psMSCentroidPosition", "psMSEvalCentroid",
                        "psMSPrimitive", "psMSDepthPosition", "psMSSampleDepthPosition",
                        "psMSSampleInterpolatedDepthPosition", "psMSCentroidOutputDepth",
                        "psMSSampleEarlyMask", "psMSEarlyNonlinearDerivative",
                        "psMSCheckerboardPrepass"};
  const char *names[]{"pipeline-statistics-vsDistances.dxil",
                      "pipeline-statistics-psDiscard.dxil",
                      "pipeline-statistics-psEarly.dxil",
                      "pipeline-statistics-psDepth.dxil",
                      "pipeline-statistics-psPixel.dxil",
                      "pipeline-statistics-psSample.dxil",
                      "pipeline-statistics-vsMSAA.dxil",
                      "pipeline-statistics-psMSCoverage.dxil",
                      "pipeline-statistics-psMSAtomic.dxil",
                      "pipeline-statistics-psMSDiscard.dxil",
                      "pipeline-statistics-psMSZeroAlpha.dxil",
                      "pipeline-statistics-psMSEarly.dxil",
                      "pipeline-statistics-psMSDepth.dxil",
                      "pipeline-statistics-psMSCentroid.dxil",
                      "pipeline-statistics-psMSEvaluate.dxil",
                      "pipeline-statistics-psMSPrepass.dxil",
                      "pipeline-statistics-psMSOutputMask.dxil",
                      "pipeline-statistics-psMSAllSnaps.dxil",
                      "pipeline-statistics-vsMSArray.dxil",
                      "pipeline-statistics-psMSMRT.dxil",
                      "pipeline-statistics-psMSCentroidPosition.dxil",
                      "pipeline-statistics-psMSEvalCentroid.dxil",
                      "pipeline-statistics-psMSPrimitive.dxil",
                      "pipeline-statistics-psMSDepthPosition.dxil",
                      "pipeline-statistics-psMSSampleDepthPosition.dxil",
                      "pipeline-statistics-psMSSampleInterpolatedDepthPosition.dxil",
                      "pipeline-statistics-psMSCentroidOutputDepth.dxil",
                      "pipeline-statistics-psMSSampleEarlyMask.dxil",
                      "pipeline-statistics-psMSEarlyNonlinearDerivative.dxil",
                      "pipeline-statistics-psMSCheckerboardPrepass.dxil"};
  std::array<ComPtr<ID3DBlob>, 30> dxbcCode;
  std::array<std::vector<uint8_t>, 30> dxilCode;
  std::array<D3D12_SHADER_BYTECODE, 30> code{};
  for (UINT i = 0; i < code.size(); ++i) {
    if (dxil) {
      dxilCode[i] = shaderFile(names[i]);
      code[i] = {dxilCode[i].data(), dxilCode[i].size()};
    } else {
      if (i == 14 || i == 17 || i == 21 || i == 25) continue; // Wine DXBC compiler lacks SM5 evaluation/sample interpolation syntax.
      dxbcCode[i] = compile(reinterpret_cast<const char *>(source.data()), entries[i],
                            i == 0 || i == 6 || i == 18 ? "vs_5_0" : "ps_5_0");
      code[i] = {dxbcCode[i]->GetBufferPointer(), dxbcCode[i]->GetBufferSize()};
    }
  }
  const StageTriangle full{{{{-1,-1,.5f,1}, {1,1,1,1}},
                            {{-1,3,.5f,1}, {1,1,1,1}},
                            {{3,-1,.5f,1}, {1,1,1,1}}}};
  const D3D12_SHADER_BYTECODE noPS{};
  StageOptions o;
  o.minClip = o.maxClip = 1; o.minPixel = o.maxPixel = 16; o.unchangedColor = true;
  stageCase(f, "stage_discard", code[0], code[1], full.data(), 3, o);
  o.depth = true; o.minPixel = o.maxPixel = 0;
  stageCase(f, "stage_early_depth_rejected", code[0], code[2], full.data(), 3, o);
  StageTriangle close = full;
  for (auto &v : close) v.position[2] = .1f;
  o.minPixel = o.maxPixel = 16;
  stageCase(f, "stage_ps_depth_rejected", code[0], code[3], close.data(), 3, o);
  o = {}; o.noTarget = true; o.minInvocation = o.maxInvocation = 0;
  o.minClip = o.maxClip = 0;
  stageCase(f, "stage_null_ps_no_depth", code[0], noPS, full.data(), 3, o);
  o.depth = true; o.minInvocation = o.maxInvocation = 1;
  o.minClip = o.maxClip = 1;
  stageCase(f, "stage_null_ps_with_depth", code[0], noPS, full.data(), 3, o);
  StageTriangle clipped = full;
  for (auto &v : clipped) v.distances[0] = -1;
  o = {}; o.noTarget = true; o.depth = true; o.minClip = o.maxClip = 0;
  stageCase(f, "stage_clipdistance_rejected", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  for (auto &v : clipped) v.distances[1] = -1;
  stageCase(f, "stage_clipdistance_second_packed_rejected", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  for (auto &v : clipped) v.distances[2] = -1;
  stageCase(f, "stage_culldistance_rejected", code[0], noPS, clipped.data(), 3, o);
  clipped = full; clipped[0].distances[0] = -1;
  o.minClip = o.maxClip = 2;
  stageCase(f, "stage_clipdistance_crossing", code[0], noPS, clipped.data(), 3, o);
  clipped = full; clipped[0].distances[2] = -1;
  o.minClip = o.maxClip = 1;
  stageCase(f, "stage_culldistance_partial", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  clipped[0].position[3] = -1;
  o.minClip = 0; o.maxClip = 2;
  stageCase(f, "stage_w_crossing", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  for (auto &v : clipped) v.position[3] = 0;
  o.minClip = o.maxClip = 0;
  stageCase(f, "stage_w_zero", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  uint32_t nanBits = 0x7fc00000;
  std::memcpy(&clipped[0].position[0], &nanBits, sizeof(nanBits));
  clipped[0].position[1] = -.5f; clipped[1].position[0] = .5f;
  clipped[1].position[1] = -.5f; clipped[2].position[0] = 0;
  clipped[2].position[1] = .5f;
  stageCase(f, "stage_nan_position_rejected", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  for (auto &v : clipped) v.position[2] = -.5f;
  uint32_t infinityBits = 0x7f800000;
  std::memcpy(&clipped[0].position[0], &infinityBits, sizeof(infinityBits));
  stageCase(f, "stage_infinite_position_behind_near", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  std::memcpy(&clipped[0].distances[0], &nanBits, sizeof(nanBits));
  stageCase(f, "stage_nan_clipdistance_rejected", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  o.minClip = o.maxClip = 1; o.cull = D3D12_CULL_MODE_BACK;
  stageCase(f, "stage_backface", code[0], noPS, clipped.data(), 3, o);
  clipped = full;
  clipped[1].position[0] = clipped[0].position[0];
  clipped[1].position[1] = clipped[0].position[1];
  clipped[2].position[0] = clipped[0].position[0];
  clipped[2].position[1] = clipped[0].position[1];
  o.cull = D3D12_CULL_MODE_NONE;
  stageCase(f, "stage_degenerate", code[0], noPS, clipped.data(), 3, o);
  StageVertex line[]{{{0,0,0,-1}, {1,1,1,1}},
                     {{.5f,.5f,0,1}, {-1,1,1,1}}};
  o.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
  o.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  o.depthClip = false;
  o.minClip = o.maxClip = 0;
  stageCase(f, "stage_line_w_boundary", code[0], noPS, line, 2, o);
  o = {}; o.multisample = true; o.minPixel = o.maxPixel = 64; o.sampleIndexCount = 16;
  stageCase(f, "stage_msaa_sample", code[0], code[5], full.data(), 3, o);
  o.minPixel = o.maxPixel = 16; o.sampleIndexCount = 0;
  stageCase(f, "stage_msaa_pixel", code[0], code[4], full.data(), 3, o);
  const MSReader reader = makeMSReader(f);
  msaaArrayMRT(f, reader, code);
  msaaEightBoundary(f, dxil);
  for (UINT samples : {2u, 4u}) {
    msaaEarlySampleMask(f, reader, code, samples, 0);
    msaaEarlySampleMask(f, reader, code, samples, 1);
    msaaViewportDepth(f, reader, code, samples, -1.f);
    msaaViewportDepth(f, reader, code, samples, 2.f);
    for (DXGI_FORMAT format : {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT})
      for (INT bias : {0, 64})
        msaaDepthInterop(f, reader, code, samples, format, bias);
    const UINT all = (1u << samples) - 1;
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_full_atomic" : "msaa_4x_full_atomic",
                           samples, all, 8});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_edge_atomic" : "msaa_4x_edge_atomic",
                           samples, all, 8, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_mask_zero" : "msaa_4x_mask_zero",
                           samples, 0, 8, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_mask_one" : "msaa_4x_mask_one",
                           samples, 1, 8, true});
    if (samples == 4)
      msaaBehavior(f, reader, code, {"msaa_4x_mask_mixed", samples, 5, 8, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_edge_discard" : "msaa_4x_edge_discard",
                           samples, all, 9, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_zero_alpha" : "msaa_4x_zero_alpha",
                           samples, all, 10, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_zero_alpha_a2c" : "msaa_4x_zero_alpha_a2c",
                           samples, all, 10, true, false, false, false, false, 0, 0, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_shader_coverage" :
                                          "msaa_4x_shader_coverage",
                           samples, all, 16, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_overlap_atomic" : "msaa_4x_overlap_atomic",
                           samples, all, 8, true, false, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_primitive_id_instanced" :
                                          "msaa_4x_primitive_id_instanced",
                           samples, all, 22, false, false, false, false, false, 0, 8});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_near_clip" : "msaa_4x_near_clip",
                           samples, all, 8, true, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_scissor" : "msaa_4x_scissor",
                           samples, all, 8, true, false, false, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_blend" : "msaa_4x_blend",
                           samples, all, 8, true, false, true, false, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_centroid" : "msaa_4x_centroid",
                           samples, all, 13});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_edge_centroid" : "msaa_4x_edge_centroid",
                           samples, all, 13, true});
    if (dxil)
      msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_edge_centroid_mask_sample1" :
                                                 "msaa_4x_edge_centroid_mask_sample2",
                                     samples, samples == 2 ? 2u : 4u, 13, true});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_centroid_mask_one" :
                                                 "msaa_4x_centroid_mask_one",
                                    samples, 1, 13});
    if (samples == 4) {
      msaaBehavior(f, reader, code, {"msaa_4x_centroid_mask_mixed", samples, 5, 13});
      msaaBehavior(f, reader, code, {"msaa_4x_centroid_position_mask_one", samples, 1, 20});
    }
    if (dxil)
      msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_eval" : "msaa_4x_eval",
                             samples, all, 14});
    if (dxil && samples == 4)
      msaaBehavior(f, reader, code, {"msaa_4x_all_snapped_offsets", samples, all, 17});
    if (dxil && samples == 4)
      msaaBehavior(f, reader, code, {"msaa_4x_evaluate_centroid_mask_one", samples, 1, 21});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_early_partial_depth" :
                                          "msaa_4x_early_partial_depth",
                           samples, all, 11, false, false, false, false, false, 1});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_early_checkerboard_fine_deriv" :
                                          "msaa_4x_early_checkerboard_fine_deriv",
                           samples, all, 28, false, false, false, false, false, 8});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_late_occluded_uav" :
                                          "msaa_4x_late_occluded_uav",
                           samples, all, 8, false, false, false, false, false, 2});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_shader_depth" :
                                          "msaa_4x_shader_depth",
                           samples, all, 12, false, false, false, false, false, 3});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_shader_depth_reject" :
                                          "msaa_4x_shader_depth_reject",
                           samples, all, 12, false, false, false, false, false, 4});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_early_full_reject" :
                                          "msaa_4x_early_full_reject",
                           samples, all, 11, false, false, false, false, false, 5});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_depth_stencil_once" :
                                          "msaa_4x_depth_stencil_once",
                           samples, all, 11, false, false, false, false, false, 6});
    msaaBehavior(f, reader, code, {samples == 2 ? "msaa_2x_stencil_partial" :
                                          "msaa_4x_stencil_partial",
                           samples, all, 11, false, false, false, false, false, 7});
    if (samples == 2)
      msaaBehavior(f, reader, code, {"msaa_2x_indexed_instanced", samples, all, 8,
                             false, false, false, false, false, 0, 1});
    else {
      msaaBehavior(f, reader, code, {"msaa_4x_offset_viewport", samples, all, 8,
                                     false, false, false, false, false, 0, 0, false, true});
      msaaBehavior(f, reader, code, {"ordinary_msaa_ordinary_one_list", samples, all, 8,
                                     false, false, false, false, false, 0, 7});
      msaaBehavior(f, reader, code, {"msaa_4x_indirect_count_one", samples, all, 8,
                             false, false, false, false, false, 0, 2});
      msaaBehavior(f, reader, code, {"msaa_4x_indirect_count_zero", samples, all, 8,
                             false, false, false, false, false, 0, 3});
      msaaBehavior(f, reader, code, {"msaa_4x_predicate_true", samples, all, 8,
                             false, false, false, false, false, 0, 4});
      msaaBehavior(f, reader, code, {"msaa_4x_predicate_false", samples, all, 8,
                             false, false, false, false, false, 0, 5});
      msaaBehavior(f, reader, code, {"msaa_4x_bundle_query", samples, all, 8,
                             false, false, false, false, false, 0, 6});
    }
  }
}
void runCase(Fixture &f, const char *name, const Stats &wanted, bool draw, bool dispatch,
             bool clearState = false, bool raster = true) {
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto fill = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(fill->Map(0, nullptr, &mapped), "map sentinel");
  std::memset(mapped, 0xA5, 512); fill->Unmap(0, nullptr);
  auto r = recording(f.device.Get()); auto *l = r.list.Get();
  f.prepare(l);
  l->CopyBufferRegion(out.Get(), 0, fill.Get(), 0, 512);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  if (draw) f.draw(l, 3, raster);
  if (clearState) l->ClearState(nullptr);
  if (dispatch) f.dispatch(l);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  if (draw && raster) f.captureColor(l);
  f.finish(l); f.submit(r);
  auto actual = readStats(out.Get(), 24); guard(out.Get(), 24, 1);
  compare(name, actual, wanted, f.oracle());
  if (draw && raster) f.verifyColor();
}
void lifecycleAndIndirect(Fixture &f) {
  constexpr auto type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
  // Resolving a slot does not freeze it: a second Begin on that same slot
  // starts at zero, even without closing the command list in between.
  {
    auto q = queryHeap(f.device.Get(), 1);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    l->BeginQuery(q.Get(), type, 0); f.draw(l, 3, false); l->EndQuery(q.Get(), type, 0);
    l->ResolveQueryData(q.Get(), type, 0, 1, out.Get(), 0);
    l->BeginQuery(q.Get(), type, 0); f.dispatch(l); l->EndQuery(q.Get(), type, 0);
    l->ResolveQueryData(q.Get(), type, 0, 1, out.Get(), 128);
    f.finish(l); f.submit(r);
    Stats draw = kDraw; draw[7] = 0;
    require(f.oracle() == std::array<UINT, 3>{3, 0, 48}, "reuse shader oracle");
    require(readStats(out.Get(), 0) == draw && readStats(out.Get(), 128) == kCompute,
            "query slot reuse must reset rather than accumulate");
    std::printf("%s_GPU_CASE_PASS case=same_recording_slot_reuse oracle=3,0,48\n", marker);
  }
  // A closed list is reusable only after the preceding execution has retired.
  {
    auto q = queryHeap(f.device.Get(), 1);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    l->BeginQuery(q.Get(), type, 0); f.dispatch(l); l->EndQuery(q.Get(), type, 0);
    l->ResolveQueryData(q.Get(), type, 0, 1, out.Get(), 0);
    f.finish(l); f.submit(r);
    compare("closed_list_first", readStats(out.Get(), 0), kCompute, f.oracle());
    ID3D12CommandList *lists[]{l}; f.queue->ExecuteCommandLists(1, lists);
    check(f.queue->Signal(f.fence.Get(), ++f.fenceValue), "replay signal");
    if (f.fence->GetCompletedValue() < f.fenceValue) {
      check(f.fence->SetEventOnCompletion(f.fenceValue, f.event), "replay event");
      require(WaitForSingleObject(f.event, kTimeout) == WAIT_OBJECT_0, "replay fence timeout");
    }
    compare("closed_list_replay", readStats(out.Get(), 0), kCompute, f.oracle());
  }
  // A bundle's draw belongs to the query on its executing parent list.
  {
    auto q = queryHeap(f.device.Get(), 1);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto b = recording(f.device.Get(), D3D12_COMMAND_LIST_TYPE_BUNDLE);
    b.list->SetGraphicsRootSignature(f.shader.root.Get());
    b.list->SetPipelineState(f.shader.graphics.Get());
    b.list->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
    b.list->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
    b.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{f.vertices->GetGPUVirtualAddress(), 6 * 4 * sizeof(float), 4 * sizeof(float)};
    b.list->IASetVertexBuffers(0, 1, &vb);
    b.list->DrawInstanced(3, 1, 0, 0);
    check(b.list->Close(), "close bundle");
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l); f.bindGraphics(l);
    l->BeginQuery(q.Get(), type, 0);
    l->ExecuteBundle(b.list.Get());
    l->EndQuery(q.Get(), type, 0); l->ResolveQueryData(q.Get(), type, 0, 1, out.Get(), 0);
    f.captureColor(l); f.finish(l); f.submit(r);
    compare("bundle_parent_query", readStats(out.Get(), 0), kDraw, f.oracle()); f.verifyColor();
  }
  // Work outside either query is visible in the GPU oracle, not in either
  // query interval; the already-cached graphics PSO must give the same vector.
  {
    auto q = queryHeap(f.device.Get(), 2);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    f.draw(l, 3, false);
    l->BeginQuery(q.Get(), type, 0); f.draw(l, 3, false); l->EndQuery(q.Get(), type, 0);
    f.dispatch(l);
    l->BeginQuery(q.Get(), type, 1); f.draw(l, 3, false); l->EndQuery(q.Get(), type, 1);
    l->ResolveQueryData(q.Get(), type, 0, 2, out.Get(), 0);
    f.finish(l); f.submit(r);
    Stats draw = kDraw; draw[7] = 0;
    require(f.oracle() == std::array<UINT, 3>{9, 0, 48}, "outside query oracle");
    require(readStats(out.Get(), 0) == draw && readStats(out.Get(), 88) == draw,
            "outside work or cached PSO leaked into query");
    std::printf("%s_GPU_CASE_PASS case=outside_and_cached_pso oracle=9,0,48\n", marker);
  }
  // Zero-vertex and zero-instance direct draws must not count a primitive,
  // regardless of the other (large) argument.
  {
    auto q = queryHeap(f.device.Get(), 2);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l); f.bindGraphics(l);
    l->BeginQuery(q.Get(), type, 0); l->DrawInstanced(0, 1000000, 0, 0); l->EndQuery(q.Get(), type, 0);
    l->BeginQuery(q.Get(), type, 1); l->DrawInstanced(1000000, 0, 0, 0); l->EndQuery(q.Get(), type, 1);
    l->ResolveQueryData(q.Get(), type, 0, 2, out.Get(), 0);
    f.finish(l); f.submit(r);
    require(f.oracle() == std::array<UINT, 3>{0, 0, 0}, "zero draws shader oracle");
    require(readStats(out.Get(), 0) == Stats{} && readStats(out.Get(), 88) == Stats{},
            "zero direct draw counted pipeline work");
    std::printf("%s_GPU_CASE_PASS case=zero_direct_draws oracle=0,0,0\n", marker);
  }
}

void lifecycleQueuesAndRetirement(Fixture &f) {
  constexpr auto type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
  // Two independent direct queues, query heaps, outputs, and UAV counters:
  // completion may occur in either order without sharing query state.
  struct Lane {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> query;
    ComPtr<ID3D12Resource> entries, zero, readback, result;
    Recording recording;
    HANDLE event = nullptr;
  } lanes[2];
  D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  for (UINT i = 0; i < 2; ++i) {
    auto &lane = lanes[i];
    check(f.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&lane.queue)), "isolation queue");
    check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lane.fence)), "isolation fence");
    lane.query = queryHeap(f.device.Get(), 1);
    lane.entries = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    lane.zero = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    lane.readback = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    lane.result = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void *mapped = nullptr; check(lane.zero->Map(0, nullptr, &mapped), "isolation clear map");
    std::memset(mapped, 0, 256); lane.zero->Unmap(0, nullptr);
    lane.recording = recording(f.device.Get()); auto *l = lane.recording.list.Get();
    l->CopyBufferRegion(lane.entries.Get(), 0, lane.zero.Get(), 0, 256);
    transition(l, lane.entries.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(f.shader.root.Get()); l->SetPipelineState(f.shader.compute.Get());
    l->SetComputeRootUnorderedAccessView(2, lane.entries->GetGPUVirtualAddress());
    l->BeginQuery(lane.query.Get(), type, 0); l->Dispatch(i + 1, 1, 1); l->EndQuery(lane.query.Get(), type, 0);
    l->ResolveQueryData(lane.query.Get(), type, 0, 1, lane.result.Get(), 0);
    transition(l, lane.entries.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyBufferRegion(lane.readback.Get(), 0, lane.entries.Get(), 0, 256);
    check(l->Close(), "isolation list close");
    lane.event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(lane.event != nullptr, "isolation event");
    ID3D12CommandList *lists[]{l}; lane.queue->ExecuteCommandLists(1, lists);
    check(lane.queue->Signal(lane.fence.Get(), 1), "isolation signal");
  }
  for (UINT i = 0; i < 2; ++i) {
    auto &lane = lanes[i];
    if (lane.fence->GetCompletedValue() < 1) {
      check(lane.fence->SetEventOnCompletion(1, lane.event), "isolation event completion");
      require(WaitForSingleObject(lane.event, kTimeout) == WAIT_OBJECT_0, "isolation fence timeout");
    }
    CloseHandle(lane.event);
    D3D12_RANGE range{0, 4}; void *mapped = nullptr;
    check(lane.readback->Map(0, &range, &mapped), "isolation oracle map");
    const UINT count = *static_cast<const UINT *>(mapped);
    D3D12_RANGE written{0, 0}; lane.readback->Unmap(0, &written);
    Stats wanted{}; wanted[10] = 8 * (i + 1);
    require(count == wanted[10], "independent queue CS oracle");
    compare(i ? "queue_isolation_second" : "queue_isolation_first", readStats(lane.result.Get(), 0),
            wanted, {0, 0, count});
  }
  // Gate execution on a fence, submit, release list/allocator/query before
  // GPU retirement, then unblock from the CPU. Resolved data remains valid.
  {
    auto gate = ComPtr<ID3D12Fence>{};
    check(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "retirement gate");
    auto q = queryHeap(f.device.Get(), 1);
    auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    l->BeginQuery(q.Get(), type, 0); f.dispatch(l); l->EndQuery(q.Get(), type, 0);
    l->ResolveQueryData(q.Get(), type, 0, 1, out.Get(), 0); f.finish(l);
    check(l->Close(), "retirement list close");
    check(f.queue->Wait(gate.Get(), 1), "retirement queue wait");
    ID3D12CommandList *lists[]{l}; f.queue->ExecuteCommandLists(1, lists);
    check(f.queue->Signal(f.fence.Get(), ++f.fenceValue), "retirement signal");
    q.Reset(); r.list.Reset(); r.allocator.Reset();
    check(gate->Signal(1), "retirement unblock");
    if (f.fence->GetCompletedValue() < f.fenceValue) {
      check(f.fence->SetEventOnCompletion(f.fenceValue, f.event), "retirement event");
      require(WaitForSingleObject(f.event, kTimeout) == WAIT_OBJECT_0, "retirement timeout");
    }
    compare("release_before_retirement", readStats(out.Get(), 0), kCompute, f.oracle());
  }
}

void indirectRootAndIndexed(Fixture &f) {
  constexpr auto type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
  static constexpr char producerSource[] = R"(
RWStructuredBuffer<uint> count : register(u2);
[numthreads(1,1,1)] void main() { count[0] = 2; }
)";
  auto producerCode = compile(producerSource, "main", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC producerDesc{};
  producerDesc.pRootSignature = f.shader.root.Get();
  producerDesc.CS = {producerCode->GetBufferPointer(), producerCode->GetBufferSize()};
  ComPtr<ID3D12PipelineState> producer;
  check(f.device->CreateComputePipelineState(&producerDesc, IID_PPV_ARGS(&producer)),
        "GPU indirect count producer");
  // Each packed command changes the compute root UAV before Dispatch. The
  // argument count is copied by the GPU, not supplied as a CPU execute count.
  struct RootDispatch { D3D12_GPU_VIRTUAL_ADDRESS uav; D3D12_DISPATCH_ARGUMENTS groups; UINT padding; };
  static_assert(sizeof(RootDispatch) == 24);
  {
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    arguments[0].UnorderedAccessView.RootParameterIndex = 2;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(RootDispatch); desc.NumArgumentDescs = 2; desc.pArgumentDescs = arguments;
    ComPtr<ID3D12CommandSignature> signature;
    check(f.device->CreateCommandSignature(&desc, f.shader.root.Get(), IID_PPV_ARGS(&signature)),
          "root UAV dispatch signature");
    auto q = queryHeap(f.device.Get(), 3);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto args = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    void *mapped = nullptr; check(upload->Map(0, nullptr, &mapped), "root indirect upload map");
    std::memset(mapped, 0, 256);
    auto *commands = static_cast<RootDispatch *>(mapped);
    commands[0] = {f.entries->GetGPUVirtualAddress() + 32, {1, 1, 1}, 0};
    commands[1] = {f.entries->GetGPUVirtualAddress() + 48, {2, 1, 1}, 0};
    auto *bytes = static_cast<uint8_t *>(mapped);
    *reinterpret_cast<UINT *>(bytes + 80) = 1;
    *reinterpret_cast<UINT *>(bytes + 84) = 0;
    *reinterpret_cast<UINT64 *>(bytes + 96) = 0;
    *reinterpret_cast<UINT64 *>(bytes + 104) = 1;
    upload->Unmap(0, nullptr);
    auto predicate = buffer(f.device.Get(), 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    l->CopyBufferRegion(args.Get(), 0, upload.Get(), 0, 128);
    l->CopyBufferRegion(predicate.Get(), 0, upload.Get(), 96, 16);
    transition(l, args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(f.shader.root.Get()); l->SetPipelineState(producer.Get());
    l->SetComputeRootUnorderedAccessView(2, args->GetGPUVirtualAddress() + 84);
    l->Dispatch(1, 1, 1);
    transition(l, args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    transition(l, predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
    l->SetComputeRootSignature(f.shader.root.Get()); l->SetPipelineState(f.shader.compute.Get());
    // Count one, count two, and predicated-off count two.
    for (UINT i = 0; i < 3; ++i) {
      if (i == 2) l->SetPredication(predicate.Get(), 8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
      l->BeginQuery(q.Get(), type, i);
      l->ExecuteIndirect(signature.Get(), 2, args.Get(), 0, args.Get(), i == 0 ? 80 : 84);
      l->EndQuery(q.Get(), type, i);
    }
    l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    l->ResolveQueryData(q.Get(), type, 0, 3, out.Get(), 0);
    f.finish(l); f.submit(r);
    auto oracle = f.oracle();
    require(oracle == std::array<UINT, 3>{0, 0, 16}, "indirect root UAV primary oracle");
    D3D12_RANGE range{48, 52}; check(f.entriesReadback->Map(0, &range, &mapped), "second root UAV oracle map");
    UINT second = *reinterpret_cast<const UINT *>(static_cast<const uint8_t *>(mapped) + 48);
    D3D12_RANGE written{0, 0}; f.entriesReadback->Unmap(0, &written);
    require(second == 16, "indirect root UAV second address oracle");
    Stats first{}; first[10] = 8; Stats both{}; both[10] = 24;
    require(readStats(out.Get(), 0) == first && readStats(out.Get(), 88) == both &&
            readStats(out.Get(), 176) == Stats{}, "root UAV indirect count/predicate statistics");
    std::printf("%s_GPU_CASE_PASS case=indirect_root_uav_count_predicate oracle=0,0,16,16\n", marker);
  }
  // DRAW_INDEXED dispatches through the indexed argument resolver and
  // participates in both GPU count and predication decisions.
  {
    D3D12_INDIRECT_ARGUMENT_DESC argument{}; argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument;
    ComPtr<ID3D12CommandSignature> signature;
    check(f.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "indexed signature");
    auto q = queryHeap(f.device.Get(), 3);
    auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto args = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto indices = buffer(f.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr; check(indices->Map(0, nullptr, &mapped), "indexed upload map");
    const UINT16 indexData[6]{0,1,2,3,4,5}; std::memcpy(mapped, indexData, sizeof(indexData));
    indices->Unmap(0, nullptr);
    check(upload->Map(0, nullptr, &mapped), "indexed args upload map");
    std::memset(mapped, 0, 128);
    auto *commands = static_cast<D3D12_DRAW_INDEXED_ARGUMENTS *>(mapped);
    commands[0] = {3,1,0,0,0}; commands[1] = {6,1,0,0,0};
    auto *bytes = static_cast<uint8_t *>(mapped);
    *reinterpret_cast<UINT *>(bytes + 64) = 1;
    *reinterpret_cast<UINT *>(bytes + 68) = 0;
    *reinterpret_cast<UINT64 *>(bytes + 80) = 1;
    upload->Unmap(0, nullptr);
    auto predicate = buffer(f.device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    l->CopyBufferRegion(args.Get(), 0, upload.Get(), 0, 72);
    l->CopyBufferRegion(predicate.Get(), 0, upload.Get(), 80, 8);
    transition(l, args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(f.shader.root.Get()); l->SetPipelineState(producer.Get());
    l->SetComputeRootUnorderedAccessView(2, args->GetGPUVirtualAddress() + 68);
    l->Dispatch(1, 1, 1);
    transition(l, args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    transition(l, predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
    f.bindGraphics(l, false);
    D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(), sizeof(indexData), DXGI_FORMAT_R16_UINT};
    l->IASetIndexBuffer(&ib);
    for (UINT i = 0; i < 3; ++i) {
      if (i == 2) l->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
      l->BeginQuery(q.Get(), type, i);
      l->ExecuteIndirect(signature.Get(), 2, args.Get(), 0, args.Get(), i == 0 ? 64 : 68);
      l->EndQuery(q.Get(), type, i);
    }
    l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    l->ResolveQueryData(q.Get(), type, 0, 3, out.Get(), 0);
    f.finish(l); f.submit(r);
    require(f.oracle() == std::array<UINT, 3>{12, 0, 0}, "indexed indirect shader oracle");
    Stats first{3,1,3,0,0,1,1,0,0,0,0};
    Stats both{9,3,9,0,0,3,3,0,0,0,0};
    require(readStats(out.Get(), 0) == first && readStats(out.Get(), 88) == both &&
            readStats(out.Get(), 176) == Stats{}, "indexed count/predicate statistics");
    std::printf("%s_GPU_CASE_PASS case=indirect_indexed_count_predicate oracle=12,0,0\n", marker);
  }
}

void indirectRootConstants(Fixture &f) {
  // The indirect signature changes both a root constant and a root UAV for
  // each dispatch; the constants govern which shader lanes mutate each UAV.
  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.ShaderRegister = 0;
  parameters[0].Constants.Num32BitValues = 1;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  for (UINT i = 1; i < 3; ++i) {
    parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[i].Descriptor.ShaderRegister = i;
    parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters = 3; rootDesc.pParameters = parameters;
  ComPtr<ID3DBlob> blob, errors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
        "serialize indirect constant root");
  ComPtr<ID3D12RootSignature> root;
  check(f.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "indirect constant root");
  static constexpr char source[] = R"(
cbuffer Params : register(b0) { uint lanes; }
RWStructuredBuffer<uint> counter : register(u2);
[numthreads(4,2,1)] void main(uint3 id : SV_GroupThreadID) {
  if (id.x < lanes) { uint prior; InterlockedAdd(counter[0], 1, prior); }
}
)";
  auto code = compile(source, "main", "cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = root.Get(); pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(f.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)),
        "indirect constant compute PSO");
  D3D12_INDIRECT_ARGUMENT_DESC arguments[3]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  arguments[0].Constant.RootParameterIndex = 0;
  arguments[0].Constant.Num32BitValuesToSet = 1;
  arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
  arguments[1].UnorderedAccessView.RootParameterIndex = 2;
  arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
  signatureDesc.ByteStride = 24; signatureDesc.NumArgumentDescs = 3;
  signatureDesc.pArgumentDescs = arguments;
  ComPtr<ID3D12CommandSignature> signature;
  check(f.device->CreateCommandSignature(&signatureDesc, root.Get(), IID_PPV_ARGS(&signature)),
        "indirect constant signature");
  #pragma pack(push, 1)
  struct Packed { UINT lanes; D3D12_GPU_VIRTUAL_ADDRESS uav; D3D12_DISPATCH_ARGUMENTS groups; };
  #pragma pack(pop)
  static_assert(sizeof(Packed) == 24);
  auto upload = buffer(f.device.Get(), 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto args = buffer(f.device.Get(), 48, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr; check(upload->Map(0, nullptr, &mapped), "constant indirect map");
  Packed commands[2]{{2, f.entries->GetGPUVirtualAddress() + 32, {1,1,1}},
                     {3, f.entries->GetGPUVirtualAddress() + 48, {1,1,1}}};
  std::memcpy(mapped, commands, sizeof(commands)); upload->Unmap(0, nullptr);
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  l->CopyBufferRegion(args.Get(), 0, upload.Get(), 0, sizeof(commands));
  transition(l, args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  l->SetComputeRootSignature(root.Get()); l->SetPipelineState(pipeline.Get());
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ExecuteIndirect(signature.Get(), 2, args.Get(), 0, nullptr, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 0);
  f.finish(l); f.submit(r);
  Stats wanted{}; wanted[10] = 16;
  require(readStats(out.Get(), 0) == wanted, "indirect root constant invocation count");
  D3D12_RANGE range{32, 52}; check(f.entriesReadback->Map(0, &range, &mapped), "constant root oracle map");
  auto *bytes = static_cast<const uint8_t *>(mapped);
  UINT first = *reinterpret_cast<const UINT *>(bytes + 32);
  UINT second = *reinterpret_cast<const UINT *>(bytes + 48);
  D3D12_RANGE written{0, 0}; f.entriesReadback->Unmap(0, &written);
  require(first == 4 && second == 6, "indirect constants did not change GPU shader output");
  std::printf("%s_GPU_CASE_PASS case=indirect_root_constant_uav cs=16 oracle=4,6\n", marker);
}

void overlap(Fixture &f) {
  auto a = queryHeap(f.device.Get(), 3), b = queryHeap(f.device.Get(), 2);
  auto out = buffer(f.device.Get(), 512, D3D12_HEAP_TYPE_READBACK,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  l->BeginQuery(a.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  f.draw(l);
  l->BeginQuery(b.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
  l->ClearState(nullptr); f.dispatch(l);
  l->EndQuery(b.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
  f.draw(l);
  l->EndQuery(a.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->BeginQuery(a.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
  l->EndQuery(a.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
  l->ResolveQueryData(a.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, out.Get(), 24);
  l->ResolveQueryData(b.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1, 1, out.Get(), 200);
  f.finish(l); f.submit(r);
  Stats outer{6,2,6,0,0,2,2,32,0,0,48};
  auto observed = f.oracle();
  require(observed == std::array<UINT, 3>{6,32,48}, "overlap shader entry oracle");
  require(readStats(out.Get(), 24) == outer, "overlap outer statistics");
  require(readStats(out.Get(), 112) == Stats{}, "overlap empty statistics");
  require(readStats(out.Get(), 200) == kCompute, "overlap inner statistics");
  std::printf("%s_GPU_CASE_PASS case=overlap slots=3 heaps=2 clear_state=1 empty=1\n", marker);
}
void conditional(Fixture &f, bool indirect, bool enabled, bool compute) {
  const char *name = indirect ? (compute ? (enabled ? "indirect_compute_one" : "indirect_compute_zero") :
                              (enabled ? "indirect_draw_two" : "indirect_draw_zero")) :
                              (enabled ? "predicate_true" : "predicate_false");
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto args = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr; check(upload->Map(0, nullptr, &mapped), "conditional upload map");
  std::memset(mapped, 0, 256);
  if (indirect) {
    if (compute) {
      auto *dispatch = static_cast<D3D12_DISPATCH_ARGUMENTS *>(mapped);
      *dispatch = {2, 3, 1};
    } else {
      auto *draw = static_cast<D3D12_DRAW_ARGUMENTS *>(mapped);
      draw[0] = {3, 1, 0, 0}; draw[1] = {6, 1, 0, 0};
    }
    // Count sits in the same GPU argument buffer at byte 64. Zero must skip
    // every command without changing any shader entry counter.
    *reinterpret_cast<UINT *>(static_cast<uint8_t *>(mapped) + 64) =
        enabled ? (compute ? 1 : 2) : 0;
  } else {
    *static_cast<UINT64 *>(mapped) = enabled ? 1 : 0;
  }
  upload->Unmap(0, nullptr);
  auto r = recording(f.device.Get()); auto *l = r.list.Get();
  f.prepare(l);
  l->CopyBufferRegion(args.Get(), 0, upload.Get(), 0, 256);
  transition(l, args.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             indirect ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT : D3D12_RESOURCE_STATE_PREDICATION);
  ComPtr<ID3D12CommandSignature> signature;
  if (indirect) {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = compute ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = compute ? sizeof(D3D12_DISPATCH_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument;
    check(f.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)),
          "CreateCommandSignature(statistics)");
  }
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  if (compute) {
    l->SetComputeRootSignature(f.shader.root.Get());
    l->SetPipelineState(f.shader.compute.Get());
    l->SetComputeRootUnorderedAccessView(2, f.entries->GetGPUVirtualAddress() + 32);
    l->ExecuteIndirect(signature.Get(), 1, args.Get(), 0, args.Get(), 64);
  } else if (indirect) {
    f.bindGraphics(l, false);
    l->ExecuteIndirect(signature.Get(), 2, args.Get(), 0, args.Get(), 64);
  } else {
    l->SetPredication(args.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    f.draw(l); f.dispatch(l);
    l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  }
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  f.finish(l); f.submit(r);
  Stats wanted{};
  if (enabled) {
    if (!indirect) wanted = kCombined;
    else if (compute) wanted = kCompute;
    else wanted = Stats{9,3,9,0,0,3,3,0,0,0,0};
  }
  compare(name, readStats(out.Get(), 24), wanted, f.oracle());
}
void strip(Fixture &f, bool restart) {
  const char *name = restart ? "indexed_strip_restart" : "indexed_strip_four";
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  const std::array<UINT16, 8> indices = restart ?
      std::array<UINT16, 8>{0,1,2,0xffff,3,4,5,0xffff} :
      std::array<UINT16, 8>{0,1,2,3,4,5,0xffff,0xffff};
  auto ib = buffer(f.device.Get(), sizeof(indices), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(ib->Map(0, nullptr, &mapped), "map index buffer");
  std::memcpy(mapped, indices.data(), sizeof(indices)); ib->Unmap(0, nullptr);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  f.bindGraphics(l, false);
  if (restart) l->SetPipelineState(f.shader.graphicsStripCut.Get());
  D3D12_INDEX_BUFFER_VIEW view{ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
  l->IASetIndexBuffer(&view);
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  l->DrawIndexedInstanced(restart ? 7 : 6, 1, 0, 0, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  f.finish(l); f.submit(r);
  Stats actual = readStats(out.Get(), 24);
  const auto oracle = f.oracle();
  require(actual[0] == 6 && actual[1] == (restart ? 2 : 4) &&
          actual[2] == oracle[0] && actual[2] >= 6 && actual[2] <= 12 &&
          actual[3] == 0 && actual[4] == 0 &&
          actual[5] == (restart ? 2 : 4) && actual[6] == (restart ? 2 : 4) &&
          actual[7] == 0 && actual[8] == 0 && actual[9] == 0 && actual[10] == 0 &&
          oracle[1] == 0 && oracle[2] == 0,
          "indexed strip/restart GPU statistics or independent VS oracle");
  std::printf("%s_GPU_CASE_PASS case=%s ia=%llu,%llu vs=%llu clip=%llu,%llu ps=0 cs=0 oracle=%u,0,0\n",
              marker, name, static_cast<unsigned long long>(actual[0]),
              static_cast<unsigned long long>(actual[1]),
              static_cast<unsigned long long>(actual[2]),
              static_cast<unsigned long long>(actual[5]),
              static_cast<unsigned long long>(actual[6]), oracle[0]);
}
void topology(Fixture &f, UINT mode) {
  const char *names[]{"nonindexed_instanced", "indexed_instanced", "incomplete_list", "degenerate_list"};
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto ib = buffer(f.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT16 *indices = nullptr;
  check(ib->Map(0, nullptr, reinterpret_cast<void **>(&indices)), "topology index map");
  indices[0] = 0; indices[1] = 1; indices[2] = 2;
  ib->Unmap(0, nullptr);
  std::array<float, 12> collapsed{-1,-1,.5f,1, -1,-1,.5f,1, -1,-1,.5f,1};
  auto degenerate = buffer(f.device.Get(), sizeof(collapsed), D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(degenerate->Map(0, nullptr, &mapped), "degenerate vertex map");
  std::memcpy(mapped, collapsed.data(), sizeof(collapsed)); degenerate->Unmap(0, nullptr);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  f.bindGraphics(l, false);
  if (mode == 1) {
    D3D12_INDEX_BUFFER_VIEW view{ib->GetGPUVirtualAddress(), 16, DXGI_FORMAT_R16_UINT};
    l->IASetIndexBuffer(&view);
    l->DrawIndexedInstanced(3, 2, 0, 0, 0);
  } else if (mode == 3) {
    D3D12_VERTEX_BUFFER_VIEW view{degenerate->GetGPUVirtualAddress(), sizeof(collapsed), 16};
    l->IASetVertexBuffers(0, 1, &view);
    l->DrawInstanced(3, 1, 0, 0);
  } else l->DrawInstanced(mode == 2 ? 2 : 3, mode == 0 ? 2 : 1, 0, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  f.finish(l); f.submit(r);
  Stats actual = readStats(out.Get(), 24);
  auto oracle = f.oracle();
  if (mode <= 1) compare(names[mode], actual, Stats{6,2,6,0,0,2,2,0,0,0,0}, oracle);
  else if (mode == 2) {
    require(actual[0] <= 2 && actual[1] == 0 && actual[2] == oracle[0] &&
            actual[2] <= 2 && actual[3] == 0 && actual[4] == 0 && actual[5] == 0 &&
            actual[6] == 0 && actual[7] == 0 && actual[8] == 0 && actual[9] == 0 &&
            actual[10] == 0 && oracle[1] == 0 && oracle[2] == 0,
            "incomplete triangle assembled a primitive or ran shader unexpectedly");
    std::printf("%s_GPU_CASE_PASS case=%s ia=%llu,0 vs=%llu clip=0,0 ps=0 cs=0\n",
                marker, names[mode], static_cast<unsigned long long>(actual[0]),
                static_cast<unsigned long long>(actual[2]));
  } else {
    require(actual[0] == 3 && actual[1] == 1 && actual[2] == oracle[0] && oracle[0] == 3 &&
            actual[3] == 0 && actual[4] == 0 && actual[5] == 1 && actual[6] <= 1 &&
            actual[7] == oracle[1] && oracle[1] == 0 && actual[8] == 0 && actual[9] == 0 &&
            actual[10] == 0 && oracle[2] == 0, "degenerate raster primitive semantics");
    std::printf("%s_GPU_CASE_PASS case=%s ia=3,1 vs=3 clip=1,%llu ps=0 cs=0\n",
                marker, names[mode], static_cast<unsigned long long>(actual[6]));
  }
}
void streamOutput(Fixture &f, bool raster, bool overflow) {
  const char *name = !raster ? "stream_no_raster" : overflow ? "stream_overflow" : "stream_raster";
  auto q = queryHeap(f.device.Get(), 1);
  D3D12_QUERY_HEAP_DESC soDesc{};
  soDesc.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS; soDesc.Count = 1; soDesc.NodeMask = 1;
  ComPtr<ID3D12QueryHeap> so;
  check(f.device->CreateQueryHeap(&soDesc, IID_PPV_ARGS(&so)), "SO statistics heap");
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto capture = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_STREAM_OUT);
  auto counter = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = buffer(f.device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(zero->Map(0, nullptr, &mapped), "SO counter map");
  std::memset(mapped, 0, 256); zero->Unmap(0, nullptr);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  l->CopyBufferRegion(counter.Get(), 0, zero.Get(), 0, 256);
  transition(l, counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->BeginQuery(so.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
  f.bindGraphics(l);
  l->SetPipelineState(raster ? f.shader.stream.Get() : f.shader.streamNoRaster.Get());
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view{capture->GetGPUVirtualAddress(),
                                        overflow ? 16ull : 256ull,
                                        counter->GetGPUVirtualAddress()};
  l->SOSetTargets(0, 1, &view);
  l->DrawInstanced(3, 1, 0, 0);
  l->EndQuery(so.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  l->ResolveQueryData(so.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, out.Get(), 112);
  f.finish(l); f.submit(r);
  Stats actual = readStats(out.Get(), 24);
  auto oracle = f.oracle();
  require(actual[0] == 3 && actual[1] == 1 && actual[2] == oracle[0] && oracle[0] == 3 &&
          actual[3] == 0 && actual[4] == 0 && actual[8] == 0 && actual[9] == 0 &&
          actual[10] == 0 && oracle[2] == 0 && actual[7] == oracle[1],
          "SO pipeline statistics fields and single VS execution");
  if (raster) require(actual[5] == 1 && actual[6] == 1 && actual[7] == 16,
                      "rasterizing SO triangle clip/PS counts");
  else require(actual[6] == 0 && actual[7] == 0,
               "SO no-raster must not equate written primitives with clip statistics");
  D3D12_RANGE range{112, 128}; check(out->Map(0, &range, &mapped), "SO query map");
  D3D12_QUERY_DATA_SO_STATISTICS soResult{};
  std::memcpy(&soResult, static_cast<uint8_t *>(mapped) + 112, sizeof(soResult));
  D3D12_RANGE written{0, 0}; out->Unmap(0, &written);
  require(soResult.PrimitivesStorageNeeded == 1 &&
          soResult.NumPrimitivesWritten == (overflow ? 0ull : 1ull),
          "SO overflow query disagrees with primitive storage");
  std::printf("%s_GPU_CASE_PASS case=%s ia=3,1 vs=3 clip=%llu,%llu ps=%llu so=%llu,%llu\n",
              marker, name, static_cast<unsigned long long>(actual[5]),
              static_cast<unsigned long long>(actual[6]),
              static_cast<unsigned long long>(actual[7]),
              static_cast<unsigned long long>(soResult.NumPrimitivesWritten),
              static_cast<unsigned long long>(soResult.PrimitivesStorageNeeded));
}
void clip(Fixture &f, const char *name, float z0, float z1, float z2,
          bool depthClip, UINT minPrimitives, UINT maxPrimitives) {
  auto q = queryHeap(f.device.Get(), 1);
  auto out = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<float, 12> points{-1,-1,z0,1, -1,3,z1,1, 3,-1,z2,1};
  auto vb = buffer(f.device.Get(), sizeof(points), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr; check(vb->Map(0, nullptr, &mapped), "clip vertex map");
  std::memcpy(mapped, points.data(), sizeof(points)); vb->Unmap(0, nullptr);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  l->BeginQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  f.bindGraphics(l);
  l->SetPipelineState(depthClip ? f.shader.graphics.Get() : f.shader.graphicsNoDepthClip.Get());
  D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), sizeof(points), 16};
  l->IASetVertexBuffers(0, 1, &view);
  l->DrawInstanced(3, 1, 0, 0);
  l->EndQuery(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(q.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out.Get(), 24);
  f.finish(l); f.submit(r);
  const Stats actual = readStats(out.Get(), 24);
  const auto oracle = f.oracle();
  require(actual[0] == 3 && actual[1] == 1 && actual[2] == oracle[0] && oracle[0] == 3 &&
          actual[3] == 0 && actual[4] == 0 && actual[5] == 1 &&
          actual[6] >= minPrimitives && actual[6] <= maxPrimitives &&
          actual[7] == oracle[1] && actual[8] == 0 && actual[9] == 0 &&
          actual[10] == 0 && oracle[2] == 0,
          "near/far clipping statistic differs from GPU entry oracle or output polygon");
  if (maxPrimitives == 0) require(oracle[1] == 0, "fully clipped triangle ran PS");
  std::printf("%s_GPU_CASE_PASS case=%s ia=3,1 vs=3 clip=%llu,%llu ps=%llu oracle=3,%u,0\n",
              marker, name, static_cast<unsigned long long>(actual[5]),
              static_cast<unsigned long long>(actual[6]),
              static_cast<unsigned long long>(actual[7]), oracle[1]);
}
int run(int argc, char **argv) {
  require(argc == 4, "requires VS PS CS DXIL inputs");
  auto dxilVS = shaderFile(argv[1]), dxilPS = shaderFile(argv[2]), dxilCS = shaderFile(argv[3]);
  static constexpr char hlsl[] = R"(
RWStructuredBuffer<uint> vertexEntries : register(u0);
RWStructuredBuffer<uint> pixelEntries : register(u1);
RWStructuredBuffer<uint> computeEntries : register(u2);
struct VI { float4 position : POSITION; };
struct VO { float4 position : SV_Position; };
VO vsMain(VI input) { uint old; InterlockedAdd(vertexEntries[0], 1, old);
  VO output; output.position = input.position; return output; }
float4 psMain(VO input) : SV_Target0 { uint old; InterlockedAdd(pixelEntries[0], 1, old);
  return float4(.25, .5, .75, 1); }
[numthreads(4,2,1)] void csMain(uint3 id : SV_DispatchThreadID) {
  uint old; InterlockedAdd(computeEntries[0], 1, old); }
)";
  auto vs = compile(hlsl, "vsMain", "vs_5_0");
  auto ps = compile(hlsl, "psMain", "ps_5_0");
  auto cs = compile(hlsl, "csMain", "cs_5_0");
  for (UINT path = 0; path < 2; ++path) {
    Fixture f;
    if (path == 0) f.init(vs->GetBufferPointer(), vs->GetBufferSize(),
                          ps->GetBufferPointer(), ps->GetBufferSize(),
                          cs->GetBufferPointer(), cs->GetBufferSize());
    else f.init(dxilVS.data(), dxilVS.size(), dxilPS.data(), dxilPS.size(),
                dxilCS.data(), dxilCS.size());
    const char *kind = path == 0 ? "dxbc" : "dxil";
    std::printf("%s_PATH shader=%s\n", marker, kind);
    validation(f);
    runCase(f, "combined", kCombined, true, true);
    runCase(f, "graphics", kDraw, true, false);
    runCase(f, "compute", kCompute, false, true);
    runCase(f, "empty", Stats{}, false, false);
    runCase(f, "clear_state", kCombined, true, true, true);
    Stats unrasterized = kDraw; unrasterized[7] = 0;
    runCase(f, "scissored", unrasterized, true, false, false, false);
    overlap(f);
    conditional(f, false, false, false);
    conditional(f, false, true, false);
    conditional(f, true, false, false);
    conditional(f, true, true, false);
    conditional(f, true, false, true);
    conditional(f, true, true, true);
    strip(f, false);
    strip(f, true);
    for (UINT mode = 0; mode < 4; ++mode) topology(f, mode);
    streamOutput(f, true, false);
    streamOutput(f, false, false);
    streamOutput(f, true, true);
    clip(f, "near_crossing", -.5f, .5f, .5f, true, 2, 2);
    clip(f, "far_crossing", 1.5f, .5f, .5f, true, 2, 2);
    clip(f, "near_rejected", -.5f, -.5f, -.5f, true, 0, 0);
    clip(f, "far_rejected", 1.5f, 1.5f, 1.5f, true, 0, 0);
    clip(f, "depth_clip_disabled", -.5f, -.5f, -.5f, false, 1, 1);
    lifecycleAndIndirect(f);
    lifecycleQueuesAndRetirement(f);
    indirectRootAndIndexed(f);
    indirectRootConstants(f);
    stageBoundaries(f, path == 1);
    std::printf("%s_MATRIX_PASS shader=%s cases=%u fields=11\n", marker, kind,
                path == 0 ? 168 : 178);
  }
  std::printf("%s_PASS shaders=dxbc,dxil fields=11 independent_gpu_oracle=1\n", marker);
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  return run(argc, argv);
}
