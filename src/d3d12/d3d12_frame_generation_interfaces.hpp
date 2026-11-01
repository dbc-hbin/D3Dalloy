// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

struct MTL_FRAME_GENERATION_CREATE_DESC {
  UINT StructSize;
  UINT display_width, display_height, max_render_width, max_render_height;
  DXGI_FORMAT backbuffer_format;
  UINT flags; // FFX frame-generation depth/MV flags, bits 0..3.
  UINT user_interface_recomposition; // NGX create-time requirement for HUDLess + UI/UIAlpha.
  UINT orthographic_projection;
};
struct MTL_FRAME_GENERATION_PREPARE_DESC {
  UINT StructSize;
  UINT64 frame_id;
  ID3D12Resource *depth, *motion_vectors;
  D3D12_RESOURCE_STATES depth_state, motion_vectors_state;
  UINT render_width, render_height, motion_width, motion_height, flags, reset;
  FLOAT jitter_x, jitter_y, motion_scale_x, motion_scale_y;
  FLOAT frame_time_delta_ms, camera_near, camera_far, camera_fov_vertical_radians, view_space_to_meters;
  FLOAT camera_aspect_ratio, motion_vectors_invalid_value;
  UINT motion_vectors_invalid_value_present;
  FLOAT camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
  UINT camera_info_present;
  UINT depth_subrect_x, depth_subrect_y, motion_subrect_x, motion_subrect_y;
  UINT reconstruct_camera_motion;
  FLOAT clip_to_previous_clip[16]; // NGX row-major, unjittered clip-space transform.
  UINT projection_matrix_present;
  FLOAT view_to_clip[16];
};
struct MTL_FRAME_GENERATION_GENERATE_DESC {
  UINT StructSize;
  UINT64 frame_id;
  ID3D12Resource *present_color, *outputs[4], *hudless_color;
  D3D12_RESOURCE_STATES present_color_state, output_states[4], hudless_color_state;
  UINT num_generated_frames, reset, backbuffer_transfer_function; // Native MetalFX supports one output only.
  INT generation_rect_left, generation_rect_top, generation_rect_width, generation_rect_height;
  FLOAT min_luminance, max_luminance;
  UINT flags; // FFX tear/reset/debug-view bits 0..2; captured at recording.
  ID3D12Resource *real_output;
  D3D12_RESOURCE_STATES real_output_state;
  UINT output_subrect_only; // NGX EvalFlags bit0: preserve both outputs outside the viewport.
  ID3D12Resource *distortion;
  D3D12_RESOURCE_STATES distortion_state;
  UINT distortion_x, distortion_y, distortion_width, distortion_height;
  UINT independent_subrects; // Otherwise all color origins use generation_rect_left/top (FSR).
  UINT present_x, present_y, hudless_x, hudless_y, output_x, output_y, real_x, real_y;
  UINT distortion_low_precision;
  FLOAT distortion_scale, distortion_bias;
  ID3D12Resource *ui, *ui_alpha;
  D3D12_RESOURCE_STATES ui_state, ui_alpha_state;
  UINT ui_x, ui_y, ui_alpha_x, ui_alpha_y;
  ID3D12Resource *disable_interpolation;
};
// MTL_FRAME_GENERATION_PREPARE_DESC::flags: the caller records this frame's
// GenerateFrames next on the same command list with nothing in between, so the
// Generate pass may read depth/motion in place instead of a Prepare snapshot.
constexpr UINT MTL_FRAME_GENERATION_PREPARE_DIRECT_INPUTS = 1u << 5;
// Native-only ABI: old phase-result consumers must not use these vtables.
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a020", IMTLD3D12FrameGenerationState) : public IUnknown {
  // OFF discards frame metadata and history. Recorded native commands own
  // their snapshots and may still write their output, without seeding new history.
  virtual HRESULT STDMETHODCALLTYPE Configure(BOOL enabled) = 0;
  // Retires this frame ID only. Recorded native commands retain their snapshots.
  virtual HRESULT STDMETHODCALLTYPE RetireFrame(UINT64 frame,
      UINT64 *oldest, BOOL *pending) = 0;
};
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a021", IMTLD3D12FrameGenerationDevice) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateFrameGenerationState(const MTL_FRAME_GENERATION_CREATE_DESC *desc,
      IMTLD3D12FrameGenerationState **state) = 0;
};
// Exposed by D3Dalloy D3D12 swapchains. Presents the interpolated member of a pair whose real
// member the caller presents next; only that real Present ends the application frame
// (async-pipeline epoch, Reflex present generation and fallback sleep).
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a023", IMTLD3D12GeneratedPresent) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE PresentGenerated(UINT SyncInterval, UINT Flags) = 0;
};
// Private flip-discard source substitution. TEST validates without enqueueing.
// Queue signals following this call retire after the drawable has read source.
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a024", IMTLD3D12ResourcePresent) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE PresentResource(ID3D12Resource *source, UINT sync, UINT flags, BOOL closes_frame) = 0;
};
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a022", IMTLD3D12FrameGenerationCommandList) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE PrepareFrameGeneration(IMTLD3D12FrameGenerationState *state,
      const MTL_FRAME_GENERATION_PREPARE_DESC *desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GenerateFrames(IMTLD3D12FrameGenerationState *state,
      const MTL_FRAME_GENERATION_GENERATE_DESC *desc) = 0;
};
