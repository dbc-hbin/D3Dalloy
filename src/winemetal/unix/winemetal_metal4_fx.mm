// SPDX-License-Identifier: LGPL-2.1-or-later
// Native SR algorithms: adapted from zzz-wine-yaagl-pr-runtime/d3dmetal-pso-cache/metalfx-backend.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstring>
#define WINEMETAL_API
#include "../winemetal_metal4_fx.h"
#include "metalfx-backend.hpp"
#include "metalfx_amd_rr.hpp"
namespace fx = yaagl::pso::metalfx;
extern "C" int32_t winemetal_metal4_fg_call(struct WMTMetal4Call *call);
extern "C" void winemetal_composition_present(id<MTLDrawable> drawable, double duration,
                                             const WMTCompositionFrame *frame, uint32_t sync_interval, bool allow_tearing);

@interface D3DalloyTemporalFeature : NSObject {
@public std::shared_ptr<fx::Feature> feature;
}
@end
@implementation D3DalloyTemporalFeature
@end
@interface D3DalloyTemporalFrame : NSObject {
@public std::shared_ptr<const fx::PreparedFrame> frame;
}
@end
@implementation D3DalloyTemporalFrame
@end
@interface D3DalloyTemporalExecution : NSObject {
@public std::shared_ptr<const fx::PreparedFrame> frame;
@public std::shared_ptr<const fx::ExecutionLease> lease;
}
@end
@implementation D3DalloyTemporalExecution
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

