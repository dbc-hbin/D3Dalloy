// Resource Binding Tier 3 / SM 6.6 dynamic resources / root signature 1.2 fixture.
//
// Every case checks GPU results or API error codes against an exact CPU oracle:
//   * cs66:        ResourceDescriptorHeap / SamplerDescriptorHeap over a 1,000,000
//                  descriptor heap (index 999,999), >64 divergent UAVs, CBV, typed/raw/
//                  structured SRV, 1D/2D-array/3D/cube textures, comparison sampling,
//                  typed atomics, UAV counter, RWTexture2D store, null descriptors and
//                  indices past the heap (bindless-cs.dxil).
//   * ps66:        per-pixel divergent NonUniformResourceIndex texture + sampler heap
//                  indices in a pixel shader, index 999,998 (bindless-{vs,ps}.dxil).
//   * dxbc51:      SM 5.1 unbounded SRV/UAV/sampler arrays with NonUniformResourceIndex in
//                  CS and PS, register spaces, >64 UAVs.
//   * heap-order:  heaps set after the root signature, heaps changed between dispatches,
//                  no sampler heap bound (null descriptors).
//   * rootsig:     DIRECTLY_INDEXED flag acceptance, rejection on local signatures,
//                  PSO creation fails when a heap-indexing shader lacks the flag.
//   * rs12:        1.2 serialize / deserialize / versioned conversions, static sampler flag
//                  validation, UINT_BORDER_COLOR and NON_NORMALIZED_COORDINATES sampling.
//   * caps:        CheckFeatureSupport ResourceBindingTier / ROOT_SIGNATURE.
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

// D3D12 root signature 1.2 declarations (DirectX-Headers v1.614.1); the llvm-mingw
// d3d12.h this fixture builds with predates them.
constexpr D3D_ROOT_SIGNATURE_VERSION kVersion_1_2 = static_cast<D3D_ROOT_SIGNATURE_VERSION>(3);
constexpr UINT kFlagResourceHeap = 0x400;
constexpr UINT kFlagSamplerHeap = 0x800;
constexpr UINT kSamplerFlagUintBorder = 0x1;
constexpr UINT kSamplerFlagNonNormalized = 0x2;
constexpr UINT kBorderOpaqueBlackUint = 3;
constexpr UINT kBorderOpaqueWhiteUint = 4;
struct StaticSamplerDesc1 {
  D3D12_STATIC_SAMPLER_DESC base;
  UINT Flags;
};
struct RootSignatureDesc2 {
  UINT NumParameters;
  const D3D12_ROOT_PARAMETER1 *pParameters;
  UINT NumStaticSamplers;
  const StaticSamplerDesc1 *pStaticSamplers;
  D3D12_ROOT_SIGNATURE_FLAGS Flags;
};
struct VersionedDesc {
  D3D_ROOT_SIGNATURE_VERSION Version;
  union {
    D3D12_ROOT_SIGNATURE_DESC Desc_1_0;
    D3D12_ROOT_SIGNATURE_DESC1 Desc_1_1;
    RootSignatureDesc2 Desc_1_2;
  };
};

constexpr DWORD kFenceTimeoutMs = 20000;
int g_failures = 0;

#define EXPECT(cond, ...)                                                                                  \
  do {                                                                                                     \
    if (!(cond)) {                                                                                         \
      std::printf("OWNED_D3D12_BINDLESS_MISMATCH line=%d ", __LINE__);                                     \
      std::printf(__VA_ARGS__);                                                                            \
      std::printf("\n");                                                                                   \
      ++g_failures;                                                                                        \
    }                                                                                                      \
  } while (0)

void die(const char *step, HRESULT hr = E_FAIL) {
  std::printf("OWNED_D3D12_BINDLESS_FAIL step=%s hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
  std::fflush(stdout);
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    die(step, hr);
}

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    die("open shader", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size())
    die("read shader");
  CloseHandle(file);
  return data;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = p.VisibleNodeMask = 1;
  return p;
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 value = 0;
  UINT viewStride = 0;
  UINT samplerStride = 0;

  void init(ID3D12Device *dev) {
    device = dev;
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "list");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    viewStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    samplerStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  }
  void submit(const char *what) {
    check(list->Close(), what);
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++value), "signal");
    check(fence->SetEventOnCompletion(value, event), "event");
    if (WaitForSingleObject(event, kFenceTimeoutMs) != WAIT_OBJECT_0)
      die(what);
    check(device->GetDeviceRemovedReason(), what);
    check(allocator->Reset(), "allocator reset");
    check(list->Reset(allocator.Get(), nullptr), "list reset");
  }

  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    auto props = heapProps(type);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
          "buffer");
    return r;
  }
  ComPtr<ID3D12Resource> uploadData(const void *data, UINT64 size) {
    auto r = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p = nullptr;
    check(r->Map(0, nullptr, &p), "map upload");
    std::memcpy(p, data, size_t(size));
    r->Unmap(0, nullptr);
    return r;
  }
  // A default-heap buffer initialised from `words`, left in `state`.
  ComPtr<ID3D12Resource> initBuffer(const std::vector<uint32_t> &words, D3D12_RESOURCE_STATES state,
                                    std::vector<ComPtr<ID3D12Resource>> &keep,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    const UINT64 size = words.size() * 4;
    auto r = buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, flags);
    auto up = uploadData(words.data(), size);
    list->CopyBufferRegion(r.Get(), 0, up.Get(), 0, size);
    barrier(r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
    keep.push_back(up);
    return r;
  }
  ComPtr<ID3D12Resource> texture(D3D12_RESOURCE_DIMENSION dim, DXGI_FORMAT format, UINT w, UINT h, UINT16 depth,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    auto props = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = dim;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = depth;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&r)),
          "texture");
    return r;
  }
  // Upload `texel(x, y, z)` (4 bytes each) into every subresource slice/depth of a 1-mip texture.
  template <typename F>
  void fillTexture(ID3D12Resource *tex, UINT subresources, F texel, std::vector<ComPtr<ID3D12Resource>> &keep,
                   D3D12_RESOURCE_STATES after) {
    auto desc = tex->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subresources);
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> rowBytes(subresources);
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, subresources, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    std::vector<uint8_t> data(static_cast<size_t>(total));
    for (UINT s = 0; s < subresources; ++s)
      for (UINT z = 0; z < fp[s].Footprint.Depth; ++z)
        for (UINT y = 0; y < fp[s].Footprint.Height; ++y)
          for (UINT x = 0; x < fp[s].Footprint.Width; ++x) {
            uint32_t v = texel(x, y, subresources > 1 ? s : z);
            std::memcpy(data.data() + fp[s].Offset + (z * fp[s].Footprint.Height + y) * fp[s].Footprint.RowPitch + x * 4,
                        &v, 4);
          }
    auto up = uploadData(data.data(), total);
    for (UINT s = 0; s < subresources; ++s) {
      D3D12_TEXTURE_COPY_LOCATION dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.SubresourceIndex = s;
      D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.PlacedFootprint = fp[s];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    keep.push_back(up);
  }
  void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &b);
  }
  void uavBarrier() {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &b);
  }
  // Copy `src` (in `state`) to a readback buffer after the list is submitted.
  std::vector<uint32_t> readBuffer(ID3D12Resource *src, D3D12_RESOURCE_STATES state, UINT64 size, const char *what) {
    auto rb = buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(rb.Get(), 0, src, 0, size);
    barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    submit(what);
    std::vector<uint32_t> out(size_t(size / 4));
    void *p = nullptr;
    D3D12_RANGE range{0, size_t(size)};
    check(rb->Map(0, &range, &p), "map readback");
    std::memcpy(out.data(), p, size_t(size));
    rb->Unmap(0, nullptr);
    return out;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cpu(ID3D12DescriptorHeap *heap, UINT index, UINT stride) {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * stride;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu(ID3D12DescriptorHeap *heap, UINT index, UINT stride) {
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * stride;
    return h;
  }
  ComPtr<ID3D12DescriptorHeap> heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible = true) {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = type;
    d.NumDescriptors = count;
    d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> h;
    check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)), "descriptor heap");
    return h;
  }

  void rawSrv(ID3D12DescriptorHeap *h, UINT index, ID3D12Resource *r, UINT firstDword, UINT dwords) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.FirstElement = firstDword;
    d.Buffer.NumElements = dwords;
    d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(r, &d, cpu(h, index, viewStride));
  }
  void rawUav(ID3D12DescriptorHeap *h, UINT index, ID3D12Resource *r, UINT firstDword, UINT dwords) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.FirstElement = firstDword;
    d.Buffer.NumElements = dwords;
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(r, nullptr, &d, cpu(h, index, viewStride));
  }
  void texSrv(ID3D12DescriptorHeap *h, UINT index, ID3D12Resource *r, DXGI_FORMAT format,
              D3D12_SRV_DIMENSION dim, UINT arraySize = 1) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = dim;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dim) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
      d.Texture1D.MipLevels = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      d.Texture2D.MipLevels = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      d.Texture2DArray.MipLevels = 1;
      d.Texture2DArray.ArraySize = arraySize;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      d.Texture3D.MipLevels = 1;
      break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      d.TextureCube.MipLevels = 1;
      break;
    default:
      break;
    }
    device->CreateShaderResourceView(r, &d, cpu(h, index, viewStride));
  }
  void sampler(ID3D12DescriptorHeap *h, UINT index, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address,
               D3D12_COMPARISON_FUNC cmp = D3D12_COMPARISON_FUNC_NEVER) {
    D3D12_SAMPLER_DESC s{};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = address;
    s.ComparisonFunc = cmp;
    s.MaxAnisotropy = 1;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&s, cpu(h, index, samplerStride));
  }
};

