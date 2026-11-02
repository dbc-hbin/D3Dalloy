// Owned D3D12 ID3D12PipelineLibrary/1 contract: store/load/serialize, error codes, thread safety,
// GPU-exact output of every loaded PSO, and cross-process reuse through a re-executed child.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool g_child = false;
std::vector<uint8_t> readFile(const wchar_t *path);

[[noreturn]] void fail(const char *step, HRESULT actual = E_FAIL, HRESULT expected = S_OK) {
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_FAIL%s step=%s actual=%08lx expected=%08lx\n",
               g_child ? "_CHILD" : "", step, static_cast<unsigned long>(actual),
               static_cast<unsigned long>(expected));
  std::fflush(stderr);
  throw std::runtime_error(step);
}
void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected)
    fail(step, actual, expected);
}
void check(HRESULT actual, const char *step) {
  if (FAILED(actual))
    fail(step, actual);
}
void passed(const char *name) {
  std::printf("OWNED_D3D12_PIPELINE_LIBRARY_%sCASE_PASS %s\n", g_child ? "CHILD_" : "", name);
  std::fflush(stdout);
}

// Stream subobjects newer than the llvm-mingw SDK; layouts follow Microsoft's d3d12.h.
constexpr UINT kDepthStencil2 = 26, kRasterizer1 = 27, kRasterizer2 = 28, kSerializedRoot = 29;
struct DepthStencilOp1 {
  D3D12_STENCIL_OP fail, depthFail, pass;
  D3D12_COMPARISON_FUNC func;
  UINT8 readMask, writeMask;
};
struct DepthStencil2 {
  BOOL depthEnable;
  D3D12_DEPTH_WRITE_MASK depthWriteMask;
  D3D12_COMPARISON_FUNC depthFunc;
  BOOL stencilEnable;
  DepthStencilOp1 front, back;
  BOOL depthBoundsTestEnable;
};
struct Rasterizer1 {
  D3D12_FILL_MODE fill;
  D3D12_CULL_MODE cull;
  BOOL frontCCW;
  FLOAT depthBias, depthBiasClamp, slopeScaledDepthBias;
  BOOL depthClip, multisample, antialiasedLine;
  UINT forcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE conservative;
};
struct Rasterizer2 {
  D3D12_FILL_MODE fill;
  D3D12_CULL_MODE cull;
  BOOL frontCCW;
  FLOAT depthBias, depthBiasClamp, slopeScaledDepthBias;
  BOOL depthClip;
  UINT lineMode;
  UINT forcedSampleCount;
  D3D12_CONSERVATIVE_RASTERIZATION_MODE conservative;
};
struct SerializedRoot {
  const void *blob;
  SIZE_T size;
};

// Same packing as CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT: pointer-aligned {type, data}.
class Stream {
  std::vector<uint64_t> words_;
  size_t size_ = 0;

public:
  template <typename T> Stream &add(UINT type, const T &data) {
    struct alignas(void *) Subobject {
      UINT type;
      T data;
    } subobject{type, data};
    const size_t offset = size_;
    size_ += (sizeof(subobject) + alignof(void *) - 1) & ~(alignof(void *) - 1);
    words_.resize((size_ + 7) / 8);
    std::memcpy(reinterpret_cast<char *>(words_.data()) + offset, &subobject, sizeof(subobject));
    return *this;
  }
  D3D12_PIPELINE_STATE_STREAM_DESC desc() const { return {size_, const_cast<uint64_t *>(words_.data())}; }
};

ComPtr<ID3DBlob> compile(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> bytes, errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), "owned-pipeline-library", nullptr, nullptr, entry, target,
                          D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytes, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("compile", hr);
  }
  return bytes;
}

const char *kComputeSource = R"(
  cbuffer Args : register(b0) { uint a; uint b; };
  RWByteAddressBuffer result : register(u0);
  [numthreads(1,1,1)] void csA() { result.Store(0, a * 0x10000u + b + 0x1234u); }
  [numthreads(1,1,1)] void csB() { result.Store(0, (a * 0x10000u + b) ^ 0x5a5a5a5au); }
)";
const char *kGraphicsSource = R"(
  cbuffer Color : register(b0) { uint4 color; };
  float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  }
  float4 ps() : SV_Target { return float4(color) / 255.0; }
  float4 inverted() : SV_Target { return 1.0 - float4(color) / 255.0; }
)";

struct Shaders {
  ComPtr<ID3DBlob> csA, csB, vs, ps, inverted;
  std::vector<uint8_t> coverage = readFile(L"pipeline-library-coverage.dxil");
  Shaders()
      : csA(compile(kComputeSource, "csA", "cs_5_0")), csB(compile(kComputeSource, "csB", "cs_5_0")),
        vs(compile(kGraphicsSource, "vs", "vs_5_0")), ps(compile(kGraphicsSource, "ps", "ps_5_0")),
        inverted(compile(kGraphicsSource, "inverted", "ps_5_0")) {}
};
D3D12_SHADER_BYTECODE code(ID3DBlob *blob) { return {blob->GetBufferPointer(), blob->GetBufferSize()}; }

ComPtr<ID3DBlob> serializeRoot(UINT constants, bool uav) {
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = constants;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = uav ? 2 : 1;
  desc.pParameters = params;
  ComPtr<ID3DBlob> bytes, errors;
  check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "serialize root");
  return bytes;
}
ComPtr<ID3D12RootSignature> root(ID3D12Device *device, ID3DBlob *bytes) {
  ComPtr<ID3D12RootSignature> result;
  check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&result)),
        "create root");
  return result;
}

