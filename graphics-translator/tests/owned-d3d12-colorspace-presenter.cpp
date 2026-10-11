// Production Presenter GPU oracle. Drawable readback verifies the Metal shader,
// not Core Animation's subsequent color-managed compositing to the display.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dxmt_presenter.hpp"
#include "Metal.hpp"
#include "Metal4FX.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <utility>

namespace dxmt { Logger Logger::s_instance("owned-d3d12-colorspace-presenter.log"); }

namespace {
constexpr unsigned kSize = 64;
constexpr unsigned kResized = 48;
const char *step = "setup";
[[noreturn]] void fail(const char *why) {
  std::fprintf(stderr, "OWNED_D3D12_COLORSPACE_PRESENTER_FAIL step=%s reason=%s\n", step, why);
  std::exit(1);
}
void require(bool ok, const char *why) { if (!ok) fail(why); }
void checkNear(float actual, double expected, double tolerance, const char *why) {
  require(std::isfinite(actual) && std::fabs(actual - expected) < tolerance, why);
}
float half(unsigned short x) {
  unsigned sign = (x >> 15) & 1, exponent = (x >> 10) & 31, mantissa = x & 1023;
  float value = exponent == 31 ? (mantissa ? NAN : INFINITY) :
      exponent ? std::ldexp(float(mantissa + 1024), int(exponent) - 25) :
                 std::ldexp(float(mantissa), -24);
  return sign ? -value : value;
}
unsigned short toHalf(float f) {
  if (f == 0) return std::signbit(f) ? 0x8000 : 0;
  unsigned bits;
  std::memcpy(&bits, &f, sizeof(bits));
  unsigned e = (bits >> 23) & 255;
  if (e < 113) {
    const unsigned fraction = e < 103 ? 0 : ((bits & 0x7fffff) | 0x800000) >> (126 - e);
    return static_cast<unsigned short>(((bits >> 16) & 0x8000) | fraction);
  }
  return static_cast<unsigned short>(((bits >> 16) & 0x8000) | ((e - 112) << 10) | ((bits >> 13) & 1023));
}
double pq(double nits) {
  const double p = std::pow(nits / 10000., 0.1593017578125);
  return std::pow((0.8359375 + 18.8515625 * p) / (1. + 18.6875 * p), 78.84375);
}
double inversePQ(double p) {
  const double x = std::pow(p, 1. / 78.84375);
  return 10000. * std::pow(std::max(0., x - 0.8359375) / (18.8515625 - 18.6875 * x),
                           1. / 0.1593017578125);
}
double expectedPQ(double input, double content, double display, double scale) {
  const double black = pq(1.), span = pq(content) - black;
  const double maximum = (pq(display) - black) / span;
  const double minimum = (pq(0.) - black) / span;
  double e = (pq(std::clamp(input, 1., content)) - black) / span;
  const double shoulder = 1.5 * maximum - 0.5;
  if (shoulder < 1. && e >= shoulder) {
    const double t = (e - shoulder) / (1. - shoulder);
    e = (2 * t * t * t - 3 * t * t + 1) * shoulder
      + (t * t * t - 2 * t * t + t) * (1. - shoulder)
      + (-2 * t * t * t + 3 * t * t) * maximum;
  }
  e += minimum * std::pow(1. - e, 4.);
  return pq(inversePQ(e * span + black) * scale);
}
struct Window {
  HWND hwnd;
  Window() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
      if (message == WM_CLOSE) { PostQuitMessage(0); return 0; }
      return DefWindowProcW(window, message, wparam, lparam);
    };
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
    cls.lpszClassName = L"OwnedColorSpacePresenterWindow";
    require(RegisterClassW(&cls) != 0, "RegisterClassW");
    hwnd = CreateWindowExW(0, cls.lpszClassName, L"color-space GPU presenter", WS_OVERLAPPEDWINDOW,
                           90, 80, 320, 192, nullptr, nullptr, cls.hInstance, nullptr);
    require(hwnd != nullptr, "CreateWindowExW");
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
  }
  ~Window() { DestroyWindow(hwnd); }
};
struct Probe {
  WMT::Reference<WMT::Array<WMT::Device>> devices;
  WMT::Device device;
  WMT::Reference<WMT::Error> compiler_error;
  WMT::Reference<WMT4::Compiler> compiler;
  WMT::Reference<WMT::MTL4CommandQueue> queue;
  obj_handle_t view = 0;
  obj_handle_t target = 0;
  bool composition = false;
  WMT::MetalLayer layer;
  dxmt::InternalCommandLibrary library;
  dxmt::Presenter presenter;
  WMT::Reference<WMT::Texture> input;

