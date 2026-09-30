// GPU oracle for the production presenter, separate from the public DXGI HRESULT fixture.
// Link this PE to the production libdxmt and WineMetal import library, not a copied shader.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dxmt_presenter.hpp"
#include "Metal.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dxmt { Logger Logger::s_instance("owned-d3d12-hdr-presenter.log"); }

namespace {
constexpr unsigned kSize = 64;
constexpr unsigned kRow = kSize * 8;
constexpr unsigned kBytes = kRow * kSize;
const char *step = "setup";
[[noreturn]] void fail(const char *why) {
  std::fprintf(stderr, "OWNED_D3D12_HDR_PRESENTER_FAIL step=%s reason=%s\n", step, why);
  std::exit(1);
}
void require(bool ok, const char *why) { if (!ok) fail(why); }
float pq(float nits) {
  constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
  constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  float p = std::pow(nits / 10000.f, m1);
  return std::pow((c1 + c2 * p) / (1.f + c3 * p), m2);
}
double pqReference(double nits) {
  const double p = std::pow(nits / 10000., 0.1593017578125);
  return std::pow((0.8359375 + 18.8515625 * p) / (1. + 18.6875 * p), 78.84375);
}
double linearReference(double p) {
  const double x = std::pow(p, 1. / 78.84375);
  return 10000. * std::pow(std::max(0., x - 0.8359375) / (18.8515625 - 18.6875 * x),
                           1. / 0.1593017578125);
}
double expectedPQ(double input, double content, double display, double scale, bool enabled) {
  if (!enabled) return pqReference(input * scale);
  const double black = pqReference(1.), span = pqReference(content) - black;
  const double max = (pqReference(display) - black) / span;
  const double min = (pqReference(0.) - black) / span;
  double e = (pqReference(std::clamp(input, 1., content)) - black) / span;
  const double shoulder = 1.5 * max - 0.5;
  if (shoulder < 1. && e >= shoulder) {
    const double t = (e - shoulder) / (1. - shoulder);
    e = (2 * t * t * t - 3 * t * t + 1) * shoulder
      + (t * t * t - 2 * t * t + t) * (1. - shoulder)
      + (-2 * t * t * t + 3 * t * t) * max;
  }
  e += min * std::pow(1. - e, 4.);
  return pqReference(linearReference(e * span + black) * scale);
}
float half(unsigned short x) {
  unsigned sign = (x >> 15) & 1, exponent = (x >> 10) & 31, mantissa = x & 1023;
  float v = exponent == 31 ? (mantissa ? NAN : INFINITY) :
      exponent ? std::ldexp(float(mantissa + 1024), int(exponent) - 25) :
                 std::ldexp(float(mantissa), -24);
  return sign ? -v : v;
}
struct Window {
  HWND hwnd;
  Window() {
    static bool registered = false;
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"OwnedHDRPresenterWindow";
    if (!registered) { require(RegisterClassW(&cls), "RegisterClassW"); registered = true; }
    hwnd = CreateWindowExW(0, cls.lpszClassName, L"HDR GPU presenter", WS_OVERLAPPEDWINDOW,
                           90, 80, kSize, kSize, nullptr, nullptr, cls.hInstance, nullptr);
    require(hwnd != nullptr, "CreateWindowExW");
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
  }
  ~Window() { if (hwnd) DestroyWindow(hwnd); }
};
struct Probe {
  WMT::Reference<WMT::Array<WMT::Device>> devices;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> queue;
  obj_handle_t native_view = 0;
  WMT::MetalLayer layer;
  dxmt::InternalCommandLibrary library;
  dxmt::Presenter presenter;
  WMT::Reference<WMT::Texture> input;

