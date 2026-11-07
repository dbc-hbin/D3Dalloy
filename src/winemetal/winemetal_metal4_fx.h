// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "winemetal_metal4.h"

/* SR 224..239; FG 240..255; additional FX 256..287. */
enum WMTMetal4FXOperation {
  WMTMetal4FXSupportsTemporal = 224,
  WMTMetal4FXCreateTemporal = 225,
  WMTMetal4FXEncodeTemporal = 226,
  WMTMetal4FXPresentDrawable = 227, /* args: drawable, double duration bits, CPU composition frame, sync interval, allow tearing (0/1) */
  WMTMetal4FXReservePresent = 230, /* args: MTL4 command buffer, drawable, duration bits, composition frame, sync, tearing; presents at commit */
  WMTMetal4FXPrepareTemporal = 228,
  WMTMetal4FXDormantTemporal = 229,
  WMTMetal4FXCreateSpatial = 231,
  WMTMetal4FXPrepareSpatial = 232,
  WMTMetal4FXEncodeSpatial = 233,
};
/* Private feature selector carried in the replayable scaler transport. */
#define WMTMetal4FXSpatialFlag 1024u
/* Encode args[4] offers adjacent queue encoders; ret[3] reports edges actually used. */
enum WMTMetal4FXHandoff { WMTMetal4FXProducer = 1u, WMTMetal4FXConsumer = 2u };
struct WMTMetal4FXRect { uint32_t x, y, width, height; };
struct WMTMetal4FXTemporalInfo {
  uint32_t input_width, input_height, output_width, output_height;
  uint32_t feature_flags, output_subrects, dynamic_output;
};
/* No COM objects or C++ layouts cross the native boundary. */
struct WMTMetal4FXTemporalFrame {
  uint64_t color, depth, motion, output, exposure, reactive, composition;
  uint32_t input_width, input_height;
  struct WMTMetal4FXRect color_rect, depth_rect, motion_rect, reactive_rect, composition_rect, output_rect;
  float jitter_x, jitter_y, motion_scale_x, motion_scale_y, pre_exposure, sharpness;
  uint32_t reset, exposure_mode, color_transfer, combine_composition, sharpening, cap_output, debug_view;
  uint64_t diffuse_albedo, specular_albedo, normals, roughness, specular_hit_distance;
  float world_to_view[16], view_to_clip[16];
  uint32_t roughness_packed, depth_linear;
  struct WMTMetal4FXRect diffuse_albedo_rect, specular_albedo_rect, normals_rect, roughness_rect, specular_hit_distance_rect;
};
#ifdef __cplusplus
static_assert(sizeof(WMTMetal4FXTemporalInfo) == 28);
static_assert(sizeof(WMTMetal4FXTemporalFrame) == 472);
#endif