  static WMT::Device firstDevice(WMT::Array<WMT::Device> d) {
    require(d && d.count() > 0, "Metal device");
    return d.object(0);
  }
  static WMT::MetalLayer makeLayer(HWND hwnd, WMT::Device device, obj_handle_t &view, bool composition) {
    obj_handle_t layer = 0;
    view = composition ? WMTCompositionCreateSurface(device.handle, true, &layer) :
        CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(hwnd), device.handle, &layer);
    require(view && layer, "CreateMetalViewFromHWND");
    return WMT::MetalLayer{layer};
  }
  Probe(HWND hwnd, bool ownedComposition = false, bool attachComposition = false)
      : devices(WMT::CopyAllDevices()), device(firstDevice(devices)),
      compiler(WMT4::Compiler::Create(device, compiler_error)),
      queue(device.newMTL4CommandQueue()), composition(ownedComposition),
      layer(makeLayer(hwnd, device, view, composition)),
      library(device), presenter(device, layer, library, 1.f, 1, compiler, composition) {
    require(compiler, "Metal4 compiler");
    require(queue, "command queue");
    if (attachComposition) {
      target = WMTCompositionCreateTarget(reinterpret_cast<uint64_t>(hwnd), true);
      WMTCompositionTargetState state{target, view, 0, 0};
      require(target && WMTCompositionCommit(1, &state) == 0, "attach paired composition surface");
    }
    for (auto color : {WMTColorSpaceSRGB, WMTColorSpaceSRGBLinear, WMTColorSpaceHDR_scRGB,
                       WMTColorSpaceHDR_PQ, WMTColorSpaceBT2020})
      require(CGColorSpace_checkColorSpaceSupported(color), "CoreGraphics color-space support");
    presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, WMTColorSpaceSRGB, kSize, kSize, 1);
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
    const auto mid = toHalf(static_cast<float>(pq(1400.)));
    const auto high = toHalf(static_cast<float>(pq(1600.)));
    for (unsigned y = 0; y < kSize; ++y)
      for (unsigned x = 0; x < kSize; ++x) {
        auto *pixel = pixels + (y * kSize + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = x < kSize / 3 ? 0 : x < kSize * 2 / 3 ? mid : high;
        pixel[3] = toHalf(1.f);
      }
    input.replaceRegion({0, 0, 0}, {kSize, kSize, 1}, 0, 0, pixels,
                        kSize * 4 * sizeof(unsigned short), sizeof(pixels));
    uint64_t version = ~uint64_t(0);
    WMTColorSpace color = WMTColorSpaceInvalid;
    WMTHDRMetadata metadata{};
    WMTEDRValue edr{};
    WMTQueryDisplaySettingForLayer(layer.handle, &version, &color, &metadata, &edr);
    require(version == 0, "external display override must not replace selected color space");
  }
  ~Probe() {
    if (target) NSObject_release(target);
    if (composition) NSObject_release(view); else ReleaseMetalView(view);
  }
  struct Sample {
    float black, mid, high;
    dxmt::DXMTPresentMetadata metadata;
  };
  Sample render(dxmt::Presenter::PresentState state, const char *label, unsigned size = kSize,
                const double (*expected)[4] = nullptr) {
    step = label;
    const unsigned row = size * 4 * sizeof(unsigned short);
    WMTBufferInfo readback{};
    readback.length = row * size;
    readback.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
    auto buffer = device.newBuffer(readback);
    require(buffer && readback.memory.get(), "shared readback buffer");
    WMTBufferInfo residualReadback{};
    residualReadback.length = row * size;
    residualReadback.options = readback.options;
    auto residualBuffer = composition ? device.newBuffer(residualReadback) : WMT::Reference<WMT::Buffer>{};
    auto allocator = device.newMTL4CommandAllocator();
    auto cmd = device.newMTL4CommandBuffer();
    require(cmd, "command buffer");
    require(allocator, "Metal4 command allocator");
    cmd.begin(allocator);
    auto drawable = presenter.encodeCommandsMetal4(cmd, input, state.metadata, [](auto) {}, [](auto) {});
    require(drawable && drawable.texture() && drawable.texture().width() == size &&
            drawable.texture().height() == size, "drawable dimensions");
    wmtcmd_blit_copy_from_texture_to_buffer copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.src = drawable.texture().handle;
    copy.size = {size, size, 1};
    copy.dst = buffer.handle;
    copy.bytes_per_row = row;
    copy.bytes_per_image = row * size;
    auto blit = cmd.blitCommandEncoder();
    blit.barrierAfterQueueStages(WMTMetal4StageFragment, WMTMetal4StageBlit, WMTMetal4VisibilityDevice);
    blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    if (composition) {
      copy.src = WMTCompositionDrawableResidualTexture(drawable.handle);
      copy.dst = residualBuffer.handle;
      require(copy.src && residualBuffer && residualReadback.memory.get(), "paired float readback");
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    }
    blit.endEncoding();
    cmd.end();
    queue.waitForDrawable(drawable);
    auto submission = queue.commit(&cmd, 1);
    require(submission, "Metal4 submission");
    queue.signalDrawable(drawable);
    WMT4::PresentMetal4Drawable(drawable, 0, nullptr, 0, false);
    submission.waitUntilCompleted();
    WMTMetal4SubmissionInfo completion{};
    submission.getInfo(completion);
    require(completion.completed && !completion.error_handle, "GPU completed");
    presenter.notifyPresentCompleted();
    const auto *bytes = static_cast<const unsigned short *>(readback.memory.get());
    if (expected) {
      const auto *residual = static_cast<const unsigned short *>(residualReadback.memory.get());
      for (unsigned i = 0; i != 8; ++i) {
        const unsigned offset = ((size / 2) * size + i * 8 + 4) * 4;
        const double alpha = expected[i][3];
        checkNear(half(bytes[offset + 3]), alpha, 0.001, "composition coverage");
        checkNear(half(residual[offset + 3]), 1, 0.001, "opaque additive plane");
        for (unsigned c = 0; c != 3; ++c) {
          const double base = std::clamp(expected[i][c], 0., alpha);
          const double energy = expected[i][c] - base;
          const double tolerance = 0.003 + std::fabs(expected[i][c]) * 0.002;
          checkNear(half(bytes[offset + c]), base, tolerance, "signed base domain");
          checkNear(half(residual[offset + c]), energy, tolerance, "extended additive domain");
          checkNear(half(bytes[offset + c]) + half(residual[offset + c]), expected[i][c],
                    tolerance, "linear reconstructed energy");
        }
      }
    }
    auto sample = [&](unsigned x) { return half(bytes[((size / 2) * size + x) * 4]); };
    Sample result{sample(size / 8), sample(size / 2), sample(size * 7 / 8), state.metadata};
    std::printf("OWNED_D3D12_COLORSPACE_PRESENTER_FRAME frame=%s black=%f mid=%f high=%f content=%f display=%f scale=%f\n",
                label, result.black, result.mid, result.high, result.metadata.max_content_luminance,
                result.metadata.max_display_luminance, result.metadata.edr_scale);
    require(std::isfinite(result.black) && std::isfinite(result.mid) && std::isfinite(result.high),
            "finite drawable GPU pixels");
    return result;
  }
};
void identity(const Probe::Sample &s, const char *why) {
  checkNear(s.black, 0., 0.01, why);
  checkNear(s.mid, half(toHalf(static_cast<float>(pq(1400.)))), 0.008, why);
  checkNear(s.high, half(toHalf(static_cast<float>(pq(1600.)))), 0.008, why);
}
void scaled(const Probe::Sample &s, const char *why) {
  checkNear(s.black, 0., 0.01, why);
  checkNear(s.mid, half(toHalf(static_cast<float>(pq(1400.)))) * s.metadata.edr_scale, 0.008, why);
  checkNear(s.high, half(toHalf(static_cast<float>(pq(1600.)))) * s.metadata.edr_scale, 0.008, why);
}
void pqMapped(const Probe::Sample &s, const char *why) {
  checkNear(s.black, 0., 0.01, why);
  checkNear(s.mid, expectedPQ(1400., s.metadata.max_content_luminance,
                              s.metadata.max_display_luminance, s.metadata.edr_scale), 0.008, why);
  checkNear(s.high, expectedPQ(1600., s.metadata.max_content_luminance,
                               s.metadata.max_display_luminance, s.metadata.edr_scale), 0.008, why);
}
} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto pool = WMT::MakeAutoreleasePool();
  Window window;
  const bool visual = argc > 1 && std::strcmp(argv[1], "--composition-visual") == 0;
  if (visual || (argc > 1 && std::strcmp(argv[1], "--composition") == 0)) {
    Probe composition(window.hwnd, true, visual);
    // Exact finite FP16 inputs cover alpha zero/additive, smallest normal
    // alpha, half coverage, opaque, negative and super-white linear energy.
    const float pixels[8][4] = {{0, 0, 0, 0}, {.25f, -.125f, 2, 0},
        {0.00006103515625f, 0, 0, 0.00006103515625f}, {.25f, .125f, .5f, .5f},
        {-.5f, 2, 4, 1}, {2, -.25f, .125f, .5f}, {1, 1, 1, 1}, {.125f, .25f, .5f, 1}};
    for (auto space : {WMTColorSpaceHDR_scRGB, WMTColorSpaceSRGB, WMTColorSpaceHDR_PQ, WMTColorSpaceBT2020}) {
      unsigned short upload[kSize * kSize * 4]{};
      double expected[8][4]{};
      for (unsigned i = 0; i != 8; ++i) {
        double linear[3]{};
        const double alpha = pixels[i][3];
        expected[i][3] = alpha;
        for (unsigned c = 0; c != 3; ++c) {
          // PQ has a bounded encoded source domain, unlike signed scRGB.
          const float source = space == WMTColorSpaceHDR_PQ
              ? static_cast<float>(pq((i + 1) * (c + 1) * 100.) * (alpha > 0 ? alpha : 1))
              : space == WMTColorSpaceHDR_scRGB ? pixels[i][c] :
                static_cast<float>(std::clamp(pixels[i][c] / (alpha > 0 ? alpha : 1), 0., 1.) *
                                   (alpha > 0 ? alpha : 1));
          const auto encoded = toHalf(source);
          const double stored = half(encoded);
          double straight = alpha > 0 ? stored / alpha : stored;
          linear[c] = space == WMTColorSpaceHDR_scRGB ? straight * .8 :
              space == WMTColorSpaceHDR_PQ ? inversePQ(std::clamp(straight, 0., 1.)) / 100. :
              straight <= .04045 ? straight / 12.92 : std::pow((straight + .055) / 1.055, 2.4);
          for (unsigned y = 0; y != kSize; ++y)
            for (unsigned x = i * 8; x != i * 8 + 8; ++x)
              upload[(y * kSize + x) * 4 + c] = encoded;
        }
        if (space == WMTColorSpaceHDR_PQ || space == WMTColorSpaceBT2020) {
          // Public D65 RGB primary transform, independent double scalar oracle.
          expected[i][0] = 1.660491 * linear[0] - .587641 * linear[1] - .072850 * linear[2];
          expected[i][1] = -.124550 * linear[0] + 1.132900 * linear[1] - .008349 * linear[2];
          expected[i][2] = -.018151 * linear[0] - .100579 * linear[1] + 1.118730 * linear[2];
        } else {
          for (unsigned c = 0; c != 3; ++c) expected[i][c] = linear[c];
        }
        for (unsigned c = 0; c != 3; ++c) if (alpha > 0) expected[i][c] *= alpha;
        for (unsigned y = 0; y != kSize; ++y)
          for (unsigned x = i * 8; x != i * 8 + 8; ++x)
            upload[(y * kSize + x) * 4 + 3] = toHalf(pixels[i][3]);
      }
      composition.input.replaceRegion({0, 0, 0}, {kSize, kSize, 1}, 0, 0, upload,
                                     kSize * 8, sizeof(upload));
      composition.presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, space, kSize, kSize, 1,
                                                 dxmt::DXMTPresentAlpha::Premultiplied);
      auto state = composition.presenter.synchronizeLayerProperties(true);
      WMTLayerProps props{}; composition.layer.getProps(props);
      require(props.pixel_format == WMTPixelFormatRGBA16Float && !props.opaque, "transparent FP16 layer");
      composition.render(std::move(state), "composition-transfer", kSize, expected);
      if (visual && space == WMTColorSpaceHDR_scRGB) {
        SetWindowTextW(window.hwnd, L"Owned FP16 Composition - signed scRGB on gray - close to finish");
        std::puts("OWNED_D3D12_COMPOSITION_COLOR_VISUAL_READY space=scRGB negative_green_strip=1 opaque_negative_red_strip=4 close_to_advance=1");
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          TranslateMessage(&message); DispatchMessageW(&message);
        }
      }
    }
    std::puts("OWNED_D3D12_COMPOSITION_COLOR_PASS real_gpu=1 paired_drawable_readback=1 spaces=4 boundary_alpha=1 signed_extended=1 compositor_proof=0");
    return 0;
  }
  Probe probe(window.hwnd);
  WMTHDRMetadata hdr{};
  hdr.max_mastering_luminance = 3000;
  hdr.max_content_light_level = 3000;
  hdr.max_frame_average_light_level = 400;
  probe.presenter.changeHDRMetadata(&hdr);
  identity(probe.render(probe.presenter.synchronizeLayerProperties(true), "sdr"), "SDR identity");

  auto acceptedSDR = probe.presenter.synchronizeLayerProperties(true);
  require(probe.presenter.changeLayerColorSpace(WMTColorSpaceHDR_PQ), "SDR to PQ");
  identity(probe.render(std::move(acceptedSDR), "accepted-sdr-after-pq-selection"), "accepted SDR retained");
  auto pqFrame = probe.render(probe.presenter.synchronizeLayerProperties(true), "pq");
  pqMapped(pqFrame, "PQ EETF oracle");
  require(std::fabs(pqFrame.mid - half(toHalf(static_cast<float>(pq(1400.))))) > 0.03f,
          "PQ and SDR distinguishable on GPU");

  auto acceptedPQ = probe.presenter.synchronizeLayerProperties(true);
  require(probe.presenter.changeLayerColorSpace(WMTColorSpaceHDR_scRGB), "PQ to scRGB");
  probe.presenter.changeHDRMetadata(nullptr);
  pqMapped(probe.render(std::move(acceptedPQ), "accepted-pq-after-scrgb-selection"), "accepted PQ retained");
  auto scrgb = probe.render(probe.presenter.synchronizeLayerProperties(true), "scrgb");
  scaled(scrgb, "linear scRGB scale oracle");
  require(std::fabs(scrgb.mid - half(toHalf(static_cast<float>(pq(1400.))))) > 0.03f,
          "scRGB and SDR distinguishable on GPU");

  auto acceptedScRGB = probe.presenter.synchronizeLayerProperties(true);
  require(probe.presenter.changeLayerColorSpace(WMTColorSpaceSRGBLinear), "scRGB to linear SDR");
  scaled(probe.render(std::move(acceptedScRGB), "accepted-scrgb-after-linear-selection"),
         "accepted scRGB retained");
  identity(probe.render(probe.presenter.synchronizeLayerProperties(true), "linear-sdr"), "linear SDR identity");

  auto acceptedLinear = probe.presenter.synchronizeLayerProperties(true);
  require(probe.presenter.changeLayerColorSpace(WMTColorSpaceBT2020), "linear SDR to BT2020");
  identity(probe.render(std::move(acceptedLinear), "accepted-linear-after-bt2020-selection"),
           "accepted linear retained");
  // BT.2020/sRGB-gamma differs at Core Animation composition, not in this Metal shader.
  identity(probe.render(probe.presenter.synchronizeLayerProperties(true), "bt2020"), "BT2020 shader identity");

  probe.presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, WMTColorSpaceBT2020,
                                        kResized, kResized, 1);
  identity(probe.render(probe.presenter.synchronizeLayerProperties(true), "bt2020-resized", kResized),
           "BT2020 shader identity after resize");

  // CPU encoding finishes while this command is held at a real Metal GPU
  // event. Color-space transition must await GPU retirement, not just encode.
  auto gate = probe.device.newSharedEvent();
  auto allocator = probe.device.newMTL4CommandAllocator();
  auto command = probe.device.newMTL4CommandBuffer();
  require(gate && command && allocator, "GPU retirement gate");
  command.begin(allocator);
  probe.queue.waitForEvent(gate, 1);
  WMT::MetalDrawable gatedDrawable;
  WMT::Reference<WMT::MTL4Submission> submission;
  {
    auto accepted = probe.presenter.synchronizeLayerProperties(true);
    gatedDrawable = probe.presenter.encodeCommandsMetal4(command, probe.input, accepted.metadata,
                                                   [](auto) {}, [](auto) {});
    require(gatedDrawable && gatedDrawable.texture(), "gated drawable");
    command.end();
    probe.queue.waitForDrawable(gatedDrawable);
    submission = probe.queue.commit(&command, 1);
    require(submission, "gated Metal4 submission");
    probe.queue.signalDrawable(gatedDrawable);
    WMT4::PresentMetal4Drawable(gatedDrawable, 0, nullptr, 0, false);
  } // CPU-encoded PresentState is released, but its native command remains gated.
  require(probe.presenter.changeLayerColorSpace(WMTColorSpaceHDR_PQ), "gated BT2020 to PQ");
  probe.presenter.changeHDRMetadata(&hdr);
  std::atomic<bool> entered = false;
  auto future = std::async(std::launch::async, [&] {
    auto pool = WMT::MakeAutoreleasePool();
    entered.store(true, std::memory_order_release);
    entered.notify_one();
    return probe.presenter.synchronizeLayerProperties(true);
  });
  entered.wait(false, std::memory_order_acquire);
  const bool heldUntilGpuRetires =
      future.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout;
  gate.signalValue(1);
  submission.waitUntilCompleted();
  WMTMetal4SubmissionInfo completion{};
  submission.getInfo(completion);
  const bool completed = completion.completed && !completion.error_handle;
  probe.presenter.notifyPresentCompleted();
  auto afterRetirement = future.get();
  require(completed, "gated GPU command completed");
  require(heldUntilGpuRetires, "new color waited for GPU completion, not just CPU encoding");
  pqMapped(probe.render(std::move(afterRetirement), "pq-after-gpu-retirement", kResized),
           "PQ after GPU retirement oracle");
  std::puts("OWNED_D3D12_COLORSPACE_PRESENTER_PASS real_gpu=1 drawable_readback=1 frames=11 retirement_gate=1 compositor_proof=0");
  return 0;
}
