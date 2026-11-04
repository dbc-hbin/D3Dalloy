// Reads every drawable texel before Core Animation and times the production
// Presenter. The same source builds against the before/after Presenter headers.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3dalloy_presenter.hpp"
#include "Metal4FX.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace d3dalloy { Logger Logger::s_instance("owned-d3d12-present-output.log"); }

namespace {
void require(bool ok, const char *reason) {
  if (!ok) {
    std::fprintf(stderr, "OWNED_D3D12_PRESENT_OUTPUT_FAIL reason=%s\n", reason);
    std::exit(1);
  }
}

template<typename P>
WMT::MetalDrawable encode(P &presenter, WMT::MTL4CommandBuffer command, WMT::Texture input,
                         d3dalloy::D3DalloyPresentMetadata metadata, bool &identity_copy, bool &pq_code_copy) {
  pq_code_copy = false;
  if constexpr (requires { typename P::PresentWork; }) {
    typename P::PresentWork work;
    auto drawable = presenter.encodeCommandsMetal4(command, input, metadata, {}, work);
    identity_copy = work.identity_copy;
    if constexpr (requires { work.pq_code_copy; }) pq_code_copy = work.pq_code_copy;
    return drawable;
  } else {
    identity_copy = false;
    return presenter.encodeCommandsMetal4(command, input, metadata, [](auto) {}, [](auto) {});
  }
}

void complete(WMT::MTL4CommandQueue queue, WMT::MTL4CommandBuffer command,
              WMTMetal4SubmissionInfo &result, WMT::MetalDrawable drawable = {}) {
  command.end();
  if (drawable) queue.waitForDrawable(drawable);
  auto submission = queue.commit(&command, 1);
  require(submission, "submit");
  if (drawable) {
    queue.signalDrawable(drawable);
    WMT4::PresentMetal4Drawable(drawable, 0, nullptr, 0, false);
  }
  submission.waitUntilCompleted();
  submission.getInfo(result);
  require(result.completed && !result.error_handle, "GPU retirement");
}

uint32_t pattern(unsigned x, unsigned y, bool ten_bit) {
  if (ten_bit)
    return ((x * 37 + y * 13) & 1023) | (((x * 11 + y * 23) & 1023) << 10) |
           (((x * 7 + y * 17) & 1023) << 20) | (((x + y) & 3) << 30);
  return ((x * 37 + y * 13) & 255) | (((x * 11 + y * 23) & 255) << 8) |
         (((x * 7 + y * 17) & 255) << 16) | (((x + y) & 255) << 24);
}

void exercise(WMT::Device device, WMT::MTL4CommandQueue queue, d3dalloy::Presenter &presenter,
              unsigned width, unsigned height, WMTPixelFormat format, bool rotate, unsigned iterations,
              bool crop = false, bool no_scale = false, unsigned clip = 0, bool pq = false) {
  const uint64_t bytes = uint64_t(width) * height * 4;
  const unsigned input_width = width + (crop ? 64 : 0), input_height = height + (crop ? 17 : 0);
  const uint64_t input_bytes = uint64_t(input_width) * input_height * 4;
  WMTTextureInfo texture{};
  texture.type = WMTTextureType2D;
  texture.pixel_format = format;
  texture.usage = WMTTextureUsageShaderRead;
  texture.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
  texture.width = input_width; texture.height = input_height; texture.depth = 1;
  texture.mipmap_level_count = texture.sample_count = texture.array_length = 1;
  auto input = device.newTexture(texture);
  WMTBufferInfo upload_info{}, readback_info{};
  upload_info.length = input_bytes;
  readback_info.length = bytes;
  upload_info.options = readback_info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
  auto upload = device.newBuffer(upload_info), readback = device.newBuffer(readback_info);
  require(input && upload && readback && upload_info.memory.get() && readback_info.memory.get(), "resources");
  auto *source = static_cast<uint32_t *>(upload_info.memory.get());
  const bool ten_bit = format == WMTPixelFormatRGB10A2Unorm;
  for (unsigned y = 0; y < input_height; ++y)
    for (unsigned x = 0; x < input_width; ++x) source[y * input_width + x] = pattern(x, y, ten_bit);
  {
    auto allocator = device.newMTL4CommandAllocator();
    auto command = device.newMTL4CommandBuffer();
    require(allocator && command, "upload command");
    command.begin(allocator);
    wmtcmd_blit_copy_from_buffer_to_texture copy{};
    copy.type = WMTBlitCommandCopyFromBufferToTexture;
    copy.src = upload;
    copy.bytes_per_row = input_width * 4;
    copy.bytes_per_image = input_bytes;
    copy.size = {input_width, input_height, 1};
    copy.dst = input;
    auto blit = command.blitCommandEncoder();
    blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    blit.endEncoding();
    WMTMetal4SubmissionInfo result{};
    complete(queue, command, result);
  }
  presenter.changeLayerProperties(format, pq ? WMTColorSpaceHDR_PQ : WMTColorSpaceSRGB, width, height, 1);
  std::vector<uint64_t> times;
  unsigned copies = 0, pq_copies = 0, checked = 0;
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    auto pool = WMT::MakeAutoreleasePool();
    auto state = presenter.synchronizeLayerProperties(true);
    // Isolate a unit-gain frame; the window's live EDR headroom may vary.
    if (pq) state.metadata.edr_scale = 1.f;
    state.metadata.geometry.source_width = width;
    state.metadata.geometry.source_height = height;
    state.metadata.geometry.destination_width = width;
    state.metadata.geometry.destination_height = height;
    state.metadata.geometry.rotation = rotate ? 3 : 1;
    state.metadata.geometry.scaling_none = state.metadata.geometry.right_to_left = no_scale;
    if (clip) {
      state.metadata.geometry.restrict_output = 1;
      state.metadata.geometry.output_clip[2] = float(width - (clip == 2));
      state.metadata.geometry.output_clip[3] = float(height);
    }
    auto allocator = device.newMTL4CommandAllocator();
    auto command = device.newMTL4CommandBuffer();
    require(allocator && command, "present command");
    command.begin(allocator);
    bool identity_copy = false, pq_code_copy = false;
    auto drawable = encode(presenter, command, input, state.metadata, identity_copy, pq_code_copy);
    require(drawable && drawable.texture().width() == width && drawable.texture().height() == height,
            "drawable dimensions");
    require((!rotate && clip != 2) || !identity_copy, "rotation/partial clip must render");
    copies += identity_copy;
    pq_copies += pq_code_copy;
    const bool verify = iteration == 0 || iteration + 1 == iterations;
    if (verify) {
      wmtcmd_blit_copy_from_texture_to_buffer copy{};
      copy.type = WMTBlitCommandCopyFromTextureToBuffer;
      copy.src = drawable.texture();
      copy.size = {width, height, 1};
      copy.dst = readback;
      copy.bytes_per_row = width * 4;
      copy.bytes_per_image = bytes;
      auto blit = command.blitCommandEncoder();
      blit.barrierAfterQueueStages(WMTMetal4StageFragment | WMTMetal4StageBlit, WMTMetal4StageBlit,
                                  WMTMetal4VisibilityDevice);
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      blit.endEncoding();
    }
    WMTMetal4SubmissionInfo result{};
    complete(queue, command, result, drawable);
    presenter.notifyPresentCompleted();
    if (verify) {
      const auto *actual = static_cast<const uint32_t *>(readback_info.memory.get());
      for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
          const uint32_t expected = clip == 2 && x == width - 1 ? 0 :
              pattern(rotate ? width - x - 1 : x, rotate ? height - y - 1 : y, ten_bit);
          if (actual[y * width + x] != expected) {
            std::fprintf(stderr, "OUTPUT_PIXEL format=%u x=%u y=%u expected=%08x actual=%08x\n",
                         unsigned(format), x, y, expected, actual[y * width + x]);
            require(false, "drawable byte oracle");
          }
        }
      ++checked;
    } else if (iteration >= 5) {
      require(result.gpu_end_ns > result.gpu_start_ns, "native timestamps");
      times.push_back(result.gpu_end_ns - result.gpu_start_ns);
    }
  }
  std::sort(times.begin(), times.end());
  std::printf("OWNED_D3D12_PRESENT_OUTPUT_CASE width=%u height=%u format=%u rotate=%u frames=%u "
              "copy_frames=%u pq_copies=%u crop=%u none=%u clip=%u checked=%u exact=1 timed=%zu gpu_median_ns=%llu gpu_min_ns=%llu\n",
              width, height, unsigned(format), unsigned(rotate), iterations, copies, pq_copies, unsigned(crop), unsigned(no_scale), clip, checked, times.size(),
              static_cast<unsigned long long>(times.empty() ? 0 : times[times.size() / 2]),
              static_cast<unsigned long long>(times.empty() ? 0 : times.front()));
}
} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto pool = WMT::MakeAutoreleasePool();
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"OwnedPresentOutput";
  require(RegisterClassW(&cls), "register window");
  HWND window = CreateWindowExW(0, cls.lpszClassName, L"Present output oracle", WS_OVERLAPPEDWINDOW,
                                60, 60, 240, 180, nullptr, nullptr, cls.hInstance, nullptr);
  require(window, "window");
  ShowWindow(window, SW_SHOWNA); UpdateWindow(window);
  auto devices = WMT::CopyAllDevices();
  require(devices && devices.count(), "device inventory");
  auto device = devices.object(0);
  WMT::Reference<WMT::Error> error;
  auto compiler = WMT4::Compiler::Create(device, error);
  auto queue = device.newMTL4CommandQueue();
  obj_handle_t layer = 0;
  auto view = CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(window), device.handle, &layer);
  require(compiler && queue && view && layer, "native context");
  {
    d3dalloy::InternalCommandLibrary library(device);
    d3dalloy::Presenter presenter(device, WMT::MetalLayer{layer}, library, 1.f, 1, compiler);
    if (argc > 1 && !std::strcmp(argv[1], "--benchmark")) {
      for (auto size : {std::pair{2560u, 1440u}, std::pair{3840u, 2160u}})
        exercise(device, queue, presenter, size.first, size.second, WMTPixelFormatBGRA8Unorm, false, 31);
    } else if (argc > 1 && !std::strcmp(argv[1], "--unit-extent")) {
      for (auto format : {WMTPixelFormatRGBA8Unorm, WMTPixelFormatBGRA8Unorm, WMTPixelFormatRGB10A2Unorm})
        for (unsigned mode = 0; mode < 4; ++mode)
          exercise(device, queue, presenter, 64, 32, format, false, 2, mode != 0, mode != 1,
                   mode == 2 ? 1 : mode == 3 ? 2 : 0);
      exercise(device, queue, presenter, 1024, 4, WMTPixelFormatRGB10A2Unorm, false, 2, false, false, 0, true);
    } else if (argc > 1 && !std::strcmp(argv[1], "--unit-benchmark")) {
      exercise(device, queue, presenter, 2560, 1440, WMTPixelFormatBGRA8Unorm, false, 31, true, true, 1);
      exercise(device, queue, presenter, 2560, 1440, WMTPixelFormatRGB10A2Unorm, false, 31, false, false, 0, true);
    } else {
      for (auto format : {WMTPixelFormatRGBA8Unorm, WMTPixelFormatBGRA8Unorm, WMTPixelFormatRGB10A2Unorm})
        for (bool rotate : {false, true}) exercise(device, queue, presenter, 64, 32, format, rotate, 2);
    }
    presenter.waitPresentsCompleted();
  }
  ReleaseMetalView(view);
  DestroyWindow(window);
  std::puts("OWNED_D3D12_PRESENT_OUTPUT_PASS production_presenter=1 drawable_readback=1");
  return 0;
}
