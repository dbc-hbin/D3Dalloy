// SPDX-License-Identifier: LGPL-2.1-or-later
// Included after the SO/MSAA transport definitions in d3dalloy_command.metal.
#include "../d3d12/d3d12_capture_frontier.hpp"

using namespace d3dalloy;

static void capture_frontier_publish(device CaptureFrontierState &s, constant CaptureFrontierParams &p) {
  device auto &out = *reinterpret_cast<device CaptureFrontierCompletion *>(p.Completion);
  out.Status = s.Status;
  out.Error = s.Error;
  out.HostReason = s.HostReason;
  out.Command = s.Command;
  out.ProducerSteps = s.ProducerSteps;
  out.ConsumerSteps = s.ConsumerSteps;
  out.CommandsCompleted = s.CommandsCompleted;
  out.Quantum = s.Quantum;
  out.ParentSteps = s.ParentSteps;
  out.ChildSteps = s.ChildSteps;
}

static void capture_frontier_fail(device CaptureFrontierState &s, constant CaptureFrontierParams &p, uint error) {
  s.Status = kCaptureFrontierError;
  s.Error = error;
  s.Indirect = CaptureFrontierIndirect{};
  capture_frontier_publish(s, p);
}

static void capture_frontier_copy(ulong destination, ulong source, uint bytes) {
  device uchar *dst = reinterpret_cast<device uchar *>(destination);
  device const uchar *src = reinterpret_cast<device const uchar *>(source);
  for (uint i = 0; i < bytes; ++i) dst[i] = src[i];
}

static uint capture_frontier_index(device const CaptureFrontierIndexView &view, uint position) {
  const uint width = view.Format == 42 ? 4 : 2;
  const uint offset = (view.FirstIndex + position) * width;
  if (!view.Address || (view.Format != 42 && view.Format != 57) || view.Size < width || offset > view.Size - width)
    return 0;
  device const uchar *source = reinterpret_cast<device const uchar *>(view.Address + ulong(offset));
  const uint low = uint(source[0]) | uint(source[1]) << 8;
  return width == 4 ? low | uint(source[2]) << 16 | uint(source[3]) << 24 : low;
}

static bool capture_frontier_cut(device const CaptureFrontierState &s, constant CaptureFrontierParams &p,
                                 uint position) {
  if (!(p.Flags & kCaptureFrontierIndexed)) return false;
  const bool wide = s.OriginalIndex.Format == 42 && p.StripCut == 2;
  const bool narrow = s.OriginalIndex.Format == 57 && p.StripCut == 1;
  return (wide || narrow) && capture_frontier_index(s.OriginalIndex, position) == (wide ? ~0u : 0xffffu);
}

static uint capture_frontier_primitives(uint topology, uint vertices) {
  switch (topology) {
  case 1: return vertices;
  case 2: return vertices / 2;
  case 3: return vertices > 1 ? vertices - 1 : 0;
  case 4: return vertices / 3;
  case 5: case 6: return vertices > 2 ? vertices - 2 : 0;
  case 10: return vertices / 4;
  case 11: return vertices > 3 ? vertices - 3 : 0;
  case 12: return vertices / 6;
  default: return vertices > 5 ? (vertices - 4) / 2 : 0;
  }
}

static void capture_frontier_end_instance(device CaptureFrontierState &s) {
  ++s.Instance;
  s.InputPosition = s.ObjectGroup = s.ChildPrimitive = s.ParentPrimitives = s.ParentSlots = 0;
  s.CarryCount = s.WindowCount = s.StripLength = s.StripStart = 0;
  s.PrimitivePrefix = s.ParentPrimitivePrefix = 0;
  s.InstanceDone = 0;
  if (s.Instance == s.InstanceCount) {
    ++s.Command;
    ++s.CommandsCompleted;
    s.CommandReady = s.AtomicReady = 0;
  }
}