struct Fixture {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Device1> device1;
  ComPtr<ID3D12Device2> device2;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 fence_value = 0;
  Shaders shaders;
  ComPtr<ID3DBlob> computeRootBlob = serializeRoot(2, true), otherRootBlob = serializeRoot(3, true),
                   graphicsRootBlob = serializeRoot(4, false);
  ComPtr<ID3D12RootSignature> computeRoot, otherRoot, graphicsRoot;

  Fixture() {
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    check(device.As(&device1), "ID3D12Device1");
    check(device.As(&device2), "ID3D12Device2");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    computeRoot = root(device.Get(), computeRootBlob.Get());
    otherRoot = root(device.Get(), otherRootBlob.Get());
    graphicsRoot = root(device.Get(), graphicsRootBlob.Get());
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc(ID3DBlob *cs = nullptr) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = computeRoot.Get();
    desc.CS = code(cs ? cs : shaders.csA.Get());
    return desc;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = graphicsRoot.Get();
    desc.VS = code(shaders.vs.Get());
    desc.PS = code(shaders.ps.Get());
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    return desc;
  }
  // Graphics stream using the newer subobject revisions (RASTERIZER1, DEPTH_STENCIL2, VIEW_INSTANCING).
  Stream graphicsStream(D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE) {
    Stream stream;
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = 1;
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    Rasterizer1 raster{D3D12_FILL_MODE_SOLID, cull, FALSE, 0.0f, 0.0f, 0.0f, TRUE, FALSE, FALSE, 0,
                       D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
    DepthStencilOp1 face{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                         D3D12_COMPARISON_FUNC_ALWAYS, 0xff, 0xff};
    DepthStencil2 depth{FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, FALSE, face, face, FALSE};
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, graphicsRoot.Get())
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, code(shaders.vs.Get()))
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, code(shaders.inverted.Get()))
        .add(kRasterizer1, raster)
        .add(kDepthStencil2, depth)
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats)
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC{})
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT(0));
    return stream;
  }
  // Compute stream with its root signature supplied as SERIALIZED_ROOT_SIGNATURE bytes.
  Stream computeStream() {
    Stream stream;
    stream.add(kSerializedRoot, SerializedRoot{computeRootBlob->GetBufferPointer(), computeRootBlob->GetBufferSize()})
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, code(shaders.csB.Get()))
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, D3D12_PIPELINE_STATE_FLAG_NONE);
    return stream;
  }

  void wait() {
    check(queue->Signal(fence.Get(), ++fence_value), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      fail("event");
    check(fence->SetEventOnCompletion(fence_value, event), "completion event");
    const DWORD waited = WaitForSingleObject(event, 15000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0)
      fail("GPU timeout");
  }

  ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)),
          "buffer");
    return resource;
  }

  static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                      D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
  }

  // Dispatch with (a, b) = (3, 0x135) and return the exact word written.
  UINT dispatch(ID3D12PipelineState *pso) {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso, IID_PPV_ARGS(&list)),
          "list");
    auto output = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT values[2]{3, 0x135};
    list->SetComputeRootSignature(computeRoot.Get());
    list->SetComputeRoot32BitConstants(0, 2, values, 0);
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4);
    check(list->Close(), "close compute");
    ID3D12CommandList *lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait();
    void *mapped = nullptr;
    D3D12_RANGE range{0, 4};
    check(readback->Map(0, &range, &mapped), "map compute");
    UINT actual;
    std::memcpy(&actual, mapped, 4);
    D3D12_RANGE none{};
    readback->Unmap(0, &none);
    return actual;
  }

  // Draw a full-target triangle into a 4x4 RGBA8 target; every texel must be exactly `expected`.
  void draw(ID3D12PipelineState *pso, const UINT (&color)[4], UINT expected, const char *label, UINT samples = 1) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "rtv heap");
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.SampleDesc.Count = samples;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&target)),
          "render target");
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, 256 * 4, D3D12_RESOURCE_STATE_COPY_DEST);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso, IID_PPV_ARGS(&list)),
          "list");
    const float clear[4]{0, 0, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    D3D12_RECT scissor{0, 0, 4, 4};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(graphicsRoot.Get());
    list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    ComPtr<ID3D12Resource> resolved;
    if (samples > 1) {
      desc.SampleDesc.Count = 1;
      check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                            nullptr, IID_PPV_ARGS(&resolved)), "resolve target");
      barrier(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, desc.Format);
      barrier(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    } else {
      barrier(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resolved ? resolved.Get() : target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "close graphics");
    ID3D12CommandList *lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait();
    void *mapped = nullptr;
    D3D12_RANGE range{0, 256 * 4};
    check(readback->Map(0, &range, &mapped), "map graphics");
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x) {
        UINT texel;
        std::memcpy(&texel, static_cast<const char *>(mapped) + y * 256 + x * 4, 4);
        if (texel != expected) {
          std::fprintf(stderr, "texel(%u,%u)=%08x expected=%08x\n", x, y, texel, expected);
          fail(label);
        }
      }
    D3D12_RANGE none{};
    readback->Unmap(0, &none);
    std::printf("OWNED_D3D12_PIPELINE_LIBRARY_GPU case=%s texels=16 value=%08x\n", label, expected);
  }

  void gpuCompute(ID3D12PipelineState *pso, UINT expected, const char *label) {
    const UINT actual = dispatch(pso);
    if (actual != expected) {
      std::fprintf(stderr, "actual=%08x expected=%08x\n", actual, expected);
      fail(label);
    }
    std::printf("OWNED_D3D12_PIPELINE_LIBRARY_GPU case=%s value=%08x\n", label, actual);
  }

  // The three stored pipelines, run from `library`, with the exact values each shader defines.
  void runLoaded(ID3D12PipelineLibrary1 *library, const char *label) {
    auto cs = computeDesc();
    ComPtr<ID3D12PipelineState> compute, graphics, stream, streamCompute, again;
    check(library->LoadComputePipeline(L"compute", &cs, IID_PPV_ARGS(&compute)), "load compute");
    auto gfx = graphicsDesc();
    check(library->LoadGraphicsPipeline(L"graphics", &gfx, IID_PPV_ARGS(&graphics)), "load graphics");
    auto gstream = graphicsStream();
    auto gdesc = gstream.desc();
    check(library->LoadPipeline(L"stream", &gdesc, IID_PPV_ARGS(&stream)), "load stream graphics");
    auto cstream = computeStream();
    auto cdesc = cstream.desc();
    check(library->LoadPipeline(L"stream-compute", &cdesc, IID_PPV_ARGS(&streamCompute)), "load stream compute");
    // A library hands out one object per name.
    check(library->LoadComputePipeline(L"compute", &cs, IID_PPV_ARGS(&again)), "reload compute");
    if (again.Get() != compute.Get())
      fail("reload returned a different PSO");
    const std::string base(label);
    gpuCompute(compute.Get(), 3u * 0x10000u + 0x135u + 0x1234u, (base + "_compute").c_str());
    gpuCompute(streamCompute.Get(), (3u * 0x10000u + 0x135u) ^ 0x5a5a5a5au, (base + "_stream_compute").c_str());
    const UINT color[4]{0x12, 0x34, 0x56, 0x78};
    draw(graphics.Get(), color, 0x78563412u, (base + "_graphics").c_str());
    draw(stream.Get(), color, 0x87a9cbedu, (base + "_stream_graphics").c_str());
  }
};

