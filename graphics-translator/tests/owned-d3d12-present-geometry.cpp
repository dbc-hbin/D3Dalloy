// Native drawable oracle for D3D12 presentation geometry. Exercises the production
// Presenter/Metal shader and reads the drawable before Core Animation composition.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dxmt_presenter.hpp"
#include "Metal.hpp"
#include "log/log.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dxmt { Logger Logger::s_instance("owned-d3d12-present-geometry.log"); }

namespace {
[[noreturn]] void fail(const char *step) {
  std::fprintf(stderr, "OWNED_D3D12_PRESENT_GEOMETRY_FAIL step=%s\n", step);
  std::exit(1);
}
void require(bool ok, const char *step) { if (!ok) fail(step); }
float half(unsigned short bits) {
  unsigned sign = bits >> 15, exponent = (bits >> 10) & 31, mantissa = bits & 1023;
  float value = exponent ? std::ldexp(float(mantissa + 1024), int(exponent) - 25)
                         : std::ldexp(float(mantissa), -24);
  return sign ? -value : value;
}
unsigned short toHalf(float value) {
  unsigned bits;
  std::memcpy(&bits, &value, 4);
  unsigned exponent = (bits >> 23) & 255;
  return value == 0 ? 0 : static_cast<unsigned short>(((bits >> 16) & 0x8000) |
      ((exponent - 112) << 10) | ((bits >> 13) & 1023));
}
void checkNear(float actual, float wanted, const char *step) {
  if (!std::isfinite(actual) || std::fabs(actual - wanted) > 0.002f) {
    std::fprintf(stderr, "OWNED_D3D12_PRESENT_GEOMETRY_PIXEL step=%s actual=%f wanted=%f\n",
                 step, actual, wanted);
    fail(step);
  }
}
struct Window {
  HWND hwnd;
  Window() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"OwnedD3D12PresentGeometryWindow";
    require(RegisterClassW(&cls) != 0, "register-window");
    hwnd = CreateWindowExW(0, cls.lpszClassName, L"geometry drawable oracle",
                           WS_OVERLAPPEDWINDOW, 60, 60, 120, 100, nullptr, nullptr, cls.hInstance, nullptr);
    require(hwnd != nullptr, "create-window");
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
  }
  ~Window() { DestroyWindow(hwnd); }
};
struct Probe {
  WMT::Reference<WMT::Array<WMT::Device>> devices;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> queue;
  obj_handle_t view = 0;
  WMT::MetalLayer layer;
  dxmt::InternalCommandLibrary library;
  dxmt::Presenter presenter;
  WMT::Reference<WMT::Texture> input;
  static WMT::Device first(WMT::Array<WMT::Device> devices) {
    require(devices && devices.count(), "device");
    return devices.object(0);
  }
  static WMT::MetalLayer makeLayer(HWND hwnd, WMT::Device device, obj_handle_t &view) {
    obj_handle_t layer = 0;
    view = CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(hwnd), device.handle, &layer);
    require(view && layer, "metal-view");
    return WMT::MetalLayer{layer};
  }
  Probe(HWND hwnd) : devices(WMT::CopyAllDevices()), device(first(devices)),
      queue(device.newCommandQueue(8)), layer(makeLayer(hwnd, device, view)),
      library(device), presenter(device, layer, library, 1.f, 1) {
    require(queue, "queue");
    presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, WMTColorSpaceSRGBLinear, 8, 6, 1);
    WMTTextureInfo info{};
    info.type = WMTTextureType2D;
    info.pixel_format = WMTPixelFormatRGBA16Float;
    info.usage = WMTTextureUsageShaderRead;
    info.options = WMTResourceStorageModeShared;
    info.width = 8; info.height = 6; info.depth = 1;
    info.mipmap_level_count = info.sample_count = info.array_length = 1;
    input = device.newTexture(info);
    require(input, "input-texture");
    unsigned short pixels[8 * 6 * 4]{};
    for (unsigned y = 0; y < 6; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        auto *pixel = pixels + (y * 8 + x) * 4;
        pixel[0] = toHalf(x / 8.f);
        pixel[1] = toHalf(y / 8.f);
        pixel[2] = toHalf(.25f);
        pixel[3] = toHalf(1.f);
      }
    input.replaceRegion({0, 0, 0}, {8, 6, 1}, 0, 0, pixels, 8 * 4 * sizeof(unsigned short), sizeof(pixels));
  }
  ~Probe() { ReleaseMetalView(view); }
  struct Pixel { float r, g, b, a; };
  Pixel render(dxmt::DXMTPresentGeometry geometry, unsigned width, unsigned height,
               unsigned x, unsigned y, const char *label) {
    auto state = presenter.synchronizeLayerProperties(true);
    state.metadata.geometry = geometry;
    WMTBufferInfo info{};
    info.length = width * height * 4 * sizeof(unsigned short);
    info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
    auto buffer = device.newBuffer(info);
    require(buffer && info.memory.get(), "readback-buffer");
    auto command = queue.commandBuffer();
    require(command, "command-buffer");
    auto drawable = presenter.encodeCommands(command, input, state.metadata, [](auto) {}, [](auto) {});
    require(drawable && drawable.texture() && drawable.texture().width() == width &&
            drawable.texture().height() == height, "drawable-size");
    wmtcmd_blit_copy_from_texture_to_buffer copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.src = drawable.texture().handle;
    copy.size = {width, height, 1};
    copy.dst = buffer.handle;
    copy.bytes_per_row = width * 4 * sizeof(unsigned short);
    copy.bytes_per_image = info.length;
    auto blit = command.blitCommandEncoder();
    blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    blit.endEncoding();
    command.presentDrawable(drawable);
    command.commit();
    command.waitUntilCompleted();
    require(command.status() == WMTCommandBufferStatusCompleted, "GPU-complete");
    presenter.notifyPresentCompleted();
    const auto *pixels = static_cast<const unsigned short *>(info.memory.get()) + (y * width + x) * 4;
    Pixel result{half(pixels[0]), half(pixels[1]), half(pixels[2]), half(pixels[3])};
    std::printf("OWNED_D3D12_PRESENT_GEOMETRY_FRAME label=%s x=%u y=%u rgba=%f,%f,%f,%f\n",
                label, x, y, result.r, result.g, result.b, result.a);
    return result;
  }
};
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto pool = WMT::MakeAutoreleasePool();
  Window window;
  Probe probe(window.hwnd);
  dxmt::DXMTPresentGeometry g{};
  auto defaultPixel = probe.render({}, 8, 6, 3, 2, "legacy-identity");
  checkNear(defaultPixel.r, 3.f / 8, "legacy-red");
  checkNear(defaultPixel.g, 2.f / 8, "legacy-green");

  g.source_width = 4; g.source_height = 3;
  g.destination_width = 8; g.destination_height = 6;
  auto cropped = probe.render(g, 8, 6, 6, 4, "source-crop");
  checkNear(cropped.r, 3.f / 8, "crop-red");
  checkNear(cropped.g, 2.f / 8, "crop-green");

  g.source_width = 8; g.source_height = 6;
  g.rotation = 2; // DXGI_MODE_ROTATION_ROTATE90
  auto rotated = probe.render(g, 8, 6, 1, 1, "rotate90");
  checkNear(rotated.r, 2.f / 8, "rotate90-red");
  checkNear(rotated.g, 4.f / 8, "rotate90-green");

  g.rotation = 3; // DXGI_MODE_ROTATION_ROTATE180
  auto upsideDown = probe.render(g, 8, 6, 1, 1, "rotate180");
  checkNear(upsideDown.r, 6.f / 8, "rotate180-red");
  checkNear(upsideDown.g, 4.f / 8, "rotate180-green");
  g.rotation = 4; // DXGI_MODE_ROTATION_ROTATE270
  auto counterclockwise = probe.render(g, 8, 6, 1, 1, "rotate270");
  checkNear(counterclockwise.r, 6.f / 8, "rotate270-red");
  checkNear(counterclockwise.g, 1.f / 8, "rotate270-green");

  probe.presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, WMTColorSpaceSRGBLinear, 10, 8, 1);
  g.rotation = 1;
  g.destination_width = 10; g.destination_height = 8;
  g.scaling_none = 1;
  g.background[0] = .125f; g.background[1] = .375f;
  g.background[2] = .625f; g.background[3] = 1.f;
  auto padding = probe.render(g, 10, 8, 9, 7, "background-padding");
  checkNear(padding.r, .125f, "padding-red");
  checkNear(padding.g, .375f, "padding-green");
  checkNear(padding.b, .625f, "padding-blue");
  auto image = probe.render(g, 10, 8, 7, 5, "unscaled-edge");
  checkNear(image.r, 7.f / 8, "unscaled-red");
  checkNear(image.g, 5.f / 8, "unscaled-green");

  g.right_to_left = 1;
  auto left = probe.render(g, 10, 8, 0, 1, "rtl-padding");
  checkNear(left.r, .125f, "rtl-padding-red");
  auto right = probe.render(g, 10, 8, 2, 1, "rtl-image");
  checkNear(right.r, 0.f, "rtl-first-red");
  checkNear(right.g, 1.f / 8, "rtl-first-green");
  probe.presenter.changeLayerProperties(WMTPixelFormatRGBA16Float, WMTColorSpaceSRGBLinear, 6, 6, 1);
  g.destination_width = 6; g.destination_height = 6;
  auto clippedRight = probe.render(g, 6, 6, 0, 2, "rtl-overflow-right-aligned");
  checkNear(clippedRight.r, 2.f / 8, "rtl-overflow-source-offset");
  g.restrict_output = 1;
  g.output_clip[0] = 2; g.output_clip[1] = 0;
  g.output_clip[2] = 4; g.output_clip[3] = 6;
  auto hidden = probe.render(g, 6, 6, 0, 2, "restricted-hidden");
  checkNear(hidden.r, 0.f, "restricted-hidden-red");
  checkNear(hidden.g, 0.f, "restricted-hidden-green");
  checkNear(hidden.b, 0.f, "restricted-hidden-blue");
  auto visible = probe.render(g, 6, 6, 2, 2, "restricted-visible");
  checkNear(visible.r, 4.f / 8, "restricted-visible-red");
  checkNear(visible.g, 2.f / 8, "restricted-visible-green");
  std::puts("OWNED_D3D12_PRESENT_GEOMETRY_PASS drawable_pixels=1 crop=1 rotations=3 background=1 rtl=1 restrict=1");
  return 0;
}
