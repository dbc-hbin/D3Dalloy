// SPDX-License-Identifier: LGPL-2.1-or-later
// Build as a PE32/PE64 executable against the corresponding winemetal.dll.a.
// Exercises actual native consumers, not rendered-pixel or presentation acceptance.
#include <windows.h>
#include "../../src/winemetal/Metal4FX.hpp"
#include <cstdio>

int main() {
  const uint64_t base = WMTGetGuestWindowBase();
#if defined(__i386__)
  if (!base || (base & 0xffffffffull)) return 1;
#endif
  std::printf("{\"guest_entry\":true,\"pointer_bits\":%zu,\"guest_base\":\"%llx\"}\n", sizeof(void *) * 8,
              static_cast<unsigned long long>(base));
  std::fflush(stdout);
  for (const char *name : {static_cast<const char *>(nullptr), "winemetal.dll"}) {
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(GetModuleHandleA(name), path, MAX_PATH);
    if (!length || length == MAX_PATH) return 9;
    std::printf("FEX_PROBE_IMAGE:%s\n", path);
  }
  WMT::Reference<WMT::Object> pool(NSAutoreleasePool_alloc_init());
  WMT::Reference<WMT::Array<WMT::Device>> devices(WMTCopyAllDevices());
  if (!devices || !devices.count()) return 2;
  auto device = devices.object(0);
  WMT::Error error;
  auto compiler = WMT4::Compiler::Create(device, error);
  if (!compiler || error || !WMT4::TemporalFeature::Supports(device)) return 3;
  WMTMetal4FXTemporalInfo create{16, 16, 32, 32, 2, 0, 0};
  int32_t result = -1;
  auto feature = WMT4::TemporalFeature::Create(device, compiler, create, result);
  if (!feature || result) return 4;
  WMTMetal4FXTemporalFrame input{};
  uint64_t generation = 0;
  auto rejected = feature.prepare(input, result, generation);
  if (rejected || uint32_t(result) != 0x80070057u) return 5;
  WMT::Reference<WMT::Texture> textures[4];
  const WMTPixelFormat formats[] = {WMTPixelFormatRGBA16Float, WMTPixelFormatR32Float,
                                   WMTPixelFormatRG16Float, WMTPixelFormatRGBA16Float};
  for (unsigned i = 0; i < 4; ++i) {
    WMTTextureInfo info{};
    info.pixel_format = formats[i];
    info.width = info.height = i == 3 ? 32 : 16;
    info.depth = info.array_length = info.mipmap_level_count = info.sample_count = 1;
    info.type = WMTTextureType2D;
    info.usage = WMTTextureUsage(WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite | WMTTextureUsageRenderTarget);
    info.options = WMTResourceStorageModePrivate;
    textures[i] = device.newTexture(info);
    if (!textures[i]) return 6;
  }
  input.color = textures[0].handle;
  input.depth = textures[1].handle;
  input.motion = textures[2].handle;
  input.output = textures[3].handle;
  input.input_width = input.input_height = 16;
  input.color_rect = input.depth_rect = input.motion_rect = {0, 0, 16, 16};
  input.output_rect = {0, 0, 32, 32};
  input.motion_scale_x = input.motion_scale_y = input.pre_exposure = 1;
  input.reset = 1;
  auto frame = feature.prepare(input, result, generation);
  if (!frame || result || !generation) return 7;
  obj_handle_t layer = 0;
  WMT::Reference<WMT::Object> surface(WMTCompositionCreateSurface(device.handle, 1, &layer));
  if (!surface || !layer) return 8;
  WMTCompositionFrame composition{surface.handle, {1, 0, 0, 1, 0, 0}, 0, 0};
  WMT::MetalDrawable no_drawable;
  WMT4::PresentMetal4Drawable(no_drawable, 0, nullptr, 0, false);
  WMT4::PresentMetal4Drawable(no_drawable, 0, &composition, 0, false);
  std::printf("{\"metal4fx_pointers\":\"PASS\",\"cases\":5,\"generation\":%llu}\n",
              static_cast<unsigned long long>(generation));
}
