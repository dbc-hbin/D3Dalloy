// Mesh shader GPU fixture (D3D12 MeshShaderTier 1): every entry of tests/dxil/mesh-shader.hlsl runs on the
// owned D3D12 device through pipeline-state streams and DispatchMesh / ExecuteIndirect(DISPATCH_MESH), and every
// case compares exact pixels and counters with an independent CPU expectation.
// usage: owned-d3d12-mesh-shader.exe   (frozen mesh-shader-<entry>.dxil files beside the exe)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#include "owned-d3d12-early-depth-oracle.inc"

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";
constexpr DWORD kFenceTimeoutMs = 120000;

[[noreturn]] void die(const char *step, HRESULT hr = E_FAIL) {
  std::printf("OWNED_D3D12_MESH_SHADER_FAIL step=%s hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
  std::fflush(stdout);
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    die(step, hr);
}

std::vector<uint8_t> shader(const char *entry) {
  std::wstring name = L"mesh-shader-";
  for (const char *c = entry; *c; ++c)
    name += static_cast<wchar_t>(*c);
  name += L".dxil";
  HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    die("CreateFileW(shader)", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
    CloseHandle(file);
    die("GetFileSizeEx(shader)");
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != data.size())
    die("ReadFile(shader)");
  return data;
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = type;
  props.CreationNodeMask = 1;
  props.VisibleNodeMask = 1;
  return props;
}

/* ---- case bookkeeping ---- */
int g_failures = 0;
int g_case_mismatches = 0;

void mismatch(const char *name, const char *what, size_t index, uint64_t actual, uint64_t expected) {
  if (g_case_mismatches++ < 32)
    std::printf("OWNED_D3D12_MESH_SHADER_MISMATCH case=%s field=%s index=%zu actual=0x%llx expected=0x%llx\n", name,
                what, index, static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
}

void begin() { g_case_mismatches = 0; }

void end(const char *name) {
  if (g_case_mismatches) {
    std::printf("OWNED_D3D12_MESH_SHADER_CASE_FAIL %s mismatches=%d\n", name, g_case_mismatches);
    ++g_failures;
  } else {
    std::printf("OWNED_D3D12_MESH_SHADER_CASE_PASS %s\n", name);
  }
  std::fflush(stdout);
}

#define EXPECT_EQ(name, what, index, actual, expected)                                                                 \
  do {                                                                                                                 \
    const uint64_t a_ = static_cast<uint64_t>(actual), e_ = static_cast<uint64_t>(expected);                          \
    if (a_ != e_)                                                                                                      \
      mismatch(name, what, index, a_, e_);                                                                             \
  } while (0)

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

/* One pipeline state stream subobject, pointer aligned as D3D12 requires. */
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Value value;
};

struct Target {
  UINT width = 64, height = 64, layers = 1, samples = 1;
};

/* Readback of one draw: RGBA8 pixels (layer-major) and the four u0 counters. */
struct Result {
  std::vector<uint32_t> pixels;
  std::array<uint32_t, 4> counters{};
  std::vector<uint32_t> words;
  HRESULT close = S_OK;
  uint32_t at(const Target &t, UINT x, UINT y, UINT layer = 0) const {
    return pixels[(size_t(layer) * t.height + y) * t.width + x];
  }
};

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Device2> device2;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  ComPtr<ID3D12RootSignature> root, rootMeshVisibility, rootDerivatives;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;

  explicit Gpu(ID3D12Device *d) : device(d) {
    check(device.As(&device2), "QueryInterface(ID3D12Device2)");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "CreateCommandList(ID3D12GraphicsCommandList6)");
    check(list->Close(), "CommandList::Close(initial)");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap(RTV)");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      die("CreateEventW");
    root = rootSignature(D3D12_SHADER_VISIBILITY_ALL);
    rootMeshVisibility = rootSignature(D3D12_SHADER_VISIBILITY_MESH);
    rootDerivatives = rootSignature(D3D12_SHADER_VISIBILITY_ALL, true);
  }
  ~Gpu() { CloseHandle(event); }

  /* b0 = 4 root constants (visible to `constants`), u0 = root UAV, t0 = root SRV (all stages). */
  ComPtr<ID3D12RootSignature> rootSignature(D3D12_SHADER_VISIBILITY constants, bool derivatives = false) {
    D3D12_ROOT_PARAMETER1 params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 4};
    params[0].ShaderVisibility = constants;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
                                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, 0};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &range};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].MaxLOD = 3;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].ShaderRegister = 1;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = derivatives ? 4 : 3;
    desc.Desc_1_1.pParameters = params;
    desc.Desc_1_1.NumStaticSamplers = derivatives ? 2 : 0;
    desc.Desc_1_1.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(hr)) {
      if (error)
        std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stdout);
      die("D3D12SerializeVersionedRootSignature", hr);
    }
    ComPtr<ID3D12RootSignature> out;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)),
          "CreateRootSignature");
    return out;
  }

  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    const auto props = heapProperties(type);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(buffer)");
    return resource;
  }

  ComPtr<ID3D12Resource> upload(const void *data, size_t size) {
    auto resource = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(resource->Map(0, nullptr, &mapped), "Map(upload)");
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
    return resource;
  }

  ComPtr<ID3D12Resource> texture(const Target &t, bool render_target) {
    const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = t.width;
    desc.Height = t.height;
    desc.DepthOrArraySize = static_cast<UINT16>(t.layers);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = render_target ? t.samples : 1;
    desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                          render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                        : D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                          render_target ? &clear : nullptr, IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(texture)");
    return resource;
  }

  void transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
  }

  void wait() {
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "Queue::Signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "Fence::SetEventOnCompletion");
      if (WaitForSingleObject(event, kFenceTimeoutMs) != WAIT_OBJECT_0)
        die("GPU fence timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
  }

  /*
   * Records `draw` into a cleared target (4 viewports/scissors of width/4 when `viewports` = 4), resolves an MSAA
   * target, and reads back every layer and the u0 counters. A failed Close is returned in Result::close.
   */
  Result render(ID3D12PipelineState *pso, ID3D12RootSignature *rootsig, const Target &t,
                std::array<uint32_t, 4> constants, UINT viewports,
                const std::function<void(ID3D12GraphicsCommandList6 *)> &draw, UINT counter_words = 4) {
    check(allocator->Reset(), "Allocator::Reset");
    // A list whose Close failed (the reject cases) cannot be reset; such a list is replaced.
    if (FAILED(list->Reset(allocator.Get(), nullptr))) {
      list.Reset();
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&list)),
            "CreateCommandList(after failed Close)");
    }
    auto target = texture(t, true);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (t.samples > 1) {
      rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      rtvDesc.Texture2DArray.ArraySize = t.layers;
    }
    const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), &rtvDesc, rtv);
    const std::vector<uint32_t> zero(counter_words);
    const UINT64 counter_bytes = UINT64(counter_words) * 4;
    auto counters = buffer(counter_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto counters_init = upload(zero.data(), size_t(counter_bytes));
    list->CopyBufferRegion(counters.Get(), 0, counters_init.Get(), 0, counter_bytes);
    transition(counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float clear[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(rootsig);
    list->SetGraphicsRoot32BitConstants(0, 4, constants.data(), 0);
    list->SetGraphicsRootUnorderedAccessView(1, counters->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(2, counters_init->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp[4];
    D3D12_RECT sc[4];
    const UINT vw = t.width / viewports;
    for (UINT i = 0; i < viewports; ++i) {
      vp[i] = {static_cast<float>(i * vw), 0, static_cast<float>(vw), static_cast<float>(t.height), 0, 1};
      sc[i] = {static_cast<LONG>(i * vw), 0, static_cast<LONG>((i + 1) * vw), static_cast<LONG>(t.height)};
    }
    list->RSSetViewports(viewports, vp);
    list->RSSetScissorRects(viewports, sc);
    draw(list.Get());

    ComPtr<ID3D12Resource> source = target;
    if (t.samples > 1) {
      auto resolved = texture(t, false);
      transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      transition(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      source = resolved;
    } else {
      transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    const UINT pitch = (t.width * 4 + 255) & ~255u;
    const UINT64 layer_bytes = UINT64(pitch) * t.height;
    auto readback = buffer(layer_bytes * t.layers, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT layer = 0; layer < t.layers; ++layer) {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint.Offset = layer_bytes * layer;
      dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, t.width, t.height, 1, pitch};
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = source.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = layer;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto counters_readback = buffer(counter_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(counters_readback.Get(), 0, counters.Get(), 0, counter_bytes);
    Result result;
    result.close = list->Close();
    if (FAILED(result.close))
      return result;
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait();
    void *mapped = nullptr;
    check(readback->Map(0, nullptr, &mapped), "Map(readback)");
    result.pixels.resize(size_t(t.width) * t.height * t.layers);
    for (UINT layer = 0; layer < t.layers; ++layer)
      for (UINT y = 0; y < t.height; ++y)
        std::memcpy(&result.pixels[(size_t(layer) * t.height + y) * t.width],
                    static_cast<const uint8_t *>(mapped) + layer_bytes * layer + size_t(pitch) * y, t.width * 4);
    readback->Unmap(0, nullptr);
    check(counters_readback->Map(0, nullptr, &mapped), "Map(counters)");
    std::memcpy(result.counters.data(), mapped, 16);
    if (counter_words > 4) {
      result.words.resize(counter_words);
      std::memcpy(result.words.data(), mapped, size_t(counter_bytes));
    }
    counters_readback->Unmap(0, nullptr);
    return result;
  }
};

/* When set, meshPso hands its stream (and the AS/MS bytecode it holds) to this instead of CreatePipelineState:
 * the PSO cache cases store, load or re-create from the very same stream. */
std::function<HRESULT(const D3D12_PIPELINE_STATE_STREAM_DESC &, const D3D12_SHADER_BYTECODE &,
                      const D3D12_SHADER_BYTECODE &, ComPtr<ID3D12PipelineState> &)>
    g_stream_consumer;
/* The CACHED_PSO subobject of every meshPso stream (empty unless a cache case sets it). */
D3D12_CACHED_PIPELINE_STATE g_cached_pso{};

/* A mesh pipeline from a stream: optional AS, MS, PS (all by entry name); `views` adds a VIEW_INSTANCING
 * subobject with that many views at locations {0, 0}. */
HRESULT meshPso(Gpu &gpu, ID3D12RootSignature *root, const char *as, const char *ms, const char *ps, UINT samples,
                ComPtr<ID3D12PipelineState> &pso, const char *vs = nullptr, UINT views = 0) {
  const auto as_code = as ? shader(as) : std::vector<uint8_t>{};
  const auto ms_code = ms ? shader(ms) : std::vector<uint8_t>{};
  const auto ps_code = shader(ps);
  const auto vs_code = vs ? shader(vs) : std::vector<uint8_t>{};
  D3D12_VIEW_INSTANCE_LOCATION locations[4]{};
  D3D12_VIEW_INSTANCING_DESC view_desc{views, locations, D3D12_VIEW_INSTANCING_FLAG_NONE};
  D3D12_RASTERIZER_DESC raster{};
  raster.FillMode = D3D12_FILL_MODE_SOLID;
  raster.CullMode = D3D12_CULL_MODE_NONE;
  raster.DepthClipEnable = TRUE;
  D3D12_DEPTH_STENCIL_DESC depth{};
  D3D12_RT_FORMAT_ARRAY formats{};
  formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  formats.NumRenderTargets = 1;
  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE> cached;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
  } stream{{.value = root},
           {.value = {as_code.data(), as_code.size()}},
           {.value = {ms_code.data(), ms_code.size()}},
           {.value = {vs_code.data(), vs_code.size()}},
           {.value = {ps_code.data(), ps_code.size()}},
           {.value = raster},
           {.value = depth},
           {.value = formats},
           {.value = {samples, 0}},
           {.value = g_cached_pso},
           {.value = view_desc}};
  // Without views the stream ends before the VIEW_INSTANCING subobject.
  const size_t size = views ? sizeof(stream) : offsetof(decltype(stream), views);
  const D3D12_PIPELINE_STATE_STREAM_DESC desc{size, &stream};
  pso.Reset();
  if (g_stream_consumer)
    return g_stream_consumer(desc, stream.as.value, stream.ms.value, pso);
  return gpu.device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
}

ComPtr<ID3D12PipelineState> requirePso(Gpu &gpu, ID3D12RootSignature *root, const char *as, const char *ms,
                                       const char *ps, UINT samples = 1) {
  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = meshPso(gpu, root, as, ms, ps, samples, pso);
  if (FAILED(hr)) {
    std::printf("OWNED_D3D12_MESH_SHADER_PSO_FAIL ms=%s hr=0x%08lx\n", ms, static_cast<unsigned long>(hr));
    ++g_case_mismatches;
  }
  return pso;
}

bool closed(const char *name, const Result &r) {
  if (SUCCEEDED(r.close))
    return true;
  mismatch(name, "Close", 0, static_cast<uint32_t>(r.close), 0);
  return false;
}

/* ms_basic: a full-target quad; red follows x, the other channels are constant. */
void testBasic(Gpu &gpu, UINT samples) {
  const char *name = samples > 1 ? "ms_basic_msaa4" : "ms_basic";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_basic", "ps_color", samples);
  if (pso) {
    const Target t{64, 64, 1, samples};
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, r))
      for (UINT y = 0; y < t.height; ++y)
        for (UINT x = 0; x < t.width; ++x) {
          const uint32_t p = r.at(t, x, y);
          EXPECT_EQ(name, "gba", y * t.width + x, p & 0xffffff00u, rgba(0, 64, 191, 255));
          // Red interpolates corner.x: (x + 0.5) / 64, within one unorm step of the exact value.
          const int red = int(p & 0xff), expected = int((x + 0.5) / 64.0 * 255.0 + 0.5);
          if (red < expected - 1 || red > expected + 1)
            mismatch(name, "red", y * t.width + x, red, expected);
        }
  }
  end(name);
}

/* ms_grid: group (x, y, z) draws tile (x, y); later groups (z = 1) overwrite earlier ones in D3D group order. */
void checkGrid(const char *name, const Result &r, uint32_t blue) {
  const Target t;
  if (!closed(name, r))
    return;
  for (UINT y = 0; y < 64; ++y)
    for (UINT x = 0; x < 64; ++x)
      EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y), rgba(x / 8, y / 8, blue, 255));
}

void testGrid(Gpu &gpu) {
  // b0 is visible to the mesh stage only (D3D12_SHADER_VISIBILITY_MESH).
  begin();
  auto pso = requirePso(gpu, gpu.rootMeshVisibility.Get(), nullptr, "ms_grid", "ps_color");
  if (pso) {
    checkGrid("ms_grid", gpu.render(pso.Get(), gpu.rootMeshVisibility.Get(), {}, {3, 0, 0, 0}, 1,
                                    [](auto *l) { l->DispatchMesh(8, 8, 2); }),
              4);
  }
  end("ms_grid");
}

/* as_main + ms_payload: AS group r launches one MS group per even-parity tile of row r through a 16 KiB payload. */
void testPayload(Gpu &gpu) {
  const char *name = "as_payload";
  begin();
  for (UINT samples : {1u,4u}) {
    auto pso = requirePso(gpu, gpu.root.Get(), "as_main", "ms_payload", "ps_color", samples);
    if (!pso) continue;
    const Target t{64,64,1,samples};
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 1, [](auto *l) { l->DispatchMesh(8, 1, 1); });
    if (closed(name, r)) {
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x) {
          const UINT tile = x / 8, row = y / 8;
          EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y),
                    ((tile + row) & 1) ? 0u : rgba(tile, row, 255, 255));
        }
      EXPECT_EQ(name, "as_threads", 0, r.counters[0], 8 * 32);
      EXPECT_EQ(name, "ms_groups", 1, r.counters[1], 8 * 4);
    }
  }
  end(name);
}

