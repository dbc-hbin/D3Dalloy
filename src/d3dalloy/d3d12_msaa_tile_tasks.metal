// SPDX-License-Identifier: LGPL-2.1-or-later
// Included after the native primitive/bounds transports in d3dalloy_command.metal.
#include "../d3d12/d3d12_msaa_tile_tasks.hpp"

using namespace d3dalloy;

static_assert(sizeof(D3DALLOYMSAAPrimitiveDraw) == 24);
static_assert(sizeof(D3DALLOYMSAATileBounds) == 28);
static_assert(sizeof(D3DALLOYMSAATile) == 172);

static void msaa_tile_task_publish(device const MSAATileTaskState &state,
                                   device MSAATileTaskCompletion &completion) {
  completion.Status = state.Status;
  completion.Error = state.Error;
  // The separately fenced extent reduction owns Width/Height.
  completion.CompletedTasks = state.CompletedTasks;
  completion.Quantum = state.Quantum;
  completion.Primitive = state.Primitive;
  completion.ReadyTasks = state.BatchCount;
  for (uint i = 0; i < 6; ++i) completion.Reserved[i] = 0;
}

static void msaa_tile_task_fail(device MSAATileTaskState &state, uint error) {
  state.Status = kMSAATileError;
  state.Error = error;
  state.BatchCount = state.Positioned = 0;
}

// Clamp before testing emptiness, then align the first tile down without an
// overflowing round-up. Primitive bounds already include their raster fringe.
static bool msaa_tile_task_rect(D3DALLOYMSAATileBounds bounds,
                                constant MSAATileTaskParams &params, thread uint4 &rect) {
  if (bounds.viewport_index >= min(params.ViewportCount, 16u) ||
      bounds.target_layer >= params.Layers) return false;
  rect = uint4(min(bounds.left, params.Width), min(bounds.top, params.Height),
               min(bounds.right, params.Width), min(bounds.bottom, params.Height));
  if (rect.x >= rect.z || rect.y >= rect.w) return false;
  rect.x &= ~7u;
  rect.y &= ~7u;
  return true;
}

// Generation moves the private cursor only. Completion accounting happens at
// the following publish, after the generated batch's native consumer fence.
static void msaa_tile_task_advance(device MSAATileTaskState &state,
                                   constant MSAATileTaskParams &params,
                                   device const D3DALLOYMSAATileBounds *bounds) {
  if (!state.Positioned || state.Primitive >= params.MaxPrimitives) {
    msaa_tile_task_fail(state, 1);
    return;
  }
  uint4 rect;
  if (msaa_tile_task_rect(bounds[state.Primitive], params, rect)) {
    if (state.X < rect.z && rect.z - state.X > 8) {
      state.X += 8;
      return;
    }
    state.X = rect.x;
    if (state.Y < rect.w && rect.w - state.Y > 8) {
      state.Y += 8;
      return;
    }
  }
  ++state.Primitive;
  state.Positioned = 0;
}

static bool msaa_tile_task_position(device MSAATileTaskState &state,
                                    constant MSAATileTaskParams &params,
                                    device const D3DALLOYMSAATileBounds *bounds,
                                    device const D3DALLOYMSAAPrimitiveDraw *draws) {
  while (state.Primitive < params.MaxPrimitives) {
    uint4 rect;
    const D3DALLOYMSAAPrimitiveDraw draw = draws[state.Primitive];
    if (!draw.index_count || !draw.instance_count ||
        !msaa_tile_task_rect(bounds[state.Primitive], params, rect)) {
      ++state.Primitive;
      state.Positioned = 0;
      continue;
    }
    if (!state.Positioned) {
      state.X = rect.x;
      state.Y = rect.y;
      state.Positioned = 1;
    }
    return true;
  }
  state.Positioned = 0;
  return false;
}

