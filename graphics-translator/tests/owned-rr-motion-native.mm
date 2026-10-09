// SPDX-License-Identifier: LGPL-2.1-or-later
// clang++ -std=c++20 -O2 -framework Foundation -framework Metal -framework MetalFX \
//   graphics-translator/tests/owned-rr-motion-native.mm -o build/owned-rr-motion-native
// Resource selection uses the production RR code and real native MetalFX usages.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define WINEMETAL_API
#include "../../src/winemetal/winemetal_metal4_fx.h"
#include "../../src/winemetal/unix/metalfx_rr.inc"

int main() { @autoreleasepool {
  auto frame = [DXMTRRFrame new];
  frame->core = std::make_shared<dxmt_rr::Core>();
  auto &core = *frame->core;
  core.device = MTLCreateSystemDefaultDevice();
  auto descriptor = [MTL4CompilerDescriptor new];
  core.compiler = [core.device newCompilerWithDescriptor:descriptor error:nil]; [descriptor release];
  if (!core.device || !core.compiler) return 1;
  auto &p = frame->props;
  p.input_width = p.input_height = 128; p.output_rect = {0,0,128,128};
  p.motion_scale_x = p.motion_scale_y = 1;
  core.info.feature_flags = dxmt_rr::Flag | 2 | 8;
  frame->generation = dxmt_rr::generation(core,p);
  if (!frame->generation) return 1;
  const MTLTextureUsage usage = frame->generation->scaler.motionTextureUsage | MTLTextureUsageShaderRead;
  unsigned cases = 0;
  for (unsigned variant = 0; variant < 9; ++variant) {
    core.info.feature_flags = dxmt_rr::Flag | 2 | 8;
    p.motion_scale_x = p.motion_scale_y = 1;
    frame->rects[2] = {0,0,128,128};
    p.output_rect = {0,0,128,128};
    if (variant == 1) p.motion_scale_x = 2;
    if (variant == 2) p.motion_scale_y = -.5f;
    if (variant == 3) core.info.feature_flags |= 4; // Jitter removal depends on execution history, not prepare order.
    if (variant == 4) { core.info.feature_flags &= ~2u; frame->rects[2] = p.output_rect = {0,0,256,256}; }
    if (variant == 6) frame->rects[2].x = 4;
    if (variant == 8) p.output_rect = {0,0,256,256}; // Upscaling still accepts input-resolution MV.
    frame->generation = dxmt_rr::generation(core,p);
    if (!frame->generation) return 1;
    const unsigned width = variant == 4 ? 256 : variant == 6 || variant == 7 ? 132 : 128;
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
      variant == 5 ? MTLPixelFormatRG32Float : MTLPixelFormatRG16Float
      width:width height:variant == 4 ? 256 : 128 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = usage;
    [frame->inputs[2] release]; frame->inputs[2] = [core.device newTextureWithDescriptor:d];
    if (!frame->inputs[2]) return 1;
    dxmt_rr::Resources resources;
    std::array<id<MTLTexture>,dxmt_rr::Count> bound{};
    const bool direct = variant == 0 || variant == 8;
    if (!dxmtRRResources(frame,resources,bound) || (bound[2] == frame->inputs[2]) != direct ||
        (resources.textures[2] == nil) != direct) {
      std::fprintf(stderr,"OWNED_RR_MOTION_FAIL case=%u expected_direct=%u\n",variant,unsigned(direct));
      return 1;
    }
    std::printf("OWNED_RR_MOTION_CASE case=%u direct=%u scratch=%u\n",variant,unsigned(direct),unsigned(!direct));
    ++cases;
  }
  [frame release];
  std::printf("OWNED_RR_MOTION_PASS cases=%u direct=2 conversion_required=7\n",cases);
} }