/* ms_cull: SV_CullPrimitive culls the two triangles of every odd tile of row 0. */
void testCull(Gpu &gpu, UINT samples) {
  const char *name = samples > 1 ? "ms_cull_msaa4" : "ms_cull";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_cull", "ps_color", samples);
  if (pso) {
    const Target t{64, 64, 1, samples};
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, r))
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x)
          EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y),
                    y < 8 && !((x / 8) & 1) ? rgba(0, 255, 0, 255) : 0u);
  }
  end(name);
}

/* ms_prim + ps_prim: nointerpolation per-primitive color and SV_PrimitiveID = 100 + primitive. */
void testPrimitiveAttributes(Gpu &gpu) {
  const char *name = "ms_primitive_attributes";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_prim", "ps_prim");
  if (pso) {
    const Target t;
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, r))
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x) {
          if (y < 8 || y >= 16) {
            EXPECT_EQ(name, "outside", y * 64 + x, r.at(t, x, y), 0u);
            continue;
          }
          const UINT lx = x % 8, ly = y % 8, sum = lx + ly + 1;
          if (sum == 8)
            continue; // on the shared diagonal edge
          const UINT prim = (x / 8) * 2 + (sum > 8 ? 1 : 0);
          EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y), rgba(prim, 128, prim, 255));
        }
  }
  end(name);
}

/* ms_layer + ps_layer: primitive i goes to render target array slice i & 3 and viewport i >> 2. */
void testLayerViewport(Gpu &gpu) {
  const char *name = "ms_layer_viewport";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_layer", "ps_layer");
  if (pso) {
    const Target t{64, 16, 4, 1};
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 4, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, r))
      for (UINT layer = 0; layer < 4; ++layer)
        for (UINT y = 0; y < 16; ++y)
          for (UINT x = 0; x < 64; ++x) {
            const UINT vp = x / 16;
            EXPECT_EQ(name, "pixel", (layer * 16 + y) * 64 + x, r.at(t, x, y, layer),
                      rgba(layer + 4 * vp, layer, vp, 255));
          }
  }
  end(name);
}

/* ms_lines: line topology, line i on pixel row 2 + 4 i. */
void testLines(Gpu &gpu) {
  const char *name = "ms_lines";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_lines", "ps_color");
  if (pso) {
    const Target t;
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, r))
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 1; x < 63; ++x) {
          const bool on = y < 32 && y % 4 == 2;
          EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y), on ? rgba(y / 4, 255, 0, 255) : 0u);
        }
  }
  end(name);
}