static void msaa_tile_task_fill(device MSAATileTaskSlot &slot,
                                device MSAATileTaskDescriptor &descriptor,
                                device const MSAATileTaskState &state,
                                constant MSAATileTaskParams &params,
                                D3DALLOYMSAATileBounds bound, D3DALLOYMSAAPrimitiveDraw draw) {
  const uint viewport = bound.viewport_index;
  MSAATileTaskControl control = params.Base;
  control.origin_x = state.X;
  control.origin_y = state.Y;
  control.primitive_id = bound.primitive_id;
  control.primitive_slot = state.Primitive;
  control.viewport_min_depth = params.ViewportDepths[viewport][0];
  control.viewport_max_depth = params.ViewportDepths[viewport][1];
  control.viewport_origin_x = params.Viewports[viewport][0];
  control.viewport_origin_y = params.Viewports[viewport][1];
  control.viewport_width = params.Viewports[viewport][2];
  control.viewport_height = params.Viewports[viewport][3];
  control.target_layer = bound.target_layer;
  control.render_width = params.Width;
  control.render_height = params.Height;
  control.scissor_left = min(params.Scissors[viewport][0], params.Width);
  control.scissor_top = min(params.Scissors[viewport][1], params.Height);
  control.scissor_right = min(params.Scissors[viewport][2], params.Width);
  control.scissor_bottom = min(params.Scissors[viewport][3], params.Height);
  control.task_active = 1;
  control.padding = 0;
  for (uint mask = params.ControlMask; mask; mask &= mask - 1) {
    const uint sample = ctz(mask);
    control.sample_base = sample;
    slot.Controls[sample] = control;
  }
  for (uint i = 0; i < 8; ++i) slot.Indexed[i] = 0;
  slot.Indexed[0] = draw.index_count;
  slot.Indexed[1] = draw.instance_count;
  slot.Indexed[2] = draw.index_start;
  slot.Indexed[3] = as_type<uint>(draw.base_vertex);
  slot.Indexed[4] = draw.base_instance;
  for (uint i = 0; i < 4; ++i) {
    slot.Fullscreen[i] = slot.Clear[i] = slot.Probe[i] = 0;
  }
  slot.Fullscreen[0] = 3;
  slot.Fullscreen[1] = 1;
  const uint clear_dwords = max(params.InputDwords, params.OutputDwords);
  slot.Clear[0] = clear_dwords / 64 + uint((clear_dwords % 64) != 0);
  slot.Clear[1] = slot.Clear[2] = 1;
  slot.Probe[0] = slot.Probe[1] = slot.Probe[2] = 1;
  descriptor = {state.X, state.Y, viewport, bound.target_layer};
}

[[kernel]] void msaa_tile_task_prepare(
    device MSAATileTaskState &state [[buffer(0)]],
    constant MSAATileTaskParams &params [[buffer(1)]],
    device const D3DALLOYMSAATileBounds *bounds [[buffer(2)]],
    device const D3DALLOYMSAAPrimitiveDraw *draws [[buffer(3)]],
    device MSAATileTaskCompletion &completion [[buffer(4)]],
    constant uint &mode [[buffer(5)]], uint index [[thread_position_in_grid]]) {
  if (index) return;
  uint budget = kMSAATileFirstQuantum;
  if (mode == kMSAATilePrepare) {
    if (state.Status != kMSAATileRunning || state.BatchCount ||
        state.CompletedTasks || state.Quantum)
      msaa_tile_task_fail(state, 3);
  } else if (mode == kMSAATilePublish) {
    budget = kMSAATileQuantum;
    if (state.Status != kMSAATileError) {
      if (state.BatchCount > kMSAATileQuantum) {
        msaa_tile_task_fail(state, 1);
      } else if (state.CompletedTasks > ~MSAATaskUInt64(0) - state.BatchCount ||
                 state.Quantum == ~MSAATaskUInt64(0)) {
        msaa_tile_task_fail(state, 2);
      } else {
        state.CompletedTasks += state.BatchCount;
        ++state.Quantum;
        state.BatchCount = 0;
      }
    }
  } else {
    msaa_tile_task_fail(state, 3);
  }

  if (state.Status != kMSAATileError) {
    // Preserve primitive -> row -> column order. Only these bounded descriptors
    // cross to the host for native scissor state; original bounds stay private.
    while (state.BatchCount < budget &&
           msaa_tile_task_position(state, params, bounds, draws)) {
      const uint slot = state.BatchCount;
      msaa_tile_task_fill(state.Slots[slot], completion.Descriptors[slot], state, params,
                         bounds[state.Primitive], draws[state.Primitive]);
      ++state.BatchCount;
      msaa_tile_task_advance(state, params, bounds);
      if (state.Status == kMSAATileError) break;
    }
    if (state.Status != kMSAATileError)
      state.Status = state.BatchCount ? kMSAATileRunning : kMSAATileDone;
  }

  // The host encodes exactly ReadyTasks slots after retirement. No padding
  // descriptor, indirect packet or unselected sample control is consumed.
  msaa_tile_task_publish(state, completion);
}

[[kernel]] void msaa_tile_task_clear(
    device const MSAATileTaskSlot &slot [[buffer(0)]],
    constant MSAATileTaskParams &params [[buffer(1)]],
    device uint *inputs [[buffer(2)]], device uint *outputs [[buffer(3)]],
    uint index [[thread_position_in_grid]]) {
  if (!slot.Controls[0].task_active) return;
  if (index < params.InputDwords) inputs[index] = 0;
  if (index < params.OutputDwords) outputs[index] = 0;
}

[[kernel]] void msaa_tile_task_extent(
    constant MSAATileTaskParams &params [[buffer(0)]],
    device const D3DALLOYMSAATileBounds *bounds [[buffer(1)]],
    device MSAATileTaskCompletion &completion [[buffer(2)]],
    uint index [[thread_position_in_grid]]) {
  if (index >= params.MaxPrimitives) return;
  uint4 rect;
  if (!msaa_tile_task_rect(bounds[index], params, rect)) return;
  atomic_fetch_max_explicit(reinterpret_cast<device atomic_uint *>(&completion.Width), rect.z,
                            memory_order_relaxed);
  atomic_fetch_max_explicit(reinterpret_cast<device atomic_uint *>(&completion.Height), rect.w,
                            memory_order_relaxed);
}
