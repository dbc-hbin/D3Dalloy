/* Fixed-width private PE/Unix/native contract for FSR frame generation. */
#pragma once

#include <stdint.h>
#include "winemetal_metal4.h"

#define WMTFG_BRIDGE_VERSION 12u

#pragma pack(push, 8)
enum WMTFG_create_flags
{
    WMTFG_DEPTH_INVERTED = 1u << 0,
    WMTFG_DEPTH_INFINITE = 1u << 1,
    WMTFG_DISPLAY_RESOLUTION_MOTION_VECTORS = 1u << 2,
    WMTFG_MOTION_VECTORS_JITTERED = 1u << 3
};

enum WMTFG_prepare_flags
{
    /* The caller records this frame's Generate next on the same command list, with
     * nothing in between. Native may then reference depth/motion instead of copying
     * them; it records no Prepare command (ret[1] is 0) when it accepts. */
    WMTFG_PREPARE_DIRECT_INPUTS = 1u << 5
};

enum WMTFGoperation
{
    /* 0 is reserved; it was a removed capability probe. */
    WMTFG_CREATE = 1,
    WMTFG_PREPARE = 2,
    WMTFG_DISPATCH = 3,
    WMTFG_DESTROY = 4,
    WMTFG_CONFIGURE = 5,
    WMTFG_RETIRE_FRAME = 6
};

struct WMTFGpacket_header
{
    uint32_t size;
    uint32_t version;
    uint32_t operation;
    uint32_t result;
    uint64_t context;
};

struct WMTFGcreate_packet
{
    struct WMTFGpacket_header header;
    uint64_t device;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t max_render_width;
    uint32_t max_render_height;
    uint32_t backbuffer_format;
    uint32_t flags;
};

struct WMTFGprepare_packet
{
    struct WMTFGpacket_header header;
    uint64_t command_list;
    uint64_t frame_id;
    uint64_t depth;
    uint64_t motion_vectors;
    uint32_t depth_state;
    uint32_t motion_vectors_state;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t motion_width;
    uint32_t motion_height;
    uint32_t flags;
    uint32_t reset;
    float jitter_x;
    float jitter_y;
    float motion_scale_x;
    float motion_scale_y;
    float frame_time_delta_ms;
    float camera_near;
    float camera_far;
    float camera_fov_vertical_radians;
    float view_space_to_meters;
    float camera_aspect_ratio;
    float motion_vectors_invalid_value;
    uint32_t motion_vectors_invalid_value_present;
    float camera_position[3];
    float camera_up[3];
    float camera_right[3];
    float camera_forward[3];
    uint32_t camera_info_present;
    uint32_t depth_subrect_x;
    uint32_t depth_subrect_y;
    uint32_t motion_subrect_x;
    uint32_t motion_subrect_y;
    uint32_t reconstruct_camera_motion;
    float clip_to_previous_clip[16];
    uint32_t projection_matrix_present;
    float view_to_clip[16];
};

struct WMTFGdispatch_packet
{
    struct WMTFGpacket_header header;
    uint64_t command_list;
    uint64_t frame_id;
    uint64_t present_color;
    uint64_t output;
    uint64_t hudless_color;
    uint32_t present_color_state;
    uint32_t output_state;
    uint32_t hudless_color_state;
    uint32_t num_generated_frames;
    uint32_t reset;
    uint32_t backbuffer_transfer_function;
    int32_t generation_rect_left;
    int32_t generation_rect_top;
    int32_t generation_rect_width;
    int32_t generation_rect_height;
    float min_luminance;
    float max_luminance;
    uint32_t flags;
    uint64_t real_output;
    uint32_t real_output_state;
    uint32_t output_subrect_only;
    uint64_t distortion;
    uint32_t distortion_state;
    uint32_t distortion_x;
    uint32_t distortion_y;
    uint32_t distortion_width;
    uint32_t distortion_height;
    uint32_t independent_subrects;
    uint32_t present_x, present_y, hudless_x, hudless_y, output_x, output_y, real_x, real_y;
    uint32_t distortion_low_precision;
    float distortion_scale, distortion_bias;
    uint64_t ui, ui_alpha;
    uint32_t ui_x, ui_y, ui_alpha_x, ui_alpha_y;
    uint64_t disable_interpolation, disable_interpolation_offset;
};

/* Configure changes metadata ownership; recorded commands retain their own snapshots. */
struct WMTFGconfigure_packet
{
    struct WMTFGpacket_header header;
    uint32_t enabled;
};

/* Retire one completed or skipped generation callback; encoded work owns its snapshot. */
struct WMTFGretire_frame_packet
{
    struct WMTFGpacket_header header;
    uint64_t frame_id;
    uint64_t oldest_pending_frame_id;
    uint32_t has_pending_frame;
};

struct WMTFGdestroy_packet
{
    struct WMTFGpacket_header header;
};

struct WMTFGNativeCommand { uint64_t device, compiler; };
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(WMTFGpacket_header) == 24);
static_assert(sizeof(WMTFGcreate_packet) == 56);
static_assert(sizeof(WMTFGprepare_packet) == 344);
static_assert(sizeof(WMTFGdispatch_packet) == 264);
static_assert(sizeof(WMTFGconfigure_packet) == 32);
static_assert(sizeof(WMTFGretire_frame_packet) == 48);
#endif
enum WMTMetal4FGOperation { WMTMetal4FGRequest = 240, WMTMetal4FGEncode = 241 };
#ifdef __OBJC__
extern "C" int32_t winemetal_metal4_fg_call(struct WMTMetal4Call *call);
#endif