ComPtr<ID3D12PipelineLibrary1> createLibrary(Fixture &f, const void *data, size_t size) {
  ComPtr<ID3D12PipelineLibrary1> library;
  check(f.device1->CreatePipelineLibrary(data, size, IID_PPV_ARGS(&library)), "create library");
  return library;
}

std::vector<uint8_t> serialize(ID3D12PipelineLibrary *library) {
  const SIZE_T size = library->GetSerializedSize();
  if (size < 104)
    fail("serialized size");
  std::vector<uint8_t> bytes(size);
  check(library->Serialize(bytes.data(), bytes.size()), "serialize");
  return bytes;
}

void rejectLibrary(Fixture &f, const void *data, size_t size, HRESULT expected, const char *label) {
  void *out = reinterpret_cast<void *>(uintptr_t(1));
  expect(f.device1->CreatePipelineLibrary(data, size, __uuidof(ID3D12PipelineLibrary), &out), expected, label);
  if (out)
    fail("failed CreatePipelineLibrary left output");
}

void testFeatures(Fixture &f) {
  D3D12_FEATURE_DATA_SHADER_CACHE cache{};
  check(f.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache)), "shader cache");
  constexpr UINT required = D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY;
  if ((cache.SupportFlags & required) != required)
    fail("shader cache flags", cache.SupportFlags);
  D3D12_FEATURE_DATA_SERIALIZATION serialization{};
  check(f.device->CheckFeatureSupport(D3D12_FEATURE_SERIALIZATION, &serialization, sizeof(serialization)),
        "serialization");
  if (serialization.HeapSerializationTier != D3D12_HEAP_SERIALIZATION_TIER_0)
    fail("heap serialization tier");
  serialization.NodeIndex = 1;
  expect(f.device->CheckFeatureSupport(D3D12_FEATURE_SERIALIZATION, &serialization, sizeof(serialization)),
         E_INVALIDARG, "serialization node 1");
  passed("feature_reporting");
}

