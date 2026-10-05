// ViewInstancingTier 1 on the owned D3D12 device, verified with exact pixels. Each case prints
// OWNED_D3D12_VIEW_INSTANCING_CASE_PASS <name>; the final line is OWNED_D3D12_VIEW_INSTANCING_PASS cases=N tier=T.
// The DXIL shaders (view-instancing.hlsl, frozen by dxil/build-view-instancing.sh) are read beside the executable.
// A view-instanced pixel is uint4(tag, upstream, ps SV_ViewID, 0xc0de); upstream is (stage << 8) |
// (previous stage SV_ViewID << 4) | SV_ViewID of the stage feeding the rasterizer (stage 1 VS, 2 GS, 3 DS).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_cases = 0;
constexpr uint32_t kClear = 7;  // every untouched texel channel

[[noreturn]] void fail(const std::string &step, HRESULT actual = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_VIEW_INSTANCING_FAIL step=%s hr=%08lx\n", step.c_str(),
               static_cast<unsigned long>(actual));
  std::fflush(stderr);
  throw std::runtime_error(step);
}
void check(HRESULT actual, const char *step) {
  if (FAILED(actual)) fail(step, actual);
}
void passed(const std::string &name) {
  ++g_cases;
  std::printf("OWNED_D3D12_VIEW_INSTANCING_CASE_PASS %s\n", name.c_str());
  std::fflush(stdout);
}

std::vector<uint8_t> shader(const char *entry) {
  std::wstring name = L"view-instancing-";
  for (const char *c = entry; *c; ++c) name += static_cast<wchar_t>(*c);
  name += L".dxil";
  HANDLE file = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail(std::string("open ") + entry, HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 24)) {
    CloseHandle(file);
    fail(std::string("size ") + entry);
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != data.size()) fail(std::string("read ") + entry);
  return data;
}

/* A pipeline state stream; every subobject is pointer aligned as D3D12 requires. */
class Stream {
public:
  template <typename T> void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T &value) {
    align(alignof(void *));
    append(&type, sizeof(type));
    align(alignof(T) > sizeof(type) ? alignof(T) : sizeof(type));
    append(&value, sizeof(value));
    align(alignof(void *));
  }
  D3D12_PIPELINE_STATE_STREAM_DESC desc() { return {bytes_.size(), bytes_.data()}; }

private:
  void align(size_t a) { bytes_.resize((bytes_.size() + a - 1) / a * a); }
  void append(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }
  std::vector<uint8_t> bytes_;
};

struct Shaders {
  std::vector<uint8_t> vs = shader("vs_main"), vs_indices = shader("vs_indices"),
                       ps = shader("ps_uint"), ps_unorm = shader("ps_unorm"),
                       gs = shader("gs_view"), vs_tess = shader("vs_tess"), hs = shader("hs_view"),
                       ds = shader("ds_view");
};

enum class Stages { VsPs, VsGsPs, Tessellation };

struct PsoDesc {
  Stages stages = Stages::VsPs;
  DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT;
  UINT samples = 1;
  bool view_instancing = true;  // false: no VIEW_INSTANCING subobject
  std::vector<D3D12_VIEW_INSTANCE_LOCATION> locations;
  bool masking = false;
  bool authored_indices = false;
};

struct Context {
  ComPtr<ID3D12Device2> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12RootSignature> root;
  UINT64 fence_value = 0;
  Shaders shaders;

  ComPtr<ID3D12PipelineState> pso(const PsoDesc &d) {
    Stream s;
    auto bytecode = [](const std::vector<uint8_t> &b) { return D3D12_SHADER_BYTECODE{b.data(), b.size()}; };
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root.Get());
    if (d.stages == Stages::Tessellation) {
      s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, bytecode(shaders.vs_tess));
      s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, bytecode(shaders.hs));
      s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, bytecode(shaders.ds));
    } else {
      s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, bytecode(d.authored_indices ? shaders.vs_indices : shaders.vs));
    }
    if (d.stages == Stages::VsGsPs) s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, bytecode(shaders.gs));
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,
          bytecode(d.format == DXGI_FORMAT_R8G8B8A8_UNORM ? shaders.ps_unorm : shaders.ps));
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
          d.stages == Stages::Tessellation ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                           : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.RTFormats[0] = d.format;
    formats.NumRenderTargets = 1;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{d.samples, 0});
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, raster);
    D3D12_DEPTH_STENCIL_DESC depth{};
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, depth);
    if (d.view_instancing)
      s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,
            D3D12_VIEW_INSTANCING_DESC{static_cast<UINT>(d.locations.size()),
                                       d.locations.empty() ? nullptr : d.locations.data(),
                                       d.masking ? D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING
                                                 : D3D12_VIEW_INSTANCING_FLAG_NONE});
    const auto desc = s.desc();
    ComPtr<ID3D12PipelineState> result;
    check(device->CreatePipelineState(&desc, IID_PPV_ARGS(&result)), "CreatePipelineState");
    return result;
  }

  struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList1> list;
  };
  List begin(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
    List result;
    check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&result.allocator)), "allocator");
    check(device->CreateCommandList(0, type, result.allocator.Get(), nullptr, IID_PPV_ARGS(&result.list)), "list");
    return result;
  }
  void execute(List &list) {
    check(list.list->Close(), "Close");
    ID3D12CommandList *lists[] = {list.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), ++fence_value), "Signal");
    check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
    const DWORD waited = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0) fail("bounded GPU completion");
    check(device->GetDeviceRemovedReason(), "device removed");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&result)),
          "buffer");
    return result;
  }
};

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}