  static WMT::Device firstDevice(WMT::Array<WMT::Device> d) {
    require(d && d.count() > 0, "Metal device");
    return d.object(0);
  }
  static WMT::MetalLayer makeLayer(HWND hwnd, WMT::Device device, obj_handle_t &view) {
    obj_handle_t layer = 0;
    view = CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(hwnd), device.handle, &layer);
    require(view && layer, "CreateMetalViewFromHWND");
    return WMT::MetalLayer{layer};
  }
  Probe(HWND hwnd) : devices(WMT::CopyAllDevices()), device(firstDevice(devices)),
      queue(device.newCommandQueue(8)), native_view(), layer(makeLayer(hwnd, device, native_view)),
      library(device), presenter(device, layer, library, 1.f, 1) {
    require(queue, "command queue");
    presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, WMTColorSpaceHDR_PQ, kSize, kSize, 1);
    WMTTextureInfo info{};
    info.type = WMTTextureType2D;
    info.pixel_format = WMTPixelFormatRGBA16Float;
    info.usage = WMTTextureUsageShaderRead;
    info.options = WMTResourceStorageModeShared;
    info.width = info.height = kSize;
    info.depth = 1;
    info.mipmap_level_count = info.sample_count = info.array_length = 1;
    input = device.newTexture(info);
    require(input, "input texture");
    unsigned short pixels[kSize * kSize * 4]{};
    // Binary16 PQ input is precise enough to separate the proposed mastering peaks.
    auto to_half = [](float f) {
      unsigned bits; std::memcpy(&bits, &f, sizeof(bits));
      unsigned e = (bits >> 23) & 255;
      return static_cast<unsigned short>(((bits >> 16) & 0x8000) | ((e - 112) << 10) | ((bits >> 13) & 1023));
    };
    auto highlight = to_half(pq(1400.f));
    auto peak = to_half(pq(1600.f));
    for (unsigned y = 0; y < kSize; ++y)
      for (unsigned x = 0; x < kSize; ++x) {
        unsigned short *p = pixels + (y * kSize + x) * 4;
        p[0] = p[1] = p[2] = x < kSize / 3 ? 0 : x < kSize * 2 / 3 ? highlight : peak;
        p[3] = to_half(1.f);
      }
    input.replaceRegion({0, 0, 0}, {kSize, kSize, 1}, 0, 0, pixels, kRow, kBytes);
  }
  ~Probe() { ReleaseMetalView(native_view); }
  struct Sample { float black, highlight, peak; float content, display, scale; };
  Sample render(dxmt::Presenter::PresentState state, const char *label) {
    step = label;
    WMTBufferInfo readback{};
    readback.length = kBytes;
    readback.options = WMTResourceStorageModeShared;
    auto buffer = device.newBuffer(readback);
    require(buffer && readback.memory.get(), "shared readback buffer");
    auto cmd = queue.commandBuffer();
    require(cmd, "command buffer");
    auto drawable = presenter.encodeCommands(cmd, input, state.metadata,
                                             [](auto) {}, [](auto) {});
    require(drawable && drawable.texture(), "drawable");
    wmtcmd_blit_copy_from_texture_to_buffer copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.src = drawable.texture().handle;
    copy.size = {kSize, kSize, 1};
    copy.dst = buffer.handle;
    copy.bytes_per_row = kRow;
    copy.bytes_per_image = kBytes;
    auto blit = cmd.blitCommandEncoder();
    blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    blit.endEncoding();
    cmd.presentDrawable(drawable);
    cmd.commit();
    cmd.waitUntilCompleted();
    require(cmd.status() == WMTCommandBufferStatusCompleted, "GPU completed");
    auto *bytes = static_cast<const unsigned short *>(readback.memory.get());
    auto pixel = [&](unsigned x) { return half(bytes[((kSize / 2) * kSize + x) * 4]); };
    Sample result{pixel(8), pixel(31), pixel(56), state.metadata.max_content_luminance,
                  state.metadata.max_display_luminance, state.metadata.edr_scale};
    std::printf("OWNED_D3D12_HDR_PRESENTER_FRAME frame=%s black=%f highlight=%f peak=%f content=%f display=%f scale=%f\n",
                label, result.black, result.highlight, result.peak, result.content, result.display, result.scale);
    require(std::isfinite(result.black) && std::isfinite(result.highlight) && std::isfinite(result.peak),
            "finite GPU pixels");
    return result;
  }
};
WMTHDRMetadata metadata(unsigned peak) {
  WMTHDRMetadata m{};
  m.max_mastering_luminance = peak;
  m.max_content_light_level = static_cast<unsigned short>(peak);
  m.max_frame_average_light_level = 400;
  return m;
}
void checkNear(float a, float b, float tolerance, const char *why) {
  require(std::fabs(a - b) < tolerance, why);
}
} // namespace
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto pool = WMT::MakeAutoreleasePool();
  Window w;
  Probe p(w.hwnd);
  auto a = metadata(3000), b = metadata(10000);
  p.presenter.changeHDRMetadata(&a);
  auto accepted_a = p.presenter.synchronizeLayerProperties();
  a.max_mastering_luminance = 10000; // caller's memory must not remain borrowed
  p.presenter.changeHDRMetadata(&b);
  auto early = p.render(std::move(accepted_a), "accepted-a-after-replacement");
  auto late = p.render(p.presenter.synchronizeLayerProperties(), "replaced-b");
  require(early.content < late.content, "metadata copy and per-frame snapshot");
  auto equal = metadata(0);
  p.presenter.changeHDRMetadata(&equal);
  auto equalPeaks = p.render(p.presenter.synchronizeLayerProperties(), "equal-content-display");
  require(equalPeaks.content == equalPeaks.display, "equal peaks precondition");
  require(equalPeaks.peak > 0.f && equalPeaks.black < 0.01f, "equal peaks endpoints");
  p.presenter.changeHDRMetadata(nullptr);
  auto none = p.render(p.presenter.synchronizeLayerProperties(), "none");
  // Live EDR headroom can change between frames; ordering is only meaningful at equal scale.
  // Every frame is additionally checked against the independent scale-aware oracle below.
  if (none.scale == late.scale)
    require(none.highlight > late.highlight, "metadata changes actual GPU highlight");
  if (early.scale == late.scale)
    require(early.highlight > late.highlight, "metadata snapshot changes actual GPU highlight");
  p.presenter.changeHDRMetadata(&b);
  auto again = p.render(p.presenter.synchronizeLayerProperties(), "reapplied-b");
  checkNear(again.highlight, late.highlight, 0.015f, "reapply identical metadata");
  require(none.black <= 0.01f && late.black <= 0.01f, "black endpoint");
  for (const auto &sample : {early, late, equalPeaks, again})
    checkNear(sample.highlight, static_cast<float>(expectedPQ(1400., sample.content, sample.display,
                                                              sample.scale, true)), 0.008f,
              "independent PQ/EETF highlight oracle");
  checkNear(none.highlight, static_cast<float>(expectedPQ(1400., 0., none.display, none.scale, false)),
            0.008f, "independent PQ untonemapped oracle");
  {
    // A retained real layer whose HWND was destroyed no longer has a screen.
    // Query the native bridge directly; never substitute a synthetic layer.
    Window detached;
    obj_handle_t view = 0;
    auto layer = Probe::makeLayer(detached.hwnd, p.device, view);
    NSObject_retain(layer.handle);
    require(DestroyWindow(detached.hwnd), "destroy detached window");
    detached.hwnd = nullptr;
    uint64_t version = ~uint64_t(0);
    WMTColorSpace color = WMTColorSpaceInvalid;
    WMTHDRMetadata data{};
    WMTEDRValue edr{-1.f, -1.f};
    WMTQueryDisplaySettingForLayer(layer.handle, &version, &color, &data, &edr);
    std::printf("OWNED_D3D12_HDR_PRESENTER_DETACHED version=%llu current=%f potential=%f\n",
                static_cast<unsigned long long>(version), edr.maximum_edr_color_component_value,
                edr.maximum_potential_edr_color_component_value);
    require(version == 0 && edr.maximum_edr_color_component_value == 1.f &&
            edr.maximum_potential_edr_color_component_value == 1.f, "detached native EDR defaults");
    NSObject_release(layer.handle);
    ReleaseMetalView(view);
  }
  std::puts("OWNED_D3D12_HDR_PRESENTER_PASS real_gpu=1 drawable_readback=1 frames=5 detached=1");
  return 0;
}