void testStreamCoverage(Fixture &f) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
  check(f.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)), "options2");
  const HRESULT depth_bounds_result = options.DepthBoundsTestSupported ? S_OK : E_INVALIDARG;
  auto create = [&](const Stream &stream, HRESULT expected, const char *label) {
    auto desc = stream.desc();
    void *out = reinterpret_cast<void *>(uintptr_t(1));
    expect(f.device2->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState), &out), expected, label);
    if (SUCCEEDED(expected))
      static_cast<IUnknown *>(out)->Release();
    else if (out)
      fail("failed CreatePipelineState left output");
  };
  create(f.graphicsStream(), S_OK, "stream_rasterizer1_depthstencil2");
  create(f.computeStream(), S_OK, "stream_serialized_root");
  DepthStencilOp1 face{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                       D3D12_COMPARISON_FUNC_ALWAYS, 0xff, 0xff};
  D3D12_DEPTH_STENCIL_DESC1 bounds{};
  bounds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  bounds.FrontFace = bounds.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                      D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  bounds.DepthBoundsTestEnable = TRUE;
  create(f.graphicsStream().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, bounds), E_INVALIDARG,
         "duplicate_depth_stencil_revisions");
  auto base = [&] {
    Stream s;
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = 1;
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, f.graphicsRoot.Get())
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, code(f.shaders.vs.Get()))
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, code(f.shaders.ps.Get()))
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats)
        .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    return s;
  };
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, bounds), depth_bounds_result, "depth_bounds_enabled");
  bounds.DepthBoundsTestEnable = FALSE;
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, bounds), S_OK, "depth_bounds_disabled");
  DepthStencil2 bounds2{FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, FALSE, face, face, TRUE};
  create(base().add(kDepthStencil2, bounds2), depth_bounds_result, "depth_bounds2_enabled");
  bounds2.depthBoundsTestEnable = FALSE;
  create(base().add(kDepthStencil2, bounds2), S_OK, "depth_bounds2_disabled");
  DepthStencil2 split{FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, TRUE, face, face, FALSE};
  split.back.readMask = 0x0f;
  // IndependentFrontAndBackStencilRefMaskSupported is TRUE: split masks are a valid PSO.
  create(base().add(kDepthStencil2, split), S_OK, "independent_stencil_masks");
  // ViewInstancingTier 1: more than D3D12_MAX_VIEW_INSTANCE_COUNT views, or unknown flags, are invalid.
  D3D12_VIEW_INSTANCE_LOCATION locations[5]{};
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC{5, locations}),
         E_INVALIDARG, "view_instancing_count_5");
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,
                    D3D12_VIEW_INSTANCING_DESC{2, locations, static_cast<D3D12_VIEW_INSTANCING_FLAGS>(2)}),
         E_INVALIDARG, "view_instancing_flags");
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, code(f.shaders.vs.Get())), E_INVALIDARG, "mesh_shader");
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, code(f.shaders.vs.Get())), E_INVALIDARG,
         "amplification_shader");
  create(base().add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE{}), S_OK, "empty_mesh_stage");
  Rasterizer2 narrow{D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0, 0, 0, TRUE, 3, 0,
                     D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
  create(base().add(kRasterizer2, narrow), E_INVALIDARG, "narrow_quadrilateral_lines");
  Rasterizer2 wide = narrow;
  wide.lineMode = 2;
  create(base().add(kRasterizer2, wide), S_OK, "rasterizer2_wide_lines");
  Rasterizer1 fractional{D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0.5f, 0, 0, TRUE, FALSE, FALSE, 0,
                         D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
  // DynamicDepthBiasSupported is TRUE: a fractional FLOAT DepthBias is carried exactly.
  create(base().add(kRasterizer1, fractional), S_OK, "fractional_depth_bias");
  create(base()
             .add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC{D3D12_FILL_MODE_SOLID,
                                                                                        D3D12_CULL_MODE_NONE})
             .add(kRasterizer1, fractional),
         E_INVALIDARG, "duplicate_rasterizer_revisions");
  create(base().add(kSerializedRoot, SerializedRoot{f.graphicsRootBlob->GetBufferPointer(),
                                                    f.graphicsRootBlob->GetBufferSize()}),
         E_INVALIDARG, "duplicate_root_signature_revisions");
  create(base().add(31u, UINT(0)), E_INVALIDARG, "unknown_subobject");
  passed("stream_subobject_coverage");
}

