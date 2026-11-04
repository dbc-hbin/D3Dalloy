// SPDX-License-Identifier: LGPL-2.1-or-later
#include <metal_stdlib>
#include "../d3d12/d3d12_work_graph_scheduler.hpp"

using namespace metal;
using namespace d3dalloy;

static void wg_scheduler_fail(device WorkGraphSchedulerState &state, uint error, uint node, uint level) {
  state.Status = kWorkGraphSchedulerError;
  state.Error = error;
  state.ErrorNode = node;
  state.ErrorLevel = level;
  state.SelectedNode = kWorkGraphSchedulerNone;
  state.HostYield = 0;
}

static void wg_scheduler_frame(device WorkGraphSchedulerFrame &frame, uint node, ulong input, uint remaining) {
  frame = WorkGraphSchedulerFrame{};
  frame.Input = input;
  frame.Node = frame.SourceNode = node;
  frame.Remaining = remaining;
  frame.Phase = kWorkGraphSchedulerEnter;
}

// A single invocation owns the frontier. The encoder must publish each helper
// and node dispatch before the next helper. A quantum has a bounded number of
// helper invocations, and this invocation has a bounded number of transitions.
// Queue capacity proves one nonterminal producer group at a time: descendants
// drain before that producer is allowed to reset and reuse its backing level.
kernel void d3d12_work_graph_prepare(device WorkGraphSchedulerState &state [[buffer(0)]],
                                    constant WorkGraphSchedulerParams &p [[buffer(1)]],
                                    uint invocation [[thread_position_in_grid]]) {
  if (invocation) return;
  device auto *indirect = reinterpret_cast<device WorkGraphSchedulerIndirect *>(p.Indirect);
  if (state.PendingPacket < p.NodeCount) {
    indirect[state.PendingPacket] = WorkGraphSchedulerIndirect{};
    state.PendingPacket = kWorkGraphSchedulerNone;
  }
  if (state.Status != kWorkGraphSchedulerRunning || state.HostYield) return;

  state.SelectedNode = kWorkGraphSchedulerNone;
  state.InputAddress = 0;
  if (!p.Depth || p.Depth > kWorkGraphSchedulerMaxDepth || state.Depth > p.Depth ||
      !p.TransitionBudget || p.TransitionBudget > kWorkGraphSchedulerMaxTransitions ||
      !p.GroupBudget || p.GroupBudget > kWorkGraphSchedulerMaxGroups || state.QuantumGroups > p.GroupBudget) {
    wg_scheduler_fail(state, kWorkGraphSchedulerInvalidMetadata, kWorkGraphSchedulerNone, state.Depth);
    return;
  }
  // Slots remaining in a preencoded quantum become no-ops once its group
  // budget is reached. The CPU resets only this budget after GPU retirement;
  // all traversal state and suspended producer queues remain GPU-owned.
  if (state.QuantumGroups == p.GroupBudget) return;
  device const auto *nodes = reinterpret_cast<device const WorkGraphSchedulerNode *>(p.Nodes);
  device const auto *templates = reinterpret_cast<device const WorkGraphSchedulerQueueTemplate *>(p.Queues);
  device const auto *seeds = reinterpret_cast<device const WorkGraphSchedulerSeed *>(p.Seeds);
  device const auto *targets = reinterpret_cast<device const uint *>(p.Targets);
  device const auto *shares = reinterpret_cast<device const uint *>(p.Shares);
  device auto *frames = reinterpret_cast<device WorkGraphSchedulerFrame *>(p.Frames);

  for (uint transition = 0; transition < p.TransitionBudget; ++transition) {
    ++state.TransitionSteps;
    if (!state.Depth) {
      if (state.SeedIndex >= p.SeedCount) {
        state.Status = kWorkGraphSchedulerDone;
        return;
      }
      const auto seed = seeds[state.SeedIndex];
      if (state.SeedRecord >= seed.Count) {
        ++state.SeedIndex;
        state.SeedRecord = 0;
        continue;
      }
      if (seed.Node >= p.NodeCount) {
        wg_scheduler_fail(state, kWorkGraphSchedulerInvalidNode, seed.Node, 0);
        return;
      }
      // r and each stride limb are uint32. Both products and their sum fit
      // uint64. Check the upper limb without a 64-bit divide, which fails
      // native Metal pipeline compilation on the validated runtime.
      const ulong low_product = ulong(state.SeedRecord) * uint(seed.Stride);
      const ulong high_product = ulong(state.SeedRecord) * uint(seed.Stride >> 32);
      if (high_product + (low_product >> 32) > 0xfffffffful) {
        wg_scheduler_fail(state, kWorkGraphSchedulerInvalidRecord, seed.Node, 0);
        return;
      }
      const ulong offset = ulong(state.SeedRecord) * seed.Stride;
      if (seed.Records > (~0ul) - offset) {
        wg_scheduler_fail(state, kWorkGraphSchedulerInvalidRecord, seed.Node, 0);
        return;
      }
      wg_scheduler_frame(frames[0], seed.Node, seed.Records + offset, nodes[seed.Node].Recursion);
      ++state.SeedRecord;
      state.Depth = 1;
      state.PeakDepth = max(state.PeakDepth, 1u);
      continue;
    }

    const uint level = state.Depth - 1;
    device auto &frame = frames[level];
    if (frame.Node >= p.NodeCount || frame.SourceNode >= p.NodeCount) {
      wg_scheduler_fail(state, kWorkGraphSchedulerInvalidNode, frame.Node, level);
      return;
    }
    device const auto &node = nodes[frame.Node];
    const ulong level_address = p.Levels + ulong(level) * p.LevelStride;
    device auto *queues = reinterpret_cast<device WorkGraphSchedulerQueue *>(level_address);
    device auto *batch = reinterpret_cast<device WorkGraphSchedulerBatch *>(level_address + p.BatchOffset);
    device auto *sharing = reinterpret_cast<device uint *>(level_address + p.SharingOffset);

    if (frame.Phase == kWorkGraphSchedulerEnter) {
      if (node.Launch < 1 || node.Launch > 3 || !node.GroupLimit || node.GroupLimit > 65535u ||
          (!(node.Flags & kWorkGraphSchedulerTerminal) && node.GroupLimit != 1) ||
          node.TargetCount > p.MaxOutputs || node.ShareCount > 255u) {
        wg_scheduler_fail(state, kWorkGraphSchedulerInvalidMetadata, frame.Node, level);
        return;
      }
      uint grid[3] = {1, 1, 1};
      if (node.Launch == 1) {
        for (uint axis = 0; axis < 3; ++axis) grid[axis] = node.Grid[axis];
        if (!(node.Flags & kWorkGraphSchedulerFixedGrid) && node.DynamicComponents) {
          const uint components = node.DynamicComponents, bytes = node.DynamicBytes;
          if (components > 3 || (bytes != 2 && bytes != 4) || !frame.Input ||
              node.DynamicOffset > node.RecordSize || components * bytes > node.RecordSize - node.DynamicOffset) {
            wg_scheduler_fail(state, kWorkGraphSchedulerInvalidRecord, frame.Node, level);
            return;
          }
          device const auto *metadata = reinterpret_cast<device const uchar *>(frame.Input + node.DynamicOffset);
          for (uint axis = 0; axis < 3; ++axis) {
            grid[axis] = 1;
            if (axis < components) {
              grid[axis] = 0;
              for (uint byte = 0; byte < bytes; ++byte)
                grid[axis] |= uint(metadata[axis * bytes + byte]) << (8 * byte);
            }
          }
        }
        const bool has_max = node.MaxGrid[0] || node.MaxGrid[1] || node.MaxGrid[2];
        for (uint axis = 0; axis < 3; ++axis) {
          if (grid[axis] > 65535u || (has_max && grid[axis] > node.MaxGrid[axis])) {
            wg_scheduler_fail(state, kWorkGraphSchedulerInvalidGrid, frame.Node, level);
            return;
          }
        }
      }
      const ulong groups = ulong(grid[0]) * grid[1] * grid[2];
      if (groups > 0xfffffful) {
        wg_scheduler_fail(state, kWorkGraphSchedulerInvalidGrid, frame.Node, level);
        return;
      }
      frame.GroupCount = uint(groups);
      frame.NextGroup = frame.NextTarget = frame.NextRecord = 0;
      for (uint axis = 0; axis < 3; ++axis) frame.Grid[axis] = grid[axis];
      *sharing = uint(groups);
      batch->GroupEnd = uint(groups);
      for (uint axis = 0; axis < 3; ++axis) batch->Grid[axis] = grid[axis];
      // Sharing peers receive the same input even if this node's grid is zero;
      // a peer may use a different fixed grid or interpret SV_DispatchGrid as data.
      frame.Phase = groups ? kWorkGraphSchedulerReady : kWorkGraphSchedulerShared;
      continue;
    }

    if (frame.Phase == kWorkGraphSchedulerReady) {
      if (frame.NextGroup >= frame.GroupCount) {
        wg_scheduler_fail(state, kWorkGraphSchedulerInvalidState, frame.Node, level);
        return;
      }
      const uint groups = min(min(frame.GroupCount - frame.NextGroup, node.GroupLimit),
                              p.GroupBudget - state.QuantumGroups);
      for (uint i = 0; i < node.DrainCount; ++i) {
        const uint target = targets[ulong(node.DrainOffset) + i];
        const auto description = templates[target];
        if (description.Mask == 0xffffffffu || ((description.Mask + 1) & description.Mask) || !description.Stride) {
          wg_scheduler_fail(state, kWorkGraphSchedulerInvalidMetadata, frame.Node, level);
          return;
        }
        queues[target] = WorkGraphSchedulerQueue{
          p.Backing + ulong(level) * p.LevelBytes + description.Offset, 0,
          description.Mask, description.Stride, 0, 0};
      }
      queues[p.NodeCount] = WorkGraphSchedulerQueue{
        frame.Input, level_address + p.SharingOffset, 0, node.RecordSize, 0, 0};
      device auto *context = reinterpret_cast<device WorkGraphSchedulerContext *>(p.Context);
      *context = WorkGraphSchedulerContext{
        level_address, level_address + p.BatchOffset, node.LocalRoot,
        p.NodeCount, 0, 1, groups, frame.NextGroup, frame.Remaining, node.TargetCount, 0};
      device auto *outputs = reinterpret_cast<device uint *>(p.Context + sizeof(WorkGraphSchedulerContext));
      for (uint element = 0; element < node.TargetCount; ++element) {
        const uint target = targets[ulong(node.TargetOffset) + element];
        if (target != kWorkGraphSchedulerNone && target >= p.NodeCount) {
          wg_scheduler_fail(state, kWorkGraphSchedulerInvalidNode, frame.Node, level);
          return;
        }
        outputs[element] = target == frame.Node && !frame.Remaining ? kWorkGraphSchedulerNone : target;
      }
      frame.NextGroup += groups;
      frame.NextTarget = frame.NextRecord = 0;
      frame.Phase = kWorkGraphSchedulerDrain;
      state.SelectedNode = frame.Node;
      state.SelectedLevel = level;
      state.InputAddress = frame.Input;
      ++state.NodeSteps;
      state.QuantumGroups += groups;
      if (!(node.Flags & kWorkGraphSchedulerTerminal)) ++state.NonterminalSteps;
      if (node.Flags & kWorkGraphSchedulerHostRequired) {
        // The CPU services this prepared batch but never traverses queues. It
        // clears HostYield only after export/publication actually retires.
        state.HostYield = 1;
        ++state.HostSteps;
      } else {
        indirect[frame.Node] = WorkGraphSchedulerIndirect{groups, 1, 1, 0};
        state.PendingPacket = frame.Node;
      }
      return;
    }

    if (frame.Phase == kWorkGraphSchedulerDrain) {
      if (frame.NextTarget < node.DrainCount) {
        const uint target = targets[ulong(node.DrainOffset) + frame.NextTarget];
        device auto &queue = queues[target];
        const uint count = atomic_load_explicit(reinterpret_cast<device atomic_uint *>(&queue.Tail), memory_order_relaxed);
        if (ulong(count) > ulong(templates[target].Mask) + 1) {
          wg_scheduler_fail(state, kWorkGraphSchedulerQueueOverflow, frame.Node, level);
          return;
        }
        if (frame.NextRecord < count) {
          if (state.Depth >= p.Depth) {
            wg_scheduler_fail(state, kWorkGraphSchedulerStackOverflow, frame.Node, level);
            return;
          }
          if (target == frame.Node && !frame.Remaining) {
            wg_scheduler_fail(state, kWorkGraphSchedulerRecursionUnderflow, frame.Node, level);
            return;
          }
          const uint remaining = target == frame.Node ? frame.Remaining - 1 : nodes[target].Recursion;
          const ulong input = queue.Records + ulong(frame.NextRecord) * queue.Stride;
          ++frame.NextRecord;
          wg_scheduler_frame(frames[state.Depth], target, input, remaining);
          ++state.Depth;
          state.PeakDepth = max(state.PeakDepth, state.Depth);
          continue;
        }
        ++frame.NextTarget;
        frame.NextRecord = 0;
        continue;
      }
      frame.Phase = frame.NextGroup < frame.GroupCount ? kWorkGraphSchedulerReady : kWorkGraphSchedulerShared;
      continue;
    }

    if (frame.Phase == kWorkGraphSchedulerShared) {
      device const auto &source = nodes[frame.SourceNode];
      if (frame.NextShared < source.ShareCount) {
        const uint shared = shares[ulong(source.ShareOffset) + frame.NextShared];
        ++frame.NextShared;
        if (shared >= p.NodeCount) {
          wg_scheduler_fail(state, kWorkGraphSchedulerInvalidNode, shared, level);
          return;
        }
        frame.Node = shared;
        frame.Remaining = 0;
        frame.Phase = kWorkGraphSchedulerEnter;
      } else {
        --state.Depth;
      }
      continue;
    }

    wg_scheduler_fail(state, kWorkGraphSchedulerInvalidState, frame.Node, level);
    return;
  }
  // Only scheduler transitions exhausted this invocation. The GPU frontier is
  // intact; a later slot or quantum continues without a CPU queue-tail read.
}
