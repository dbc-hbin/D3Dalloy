// SamplerFeedbackTier 1.0 on the owned D3D12 device, checked against a CPU oracle. Each case prints
// OWNED_D3D12_SAMPLER_FEEDBACK_CASE_PASS <name>; the last line is OWNED_D3D12_SAMPLER_FEEDBACK_PASS cases=N tier=T.
// The DXIL shaders (dxil/sampler-feedback.hlsl, frozen by dxil/build-sampler-feedback.sh) are read beside the
// executable as sampler-feedback-<entry>.dxil.
//
// The oracle models the D3D12 sampling pattern of the paired Sample: the LOD from the gradients (isotropic, or
// anisotropic with the major-axis taps), sampler and instruction bias, sampler MinLOD/MaxLOD, the instruction
// clamp and the view's ResourceMinLODClamp; the one or two mips of the mip filter; the 1 or 2x2 texel
// footprint of the min/mag filter; the address modes (BORDER texels outside the texture are not read). Each
// texel maps to a mip region through its top-left corner in mip-0 texel space. Decoded MinMip texels hold the
// most detailed mip requested (0xFF: none); decoded MipRegionUsed texels hold 0xFF (requested) or 0.
// Parameters are chosen away from rounding boundaries; the oracle fails the run if one lands on one.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DXGI_FORMAT kMinMip = DXGI_FORMAT(189);   // DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
constexpr DXGI_FORMAT kRegionUsed = DXGI_FORMAT(190);  // DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE

int g_cases = 0;

[[noreturn]] void fail(const std::string &step, HRESULT actual = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_SAMPLER_FEEDBACK_FAIL step=%s hr=%08lx\n", step.c_str(),
               static_cast<unsigned long>(actual));
  std::fflush(stderr);
  throw std::runtime_error(step);
}
void check(HRESULT actual, const std::string &step) {
  if (FAILED(actual)) fail(step, actual);
}
void passed(const std::string &name) {
  ++g_cases;
  std::printf("OWNED_D3D12_SAMPLER_FEEDBACK_CASE_PASS %s\n", name.c_str());
  std::fflush(stdout);
}

std::vector<uint8_t> shader(const std::string &entry) {
  std::wstring name = L"sampler-feedback-";
  for (char c : entry) name += static_cast<wchar_t>(c);
  name += L".dxil";
  HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail("open " + entry, HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 24)) {
    CloseHandle(file);
    fail("size " + entry);
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != data.size()) fail("read " + entry);
  return data;
}

/* ------------------------------------------------------------------------------------------------ */
/* CPU oracle                                                                                        */
/* ------------------------------------------------------------------------------------------------ */

struct MapShape {
  uint32_t width, height, mips, slices, region_w, region_h;
  bool used;
  uint32_t gridW(uint32_t mip = 0) const { return std::max(1u, ((width + region_w - 1) / region_w) >> mip); }
  uint32_t gridH(uint32_t mip = 0) const { return std::max(1u, ((height + region_h - 1) / region_h) >> mip); }
  uint32_t decodedMips() const { return used ? mips : 1; }
};

struct SamplerState {
  D3D12_FILTER filter;
  D3D12_TEXTURE_ADDRESS_MODE u, v;
  float bias = 0, min_lod = 0, max_lod = D3D12_FLOAT32_MAX;
  UINT anisotropy = 1;
  D3D12_SAMPLER_DESC desc() const {
    D3D12_SAMPLER_DESC d{};
    d.Filter = filter;
    d.AddressU = u;
    d.AddressV = v;
    d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    d.MipLODBias = bias;
    d.MaxAnisotropy = anisotropy;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    d.MinLOD = min_lod;
    d.MaxLOD = max_lod;
    return d;
  }
};

struct ViewRange {
  uint32_t first_mip = 0, mips = 0 /* 0: all */, first_slice = 0, slices = 0 /* 0: all */;
  float min_lod_clamp = 0;
};

/* The decoded texels of every subresource, [slice][mip][y][x] (MinMip: one mip). */
struct Decoded {
  MapShape shape;
  std::vector<uint8_t> texels;
  explicit Decoded(const MapShape &s) : shape(s) {
    size_t n = 0;
    for (uint32_t m = 0; m < s.decodedMips(); ++m) n += size_t(s.gridW(m)) * s.gridH(m);
    texels.assign(n * s.slices, s.used ? 0 : 0xff);
  }
  size_t offset(uint32_t slice, uint32_t mip) const {
    size_t per_slice = 0, before = 0;
    for (uint32_t m = 0; m < shape.decodedMips(); ++m) {
      if (m < mip) before += size_t(shape.gridW(m)) * shape.gridH(m);
      per_slice += size_t(shape.gridW(m)) * shape.gridH(m);
    }
    return slice * per_slice + before;
  }
  uint8_t &at(uint32_t slice, uint32_t mip, uint32_t x, uint32_t y) {
    return texels[offset(slice, mip) + size_t(y) * shape.gridW(mip) + x];
  }
};

struct Write {
  float u, v, layer;
  float dudx, dvdx, dudy, dvdy;
  float value;  // Bias: bias; Level: level
  float clamp;  // -FLT_MAX: none
  bool level;
};

// When set, an ambiguous write is reported through g_ambiguous instead of failing (runCompute drops it).
bool g_probe = false, g_ambiguous = false;

void ambiguity(bool condition, const char *what, float value) {
  if (condition && g_probe) {
    g_ambiguous = true;
    return;
  }
  if (condition) {
    char text[128];
    std::snprintf(text, sizeof(text), "oracle ambiguity %s=%.9g", what, value);
    fail(text);
  }
}

float frac(float x) { return x - std::floor(x); }
void boundary(float x, const char *what) {
  const float f = frac(x);
  ambiguity((f > 0 && f < 1e-3f) || f > 1 - 1e-3f, what, x);
}

