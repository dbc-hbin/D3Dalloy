// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(__METAL_VERSION__)
using MSAATaskUInt = uint;
using MSAATaskUInt64 = ulong;
#else
#include <cstdint>
using MSAATaskUInt = uint32_t;
using MSAATaskUInt64 = uint64_t;
#endif

namespace d3dalloy {

enum : MSAATaskUInt {
  kMSAATileFirstQuantum = 1,
  kMSAATileQuantum = 64,
  kMSAATileRunning = 0,
  kMSAATileDone = 1,
  kMSAATileError = 2,
  kMSAATilePrepare = 0,
  kMSAATilePublish = 1,
};

// The first 172 bytes mirror SM50_MSAA_TILE_CONTROL. Keep each sample's
// immutable control aligned to 16 bytes; native passes bind a fixed offset.
struct MSAATileTaskControl {
  MSAATaskUInt origin_x, origin_y, sample_count, sample_mask;
  float viewport_min_depth, viewport_max_depth;
  MSAATaskUInt primitive_id, primitive_slot;
  float viewport_origin_x, viewport_origin_y, viewport_width, viewport_height;
  MSAATaskUInt output_mask, sample_base, shading_rate;
  MSAATaskUInt shading_rate_image_width, shading_rate_image_height;
  MSAATaskUInt programmable_sample_count;
  float programmable_sample_positions[8][2];
  MSAATaskUInt depth_bounds_enabled;
  MSAATaskUInt target_layer, render_width, render_height;
  MSAATaskUInt scissor_left, scissor_top, scissor_right, scissor_bottom, task_active;
  MSAATaskUInt padding;
};

struct MSAATileTaskParams {
  MSAATaskUInt MaxPrimitives, Width, Height, Layers;
  MSAATaskUInt InputDwords, OutputDwords, ViewportCount, ControlMask;
  MSAATileTaskControl Base;
  float ViewportDepths[16][2];
  float Viewports[16][4];
  MSAATaskUInt Scissors[16][4];
};

struct MSAATileTaskDescriptor {
  MSAATaskUInt X, Y, Viewport, Layer;
};

// Only this bounded mailbox is read by the host after actual GPU retirement.
// Width/Height are used solely for attachmentless native texture allocation.
// ReadyTasks describes the prepared immutable slots; CompletedTasks advances
// only after the preceding batch's native consumers have finished.
struct MSAATileTaskCompletion {
  MSAATaskUInt Status, Error, Width, Height;
  MSAATaskUInt64 CompletedTasks, Quantum;
  MSAATaskUInt Primitive, ReadyTasks, Reserved[6];
  MSAATileTaskDescriptor Descriptors[kMSAATileQuantum];
};

struct MSAATileTaskSlot {
  MSAATaskUInt Indexed[8];
  MSAATaskUInt Fullscreen[4];
  MSAATaskUInt Clear[4];
  MSAATaskUInt Probe[4];
  MSAATileTaskControl Controls[16];
};

struct MSAATileTaskState {
  MSAATaskUInt Primitive, X, Y, Positioned;
  MSAATaskUInt BatchCount, Status, Error, Reserved;
  MSAATaskUInt64 CompletedTasks, Quantum;
  MSAATileTaskSlot Slots[kMSAATileQuantum];
};

static_assert(sizeof(MSAATileTaskControl) == 176);
static_assert(sizeof(MSAATileTaskParams) == 848);
static_assert(sizeof(MSAATileTaskDescriptor) == 16);
static_assert(sizeof(MSAATileTaskCompletion) == 1088);
static_assert(sizeof(MSAATileTaskSlot) == 2896);
static_assert(sizeof(MSAATileTaskState) == 185392);
static_assert(__builtin_offsetof(MSAATileTaskControl, target_layer) == 140);
static_assert(__builtin_offsetof(MSAATileTaskControl, task_active) == 168);
static_assert(__builtin_offsetof(MSAATileTaskParams, Base) == 32);
static_assert(__builtin_offsetof(MSAATileTaskState, BatchCount) == 16);
static_assert(__builtin_offsetof(MSAATileTaskState, CompletedTasks) == 32);
static_assert(__builtin_offsetof(MSAATileTaskState, Quantum) == 40);
static_assert(__builtin_offsetof(MSAATileTaskState, Slots) == 48);
static_assert(__builtin_offsetof(MSAATileTaskSlot, Indexed) == 0);
static_assert(__builtin_offsetof(MSAATileTaskSlot, Fullscreen) == 32);
static_assert(__builtin_offsetof(MSAATileTaskSlot, Clear) == 48);
static_assert(__builtin_offsetof(MSAATileTaskSlot, Probe) == 64);
static_assert(__builtin_offsetof(MSAATileTaskSlot, Controls) == 80);
static_assert(__builtin_offsetof(MSAATileTaskCompletion, CompletedTasks) == 16);
static_assert(__builtin_offsetof(MSAATileTaskCompletion, ReadyTasks) == 36);
static_assert(__builtin_offsetof(MSAATileTaskCompletion, Descriptors) == 64);

} // namespace d3dalloy