void testContracts(Fixture &f, std::vector<uint8_t> &serialized) {
  expect(f.device1->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), nullptr), S_FALSE,
         "query without output");
  auto library = createLibrary(f, nullptr, 0);
  ComPtr<ID3D12PipelineLibrary> base;
  check(library.As(&base), "QI ID3D12PipelineLibrary");
  ComPtr<ID3D12Device> owner;
  check(library->GetDevice(IID_PPV_ARGS(&owner)), "GetDevice");
  if (owner.Get() != f.device.Get())
    fail("library device");
  if (library->GetSerializedSize() != 104)
    fail("empty serialized size");

  auto cs = f.computeDesc();
  auto gfx = f.graphicsDesc();
  void *out = reinterpret_cast<void *>(uintptr_t(1));
  expect(library->LoadComputePipeline(L"compute", &cs, __uuidof(ID3D12PipelineState), &out), E_INVALIDARG,
         "missing name");
  if (out)
    fail("missing name left output");

  ComPtr<ID3D12PipelineState> compute, graphics, stream, streamCompute;
  check(f.device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&compute)), "create compute");
  check(f.device->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(&graphics)), "create graphics");
  auto gstream = f.graphicsStream();
  auto gdesc = gstream.desc();
  check(f.device2->CreatePipelineState(&gdesc, IID_PPV_ARGS(&stream)), "create stream graphics");
  auto cstream = f.computeStream();
  auto cdesc = cstream.desc();
  check(f.device2->CreatePipelineState(&cdesc, IID_PPV_ARGS(&streamCompute)), "create stream compute");

  expect(library->StorePipeline(nullptr, compute.Get()), E_INVALIDARG, "null name");
  expect(library->StorePipeline(L"null", nullptr), E_INVALIDARG, "null pipeline");
  check(library->StorePipeline(L"compute", compute.Get()), "store compute");
  check(library->StorePipeline(L"graphics", graphics.Get()), "store graphics");
  check(library->StorePipeline(L"stream", stream.Get()), "store stream");
  check(library->StorePipeline(L"stream-compute", streamCompute.Get()), "store stream compute");
  expect(library->StorePipeline(L"compute", graphics.Get()), E_INVALIDARG, "duplicate name");
  passed("store");

  ComPtr<ID3D12PipelineState> loaded;
  check(library->LoadGraphicsPipeline(L"graphics", &gfx, IID_PPV_ARGS(&loaded)), "load after store");
  if (loaded.Get() != graphics.Get())
    fail("load after store is not the stored PSO");
  loaded.Reset();

  auto reject = [&](auto call, const char *label) {
    void *result = reinterpret_cast<void *>(uintptr_t(1));
    expect(call(&result), E_INVALIDARG, label);
    if (result)
      fail("rejected load left output");
  };
  reject([&](void **p) { return library->LoadComputePipeline(L"graphics", &cs, __uuidof(ID3D12PipelineState), p); },
         "kind_mismatch");
  auto changed = f.computeDesc();
  changed.pRootSignature = f.otherRoot.Get();
  reject([&](void **p) { return library->LoadComputePipeline(L"compute", &changed, __uuidof(ID3D12PipelineState), p); },
         "root_signature_mismatch");
  changed = f.computeDesc(f.shaders.csB.Get());
  reject([&](void **p) { return library->LoadComputePipeline(L"compute", &changed, __uuidof(ID3D12PipelineState), p); },
         "shader_mismatch");
  auto state = f.graphicsDesc();
  state.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
  reject([&](void **p) { return library->LoadGraphicsPipeline(L"graphics", &state, __uuidof(ID3D12PipelineState), p); },
         "rasterizer_state_mismatch");
  state = f.graphicsDesc();
  state.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  reject([&](void **p) { return library->LoadGraphicsPipeline(L"graphics", &state, __uuidof(ID3D12PipelineState), p); },
         "render_target_format_mismatch");
  auto culled = f.graphicsStream(D3D12_CULL_MODE_FRONT);
  auto culledDesc = culled.desc();
  reject([&](void **p) { return library->LoadPipeline(L"stream", &culledDesc, __uuidof(ID3D12PipelineState), p); },
         "stream_state_mismatch");
  reject([&](void **p) { return library->LoadPipeline(L"missing", &gdesc, __uuidof(ID3D12PipelineState), p); },
         "stream_missing_name");
  passed("load_rejections");

  f.runLoaded(library.Get(), "stored");

  const SIZE_T size = library->GetSerializedSize();
  std::vector<uint8_t> bytes(size + 16, 0xcc);
  expect(library->Serialize(bytes.data(), size - 1), E_INVALIDARG, "serialize too small");
  expect(library->Serialize(nullptr, size), E_INVALIDARG, "serialize null");
  check(library->Serialize(bytes.data(), size + 16), "serialize larger buffer");
  if (bytes[size] != 0xcc)
    fail("serialize wrote past its size");
  bytes.resize(size);
  serialized = serialize(library.Get());
  if (serialized != bytes)
    fail("serialize is not deterministic");
  passed("serialize");

  // Foreign, corrupt, truncated and mismatched blobs.
  ComPtr<ID3DBlob> pso_blob;
  check(compute->GetCachedBlob(&pso_blob), "pso blob");
  const std::vector<uint8_t> cached_before(static_cast<const uint8_t*>(pso_blob->GetBufferPointer()),
      static_cast<const uint8_t*>(pso_blob->GetBufferPointer()) + pso_blob->GetBufferSize());
  std::memset(pso_blob->GetBufferPointer(),0xa5,pso_blob->GetBufferSize());
  ComPtr<ID3DBlob> cached_after;
  check(compute->GetCachedBlob(&cached_after),"independent public cached blob");
  if (cached_after->GetBufferPointer() == pso_blob->GetBufferPointer() ||
      cached_after->GetBufferSize() != cached_before.size() ||
      std::memcmp(cached_after->GetBufferPointer(),cached_before.data(),cached_before.size()) ||
      serialize(library.Get()) != serialized)
    fail("public cached blob mutation changed private library payload");
  // Multiple names retain the private immutable payload, not the writable blob.
  auto aliases = createLibrary(f,nullptr,0);
  check(aliases->StorePipeline(L"a",compute.Get()),"store alias a");
  check(aliases->StorePipeline(L"b",compute.Get()),"store alias b");
  ComPtr<ID3D12PipelineState> aliasA,aliasB;
  check(aliases->LoadComputePipeline(L"a",&cs,IID_PPV_ARGS(&aliasA)),"load alias a");
  check(aliases->LoadComputePipeline(L"b",&cs,IID_PPV_ARGS(&aliasB)),"load alias b");
  if (aliasA.Get()!=compute.Get() || aliasB.Get()!=compute.Get()) fail("alias identity");
  pso_blob = cached_after;
  passed("private_payload_public_mutation_isolation");
  rejectLibrary(f, pso_blob->GetBufferPointer(), pso_blob->GetBufferSize(), E_INVALIDARG, "pso_blob_as_library");
  rejectLibrary(f, nullptr, 16, E_INVALIDARG, "null_nonempty");
  rejectLibrary(f, serialized.data(), 0, E_INVALIDARG, "nonnull_empty");
  rejectLibrary(f, serialized.data(), 64, E_INVALIDARG, "short_header");
  rejectLibrary(f, serialized.data(), serialized.size() - 1, E_INVALIDARG, "truncated");
  auto damaged = serialized;
  damaged.back() ^= 1;
  rejectLibrary(f, damaged.data(), damaged.size(), E_INVALIDARG, "corrupt_payload");
  damaged = serialized;
  damaged[200] ^= 0x40;
  rejectLibrary(f, damaged.data(), damaged.size(), E_INVALIDARG, "corrupt_entry");
  damaged = serialized;
  damaged[8] ^= 0xff; // version
  rejectLibrary(f, damaged.data(), damaged.size(), D3D12_ERROR_DRIVER_VERSION_MISMATCH, "version_mismatch");
  damaged = serialized;
  damaged[24] ^= 0xff; // adapter registry ID
  rejectLibrary(f, damaged.data(), damaged.size(), D3D12_ERROR_ADAPTER_NOT_FOUND, "adapter_mismatch");
  damaged = serialized;
  damaged[32] ^= 0xff; // compiler UUID
  rejectLibrary(f, damaged.data(), damaged.size(), D3D12_ERROR_DRIVER_VERSION_MISMATCH, "compiler_mismatch");
  damaged.assign(serialized.size(), 0xa5);
  rejectLibrary(f, damaged.data(), damaged.size(), E_INVALIDARG, "foreign_blob");
  passed("blob_rejections");

  // A new library from the blob; the caller's copy is destroyed before any load.
  {
    std::vector<uint8_t> scratch = serialized;
    auto restored = createLibrary(f, scratch.data(), scratch.size());
    std::fill(scratch.begin(), scratch.end(), 0xda);
    scratch.clear();
    scratch.shrink_to_fit();
    compute.Reset();
    graphics.Reset();
    stream.Reset();
    streamCompute.Reset();
    library.Reset();
    f.runLoaded(restored.Get(), "deserialized");
    if (serialize(restored.Get()) != serialized)
      fail("round trip changed the library");
  }
  passed("deserialize_same_process");
}

