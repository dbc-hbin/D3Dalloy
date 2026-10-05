// SPDX-License-Identifier: LGPL-2.1-or-later
// Native SR algorithms: adapted from zzz-wine-yaagl-pr-runtime/d3dmetal-pso-cache/metalfx-backend.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstring>
#define WINEMETAL_API
#include "../winemetal_metal4_fx.h"
#include "metalfx-backend.hpp"
namespace fx = yaagl::pso::metalfx;
extern "C" int32_t winemetal_metal4_fg_call(struct WMTMetal4Call *call);
extern "C" void winemetal_composition_present(id<MTLDrawable> drawable, double duration,
                                             const WMTCompositionFrame *frame, uint32_t sync_interval, bool allow_tearing);

@interface DXMTTemporalFeature : NSObject {
@public std::shared_ptr<fx::Feature> feature;
}
@end
@implementation DXMTTemporalFeature
@end
@interface DXMTTemporalFrame : NSObject {
@public std::shared_ptr<const fx::PreparedFrame> frame;
}
@end
@implementation DXMTTemporalFrame
@end
@interface DXMTTemporalExecution : NSObject {
@public std::shared_ptr<const fx::PreparedFrame> frame;
@public std::shared_ptr<const fx::ExecutionLease> lease;
}
@end
@implementation DXMTTemporalExecution
@end