/* ms_max: 128 threads, 256 vertices and 256 primitives from groupshared staging. */
void testMax(Gpu &gpu) {
  const char *name = "ms_max_256";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_max", "ps_max");
  if (pso) {
    const Target t;
    auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, r))
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x) {
          const UINT cx = x / 4, cy = y / 4, cell = cy * 15 + cx;
          if (cx >= 15 || cell >= 128) {
            EXPECT_EQ(name, "outside", y * 64 + x, r.at(t, x, y), 0u);
            continue;
          }
          const UINT sum = x % 4 + y % 4 + 1;
          if (sum == 4)
            continue; // on the shared diagonal edge
          EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y), rgba(cell, sum > 4 ? 1 : 0, 255, 255));
        }
  }
  end(name);
}

/* ms_partial + ps_rows: a dynamically indexed, partially written output row (threadgroup staging) and
 * SV_ClipDistance clipping the right half. */
void testPartialRows(Gpu &gpu) {
  const char *name = "ms_partial_clip";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_partial", "ps_rows");
  if (pso) {
    const Target t;
    for (uint32_t row : {0u, 1u}) {
      auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {row, 0, 0, 0}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
      if (!closed(name, r))
        continue;
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x)
          EXPECT_EQ(name, "pixel", row * 4096 + y * 64 + x, r.at(t, x, y), x < 32 ? rgba(128, 64, 255, 255) : 0u);
    }
  }
  end(name);
}

/* ExecuteIndirect(DISPATCH_MESH): the count buffer limits the slots; an empty slot launches nothing. */
void testIndirect(Gpu &gpu) {
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_grid", "ps_color");
  D3D12_INDIRECT_ARGUMENT_DESC argument{};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
  D3D12_COMMAND_SIGNATURE_DESC desc{};
  desc.ByteStride = 16;
  desc.NumArgumentDescs = 1;
  desc.pArgumentDescs = &argument;
  ComPtr<ID3D12CommandSignature> signature;
  HRESULT hr = gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature));
  EXPECT_EQ("ms_indirect", "CreateCommandSignature", 0, static_cast<uint32_t>(hr), 0);
  if (pso && signature) {
    // Slot 0: z = 2 (b = 4), slot 1: empty, slot 2: z = 1 (b = 3).
    const uint32_t args[12] = {8, 8, 2, 0, 0, 1, 1, 0, 8, 8, 1, 0};
    for (uint32_t count : {1u, 3u}) {
      auto arguments = gpu.upload(args, sizeof(args));
      auto count_buffer = gpu.upload(&count, sizeof(count));
      const char *name = count == 1 ? "ms_indirect_count1" : "ms_indirect_count3";
      checkGrid(name, gpu.render(pso.Get(), gpu.root.Get(), {}, {3, 0, 0, 0}, 1, [&](auto *l) {
        l->ExecuteIndirect(signature.Get(), 3, arguments.Get(), 0, count_buffer.Get(), 0);
      }), count == 1 ? 4 : 3);
      if (count == 3) {
        // One query spans both nonempty command slots and the empty slot. Predication must
        // suppress all three; otherwise visibility accumulates 2 + 1 full-target layers.
        ComPtr<ID3D12QueryHeap> query;
        const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
        check(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query)), "CreateQueryHeap");
        auto visibility = gpu.buffer(8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        const uint64_t predicates[] = {0, 1};
        auto predicate_upload = gpu.upload(predicates, sizeof(predicates));
        auto predicate_buffer = gpu.buffer(8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_PREDICATION);
        for (uint64_t predicate : predicates) {
          auto r = gpu.render(pso.Get(), gpu.root.Get(), {}, {3, 0, 0, 0}, 1, [&](auto *l) {
            gpu.transition(predicate_buffer.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyBufferRegion(predicate_buffer.Get(), 0, predicate_upload.Get(), predicate * sizeof(uint64_t), 8);
            gpu.transition(predicate_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
            l->BeginQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            l->SetPredication(predicate_buffer.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            l->ExecuteIndirect(signature.Get(), 3, arguments.Get(), 0, count_buffer.Get(), 0);
            l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            l->EndQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            l->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, visibility.Get(), 0);
          });
          if (!closed(name, r))
            continue;
          if (predicate)
            checkGrid(name, r, 3);
          else
            for (size_t i = 0; i < r.pixels.size(); ++i)
              EXPECT_EQ(name, "predicated_pixel", i, r.pixels[i], 0);
          void *mapped = nullptr;
          check(visibility->Map(0, nullptr, &mapped), "Map(visibility)");
          uint64_t visible = 0;
          std::memcpy(&visible, mapped, sizeof(visible));
          visibility->Unmap(0, nullptr);
          const uint64_t expected = predicate ? 3 * 64 * 64 : 0;
          std::printf("OWNED_D3D12_MESH_SHADER_INDIRECT_OCCLUSION predicate=%llu visible=%llu expected=%llu\n",
                      static_cast<unsigned long long>(predicate), static_cast<unsigned long long>(visible),
                      static_cast<unsigned long long>(expected));
          EXPECT_EQ(name, "occlusion", predicate, visible, expected);
        }
      }
    }
    // AS + MS through the indirect path: 4 AS groups each launch 2 x 3 MS groups.
    auto count_pso = requirePso(gpu, gpu.root.Get(), "as_count", "ms_count", "ps_color");
    if (count_pso) {
      const uint32_t as_args[4] = {4, 1, 1, 0};
      auto arguments = gpu.upload(as_args, sizeof(as_args));
      auto r = gpu.render(count_pso.Get(), gpu.root.Get(), {}, {2, 3, 1, 0}, 1, [&](auto *l) {
        l->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
      });
      if (closed("ms_indirect_as", r))
        EXPECT_EQ("ms_indirect_as", "ms_groups", 0, r.counters[0], 24);
    }
  }
  end("ms_indirect");
}

/* Grid limits: 65535 per dimension and 2^22 total groups, from the API and from one AS group. */
void testGridLimits(Gpu &gpu) {
  const char *name = "grid_limits";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_count", "ps_color");
  auto as_pso = requirePso(gpu, gpu.root.Get(), "as_count", "ms_count", "ps_color");
  struct Case {
    ID3D12PipelineState *pso;
    std::array<uint32_t, 3> grid, k, expected;
  };
  if (pso && as_pso) {
    const Case cases[] = {
        {pso.Get(), {65535, 1, 1}, {}, {65535, 1, 0}},
        {pso.Get(), {65535, 64, 1}, {}, {4194240, 64, 65535}},
        {pso.Get(), {65536, 1, 1}, {}, {0, 0, 0}},     // above the per-dimension limit: dropped
        {pso.Get(), {2048, 2049, 1}, {}, {0, 0, 0}},   // above 2^22 groups: dropped
        {as_pso.Get(), {1, 1, 1}, {65535, 64, 1}, {4194240, 64, 65535}},
    };
    for (size_t i = 0; i < std::size(cases); ++i) {
      const auto &c = cases[i];
      auto r = gpu.render(c.pso, gpu.root.Get(), {}, {c.k[0], c.k[1], c.k[2], 0}, 1,
                          [&](auto *l) { l->DispatchMesh(c.grid[0], c.grid[1], c.grid[2]); });
      if (!closed(name, r))
        continue;
      for (int j = 0; j < 3; ++j)
        EXPECT_EQ(name, "counter", i * 3 + j, r.counters[j], c.expected[j]);
      for (size_t p = 0; p < r.pixels.size(); ++p)
        EXPECT_EQ(name, "pixel", p, r.pixels[p], 0u);
    }
  }
  end(name);
}

/* Invalid pipelines and draws: each is rejected, nothing runs. */
void testRejects(Gpu &gpu) {
  const char *name = "rejects";
  begin();
  ComPtr<ID3D12PipelineState> pso;
  // AS without MS.
  EXPECT_EQ(name, "as_without_ms", 0, static_cast<uint32_t>(meshPso(gpu, gpu.root.Get(), "as_count", nullptr,
                                                                     "ps_color", 1, pso)),
            static_cast<uint32_t>(E_INVALIDARG));
  // MS next to a VS (the pixel shader stands in as an invalid VS blob; the stream is rejected before it is read).
  EXPECT_EQ(name, "ms_with_vs", 1, static_cast<uint32_t>(meshPso(gpu, gpu.root.Get(), nullptr, "ms_basic",
                                                                  "ps_color", 1, pso, "ps_color")),
            static_cast<uint32_t>(E_INVALIDARG));
  // A DrawInstanced on a mesh-shader pipeline fails the list.
  auto mesh = requirePso(gpu, gpu.root.Get(), nullptr, "ms_basic", "ps_color");
  if (mesh) {
    auto r = gpu.render(mesh.Get(), gpu.root.Get(), {}, {}, 1, [](auto *l) {
      l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      l->DrawInstanced(3, 1, 0, 0);
    });
    EXPECT_EQ(name, "draw_on_mesh_pso", 2, static_cast<uint32_t>(r.close), static_cast<uint32_t>(E_INVALIDARG));
  }
  end(name);
}

/* ---- pipeline statistics ---- */

/* Runs `draw` (via Gpu::render) inside a PIPELINE_STATISTICS1 query and a nested PIPELINE_STATISTICS query and
 * returns both resolved records. */
