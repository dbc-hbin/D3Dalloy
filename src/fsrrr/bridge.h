/* In-module packets between the AMD RR 1.0/1.1/1.2 ABI frontend and its D3D12 backend. */
#pragma once
#include <stdint.h>

struct rr_packet_header
{
    uint32_t size;
    uint32_t result;
    uint32_t operation;
    uint32_t reserved;
    uint64_t context;
};

#define RR_BRIDGE_SIGNAL_COUNT 7
#define RR_BRIDGE_COMMON_COUNT 5
#define RR_BRIDGE_FUSED_ALBEDO (RR_BRIDGE_COMMON_COUNT + 2 * RR_BRIDGE_SIGNAL_COUNT)
#define RR_BRIDGE_RESOURCE_COUNT (RR_BRIDGE_FUSED_ALBEDO + 2)

enum rr_bridge_operation
{
    RR_BRIDGE_CREATE, RR_BRIDGE_DESTROY, RR_BRIDGE_CONFIGURE,
    RR_BRIDGE_QUERY, RR_BRIDGE_DISPATCH
};

/* Common resources: linear depth, motion, packed normal, specular albedo,
 * diffuse albedo. Followed by input/output pairs in AMD signal-bit order,
 * then legacy fused albedo and the optional debug output. */
struct rr_bridge_resource
{
    uint64_t object;
    uint32_t state;
    uint32_t reserved;
};

struct rr_bridge_create_packet
{
    struct rr_packet_header header;
    uint64_t device;
    uint32_t version, width, height, signals, checkerboard, flags;
};

struct rr_bridge_settings
{
    float normal_strength, stability_bias, max_radiance, radiance_clip_std_k;
    float gaussian_relaxation, disocclusion_threshold;
    float debug_depth_min, debug_depth_max;
    float history_rejection_strength;
};

struct rr_bridge_configure_packet
{
    struct rr_packet_header header;
    uint64_t key;
    float value[2];
    /* key 8 atomically configures the six RR 1.0 settings. */
    float legacy_values[6];
};

struct rr_bridge_query_packet
{
    struct rr_packet_header header;
    uint64_t key;
    /* key 0 queries memory; keys 1..7 query default settings. */
    float value[2];
    uint64_t total_bytes, aliasable_bytes;
    uint64_t device;
    uint32_t width, height, signals, checkerboard, flags, reserved;
};

struct rr_bridge_dispatch_packet
{
    struct rr_packet_header header;
    uint64_t command_list;
    struct rr_bridge_resource resources[RR_BRIDGE_RESOURCE_COUNT];
    float motion_scale[3], jitter[2], camera_delta[3];
    float view[16], projection[16];
    float depth_min, depth_max;
    uint32_t width, height, frame_index, flags;
    uint32_t checkerboard_origin[RR_BRIDGE_SIGNAL_COUNT];
    float light_direction[3], light_emission[3], light_angular_radius;
    uint32_t debug_width, debug_height, debug_mode, debug_viewport;
    /* 0: 1.2 independent signals; 1: legacy split; 2: fused pair; 3: composite. */
    uint32_t legacy_mode;
};

#ifdef __cplusplus
extern "C"
#endif
uint32_t rr_backend_call(void *packet);
