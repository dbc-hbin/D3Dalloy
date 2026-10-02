// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "winemetal_metal4.h"

/* SR 224..239; FG 240..255; additional FX 256..287. */
enum WMTMetal4FXOperation {
  WMTMetal4FXSupportsTemporal = 224,
  WMTMetal4FXCreateTemporal = 225,
  WMTMetal4FXEncodeTemporal = 226,
  WMTMetal4FXPresentDrawable = 227,
  WMTMetal4FXPrepareTemporal = 228,
  WMTMetal4FXDormantTemporal = 229,
};
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
  uint32_t reset, exposure_mode, color_transfer, combine_composition, sharpening, cap_output;
};
#ifdef __cplusplus
static_assert(sizeof(WMTMetal4FXTemporalInfo) == 28);
static_assert(sizeof(WMTMetal4FXTemporalFrame) == 208);
#endif