struct Statistics {
  D3D12_QUERY_DATA_PIPELINE_STATISTICS1 stats1{};
  D3D12_QUERY_DATA_PIPELINE_STATISTICS stats{};
  Result result;
};

Statistics withStatistics(Gpu &gpu, ID3D12PipelineState *pso, const Target &t, std::array<uint32_t, 4> constants,
                          const std::function<void(ID3D12GraphicsCommandList6 *)> &draw) {
  Statistics out;
  ComPtr<ID3D12QueryHeap> heap1, heap;
  D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, 1, 0};
  check(gpu.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap1)), "CreateQueryHeap(PIPELINE_STATISTICS1)");
  desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
  check(gpu.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)), "CreateQueryHeap(PIPELINE_STATISTICS)");
  auto readback = gpu.buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  out.result = gpu.render(pso, gpu.root.Get(), t, constants, 1, [&](ID3D12GraphicsCommandList6 *l) {
    l->BeginQuery(heap1.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    l->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    draw(l);
    l->EndQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    l->EndQuery(heap1.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    l->ResolveQueryData(heap1.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0, 1, readback.Get(), 0);
    l->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, readback.Get(), 128);
  });
  if (SUCCEEDED(out.result.close)) {
    void *mapped = nullptr;
    check(readback->Map(0, nullptr, &mapped), "Map(statistics)");
    std::memcpy(&out.stats1, mapped, sizeof(out.stats1));
    std::memcpy(&out.stats, static_cast<const uint8_t *>(mapped) + 128, sizeof(out.stats));
    readback->Unmap(0, nullptr);
  }
  return out;
}

/* Exact D3D12 counters of a mesh draw: IA/VS/GS/HS/DS/CS zero; both query layouts agree on the common fields. */
void checkStatistics(const char *name, const Statistics &s, uint64_t as, uint64_t ms, uint64_t prims,
                     uint64_t ps) {
  if (!closed(name, s.result))
    return;
  const uint64_t expected[14] = {0, 0, 0, 0, 0, prims, prims, ps, 0, 0, 0, as, ms, prims};
  const uint64_t *actual = &s.stats1.IAVertices;
  static const char *fields[14] = {"IAVertices",    "IAPrimitives", "VSInvocations", "GSInvocations", "GSPrimitives",
                                   "CInvocations",  "CPrimitives",  "PSInvocations", "HSInvocations", "DSInvocations",
                                   "CSInvocations", "ASInvocations", "MSInvocations", "MSPrimitives"};
  for (int i = 0; i < 14; ++i)
    EXPECT_EQ(name, fields[i], i, actual[i], expected[i]);
  const uint64_t *legacy = &s.stats.IAVertices;
  for (int i = 0; i < 11; ++i)
    EXPECT_EQ(name, fields[i], 100 + i, legacy[i], expected[i]);
}

void testStatistics(Gpu &gpu) {
  const char *name = "ms_statistics";
  begin();
  // ms_basic: 4 threads, 2 triangles covering 64x64 pixels.
  if (auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_basic", "ps_color"))
    checkStatistics(name, withStatistics(gpu, pso.Get(), {}, {}, [](auto *l) { l->DispatchMesh(1, 1, 1); }), 0, 4,
                    2, 64 * 64);
  // ms_cull: 32 threads, 16 primitives of which 8 are culled; 4 tiles of 8x8 pixels survive.
  if (auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_cull", "ps_color"))
    checkStatistics("ms_statistics_cull",
                    withStatistics(gpu, pso.Get(), {}, {}, [](auto *l) { l->DispatchMesh(1, 1, 1); }), 0, 32, 8,
                    4 * 64);
  // as_main + ms_payload: 8 AS groups of 32 threads, 32 MS groups of 4 threads with 2 triangles of 8x8 each.
  if (auto pso = requirePso(gpu, gpu.root.Get(), "as_main", "ms_payload", "ps_color"))
    checkStatistics("ms_statistics_as",
                    withStatistics(gpu, pso.Get(), {}, {}, [](auto *l) { l->DispatchMesh(8, 1, 1); }), 8 * 32,
                    32 * 4, 32 * 2, 32 * 64);
  // 4x MSAA: PSInvocations counts once per covered pixel and primitive (pixel-frequency PS). Each tile's diagonal
  // edge crosses 8 pixels whose samples are split between its two triangles: those pixels shade twice.
  if (auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_cull", "ps_color", 4))
    checkStatistics("ms_statistics_msaa4",
                    withStatistics(gpu, pso.Get(), {64, 64, 1, 4}, {},
                                   [](auto *l) { l->DispatchMesh(1, 1, 1); }),
                    0, 32, 8, 4 * 64 + 4 * 8);
  // ExecuteIndirect: ms_grid 8x8x2 from the argument buffer (128 groups of 4 threads, 2 triangles each).
  if (auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_grid", "ps_color")) {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    const D3D12_COMMAND_SIGNATURE_DESC desc{12, 1, &argument, 0};
    ComPtr<ID3D12CommandSignature> signature;
    check(gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "CreateCommandSignature");
    const uint32_t args[3] = {8, 8, 2};
    auto arguments = gpu.upload(args, sizeof(args));
    // The z = 1 layer overdraws z = 0: every pixel is shaded twice.
    checkStatistics("ms_statistics_indirect", withStatistics(gpu, pso.Get(), {}, {3, 0, 0, 0}, [&](auto *l) {
                      l->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
                    }),
                    0, 128 * 4, 128 * 2, 2 * 64 * 64);
  }
  end(name);
}

/* ---- forced-early-depth statistics under MSAA ----
 * ms_early with a forced-early-depth PS and a LESS_EQUAL depth test, 64x64 at 2x/4x/8x: PSInvocations counts once
 * per (pixel, primitive) with a sample passing the depth test (claim buffer). The occluder (the first group, depth
 * 0.25) and the counted triangles (depth 0.5, two overlapping pairs drawn twice: equal-depth overdraw) come from t0;
 * one DispatchMesh draws them all, so the occluder is counted too. Direct DispatchMesh and ExecuteIndirect with
 * GPU-written arguments and count ({groups}, {0}, then past the count) both equal the CPU oracle. */
void testStatisticsEarlyDepth(Gpu &gpu) {
  const char *name = "ms_statistics_early_depth_msaa";
  begin();
  using early_depth_oracle::Triangle;
  constexpr double g = 1.0 / 256; // vertices on the 1/256-pixel grid, every sample >= 0.0015 pixel off every edge
  const std::vector<Triangle> drawn = {
      {{{0, 0, 0.25}, {6244 * g, 0, 0.25}, {0, 13805 * g, 0.25}}},
      {{{0, 0, 0.25}, {1 * g, 0, 0.25}, {0, 1 * g, 0.25}}}, // covers no sample: the occluder group's second triangle
      {{{1674 * g, 1424 * g, 0.5}, {7409 * g, 2051 * g, 0.5}, {3159 * g, 7395 * g, 0.5}}},
      {{{4594 * g, 3244 * g, 0.5}, {10572 * g, 4952 * g, 0.5}, {5692 * g, 9834 * g, 0.5}}},
      {{{8843 * g, 7907 * g, 0.5}, {15109 * g, 9475 * g, 0.5}, {11090 * g, 15766 * g, 0.5}}},
      {{{1368 * g, 10980 * g, 0.5}, {6776 * g, 9320 * g, 0.5}, {4531 * g, 15252 * g, 0.5}}},
      {{{1674 * g, 1424 * g, 0.5}, {7409 * g, 2051 * g, 0.5}, {3159 * g, 7395 * g, 0.5}}},
      {{{4594 * g, 3244 * g, 0.5}, {10572 * g, 4952 * g, 0.5}, {5692 * g, 9834 * g, 0.5}}},
  };
  std::vector<float> words;
  for (const auto &t : drawn)
    for (const auto &v : t.v) {
      words.push_back(float(v.x));
      words.push_back(float(v.y));
      words.push_back(float(v.z));
    }
  const UINT groups = UINT(drawn.size() / 2);
  auto input = gpu.upload(words.data(), words.size() * 4);
  const auto ms_code = shader("ms_early"), ps_code = shader("ps_early");
  for (UINT samples : {2u, 4u, 8u}) {
    const uint64_t want = early_depth_oracle::ps_invocations(64, 64, samples, {}, drawn);
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.NumRenderTargets = 1;
    struct {
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depthFormat;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream{{.value = gpu.root.Get()},
             {.value = {ms_code.data(), ms_code.size()}},
             {.value = {ps_code.data(), ps_code.size()}},
             {.value = raster},
             {.value = depth},
             {.value = DXGI_FORMAT_D32_FLOAT},
             {.value = formats},
             {.value = {samples, 0}}};
    const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pso;
    check(gpu.device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "CreatePipelineState(ms_early)");
    const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 64;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.SampleDesc.Count = samples;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE colorClear{};
    colorClear.Format = td.Format;
    ComPtr<ID3D12Resource> color, depthTarget;
    check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              &colorClear, IID_PPV_ARGS(&color)),
          "CreateCommittedResource(color)");
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = td.Format;
    depthClear.DepthStencil.Depth = 1;
    check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                              &depthClear, IID_PPV_ARGS(&depthTarget)),
          "CreateCommittedResource(depth)");
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap(DSV)");
    const auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateDepthStencilView(depthTarget.Get(), nullptr, dsv);
    const auto rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    const D3D12_COMMAND_SIGNATURE_DESC sigDesc{12, 1, &argument, 0};
    ComPtr<ID3D12CommandSignature> signature;
    check(gpu.device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&signature)), "CreateCommandSignature");
    const uint32_t args[16] = {groups, 1, 1, 0, 1, 1, 0x10000, 1, 1, 0x10000, 1, 1, 2};
    auto argsUpload = gpu.upload(args, sizeof(args));
    for (bool indirect : {false, true}) {
      auto argsGpu = gpu.buffer(sizeof(args), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      ComPtr<ID3D12QueryHeap> heap;
      D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, 1, 0};
      check(gpu.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)), "CreateQueryHeap");
      auto readback = gpu.buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      check(gpu.allocator->Reset(), "Allocator::Reset");
      check(gpu.list->Reset(gpu.allocator.Get(), nullptr), "CommandList::Reset");
      auto *l = gpu.list.Get();
      l->CopyBufferRegion(argsGpu.Get(), 0, argsUpload.Get(), 0, sizeof(args));
      gpu.transition(argsGpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
      const float clear[4] = {};
      l->ClearRenderTargetView(rtv, clear, 0, nullptr);
      l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
      l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      const D3D12_VIEWPORT vp{0, 0, 64, 64, 0, 1};
      const D3D12_RECT sc{0, 0, 64, 64};
      l->RSSetViewports(1, &vp);
      l->RSSetScissorRects(1, &sc);
      l->SetPipelineState(pso.Get());
      l->SetGraphicsRootSignature(gpu.root.Get());
      l->SetGraphicsRootShaderResourceView(2, input->GetGPUVirtualAddress());
      l->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
      if (indirect)
        l->ExecuteIndirect(signature.Get(), 4, argsGpu.Get(), 0, argsGpu.Get(), 48);
      else
        l->DispatchMesh(groups, 1, 1);
      l->EndQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
      l->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0, 1, readback.Get(), 0);
      check(l->Close(), "CommandList::Close(ms_early)");
      ID3D12CommandList *lists[] = {l};
      gpu.queue->ExecuteCommandLists(1, lists);
      gpu.wait();
      D3D12_QUERY_DATA_PIPELINE_STATISTICS1 s{};
      void *mapped = nullptr;
      check(readback->Map(0, nullptr, &mapped), "Map(statistics)");
      std::memcpy(&s, mapped, sizeof(s));
      readback->Unmap(0, nullptr);
      std::printf("OWNED_D3D12_MESH_SHADER_EARLY_DEPTH samples=%u indirect=%d ps=%llu oracle=%llu\n", samples,
                  indirect ? 1 : 0, static_cast<unsigned long long>(s.PSInvocations),
                  static_cast<unsigned long long>(want));
      EXPECT_EQ(name, "PSInvocations", samples * 2 + indirect, s.PSInvocations, want);
      EXPECT_EQ(name, "MSPrimitives", samples * 2 + indirect, s.MSPrimitives, drawn.size());
      EXPECT_EQ(name, "MSInvocations", samples * 2 + indirect, s.MSInvocations, uint64_t(groups) * 6);
    }
  }
  end(name);
}