static void capture_frontier_finish_batch(device CaptureFrontierState &s, constant CaptureFrontierParams &p) {
  if (!(s.BatchActive & 1)) return;
  if (p.Kind == kCaptureFrontierVertex) {
    // Preserve shaded records, including a fan's anchor, before the bounded
    // capture is reused. This is transport only; no application stage is replayed.
    device uint *carry = reinterpret_cast<device uint *>(p.CarryScratch);
    device uint *vertices = reinterpret_cast<device uint *>(p.CarryVertexRecords);
    device const uint *scratch = reinterpret_cast<device const uint *>(p.Scratch);
    device const uint *records = reinterpret_cast<device const uint *>(p.VertexRecords);
    for (uint i = 0; i < s.WindowCount; ++i) {
      const uint source = s.Window[i];
      for (uint word = 0; word < p.SORecordDwords; ++word)
        carry[ulong(i) * p.SORecordDwords + word] = scratch[ulong(source) * p.SORecordDwords + word];
      for (uint word = 0; word < p.VertexRecordDwords; ++word)
        vertices[ulong(i) * p.VertexRecordDwords + word] = records[ulong(source) * p.VertexRecordDwords + word];
      s.Window[i] = i;
    }
    s.CarryCount = s.WindowCount;
    s.InputPosition = s.NextInputPosition;
    s.PrimitivePrefix += s.PrimitiveCount;
  } else if (p.Kind == kCaptureFrontierTessellationGeometry) {
    s.ChildPrimitive = s.NextChildPrimitive;
    s.PrimitivePrefix += s.Shader.local_primitive_count;
    if (s.ChildPrimitive == s.ParentPrimitives) {
      s.ObjectGroup = s.NextObjectGroup;
      s.ChildPrimitive = s.ParentPrimitives = s.ParentSlots = 0;
      const uint patches = s.VertexCount / p.ControlPoints;
      s.InstanceDone = s.ObjectGroup >= (patches - 1) / p.PatchesPerGroup + 1;
    }
  } else if (p.Flags & kCaptureFrontierExpandStrip) {
    s.InputPosition = s.NextInputPosition;
    s.PrimitivePrefix += s.Shader.local_primitive_count;
  } else {
    s.ObjectGroup = s.NextObjectGroup;
    s.PrimitivePrefix += s.Shader.local_primitive_count;
  }
  ++s.ConsumerSteps;
  s.BatchActive = 0;
  s.Phase = kCaptureFrontierProducer;
  if (s.InstanceDone) capture_frontier_end_instance(s);
  if (s.Command >= s.EffectiveCount) s.Status = kCaptureFrontierDone;
}

static void capture_frontier_query_epoch(device CaptureFrontierState &s, constant CaptureFrontierParams &p) {
  if (!p.StatisticsClaim) return;
  if (++s.QueryEpoch <= 1) {
    s.QueryEpoch = 1;
    s.BatchActive |= 4;
  }
  device uint *header = reinterpret_cast<device uint *>(p.StatisticsClaim);
  header[3] = s.QueryEpoch;
  header[5] = 0;
}

static void capture_frontier_command(device CaptureFrontierState &s, constant CaptureFrontierParams &p) {
  device const uint *source = reinterpret_cast<device const uint *>(
      p.Arguments + ulong(s.Command) * p.ArgumentStride + p.DrawOffset);
  const bool indexed = p.Flags & kCaptureFrontierIndexed;
  s.VertexCount = source[0];
  s.InstanceCount = source[1];
  s.StartLocation = source[2];
  s.BaseVertex = indexed ? source[3] : source[2];
  s.StartInstance = source[indexed ? 4 : 3];
  for (uint i = 0; i < 8; ++i) s.DrawArguments[i] = i < (indexed ? 5u : 4u) ? source[i] : 0;
  s.CommandReady = 1;
  // Empty commands do not consume bindings. In particular, their unused IBV
  // or root bytes may be poison, and must not be read or validated.
  if (!s.VertexCount || !s.InstanceCount ||
      ((p.Kind == kCaptureFrontierDomain || p.Kind == kCaptureFrontierTessellationGeometry) &&
       s.VertexCount < p.ControlPoints)) return;
  s.OriginalIndex = CaptureFrontierIndexView{};
  if (indexed) {
    if (p.StagedIndexViews) {
      device const auto *view = reinterpret_cast<device const CaptureFrontierIndexView *>(
          p.StagedIndexViews + ulong(s.Command) * p.IndexViewStride);
      s.OriginalIndex = *view;
    } else {
      s.OriginalIndex.Address = p.FixedIndexAddress;
      s.OriginalIndex.Size = p.FixedIndexSize;
      s.OriginalIndex.Format = p.FixedIndexFormat;
    }
    s.OriginalIndex.FirstIndex = s.StartLocation;
    if (s.OriginalIndex.Address && s.OriginalIndex.Format != 42 && s.OriginalIndex.Format != 57) {
      capture_frontier_fail(s, p, kCaptureFrontierInvalidIndexView);
      return;
    }
  }
  if (p.RootBytes)
    capture_frontier_copy(p.ActiveRoots, p.StagedRoots + ulong(s.Command) * p.RootStride, p.RootBytes);
  if (p.VertexTableBytes)
    capture_frontier_copy(p.ActiveVertices, p.StagedVertices + ulong(s.Command) * p.VertexStride, p.VertexTableBytes);
  s.ActiveIndex = s.OriginalIndex;
  s.Instance = s.InputPosition = s.ObjectGroup = s.ChildPrimitive = s.ParentPrimitives = 0;
  s.CarryCount = s.WindowCount = s.StripLength = s.StripStart = s.SOStoppedMask = 0;
  s.PrimitivePrefix = s.ParentPrimitivePrefix = 0;
  s.CommandReady = 1;
}

