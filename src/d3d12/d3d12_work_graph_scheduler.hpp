// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The scheduler and its Metal helper share only fixed-width PODs. Addresses
// point into allocations retained by GraphExecution until actual retirement.
#ifdef __METAL_VERSION__
#include <metal_stdlib>
#include <metal_command_buffer>
#else
#include <cstddef>
#include <cstdint>
#include <type_traits>
#endif

namespace d3dalloy {

#ifdef __METAL_VERSION__
using WorkGraphSchedulerUInt = uint;
using WorkGraphSchedulerAddress = ulong;
#else
using WorkGraphSchedulerUInt = uint32_t;
using WorkGraphSchedulerAddress = uint64_t;
#endif

enum WorkGraphSchedulerStatus : WorkGraphSchedulerUInt {
  kWorkGraphSchedulerRunning = 0,
  kWorkGraphSchedulerDone = 1,
  kWorkGraphSchedulerError = 2,
};

enum WorkGraphSchedulerError : WorkGraphSchedulerUInt {
  kWorkGraphSchedulerNoError = 0,
  kWorkGraphSchedulerInvalidState = 1,
  kWorkGraphSchedulerInvalidNode = 2,
  kWorkGraphSchedulerInvalidGrid = 3,
  kWorkGraphSchedulerQueueOverflow = 4,
  kWorkGraphSchedulerStackOverflow = 5,
  kWorkGraphSchedulerInvalidRecord = 6,
  kWorkGraphSchedulerInvalidMetadata = 7,
  kWorkGraphSchedulerRecursionUnderflow = 8,
};

enum WorkGraphSchedulerNodeFlags : WorkGraphSchedulerUInt {
  kWorkGraphSchedulerFixedGrid = 1u,
  kWorkGraphSchedulerHostRequired = 2u,
  kWorkGraphSchedulerTerminal = 4u,
};

enum WorkGraphSchedulerFramePhase : WorkGraphSchedulerUInt {
  kWorkGraphSchedulerEnter = 0,
  kWorkGraphSchedulerReady = 1,
  kWorkGraphSchedulerDrain = 2,
  kWorkGraphSchedulerShared = 3,
};

enum : WorkGraphSchedulerUInt {
  kWorkGraphSchedulerNone = 0xffffffffu,
  kWorkGraphSchedulerMaxDepth = 32u,
  kWorkGraphSchedulerMaxTransitions = 256u,
  kWorkGraphSchedulerQuantum = 64u,
  kWorkGraphSchedulerMaxGroups = 65535u,
};

struct WorkGraphSchedulerParams {
  WorkGraphSchedulerAddress Nodes;
  WorkGraphSchedulerAddress Queues; // WorkGraphSchedulerQueueTemplate[NodeCount].
  WorkGraphSchedulerAddress Targets;
  WorkGraphSchedulerAddress Shares;
  WorkGraphSchedulerAddress Seeds;
  WorkGraphSchedulerAddress Frames;
  WorkGraphSchedulerAddress Levels; // Each level starts with NodeCount + 1 queues.
  WorkGraphSchedulerAddress Context; // One fixed active context, including Outputs[].
  WorkGraphSchedulerAddress Indirect;
  WorkGraphSchedulerAddress Backing;
  WorkGraphSchedulerAddress LevelBytes; // Record backing bytes per suspended producer.
  WorkGraphSchedulerAddress LevelStride; // Control bytes per suspended producer.
  WorkGraphSchedulerUInt NodeCount;
  WorkGraphSchedulerUInt Depth;
  WorkGraphSchedulerUInt BatchOffset;
  WorkGraphSchedulerUInt SharingOffset;
#ifdef __METAL_VERSION__
  metal::command_buffer Commands;
#else
  uint64_t Commands;
#endif
  WorkGraphSchedulerAddress Roots, Samplers;
};

struct WorkGraphSchedulerNode {
  WorkGraphSchedulerAddress LocalRoot;
  WorkGraphSchedulerUInt Flags;
  WorkGraphSchedulerUInt Launch; // SM50_WG_LAUNCH.
  WorkGraphSchedulerUInt Grid[3];
  WorkGraphSchedulerUInt MaxGrid[3];
  WorkGraphSchedulerUInt DynamicOffset;
  WorkGraphSchedulerUInt DynamicBytes; // 2 or 4 for record-supplied grids.
  WorkGraphSchedulerUInt DynamicComponents;
  WorkGraphSchedulerUInt RecordSize;
  WorkGraphSchedulerUInt Recursion;
  WorkGraphSchedulerUInt TargetOffset;
  WorkGraphSchedulerUInt TargetCount;
  WorkGraphSchedulerUInt ShareOffset;
  WorkGraphSchedulerUInt ShareCount;
  WorkGraphSchedulerUInt GroupLimit; // 1 nonterminal, 65535 terminal, 32 atomic64 terminal.
  WorkGraphSchedulerUInt DrainOffset, DrainCount; // Sorted distinct live targets in Targets[].
  WorkGraphSchedulerUInt Reserved[2];
#ifdef __METAL_VERSION__
  metal::compute_pipeline_state Pipeline;
#else
  uint64_t Pipeline;
#endif
  WorkGraphSchedulerAddress Constants;
  WorkGraphSchedulerUInt Threads[3];
};

struct WorkGraphSchedulerQueueTemplate {
  WorkGraphSchedulerAddress Offset;
  WorkGraphSchedulerUInt Mask;
  WorkGraphSchedulerUInt Stride;
};

struct WorkGraphSchedulerSeed {
  WorkGraphSchedulerAddress Records;
  WorkGraphSchedulerAddress Stride;
  WorkGraphSchedulerUInt Count;
  WorkGraphSchedulerUInt Node;
};

// A frame owns one backing level until all of its groups, descendants and
// ShareInputOf peers finish. Peers replace Node in this frame, not its level.
struct WorkGraphSchedulerFrame {
  WorkGraphSchedulerAddress Input;
  WorkGraphSchedulerUInt Node;
  WorkGraphSchedulerUInt SourceNode;
  WorkGraphSchedulerUInt Remaining;
  WorkGraphSchedulerUInt Phase;
  WorkGraphSchedulerUInt Grid[3];
  WorkGraphSchedulerUInt GroupCount;
  WorkGraphSchedulerUInt NextGroup;
  WorkGraphSchedulerUInt NextTarget;
  WorkGraphSchedulerUInt NextRecord;
  WorkGraphSchedulerUInt NextShared;
  WorkGraphSchedulerUInt Reserved[2];
};

// Metal dispatchThreadgroups indirect arguments occupy the first twelve bytes.
struct WorkGraphSchedulerIndirect {
  WorkGraphSchedulerUInt X, Y, Z, Reserved;
};

struct WorkGraphSchedulerState {
  WorkGraphSchedulerUInt Status;
  WorkGraphSchedulerUInt Error;
  WorkGraphSchedulerUInt Depth; // Number of live frames, zero when selecting a seed.
  WorkGraphSchedulerUInt SeedRecord;
  WorkGraphSchedulerUInt SelectedNode;
  WorkGraphSchedulerUInt SelectedLevel;
  WorkGraphSchedulerUInt HostYield;
  WorkGraphSchedulerAddress NodeSteps;
  WorkGraphSchedulerAddress NonterminalSteps;
  WorkGraphSchedulerAddress HostSteps;
  WorkGraphSchedulerAddress InputAddress;
  WorkGraphSchedulerUInt PeakDepth;
  WorkGraphSchedulerUInt ErrorNode;
  WorkGraphSchedulerUInt ErrorLevel;
  // Only the CPU resets this counter, after the previous quantum actually retires.
  WorkGraphSchedulerUInt QuantumGroups;
  WorkGraphSchedulerUInt PendingPacket; // The only nonzero native indirect packet.
  WorkGraphSchedulerUInt Reserved;
};

// These three layouts match the existing SM50_WG_* runtime ABI. Tail and the
// separate sharing word are accessed atomically by compiled node helpers.
struct WorkGraphSchedulerQueue {
  WorkGraphSchedulerAddress Records, Sharing;
  WorkGraphSchedulerUInt Mask, Stride, Tail, Reserved;
};

struct WorkGraphSchedulerBatch {
  WorkGraphSchedulerUInt GroupEnd, Grid[3];
};

struct WorkGraphSchedulerContext {
  WorkGraphSchedulerAddress QueueTable, BatchTable, LocalRootArguments;
  WorkGraphSchedulerUInt InputQueue, BatchStart, BatchRecords, BatchGroups;
  WorkGraphSchedulerUInt GroupStart, RemainingRecursionLevels, OutputElementCount, Reserved;
  // OutputElementCount uint32 values immediately follow this fixed header.
};

#ifndef __METAL_VERSION__
static_assert(sizeof(WorkGraphSchedulerParams) == 136 && offsetof(WorkGraphSchedulerParams, Commands) == 112);
static_assert(offsetof(WorkGraphSchedulerParams, LevelStride) == 88);
static_assert(offsetof(WorkGraphSchedulerParams, NodeCount) == 96);
static_assert(offsetof(WorkGraphSchedulerParams, SharingOffset) == 108);
static_assert(sizeof(WorkGraphSchedulerNode) == 128 && offsetof(WorkGraphSchedulerNode, Pipeline) == 96);
static_assert(offsetof(WorkGraphSchedulerNode, Grid) == 16);
static_assert(offsetof(WorkGraphSchedulerNode, DynamicOffset) == 40);
static_assert(offsetof(WorkGraphSchedulerNode, TargetOffset) == 60);
static_assert(offsetof(WorkGraphSchedulerNode, GroupLimit) == 76);
static_assert(sizeof(WorkGraphSchedulerQueueTemplate) == 16);
static_assert(sizeof(WorkGraphSchedulerSeed) == 24);
static_assert(sizeof(WorkGraphSchedulerFrame) == 64 && alignof(WorkGraphSchedulerFrame) == 8);
static_assert(offsetof(WorkGraphSchedulerFrame, Grid) == 24);
static_assert(offsetof(WorkGraphSchedulerFrame, NextGroup) == 40);
static_assert(sizeof(WorkGraphSchedulerIndirect) == 16);
static_assert(sizeof(WorkGraphSchedulerState) == 88 && alignof(WorkGraphSchedulerState) == 8);
static_assert(offsetof(WorkGraphSchedulerState, NodeSteps) == 32);
static_assert(offsetof(WorkGraphSchedulerState, InputAddress) == 56);
static_assert(offsetof(WorkGraphSchedulerState, PeakDepth) == 64);
static_assert(offsetof(WorkGraphSchedulerState, QuantumGroups) == 76);
static_assert(sizeof(WorkGraphSchedulerQueue) == 32 && offsetof(WorkGraphSchedulerQueue, Tail) == 24);
static_assert(sizeof(WorkGraphSchedulerBatch) == 16);
static_assert(sizeof(WorkGraphSchedulerContext) == 56);
static_assert(offsetof(WorkGraphSchedulerContext, InputQueue) == 24);
static_assert(offsetof(WorkGraphSchedulerContext, GroupStart) == 40);
static_assert(offsetof(WorkGraphSchedulerContext, OutputElementCount) == 48);
static_assert(std::is_trivially_copyable_v<WorkGraphSchedulerParams>);
static_assert(std::is_trivially_copyable_v<WorkGraphSchedulerNode>);
static_assert(std::is_trivially_copyable_v<WorkGraphSchedulerFrame>);
static_assert(std::is_trivially_copyable_v<WorkGraphSchedulerState>);
#endif

} // namespace d3dalloy