ComPtr<ID3D12RootSignature> makeRoot(ID3D12Device *device, const VersionedDesc &desc, HRESULT *create_hr = nullptr) {
  ComPtr<ID3DBlob> blob, error;
  HRESULT hr = D3D12SerializeVersionedRootSignature(reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&desc),
                                                    &blob, &error);
  if (FAILED(hr)) {
    if (create_hr) {
      *create_hr = hr;
      return nullptr;
    }
    die("serialize root", hr);
  }
  ComPtr<ID3D12RootSignature> root;
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
  if (create_hr)
    *create_hr = hr;
  else
    check(hr, "create root");
  return root;
}

// One root constant at b0 plus optional parameters, DIRECTLY_INDEXED flags.
VersionedDesc directRootDesc(const D3D12_ROOT_PARAMETER1 *params, UINT count, UINT flags) {
  VersionedDesc d{};
  d.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  d.Desc_1_1 = {count, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAGS(flags)};
  return d;
}

ComPtr<ID3D12PipelineState> computePso(ID3D12Device *device, ID3D12RootSignature *root, const void *code, size_t size,
                                       HRESULT *hr_out = nullptr) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
  d.pRootSignature = root;
  d.CS = {code, size};
  ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
  if (hr_out)
    *hr_out = hr;
  else
    check(hr, "compute pso");
  return pso;
}