/* ---- predication ---- */

void testPredication(Gpu &gpu) {
  const char *name = "ms_predication";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_grid", "ps_color");
  D3D12_INDIRECT_ARGUMENT_DESC argument{};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
  const D3D12_COMMAND_SIGNATURE_DESC desc{12, 1, &argument, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "CreateCommandSignature");
  if (pso) {
    const uint32_t args[3] = {8, 8, 1};
    auto arguments = gpu.upload(args, sizeof(args));
    for (uint64_t value : {0ull, 1ull}) {
      auto predicate_upload = gpu.upload(&value, sizeof(value));
      auto predicate = gpu.buffer(8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      for (bool indirect : {false, true}) {
        // EQUAL_ZERO skips the draw when the predicate is 0.
        auto r = gpu.render(pso.Get(), gpu.root.Get(), {}, {3, 0, 0, 0}, 1, [&](ID3D12GraphicsCommandList6 *l) {
          l->CopyBufferRegion(predicate.Get(), 0, predicate_upload.Get(), 0, 8);
          gpu.transition(predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
          l->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
          if (indirect)
            l->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
          else
            l->DispatchMesh(8, 8, 1);
          l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
        const char *sub = indirect ? (value ? "ms_predication_indirect_true" : "ms_predication_indirect_false")
                                   : (value ? "ms_predication_direct_true" : "ms_predication_direct_false");
        if (value)
          checkGrid(sub, r, 3);
        else if (closed(sub, r))
          for (size_t p = 0; p < r.pixels.size(); ++p)
            EXPECT_EQ(sub, "pixel", p, r.pixels[p], 0u);
      }
    }
  }
  end(name);
}

/* ---- PSO cache: GetCachedBlob -> CachedPSO and a pipeline library Store/Load of an AS+MS PSO ---- */

void testPsoCache(Gpu &gpu) {
  const char *name = "ms_pso_cache";
  begin();
  auto check_payload = [&](const char *sub, ID3D12PipelineState *pso) {
    auto r = gpu.render(pso, gpu.root.Get(), {}, {}, 1, [](auto *l) { l->DispatchMesh(8, 1, 1); });
    if (!closed(sub, r))
      return;
    const Target t;
    for (UINT y = 0; y < 64; ++y)
      for (UINT x = 0; x < 64; ++x) {
        const UINT tile = x / 8, row = y / 8;
        EXPECT_EQ(sub, "pixel", y * 64 + x, r.at(t, x, y), ((tile + row) & 1) ? 0u : rgba(tile, row, 255, 255));
      }
    EXPECT_EQ(sub, "as_threads", 0, r.counters[0], 8 * 32);
    EXPECT_EQ(sub, "ms_groups", 1, r.counters[1], 8 * 4);
  };
  auto original = requirePso(gpu, gpu.root.Get(), "as_main", "ms_payload", "ps_color");
  if (original) {
    ComPtr<ID3DBlob> blob;
    HRESULT hr = original->GetCachedBlob(&blob);
    EXPECT_EQ(name, "GetCachedBlob", 0, static_cast<uint32_t>(hr), 0);
    if (blob) {
      g_cached_pso = {blob->GetBufferPointer(), blob->GetBufferSize()};
      ComPtr<ID3D12PipelineState> cached;
      hr = meshPso(gpu, gpu.root.Get(), "as_main", "ms_payload", "ps_color", 1, cached);
      g_cached_pso = {};
      EXPECT_EQ(name, "CreatePipelineState(CachedPSO)", 1, static_cast<uint32_t>(hr), 0);
      if (cached)
        check_payload("ms_pso_cache_cached", cached.Get());
    }
    ComPtr<ID3D12Device1> device1;
    check(gpu.device.As(&device1), "QueryInterface(ID3D12Device1)");
    ComPtr<ID3D12PipelineLibrary1> library;
    check(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)), "CreatePipelineLibrary");
    EXPECT_EQ(name, "StorePipeline", 2, static_cast<uint32_t>(library->StorePipeline(L"mesh", original.Get())), 0);
    std::vector<uint8_t> serialized(library->GetSerializedSize());
    EXPECT_EQ(name, "Serialize", 3,
              static_cast<uint32_t>(library->Serialize(serialized.data(), serialized.size())), 0);
    ComPtr<ID3D12PipelineLibrary1> loaded_library;
    check(device1->CreatePipelineLibrary(serialized.data(), serialized.size(), IID_PPV_ARGS(&loaded_library)),
          "CreatePipelineLibrary(serialized)");
    ComPtr<ID3D12PipelineState> loaded;
    g_stream_consumer = [&](const D3D12_PIPELINE_STATE_STREAM_DESC &desc, const D3D12_SHADER_BYTECODE &,
                            const D3D12_SHADER_BYTECODE &, ComPtr<ID3D12PipelineState> &out) {
      return loaded_library->LoadPipeline(L"mesh", &desc, IID_PPV_ARGS(&out));
    };
    hr = meshPso(gpu, gpu.root.Get(), "as_main", "ms_payload", "ps_color", 1, loaded);
    // A different MS than the stored one does not match the stored pipeline.
    ComPtr<ID3D12PipelineState> mismatched;
    const HRESULT mismatch_hr = meshPso(gpu, gpu.root.Get(), "as_main", "ms_count", "ps_color", 1, mismatched);
    g_stream_consumer = nullptr;
    EXPECT_EQ(name, "LoadPipeline", 4, static_cast<uint32_t>(hr), 0);
    EXPECT_EQ(name, "LoadPipeline(mismatch)", 5, static_cast<uint32_t>(mismatch_hr),
              static_cast<uint32_t>(E_INVALIDARG));
    if (loaded)
      check_payload("ms_pso_cache_library", loaded.Get());
  }
  end(name);
}

/* ---- view instancing: DispatchMesh replays per view; SV_ViewID in the MS ---- */

void testViewInstancing(Gpu &gpu) {
  const char *name = "ms_view_instancing";
  begin();
  ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = meshPso(gpu, gpu.root.Get(), nullptr, "ms_viewid", "ps_color", 1, pso, nullptr, 3);
  EXPECT_EQ(name, "CreatePipelineState", 0, static_cast<uint32_t>(hr), 0);
  if (pso) {
    // Additive blending is not in the PSO: the last view (2) wins at every pixel, so the target names it; the
    // statistics count all three views (ViewInstancingTier 1: each view is a draw).
    auto s = withStatistics(gpu, pso.Get(), {}, {}, [](auto *l) { l->DispatchMesh(1, 1, 1); });
    if (closed(name, s.result))
      for (size_t p = 0; p < s.result.pixels.size(); ++p)
        EXPECT_EQ(name, "pixel", p, s.result.pixels[p], rgba(3, 0, 255, 255));
    checkStatistics("ms_view_instancing_statistics", s, 0, 3 * 4, 3 * 2, 3 * 64 * 64);
    // The ExecuteIndirect path replays the whole buffer per view as well.
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    const D3D12_COMMAND_SIGNATURE_DESC desc{12, 1, &argument, 0};
    ComPtr<ID3D12CommandSignature> signature;
    check(gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)), "CreateCommandSignature");
    const uint32_t args[3] = {1, 1, 1};
    auto arguments = gpu.upload(args, sizeof(args));
    auto si = withStatistics(gpu, pso.Get(), {}, {}, [&](auto *l) {
      l->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
    });
    if (closed("ms_view_instancing_indirect", si.result))
      for (size_t p = 0; p < si.result.pixels.size(); ++p)
        EXPECT_EQ("ms_view_instancing_indirect", "pixel", p, si.result.pixels[p], rgba(3, 0, 255, 255));
    checkStatistics("ms_view_instancing_indirect_statistics", si, 0, 3 * 4, 3 * 2, 3 * 64 * 64);
  }
  end(name);
}