using Texel = std::array<uint32_t, 4>;

/* A 2D array render target (cleared to kClear) with one RTV over every slice. */
struct Target {
  Context &c;
  UINT width, height, slices, samples;
  DXGI_FORMAT format;
  ComPtr<ID3D12Resource> texture;
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};

  Target(Context &ctx, UINT w, UINT h, UINT n, DXGI_FORMAT f = DXGI_FORMAT_R32G32B32A32_UINT, UINT s = 1)
      : c(ctx), width(w), height(h), slices(n), samples(s), format(f) {
    texture = make(samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "rtv heap");
    rtv = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = format;
    if (samples > 1) {
      vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
      vd.Texture2DMSArray = {0, slices};
    } else {
      vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      vd.Texture2DArray = {0, 0, slices, 0};
    }
    c.device->CreateRenderTargetView(texture.Get(), &vd, rtv);
  }
  ComPtr<ID3D12Resource> make(UINT s, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(slices);
    desc.MipLevels = 1;
    desc.SampleDesc.Count = s;
    desc.Format = format;
    desc.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    const float value = format == DXGI_FORMAT_R8G8B8A8_UNORM ? kClear / 255.0f : float(kClear);
    for (float &channel : clear.Color) channel = value;
    ComPtr<ID3D12Resource> result;
    check(c.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state,
                                            (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clear : nullptr,
                                            IID_PPV_ARGS(&result)),
          "texture");
    return result;
  }
  /* Binds the target, clears it to kClear, sets `viewports` (each with a full-target scissor) and the root signature. */
  void bind(ID3D12GraphicsCommandList *list, const std::vector<D3D12_VIEWPORT> &viewports) {
    const float value = format == DXGI_FORMAT_R8G8B8A8_UNORM ? kClear / 255.0f : float(kClear);
    const float color[4] = {value, value, value, value};
    list->ClearRenderTargetView(rtv, color, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    std::vector<D3D12_RECT> scissors(viewports.size(), D3D12_RECT{0, 0, LONG(width), LONG(height)});
    list->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
    list->SetGraphicsRootSignature(c.root.Get());
  }
  D3D12_VIEWPORT full() const { return {0, 0, float(width), float(height), 0, 1}; }

  /* Every texel, [slice][y][x]; UNORM channels are returned as bytes. */
  std::vector<Texel> read() {
    auto list = c.begin();
    ComPtr<ID3D12Resource> source = texture;
    if (samples > 1) {
      source = make(1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
      transition(list.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      for (UINT s = 0; s < slices; ++s) list.list->ResolveSubresource(source.Get(), s, texture.Get(), s, format);
      transition(list.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
      transition(list.list.Get(), source.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    } else {
      transition(list.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    const UINT texel = format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 16;
    const UINT pitch = (width * texel + 255) & ~255u;
    const UINT64 slice_bytes = UINT64(pitch) * height;
    auto readback = c.buffer(slice_bytes * slices, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT s = 0; s < slices; ++s) {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = {slice_bytes * s, {format, width, height, 1, pitch}};
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = source.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = s;
      list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (samples == 1)
      transition(list.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    c.execute(list);
    void *mapped = nullptr;
    D3D12_RANGE range{0, SIZE_T(slice_bytes * slices)};
    check(readback->Map(0, &range, &mapped), "Map");
    std::vector<Texel> result(size_t(width) * height * slices);
    for (UINT s = 0; s < slices; ++s)
      for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
          const auto *row = static_cast<const uint8_t *>(mapped) + slice_bytes * s + UINT64(pitch) * y;
          Texel &t = result[(size_t(s) * height + y) * width + x];
          if (texel == 4) {
            for (int i = 0; i < 4; ++i) t[i] = row[x * 4 + i];
          } else {
            std::memcpy(t.data(), row + x * 16, 16);
          }
        }
    D3D12_RANGE none{};
    readback->Unmap(0, &none);
    return result;
  }
};

/* Expected texel of view `view` for `stage` (1 VS, 2 GS, 3 DS); `previous` is the upstream stage's SV_ViewID. */
Texel viewTexel(uint32_t tag, uint32_t stage, uint32_t view, uint32_t previous, bool unorm = false) {
  const uint32_t up = (stage << 8) | (stage == 1 ? 0 : previous << 4) | view;
  return unorm ? Texel{tag, up & 0xffu, view, 255} : Texel{tag, up, view, 0xc0deu};
}
constexpr Texel kUntouched{kClear, kClear, kClear, kClear};

/* `expected(slice, x, y)` against every texel; the first mismatch fails the case. */
template <typename Expected> void compare(const char *name, Target &t, Expected &&expected) {
  const auto texels = t.read();
  int mismatches = 0;
  for (UINT s = 0; s < t.slices; ++s)
    for (UINT y = 0; y < t.height; ++y)
      for (UINT x = 0; x < t.width; ++x) {
        const Texel got = texels[(size_t(s) * t.height + y) * t.width + x];
        const Texel want = expected(s, x, y);
        if (got != want && mismatches++ < 8) {
          std::fprintf(stderr,
                       "OWNED_D3D12_VIEW_INSTANCING_MISMATCH case=%s slice=%u x=%u y=%u got=%x,%x,%x,%x "
                       "want=%x,%x,%x,%x\n",
                       name, s, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        }
      }
  if (mismatches) fail(name);
  passed(name);
}

void draw(Context::List &list, ID3D12PipelineState *pso, uint32_t tag, bool patch = false) {
  list.list->SetPipelineState(pso);
  list.list->IASetPrimitiveTopology(patch ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                          : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list.list->SetGraphicsRoot32BitConstant(0, tag, 0);
  list.list->DrawInstanced(3, 1, 0, 0);
}

/* Render target array index locations: view v lands in slice locations[v] (an out-of-range slice selects 0). */
void testArraySlices(Context &c, const char *name, std::vector<UINT> slices, UINT array_size,
                     Stages stages = Stages::VsPs) {
  PsoDesc d;
  d.stages = stages;
  for (UINT s : slices) d.locations.push_back({0, s});
  auto pso = c.pso(d);
  Target t(c, 8, 8, array_size);
  auto list = c.begin();
  t.bind(list.list.Get(), {t.full()});
  draw(list, pso.Get(), 0x40, stages == Stages::Tessellation);
  c.execute(list);
  const uint32_t stage = stages == Stages::VsPs ? 1 : stages == Stages::VsGsPs ? 2 : 3;
  compare(name, t, [&](UINT slice, UINT, UINT) {
    // Views draw in order, so the last view resolving to a slice wins.
    Texel want = kUntouched;
    for (UINT v = 0; v < slices.size(); ++v)
      if ((slices[v] < array_size ? slices[v] : 0) == slice) want = viewTexel(0x40, stage, v, v);
    return want;
  });
}

/* Viewport array index locations: four 4x4 viewports side by side on one 16x4 slice. */
void testViewports(Context &c, const char *name, std::vector<UINT> viewports) {
  PsoDesc d;
  for (UINT v : viewports) d.locations.push_back({v, 0});
  auto pso = c.pso(d);
  Target t(c, 16, 4, 1);
  auto list = c.begin();
  t.bind(list.list.Get(), {{0, 0, 4, 4, 0, 1}, {4, 0, 4, 4, 0, 1}, {8, 0, 4, 4, 0, 1}, {12, 0, 4, 4, 0, 1}});
  draw(list, pso.Get(), 0x50);
  c.execute(list);
  compare(name, t, [&](UINT, UINT x, UINT) {
    Texel want = kUntouched;
    for (UINT v = 0; v < viewports.size(); ++v)
      if ((viewports[v] < 4 ? viewports[v] : 0) == x / 4) want = viewTexel(0x50, 1, v, v);
    return want;
  });
}

/* SetViewInstanceMask: honored only with ENABLE_VIEW_INSTANCE_MASKING; a masked-off view is not drawn at all. */
void testMasking(Context &c) {
  PsoDesc d;
  d.locations = {{0, 0}, {0, 1}, {0, 2}, {0, 3}};
  d.masking = true;
  auto masked = c.pso(d);
  d.masking = false;
  auto unmasked = c.pso(d);
  struct Case {
    const char *name;
    ID3D12PipelineState *pso;
    UINT mask, drawn;
  } cases[] = {{"mask_flag_0b1010", masked.Get(), 0b1010, 0b1010},
               {"mask_flag_upper_bits_ignored", masked.Get(), 0xfffffff1u, 0b0001},
               {"mask_flag_zero_draws_nothing", masked.Get(), 0, 0},
               {"mask_without_flag_ignored", unmasked.Get(), 0b0001, 0b1111}};
  for (const auto &k : cases) {
    Target t(c, 8, 8, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {t.full()});
    list.list->SetViewInstanceMask(k.mask);
    draw(list, k.pso, 0x60);
    c.execute(list);
    compare(k.name, t, [&](UINT slice, UINT, UINT) {
      return (k.drawn >> slice) & 1 ? viewTexel(0x60, 1, slice, slice) : kUntouched;
    });
  }
}

/* PSOs without view instancing (no subobject, count 0, count 1) read SV_ViewID 0; a view-instanced draw earlier in
 * the same list must not leak its last view id. */
void testNoViewInstancing(Context &c) {
  PsoDesc vi;
  vi.locations = {{0, 0}, {0, 1}, {0, 2}, {0, 3}};
  auto instanced = c.pso(vi);
  PsoDesc none;
  none.view_instancing = false;
  auto plain = c.pso(none);
  PsoDesc zero;
  auto count0 = c.pso(zero);
  PsoDesc one;
  one.locations = {{0, 2}};
  auto count1 = c.pso(one);
  {
    Target t(c, 8, 8, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {t.full()});
    draw(list, instanced.Get(), 0x70);
    draw(list, plain.Get(), 0x71);
    c.execute(list);
    compare("no_subobject_reads_zero_after_instanced_draw", t, [&](UINT slice, UINT, UINT) {
      return slice == 0 ? viewTexel(0x71, 1, 0, 0) : viewTexel(0x70, 1, slice, slice);
    });
  }
  {
    Target t(c, 8, 8, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {t.full()});
    draw(list, count0.Get(), 0x72);
    c.execute(list);
    compare("count0_reads_zero", t,
            [&](UINT slice, UINT, UINT) { return slice == 0 ? viewTexel(0x72, 1, 0, 0) : kUntouched; });
  }
  {
    Target t(c, 8, 8, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {t.full()});
    draw(list, count1.Get(), 0x73);
    c.execute(list);
    compare("count1_location_slice2", t,
            [&](UINT slice, UINT, UINT) { return slice == 2 ? viewTexel(0x73, 1, 0, 0) : kUntouched; });
  }
}

/* 4x MSAA (captured-pixel path): view locations by slice and by viewport, including out-of-range -> 0. */
void testMultisample(Context &c) {
  {
    PsoDesc d;
    d.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.samples = 4;
    d.locations = {{0, 1}, {0, 3}, {0, 2}};  // slice 3 is out of range on a 3-slice array -> slice 0
    auto pso = c.pso(d);
    Target t(c, 8, 8, 3, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {t.full()});
    draw(list, pso.Get(), 0x41);
    c.execute(list);
    compare("msaa4x_rtai_3_views_out_of_range_to_0", t, [&](UINT slice, UINT, UINT) {
      const UINT view = slice == 1 ? 0 : slice == 0 ? 1 : 2;
      return viewTexel(0x41, 1, view, view, true);
    });
  }
  {
    PsoDesc d;
    d.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.samples = 4;
    d.locations = {{3, 0}, {1, 0}, {5, 0}};  // viewport 5 is out of range with 4 viewports -> viewport 0
    auto pso = c.pso(d);
    Target t(c, 16, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {{0, 0, 4, 4, 0, 1}, {4, 0, 4, 4, 0, 1}, {8, 0, 4, 4, 0, 1}, {12, 0, 4, 4, 0, 1}});
    draw(list, pso.Get(), 0x42);
    c.execute(list);
    compare("msaa4x_vpai_3_views_out_of_range_to_0", t, [&](UINT, UINT x, UINT) {
      const UINT region = x / 4;
      return region == 3 ? viewTexel(0x42, 1, 0, 0, true)
             : region == 1 ? viewTexel(0x42, 1, 1, 1, true)
             : region == 0 ? viewTexel(0x42, 1, 2, 2, true)
                           : Texel{kClear, kClear, kClear, kClear};
    });
  }
  {
    PsoDesc d;
    d.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.samples = 4;
    d.locations = {{0, 0}, {0, 1}};
    d.masking = true;
    auto pso = c.pso(d);
    Target t(c, 8, 8, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    auto list = c.begin();
    t.bind(list.list.Get(), {t.full()});
    list.list->SetViewInstanceMask(0b10);
    draw(list, pso.Get(), 0x43);
    c.execute(list);
    compare("msaa4x_mask_0b10", t, [&](UINT slice, UINT, UINT) {
      return slice == 1 ? viewTexel(0x43, 1, 1, 1, true) : Texel{kClear, kClear, kClear, kClear};
    });
  }
}

/* ExecuteIndirect: every view replays the whole argument buffer (root constant + draw per command). */
void testExecuteIndirect(Context &c) {
  PsoDesc d;
  d.locations = {{0, 2}, {0, 0}};
  auto pso = c.pso(d);
  D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
  args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  args[0].Constant = {0, 0, 1};
  args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  struct Command {
    uint32_t tag;
    D3D12_DRAW_ARGUMENTS draw;
  };
  D3D12_COMMAND_SIGNATURE_DESC sd{sizeof(Command), 2, args, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(c.device->CreateCommandSignature(&sd, c.root.Get(), IID_PPV_ARGS(&signature)), "command signature");
  const Command commands[2] = {{0x81, {3, 1, 0, 0}}, {0x82, {3, 1, 0, 0}}};
  auto upload = c.buffer(sizeof(commands), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "Map(upload)");
  std::memcpy(mapped, commands, sizeof(commands));
  upload->Unmap(0, nullptr);
  Target t(c, 8, 8, 3);
  auto list = c.begin();
  t.bind(list.list.Get(), {t.full()});
  list.list->SetPipelineState(pso.Get());
  list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list.list->SetGraphicsRoot32BitConstant(0, 0x80, 0);
  list.list->ExecuteIndirect(signature.Get(), 2, upload.Get(), 0, nullptr, 0);
  c.execute(list);
  compare("execute_indirect_2_views_2_commands", t, [&](UINT slice, UINT, UINT) {
    return slice == 2 ? viewTexel(0x82, 1, 0, 0) : slice == 0 ? viewTexel(0x82, 1, 1, 1) : kUntouched;
  });

  {
    PsoDesc masked;
    masked.locations = {{0, 2}, {0, 1}, {0, 0}};
    masked.masking = true;
    auto masked_pso = c.pso(masked);
    Target target(c, 8, 8, 3);
    auto commands_list = c.begin();
    target.bind(commands_list.list.Get(), {target.full()});
    commands_list.list->SetPipelineState(masked_pso.Get());
    commands_list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands_list.list->SetViewInstanceMask(0b101);
    commands_list.list->ExecuteIndirect(signature.Get(), 2, upload.Get(), 0, nullptr, 0);
    // A direct draw in the same pass must restore native mapping offset 1, not keep the ICB neutral offsets.
    commands_list.list->SetViewInstanceMask(0b010);
    draw(commands_list, masked_pso.Get(), 0x83);
    c.execute(commands_list);
    compare("execute_indirect_masked_rtai_then_direct", target, [&](UINT slice, UINT, UINT) {
      return slice == 2 ? viewTexel(0x82, 1, 0, 0) : slice == 0 ? viewTexel(0x82, 1, 2, 2)
                                                             : viewTexel(0x83, 1, 1, 1);
    });
  }
  {
    PsoDesc masked;
    masked.locations = {{3, 0}, {1, 0}, {5, 0}};
    masked.masking = true;
    auto masked_pso = c.pso(masked);
    D3D12_INDIRECT_ARGUMENT_DESC plain_argument{};
    plain_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC plain_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &plain_argument, 0};
    ComPtr<ID3D12CommandSignature> plain_signature;
    check(c.device->CreateCommandSignature(&plain_desc, nullptr, IID_PPV_ARGS(&plain_signature)),
          "plain draw signature");
    const D3D12_DRAW_ARGUMENTS plain_draws[2] = {{3, 1, 0, 0}, {3, 1, 0, 0}};
    auto plain_upload = c.buffer(sizeof(plain_draws), D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
    check(plain_upload->Map(0, nullptr, &mapped), "Map(plain upload)");
    std::memcpy(mapped, plain_draws, sizeof(plain_draws));
    plain_upload->Unmap(0, nullptr);
    Target target(c, 16, 4, 1);
    auto commands_list = c.begin();
    target.bind(commands_list.list.Get(), {{0, 0, 4, 4, 0, 1}, {4, 0, 4, 4, 0, 1},
                                          {8, 0, 4, 4, 0, 1}, {12, 0, 4, 4, 0, 1}});
    commands_list.list->SetPipelineState(masked_pso.Get());
    commands_list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands_list.list->SetViewInstanceMask(0b101);
    commands_list.list->SetGraphicsRoot32BitConstant(0, 0x82, 0);
    commands_list.list->ExecuteIndirect(plain_signature.Get(), 2, plain_upload.Get(), 0, nullptr, 0);
    c.execute(commands_list);
    compare("execute_indirect_masked_vpai_out_of_range", target, [&](UINT, UINT x, UINT) {
      return x / 4 == 3 ? viewTexel(0x82, 1, 0, 0) : x / 4 == 0 ? viewTexel(0x82, 1, 2, 2) : kUntouched;
    });
  }
  {
    PsoDesc authored;
    authored.locations = {{1, 1}, {2, 2}};
    authored.authored_indices = true;
    auto authored_pso = c.pso(authored);
    struct IndexedCommand {
      uint32_t tag, viewport, layer;
      D3D12_DRAW_ARGUMENTS draw;
    };
    args[0].Constant.Num32BitValuesToSet = 3;
    D3D12_COMMAND_SIGNATURE_DESC indexed_desc{sizeof(IndexedCommand), 2, args, 0};
    ComPtr<ID3D12CommandSignature> indexed_signature;
    check(c.device->CreateCommandSignature(&indexed_desc, c.root.Get(), IID_PPV_ARGS(&indexed_signature)),
          "indexed draw signature");
    const IndexedCommand indexed_commands[2] = {{0x1181, 1, 1, {3, 1, 0, 0}},
                                                 {0x1182, 1, 1, {3, 1, 0, 0}}};
    auto indexed_upload = c.buffer(sizeof(indexed_commands), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
    check(indexed_upload->Map(0, nullptr, &mapped), "Map(indexed upload)");
    std::memcpy(mapped, indexed_commands, sizeof(indexed_commands));
    indexed_upload->Unmap(0, nullptr);
    Target target(c, 12, 4, 3);
    auto commands_list = c.begin();
    target.bind(commands_list.list.Get(), {{0, 0, 4, 4, 0, 1}, {4, 0, 4, 4, 0, 1}, {8, 0, 4, 4, 0, 1}});
    commands_list.list->SetPipelineState(authored_pso.Get());
    commands_list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands_list.list->ExecuteIndirect(indexed_signature.Get(), 2, indexed_upload.Get(), 0, nullptr, 0);
    c.execute(commands_list);
    compare("execute_indirect_adds_authored_indices_and_clamps", target, [&](UINT slice, UINT x, UINT) {
      return slice == 2 && x / 4 == 2 ? viewTexel(0x1182, 1, 0, 0)
             : slice == 0 && x / 4 == 0 ? viewTexel(0x1182, 1, 1, 1) : kUntouched;
    });
  }
}

// Full-width shader indices isolate UINT32 addition on each axis. No GS, tessellation, or MSAA:
// these draws take the native ICB VS epilogue, rather than a replay lowering.
void testIndirectFullWidthIndices(Context &c) {
  D3D12_INDIRECT_ARGUMENT_DESC plain_argument{};
  plain_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  D3D12_COMMAND_SIGNATURE_DESC plain_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &plain_argument, 0};
  ComPtr<ID3D12CommandSignature> plain_signature;
  check(c.device->CreateCommandSignature(&plain_desc, nullptr, IID_PPV_ARGS(&plain_signature)),
        "full-width draw signature");
  const D3D12_DRAW_ARGUMENTS plain_draw{3, 1, 0, 0};
  auto plain_upload = c.buffer(sizeof(plain_draw), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(plain_upload->Map(0, nullptr, &mapped), "Map(full-width draw)");
  std::memcpy(mapped, &plain_draw, sizeof(plain_draw));
  plain_upload->Unmap(0, nullptr);
  struct IndexCase {
    const char *name;
    uint32_t authored;
    UINT expected;
  };
  const IndexCase index_cases[2][3] = {
    {{"execute_indirect_viewport_uint32_wrap", 0xffffffffu, 1},
     {"execute_indirect_viewport_uint32_valid", 1, 3},
     {"execute_indirect_viewport_uint32_oob", 2, 0}},
    {{"execute_indirect_layer_uint32_wrap", 0xffffffffu, 1},
     {"execute_indirect_layer_uint32_valid", 1, 3},
     {"execute_indirect_layer_uint32_oob", 2, 0}},
  };
  for (unsigned axis = 0; axis < 2; ++axis) {
    PsoDesc authored;
    authored.locations = {{axis == 0 ? 2u : 0u, axis == 1 ? 2u : 0u}};
    authored.authored_indices = true;
    auto authored_pso = c.pso(authored);
    for (const auto &test : index_cases[axis]) {
      Target target(c, 16, 4, 4);
      auto commands_list = c.begin();
      target.bind(commands_list.list.Get(), {{0, 0, 4, 4, 0, 1}, {4, 0, 4, 4, 0, 1},
                                            {8, 0, 4, 4, 0, 1}, {12, 0, 4, 4, 0, 1}});
      commands_list.list->SetPipelineState(authored_pso.Get());
      commands_list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      const uint32_t constants[] = {0xa1, axis == 0 ? test.authored : 0, axis == 1 ? test.authored : 0};
      commands_list.list->SetGraphicsRoot32BitConstants(0, 3, constants, 0);
      commands_list.list->ExecuteIndirect(plain_signature.Get(), 1, plain_upload.Get(), 0, nullptr, 0);
      c.execute(commands_list);
      compare(test.name, target, [&](UINT slice, UINT x, UINT) {
        const UINT viewport = axis == 0 ? test.expected : 0;
        const UINT layer = axis == 1 ? test.expected : 0;
        return slice == layer && x / 4 == viewport ? viewTexel(0xa1, 1, 0, 0) : kUntouched;
      });
    }
  }
}

/* Bundles do not inherit the caller's view instance mask (it starts at 0), and a mask set in a bundle leaks back
 * to the caller after ExecuteBundle. */
void testBundles(Context &c) {
  PsoDesc d;
  d.locations = {{0, 0}, {0, 1}, {0, 2}, {0, 3}};
  d.masking = true;
  auto pso = c.pso(d);
  auto bundle = c.begin(D3D12_COMMAND_LIST_TYPE_BUNDLE);
  bundle.list->SetGraphicsRootSignature(c.root.Get());
  bundle.list->SetPipelineState(pso.Get());
  bundle.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  bundle.list->SetGraphicsRoot32BitConstant(0, 0x91, 0);
  bundle.list->DrawInstanced(3, 1, 0, 0);  // mask 0 inside the bundle: nothing is drawn
  bundle.list->SetViewInstanceMask(0b0100);
  bundle.list->SetGraphicsRoot32BitConstant(0, 0x92, 0);
  bundle.list->DrawInstanced(3, 1, 0, 0);  // view 2 only
  check(bundle.list->Close(), "Close(bundle)");
  Target t(c, 8, 8, 4);
  auto list = c.begin();
  t.bind(list.list.Get(), {t.full()});
  list.list->SetPipelineState(pso.Get());
  list.list->SetViewInstanceMask(0b0011);  // not inherited by the bundle
  list.list->ExecuteBundle(bundle.list.Get());
  draw(list, pso.Get(), 0x93);  // the bundle's mask 0b0100 leaked back: view 2 only
  c.execute(list);
  compare("bundle_mask_reset_and_leak", t,
          [&](UINT slice, UINT, UINT) { return slice == 2 ? viewTexel(0x93, 1, 2, 2) : kUntouched; });
  // The bundle on its own: only its second draw (view 2, tag 0x92) reaches the target.
  Target t2(c, 8, 8, 4);
  auto list2 = c.begin();
  t2.bind(list2.list.Get(), {t2.full()});
  list2.list->SetPipelineState(pso.Get());
  list2.list->SetViewInstanceMask(0b1111);
  list2.list->ExecuteBundle(bundle.list.Get());
  c.execute(list2);
  compare("bundle_starts_with_mask_0", t2,
          [&](UINT slice, UINT, UINT) { return slice == 2 ? viewTexel(0x92, 1, 2, 2) : kUntouched; });
}

/* Tier 1 counts a view-instanced draw as one ordinary draw per enabled view. */
void testPipelineStatistics(Context &c) {
  PsoDesc d;
  d.locations = {{0, 0}, {0, 1}, {0, 2}};
  d.masking = true;
  auto pso = c.pso(d);
  D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2, 0};
  ComPtr<ID3D12QueryHeap> heap;
  check(c.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)), "query heap");
  auto results = c.buffer(2 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), D3D12_HEAP_TYPE_READBACK,
                          D3D12_RESOURCE_STATE_COPY_DEST);
  Target t(c, 8, 8, 3);
  auto list = c.begin();
  t.bind(list.list.Get(), {t.full()});
  list.list->SetViewInstanceMask(0b111);
  list.list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  draw(list, pso.Get(), 0xa0);
  list.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  list.list->SetViewInstanceMask(0b101);
  list.list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
  draw(list, pso.Get(), 0xa1);
  list.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
  list.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, results.Get(), 0);
  c.execute(list);
  D3D12_QUERY_DATA_PIPELINE_STATISTICS s[2]{};
  void *mapped = nullptr;
  D3D12_RANGE range{0, sizeof(s)};
  check(results->Map(0, &range, &mapped), "Map(statistics)");
  std::memcpy(s, mapped, sizeof(s));
  D3D12_RANGE none{};
  results->Unmap(0, &none);
  for (int i = 0; i < 2; ++i) {
    const UINT64 views = i == 0 ? 3 : 2;
    std::printf("OWNED_D3D12_VIEW_INSTANCING_STATISTICS views=%llu ia_vertices=%llu ia_primitives=%llu vs=%llu "
                "c_invocations=%llu c_primitives=%llu ps=%llu\n",
                (unsigned long long)views, (unsigned long long)s[i].IAVertices,
                (unsigned long long)s[i].IAPrimitives, (unsigned long long)s[i].VSInvocations,
                (unsigned long long)s[i].CInvocations, (unsigned long long)s[i].CPrimitives,
                (unsigned long long)s[i].PSInvocations);
    if (s[i].IAVertices != 3 * views || s[i].IAPrimitives != views || s[i].VSInvocations != 3 * views ||
        s[i].CInvocations != views || s[i].CPrimitives != views || s[i].PSInvocations != 64 * views ||
        s[i].GSInvocations || s[i].HSInvocations || s[i].DSInvocations || s[i].CSInvocations)
      fail("pipeline statistics");
  }
  compare("pipeline_statistics_count_views", t, [&](UINT slice, UINT, UINT) {
    return slice == 1 ? viewTexel(0xa0, 1, 1, 1) : viewTexel(0xa1, 1, slice, slice);
  });
}

/* Invalid VIEW_INSTANCING descriptions fail creation with E_INVALIDARG. */
void testInvalid(Context &c) {
  const D3D12_VIEW_INSTANCE_LOCATION five[5] = {};
  const D3D12_VIEW_INSTANCE_LOCATION bad_vp[1] = {{16, 0}};
  const D3D12_VIEW_INSTANCE_LOCATION bad_rt[1] = {{0, 2048}};
  const D3D12_VIEW_INSTANCING_DESC cases[] = {
      {5, five, D3D12_VIEW_INSTANCING_FLAG_NONE},
      {2, five, static_cast<D3D12_VIEW_INSTANCING_FLAGS>(2)},
      {2, nullptr, D3D12_VIEW_INSTANCING_FLAG_NONE},
      {1, bad_vp, D3D12_VIEW_INSTANCING_FLAG_NONE},
      {1, bad_rt, D3D12_VIEW_INSTANCING_FLAG_NONE},
  };
  for (const auto &vi : cases) {
    Stream s;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, c.root.Get());
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{c.shaders.vs.data(), c.shaders.vs.size()});
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE{c.shaders.ps.data(), c.shaders.ps.size()});
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.RTFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
    formats.NumRenderTargets = 1;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, vi);
    const auto desc = s.desc();
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = c.device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (hr != E_INVALIDARG || pso) fail("invalid view instancing desc accepted", hr);
  }
  passed("invalid_descs_rejected");
}

int run() {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) {
    std::fprintf(stderr, "OWNED_D3D12_VIEW_INSTANCING_FAIL step=backend\n");
    return 3;
  }
  std::printf("OWNED_D3D12_VIEW_INSTANCING_BACKEND id=%s\n", backend());
  Context c;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)), "D3D12CreateDevice");
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3)), "OPTIONS3");
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  check(c.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&c.queue)), "queue");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)), "fence");
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, 3};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> bytes, errors;
  check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "serialize root");
  check(c.device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&c.root)),
        "root");

  testArraySlices(c, "rtai_2_views", {3, 1}, 4);
  testArraySlices(c, "rtai_4_views", {2, 0, 3, 1}, 4);
  testArraySlices(c, "rtai_out_of_range_to_0", {5, 2}, 4);
  testViewports(c, "vpai_2_views", {2, 1});
  testViewports(c, "vpai_4_views", {3, 0, 2, 1});
  testViewports(c, "vpai_out_of_range_to_0", {15, 3});
  testMasking(c);
  testArraySlices(c, "gs_rtai_2_views", {1, 0}, 2, Stages::VsGsPs);
  testArraySlices(c, "tess_rtai_2_views", {1, 0}, 2, Stages::Tessellation);
  testNoViewInstancing(c);
  testMultisample(c);
  testExecuteIndirect(c);
  testIndirectFullWidthIndices(c);
  testBundles(c);
  testPipelineStatistics(c);
  testInvalid(c);
  std::printf("OWNED_D3D12_VIEW_INSTANCING_PASS cases=%d tier=%d\n", g_cases, int(options3.ViewInstancingTier));
  return 0;
}

} // namespace

int main() {
  try {
    return run();
  } catch (const std::exception &) {
    return 1;
  }
}