uint32_t f2u(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

// Rows of subresource 0 of a 2D texture, `bpp` bytes per texel, packed tightly.
std::vector<uint32_t> readTexture(Gpu &g, ID3D12Resource *tex, D3D12_RESOURCE_STATES state, UINT bpp,
                                  const char *what) {
  auto desc = tex->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  g.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  auto rb = g.buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  g.barrier(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = fp;
  D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = 0;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  g.barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  g.submit(what);
  std::vector<uint32_t> out(size_t(desc.Width) * desc.Height * bpp / 4);
  uint8_t *p = nullptr;
  check(rb->Map(0, nullptr, reinterpret_cast<void **>(&p)), "map texture readback");
  for (UINT y = 0; y < desc.Height; ++y)
    std::memcpy(reinterpret_cast<uint8_t *>(out.data()) + size_t(y) * desc.Width * bpp, p + fp.Offset + y * fp.Footprint.RowPitch,
                size_t(desc.Width) * bpp);
  rb->Unmap(0, nullptr);
  return out;
}

constexpr UINT kHeapSize = 1000000;
constexpr UINT kSamplerHeapSize = 2048;

/* The shared 1,000,000 / 2048 heaps and every resource the SM 6.6 cases index. */
struct BindlessScene {
  ComPtr<ID3D12DescriptorHeap> views, samplers, dsv;
  std::vector<ComPtr<ID3D12Resource>> keep;
  ComPtr<ID3D12Resource> output, many, atomics, counted, counter, image;
};

void buildScene(Gpu &g, BindlessScene &s) {
  auto *dev = g.device.Get();
  s.views = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize);
  s.samplers = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  auto *v = s.views.Get();

  s.output = g.initBuffer(std::vector<uint32_t>(1024, 0xcccccccc), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  g.rawUav(v, 999999, s.output.Get(), 0, 1024);

  s.many = g.initBuffer(std::vector<uint32_t>(320, 0), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  for (UINT k = 0; k < 80; ++k)
    g.rawUav(v, 100 + k, s.many.Get(), 4 * k, 4);

  std::vector<uint32_t> cb(256, 0);
  for (UINT k = 0; k < 4; ++k)
    cb[64 * k] = 0x1000 + k;
  auto cbuf = g.initBuffer(cb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, s.keep, D3D12_RESOURCE_FLAG_NONE);
  s.keep.push_back(cbuf);
  for (UINT k = 0; k < 4; ++k) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC c{cbuf->GetGPUVirtualAddress() + 256 * k, 256};
    dev->CreateConstantBufferView(&c, g.cpu(v, 200 + k, g.viewStride));
  }

  std::vector<uint32_t> typed(16), structured(16), raw(16);
  for (UINT e = 0; e < 16; ++e) {
    typed[e] = 0x2000 + e;
    raw[e] = 0x2200 + e;
  }
  for (UINT e = 0; e < 8; ++e) {
    structured[2 * e] = e;
    structured[2 * e + 1] = 0x2100 + e;
  }
  auto typedBuf = g.initBuffer(typed, kShaderRead, s.keep);
  auto structBuf = g.initBuffer(structured, kShaderRead, s.keep);
  auto rawBuf = g.initBuffer(raw, kShaderRead, s.keep);
  s.keep.insert(s.keep.end(), {typedBuf, structBuf, rawBuf});
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Format = DXGI_FORMAT_R32_UINT;
  srv.Buffer.NumElements = 16;
  dev->CreateShaderResourceView(typedBuf.Get(), &srv, g.cpu(v, 300, g.viewStride));
  srv.Format = DXGI_FORMAT_UNKNOWN;
  srv.Buffer.NumElements = 8;
  srv.Buffer.StructureByteStride = 8;
  dev->CreateShaderResourceView(structBuf.Get(), &srv, g.cpu(v, 301, g.viewStride));
  g.rawSrv(v, 302, rawBuf.Get(), 0, 16);

  std::vector<uint32_t> extra{0, 0x77777, 0, 0};
  auto extraBuf = g.initBuffer(extra, kShaderRead, s.keep);
  s.keep.push_back(extraBuf);
  g.rawSrv(v, 999990, extraBuf.Get(), 0, 4);

  s.atomics = g.initBuffer(std::vector<uint32_t>(4, 0), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Format = DXGI_FORMAT_R32_UINT;
  uav.Buffer.NumElements = 4;
  dev->CreateUnorderedAccessView(s.atomics.Get(), nullptr, &uav, g.cpu(v, 303, g.viewStride));
  s.counted = g.initBuffer(std::vector<uint32_t>(64, 0), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  s.counter = g.initBuffer(std::vector<uint32_t>(1024, 0), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  uav.Format = DXGI_FORMAT_UNKNOWN;
  uav.Buffer.NumElements = 64;
  uav.Buffer.StructureByteStride = 4;
  dev->CreateUnorderedAccessView(s.counted.Get(), s.counter.Get(), &uav, g.cpu(v, 304, g.viewStride));

  auto t1 = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE1D, DXGI_FORMAT_R32_UINT, 4, 1, 1);
  g.fillTexture(t1.Get(), 1, [](UINT x, UINT, UINT) { return 0x3000 + x; }, s.keep, kShaderRead);
  g.texSrv(v, 400, t1.Get(), DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE1D);
  auto t2a = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_UINT, 2, 2, 3);
  g.fillTexture(t2a.Get(), 3, [](UINT x, UINT y, UINT z) { return 0x3100 + x + 2 * y + 4 * z; }, s.keep, kShaderRead);
  g.texSrv(v, 401, t2a.Get(), DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2DARRAY, 3);
  auto t3 = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R32_UINT, 2, 2, 2);
  g.fillTexture(t3.Get(), 1, [](UINT x, UINT y, UINT z) { return 0x3200 + x + 2 * y + 4 * z; }, s.keep, kShaderRead);
  g.texSrv(v, 402, t3.Get(), DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE3D);
  auto t2 = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 2, 2, 1);
  g.fillTexture(t2.Get(), 1, [](UINT x, UINT y, UINT) { return f2u(float(10 + x + 2 * y)); }, s.keep, kShaderRead);
  g.texSrv(v, 403, t2.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D);
  auto cube = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 1, 1, 6);
  g.fillTexture(cube.Get(), 6, [](UINT, UINT, UINT f) { return f2u(float(20 + f)); }, s.keep, kShaderRead);
  g.texSrv(v, 404, cube.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURECUBE);
  s.keep.insert(s.keep.end(), {t1, t2a, t3, t2, cube});

  {
    auto props = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = d.Height = 2;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT};
    clear.DepthStencil.Depth = 0.5f;
    ComPtr<ID3D12Resource> depth;
    check(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                       IID_PPV_ARGS(&depth)),
          "depth");
    auto dsvHeap = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
    dev->CreateDepthStencilView(depth.Get(), &dsv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
    g.list->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0,
                                  nullptr);
    g.barrier(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kShaderRead);
    g.texSrv(v, 405, depth.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D);
    s.keep.push_back(depth);
    s.dsv = dsvHeap;
  }

  s.image = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_UINT, 4, 4, 1,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  g.barrier(s.image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  uav = {};
  uav.Format = DXGI_FORMAT_R32_UINT;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  dev->CreateUnorderedAccessView(s.image.Get(), nullptr, &uav, g.cpu(v, 406, g.viewStride));

  // Null descriptors 500..503.
  g.rawSrv(v, 500, nullptr, 0, 4);
  g.rawUav(v, 501, nullptr, 0, 4);
  g.texSrv(v, 502, nullptr, DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2D);
  dev->CreateConstantBufferView(nullptr, g.cpu(v, 503, g.viewStride));

  // Pixel-shader textures 600..603 and 999998.
  for (UINT k = 0; k < 4; ++k) {
    auto t = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 1, 1, 1);
    g.fillTexture(t.Get(), 1, [k](UINT, UINT, UINT) { return f2u(float(100 + k)); }, s.keep, kShaderRead);
    g.texSrv(v, 600 + k, t.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D);
    s.keep.push_back(t);
  }
  auto farTexture = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 1, 1, 1);
  g.fillTexture(farTexture.Get(), 1, [](UINT, UINT, UINT) { return f2u(7.0f); }, s.keep, kShaderRead);
  g.texSrv(v, 999998, farTexture.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D);
  s.keep.push_back(farTexture);

  auto *sm = s.samplers.Get();
  g.sampler(sm, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
  g.sampler(sm, 5, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_COMPARISON_FUNC_LESS);
  g.sampler(sm, 6, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
  g.sampler(sm, 7, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  g.sampler(sm, 2047, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  g.submit("scene upload");
}

D3D12_ROOT_PARAMETER1 rootConstant(UINT reg, UINT count = 1, UINT space = 0) {
  D3D12_ROOT_PARAMETER1 p{};
  p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p.Constants = {reg, space, count};
  p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  return p;
}
D3D12_ROOT_PARAMETER1 rootDescriptor(D3D12_ROOT_PARAMETER_TYPE type, UINT reg, UINT space = 0) {
  D3D12_ROOT_PARAMETER1 p{};
  p.ParameterType = type;
  p.Descriptor = {reg, space, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
  p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  return p;
}

void passCase(const char *name, int before) {
  std::printf("OWNED_D3D12_BINDLESS_CASE %s status=%s\n", name, g_failures == before ? "PASS" : "FAIL");
  std::fflush(stdout);
}

void testCompute66(Gpu &g, BindlessScene &s, const std::vector<uint8_t> &cs) {
  const int before = g_failures;
  auto param = rootConstant(0);
  auto root = makeRoot(g.device.Get(), directRootDesc(&param, 1, kFlagResourceHeap | kFlagSamplerHeap));
  auto pso = computePso(g.device.Get(), root.Get(), cs.data(), cs.size());
  ID3D12DescriptorHeap *heaps[] = {s.views.Get(), s.samplers.Get()};
  g.list->SetDescriptorHeaps(2, heaps);
  g.list->SetComputeRootSignature(root.Get());
  g.list->SetPipelineState(pso.Get());
  g.list->SetComputeRoot32BitConstant(0, 999990, 0);
  g.list->Dispatch(1, 1, 1);
  g.uavBarrier();
  g.submit("cs66 dispatch");

  auto out = g.readBuffer(s.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4096, "cs66 output");
  std::vector<bool> slots(64, false);
  for (UINT i = 0; i < 64; ++i) {
    const uint32_t *w = out.data() + i * 16;
    const uint32_t expect[16] = {
        0x1000 + i % 4,
        0x2000 + i % 16,
        0x2100 + i % 8,
        0x2200 + i % 16,
        0x3000 + i % 4,
        0x3100 + i % 2 + 2 * ((i / 2) % 2) + 4 * (i % 3),
        0x3200 + i % 2 + 2 * ((i / 2) % 2) + 4 * ((i / 4) % 2),
        f2u((i % 2) ? 11.0f : 10.0f),
        f2u(float(20 + i % 6)),
        f2u((i % 2) ? 1.0f : 0.0f),
        0x5a000000,
        0x5b000000,
        0x5c000000,
        w[13],
        0x77777,
        0xb1d1e500 + i,
    };
    static const char *names[16] = {"cbv", "typed", "structured", "raw", "tex1d", "tex2darray", "tex3d",
                                    "sampler", "cube", "cmp", "null-srv", "null-uav", "null-cbv", "counter",
                                    "root-index", "tag"};
    for (UINT k = 0; k < 16; ++k)
      EXPECT(w[k] == expect[k], "cs66 lane=%u %s got=0x%08x expected=0x%08x", i, names[k], w[k], expect[k]);
    EXPECT(w[13] < 64 && !slots[w[13]], "cs66 lane=%u counter slot %u not unique", i, w[13]);
    if (w[13] < 64)
      slots[w[13]] = true;
  }
  auto many = g.readBuffer(s.many.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1280, "cs66 many");
  for (UINT k = 0; k < 80; ++k) {
    uint32_t expect = 0;
    for (UINT i = 0; i < 64; ++i)
      if ((i * 7) % 80 == k)
        expect = i + 1;
    EXPECT(many[4 * k] == expect, "cs66 uav view=%u got=%u expected=%u", 100 + k, many[4 * k], expect);
  }
  auto atomics = g.readBuffer(s.atomics.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 16, "cs66 atomics");
  for (UINT k = 0; k < 4; ++k)
    EXPECT(atomics[k] == 16, "cs66 atomic[%u]=%u expected 16", k, atomics[k]);
  auto counter = g.readBuffer(s.counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4, "cs66 counter");
  EXPECT(counter[0] == 64, "cs66 counter=%u expected 64", counter[0]);
  auto counted = g.readBuffer(s.counted.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, "cs66 counted");
  for (UINT i = 0; i < 64; ++i) {
    const uint32_t slot = out[i * 16 + 13];
    if (slot < 64)
      EXPECT(counted[slot] == i + 1, "cs66 counted[%u]=%u expected %u", slot, counted[slot], i + 1);
  }
  auto image = readTexture(g, s.image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4, "cs66 image");
  for (UINT t = 0; t < 16; ++t)
    EXPECT(image[t] == 0x3300 + t, "cs66 image[%u]=0x%x", t, image[t]);
  passCase("cs66-dynamic-resources", before);
}

ComPtr<ID3D12PipelineState> graphicsPso(ID3D12Device *device, ID3D12RootSignature *root, const void *vs, size_t vsSize,
                                        const void *ps, size_t psSize, DXGI_FORMAT rtv, HRESULT *hr_out = nullptr) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = root;
  d.VS = {vs, vsSize};
  d.PS = {ps, psSize};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = rtv;
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  if (hr_out)
    *hr_out = hr;
  else
    check(hr, "graphics pso");
  return pso;
}

/* Draw a fullscreen triangle into a 4x4 target and return its texels. */
std::vector<uint32_t> drawQuad(Gpu &g, ID3D12PipelineState *pso, ID3D12RootSignature *root, DXGI_FORMAT format,
                               UINT bpp, void (*bind)(Gpu &, void *), void *context, const char *what) {
  auto target = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, format, 4, 4, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto rtvHeap = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
  g.device->CreateRenderTargetView(target.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
  g.barrier(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  const float clear[4] = {0, 0, 0, 0};
  g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT vp{0, 0, 4, 4, 0, 1};
  D3D12_RECT sc{0, 0, 4, 4};
  g.list->RSSetViewports(1, &vp);
  g.list->RSSetScissorRects(1, &sc);
  g.list->SetGraphicsRootSignature(root);
  bind(g, context);
  g.list->SetPipelineState(pso);
  g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g.list->DrawInstanced(3, 1, 0, 0);
  return readTexture(g, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, bpp, what);
}

void testPixel66(Gpu &g, BindlessScene &s, const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps) {
  const int before = g_failures;
  auto root = makeRoot(g.device.Get(), directRootDesc(nullptr, 0, kFlagResourceHeap | kFlagSamplerHeap));
  auto pso = graphicsPso(g.device.Get(), root.Get(), vs.data(), vs.size(), ps.data(), ps.size(),
                         DXGI_FORMAT_R32G32B32A32_FLOAT);
  auto texels = drawQuad(
      g, pso.Get(), root.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 16,
      [](Gpu &g, void *c) {
        auto *s = static_cast<BindlessScene *>(c);
        ID3D12DescriptorHeap *heaps[] = {s->views.Get(), s->samplers.Get()};
        g.list->SetDescriptorHeaps(2, heaps);
      },
      &s, "ps66 draw");
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 4; ++x) {
      const float *t = reinterpret_cast<const float *>(texels.data() + (y * 4 + x) * 4);
      const float expect = float(100 + x) + (x == 3 ? 7000.0f : 0.0f);
      EXPECT(t[0] == expect && t[1] == float(y) && t[2] == 0.0f && t[3] == 1.0f,
             "ps66 pixel=(%u,%u) got=(%g,%g,%g,%g) expected=(%g,%u,0,1)", x, y, t[0], t[1], t[2], t[3], expect, y);
    }
  passCase("ps66-divergent-heap-sampling", before);
}

ComPtr<ID3DBlob> compileDxbc(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> blob, errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), "bindless-dxbc", nullptr, nullptr, entry, target, 0, 0, &blob,
                          &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stdout);
    die("D3DCompile", hr);
  }
  return blob;
}

/*
 * SM 5.1 DXBC unbounded arrays (Tier 3 layout), frozen from dxil/bindless-dxbc51.hlsl
 * with Microsoft FXC: Wine's d3dcompiler cannot compile unbounded arrays.
 */

void testDxbc51(Gpu &g, BindlessScene &s) {
  const int before = g_failures;
  const auto cs = readFile(L"bindless-dxbc51-cs.dxbc");
  const auto vs = readFile(L"bindless-dxbc51-vs.dxbc");
  const auto ps = readFile(L"bindless-dxbc51-ps.dxbc");

  // Table layout in the shared heap: UAV 100..179 (80), SRV table at 700 (16 raw views),
  // texture table at 600 (4), sampler table at sampler 6 (6 clamp, 7 wrap).
  std::vector<uint32_t> srcWords(64);
  for (UINT k = 0; k < 16; ++k)
    srcWords[4 * k] = 0x4000 + k;
  auto src = g.initBuffer(srcWords, kShaderRead, s.keep);
  s.keep.push_back(src);
  for (UINT k = 0; k < 16; ++k)
    g.rawSrv(s.views.Get(), 700 + k, src.Get(), 4 * k, 4);
  auto result = g.initBuffer(std::vector<uint32_t>(128, 0xcccccccc), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  auto many = g.initBuffer(std::vector<uint32_t>(320, 0), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.keep);
  for (UINT k = 0; k < 80; ++k)
    g.rawUav(s.views.Get(), 800 + k, many.Get(), 4 * k, 4);
  g.submit("dxbc51 upload");

  D3D12_DESCRIPTOR_RANGE1 uavRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, 0,
                                   D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_DESCRIPTOR_RANGE1 srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1,
                                   D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_DESCRIPTOR_RANGE1 texRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2,
                                   D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_DESCRIPTOR_RANGE1 sampRange{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 0,
                                    D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 params[5]{};
  const D3D12_DESCRIPTOR_RANGE1 *ranges[4] = {&uavRange, &srvRange, &texRange, &sampRange};
  for (UINT k = 0; k < 4; ++k) {
    params[k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[k].DescriptorTable = {1, ranges[k]};
    params[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[4] = rootDescriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0, 3);
  auto root = makeRoot(g.device.Get(), directRootDesc(params, 5, 0));
  auto pso = computePso(g.device.Get(), root.Get(), cs.data(), cs.size());
  ID3D12DescriptorHeap *heaps[] = {s.views.Get(), s.samplers.Get()};
  g.list->SetDescriptorHeaps(2, heaps);
  g.list->SetComputeRootSignature(root.Get());
  g.list->SetPipelineState(pso.Get());
  g.list->SetComputeRootDescriptorTable(0, g.gpu(s.views.Get(), 800, g.viewStride));
  g.list->SetComputeRootDescriptorTable(1, g.gpu(s.views.Get(), 700, g.viewStride));
  g.list->SetComputeRootDescriptorTable(2, g.gpu(s.views.Get(), 600, g.viewStride));
  g.list->SetComputeRootDescriptorTable(3, g.gpu(s.samplers.Get(), 6, g.samplerStride));
  g.list->SetComputeRootUnorderedAccessView(4, result->GetGPUVirtualAddress());
  g.list->Dispatch(1, 1, 1);
  g.uavBarrier();
  g.submit("dxbc51 dispatch");
  auto out = g.readBuffer(result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 512, "dxbc51 result");
  for (UINT i = 0; i < 64; ++i) {
    EXPECT(out[2 * i] == 0x4000 + i % 16, "dxbc51 cs lane=%u srv got=0x%x", i, out[2 * i]);
    EXPECT(out[2 * i + 1] == f2u(float(100 + i % 4)), "dxbc51 cs lane=%u tex got=0x%x", i, out[2 * i + 1]);
  }
  auto uavs = g.readBuffer(many.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 1280, "dxbc51 uavs");
  for (UINT k = 0; k < 80; ++k) {
    uint32_t expect = 0;
    for (UINT i = 0; i < 64; ++i)
      if ((i * 7) % 80 == k)
        expect = i + 1;
    EXPECT(uavs[4 * k] == expect, "dxbc51 cs uav[%u]=%u expected %u", k, uavs[4 * k], expect);
  }

  struct Bind {
    BindlessScene *s;
    ID3D12RootSignature *root;
  } bind{&s, root.Get()};
  auto gpso = graphicsPso(g.device.Get(), root.Get(), vs.data(), vs.size(), ps.data(), ps.size(),
                          DXGI_FORMAT_R32G32B32A32_FLOAT);
  auto texels = drawQuad(
      g, gpso.Get(), root.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 16,
      [](Gpu &g, void *c) {
        auto *b = static_cast<Bind *>(c);
        ID3D12DescriptorHeap *heaps[] = {b->s->views.Get(), b->s->samplers.Get()};
        g.list->SetDescriptorHeaps(2, heaps);
        g.list->SetGraphicsRootDescriptorTable(0, g.gpu(b->s->views.Get(), 800, g.viewStride));
        g.list->SetGraphicsRootDescriptorTable(1, g.gpu(b->s->views.Get(), 700, g.viewStride));
        g.list->SetGraphicsRootDescriptorTable(2, g.gpu(b->s->views.Get(), 600, g.viewStride));
        g.list->SetGraphicsRootDescriptorTable(3, g.gpu(b->s->samplers.Get(), 6, g.samplerStride));
        g.list->SetGraphicsRootUnorderedAccessView(4, 0);
      },
      &bind, "dxbc51 draw");
  for (UINT y = 0; y < 4; ++y)
    for (UINT x = 0; x < 4; ++x) {
      const uint32_t *t = texels.data() + (y * 4 + x) * 4;
      EXPECT(t[0] == f2u(float(100 + x)) && t[1] == 0x4000 + y * 4 + x && t[2] == 0 && t[3] == f2u(1.0f),
             "dxbc51 ps pixel=(%u,%u) got=(0x%x,0x%x,0x%x,0x%x)", x, y, t[0], t[1], t[2], t[3]);
    }
  s.keep.insert(s.keep.end(), {result, many});
  passCase("dxbc51-unbounded-nonuniform", before);
}

/*
 * SetDescriptorHeaps / Set*RootSignature interplay for heap-indexed shaders:
 *   * heaps set before the signature (the order D3D12 requires) resolve that heap;
 *   * switching heaps and re-setting the same signature between dispatches resolves the
 *     new heap (the signature object is unchanged, only the heap binding moved);
 *   * an index past the bound heap is a null descriptor (reads zero), even when a larger
 *     heap was bound earlier in the same list.
 */
void testHeapOrder(Gpu &g, const std::vector<uint8_t> &cs) {
  const int before = g_failures;
  std::vector<ComPtr<ID3D12Resource>> keep;
  ComPtr<ID3D12DescriptorHeap> heaps[2] = {g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16),
                                           g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16)};
  auto samplers = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1);
  g.sampler(samplers.Get(), 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
  for (UINT h = 0; h < 2; ++h) {
    auto raw = g.initBuffer({0xa1u + 0x11u * h, 0, 0, 0}, kShaderRead, keep);
    auto tex = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 1, 1, 1);
    g.fillTexture(tex.Get(), 1, [h](UINT, UINT, UINT) { return f2u(1.5f + h); }, keep, kShaderRead);
    for (UINT k = 0; k < 16; ++k)
      g.rawSrv(heaps[h].Get(), k, nullptr, 0, 4);
    g.rawSrv(heaps[h].Get(), 3, raw.Get(), 0, 4);
    g.texSrv(heaps[h].Get(), 4, tex.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D);
    keep.insert(keep.end(), {raw, tex});
  }
  auto result = g.initBuffer(std::vector<uint32_t>(12, 0xcccccccc), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
  g.submit("heap order upload");

  D3D12_ROOT_PARAMETER1 params[2] = {rootConstant(0), rootDescriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
  auto root = makeRoot(g.device.Get(), directRootDesc(params, 2, kFlagResourceHeap | kFlagSamplerHeap));
  auto pso = computePso(g.device.Get(), root.Get(), cs.data(), cs.size());
  const UINT64 va = result->GetGPUVirtualAddress();
  auto dispatch = [&](UINT heap, UINT index, UINT slot) {
    ID3D12DescriptorHeap *bound[] = {heaps[heap].Get(), samplers.Get()};
    g.list->SetDescriptorHeaps(2, bound);
    g.list->SetComputeRootSignature(root.Get());
    g.list->SetPipelineState(pso.Get());
    g.list->SetComputeRoot32BitConstant(0, index, 0);
    g.list->SetComputeRootUnorderedAccessView(1, va + slot * 16);
    g.list->Dispatch(1, 1, 1);
  };
  dispatch(0, 3, 0);
  dispatch(1, 3, 1);
  dispatch(0, 15, 2); // index 16 is past the 16-descriptor heap
  g.uavBarrier();
  g.submit("heap order dispatch");
  auto out = g.readBuffer(result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 48, "heap order result");
  const uint32_t expect[12] = {0xa1, f2u(1.5f), 0xabc, 0xcccccccc, 0xb2, f2u(2.5f), 0xabc, 0xcccccccc,
                               0,    0,         0xabc, 0xcccccccc};
  for (UINT k = 0; k < 12; ++k)
    EXPECT(out[k] == expect[k], "heap-order word=%u got=0x%x expected=0x%x", k, out[k], expect[k]);
  passCase("heap-order-and-past-heap", before);
}

/*
 * Rasterizer ordered views from ResourceDescriptorHeap (bindless-rov-ps.hlsl): kRovTriangles
 * fullscreen triangles each do v = v * 4 + primitive + 1 at every pixel of a structured
 * buffer, a typed buffer and a 2D texture ROV picked per pixel from six heap slots at
 * 999990+. Only per-pixel primitive order yields the exact base-4 value everywhere.
 */
void testHeapRov(Gpu &g) {
  constexpr UINT kSize = 16, kElements = kSize * kSize, kRovTriangles = 512, kBase = 999990;
  const int before = g_failures;
  const auto vs = readFile(L"bindless-rov-vs.dxil");
  const auto ps = readFile(L"bindless-rov-ps.dxil");
  std::vector<ComPtr<ID3D12Resource>> keep;
  auto views = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize);
  ComPtr<ID3D12Resource> buffers[4], images[2];
  for (UINT k = 0; k < 4; ++k) {
    buffers[k] = g.initBuffer(std::vector<uint32_t>(kElements, 0), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kElements;
    uav.Format = k < 2 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
    uav.Buffer.StructureByteStride = k < 2 ? 4 : 0;
    g.device->CreateUnorderedAccessView(buffers[k].Get(), nullptr, &uav, g.cpu(views.Get(), kBase + k, g.viewStride));
  }
  for (UINT k = 0; k < 2; ++k) {
    images[k] = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_UINT, kSize, kSize, 1,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g.fillTexture(images[k].Get(), 1, [](UINT, UINT, UINT) { return 0u; }, keep,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(images[k].Get(), nullptr, &uav,
                                        g.cpu(views.Get(), kBase + 4 + k, g.viewStride));
  }
  g.submit("heap rov upload");

  D3D12_ROOT_PARAMETER1 param = rootConstant(0);
  auto root = makeRoot(g.device.Get(), directRootDesc(&param, 1, kFlagResourceHeap));
  auto pso = graphicsPso(g.device.Get(), root.Get(), vs.data(), vs.size(), ps.data(), ps.size(),
                         DXGI_FORMAT_R32_UINT);
  auto target = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_UINT, kSize, kSize, 1,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto rtvHeap = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
  auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  g.device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  g.barrier(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
  g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
  D3D12_RECT sc{0, 0, LONG(kSize), LONG(kSize)};
  g.list->RSSetViewports(1, &vp);
  g.list->RSSetScissorRects(1, &sc);
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  g.list->SetDescriptorHeaps(1, heaps);
  g.list->SetGraphicsRootSignature(root.Get());
  g.list->SetGraphicsRoot32BitConstant(0, kBase, 0);
  g.list->SetPipelineState(pso.Get());
  g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g.list->DrawInstanced(3 * kRovTriangles, 1, 0, 0);
  g.uavBarrier();

  uint32_t expect = 0;
  for (UINT t = 0; t < kRovTriangles; ++t)
    expect = expect * 4u + t + 1u;
  int mismatches[3] = {};
  std::vector<uint32_t> words[4];
  for (UINT k = 0; k < 4; ++k)
    words[k] = g.readBuffer(buffers[k].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kElements * 4, "heap rov buffer");
  std::vector<uint32_t> texels[2];
  for (UINT k = 0; k < 2; ++k)
    texels[k] = readTexture(g, images[k].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4, "heap rov image");
  auto color = readTexture(g, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 4, "heap rov target");
  for (UINT y = 0; y < kSize; ++y)
    for (UINT x = 0; x < kSize; ++x) {
      const UINT e = y * kSize + x;
      const uint32_t got[3] = {words[x & 1][e], words[2 + ((x >> 1) & 1)][e], texels[y & 1][e]};
      for (UINT kind = 0; kind < 3; ++kind)
        mismatches[kind] += got[kind] != expect;
      // The unselected slot of each pair stays untouched at this pixel.
      EXPECT(words[(x & 1) ^ 1][e] == 0 && words[2 + (((x >> 1) & 1) ^ 1)][e] == 0 && texels[(y & 1) ^ 1][e] == 0,
             "heap-rov pixel=(%u,%u) wrote an unselected descriptor", x, y);
      EXPECT(color[e] == kRovTriangles - 1, "heap-rov pixel=(%u,%u) color=%u", x, y, color[e]);
    }
  std::printf("OWNED_D3D12_BINDLESS_ROV triangles=%u pixels=%u expected=0x%08x structured_mismatches=%d "
              "typed_mismatches=%d texture_mismatches=%d\n",
              kRovTriangles, kElements, expect, mismatches[0], mismatches[1], mismatches[2]);
  EXPECT(!mismatches[0] && !mismatches[1] && !mismatches[2], "heap-rov lost ordered updates got=(0x%08x,0x%08x,0x%08x)",
         words[0][0], words[2][0], texels[0][0]);
  passCase("ps66-heap-rasterizer-ordered-views", before);
}

HRESULT serializeHr(const VersionedDesc &desc) {
  ComPtr<ID3DBlob> blob, error;
  return D3D12SerializeVersionedRootSignature(reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&desc),
                                              &blob, &error);
}

/* DIRECTLY_INDEXED flag legality and shader / root signature matching. */
void testRootFlags(Gpu &g, const std::vector<uint8_t> &cs, const std::vector<uint8_t> &rootCs) {
  const int before = g_failures;
  auto *dev = g.device.Get();
  HRESULT hr = E_FAIL;
  auto both = makeRoot(dev, directRootDesc(nullptr, 0, kFlagResourceHeap | kFlagSamplerHeap), &hr);
  EXPECT(SUCCEEDED(hr) && both, "flags 0xc00 create hr=0x%08lx", static_cast<unsigned long>(hr));
  EXPECT(serializeHr(directRootDesc(nullptr, 0, 0x80 | kFlagResourceHeap)) == E_INVALIDARG,
         "local root signature accepted DIRECTLY_INDEXED");
  EXPECT(serializeHr(directRootDesc(nullptr, 0, 0x1000)) == E_INVALIDARG, "unknown root flag 0x1000 accepted");
  VersionedDesc v10{};
  v10.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  v10.Desc_1_0 = {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAGS(kFlagSamplerHeap)};
  EXPECT(serializeHr(v10) == S_OK, "1.0 signature rejected SAMPLER_HEAP_DIRECTLY_INDEXED");

  // A heap-indexing shader needs the matching flag at PSO creation.
  auto param = rootConstant(0);
  auto none = makeRoot(dev, directRootDesc(&param, 1, 0));
  auto viewsOnly = makeRoot(dev, directRootDesc(&param, 1, kFlagResourceHeap));
  // D3D12 fails the PSO; the owned compiler reports the mismatch as a compile failure.
  auto pso = computePso(dev, none.Get(), cs.data(), cs.size(), &hr);
  EXPECT(FAILED(hr) && !pso, "heap shader without flags pso hr=0x%08lx", static_cast<unsigned long>(hr));
  std::printf("OWNED_D3D12_BINDLESS_FLAG_MISMATCH resource_heap hr=0x%08lx\n", static_cast<unsigned long>(hr));
  pso = computePso(dev, viewsOnly.Get(), cs.data(), cs.size(), &hr);
  EXPECT(FAILED(hr) && !pso, "sampler-heap shader without SAMPLER flag pso hr=0x%08lx", static_cast<unsigned long>(hr));
  std::printf("OWNED_D3D12_BINDLESS_FLAG_MISMATCH sampler_heap hr=0x%08lx\n", static_cast<unsigned long>(hr));
  D3D12_ROOT_PARAMETER1 params[2] = {rootConstant(0), rootDescriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
  auto full = makeRoot(dev, directRootDesc(params, 2, kFlagResourceHeap | kFlagSamplerHeap));
  auto ok = computePso(dev, full.Get(), rootCs.data(), rootCs.size(), &hr);
  EXPECT(SUCCEEDED(hr) && ok, "heap shader with flags pso hr=0x%08lx", static_cast<unsigned long>(hr));
  passCase("root-flags-validation", before);
}

StaticSamplerDesc1 staticSampler(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address, UINT border, UINT flags,
                                 UINT reg) {
  StaticSamplerDesc1 s{};
  s.base.Filter = filter;
  s.base.AddressU = s.base.AddressV = s.base.AddressW = address;
  s.base.MaxAnisotropy = 1;
  s.base.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  s.base.BorderColor = D3D12_STATIC_BORDER_COLOR(border);
  s.base.MinLOD = 0;
  s.base.MaxLOD = flags & kSamplerFlagNonNormalized ? 0.0f : D3D12_FLOAT32_MAX;
  s.base.ShaderRegister = reg;
  s.base.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  s.Flags = flags;
  return s;
}

VersionedDesc desc12(const D3D12_ROOT_PARAMETER1 *params, UINT count, const StaticSamplerDesc1 *samplers,
                     UINT numSamplers) {
  VersionedDesc d{};
  d.Version = kVersion_1_2;
  d.Desc_1_2 = {count, params, numSamplers, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  return d;
}

const char *kStaticSamplerCs = R"(
Texture2D<float> ramp : register(t0);
Texture2D<uint> ints : register(t1);
SamplerState unnormalized : register(s0);
SamplerState whiteUint : register(s1);
SamplerState blackUint : register(s2);
SamplerState whiteFloat : register(s3);
RWByteAddressBuffer result : register(u0);
[numthreads(1, 1, 1)] void main() {
  result.Store(0, asuint(ramp.SampleLevel(unnormalized, float2(1.5, 0.5), 0)));
  result.Store(4, asuint(ramp.SampleLevel(unnormalized, float2(3.5, 0.5), 0)));
  result.Store4(8, ints.GatherRed(whiteUint, float2(-4, -4)));
  result.Store4(24, ints.GatherRed(blackUint, float2(-4, -4)));
  result.Store4(40, ints.GatherRed(whiteUint, float2(0.5, 0.5)));
  result.Store(56, asuint(ramp.SampleLevel(whiteFloat, float2(-4, -4), 0)));
}
)";

/* Root signature 1.2: serialization round trip, flag validation and GPU sampling. */
void testRootSignature12(Gpu &g) {
  const int before = g_failures;
  auto *dev = g.device.Get();
  const auto point = D3D12_FILTER_MIN_MAG_MIP_POINT;
  StaticSamplerDesc1 samplers[4] = {
      staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
                    kSamplerFlagNonNormalized, 0),
      staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_BORDER, kBorderOpaqueWhiteUint, kSamplerFlagUintBorder, 1),
      staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_BORDER, kBorderOpaqueBlackUint, 0, 2),
      staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0, 3),
  };
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1] = rootDescriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0);
  auto desc = desc12(params, 2, samplers, 4);

  // Serialization round trip through the versioned deserializer.
  ComPtr<ID3DBlob> blob, error;
  check(D3D12SerializeVersionedRootSignature(reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&desc),
                                             &blob, &error),
        "serialize 1.2");
  ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
  check(D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      IID_PPV_ARGS(&deserializer)),
        "deserializer 1.2");
  auto *unconverted = reinterpret_cast<const VersionedDesc *>(deserializer->GetUnconvertedRootSignatureDesc());
  EXPECT(unconverted->Version == kVersion_1_2 && unconverted->Desc_1_2.NumStaticSamplers == 4 &&
             unconverted->Desc_1_2.pStaticSamplers[0].Flags == kSamplerFlagNonNormalized &&
             unconverted->Desc_1_2.pStaticSamplers[1].Flags == kSamplerFlagUintBorder &&
             UINT(unconverted->Desc_1_2.pStaticSamplers[1].base.BorderColor) == kBorderOpaqueWhiteUint &&
             unconverted->Desc_1_2.pStaticSamplers[3].base.ShaderRegister == 3,
         "1.2 unconverted desc lost sampler flags");
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *at11 = nullptr;
  check(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &at11), "1.2 at 1.1");
  EXPECT(at11->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && at11->Desc_1_1.NumStaticSamplers == 4 &&
             at11->Desc_1_1.pStaticSamplers[2].ShaderRegister == 2 && at11->Desc_1_1.NumParameters == 2,
         "1.2 desc at 1.1 wrong");
  VersionedDesc v11{};
  v11.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  D3D12_STATIC_SAMPLER_DESC plain = samplers[3].base;
  v11.Desc_1_1 = {0, nullptr, 1, &plain, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob11;
  check(D3D12SerializeVersionedRootSignature(reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *>(&v11),
                                             &blob11, &error),
        "serialize 1.1");
  ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer11;
  check(D3D12CreateVersionedRootSignatureDeserializer(blob11->GetBufferPointer(), blob11->GetBufferSize(),
                                                      IID_PPV_ARGS(&deserializer11)),
        "deserializer 1.1");
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *at12 = nullptr;
  check(deserializer11->GetRootSignatureDescAtVersion(kVersion_1_2, &at12), "1.1 at 1.2");
  auto *up = reinterpret_cast<const VersionedDesc *>(at12);
  EXPECT(up->Version == kVersion_1_2 && up->Desc_1_2.NumStaticSamplers == 1 && up->Desc_1_2.pStaticSamplers[0].Flags == 0 &&
             up->Desc_1_2.pStaticSamplers[0].base.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
         "1.1 desc at 1.2 wrong");
  EXPECT(deserializer11->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION(4), &at12) == E_INVALIDARG,
         "version 4 accepted");

  // Validation (VulkanOn12 spec: sampler flags).
  struct Bad {
    const char *what;
    StaticSamplerDesc1 sampler;
  } bad[] = {
      {"uint-flag-with-float-border",
       staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
                     kSamplerFlagUintBorder, 0)},
      {"unknown-flag", staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 0x4, 0)},
      {"non-normalized-wrap", staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, kSamplerFlagNonNormalized, 0)},
      {"non-normalized-mip-linear",
       staticSampler(D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0,
                     kSamplerFlagNonNormalized, 0)},
      {"non-normalized-min-ne-mag",
       staticSampler(D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0,
                     kSamplerFlagNonNormalized, 0)},
      {"non-normalized-comparison",
       staticSampler(D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0,
                     kSamplerFlagNonNormalized, 0)},
      {"border-color-5", staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 5, 0, 0)},
  };
  for (const auto &b : bad)
    EXPECT(serializeHr(desc12(nullptr, 0, &b.sampler, 1)) == E_INVALIDARG, "rs12 accepted %s", b.what);
  auto nnMax = staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, kSamplerFlagNonNormalized, 0);
  nnMax.base.MaxLOD = 1.0f;
  EXPECT(serializeHr(desc12(nullptr, 0, &nnMax, 1)) == E_INVALIDARG, "rs12 accepted non-normalized MaxLOD 1");
  auto nnLinear = staticSampler(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0,
                                kSamplerFlagNonNormalized, 0);
  EXPECT(serializeHr(desc12(nullptr, 0, &nnLinear, 1)) == S_OK, "rs12 rejected non-normalized linear border");
  auto uintNoFlag = staticSampler(point, D3D12_TEXTURE_ADDRESS_MODE_BORDER, kBorderOpaqueBlackUint, 0, 0);
  EXPECT(serializeHr(desc12(nullptr, 0, &uintNoFlag, 1)) == S_OK, "rs12 rejected UINT border without flag");
  v11.Desc_1_1.pStaticSamplers = &uintNoFlag.base;
  EXPECT(serializeHr(v11) == E_INVALIDARG, "rs11 accepted a *_UINT border color");

  // GPU: non-normalized coordinates and UINT border colors.
  std::vector<ComPtr<ID3D12Resource>> keep;
  auto views = g.heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2);
  auto ramp = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT, 4, 1, 1);
  g.fillTexture(ramp.Get(), 1, [](UINT x, UINT, UINT) { return f2u(float(x)); }, keep, kShaderRead);
  // 2x2 so a gather at the center reads four in-bounds texels and no border.
  auto ints = g.texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_UINT, 2, 2, 1);
  g.fillTexture(ints.Get(), 1, [](UINT, UINT, UINT) { return 5u; }, keep, kShaderRead);
  g.texSrv(views.Get(), 0, ramp.Get(), DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D);
  g.texSrv(views.Get(), 1, ints.Get(), DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2D);
  auto result = g.initBuffer(std::vector<uint32_t>(16, 0xcccccccc), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
  g.submit("rs12 upload");
  auto root = makeRoot(dev, desc);
  auto shader = compileDxbc(kStaticSamplerCs, "main", "cs_5_1");
  auto pso = computePso(dev, root.Get(), shader->GetBufferPointer(), shader->GetBufferSize());
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  g.list->SetDescriptorHeaps(1, heaps);
  g.list->SetComputeRootSignature(root.Get());
  g.list->SetPipelineState(pso.Get());
  g.list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  g.list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
  g.list->Dispatch(1, 1, 1);
  g.uavBarrier();
  g.submit("rs12 dispatch");
  auto out = g.readBuffer(result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 64, "rs12 result");
  const uint32_t expect[15] = {f2u(1.0f), f2u(3.0f), 1, 1, 1, 1, 0, 0, 0, 0, 5, 5, 5, 5, f2u(1.0f)};
  static const char *names[15] = {"nn-texel1", "nn-texel3", "white-uint", "white-uint", "white-uint",
                                  "white-uint", "black-uint", "black-uint", "black-uint", "black-uint",
                                  "inside", "inside", "inside", "inside", "white-float"};
  for (UINT k = 0; k < 15; ++k)
    EXPECT(out[k] == expect[k], "rs12 %s word=%u got=0x%x expected=0x%x", names[k], k, out[k], expect[k]);
  passCase("root-signature-1.2", before);
}