bool address(int t, int size, D3D12_TEXTURE_ADDRESS_MODE mode, int &out) {
  switch (mode) {
  case D3D12_TEXTURE_ADDRESS_MODE_WRAP: out = ((t % size) + size) % size; return true;
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: {
    int r = ((t % (2 * size)) + 2 * size) % (2 * size);
    out = r >= size ? 2 * size - 1 - r : r;
    return true;
  }
  case D3D12_TEXTURE_ADDRESS_MODE_BORDER: out = t; return t >= 0 && t < size;
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: out = std::clamp(t < 0 ? -t - 1 : t, 0, size - 1); return true;
  default: out = std::clamp(t, 0, size - 1); return true;
  }
}

void oracle(Decoded &d, const SamplerState &s, const ViewRange &view, const Write &w) {
  const MapShape &m = d.shape;
  const uint32_t first_mip = view.first_mip, view_mips = view.mips ? view.mips : m.mips - first_mip;
  const uint32_t view_slices = view.slices ? view.slices : m.slices;
  const bool anisotropic = D3D12_DECODE_IS_ANISOTROPIC_FILTER(s.filter);
  const bool min_linear = D3D12_DECODE_MIN_FILTER(s.filter), mag_linear = D3D12_DECODE_MAG_FILTER(s.filter);
  const bool mip_linear = D3D12_DECODE_MIP_FILTER(s.filter);
  const float view_w = float(std::max(1u, m.width >> first_mip)), view_h = float(std::max(1u, m.height >> first_mip));
  float lod;
  uint32_t taps = 1;
  float major_u = 0, major_v = 0;
  if (w.level) {
    lod = w.value + s.bias;
  } else {
    const float ax = w.dudx * view_w, ay = w.dvdx * view_h, bx = w.dudy * view_w, by = w.dvdy * view_h;
    const float px = std::sqrt(ax * ax + ay * ay), py = std::sqrt(bx * bx + by * by);
    const float p_max = std::max(px, py), p_min = std::min(px, py);
    float ratio = 1;
    if (anisotropic && p_max > 0) {
      if (p_min > 0) {
        const float r = p_max / p_min;
        ambiguity(frac(r) > 0 && (frac(r) < 1e-4f || frac(r) > 1 - 1e-4f), "anisotropy ratio", r);
        ratio = std::min(std::ceil(r), float(std::clamp(s.anisotropy, 1u, 16u)));
      } else {
        ratio = float(std::clamp(s.anisotropy, 1u, 16u));
      }
      taps = uint32_t(ratio);
      major_u = px >= py ? w.dudx : w.dudy;
      major_v = px >= py ? w.dvdx : w.dvdy;
      ambiguity(px != py && std::fabs(px - py) < 1e-4f * p_max, "major axis", px - py);
    }
    lod = std::log2(p_max / ratio) + s.bias + w.value;
  }
  // ResourceMinLODClamp is in the resource's mip space; `lod` is relative to the view's most detailed mip.
  lod = std::max(lod, std::max(std::max(s.min_lod, w.clamp), view.min_lod_clamp - float(first_mip)));
  lod = std::min(lod, s.max_lod);
  ambiguity(lod != 0 && std::fabs(lod) < 1e-4f, "mag/min lod", lod);
  const bool linear = lod <= 0 ? mag_linear : min_linear;
  const float last = float(view_mips - 1);
  const float clamped = std::clamp(lod, 0.0f, last);
  uint32_t m0, m1;
  if (mip_linear) {
    ambiguity(frac(clamped) > 0 && (frac(clamped) < 1e-4f || frac(clamped) > 1 - 1e-4f), "mip", clamped);
    m0 = uint32_t(std::floor(clamped));
    m1 = clamped > float(m0) ? std::min(m0 + 1, view_mips - 1) : m0;
  } else {
    ambiguity(std::fabs(frac(clamped) - 0.5f) < 1e-4f, "nearest mip", clamped);
    m0 = m1 = std::min(uint32_t(std::floor(clamped + 0.5f)), view_mips - 1);
  }
  uint32_t slice = view.first_slice;
  if (m.slices > 1 || view_slices > 1) {
    ambiguity(std::fabs(frac(w.layer) - 0.5f) < 1e-4f, "layer", w.layer);
    slice += uint32_t(std::clamp(std::nearbyint(w.layer), 0.0f, float(view_slices - 1)));
  }
  for (uint32_t pass = 0; pass < (m1 != m0 ? 2u : 1u); ++pass) {
    const uint32_t level = first_mip + (pass ? m1 : m0);
    const uint32_t sx = std::max(1u, m.width >> level), sy = std::max(1u, m.height >> level);
    for (uint32_t tap = 0; tap < taps; ++tap) {
      const float off = taps > 1 ? (float(tap) + 0.5f) / float(taps) - 0.5f : 0;
      const float su = (w.u + off * major_u) * float(sx), sv = (w.v + off * major_v) * float(sy);
      boundary(linear ? su - 0.5f : su, "u texel");
      boundary(linear ? sv - 0.5f : sv, "v texel");
      const int x0 = int(std::floor(linear ? su - 0.5f : su)), y0 = int(std::floor(linear ? sv - 0.5f : sv));
      const int extent = linear ? 2 : 1;
      for (int dy = 0; dy < extent; ++dy) {
        int ty;
        if (!address(y0 + dy, int(sy), s.v, ty)) continue;
        for (int dx = 0; dx < extent; ++dx) {
          int tx;
          if (!address(x0 + dx, int(sx), s.u, tx)) continue;
          const uint32_t rx0 = uint32_t(tx) * m.width / sx / m.region_w, ry0 = uint32_t(ty) * m.height / sy / m.region_h;
          if (m.used) {
            const uint32_t gx = m.gridW(level), gy = m.gridH(level);
            d.at(slice, level, std::min(gx - 1, rx0 >> level), std::min(gy - 1, ry0 >> level)) = 0xff;
          } else {
            uint8_t &cell = d.at(slice, 0, std::min(m.gridW() - 1, rx0), std::min(m.gridH() - 1, ry0));
            cell = uint8_t(std::min<uint32_t>(cell, level));
          }
        }
      }
    }
  }
}

