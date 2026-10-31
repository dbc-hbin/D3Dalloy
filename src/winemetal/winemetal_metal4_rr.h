/* Fixed-width private D3D12/native contract for AMD Ray Regeneration 1.0/1.1/1.2. */
#pragma once

#include <stdint.h>
#include "winemetal_metal4.h"

#define WMTRR_SIGNAL_COUNT 7
#define WMTRR_COMMON_COUNT 5
#define WMTRR_FUSED_ALBEDO (WMTRR_COMMON_COUNT + 2 * WMTRR_SIGNAL_COUNT)
#define WMTRR_RESOURCE_COUNT (WMTRR_FUSED_ALBEDO + 2)

#pragma pack(push, 8)
/* AMD ffxCreateContextDescDenoiser values after legacy normalization. */
struct WMTRRCreateInfo
{
    uint32_t version, width, height, signals, checkerboard, flags;
};

struct WMTRRSettings
{
    float normal_strength, stability_bias, max_radiance, radiance_clip_std_k;
    float gaussian_relaxation, disocclusion_threshold;
    float debug_depth_min, debug_depth_max;
    float history_rejection_strength;
};

/* Textures: linear depth, motion, packed normal, specular albedo, diffuse
 * albedo; input/output pairs in AMD signal-bit order; legacy fused albedo;
 * the optional debug output. Native texture handles, not COM objects. */
struct WMTRRFrame
{
    uint64_t textures[WMTRR_RESOURCE_COUNT];
    struct WMTRRSettings settings;
    float motion_scale[3], jitter[2], camera_delta[3];
    float view[16], projection[16];
    float depth_min, depth_max;
    uint32_t width, height, frame_index, flags;
    uint32_t checkerboard_origin[WMTRR_SIGNAL_COUNT];
    float light_direction[3], light_emission[3], light_angular_radius;
    uint32_t debug_width, debug_height, debug_mode, debug_viewport;
    /* 0: 1.2 independent signals; 1: legacy split; 2: fused pair; 3: composite. */
    uint32_t legacy_mode;
};
#pragma pack(pop)

/* Native results use the FFX return-code values. */
enum WMTRRResult { WMTRR_OK = 0, WMTRR_RUNTIME = 3, WMTRR_NO_PROVIDER = 4, WMTRR_MEMORY = 5, WMTRR_PARAMETER = 6 };

/*
 * Create: args device, compiler, CPU WMTRRCreateInfo; ret feature, result.
 * Prepare: args feature, CPU WMTRRFrame; ret prepared frame, result.
 * Encode: args MTL4 command buffer, prepared frame, fence; ret result.
 * Dormant: args feature. Memory: args feature; ret bytes.
 */
enum WMTMetal4RROperation
{
    WMTMetal4RRCreate = 260, WMTMetal4RRPrepare = 261, WMTMetal4RREncode = 262,
    WMTMetal4RRDormant = 263, WMTMetal4RRMemory = 264
};

/* Provider-specific correction defaults, not AMD or Apple model defaults. */
static inline struct WMTRRSettings WMTRRDefaultSettings(void)
{
    struct WMTRRSettings settings = {0.5f, 0.75f, 65504.0f, 2.0f, 0.5f, 0.02f, 0.0f, 1000.0f, 0.0f};
    return settings;
}

#ifdef __cplusplus
static_assert(sizeof(WMTRRCreateInfo) == 24);
static_assert(sizeof(WMTRRSettings) == 36);
static_assert(sizeof(WMTRRFrame) == 464);
#endif