// Expand only the next bounded input prefix. Prefix/cut state survives empty
// batches, and triangle adjacency only reads its two-index lookahead.
static uint capture_frontier_expand(device CaptureFrontierState &s, constant CaptureFrontierParams &p) {
  device uint *list = reinterpret_cast<device uint *>(p.ExpandedIndices);
  const uint vpp = p.Topology == 3 ? 2 : p.Topology == 5 || p.Topology == 6 ? 3 : p.Topology == 11 ? 4 : 6;
  uint emitted = 0, scanned = 0, k = s.InputPosition;
  s.StatisticsVertexCorrection = 0;
  while (k < s.VertexCount && scanned < kCaptureFrontierScanCapacity && emitted < p.PrimitivesPerGroup) {
    if (capture_frontier_cut(s, p, k)) {
      s.StripStart = k + 1;
      s.StripLength = 0;
      ++k;
      ++scanned;
      continue;
    }
    ++s.StripLength;
    const uint length = s.StripLength;
    if (length >= vpp && (p.Topology != 13 || !(length & 1))) {
      const uint i = capture_frontier_primitives(p.Topology, length) - 1;
      uint v[6];
      if (p.Topology == 3) {
        v[0] = i; v[1] = i + 1;
      } else if (p.Topology == 5) {
        v[0] = i; v[1] = (i & 1) ? i + 2 : i + 1; v[2] = (i & 1) ? i + 1 : i + 2;
      } else if (p.Topology == 6) {
        v[0] = i + 1; v[1] = i + 2; v[2] = 0;
      } else if (p.Topology == 11) {
        for (uint j = 0; j < 4; ++j) v[j] = i + j;
      } else {
        const uint t = 2 * i;
        const bool last = s.VertexCount - k <= 2 || capture_frontier_cut(s, p, k + 1) ||
                           capture_frontier_cut(s, p, k + 2);
        if (i == 0 && last) {
          v[0] = 0; v[1] = 1; v[2] = 2; v[3] = 5; v[4] = 4; v[5] = 3;
        } else if (i == 0) {
          v[0] = 0; v[1] = 1; v[2] = 2; v[3] = 6; v[4] = 4; v[5] = 3;
        } else if (i & 1) {
          v[0] = t; v[1] = t + 3; v[2] = t + 4; v[3] = last ? t + 5 : t + 6;
          v[4] = t + 2; v[5] = t - 2;
        } else {
          v[0] = t; v[1] = t - 2; v[2] = t + 2; v[3] = last ? t + 5 : t + 6;
          v[4] = t + 4; v[5] = t + 3;
        }
      }
      for (uint j = 0; j < vpp; ++j) {
        const uint position = s.StripStart + v[j];
        list[emitted * vpp + j] = p.Flags & kCaptureFrontierIndexed
            ? capture_frontier_index(s.OriginalIndex, position) : position;
      }
      // The expanded object stage counts list vertices. Restore IA's unique
      // strip input count, including the initial adjacency vertices once.
      s.StatisticsVertexCorrection += vpp - (i ? (p.Topology == 13 ? 2u : 1u) : vpp);
      ++emitted;
    }
    ++k;
    ++scanned;
  }
  s.NextInputPosition = k;
  s.InstanceDone = k == s.VertexCount;
  s.ActiveIndex = {p.ExpandedIndices, p.PrimitivesPerGroup * vpp * 4, 42, 0, {0, 0, 0}};
  s.DrawArguments[0] = emitted * vpp;
  s.DrawArguments[1] = 1;
  s.DrawArguments[2] = 0;
  s.DrawArguments[3] = s.BaseVertex;
  s.DrawArguments[4] = s.StartInstance;
  s.Shader.producer_count = emitted * vpp;
  return emitted;
}

