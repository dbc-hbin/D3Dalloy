// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

struct MTL_FRAME_GENERATION_CREATE_DESC {
  UINT StructSize;
  UINT display_width, display_height, max_render_width, max_render_height;
  DXGI_FORMAT backbuffer_format;
  UINT flags; // FFX frame-generation depth/MV flags, bits 0..3.
};
struct MTL_FRAME_GENERATION_PREPARE_DESC {
  UINT StructSize;
  UINT64 frame_id;
  ID3D12Resource *depth, *motion_vectors;
  D3D12_RESOURCE_STATES depth_state, motion_vectors_state;
  UINT render_width, render_height, flags, reset;
  FLOAT jitter_x, jitter_y, motion_scale_x, motion_scale_y;
  FLOAT frame_time_delta_ms, camera_near, camera_far, camera_fov_vertical_radians, view_space_to_meters;
  FLOAT camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
  UINT camera_info_present;
};
struct MTL_FRAME_GENERATION_GENERATE_DESC {
  UINT StructSize;
  UINT64 frame_id;
  ID3D12Resource *present_color, *output, *hudless_color;
  D3D12_RESOURCE_STATES present_color_state, output_state, hudless_color_state;
  UINT num_generated_frames, reset, backbuffer_transfer_function;
  INT generation_rect_left, generation_rect_top, generation_rect_width, generation_rect_height;
  FLOAT min_luminance, max_luminance;
};
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a010", IMTLD3D12FrameGenerationState) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Configure(BOOL enabled) = 0;
  virtual HRESULT STDMETHODCALLTYPE RetireFrame(UINT64 frame, UINT64 *oldest, BOOL *pending) = 0;
};
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a011", IMTLD3D12FrameGenerationDevice) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateFrameGenerationState(const MTL_FRAME_GENERATION_CREATE_DESC *desc,
      IMTLD3D12FrameGenerationState **state) = 0;
};
DEFINE_COM_INTERFACE("b2a31090-a1f2-4717-bb50-617a47f2a012", IMTLD3D12FrameGenerationCommandList) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE PrepareFrameGeneration(IMTLD3D12FrameGenerationState *state,
      const MTL_FRAME_GENERATION_PREPARE_DESC *desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GenerateFrame(IMTLD3D12FrameGenerationState *state,
      const MTL_FRAME_GENERATION_GENERATE_DESC *desc) = 0;
};