/* ---- argument-changing DISPATCH_MESH signatures: root constants per command ---- */

void testIndirectRootConstants(Gpu &gpu) {
  const char *name = "ms_indirect_root_constants";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_rootk", "ps_color");
  D3D12_INDIRECT_ARGUMENT_DESC arguments[3]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  arguments[0].Constant = {0, 0, 2};
  arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
  arguments[1].UnorderedAccessView.RootParameterIndex = 1;
  arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
  // 2 constants (8 B) + UAV address (8 B) + grid (12 B), padded to 32.
  const D3D12_COMMAND_SIGNATURE_DESC desc{32, 3, arguments, 0};
  ComPtr<ID3D12CommandSignature> signature;
  HRESULT hr = gpu.device->CreateCommandSignature(&desc, gpu.root.Get(), IID_PPV_ARGS(&signature));
  EXPECT_EQ(name, "CreateCommandSignature", 0, static_cast<uint32_t>(hr), 0);
  // A signature without the root signature is invalid when it changes root arguments.
  ComPtr<ID3D12CommandSignature> invalid;
  EXPECT_EQ(name, "CreateCommandSignature(no root)", 1,
            static_cast<uint32_t>(gpu.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&invalid))),
            static_cast<uint32_t>(E_INVALIDARG));
  if (pso && signature) {
    // Two other counters buffers receive the per-command UAV writes; the list's own u0 must stay 0.
    const uint32_t zero[4] = {};
    auto a = gpu.buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto b = gpu.buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto zeros = gpu.upload(zero, sizeof(zero));
    struct Command {
      uint32_t k[2];
      uint64_t uav;
      uint32_t grid[3];
      uint32_t pad;
    };
    static_assert(sizeof(Command) == 32);
    // Command i draws column k.x = 2 i + 1 with k.y = 10 + i, rows 0 .. 2 i (gid.x), into a / b alternately;
    // slot 3 is past the count buffer.
    const Command commands[4] = {{{1, 10}, a->GetGPUVirtualAddress(), {1, 1, 1}, 0},
                                 {{3, 11}, b->GetGPUVirtualAddress(), {3, 1, 1}, 0},
                                 {{5, 12}, a->GetGPUVirtualAddress(), {5, 1, 1}, 0},
                                 {{7, 13}, b->GetGPUVirtualAddress(), {7, 1, 1}, 0}};
    auto argument_buffer = gpu.upload(commands, sizeof(commands));
    const uint32_t count = 3;
    auto count_buffer = gpu.upload(&count, sizeof(count));
    auto readback = gpu.buffer(32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto r = gpu.render(pso.Get(), gpu.root.Get(), {}, {0, 99, 0, 0}, 1, [&](ID3D12GraphicsCommandList6 *l) {
      l->CopyBufferRegion(a.Get(), 0, zeros.Get(), 0, 16);
      l->CopyBufferRegion(b.Get(), 0, zeros.Get(), 0, 16);
      gpu.transition(a.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.transition(b.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      l->ExecuteIndirect(signature.Get(), 4, argument_buffer.Get(), 0, count_buffer.Get(), 0);
      // After ExecuteIndirect the updated root parameters are undefined; re-set them, then draw column 0 into a.
      l->SetGraphicsRoot32BitConstants(0, 4, std::array<uint32_t, 4>{0, 1, 0, 0}.data(), 0);
      l->SetGraphicsRootUnorderedAccessView(1, a->GetGPUVirtualAddress());
      l->DispatchMesh(1, 1, 1);
      gpu.transition(a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.transition(b.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      l->CopyBufferRegion(readback.Get(), 0, a.Get(), 0, 16);
      l->CopyBufferRegion(readback.Get(), 16, b.Get(), 0, 16);
    });
    if (closed(name, r)) {
      const Target t;
      for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x) {
          const UINT column = x / 8, row = y / 8;
          uint32_t expected = 0;
          if (column == 0 && row == 0)
            expected = rgba(0, 1, 0, 255);
          else if ((column & 1) && column <= 5 && row < column)
            expected = rgba(column, 10 + column / 2, row, 255);
          EXPECT_EQ(name, "pixel", y * 64 + x, r.at(t, x, y), expected);
        }
      // No command wrote the list's own u0 (every command named a or b).
      EXPECT_EQ(name, "u0", 0, r.counters[0], 0);
      void *mapped = nullptr;
      check(readback->Map(0, nullptr, &mapped), "Map(root UAVs)");
      const auto *words = static_cast<const uint32_t *>(mapped);
      EXPECT_EQ(name, "a", 1, words[0], 10 * 1 + 12 * 5 + 1);
      EXPECT_EQ(name, "b", 2, words[4], 11 * 3);
      readback->Unmap(0, nullptr);
    }
  }
  end(name);
}

/* ---- full-range SV_RenderTargetArrayIndex from an MS ---- */

void testFullRangeRtai(Gpu &gpu) {
  const char *name = "ms_full_range_rtai";
  begin();
  auto pso = requirePso(gpu, gpu.root.Get(), nullptr, "ms_rtai", "ps_layer");
  if (pso) {
    // 2048 slices of 4x4 (the D3D12 maximum, 11 bits): primitive i writes slice base + i, for bases below 8,
    // across the 8-bit boundary and at the top of the range. ps_layer: red = primitive, green = min(slice, 255).
    const Target t{4, 4, 2048, 1};
    for (uint32_t base : {0u, 6u, 254u, 1023u, 2044u}) {
      auto r = gpu.render(pso.Get(), gpu.root.Get(), t, {base, 0, 0, 0}, 1, [](auto *l) { l->DispatchMesh(1, 1, 1); });
      if (!closed(name, r))
        continue;
      for (UINT layer = 0; layer < t.layers; ++layer) {
        const bool hit = layer >= base && layer < base + 4;
        EXPECT_EQ(name, "pixel", base * 4096 + layer, r.at(t, 1, 1, layer),
                  hit ? rgba(layer - base, std::min(layer, 255u), 0, 255) : 0u);
      }
    }
  }
  end(name);
}

void testOutputContract(Gpu &gpu) {
  struct Boundary { const char *ms; const char *ps; const char *name; };
  const Boundary boundaries[] = {
    {"ms_contract124", "ps_contract124", "ms_output124"},
    {"ms_contract125", "ps_contract125", "ms_output125"},
    {"ms_contract128_perspective", "ps_contract128_perspective", "ms_output128_perspective"},
    {"ms_contract128_affine", "ps_contract128_affine", "ms_output128_affine"},
    {"ms_contract128_flat", "ps_contract128_flat", "ms_output128_flat"},
    {"ms_contract128_scalars", "ps_contract128_scalars", "ms_output128_scalars"},
    {"ms_contract128_perspective", "ps_contract128_pull_perspective", "ms_output128_pull_perspective"},
    {"ms_contract128_affine", "ps_contract128_pull_affine", "ms_output128_pull_affine"},
    {"ms_contract128_budget", "ps_contract128_perspective", "ms_output128_budget"},
    {"ms_contract128_split", "ps_contract128_split", "ms_output128_split"},
  };
  D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
  dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
  const D3D12_COMMAND_SIGNATURE_DESC signatureDesc{16,1,&dispatch,0};
  ComPtr<ID3D12CommandSignature> signature;
  check(gpu.device->CreateCommandSignature(&signatureDesc,nullptr,IID_PPV_ARGS(&signature)), "wide command signature");
  const uint32_t indirectArgs[12] = {1,1,1,0, 0,1,1,0, 1,1,1,0};
  const uint32_t indirectCount = 2;
  auto arguments = gpu.upload(indirectArgs,sizeof(indirectArgs));
  auto count = gpu.upload(&indirectCount,sizeof(indirectCount));
  for (const auto &boundary : boundaries) {
    begin();
    for (UINT samples : {1u, 4u}) {
      auto pso = requirePso(gpu, gpu.root.Get(), nullptr, boundary.ms, boundary.ps, samples);
      if (!pso) continue;
      Target target{64,64,1,samples};
      auto observed = withStatistics(gpu, pso.Get(), target, {samples,0,0,0},
        [](auto *list) { list->DispatchMesh(1,1,1); });
      const uint64_t threads = std::strcmp(boundary.ms,"ms_contract128_budget") == 0 ? 128 : 4;
      checkStatistics(boundary.name,observed,0,threads,1,4096);
      const auto &result = observed.result;
      if (!closed(boundary.name, result)) continue;
      for (size_t i = 0; i < result.pixels.size(); ++i)
        EXPECT_EQ(boundary.name, "exact_consumed_lanes", i + samples * 4096, result.pixels[i], rgba(0,255,0,255));
      auto indirect = withStatistics(gpu,pso.Get(),target,{samples,0,0,0},[&](auto *list) {
        // GPU count excludes slot 2; slot 1 is empty. Exactly one producer group reaches replay.
        list->ExecuteIndirect(signature.Get(),3,arguments.Get(),0,count.Get(),0);
      });
      checkStatistics(boundary.name,indirect,0,threads,1,4096);
      if (closed(boundary.name,indirect.result))
        for (size_t i=0;i<indirect.result.pixels.size();++i)
          EXPECT_EQ(boundary.name,"indirect_consumed_lanes",i+samples*4096,indirect.result.pixels[i],rgba(0,255,0,255));
      // Restore the same wide MS from its cached blob; spill state is derived from retained bytecode.
      ComPtr<ID3DBlob> cache;
      check(pso->GetCachedBlob(&cache), "wide GetCachedBlob");
      g_cached_pso = {cache->GetBufferPointer(), cache->GetBufferSize()};
      auto restored = requirePso(gpu, gpu.root.Get(), nullptr, boundary.ms, boundary.ps, samples);
      g_cached_pso = {};
      if (restored) {
        auto cached = gpu.render(restored.Get(), gpu.root.Get(), target, {samples,0,0,0}, 1,
          [](auto *list) { list->DispatchMesh(1,1,1); });
        if (closed(boundary.name, cached))
          for (size_t i = 0; i < cached.pixels.size(); ++i)
            EXPECT_EQ(boundary.name, "cached_consumed_lanes", i + samples * 4096, cached.pixels[i], rgba(0,255,0,255));
      }
    }
    end(boundary.name);
  }
  begin();
  const char *coarse = "ms_output128_coarse";
  for (const auto &boundary : boundaries) {
    for (UINT samples : {1u,4u}) {
      auto pso = requirePso(gpu, gpu.root.Get(), nullptr, boundary.ms, boundary.ps, samples);
      if (!pso) continue;
      auto result = gpu.render(pso.Get(), gpu.root.Get(), {64,64,1,samples}, {samples,1,0,0}, 1,
        [](auto *list) { list->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr); list->DispatchMesh(1,1,1); });
      if (closed(coarse,result))
        for (size_t i = 0; i < result.pixels.size(); ++i)
          EXPECT_EQ(coarse,boundary.name,i+samples*4096,result.pixels[i],rgba(0,255,0,255));
    }
  }
  end(coarse);
  begin();
  const char *reject = "ms_output129_reject";
  g_stream_consumer = [&](const D3D12_PIPELINE_STATE_STREAM_DESC &desc, const D3D12_SHADER_BYTECODE &,
                          const D3D12_SHADER_BYTECODE &ms, ComPtr<ID3D12PipelineState> &pso) {
    std::vector<uint8_t> invalid(static_cast<const uint8_t *>(ms.pShaderBytecode),
                                 static_cast<const uint8_t *>(ms.pShaderBytecode) + ms.BytecodeLength);
    auto word = [&](size_t offset) { uint32_t value; std::memcpy(&value, invalid.data()+offset,4); return value; };
    auto set = [&](size_t offset, uint32_t value) { std::memcpy(invalid.data()+offset,&value,4); };
    const uint32_t chunks = word(28);
    bool changed = false;
    for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
      const uint32_t start = word(32+chunk*4);
      if (word(start) != 0x3147534fu) continue; // OSG1
      const uint32_t count = word(start+8), first = word(start+12);
      const size_t insert = start+8+first+size_t(count)*32;
      std::array<uint8_t,32> extra{};
      std::memcpy(extra.data(), invalid.data()+insert-32,32);
      invalid.insert(invalid.begin()+insert,extra.begin(),extra.end());
      set(start+4,word(start+4)+32); set(start+8,count+1);
      for (uint32_t row = 0; row <= count; ++row) {
        const size_t element = start+8+first+row*32;
        set(element+4,word(element+4)+32); // signature-relative semantic string offset
        if (row == count) {
          set(element+8,word(element+8)+1); // extra semantic index
          set(element+20,32); // 33rd packed row: 129 scalars including position
          invalid[element+24]=1; invalid[element+25]=1;
        }
      }
      for (uint32_t c = 0; c < chunks; ++c)
        if (word(32+c*4)>start) set(32+c*4,word(32+c*4)+32);
      set(24,word(24)+32);
      changed = true; break;
    }
    if (!changed) die("missing boundary output signature");
    std::vector<uint64_t> stream((desc.SizeInBytes+7)/8);
    std::memcpy(stream.data(),desc.pPipelineStateSubobjectStream,desc.SizeInBytes);
    constexpr size_t ms_offset = sizeof(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature *>) +
        sizeof(Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS,D3D12_SHADER_BYTECODE>);
    auto *sub = reinterpret_cast<Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,D3D12_SHADER_BYTECODE> *>(
        reinterpret_cast<uint8_t *>(stream.data())+ms_offset);
    sub->value = {invalid.data(),invalid.size()};
    D3D12_PIPELINE_STATE_STREAM_DESC modified{desc.SizeInBytes,stream.data()};
    return gpu.device2->CreatePipelineState(&modified,IID_PPV_ARGS(&pso));
  };
  ComPtr<ID3D12PipelineState> invalid;
  const HRESULT invalid_hr = meshPso(gpu,gpu.root.Get(),nullptr,"ms_contract128_flat","ps_contract128_flat",1,invalid);
  g_stream_consumer = nullptr;
  EXPECT_EQ(reject,"129_scalar_signature",0,uint32_t(invalid_hr),uint32_t(E_INVALIDARG));
  end(reject);
  const char *name = "ms_cull_distance";
  begin();
  for (UINT samples : {1u,4u}) {
    for (bool line : {false,true}) {
    auto pso = requirePso(gpu, gpu.root.Get(), nullptr, line?"ms_contract_cull_line":"ms_contract_cull", "ps_contract_cull", samples);
    if (!pso) continue;
    Target target{64,64,1,samples};
    for (uint32_t mode = 0; mode < 6; ++mode) {
      auto result = gpu.render(pso.Get(), gpu.root.Get(), target, {mode,0,0,0}, 1,
        [](auto *list) { list->DispatchMesh(1,1,1); });
      if (!closed(name, result)) continue;
      for (size_t i = 0; i < result.pixels.size(); ++i)
        EXPECT_EQ(name, "ordered_plane_and_ps_values", i + mode * 4096, result.pixels[i],
          mode == 0 || mode == 5 || (line && i/64 != 16) ? 0u : rgba(0,255,0,255));
    }
    }
  }
  end(name);
}

void testDerivatives(Gpu &gpu) {
  // Constant values differ by mip; a two-texel quad footprint selects mip1, bias1 selects mip2.
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 16;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 4;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "derivative texture");
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 1;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "derivative SRV heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = desc.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 4;
  gpu.device->CreateShaderResourceView(texture.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());
  const std::array<float, 4> mip_values{0.125f, 0.25f, 0.5f, 0.75f};
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
  UINT64 bytes = 0;
  gpu.device->GetCopyableFootprints(&desc, 0, 4, 0, footprints.data(), nullptr, nullptr, &bytes);
  auto upload = gpu.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "derivative texture upload");
  for (UINT mip = 0; mip < 4; ++mip)
    for (UINT y = 0; y < footprints[mip].Footprint.Height; ++y)
      for (UINT x = 0; x < footprints[mip].Footprint.Width; ++x)
        std::memcpy(static_cast<uint8_t *>(mapped) + footprints[mip].Offset +
            y * footprints[mip].Footprint.RowPitch + x * 4, &mip_values[mip], 4);
  upload->Unmap(0, nullptr);
  check(gpu.allocator->Reset(), "derivative upload allocator");
  check(gpu.list->Reset(gpu.allocator.Get(), nullptr), "derivative upload list");
  for (UINT mip = 0; mip < 4; ++mip) {
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprints[mip];
    dst.pResource = texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = mip;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  check(gpu.list->Close(), "derivative upload close");
  ID3D12CommandList *upload_lists[]{gpu.list.Get()};
  gpu.queue->ExecuteCommandLists(1, upload_lists);
  gpu.wait();
  const UINT shapes[3][3]{{32,1,1}, {8,4,1}, {4,2,2}};
  for (UINT stage = 0; stage < 2; ++stage) for (UINT shape = 0; shape < 3; ++shape) {
    const char *suffix = shape == 0 ? "1d" : shape == 1 ? "2d" : "3d";
    const std::string entry = std::string(stage ? "as_derivatives_" : "ms_derivatives_") + suffix;
    const std::string name = std::string(stage ? "derivatives_as_" : "derivatives_ms_") + suffix;
    begin();
    auto pso = requirePso(gpu, gpu.rootDerivatives.Get(), stage ? entry.c_str() : nullptr,
        stage ? "ms_derivative_payload" : entry.c_str(), "ps_color");
    if (pso) {
      const UINT X = shapes[shape][0], Y = shapes[shape][1], Z = shapes[shape][2], threads = X * Y * Z;
      auto result = gpu.render(pso.Get(), gpu.rootDerivatives.Get(), {}, {}, 1, [&](auto *list) {
        ID3D12DescriptorHeap *heaps[]{heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(3, heap->GetGPUDescriptorHandleForHeapStart());
        list->DispatchMesh(2, 1, 1);
      }, 4 + 2 * threads * 24);
      if (closed(name.c_str(), result)) {
        for (UINT group = 0; group < 2; ++group) for (UINT i = 0; i < threads; ++i) {
          const UINT x = i % X, y = (i / X) % Y, z = i / (X * Y);
          const UINT q = shape == 0 ? i % 4 : (x & 1) + 2 * (y & 1);
          auto field = [](UINT a, UINT b, UINT c) { return float(100 * c + 10 * a + b + a * b); };
          std::array<float,4> v{};
          for (UINT lane = 0; lane < 4; ++lane)
            v[lane] = shape == 0 ? field((i & ~3u) + lane, 0, 0) : field((x & ~1u) + (lane & 1),
                (y & ~1u) + (lane >> 1), z);
          const float dx = v[q | 1] - v[q & 2], dy = v[q | 2] - v[q & 1];
          const std::array<float,16> expected{v[q], dx, dy, v[1]-v[0], v[2]-v[0], v[q^1],
              v[q^2], v[q^3], v[3], 1, 1, 0.25f, 0.5f, (q & 1) ? 0.0f : 1.0f, dx, dy};
          const size_t base = 4 + (group * threads + i) * 24;
          const UINT identities[]{x,y,z,i,group*X+x,y,z,q};
          for (UINT k = 0; k < 8; ++k)
            EXPECT_EQ(name.c_str(), "thread_identity", base+k, result.words[base+k], identities[k]);
          for (UINT k = 0; k < expected.size(); ++k) {
            float actual; std::memcpy(&actual, &result.words[base+8+k], 4);
            if (!std::isfinite(actual) || std::abs(actual-expected[k]) > 0.0001f) {
              uint32_t bits; std::memcpy(&bits, &expected[k], 4);
              mismatch(name.c_str(), "quad_sample_lod", base+8+k, result.words[base+8+k], bits);
            }
          }
        }
        for (size_t i = 0; i < result.pixels.size(); ++i)
          EXPECT_EQ(name.c_str(), "payload_render", i, result.pixels[i], stage ? rgba(0,255,0,255) : 0);
      }
    }
    end(name.c_str());
  }
  begin();
  for (UINT stage = 0; stage < 2; ++stage) {
    ComPtr<ID3D12PipelineState> invalid;
    const HRESULT hr = meshPso(gpu, gpu.rootDerivatives.Get(), stage ? "as_derivatives_invalid" : nullptr,
        stage ? "ms_derivative_payload" : "ms_derivatives_invalid", "ps_color", 1, invalid);
    EXPECT_EQ("derivatives_invalid_shape", "pso_rejected", stage, uint32_t(hr), uint32_t(E_INVALIDARG));
  }
  end("derivatives_invalid_shape");
}

void testGroupshared64(Gpu &gpu) {
  for (UINT stage=0;stage<2;++stage) {
    const char *name=stage?"groupshared64_as":"groupshared64_ms";
    begin();
    auto pso=requirePso(gpu,gpu.root.Get(),stage?"as_groupshared64":nullptr,
        stage?"ms_derivative_payload":"ms_groupshared64","ps_color");
    if(pso) {
      auto result=gpu.render(pso.Get(),gpu.root.Get(),{},{},1,[](auto *list){list->DispatchMesh(2,1,1);},4+2*2*153);
      if(closed(name,result)) {
        std::array<uint64_t,128> returned{};
        for(UINT group=0;group<2;++group) {
          const size_t group_base=4+2*group*153;
          for(UINT op=0;op<9;++op) {
            const uint64_t initial=op==4||op==5?0xfffffff000000002ull:0x200000002ull;
            const uint64_t operand=op==4||op==5?0xfffffff100000003ull:0x100000001ull;
            uint64_t expected=operand;
            switch(op) {
            case 0: expected=initial+operand; break;
            case 1: expected=initial&operand; break;
            case 2: expected=initial|operand; break;
            case 3: expected=initial^operand; break;
            case 4: expected=uint64_t(std::min(int64_t(initial),int64_t(operand))); break;
            case 5: expected=uint64_t(std::max(int64_t(initial),int64_t(operand))); break;
            case 6: expected=std::min(initial,operand); break;
            case 7: expected=std::max(initial,operand); break;
            }
            const size_t base=group_base+4*op;
            EXPECT_EQ(name,"operation_old",group*9+op,uint64_t(result.words[base])|(uint64_t(result.words[base+1])<<32),initial);
            EXPECT_EQ(name,"operation_final",group*9+op,uint64_t(result.words[base+2])|(uint64_t(result.words[base+3])<<32),expected);
          }
          const uint64_t compare_wrap[]={0x200000002ull,0x300000003ull,0x300000003ull,UINT64_MAX,0,0x8000000000000000ull};
          for(UINT op=0;op<6;++op) {
            const size_t base=group_base+2*(18+op);
            EXPECT_EQ(name,"compare_wrap",group*6+op,uint64_t(result.words[base])|(uint64_t(result.words[base+1])<<32),compare_wrap[op]);
          }
          for(UINT lane=0;lane<128;++lane) {
            const size_t base=group_base+2*(24+lane);
            returned[lane]=uint64_t(result.words[base])|(uint64_t(result.words[base+1])<<32);
          }
          std::sort(returned.begin(),returned.end());
          for(UINT lane=0;lane<128;++lane)
            EXPECT_EQ(name,"returned_permutation",group*128+lane,returned[lane],uint64_t(lane)*0x100000001ull);
          const size_t base=group_base+2*152;
          EXPECT_EQ(name,"final",group,uint64_t(result.words[base])|(uint64_t(result.words[base+1])<<32),128ull*0x100000001ull);
        }
        for(size_t pixel=0;pixel<result.pixels.size();++pixel)
          EXPECT_EQ(name,"payload_render",pixel,result.pixels[pixel],stage?rgba(0,255,0,255):0);
      }
    }
    end(name);
  }
}

void printCaps(ID3D12Device *device) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)), "CheckFeatureSupport(OPTIONS7)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9)), "CheckFeatureSupport(OPTIONS9)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12)),
        "CheckFeatureSupport(OPTIONS12)");
  std::printf("OWNED_D3D12_MESH_SHADER_CAPS tier=%d stats=%d full_range_rtai=%d derivatives=%d "
              "ms_primitives_culled=%d\n",
              o7.MeshShaderTier, o9.MeshShaderPipelineStatsSupported,
              o9.MeshShaderSupportsFullRangeRenderTargetArrayIndex, o9.DerivativesInMeshAndAmplificationShadersSupported,
              o12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives);
  std::fflush(stdout);
}