[[kernel]] void capture_frontier_prepare(device CaptureFrontierState &s [[buffer(0)]],
                                        constant CaptureFrontierParams &p [[buffer(1)]]) {
  s.Indirect = CaptureFrontierIndirect{};
  s.BatchActive = 0;
  if (!s.Initialized) {
    s.Initialized = 1;
    s.EffectiveCount = p.MaxCommands;
    if (p.Count) s.EffectiveCount = min(s.EffectiveCount, *reinterpret_cast<device const uint *>(p.Count));
    if (p.Predicate) {
      const ulong predicate = *reinterpret_cast<device const ulong *>(p.Predicate);
      if (p.PredicateNotEqual ? predicate != 0 : predicate == 0) s.EffectiveCount = 0;
    }
    if (!s.EffectiveCount) s.Status = kCaptureFrontierDone;
  }
  if (s.Status == kCaptureFrontierHost) {
    device const auto &completion = *reinterpret_cast<device const CaptureFrontierCompletion *>(p.Completion);
    if (completion.Status != kCaptureFrontierRunning) return;
    s.Status = kCaptureFrontierRunning;
    if (s.HostReason == kCaptureFrontierAtomicBindings) s.AtomicReady = 1;
    else if (s.HostReason == kCaptureFrontierMSAAConsume) {
      // The host encoded and retired the existing MSAA consumer before this
      // acknowledgement. Resume the saved batch; no producer is executed twice.
      s.BatchActive = 1;
      capture_frontier_finish_batch(s, p);
      // The pixel consumer uses the existing separate Atomic64 phase. Its
      // completed export precedes a new producer binding phase on resume.
      if (p.Flags & kCaptureFrontierAtomic) s.AtomicReady = 0;
    }
    s.HostReason = kCaptureFrontierNoHost;
  }
  if (s.Status != kCaptureFrontierRunning) {
    capture_frontier_publish(s, p);
    return;
  }
  for (uint transition = 0; transition < kCaptureFrontierQuantum; ++transition) {
    if (s.Command >= s.EffectiveCount) {
      s.Status = kCaptureFrontierDone;
      capture_frontier_publish(s, p);
      return;
    }
    if (!s.CommandReady) capture_frontier_command(s, p);
    if (s.Status != kCaptureFrontierRunning) return;
    if (!s.VertexCount || !s.InstanceCount ||
        ((p.Kind == kCaptureFrontierDomain || p.Kind == kCaptureFrontierTessellationGeometry) &&
         s.VertexCount < p.ControlPoints)) {
      ++s.Command;
      ++s.CommandsCompleted;
      s.CommandReady = s.AtomicReady = 0;
      continue;
    }
    if ((p.Flags & kCaptureFrontierAtomic) && !s.AtomicReady) {
      s.Status = kCaptureFrontierHost;
      s.HostReason = kCaptureFrontierAtomicBindings;
      capture_frontier_publish(s, p);
      return;
    }
    s.Shader = CaptureFrontierShaderControl{};
    s.Shader.instance_base = s.Instance;
    s.Shader.primitive_prefix = s.PrimitivePrefix;
    s.Shader.object_group_base = s.ObjectGroup;
    s.ActiveIndex = s.OriginalIndex;
    s.InstanceDone = 0;
    s.PrimitiveCount = 0;
    s.BatchActive = 1;
    s.Phase = kCaptureFrontierProducer;
    if (p.Kind == kCaptureFrontierVertex) {
      const uint vpp = p.Topology == 0 ? 1 : p.Topology <= 2 ? 2 : 3;
      const bool strip = p.Topology == 2 || p.Topology == 4 || p.Topology == 5;
      const uint active = p.Topology >= 6 ? s.VertexCount : strip
          ? (s.VertexCount >= vpp ? s.VertexCount : 0) : s.VertexCount / vpp * vpp;
      const uint fresh = min(active - min(active, s.InputPosition), kCaptureFrontierVertexCapacity);
      s.Shader.first_position = s.InputPosition;
      s.Shader.producer_record_base = s.CarryCount;
      s.Shader.producer_count = fresh;
      s.Shader.local_key_count = s.CarryCount + fresh;
      s.NextInputPosition = s.InputPosition + fresh;
      s.InstanceDone = s.NextInputPosition == active;
      s.Indirect.Vertex[0] = (fresh + 63) / 64;
      s.Indirect.Vertex[1] = s.Indirect.Vertex[2] = 1;
      if (fresh) ++s.ProducerSteps;
    } else if (p.Kind == kCaptureFrontierGeometry) {
      uint primitives = 0;
      if (p.Flags & kCaptureFrontierExpandStrip) {
        s.Shader.object_group_base = 0;
        primitives = capture_frontier_expand(s, p);
        s.Indirect.Producer[0] = primitives ? 1 : 0;
      } else {
        const uint first = s.ObjectGroup * p.WarpIncrement;
        const uint vertices = min(s.VertexCount - first, p.VertexPerWarp);
        primitives = capture_frontier_primitives(p.Topology, vertices);
        s.Shader.producer_count = vertices;
        s.NextObjectGroup = s.ObjectGroup + 1;
        s.InstanceDone = s.NextObjectGroup >= (s.VertexCount - 1) / p.WarpIncrement + 1;
        s.Indirect.Producer[0] = 1;
      }
      s.Shader.local_primitive_count = primitives;
      s.Shader.local_key_count = primitives * p.GSInstances;
      s.Indirect.Producer[1] = s.Indirect.Producer[2] = 1;
      if (s.Indirect.Producer[0]) ++s.ProducerSteps;
    } else {
      const uint patches = s.VertexCount / p.ControlPoints;
      s.Shader.first_patch = s.ObjectGroup * p.PatchesPerGroup;
      if (p.Kind == kCaptureFrontierTessellationGeometry && s.ChildPrimitive < s.ParentPrimitives) {
        s.Phase = kCaptureFrontierChildren;
      } else {
        const uint active = min(patches - s.Shader.first_patch, p.PatchesPerGroup);
        s.ParentSlots = active * p.TessWorkloads;
        s.ParentPrimitivePrefix = s.PrimitivePrefix;
        s.Shader.local_key_count = s.ParentSlots;
        s.NextObjectGroup = s.ObjectGroup + 1;
        s.Tess = {s.ParentSlots, patches, 0, 0};
        s.BatchActive |= 2;
        s.Indirect.Producer[0] = s.Indirect.Producer[1] = s.Indirect.Producer[2] = 1;
        s.InstanceDone = p.Kind == kCaptureFrontierDomain &&
            s.NextObjectGroup >= (patches - 1) / p.PatchesPerGroup + 1;
        ++s.ProducerSteps;
        ++s.ParentSteps;
      }
    }
    s.SO = {s.Shader.local_key_count, s.Shader.first_position, s.Shader.local_key_count,
            p.Flags & kCaptureFrontierIndexed ? 8u : 0u,
            s.StartLocation, s.BaseVertex, s.StartInstance, s.StartLocation};
    s.Vertex.Capture = s.SO;
    const bool strip = p.Topology == 2 || p.Topology == 4 || p.Topology == 5 ||
        p.Topology == 7 || p.Topology == 9;
    s.Vertex.StripCutEnabled = p.Kind == kCaptureFrontierVertex && strip &&
        (p.Flags & kCaptureFrontierIndexed) &&
        ((s.OriginalIndex.Format == 42 && p.StripCut == 2) || (s.OriginalIndex.Format == 57 && p.StripCut == 1));
    s.Vertex.ActivePositionsPerInstance = s.VertexCount;
    s.Mesh = {s.Shader.local_primitive_count, 1, 0, s.Shader.local_key_count};
    capture_frontier_query_epoch(s, p);
    uint clear_words = p.ClearDwords;
    if (s.BatchActive & 4) clear_words = max(clear_words, uint(p.StatisticsClaimBytes / 4));
    s.Indirect.Clear[0] = clear_words ? (clear_words - 1) / 64 + 1 : 0;
    s.Indirect.Clear[1] = s.Indirect.Clear[2] = 1;
    return;
  }
  capture_frontier_publish(s, p);
}