void testCaps(Gpu &g) {
  const int before = g_failures;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  check(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "options");
  EXPECT(options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3, "ResourceBindingTier=%d",
         int(options.ResourceBindingTier));
  EXPECT(options.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_2, "ResourceHeapTier=%d", int(options.ResourceHeapTier));
  D3D12_FEATURE_DATA_ROOT_SIGNATURE rs{kVersion_1_2};
  check(g.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof(rs)), "root signature 1.2");
  EXPECT(rs.HighestVersion == kVersion_1_2, "HighestVersion(1.2)=%d", int(rs.HighestVersion));
  rs.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
  check(g.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof(rs)), "root signature 1.1");
  EXPECT(rs.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1, "HighestVersion(1.1)=%d", int(rs.HighestVersion));
  rs.HighestVersion = D3D_ROOT_SIGNATURE_VERSION(4);
  EXPECT(g.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof(rs)) == E_INVALIDARG,
         "HighestVersion(4) accepted");
  std::printf("OWNED_D3D12_BINDLESS_CAPS binding_tier=%d heap_tier=%d root_signature=0x%x\n",
              int(options.ResourceBindingTier), int(options.ResourceHeapTier), unsigned(kVersion_1_2));
  passCase("caps", before);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 5) {
    std::printf("usage: owned-d3d12-bindless.exe <bindless-cs.dxil> <bindless-vs.dxil> <bindless-ps.dxil> "
                "<bindless-root-cs.dxil>\n");
    return 2;
  }
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  auto backend = d3d12 ? reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"))
                       : nullptr;
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12") != 0)
    die("OwnedGraphicsBackendId");
  std::printf("OWNED_D3D12_BINDLESS_BACKEND id=%s\n", backend());
  std::fflush(stdout);
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  const auto cs = readFile(argv[1]);
  const auto vs = readFile(argv[2]);
  const auto ps = readFile(argv[3]);
  const auto rootCs = readFile(argv[4]);

  Gpu g;
  g.init(device.Get());
  testCaps(g);
  testRootFlags(g, cs, rootCs);
  testRootSignature12(g);
  {
    BindlessScene scene;
    buildScene(g, scene);
    testCompute66(g, scene, cs);
    testPixel66(g, scene, vs, ps);
    testDxbc51(g, scene);
  }
  testHeapOrder(g, rootCs);
  testHeapRov(g);
  check(device->GetDeviceRemovedReason(), "device removed");
  if (g_failures) {
    std::printf("OWNED_D3D12_BINDLESS_FAIL mismatches=%d\n", g_failures);
    return 1;
  }
  std::puts("OWNED_D3D12_BINDLESS_PASS cases=8 heap=1000000 samplers=2048 max_index=999999");
  return 0;
}