void testThreads(Fixture &f, const std::vector<uint8_t> &serialized) {
  auto library = createLibrary(f, serialized.data(), serialized.size());
  ComPtr<ID3D12PipelineState> extra;
  auto cs = f.computeDesc();
  check(f.device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&extra)), "thread source pso");
  constexpr int kThreads = 8, kRounds = 24;
  std::atomic<int> failures{0};
  std::vector<ID3D12PipelineState *> first(kThreads * 2, nullptr);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&, t] {
      auto gfx = f.graphicsDesc();
      auto gstream = f.graphicsStream();
      auto gdesc = gstream.desc();
      const std::wstring name = L"thread-" + std::to_wstring(t);
      if (FAILED(library->StorePipeline(name.c_str(), extra.Get())))
        ++failures;
      for (int round = 0; round < kRounds; ++round) {
        ComPtr<ID3D12PipelineState> a, b, c;
        if (FAILED(library->LoadComputePipeline(L"compute", &cs, IID_PPV_ARGS(&a))) ||
            FAILED(library->LoadGraphicsPipeline(L"graphics", &gfx, IID_PPV_ARGS(&b))) ||
            FAILED(library->LoadPipeline(L"stream", &gdesc, IID_PPV_ARGS(&c))) ||
            library->StorePipeline(L"compute", extra.Get()) != E_INVALIDARG) {
          ++failures;
          continue;
        }
        if (!round) {
          first[t * 2] = a.Get();
          first[t * 2 + 1] = b.Get();
        } else if (first[t * 2] != a.Get() || first[t * 2 + 1] != b.Get()) {
          ++failures;
        }
        if (library->GetSerializedSize() < serialized.size())
          ++failures;
      }
    });
  for (auto &thread : threads)
    thread.join();
  if (failures)
    fail("concurrent library access", failures.load());
  for (int t = 1; t < kThreads; ++t)
    if (first[t * 2] != first[0] || first[t * 2 + 1] != first[1])
      fail("threads received different PSOs for one name");
  auto after = serialize(library.Get());
  auto reloaded = createLibrary(f, after.data(), after.size());
  for (int t = 0; t < kThreads; ++t) {
    const std::wstring name = L"thread-" + std::to_wstring(t);
    ComPtr<ID3D12PipelineState> pso;
    check(reloaded->LoadComputePipeline(name.c_str(), &cs, IID_PPV_ARGS(&pso)), "load thread entry");
    if (t == kThreads - 1)
      f.gpuCompute(pso.Get(), 3u * 0x10000u + 0x135u + 0x1234u, "thread_entry");
  }
  std::printf("OWNED_D3D12_PIPELINE_LIBRARY_THREADS threads=%d rounds=%d entries=%d\n", kThreads, kRounds,
              4 + kThreads);
  passed("concurrent_load_store");
}

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("open library file", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024)
    fail("library file size");
  std::vector<uint8_t> bytes(size_t(size.QuadPart));
  DWORD count = 0;
  const BOOL ok = ReadFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr);
  CloseHandle(file);
  if (!ok || count != bytes.size())
    fail("read library file");
  return bytes;
}

void writeFile(const wchar_t *path, const std::vector<uint8_t> &bytes) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("create library file", HRESULT_FROM_WIN32(GetLastError()));
  DWORD count = 0;
  const BOOL ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr);
  CloseHandle(file);
  if (!ok || count != bytes.size())
    fail("write library file");
}