static void capture_frontier_packet(device CaptureFrontierState &s, constant CaptureFrontierParams &p,
                                    thread const uint *primitive, uint vertices, uint slot, uint primitive_id) {
  if (slot >= p.PrimitiveCapacity) {
    capture_frontier_fail(s, p, kCaptureFrontierInvalidCapture);
    return;
  }
  device uint *native = reinterpret_cast<device uint *>(p.ReplayIndices);
  if (p.ReplayIndices)
    for (uint v = 0; v < vertices; ++v) native[slot * vertices + v] = primitive[v];
  if (!(p.Flags & kCaptureFrontierMSAA)) return;
  device uint *indices = reinterpret_cast<device uint *>(p.MSAAIndices);
  device auto *draws = reinterpret_cast<device D3DALLOYMSAAPrimitiveDraw *>(p.MSAADraws);
  const bool line = p.LineMode && vertices == 2;
  const bool conservative = p.Conservative && vertices == 3;
  uint count = vertices;
  if (line) {
    const uint corners[18] = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5, 4, 5, 6, 6, 5, 7};
    const uint first = p.LineMode == 3 ? 6 : 0;
    count = p.LineMode == 3 ? 6 : 18;
    for (uint v = 0; v < count; ++v) indices[20 * slot + v] = 8 * slot + corners[first + v];
    indices[20 * slot + 18] = primitive[0];
    indices[20 * slot + 19] = primitive[1];
  } else if (conservative) {
    const uint corners[9] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
    count = 18;
    for (uint v = 0; v < 18; ++v) indices[24 * slot + v] = 16 * slot + corners[v % 9] + (v < 9 ? 0 : 5);
    for (uint v = 0; v < 3; ++v) indices[24 * slot + 18 + v] = primitive[v];
  } else {
    for (uint v = 0; v < vertices; ++v) indices[3 * slot + v] = primitive[v];
  }
  draws[slot] = {count, 1, p.MSAAIndexStride * slot, 0, 0, primitive_id};
}