/* ------------------------------------------------------------------------------------------------ */
/* Device helpers                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

struct List {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
};

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, subresource, before, after};
  list->ResourceBarrier(1, &b);
}
void uavBarrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = resource;
  list->ResourceBarrier(1, &b);
}

struct Param {
  float uvl[4];
  float grad[4];
  float clampv;
  uint32_t op;  // 0 Grad, 1 Grad + clamp, 2 Level
  uint32_t pad[2];
};
static_assert(sizeof(Param) == 48);

struct Context {
  ComPtr<ID3D12Device8> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 fence_value = 0;
  ComPtr<ID3D12RootSignature> root, root_static;
  ComPtr<ID3D12DescriptorHeap> heap, cpu_heap, samplers;
  UINT increment = 0, sampler_increment = 0;
  ComPtr<ID3D12PipelineState> cs[2][2][2];  // [static sampler root][used][array]
  std::vector<SamplerState> sampler_states;

  List begin() {
    List l;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&l.allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, l.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&l.list)),
          "command list");
    return l;
  }
  void execute(List &l) {
    check(l.list->Close(), "Close");
    ID3D12CommandList *lists[] = {l.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), ++fence_value), "Signal");
    check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
    const DWORD waited = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0) fail("bounded GPU completion");
    check(device->GetDeviceRemovedReason(), "device removed");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES props{type};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)),
          "buffer");
    return r;
  }
  ComPtr<ID3D12Resource> texture(UINT w, UINT h, UINT16 slices, UINT16 mips, DXGI_FORMAT format,
                                 D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = slices;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)),
          "texture");
    return r;
  }
  static D3D12_RESOURCE_DESC1 feedbackDesc(const MapShape &s) {
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = s.width;
    d.Height = s.height;
    d.DepthOrArraySize = UINT16(s.slices);
    d.MipLevels = UINT16(s.mips);
    d.Format = s.used ? kRegionUsed : kMinMip;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.SamplerFeedbackMipRegion = {s.region_w, s.region_h, 1};
    return d;
  }
  ComPtr<ID3D12Resource> map(const MapShape &s) {
    const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    const auto desc = feedbackDesc(s);
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource2(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           nullptr, nullptr, IID_PPV_ARGS(&r)),
          "CreateCommittedResource2 feedback");
    return r;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE cpu(ID3D12DescriptorHeap *h, UINT i, UINT inc) {
    auto handle = h->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(i) * inc;
    return handle;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu(ID3D12DescriptorHeap *h, UINT i, UINT inc) {
    auto handle = h->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64(i) * inc;
    return handle;
  }
  /* Slot 0: paired SRV, slot 1: feedback UAV (the shader table), mirrored into the CPU heap for clears. */
  void bindViews(ID3D12Resource *paired, ID3D12Resource *map, const MapShape &s, const ViewRange &view,
                 bool null_paired = false) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const UINT mips = view.mips ? view.mips : s.mips - view.first_mip;
    if (s.slices > 1) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Texture2DArray = {view.first_mip, mips, view.first_slice, view.slices ? view.slices : s.slices, 0,
                            view.min_lod_clamp};
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D = {view.first_mip, mips, 0, view.min_lod_clamp};
    }
    device->CreateShaderResourceView(paired, &srv, cpu(heap.Get(), 0, increment));
    device->CreateSamplerFeedbackUnorderedAccessView(null_paired ? nullptr : paired, map,
                                                     cpu(heap.Get(), 1, increment));
    device->CreateSamplerFeedbackUnorderedAccessView(null_paired ? nullptr : paired, map,
                                                     cpu(cpu_heap.Get(), 1, increment));
  }
  void clear(ID3D12GraphicsCommandList *list, ID3D12Resource *map) {
    const UINT values[4] = {1, 2, 3, 4};  // ignored for feedback maps
    const D3D12_RECT rect{0, 0, 1, 1};    // ignored for feedback maps
    list->ClearUnorderedAccessViewUint(gpu(heap.Get(), 1, increment), cpu(cpu_heap.Get(), 1, increment), map,
                                       values, 1, &rect);
    uavBarrier(list, map);
  }

  /* An R8_UINT transcode texture for `s`: at least the decoded grid, padded (data locked to the top left) so a
   * MipRegionUsed map's full mip count is a valid texture (SamplerFeedback spec, Transcode resources and padding). */
  ComPtr<ID3D12Resource> transcodeTexture(const MapShape &s, D3D12_RESOURCE_STATES state) {
    const UINT mips = s.decodedMips();
    return texture(std::max(s.gridW(), 1u << (mips - 1)), std::max(s.gridH(), 1u << (mips - 1)), UINT16(s.slices),
                   UINT16(mips), DXGI_FORMAT_R8_UINT, state);
  }
  /* Reads an R8 texture [slice][mip] (row pitch 256) or an R8 buffer into a Decoded. */
  Decoded decode(ID3D12Resource *map, const MapShape &s, bool to_buffer = false) {
    Decoded out(s);
    auto l = begin();
    transition(l.list.Get(), map, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    const size_t bytes = out.texels.size();
    if (to_buffer) {
      auto dst = buffer(bytes + 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RESOLVE_DEST);
      l.list->ResolveSubresourceRegion(dst.Get(), 0, 0, 0, map, UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT,
                                       D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
      transition(l.list.Get(), map, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(l.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      auto rb = buffer(bytes + 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      l.list->CopyBufferRegion(rb.Get(), 0, dst.Get(), 0, bytes + 64);
      execute(l);
      void *mapped = nullptr;
      check(rb->Map(0, nullptr, &mapped), "Map");
      std::memcpy(out.texels.data(), mapped, bytes);
      rb->Unmap(0, nullptr);
      return out;
    }
    auto dst = transcodeTexture(s, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    l.list->ResolveSubresourceRegion(dst.Get(), UINT_MAX, 0, 0, map, UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT,
                                     D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
    transition(l.list.Get(), map, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(l.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    out.texels = readTexture(l, dst.Get(), s);
    return out;
  }
  /* Copies every subresource of an R8 texture shaped like `s`'s decoded form and returns its texels. */
  std::vector<uint8_t> readTexture(List &l, ID3D12Resource *tex, const MapShape &s) {
    const UINT mips = s.decodedMips();
    const UINT64 sub_bytes = (256ull * s.gridH() + 511) & ~511ull;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
    auto rb = buffer(sub_bytes * mips * s.slices, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT slice = 0; slice < s.slices; ++slice)
      for (UINT mip = 0; mip < mips; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = {sub_bytes * (slice * mips + mip),
                               {DXGI_FORMAT_R8_UINT, s.gridW(mip), s.gridH(mip), 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = mip + slice * mips;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
    execute(l);
    Decoded out(s);
    uint8_t *mapped = nullptr;
    check(rb->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "Map");
    for (UINT slice = 0; slice < s.slices; ++slice)
      for (UINT mip = 0; mip < mips; ++mip)
        for (UINT y = 0; y < s.gridH(mip); ++y)
          for (UINT x = 0; x < s.gridW(mip); ++x)
            out.at(slice, mip, x, y) = mapped[sub_bytes * (slice * mips + mip) + 256 * y + x];
    rb->Unmap(0, nullptr);
    return out.texels;
  }
  /* An R8 texture [slice][mip] holding `d`, in `state`. */
  ComPtr<ID3D12Resource> upload(const Decoded &d, D3D12_RESOURCE_STATES state) {
    const MapShape &s = d.shape;
    const UINT mips = s.decodedMips();
    auto tex = transcodeTexture(s, D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT64 sub_bytes = (256ull * s.gridH() + 511) & ~511ull;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
    auto up = buffer(sub_bytes * mips * s.slices, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *mapped = nullptr;
    check(up->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "Map upload");
    auto &dd = const_cast<Decoded &>(d);
    for (UINT slice = 0; slice < s.slices; ++slice)
      for (UINT mip = 0; mip < mips; ++mip)
        for (UINT y = 0; y < s.gridH(mip); ++y)
          for (UINT x = 0; x < s.gridW(mip); ++x)
            mapped[sub_bytes * (slice * mips + mip) + 256 * y + x] = dd.at(slice, mip, x, y);
    up->Unmap(0, nullptr);
    auto l = begin();
    for (UINT slice = 0; slice < s.slices; ++slice)
      for (UINT mip = 0; mip < mips; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = {sub_bytes * (slice * mips + mip),
                               {DXGI_FORMAT_R8_UINT, s.gridW(mip), s.gridH(mip), 1, 256}};
        D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = mip + slice * mips;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
    transition(l.list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
    execute(l);
    return tex;
  }
};

void compare(const Decoded &actual, const Decoded &expected, const std::string &name) {
  if (actual.texels == expected.texels) return;
  const MapShape &s = expected.shape;
  auto &a = const_cast<Decoded &>(actual);
  auto &e = const_cast<Decoded &>(expected);
  int shown = 0;
  for (UINT slice = 0; slice < s.slices; ++slice)
    for (UINT mip = 0; mip < s.decodedMips(); ++mip)
      for (UINT y = 0; y < s.gridH(mip); ++y)
        for (UINT x = 0; x < s.gridW(mip); ++x)
          if (a.at(slice, mip, x, y) != e.at(slice, mip, x, y) && shown++ < 16)
            std::fprintf(stderr,
                         "OWNED_D3D12_SAMPLER_FEEDBACK_MISMATCH case=%s slice=%u mip=%u x=%u y=%u got=%u want=%u\n",
                         name.c_str(), slice, mip, x, y, a.at(slice, mip, x, y), e.at(slice, mip, x, y));
  fail("mismatch " + name);
}

/* ------------------------------------------------------------------------------------------------ */
/* Cases                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

std::vector<Write> writePattern(const MapShape &s) {
  const float w = float(s.width), h = float(s.height);
  const float none = -D3D12_FLOAT32_MAX;
  std::vector<Write> out;
  const float uvs[][2] = {{0.137f, 0.219f}, {0.561f, 0.707f}, {0.913f, 0.043f},  {-0.213f, 0.451f},
                          {1.317f, 1.109f}, {0.4421f, 0.5383f}, {0.0137f, 0.9811f}};
  const float ks[] = {0.7f, 1.41f, 2.9f, 5.3f, 11.7f, 300.0f};
  float layer = 0.2f;
  for (const auto &uv : uvs) {
    for (float k : ks) out.push_back({uv[0], uv[1], layer, k / w, 0, 0, 0.37f * k / h, 0, none, false});
    out.push_back({uv[0], uv[1], layer, 5.3f / w, 0, 0, 1.41f / h, 0, none, false});         // anisotropic
    out.push_back({uv[0], uv[1], layer, 0, 1.3f / h * 0, 1.27f / w, 0, 0, none, false});      // swapped axis
    out.push_back({uv[0], uv[1], layer, 2.9f / w, 0, 0, 2.9f / h, 0, 2.6f, false});          // clamp
    out.push_back({uv[0], uv[1], layer, 1.41f / w, 0, 0, 1.41f / h, 0, 1.3f, false});        // clamp
    for (float level : {0.3f, 1.7f, 2.45f, 9.0f, -1.2f}) out.push_back({uv[0], uv[1], layer, 0, 0, 0, 0, level, none, true});
    layer = std::fmod(layer + 1.13f, float(s.slices) + 0.6f);
  }
  return out;
}

void runCompute(Context &c, const std::string &name, const MapShape &s, UINT sampler_index, const ViewRange &view,
                const std::vector<Write> &pattern, bool static_sampler = false, bool null_paired = false) {
  const SamplerState &sampler = c.sampler_states[sampler_index];
  // A write whose footprint lies within float rounding of a texel, mip, LOD or anisotropy boundary has no
  // exact expected result; it is left out of both the dispatch and the oracle.
  std::vector<Write> writes;
  for (const auto &w : pattern) {
    Decoded scratch(s);
    g_probe = true;
    g_ambiguous = false;
    oracle(scratch, sampler, view, w);
    g_probe = false;
    if (!g_ambiguous) writes.push_back(w);
  }
  if (writes.size() * 4 < pattern.size() * 3) fail("too many ambiguous writes in " + name);
  if (writes.size() != pattern.size())
    std::printf("OWNED_D3D12_SAMPLER_FEEDBACK_AMBIGUOUS case=%s dropped=%zu of %zu\n", name.c_str(),
                pattern.size() - writes.size(), pattern.size());
  Decoded expected(s);
  if (!null_paired)
    for (const auto &w : writes) oracle(expected, sampler, view, w);

  auto paired = c.texture(s.width, s.height, UINT16(s.slices), UINT16(s.mips), DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  auto map = c.map(s);
  c.bindViews(paired.Get(), map.Get(), s, view, null_paired);
  std::vector<Param> params;
  for (const auto &w : writes) {
    Param p{};
    p.uvl[0] = w.u, p.uvl[1] = w.v, p.uvl[2] = w.layer, p.uvl[3] = w.value;
    p.grad[0] = w.dudx, p.grad[1] = w.dvdx, p.grad[2] = w.dudy, p.grad[3] = w.dvdy;
    p.clampv = w.clamp;
    p.op = w.level ? 2 : (w.clamp != -D3D12_FLOAT32_MAX ? 1 : 0);
    params.push_back(p);
  }
  auto up = c.buffer(params.size() * sizeof(Param), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(up->Map(0, nullptr, &mapped), "Map params");
  std::memcpy(mapped, params.data(), params.size() * sizeof(Param));
  up->Unmap(0, nullptr);

  auto l = c.begin();
  ID3D12DescriptorHeap *heaps[] = {c.heap.Get(), c.samplers.Get()};
  l.list->SetDescriptorHeaps(2, heaps);
  c.clear(l.list.Get(), map.Get());
  l.list->SetComputeRootSignature(static_sampler ? c.root_static.Get() : c.root.Get());
  l.list->SetPipelineState(c.cs[static_sampler][s.used][s.slices > 1].Get());
  const uint32_t constants[8] = {0, 0, 0, 0, 0, 0, uint32_t(params.size()), 0};
  l.list->SetComputeRoot32BitConstants(0, 8, constants, 0);
  l.list->SetComputeRootDescriptorTable(1, c.gpu(c.heap.Get(), 0, c.increment));
  l.list->SetComputeRootShaderResourceView(2, up->GetGPUVirtualAddress());
  if (!static_sampler)
    l.list->SetComputeRootDescriptorTable(3, c.gpu(c.samplers.Get(), sampler_index, c.sampler_increment));
  l.list->Dispatch(UINT(params.size() + 63) / 64, 1, 1);
  uavBarrier(l.list.Get(), map.Get());
  c.execute(l);
  compare(c.decode(map.Get(), s), expected, name);
  if (!s.used && s.slices == 1) compare(c.decode(map.Get(), s, true), expected, name + "_buffer");
  passed(name);
}

/* A 16x16 full-screen draw: uv = SV_Position.xy * scale + offset, constant derivatives `scale`. */
void runPixel(Context &c, const std::string &name, const MapShape &s, UINT sampler_index, const char *ps,
              float bias, float lclamp, uint32_t layer) {
  const SamplerState &sampler = c.sampler_states[sampler_index];
  const float scale[2] = {9.0f / 128, 5.0f / 128}, offset[2] = {0.0123f, 0.0456f};
  Decoded expected(s);
  for (UINT y = 0; y < 16; ++y)
    for (UINT x = 0; x < 16; ++x) {
      Write w{(x + 0.5f) * scale[0] + offset[0], (y + 0.5f) * scale[1] + offset[1], float(layer), scale[0], 0, 0,
              scale[1], 0, -D3D12_FLOAT32_MAX, false};
      if (!std::strcmp(ps, "ps_bias")) w.value = bias;
      if (!std::strcmp(ps, "ps_clamp")) w.clamp = lclamp;
      oracle(expected, sampler, {}, w);
    }
  const std::string suffix = std::string(s.used ? "_used" : "_minmip") + (s.slices > 1 ? "_array" : "");
  const auto vs = shader("vs_main"), pixel = shader(std::string(ps) + suffix);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = c.root.Get();
  pd.VS = {vs.data(), vs.size()};
  pd.PS = {pixel.data(), pixel.size()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "graphics pso " + name);

  auto paired = c.texture(s.width, s.height, UINT16(s.slices), UINT16(s.mips), DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  auto map = c.map(s);
  c.bindViews(paired.Get(), map.Get(), s, {});
  auto rt = c.texture(16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  c.device->CreateRenderTargetView(rt.Get(), nullptr, rtv);
  auto dummy = c.buffer(16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

  auto l = c.begin();
  ID3D12DescriptorHeap *heaps[] = {c.heap.Get(), c.samplers.Get()};
  l.list->SetDescriptorHeaps(2, heaps);
  c.clear(l.list.Get(), map.Get());
  l.list->SetGraphicsRootSignature(c.root.Get());
  l.list->SetPipelineState(pso.Get());
  uint32_t constants[8];
  std::memcpy(&constants[0], scale, 8);
  std::memcpy(&constants[2], offset, 8);
  std::memcpy(&constants[4], &bias, 4);
  std::memcpy(&constants[5], &lclamp, 4);
  constants[6] = 0;
  constants[7] = layer;
  l.list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
  l.list->SetGraphicsRootDescriptorTable(1, c.gpu(c.heap.Get(), 0, c.increment));
  l.list->SetGraphicsRootShaderResourceView(2, dummy->GetGPUVirtualAddress());
  l.list->SetGraphicsRootDescriptorTable(3, c.gpu(c.samplers.Get(), sampler_index, c.sampler_increment));
  l.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0, 0, 16, 16, 0, 1};
  const D3D12_RECT sc{0, 0, 16, 16};
  l.list->RSSetViewports(1, &vp);
  l.list->RSSetScissorRects(1, &sc);
  l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  l.list->DrawInstanced(3, 1, 0, 0);
  uavBarrier(l.list.Get(), map.Get());
  c.execute(l);
  compare(c.decode(map.Get(), s), expected, name);
  passed(name);
}

void testCaps(Context &c, int &tier) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)), "OPTIONS7");
  tier = int(o7.SamplerFeedbackTier);
  for (DXGI_FORMAT f : {kMinMip, kRegionUsed}) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{f};
    check(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)), "FORMAT_SUPPORT");
    if (!(fs.Support2 & D3D12_FORMAT_SUPPORT2_SAMPLER_FEEDBACK) || !(fs.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
      fail("feedback format support");
    D3D12_FEATURE_DATA_FORMAT_INFO fi{f};
    check(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &fi, sizeof(fi)), "FORMAT_INFO");
    if (fi.PlaneCount != 1) fail("feedback plane count");
  }
  passed("caps_and_format_support");
}

void testCreation(Context &c) {
  const D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, upload{D3D12_HEAP_TYPE_UPLOAD};
  auto attempt = [&](D3D12_RESOURCE_DESC1 d, const D3D12_HEAP_PROPERTIES &props = {D3D12_HEAP_TYPE_DEFAULT},
                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    ComPtr<ID3D12Resource> r;
    return c.device->CreateCommittedResource2(&props, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, nullptr,
                                              IID_PPV_ARGS(&r));
  };
  const MapShape base{64, 64, 7, 1, 8, 8, false};
  const auto good = Context::feedbackDesc(base);
  check(attempt(good), "valid 8x8 region");
  for (auto region : {D3D12_MIP_REGION{4, 4, 1}, {32, 32, 0}, {16, 4, 1}, {4, 32, 1}}) {
    auto d = good;
    d.SamplerFeedbackMipRegion = region;
    check(attempt(d), "valid region");
  }
  {
    auto d = good;
    d.Width = 100;
    d.Height = 100;
    d.MipLevels = 0;
    d.SamplerFeedbackMipRegion = {32, 32, 1};
    check(attempt(d), "valid 100x100 32x32 region");
    d.SamplerFeedbackMipRegion = {64, 64, 1};
    if (SUCCEEDED(attempt(d))) fail("region larger than half accepted");
  }
  for (auto region : {D3D12_MIP_REGION{0, 0, 0}, {2, 2, 1}, {6, 8, 1}, {8, 12, 1}, {64, 8, 1}, {8, 8, 2}}) {
    auto d = good;
    d.SamplerFeedbackMipRegion = region;
    if (SUCCEEDED(attempt(d))) fail("invalid mip region accepted");
  }
  {
    auto d = good;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (SUCCEEDED(attempt(d, def, D3D12_RESOURCE_STATE_COMMON))) fail("feedback without UAV flag accepted");
    d = good;
    d.SampleDesc.Count = 4;
    if (SUCCEEDED(attempt(d))) fail("multisampled feedback accepted");
    d = good;
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (SUCCEEDED(attempt(d))) fail("3D feedback accepted");
    if (SUCCEEDED(attempt(good, upload, D3D12_RESOURCE_STATE_GENERIC_READ))) fail("upload-heap feedback accepted");
    d = good;
    d.Format = DXGI_FORMAT_R8_UINT;
    if (SUCCEEDED(attempt(d))) fail("mip region on an ordinary texture accepted");
  }
  {
    D3D12_RESOURCE_DESC plain{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 7, kMinMip, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> r;
    if (SUCCEEDED(c.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &plain,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r))))
      fail("feedback format without a mip region accepted");
    plain.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    if (SUCCEEDED(c.device->CreateReservedResource1(&plain, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                                    IID_PPV_ARGS(&r))))
      fail("reserved feedback accepted");
  }
  D3D12_RESOURCE_ALLOCATION_INFO1 info1{};
  const auto info = c.device->GetResourceAllocationInfo2(0, 1, &good, &info1);
  if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes < 8 * 8 * 4 || info1.SizeInBytes != info.SizeInBytes)
    fail("feedback allocation info");
  passed("creation_validation");
}

void testCleared(Context &c) {
  for (bool used : {false, true}) {
    const MapShape s{60, 36, 6, 1, 8, 4, used};
    auto map = c.map(s);
    auto paired = c.texture(60, 36, 1, 6, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON);
    c.bindViews(paired.Get(), map.Get(), s, {});
    // Committed maps are created cleared; a clear keeps them cleared.
    compare(c.decode(map.Get(), s), Decoded(s), used ? "created_cleared_used" : "created_cleared_minmip");
    auto l = c.begin();
    ID3D12DescriptorHeap *heaps[] = {c.heap.Get(), c.samplers.Get()};
    l.list->SetDescriptorHeaps(2, heaps);
    c.clear(l.list.Get(), map.Get());
    c.execute(l);
    compare(c.decode(map.Get(), s), Decoded(s), "cleared");
  }
  passed("clear_decodes_ff_and_0");
}

void testPlaced(Context &c) {
  const MapShape s{64, 64, 7, 1, 8, 8, true};
  const auto desc = Context::feedbackDesc(s);
  D3D12_RESOURCE_ALLOCATION_INFO1 info1{};
  const auto info = c.device->GetResourceAllocationInfo2(0, 1, &desc, &info1);
  D3D12_HEAP_DESC hd{info.SizeInBytes * 2, {D3D12_HEAP_TYPE_DEFAULT}, info.Alignment,
                     D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
  ComPtr<ID3D12Heap> heap;
  check(c.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "heap");
  ComPtr<ID3D12Resource> map;
  check(c.device->CreatePlacedResource1(heap.Get(), info.SizeInBytes, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&map)),
        "CreatePlacedResource1 feedback");
  ComPtr<ID3D12Resource> bad;
  if (SUCCEEDED(c.device->CreatePlacedResource1(heap.Get(), info.SizeInBytes + 1, &desc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&bad))))
    fail("misaligned placed feedback accepted");
  auto paired = c.texture(64, 64, 1, 7, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON);
  c.bindViews(paired.Get(), map.Get(), s, {});
  auto l = c.begin();
  ID3D12DescriptorHeap *heaps[] = {c.heap.Get(), c.samplers.Get()};
  l.list->SetDescriptorHeaps(2, heaps);
  c.clear(l.list.Get(), map.Get());
  c.execute(l);
  compare(c.decode(map.Get(), s), Decoded(s), "placed_cleared");
  passed("placed_map_clear");
}

/* Encode an R8 image, decode it back: MipRegionUsed is transitive; MinMip round-trips through texture and buffer. */
void testTranscode(Context &c) {
  {  // MipRegionUsed, 3 slices: encode all, decode all.
    const MapShape s{60, 36, 6, 3, 8, 4, true};
    Decoded src(s), want(s);
    uint32_t seed = 7;
    for (size_t i = 0; i < src.texels.size(); ++i) {
      seed = seed * 1103515245u + 12345u;
      const uint8_t v = (seed >> 16) % 3 == 0 ? 0 : ((seed >> 16) % 3 == 1 ? 0xff : uint8_t(1 + (seed >> 20) % 200));
      src.texels[i] = v;
      want.texels[i] = v ? 0xff : 0;
    }
    auto tex = c.upload(src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    auto map = c.map(s);
    auto l = c.begin();
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    l.list->ResolveSubresourceRegion(map.Get(), UINT_MAX, 0, 0, tex.Get(), UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT,
                                     D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute(l);
    compare(c.decode(map.Get(), s), want, "used_encode_decode");

    // One subresource (mip 1, slice 2) with a source rect and a destination offset, into a 0x55 texture.
    Decoded filler(s);
    std::fill(filler.texels.begin(), filler.texels.end(), 0x55);
    auto dst = c.upload(filler, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    l = c.begin();
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    D3D12_RECT rect{1, 2, 3, 4};  // mip 1 grid is 4x4
    const UINT sub = 1 + 2 * s.mips;
    l.list->ResolveSubresourceRegion(dst.Get(), sub, 1, 0, map.Get(), sub, &rect, DXGI_FORMAT_R8_UINT,
                                     D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(l.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Decoded got(s);
    got.texels = c.readTexture(l, dst.Get(), s);
    Decoded expect = filler;
    for (UINT y = 0; y < 2; ++y)
      for (UINT x = 0; x < 2; ++x) expect.at(2, 1, 1 + x, y) = want.at(2, 1, 1 + x, 2 + y);
    compare(got, expect, "used_decode_rect_offset");
  }
  {  // MinMip 2D: encode from a buffer, decode to a buffer and a texture.
    const MapShape s{64, 64, 7, 1, 4, 8, false};
    Decoded src(s);
    for (size_t i = 0; i < src.texels.size(); ++i) src.texels[i] = (i % 5 == 0) ? 0xff : uint8_t(i % 7);
    auto up = c.buffer(src.texels.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(up->Map(0, nullptr, &mapped), "Map");
    std::memcpy(mapped, src.texels.data(), src.texels.size());
    up->Unmap(0, nullptr);
    auto staging = c.buffer(src.texels.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto map = c.map(s);
    auto l = c.begin();
    l.list->CopyBufferRegion(staging.Get(), 0, up.Get(), 0, src.texels.size());
    transition(l.list.Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    l.list->ResolveSubresourceRegion(map.Get(), UINT_MAX, 0, 0, staging.Get(), 0, nullptr, DXGI_FORMAT_R8_UINT,
                                     D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute(l);
    compare(c.decode(map.Get(), s, true), src, "minmip_buffer_round_trip");
    compare(c.decode(map.Get(), s), src, "minmip_buffer_to_texture");
  }
  {  // MinMip array: encode slice 1 from a texture, decode all slices.
    const MapShape s{32, 32, 6, 3, 4, 4, false};
    Decoded src(s), want(s);
    for (UINT y = 0; y < 8; ++y)
      for (UINT x = 0; x < 8; ++x) {
        src.at(1, 0, x, y) = uint8_t((x + y) % 4 == 3 ? 0xff : (x * y) % 6);
        src.at(0, 0, x, y) = src.at(2, 0, x, y) = 3;  // not encoded
        want.at(1, 0, x, y) = src.at(1, 0, x, y);
      }
    auto tex = c.upload(src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    auto map = c.map(s);
    auto l = c.begin();
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    l.list->ResolveSubresourceRegion(map.Get(), UINT_MAX, 0, 0, tex.Get(), 1, nullptr, DXGI_FORMAT_R8_UINT,
                                     D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);
    transition(l.list.Get(), map.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.execute(l);
    compare(c.decode(map.Get(), s), want, "minmip_array_encode_slice");
  }
  passed("encode_decode_round_trips");
}

void testCopyRules(Context &c) {
  const MapShape s{64, 64, 7, 1, 8, 8, true};
  Decoded src(s);
  for (size_t i = 0; i < src.texels.size(); ++i) src.texels[i] = (i * 7 % 3) ? 0xff : 0;
  auto tex = c.upload(src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  auto a = c.map(s), b = c.map(s);
  auto l = c.begin();
  transition(l.list.Get(), a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  l.list->ResolveSubresourceRegion(a.Get(), UINT_MAX, 0, 0, tex.Get(), UINT_MAX, nullptr, DXGI_FORMAT_R8_UINT,
                                   D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);
  transition(l.list.Get(), a.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(l.list.Get(), b.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  l.list->CopyResource(b.Get(), a.Get());
  transition(l.list.Get(), a.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(l.list.Get(), b.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  c.execute(l);
  compare(c.decode(b.Get(), s), src, "copy_resource");

  // CopyTextureRegion is not permitted for feedback maps: the list fails to close.
  l = c.begin();
  D3D12_TEXTURE_COPY_LOCATION dst{b.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION from{a.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  l.list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
  if (SUCCEEDED(l.list->Close())) fail("CopyTextureRegion on a feedback map accepted");
  // A MinMip map cannot be copied into a MipRegionUsed map.
  auto other = c.map({64, 64, 7, 1, 8, 8, false});
  l = c.begin();
  l.list->CopyResource(other.Get(), a.Get());
  if (SUCCEEDED(l.list->Close())) fail("CopyResource between feedback kinds accepted");
  // A MinMip decode with a source rect is not permitted.
  l = c.begin();
  auto plain = c.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  D3D12_RECT rect{0, 0, 1, 1};
  l.list->ResolveSubresourceRegion(plain.Get(), 0, 0, 0, other.Get(), UINT_MAX, &rect, DXGI_FORMAT_R8_UINT,
                                   D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
  if (SUCCEEDED(l.list->Close())) fail("MinMip decode with a source rect accepted");
  passed("copy_resource_allowed_copy_texture_region_rejected");
}

int run() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) {
    std::fprintf(stderr, "OWNED_D3D12_SAMPLER_FEEDBACK_FAIL step=backend\n");
    return 3;
  }
  std::printf("OWNED_D3D12_SAMPLER_FEEDBACK_BACKEND id=%s\n", backend());
  Context c;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)), "D3D12CreateDevice");
  D3D12_COMMAND_QUEUE_DESC qd{};
  check(c.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue)), "queue");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");

  // Root: b0 constants (8), table {t0, u0}, root SRV t1, sampler table s0; root_static: s0 static.
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
  D3D12_DESCRIPTOR_RANGE sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[4]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants = {0, 0, 8};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable = {2, ranges};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[2].Descriptor = {1, 0};
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable = {1, &sampler_range};

  c.sampler_states = {
      {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP},
      {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP},
      {D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 0,
       D3D12_FLOAT32_MAX, 4},
      {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER},
      {D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
       0.31f, 1.0f, 3.0f},
      {D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
       D3D12_TEXTURE_ADDRESS_MODE_MIRROR, -0.77f},
  };
  const auto static_sampler_state = c.sampler_states[4];
  D3D12_STATIC_SAMPLER_DESC static_sampler{};
  static_sampler.Filter = static_sampler_state.filter;
  static_sampler.AddressU = static_sampler_state.u;
  static_sampler.AddressV = static_sampler_state.v;
  static_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.MipLODBias = static_sampler_state.bias;
  static_sampler.MaxAnisotropy = 1;
  static_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  static_sampler.MinLOD = static_sampler_state.min_lod;
  static_sampler.MaxLOD = static_sampler_state.max_lod;
  static_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  for (int variant = 0; variant < 2; ++variant) {
    D3D12_ROOT_SIGNATURE_DESC rd{variant ? 3u : 4u, params, variant ? 1u : 0u, variant ? &static_sampler : nullptr,
                                 D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> bytes, errors;
    const HRESULT serialized = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors);
    if (FAILED(serialized) && errors)
      std::fprintf(stderr, "root signature: %.*s\n", int(errors->GetBufferSize()),
                   static_cast<const char *>(errors->GetBufferPointer()));
    check(serialized, "serialize root");
    check(c.device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                        IID_PPV_ARGS(variant ? &c.root_static : &c.root)),
          "root");
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.heap)), "heap");
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.cpu_heap)), "cpu heap");
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, UINT(c.sampler_states.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.samplers)), "sampler heap");
  c.increment = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  c.sampler_increment = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (UINT i = 0; i < c.sampler_states.size(); ++i) {
    const auto d = c.sampler_states[i].desc();
    c.device->CreateSampler(&d, c.cpu(c.samplers.Get(), i, c.sampler_increment));
  }
  // A PSO is compiled against its root signature (the static sampler binding is baked in), so the static-sampler
  // root gets its own PSOs.
  for (int static_root = 0; static_root < 2; ++static_root)
    for (int used = 0; used < 2; ++used)
      for (int array = 0; array < 2; ++array) {
        const auto cs = shader(std::string("cs_main_") + (used ? "used" : "minmip") + (array ? "_array" : ""));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{static_root ? c.root_static.Get() : c.root.Get(), {cs.data(), cs.size()}};
        check(c.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&c.cs[static_root][used][array])),
              "compute pso");
      }

  int tier = 0;
  testCaps(c, tier);
  testCreation(c);
  testCleared(c);
  testPlaced(c);
  testTranscode(c);
  testCopyRules(c);

  const char *sampler_names[] = {"point_clamp", "linear_wrap", "aniso4", "linear_border", "mippoint_mirror_lod",
                                 "magpoint_mirroronce_bias"};
  const MapShape shapes[] = {{64, 64, 7, 1, 8, 8, false}, {64, 64, 7, 1, 8, 8, true},
                             {60, 36, 6, 1, 8, 4, false}, {60, 36, 6, 1, 8, 4, true},
                             {32, 32, 6, 3, 4, 4, false}, {32, 32, 6, 3, 4, 4, true}};
  const char *shape_names[] = {"pow2_minmip", "pow2_used", "npot_minmip", "npot_used", "array_minmip", "array_used"};
  for (int sh = 0; sh < 6; ++sh)
    for (UINT si = 0; si < c.sampler_states.size(); ++si)
      runCompute(c, std::string("cs_") + shape_names[sh] + "_" + sampler_names[si], shapes[sh], si, {},
                 writePattern(shapes[sh]));
  // Tier 1.0: any SRV (a mip/slice subrange and a ResourceMinLODClamp), any address mode, static samplers.
  runCompute(c, "cs_srv_subrange_minmip", shapes[0], 1, {1, 3, 0, 0, 1.5f}, writePattern(shapes[0]));
  runCompute(c, "cs_srv_subrange_used", shapes[1], 2, {2, 4, 0, 0, 0.25f}, writePattern(shapes[1]));
  runCompute(c, "cs_srv_array_subrange_used", shapes[5], 1, {1, 4, 1, 2, 0}, writePattern(shapes[5]));
  runCompute(c, "cs_static_sampler_used", shapes[3], 4, {}, writePattern(shapes[3]), true);
  runCompute(c, "cs_null_paired_texture_drops_writes", shapes[1], 1, {}, writePattern(shapes[1]), false, true);
  for (int sh : {0, 1, 4, 5})
    for (UINT si : {1u, 2u, 4u}) {
      const std::string base = std::string(shape_names[sh]) + "_" + sampler_names[si];
      runPixel(c, "ps_implicit_" + base, shapes[sh], si, "ps_implicit", 0, 0, 2);
      runPixel(c, "ps_bias_" + base, shapes[sh], si, "ps_bias", -1.3f, 0, 1);
      runPixel(c, "ps_clamp_" + base, shapes[sh], si, "ps_clamp", 0, 2.6f, 0);
    }
  std::printf("OWNED_D3D12_SAMPLER_FEEDBACK_PASS cases=%d tier=%d\n", g_cases, tier);
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception &) {
    return 1;
  }
}