// The child is a copy of this executable so its backend log (named after the executable)
// is separate: the harness proves from it that the child compiled no stage.
void runChild(const std::vector<uint8_t> &serialized) {
  constexpr const wchar_t *kBlob = L"pipeline-library.bin";
  constexpr const wchar_t *kChild = L"owned-d3d12-pipeline-library-child.exe";
  writeFile(kBlob, serialized);
  wchar_t self[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
    fail("module path");
  if (!CopyFileW(self, kChild, FALSE))
    fail("copy child", HRESULT_FROM_WIN32(GetLastError()));
  std::wstring command = std::wstring(L"\"") + kChild + L"\" --child " + kBlob;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  std::fflush(stdout);
  std::fflush(stderr);
  if (!CreateProcessW(kChild, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
    fail("spawn child", HRESULT_FROM_WIN32(GetLastError()));
  const DWORD waited = WaitForSingleObject(process.hProcess, 300000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (waited != WAIT_OBJECT_0 || code)
    fail("child process", HRESULT(code));
  passed("cross_process_reuse");
}

int child(const wchar_t *path) {
  g_child = true;
  const auto serialized = readFile(path);
  Fixture f;
  auto library = createLibrary(f, serialized.data(), serialized.size());
  f.runLoaded(library.Get(), "child");
  std::printf("OWNED_D3D12_PIPELINE_LIBRARY_CHILD_PASS bytes=%zu\n", serialized.size());
  return 0;
}

void testStageArtifacts(Fixture &f) {
  struct DeveloperMode {
    HKEY key = nullptr; DWORD previous = 0; bool existed = false;
    DeveloperMode() {
      char isolated[2]{};
      if (GetEnvironmentVariableA("OWNED_SHADER_CACHE_CONTROL_ISOLATED", isolated, 2) != 1 || isolated[0] != '1')
        fail("stage cache requires isolated prefix");
      if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
          0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr)) fail("developer registry open");
      DWORD type = 0, size = sizeof(previous);
      existed = RegQueryValueExW(key, L"AllowDevelopmentWithoutDevLicense", nullptr, &type,
                                reinterpret_cast<BYTE*>(&previous), &size) == ERROR_SUCCESS;
      if (existed && (type != REG_DWORD || size != sizeof(previous))) fail("developer registry type");
      const DWORD enabled = 1;
      if (RegSetValueExW(key, L"AllowDevelopmentWithoutDevLicense", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled))) fail("developer registry enable");
    }
    ~DeveloperMode() {
      if (existed) RegSetValueExW(key, L"AllowDevelopmentWithoutDevLicense", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&previous), sizeof(previous));
      else RegDeleteValueW(key, L"AllowDevelopmentWithoutDevLicense");
      RegCloseKey(key);
    }
  } developer;
  ComPtr<ID3D12Device9> control;check(f.device.As(&control), "stage cache device9");
  constexpr auto driver = D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED;
  check(control->ShaderCacheControl(driver, D3D12_SHADER_CACHE_CONTROL_FLAGS(
      D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE | D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)), "stage cache clear");
  // Different raster state gives eight distinct whole-PSO keys, but exactly the same VS/PS lowering inputs.
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=concurrent_graphics\n");std::fflush(stderr);
  std::barrier start(8);std::vector<std::thread> threads;
  ComPtr<ID3D12PipelineState> psos[8];HRESULT results[8]{};
  for (UINT i = 0; i < 8; ++i) threads.emplace_back([&, i] {
    auto desc = f.graphicsDesc();desc.RasterizerState.DepthBias = i;
    start.arrive_and_wait();results[i] = f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&psos[i]));
  });
  for (auto &thread : threads) thread.join();
  for (UINT i = 0; i < 8; ++i) check(results[i], "distinct graphics stage create");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=concurrent_graphics air=2 libraries=2\n");std::fflush(stderr);
  const UINT color[4]{17, 34, 51, 255};
  for (auto &pso : psos) f.draw(pso.Get(), color, 0xff332211u, "distinct_graphics_shared_stages");

  // Same entry bytecode but a different root ABI must make new stage artifacts.
  auto oldRoot = f.graphicsRoot;auto bytes = serializeRoot(8, false);
  f.graphicsRoot = root(f.device.Get(), bytes.Get());
  auto changed = f.graphicsDesc();ComPtr<ID3D12PipelineState> rootPso;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=root_abi\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&rootPso)), "different root ABI create");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=root_abi air=2 libraries_max=2\n");std::fflush(stderr);
  f.draw(rootPso.Get(), color, 0xff332211u, "different_root_abi");f.graphicsRoot = oldRoot;

  // The ordinary PS epilogue consumes SampleMask. VS may share; PS must not.
  changed = f.graphicsDesc();changed.SampleMask = 0;ComPtr<ID3D12PipelineState> masked;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=pixel_specialization\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&masked)), "sample mask specialization");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=pixel_specialization air=1 libraries=1\n");std::fflush(stderr);
  f.draw(masked.Get(), color, 0, "pixel_specialization_separation");

  // Non-full masks retain the same coverage signature and specialize only the scalar.
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=constant_concurrent\n");std::fflush(stderr);
  std::barrier constantStart(8);threads.clear();
  ComPtr<ID3D12PipelineState> constantPsos[8];
  for (UINT i = 0; i < 8; ++i) threads.emplace_back([&, i] {
    auto desc = f.graphicsDesc();desc.SampleMask = 1;desc.RasterizerState.DepthBias = 30 + i;
    constantStart.arrive_and_wait();
    results[i] = f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&constantPsos[i]));
  });
  for (auto &thread : threads) thread.join();
  for (UINT i = 0; i < 8; ++i) check(results[i], "concurrent constant specialization");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=constant_concurrent air=0 libraries=0 specializations=1\n");std::fflush(stderr);
  for (auto &pso : constantPsos) f.draw(pso.Get(), color, 0xff332211u, "constant_mask_one");

  changed = f.graphicsDesc();changed.SampleMask = 2;ComPtr<ID3D12PipelineState> otherMask;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=constant_distinct\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&otherMask)), "second native mask");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=constant_distinct air=0 libraries=0 specializations=1\n");std::fflush(stderr);
  f.draw(otherMask.Get(), color, 0, "constant_mask_two");
  changed.SampleMask = 1;changed.RasterizerState.DepthBias = 41;ComPtr<ID3D12PipelineState> constantHit;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=constant_hit\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&constantHit)), "cached native mask");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=constant_hit air=0 libraries=0 specializations=0\n");std::fflush(stderr);
  f.draw(constantHit.Get(), color, 0xff332211u, "constant_cached_mask_one");

  f.graphicsRoot = root(f.device.Get(), bytes.Get());
  changed = f.graphicsDesc();changed.SampleMask = 1;ComPtr<ID3D12PipelineState> constantRoot;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=constant_root_abi\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&constantRoot)), "constant root separation");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=constant_root_abi air=1 libraries_max=1 specializations=1\n");std::fflush(stderr);
  f.draw(constantRoot.Get(), color, 0xff332211u, "constant_root_separation");f.graphicsRoot = oldRoot;
  changed = f.graphicsDesc();changed.PS = code(f.shaders.inverted.Get());changed.SampleMask = 1;
  ComPtr<ID3D12PipelineState> constantEntry;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=constant_entry\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&constantEntry)), "constant entry separation");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=constant_entry air=1 libraries=1 specializations=1\n");std::fflush(stderr);
  f.draw(constantEntry.Get(), color, 0x00ccddee, "constant_entry_separation");

  auto gs = compile(R"(
    struct Vertex { float4 position : SV_Position; };
    [maxvertexcount(3)] void gs(triangle Vertex input[3], inout TriangleStream<Vertex> output) {
      [unroll] for (uint i = 0; i < 3; ++i) output.Append(input[i]);
    }
  )", "gs", "gs_5_0");
  ComPtr<ID3D12PipelineState> geometry[2];
  for (UINT i = 0; i < 2; ++i) {
    changed = f.graphicsDesc();changed.GS = code(gs.Get());changed.RasterizerState.DepthBias = 50 + i;
    const char *phase = i ? "linked_geometry_hit" : "linked_geometry_cold";
    std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=%s\n", phase);std::fflush(stderr);
    check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&geometry[i])), "linked geometry create");
    f.draw(geometry[i].Get(), color, 0xff332211u, i ? "linked_geometry_reuse" : "linked_geometry_first");
    std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=%s air=%u libraries=%u\n",
                 phase, i ? 0 : 2, i ? 0 : 2);std::fflush(stderr);
  }
  auto linkedVertex = compile(R"(
    float4 vs(uint id : SV_VertexID) : SV_Position {
      float2 uv = float2((id << 1) & 2, id & 2);
      return float4(uv * float2(4, -4) + float2(-2, 2), 0, 2);
    }
  )", "vs", "vs_5_0");
  changed = f.graphicsDesc();changed.GS = code(gs.Get());changed.VS = code(linkedVertex.Get());
  ComPtr<ID3D12PipelineState> changedLink;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=linked_geometry_input\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&changedLink)), "linked VS bytecode separation");
  f.draw(changedLink.Get(), color, 0xff332211u, "linked_geometry_input_separation");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=linked_geometry_input air=2 libraries_max=2\n");std::fflush(stderr);

  ComPtr<ID3D12PipelineState> captured[2];
  for (UINT i = 0; i < 2; ++i) {
    changed = f.graphicsDesc();changed.SampleDesc.Count = 2;changed.RasterizerState.DepthBias = 60 + i;
    changed.PS = {f.shaders.coverage.data(), f.shaders.coverage.size()};
    const char *phase = i ? "captured_layout_hit" : "captured_layout_cold";
    std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=%s\n", phase);std::fflush(stderr);
    check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&captured[i])), "captured layout create");
    f.draw(captured[i].Get(), color, 0xff332211u, i ? "captured_layout_reuse" : "captured_layout_first", 2);
    std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=%s air=%u libraries_max=%u\n",
                 phase, i ? 0 : 5, i ? 0 : 5);std::fflush(stderr);
  }

  changed = f.graphicsDesc();changed.SampleDesc.Count = 2;changed.RasterizerState.DepthBias = 62;
  ComPtr<ID3D12PipelineState> ordinaryMSAA;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_NATIVE_MSAA_BEGIN count=2\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&ordinaryMSAA)), "ordinary native2x create");
  f.draw(ordinaryMSAA.Get(), color, 0xff332211u, "ordinary_native2x", 2);
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_NATIVE_MSAA_END count=2 resolved=ff332211\n");std::fflush(stderr);

  check(control->ShaderCacheControl(driver, D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR), "live stage cache clear");
  changed = f.graphicsDesc();changed.RasterizerState.DepthBias = 19;ComPtr<ID3D12PipelineState> rebuilt;
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE begin=clear_rebuild\n");std::fflush(stderr);
  check(f.device->CreateGraphicsPipelineState(&changed, IID_PPV_ARGS(&rebuilt)), "stage clear rebuild");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_LIBRARY_ARTIFACT_PHASE end=clear_rebuild air=2 libraries=2\n");std::fflush(stderr);
  // Native function/PSO ownership survives cache eviction; reuse the original object after rebuilding.
  f.draw(psos[0].Get(), color, 0xff332211u, "stage_library_retained_after_clear");
  f.draw(rebuilt.Get(), color, 0xff332211u, "stage_library_clear_rebuilt");
  f.draw(constantPsos[0].Get(), color, 0xff332211u, "constant_retained_after_clear");
  f.draw(otherMask.Get(), color, 0, "constant_zero_retained_after_clear");
  passed("immutable_stage_native_library_reuse");
}

int run() {
  using BackendId = const char *(WINAPI *)();
  auto identity = reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!identity || std::strcmp(identity(), "owned-graphics-d3d12"))
    fail("backend identity");
  std::printf("OWNED_D3D12_PIPELINE_LIBRARY_BACKEND id=%s\n", identity());
  std::vector<uint8_t> serialized;
  {
    Fixture f;
    testFeatures(f);
    testStageArtifacts(f);
    testStreamCoverage(f);
    testContracts(f, serialized);
    testThreads(f, serialized);
  }
  runChild(serialized);
  std::puts("OWNED_D3D12_PIPELINE_LIBRARY_PASS");
  return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    if (argc == 3 && !std::wcscmp(argv[1], L"--child"))
      return child(argv[2]);
    return argc == 1 ? run() : 2;
  } catch (const std::exception &) {
    return 1;
  }
}