// Appending is ordered across batches and commands. A stream which fills up
// remains stopped until the next logical draw, while needed and shader work
// continue. The filled-size words retain their API-defined 32-bit format.
static void capture_frontier_append(device CaptureFrontierState &s, constant CaptureFrontierParams &p,
                                    constant D3DALLOYSOAssembly &layout, uint stream,
                                    device const uint *records, uint stride,
                                    thread const uint *primitive, uint vertices, bool claimed) {
  const auto binding = p.Streams[stream];
  if (!layout.stream_output_enabled) return;
  device auto *counters = reinterpret_cast<device ulong2 *>(binding.Counters);
  counters[0].y++;
  bool stopped = (s.SOStoppedMask & (1u << stream)) || (layout.referenced_mask & ~layout.bound_mask);
  ulong filled[4] = {};
  for (uint slot = 0; slot < 4; ++slot) {
    if (!(layout.bound_mask & (1u << slot))) continue;
    filled[slot] = d3dalloy_so_load_dword(reinterpret_cast<device const uchar *>(binding.FilledSizes[slot]), 0);
    if ((layout.referenced_mask & (1u << slot)) &&
        (filled[slot] > layout.size[slot] || ulong(vertices) * layout.stride[slot] > layout.size[slot] - filled[slot]))
      stopped = true;
  }
  if (stopped) {
    s.SOStoppedMask |= 1u << stream;
    return;
  }
  if (!claimed) {
    if (binding.Status) ++*reinterpret_cast<device uint *>(binding.Status);
    capture_frontier_fail(s, p, kCaptureFrontierInvalidCapture);
    return;
  }
  for (uint v = 0; v < vertices; ++v) {
    device const uint *source = records + ulong(primitive[v]) * stride;
    for (uint e = 0; e < layout.element_count; ++e) {
      const auto element = layout.elements[e];
      const ulong offset = filled[element.slot] + ulong(v) * layout.stride[element.slot] + element.byte_offset;
      for (uint component = 0; component < element.component_count; ++component)
        d3dalloy_so_store_dword(reinterpret_cast<device uchar *>(binding.Targets[element.slot]),
            offset + 4 * component, source[element.dword_offset + component]);
    }
  }
  for (uint slot = 0; slot < 4; ++slot)
    if ((layout.referenced_mask & layout.bound_mask) & (1u << slot))
      d3dalloy_so_store_dword(reinterpret_cast<device uchar *>(binding.FilledSizes[slot]), 0,
          uint(filled[slot] + ulong(vertices) * layout.stride[slot]));
  counters[0].x++;
}

static void capture_frontier_vertex_consume(device CaptureFrontierState &s, constant CaptureFrontierParams &p,
                                            constant D3DALLOYSOAssembly *layouts) {
  const uint topology = p.Topology;
  const uint vpp = topology == 0 ? 1 : (topology <= 2 || topology == 6 || topology == 7) ? 2 : 3;
  const uint input = topology >= 8 ? 6 : topology >= 6 ? 4 : vpp;
  const bool strip = topology == 2 || topology == 4 || topology == 5 || topology == 7 || topology == 9;
  uint count = s.WindowCount, strip_index = s.StripLength, slot = 0;
  uint window[6];
  for (uint i = 0; i < count; ++i) window[i] = i;
  device const uint *records = reinterpret_cast<device const uint *>(p.Scratch);
  device const uint *claims = records + ulong(s.SO.RecordCount) * p.SORecordDwords;
  for (uint fresh = 0; fresh < s.Shader.producer_count; ++fresh) {
    const uint position = s.Shader.first_position + fresh;
    if (strip && capture_frontier_cut(s, p, position)) {
      count = strip_index = 0;
      continue;
    }
    const uint ordinal = s.Shader.producer_record_base + fresh;
    uint primitive[3];
    bool complete = false;
    if (topology >= 6) {
      window[count++] = ordinal;
      if (count == input) {
        if (vpp == 2) { primitive[0] = window[1]; primitive[1] = window[2]; }
        else {
          primitive[0] = window[0];
          primitive[1] = window[(strip_index & 1) ? 4 : 2];
          primitive[2] = window[(strip_index & 1) ? 2 : 4];
        }
        if (strip) {
          const uint step = topology == 7 ? 1 : 2;
          for (uint i = 0; i < input - step; ++i) window[i] = window[i + step];
          count = input - step;
          ++strip_index;
        } else count = 0;
        complete = true;
      }
    } else if (!strip) {
      window[count++] = ordinal;
      if (count == vpp) {
        for (uint i = 0; i < vpp; ++i) primitive[i] = window[i];
        count = 0;
        complete = true;
      }
    } else if (topology == 2) {
      if (!count) { window[0] = ordinal; count = 1; }
      else { primitive[0] = window[0]; primitive[1] = ordinal; window[0] = ordinal; ++strip_index; complete = true; }
    } else if (count < 2) window[count++] = ordinal;
    else if (topology == 5) {
      primitive[0] = window[1]; primitive[1] = ordinal; primitive[2] = window[0];
      window[1] = ordinal; ++strip_index; complete = true;
    } else {
      primitive[0] = window[0];
      primitive[1] = (strip_index & 1) ? ordinal : window[1];
      primitive[2] = (strip_index & 1) ? window[1] : ordinal;
      window[0] = window[1]; window[1] = ordinal; ++strip_index; complete = true;
    }
    if (!complete) continue;
    for (uint stream = 0; stream < p.StreamCount; ++stream) {
      constant auto &layout = layouts[stream];
      bool claimed = true;
      if (layout.stream_output_enabled || layout.statistics_enabled)
        for (uint v = 0; v < vpp; ++v)
          claimed = claimed && primitive[v] < s.SO.RecordCount && claims[primitive[v]];
      if (layout.statistics_enabled) {
        device ulong *statistics = reinterpret_cast<device ulong *>(p.Streams[stream].Statistics);
        statistics[0] += !strip || strip_index == 1 ? input : topology == 9 ? 2 : 1;
        statistics[1]++;
        if (!claimed) {
          capture_frontier_fail(s, p, kCaptureFrontierInvalidCapture);
          return;
        }
        if (layout.rasterization_enabled) {
          statistics[5]++;
          statistics[6] += d3dalloy_clipped_primitives(layout, records, primitive, vpp);
        }
      }
      capture_frontier_append(s, p, layout, stream, records, p.SORecordDwords, primitive, vpp, claimed);
      if (s.Status != kCaptureFrontierRunning) return;
    }
    capture_frontier_packet(s, p, primitive, vpp, slot, uint(s.PrimitivePrefix + slot));
    if (s.Status != kCaptureFrontierRunning) return;
    ++slot;
  }
  s.PrimitiveCount = slot;
  s.Shader.local_primitive_count = slot;
  s.WindowCount = count;
  s.StripLength = strip_index;
  for (uint i = 0; i < count; ++i) s.Window[i] = window[i];
  s.Indirect.Replay[0] = slot * vpp;
  s.Indirect.Replay[1] = 1;
}