static uint32_t resultFor(const fx::Error &error) {
  switch (error.code) {
  case fx::ErrorCode::None: return 0;
  case fx::ErrorCode::InvalidContext:
  case fx::ErrorCode::InvalidFrame:
  case fx::ErrorCode::IncompatibleTexture: return 0x80070057;
  case fx::ErrorCode::UnsupportedFeature: return 0x887a0004;
  case fx::ErrorCode::ResourceCreationFailed: return 0x8007000e;
  case fx::ErrorCode::ScalerCreationFailed: return 0x887a0004;
  case fx::ErrorCode::EncodeFailed: return 0x887a0020;
  }
}
static fx::Rect rect(const WMTMetal4FXRect &r) { return {r.x, r.y, r.width, r.height}; }
#include "metalfx_rr.inc"
extern "C" int32_t winemetal_metal4_fx_call(struct WMTMetal4Call *call) {
  if (call->operation >= 240 && call->operation < 256) return winemetal_metal4_fg_call(call);
  @autoreleasepool {
    @try {
      if (@available(macOS 26.0, *)) {
        if (dxmtRRCall(call)) return 0;
        switch (call->operation) {
        case WMTMetal4FXSupportsTemporal:
          if (call->args[1] & 256u)
            call->ret[0] = [MTLFXTemporalDenoisedScalerDescriptor supportsMetal4FX:(id<MTLDevice>)call->args[0]];
          else if (call->args[1] & 512u)
            call->ret[0] = [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:(id<MTLDevice>)call->args[0]];
          else call->ret[0] = [MTLFXTemporalScalerDescriptor supportsMetal4FX:(id<MTLDevice>)call->args[0]];
          return 0;
        case WMTMetal4FXCreateTemporal: {
          const auto &p = *reinterpret_cast<const WMTMetal4FXTemporalInfo *>(call->args[2]);
          fx::CreateInfo info;
          info.input = {p.input_width, p.input_height}; info.output = {p.output_width, p.output_height};
          info.featureFlags = p.feature_flags; info.outputSubrects = p.output_subrects; info.dynamicOutput = p.dynamic_output;
          fx::Error error;
          auto feature = fx::Feature::create({reinterpret_cast<void *>(call->args[0]),
              reinterpret_cast<void *>(call->args[1]), fx::CommandMode::Metal4}, info, &error);
          call->ret[1] = resultFor(error);
          if (feature) {
            auto holder = [DXMTTemporalFeature new]; holder->feature = std::move(feature);
            call->ret[0] = reinterpret_cast<uintptr_t>(holder);
          }
          return 0;
        }
        case WMTMetal4FXDormantTemporal:
          ((DXMTTemporalFeature *)call->args[0])->feature->markDormant();
          return 0;
        case WMTMetal4FXPrepareTemporal: {
          const auto &p = *reinterpret_cast<const WMTMetal4FXTemporalFrame *>(call->args[1]);
          fx::FrameInfo info;
          info.color = reinterpret_cast<void *>(p.color); info.depth = reinterpret_cast<void *>(p.depth);
          info.motionVectors = reinterpret_cast<void *>(p.motion); info.output = reinterpret_cast<void *>(p.output);
          info.exposureTexture = reinterpret_cast<void *>(p.exposure); info.reactiveMask = reinterpret_cast<void *>(p.reactive);
          info.compositionMask = reinterpret_cast<void *>(p.composition);
          info.inputContent = {p.input_width, p.input_height};
          info.colorRect = rect(p.color_rect); info.depthRect = rect(p.depth_rect); info.motionRect = rect(p.motion_rect);
          info.reactiveRect = rect(p.reactive_rect); info.compositionRect = rect(p.composition_rect); info.outputRect = rect(p.output_rect);
          info.jitterOffsetX = p.jitter_x; info.jitterOffsetY = p.jitter_y;
          info.motionVectorScaleX = p.motion_scale_x; info.motionVectorScaleY = p.motion_scale_y;
          info.preExposure = p.pre_exposure; info.resetHistory = p.reset; info.exposureMode = fx::ExposureMode(p.exposure_mode);
          fx::FrameOperations operations{fx::ColorTransfer(p.color_transfer), bool(p.combine_composition),
              bool(p.sharpening), p.sharpness, bool(p.cap_output), bool(p.debug_view)};
          fx::TextureSet textures{info.color, info.depth, info.motionVectors, info.output,
              info.exposureTexture, info.reactiveMask, info.compositionMask};
          fx::Error error;
          auto frame = ((DXMTTemporalFeature *)call->args[0])->feature->prepare(info, textures, &error, operations);
          call->ret[1] = resultFor(error);
          if (frame) {
            auto holder = [DXMTTemporalFrame new]; call->ret[2] = frame->generationId();
            holder->frame = std::move(frame); call->ret[0] = reinterpret_cast<uintptr_t>(holder);
          }
          return 0;
        }
        case WMTMetal4FXEncodeTemporal: {
          auto holder = [DXMTTemporalExecution new];
          holder->frame = ((DXMTTemporalFrame *)call->args[1])->frame;
          // The mutable lease slot is published by PreparedFrame before ANY GPU command.
          // Attach its owner beforehand; partial failure still retires via M4Recording.objects.
          winemetal_metal4_retain_resource(call->args[0], holder);
          [holder release];
          winemetal_metal4_retain_resource(call->args[0], (id)call->args[2]);
          fx::Error error;
          const bool success = holder->frame->encode(winemetal_metal4_command_buffer(call->args[0]),
              reinterpret_cast<void *>(call->args[2]), holder->lease, &error, bool(call->args[3]),
              reinterpret_cast<void *>(call->args[0]), winemetal_metal4_split_command_buffer);
          call->ret[1] = success ? 0 : (resultFor(error) ? resultFor(error) : 0x887a0020);
          return 0;
        }
        case WMTMetal4FXPresentDrawable: {
          double duration; std::memcpy(&duration, &call->args[1], sizeof(duration));
          id<MTLDrawable> drawable = (id<MTLDrawable>)call->args[0];
          winemetal_composition_present(drawable, duration,
              reinterpret_cast<const WMTCompositionFrame *>(call->args[2]), uint32_t(call->args[3]),
              call->args[4] != 0);
          return 0;
        }
        default: return int32_t(0xc0000002);
        }
      }
      return int32_t(0xc0000002);
    } @catch (id) {
      call->ret[1] = 0x887a0020;
      return 0;
    }
  }
}
