/* RR 1.0/1.1 wire layouts from FidelityFX SDK v2.1.0/v2.2.0 ffx_denoiser.h.
 * Names are private so these layouts can coexist with the 1.2 SDK header. */
#pragma once
#define RR_10 0x00400000u
#define RR_11 0x00401000u
#define RR_TYPE(id) FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, id)
struct rr_legacy_create {
    ffxCreateContextDescHeader header;
    uint32_t version;
    struct FfxApiDimensions2D maxRenderSize;
    ffxApiMessage fpMessage;
    uint32_t mode, flags;
};
struct rr_legacy_signal {
    struct FfxApiResource input, output;
    uint32_t reserved[2];
};
struct rr_legacy_settings {
    float historyRejectionStrength, crossBilateralNormalStrength, stabilityBias;
    float maxRadiance, radianceClipStdK, gaussianKernelRelaxation;
    uint32_t reserved[26];
};
/* The motion scale is the only dispatch layout change between 1.0 and 1.1. */
#define RR_LEGACY_FRAME_FIELDS(MOTION) \
    ffxDispatchDescHeader header; \
    void *commandList; \
    struct FfxApiResource linearDepth, motionVectors, normals, specularAlbedo, diffuseAlbedo; \
    struct MOTION motionVectorScale; \
    struct FfxApiFloatCoords2D jitterOffsets; \
    struct FfxApiFloatCoords3D cameraPositionDelta, cameraRight, cameraUp, cameraForward; \
    float cameraAspectRatio, cameraNear, cameraFar, cameraFovAngleVertical; \
    struct FfxApiDimensions2D renderSize; \
    float deltaTime; \
    uint32_t frameIndex, flags
struct rr_frame_10 { RR_LEGACY_FRAME_FIELDS(FfxApiFloatCoords2D); };
struct rr_frame_11 { RR_LEGACY_FRAME_FIELDS(FfxApiFloatCoords3D); };
#undef RR_LEGACY_FRAME_FIELDS
struct rr_legacy_four {
    ffxDispatchDescHeader header;
    struct rr_legacy_signal indirectSpecularRadiance, indirectDiffuseRadiance;
    struct rr_legacy_signal directSpecularRadiance, directDiffuseRadiance;
};
struct rr_legacy_two {
    ffxDispatchDescHeader header;
    struct rr_legacy_signal specularRadiance, diffuseRadiance;
};
struct rr_legacy_one {
    ffxDispatchDescHeader header;
    struct rr_legacy_signal radiance;
    struct FfxApiResource fusedAlbedo;
};
struct rr_legacy_light {
    ffxDispatchDescHeader header;
    struct rr_legacy_signal dominantLightVisibility;
    struct FfxApiFloatCoords3D dominantLightDirection, dominantLightEmission;
};
struct rr_configure_10 { ffxConfigureDescHeader header; struct rr_legacy_settings settings; };
struct rr_defaults_10 { ffxQueryDescHeader header; void *device; struct rr_legacy_settings *defaultSettings; };
struct rr_legacy_memory {
    ffxQueryDescHeader header;
    void *device;
    struct FfxApiDimensions2D maxRenderSize;
    uint32_t mode, flags;
    struct FfxApiEffectMemoryUsage *gpuMemoryUsage;
};
struct rr_legacy_version {
    ffxQueryDescHeader header;
    void *device;
    uint32_t *major, *minor, *patch;
};