static void capture_frontier_mesh_consume(device CaptureFrontierState &s, constant CaptureFrontierParams &p,
                                          constant D3DALLOYSOAssembly *layouts) {
  for (uint stream = 0; stream < p.StreamCount; ++stream) {
    constant auto &layout = layouts[stream];
    const uint vertices = layout.topology == 0 ? 1 : layout.topology == 2 ? 2 : 3;
    const uint stride = layout.record_dwords + 1;
    device const uint *base = reinterpret_cast<device const uint *>(p.Scratch) +
        ulong(s.SO.RecordCount) * p.Streams[stream].PrefixDwords;
    device const uint *counts = base + ulong(s.SO.RecordCount) * layout.vertex_count * stride;
    for (uint key = 0; key < s.SO.RecordCount; ++key) {
      const uint count = counts[key];
      if (count > layout.vertex_count) {
        capture_frontier_fail(s, p, kCaptureFrontierInvalidCapture);
        return;
      }
      device const uint *records = base + ulong(key) * layout.vertex_count * stride;
      uint run = 0;
      for (uint ordinal = 0; ordinal < count; ++ordinal) {
        const uint position = records[ulong(ordinal) * stride + layout.record_dwords];
        if (!position) run = ordinal;
        uint primitive[3];
        bool complete = false;
        if (vertices == 1) { primitive[0] = ordinal; complete = true; }
        else if (vertices == 2 && position >= 1) {
          primitive[0] = ordinal - 1; primitive[1] = ordinal; complete = true;
        } else if (vertices == 3 && position >= 2) {
          const uint i = position - 2;
          primitive[0] = run + i;
          primitive[1] = (i & 1) ? ordinal : run + i + 1;
          primitive[2] = (i & 1) ? run + i + 1 : ordinal;
          complete = true;
        }
        if (complete) capture_frontier_append(s, p, layout, stream, records, stride, primitive, vertices, true);
        if (s.Status != kCaptureFrontierRunning) return;
      }
    }
  }
  if (p.Flags & kCaptureFrontierMSAA) {
    const uint count = s.Shader.local_key_count * p.MeshOutputPrimitives;
    for (uint slot = 0; slot < count; ++slot) {
      uint primitive[3];
      for (uint v = 0; v < p.MeshOutputVertices; ++v) primitive[v] = slot * p.MeshOutputVertices + v;
      // The mesh transport zeros W for absent/culled output. Bounds retains
      // the shader's own primitive ID from the captured record when present.
      capture_frontier_packet(s, p, primitive, p.MeshOutputVertices, slot, slot);
      if (s.Status != kCaptureFrontierRunning) return;
    }
  }
}