@interface D3DalloyAmdRRFeature : NSObject {
@public std::shared_ptr<d3dalloy::rr::Feature> feature;
}
@end
@implementation D3DalloyAmdRRFeature
@end
@interface D3DalloyAmdRRFrame : NSObject {
@public std::shared_ptr<const d3dalloy::rr::PreparedFrame> frame;
}
@end
@implementation D3DalloyAmdRRFrame
@end
@interface D3DalloyAmdRRExecution : NSObject {
@public std::shared_ptr<const d3dalloy::rr::PreparedFrame> frame;
@public std::shared_ptr<const d3dalloy::rr::ExecutionLease> lease;
}
@end
@implementation D3DalloyAmdRRExecution
@end
static uint64_t amdRRResult(const fx::Error &error) {
  switch (error.code) {
  case fx::ErrorCode::None: return WMTRR_OK;
  case fx::ErrorCode::InvalidContext:
  case fx::ErrorCode::InvalidFrame: return WMTRR_PARAMETER;
  case fx::ErrorCode::UnsupportedFeature:
  case fx::ErrorCode::IncompatibleTexture:
  case fx::ErrorCode::ScalerCreationFailed: return WMTRR_NO_PROVIDER;
  case fx::ErrorCode::ResourceCreationFailed: return WMTRR_MEMORY;
  default: return WMTRR_RUNTIME;
  }
}
// AMD Ray Regeneration: one native feature per FFX context, one prepared frame per recorded dispatch.
static bool amdRRCall(struct WMTMetal4Call *call) {
  switch (call->operation) {
  case WMTMetal4RRCreate: {
    fx::Error error;
    auto feature = d3dalloy::rr::Feature::create(reinterpret_cast<void *>(call->args[0]),
        reinterpret_cast<void *>(call->args[1]), *reinterpret_cast<const WMTRRCreateInfo *>(call->args[2]), &error);
    call->ret[0] = 0; call->ret[1] = amdRRResult(error);
    if (feature) {
      auto holder = [D3DalloyAmdRRFeature new]; holder->feature = std::move(feature);
      call->ret[0] = reinterpret_cast<uintptr_t>(holder); call->ret[1] = WMTRR_OK;
    } else if (!call->ret[1]) call->ret[1] = WMTRR_RUNTIME;
    return true;
  }
  case WMTMetal4RRPrepare: {
    const auto &p = *reinterpret_cast<const WMTRRFrame *>(call->args[1]);
    d3dalloy::rr::TextureSet textures{};
    for (size_t i = 0; i < textures.size(); ++i) textures[i] = reinterpret_cast<void *>(p.textures[i]);
    fx::Error error;
    auto frame = ((D3DalloyAmdRRFeature *)call->args[0])->feature->prepare(p, textures, p.settings, &error);
    call->ret[0] = 0; call->ret[1] = amdRRResult(error);
    if (frame) {
      auto holder = [D3DalloyAmdRRFrame new]; holder->frame = std::move(frame);
      call->ret[0] = reinterpret_cast<uintptr_t>(holder); call->ret[1] = WMTRR_OK;
    } else if (!call->ret[1]) call->ret[1] = WMTRR_RUNTIME;
    return true;
  }
  case WMTMetal4RREncode: {
    auto holder = [D3DalloyAmdRRExecution new];
    holder->frame = ((D3DalloyAmdRRFrame *)call->args[1])->frame;
    // The lease is published before any GPU command; the recording owns it through retirement.
    winemetal_metal4_retain_resource(call->args[0], holder);
    [holder release];
    winemetal_metal4_retain_resource(call->args[0], (id)call->args[2]);
    fx::Error error;
    const bool success = holder->frame->encode(winemetal_metal4_command_buffer(call->args[0]),
        reinterpret_cast<void *>(call->args[2]), holder->lease, winemetal_metal4_split_command_buffer,
        reinterpret_cast<void *>(call->args[0]), &error);
    call->ret[0] = success ? WMTRR_OK : (amdRRResult(error) ? amdRRResult(error) : WMTRR_RUNTIME);
    return true;
  }
  case WMTMetal4RRDormant:
    ((D3DalloyAmdRRFeature *)call->args[0])->feature->markDormant();
    return true;
  case WMTMetal4RRMemory:
    call->ret[0] = ((D3DalloyAmdRRFeature *)call->args[0])->feature->memoryUsage();
    return true;
  default: return false;
  }
}
#include "metalfx_rr.inc"
#include "metalfx_spatial.inc"
extern "C" int32_t winemetal_metal4_fx_call(struct WMTMetal4Call *call) {
  if (call->operation >= 240 && call->operation < 256) return winemetal_metal4_fg_call(call);
  @autoreleasepool {
    @try {
      if (@available(macOS 26.0, *)) {
        if (d3dalloyRRCall(call) || amdRRCall(call)) return 0;
        switch (call->operation) {
        case WMTMetal4FXCreateSpatial:
        case WMTMetal4FXPrepareSpatial:
        case WMTMetal4FXEncodeSpatial:
          spatialCall(call);
          return 0;
        case WMTMetal4FXSupportsTemporal:
          if (call->args[1] == WMTMetal4FXSpatialFlag)
            call->ret[0] = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)call->args[0]];
          else if (call->args[1] & 256u)
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
              reinterpret_cast<void *>(call->args[1])}, info, &error);
          call->ret[1] = resultFor(error);
          if (feature) {
            auto holder = [D3DalloyTemporalFeature new]; holder->feature = std::move(feature);
            call->ret[0] = reinterpret_cast<uintptr_t>(holder);
          }
          return 0;
        }
        case WMTMetal4FXDormantTemporal:
          ((D3DalloyTemporalFeature *)call->args[0])->feature->markDormant();
          return 0;
        case WMTMetal4FXPrepareTemporal: {
          const auto &p = *reinterpret_cast<const WMTMetal4FXTemporalFrame *>(call->args[1]);
          fx::FrameInfo info;
          info.inputContent = {p.input_width, p.input_height};
          info.colorRect = rect(p.color_rect); info.depthRect = rect(p.depth_rect); info.motionRect = rect(p.motion_rect);
          info.reactiveRect = rect(p.reactive_rect); info.compositionRect = rect(p.composition_rect); info.outputRect = rect(p.output_rect);
          info.jitterOffsetX = p.jitter_x; info.jitterOffsetY = p.jitter_y;
          info.motionVectorScaleX = p.motion_scale_x; info.motionVectorScaleY = p.motion_scale_y;
          info.preExposure = p.pre_exposure; info.resetHistory = p.reset; info.exposureMode = fx::ExposureMode(p.exposure_mode);
          fx::FrameOperations operations{fx::ColorTransfer(p.color_transfer), bool(p.combine_composition),
              bool(p.sharpening), p.sharpness, bool(p.cap_output), bool(p.debug_view)};
          fx::TextureSet textures{reinterpret_cast<void *>(p.color), reinterpret_cast<void *>(p.depth),
              reinterpret_cast<void *>(p.motion), reinterpret_cast<void *>(p.output), reinterpret_cast<void *>(p.exposure),
              reinterpret_cast<void *>(p.reactive), reinterpret_cast<void *>(p.composition)};
          fx::Error error;
          auto frame = ((D3DalloyTemporalFeature *)call->args[0])->feature->prepare(info, textures, &error, operations);
          call->ret[1] = resultFor(error);
          if (frame) {
            auto holder = [D3DalloyTemporalFrame new]; call->ret[2] = frame->generationId();
            holder->frame = std::move(frame); call->ret[0] = reinterpret_cast<uintptr_t>(holder);
          }
          return 0;
        }
        case WMTMetal4FXEncodeTemporal: {
          auto holder = [D3DalloyTemporalExecution new];
          holder->frame = ((D3DalloyTemporalFrame *)call->args[1])->frame;
          // The mutable lease slot is published by PreparedFrame before ANY GPU command.
          // Attach its owner beforehand; partial failure still retires via M4Recording.objects.
          winemetal_metal4_retain_resource(call->args[0], holder);
          [holder release];
          winemetal_metal4_retain_resource(call->args[0], (id)call->args[2]);
          fx::Error error;
          uint32_t handoff = uint32_t(call->args[4]);
          const bool success = holder->frame->encode(winemetal_metal4_command_buffer(call->args[0]),
              reinterpret_cast<void *>(call->args[2]), holder->lease, &error, bool(call->args[3]),
              reinterpret_cast<void *>(call->args[0]), winemetal_metal4_split_command_buffer, &handoff);
          call->ret[3] = success ? handoff : 0;
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
        case WMTMetal4FXReservePresent: {
          return winemetal_metal4_reserve_present(call);
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