int run(bool groupshared64Only) {
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId) != 0)
    die("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_MESH_SHADER_BACKEND id=%s\n", backend());

  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  printCaps(device.Get());

  Gpu gpu(device.Get());
  if (groupshared64Only) {
    testGroupshared64(gpu);
    std::printf("OWNED_D3D12_MESH_SHADER_%s groupshared64=2 failures=%d\n",
                g_failures ? "FAIL" : "PASS", g_failures);
    std::fflush(stdout);
    return g_failures ? 1 : 0;
  }
  testDerivatives(gpu);
  testGroupshared64(gpu);
  testBasic(gpu, 1);
  testGrid(gpu);
  testPayload(gpu);
  testCull(gpu, 1);
  testPrimitiveAttributes(gpu);
  testLayerViewport(gpu);
  testLines(gpu);
  testMax(gpu);
  testPartialRows(gpu);
  testIndirect(gpu);
  testBasic(gpu, 4);
  testCull(gpu, 4);
  testGridLimits(gpu);
  testRejects(gpu);
  testStatistics(gpu);
  testStatisticsEarlyDepth(gpu);
  testPredication(gpu);
  testPsoCache(gpu);
  testViewInstancing(gpu);
  testIndirectRootConstants(gpu);
  testFullRangeRtai(gpu);
  testOutputContract(gpu);

  if (!g_failures)
    std::printf("OWNED_D3D12_MESH_SHADER_PASS\n");
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 1) return run(false);
  if (argc == 2 && std::wcscmp(argv[1], L"--groupshared64-only") == 0) return run(true);
  die("usage: mesh-shader [--groupshared64-only]");
}