[[kernel]] void capture_frontier_consume(device CaptureFrontierState &s [[buffer(0)]],
                                        constant CaptureFrontierParams &p [[buffer(1)]],
                                        constant D3DALLOYSOAssembly *layouts [[buffer(2)]],
                                        constant uint &point [[buffer(3)]]) {
  if (s.Status != kCaptureFrontierRunning || !(s.BatchActive & 1)) return;
  if (p.Kind == kCaptureFrontierTessellationGeometry) {
    if (!point) {
      device uint *list = reinterpret_cast<device uint *>(p.TessList);
      if (s.BatchActive & 2) {
        device const uint *counts = reinterpret_cast<device const uint *>(p.TessScratch);
        uint total = 0;
        for (uint slot = 0; slot < s.ParentSlots; ++slot) {
          const uint count = counts[slot];
          if (count > p.TessPrimitives || total > p.ParentSlotCapacity * p.TessPrimitives - count) {
            capture_frontier_fail(s, p, kCaptureFrontierInvalidCapture);
            return;
          }
          for (uint primitive = 0; primitive < count; ++primitive)
            list[4 + total++] = slot * p.TessPrimitives + primitive;
        }
        s.ParentPrimitives = total;
        s.ChildPrimitive = 0;
        list[0] = total;
        list[1] = list[2] = list[3] = 0;
      }
      const uint count = min(s.ParentPrimitives - s.ChildPrimitive, p.PrimitivesPerGroup);
      s.Shader.object_group_base = 0;
      s.Shader.child_primitive_first = s.ChildPrimitive;
      s.Shader.local_primitive_count = count;
      s.Shader.local_key_count = count * p.GSInstances;
      s.SO.PositionsPerInstance = count;
      s.SO.RecordCount = s.Shader.local_key_count;
      s.Mesh = {count, 1, 0, s.Shader.local_key_count};
      s.NextChildPrimitive = s.ChildPrimitive + count;
      s.Indirect.Consumer[0] = count ? 1 : 0;
      s.Indirect.Consumer[1] = s.Indirect.Consumer[2] = 1;
      if (count) { ++s.ProducerSteps; ++s.ChildSteps; }
      return;
    }
    capture_frontier_mesh_consume(s, p, layouts);
  } else if (!point) {
    if (p.Kind == kCaptureFrontierVertex) capture_frontier_vertex_consume(s, p, layouts);
    else capture_frontier_mesh_consume(s, p, layouts);
    if ((p.Flags & kCaptureFrontierExpandStrip) && p.PipelineStatistics)
      reinterpret_cast<device ulong *>(p.PipelineStatistics)[0] -= s.StatisticsVertexCorrection;
  }
}

[[kernel]] void capture_frontier_advance(device CaptureFrontierState &s [[buffer(0)]],
                                        constant CaptureFrontierParams &p [[buffer(1)]],
                                        constant uint &point [[buffer(3)]]) {
  if (point) {
    ++s.Quantum;
    capture_frontier_publish(s, p);
    return;
  }
  if (s.Status != kCaptureFrontierRunning || !(s.BatchActive & 1)) return;
  if (p.Flags & kCaptureFrontierMSAA) {
    s.Status = kCaptureFrontierHost;
    s.HostReason = kCaptureFrontierMSAAConsume;
    capture_frontier_publish(s, p);
    return;
  }
  capture_frontier_finish_batch(s, p);
}

[[kernel]] void capture_frontier_clear(device CaptureFrontierState &s [[buffer(0)]],
                                      constant CaptureFrontierParams &p [[buffer(1)]],
                                      uint word [[thread_position_in_grid]]) {
  if (!(s.BatchActive & 1) || s.Status != kCaptureFrontierRunning) return;
  if (ulong(word) < p.ScratchBytes / 4) {
    uint value = 0;
    if (p.Kind == kCaptureFrontierVertex) {
      if (word < s.CarryCount * p.SORecordDwords)
        value = reinterpret_cast<device const uint *>(p.CarryScratch)[word];
      const uint claims = s.SO.RecordCount * p.SORecordDwords;
      if (word >= claims && word - claims < s.CarryCount) value = 1;
    }
    reinterpret_cast<device uint *>(p.Scratch)[word] = value;
  }
  if (ulong(word) < p.VertexBytes / 4) {
    const uint value = p.Kind == kCaptureFrontierVertex && word < s.CarryCount * p.VertexRecordDwords
        ? reinterpret_cast<device const uint *>(p.CarryVertexRecords)[word] : 0;
    reinterpret_cast<device uint *>(p.VertexRecords)[word] = value;
  }
  if (s.BatchActive & 2) {
    if (ulong(word) < p.TessScratchBytes / 4) reinterpret_cast<device uint *>(p.TessScratch)[word] = 0;
    if (ulong(word) < p.TessListBytes / 4) reinterpret_cast<device uint *>(p.TessList)[word] = 0;
  }
  if (ulong(word) < p.MSAAIndicesBytes / 4) reinterpret_cast<device uint *>(p.MSAAIndices)[word] = 0;
  if (ulong(word) < p.MSAADrawsBytes / 4) reinterpret_cast<device uint *>(p.MSAADraws)[word] = 0;
  if (ulong(word) < p.MSAABoundsBytes / 4) reinterpret_cast<device uint *>(p.MSAABounds)[word] = 0;
  if ((s.BatchActive & 4) && ulong(word) < p.StatisticsClaimBytes / 4)
    reinterpret_cast<device uint *>(p.StatisticsClaimEntries)[word] = 0;
}
