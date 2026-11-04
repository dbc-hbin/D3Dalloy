// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(__METAL_VERSION__)
using CaptureFrontierUInt = uint;
using CaptureFrontierAddress = ulong;
#else
#include <cstdint>
using CaptureFrontierUInt = uint32_t;
using CaptureFrontierAddress = uint64_t;
#endif

namespace d3dalloy {

// These bound resident work, never the draw's input. The GPU resumes the same
// command/instance after every batch and the host resumes unfinished quanta.
enum : CaptureFrontierUInt {
  kCaptureFrontierQuantum = 64,
  kCaptureFrontierFirstQuantum = 1,
  kCaptureFrontierVertexCapacity = 256,
  kCaptureFrontierCarryCapacity = 6,
  kCaptureFrontierScanCapacity = 256,
};

enum CaptureFrontierKind : CaptureFrontierUInt {
  kCaptureFrontierVertex = 0,
  kCaptureFrontierGeometry = 1,
  kCaptureFrontierDomain = 2,
  kCaptureFrontierTessellationGeometry = 3,
};
enum CaptureFrontierStatus : CaptureFrontierUInt {
  kCaptureFrontierRunning = 0,
  kCaptureFrontierDone = 1,
  kCaptureFrontierError = 2,
  kCaptureFrontierHost = 3,
};
enum CaptureFrontierHost : CaptureFrontierUInt {
  kCaptureFrontierNoHost = 0,
  kCaptureFrontierAtomicBindings = 1,
  kCaptureFrontierMSAAConsume = 2,
};
enum CaptureFrontierError : CaptureFrontierUInt {
  kCaptureFrontierNoError = 0,
  kCaptureFrontierInvalidMetadata = 1,
  kCaptureFrontierInvalidIndexView = 2,
  kCaptureFrontierInvalidCapture = 3,
};
enum CaptureFrontierFlags : CaptureFrontierUInt {
  kCaptureFrontierIndexed = 1,
  kCaptureFrontierExpandStrip = 2,
  kCaptureFrontierAtomic = 4,
  kCaptureFrontierMSAA = 8,
  kCaptureFrontierRaster = 16,
};
enum CaptureFrontierPhase : CaptureFrontierUInt {
  kCaptureFrontierCommand = 0,
  kCaptureFrontierProducer = 1,
  kCaptureFrontierChildren = 2,
  kCaptureFrontierConsume = 3,
};

// Mirrors SM50_CAPTURE_FRONTIER_CONTROL. This is the only scheduler state
// visible to application-stage wrappers; global IDs and local storage are
// separate, and each producer batch belongs to exactly one draw instance.
struct CaptureFrontierShaderControl {
  CaptureFrontierUInt object_group_base;
  CaptureFrontierUInt instance_base;
  CaptureFrontierUInt first_position;
  CaptureFrontierUInt local_key_count;
  CaptureFrontierAddress primitive_prefix;
  CaptureFrontierUInt child_primitive_first;
  CaptureFrontierUInt producer_record_base;
  CaptureFrontierUInt producer_count;
  CaptureFrontierUInt local_primitive_count;
  CaptureFrontierUInt first_patch;
  CaptureFrontierUInt reserved[5];
};

// Host reads only this completion mailbox after actual GPU retirement.
// Command is used solely when an existing Atomic64 binding phase must resume.
struct CaptureFrontierCompletion {
  CaptureFrontierUInt Status, Error, HostReason, Command;
  CaptureFrontierAddress ProducerSteps, ConsumerSteps, CommandsCompleted, Quantum;
  CaptureFrontierAddress ParentSteps, ChildSteps;
};

struct CaptureFrontierIndexView {
  CaptureFrontierAddress Address;
  CaptureFrontierUInt Size, Format, FirstIndex, Padding[3];
};
struct CaptureFrontierSOControl {
  CaptureFrontierUInt PositionsPerInstance, FirstPosition, RecordCount, IndexSize;
  CaptureFrontierUInt IndexFirst, BaseVertex, BaseInstance, StartVertex;
};
struct CaptureFrontierVertexControl {
  CaptureFrontierSOControl Capture;
  CaptureFrontierUInt StripCutEnabled, ActivePositionsPerInstance;
};
struct CaptureFrontierTessControl {
  CaptureFrontierUInt SlotCount, PatchCount, TotalOffset, Reserved;
};
struct CaptureFrontierMeshControl {
  CaptureFrontierUInt GroupsX, GroupsY, GroupBase, TotalSlots;
};
struct CaptureFrontierIndirect {
  CaptureFrontierUInt Clear[4];
  CaptureFrontierUInt Producer[4];
  CaptureFrontierUInt Consumer[4];
  CaptureFrontierUInt Vertex[4];
  CaptureFrontierUInt Replay[8];
};

struct CaptureFrontierStream {
  CaptureFrontierAddress Params, Counters, Statistics, Status;
  CaptureFrontierAddress Targets[4], FilledSizes[4];
  CaptureFrontierAddress PrefixDwords;
};

// Immutable CPU-known layout and GPU addresses. No input dimension is copied
// back to size an allocation. A complete TS object group is the minimum parent
// capacity, including all W workloads (W may exceed 64).
struct CaptureFrontierParams {
  CaptureFrontierAddress Arguments, Count, Predicate, FixedIndexAddress;
  CaptureFrontierAddress StagedRoots, StagedVertices, StagedIndexViews;
  CaptureFrontierAddress ActiveRoots, ActiveVertices;
  CaptureFrontierAddress Scratch, VertexRecords, ReplayIndices;
  CaptureFrontierAddress CarryScratch, CarryVertexRecords;
  CaptureFrontierAddress TessScratch, TessList, ExpandedIndices;
  CaptureFrontierAddress MSAAAssembly, MSAAIndices, MSAADraws, MSAABounds;
  CaptureFrontierAddress Completion, StatisticsClaim, StatisticsClaimEntries, StatisticsClaimBytes;
  CaptureFrontierAddress PipelineStatistics;
  CaptureFrontierAddress RootStride, VertexStride, IndexViewStride;
  CaptureFrontierAddress ScratchBytes, VertexBytes, TessScratchBytes, TessListBytes;
  CaptureFrontierAddress MSAAIndicesBytes, MSAADrawsBytes, MSAABoundsBytes;
  CaptureFrontierStream Streams[4];
  CaptureFrontierUInt Kind, Flags, MaxCommands, ArgumentStride;
  CaptureFrontierUInt DrawOffset, IndexViewOffset, PredicateNotEqual, FixedIndexFormat;
  CaptureFrontierUInt FixedIndexSize, RootBytes, VertexTableBytes, StreamCount;
  CaptureFrontierUInt Topology, StripCut, VertexPerWarp, WarpIncrement;
  CaptureFrontierUInt PrimitivesPerGroup, GSInstances, ControlPoints, PatchesPerGroup;
  CaptureFrontierUInt TessWorkloads, TessVertices, TessPrimitives, TessRegisters;
  CaptureFrontierUInt TessPrimitiveVertices, ParentSlotCapacity, KeyCapacity, VertexRecordDwords;
  CaptureFrontierUInt PrimitiveCapacity, MSAAIndexStride, MSAARecordDwords, SORecordDwords;
  CaptureFrontierUInt MeshOutputPrimitives, MeshOutputVertices, ClearDwords, LineMode;
  CaptureFrontierUInt Conservative, Reserved[3];
};

// Private GPU continuation. Cursors remain tuples: flattening the entire draw's
// vertex * instance * GS-instance space can exceed even 64 bits.
struct CaptureFrontierState {
  CaptureFrontierUInt Status, Error, Phase, Initialized;
  CaptureFrontierUInt EffectiveCount, Command, Instance, InputPosition;
  CaptureFrontierUInt ObjectGroup, ChildPrimitive, ParentSlots, ParentPrimitives;
  CaptureFrontierUInt NextInputPosition, NextObjectGroup, NextChildPrimitive, BatchActive;
  CaptureFrontierUInt CarryCount, WindowCount, StripLength, StripStart;
  CaptureFrontierUInt Window[6], CarrySources[6];
  CaptureFrontierUInt InstanceDone, CommandReady, AtomicReady, HostReason;
  CaptureFrontierUInt VertexCount, InstanceCount, StartLocation, BaseVertex, StartInstance;
  CaptureFrontierUInt PrimitiveCount, QueryEpoch, SOStoppedMask, StatisticsVertexCorrection;
  CaptureFrontierAddress PrimitivePrefix, ParentPrimitivePrefix;
  CaptureFrontierAddress ProducerSteps, ConsumerSteps, CommandsCompleted, Quantum;
  CaptureFrontierAddress ParentSteps, ChildSteps;
  CaptureFrontierUInt DrawArguments[8];
  CaptureFrontierIndexView OriginalIndex, ActiveIndex;
  alignas(16) CaptureFrontierShaderControl Shader;
  alignas(16) CaptureFrontierSOControl SO;
  alignas(16) CaptureFrontierVertexControl Vertex;
  alignas(16) CaptureFrontierTessControl Tess;
  alignas(16) CaptureFrontierMeshControl Mesh;
  alignas(16) CaptureFrontierIndirect Indirect;
};

static_assert(sizeof(CaptureFrontierShaderControl) == 64);
static_assert(sizeof(CaptureFrontierCompletion) == 64);
static_assert(sizeof(CaptureFrontierIndexView) == 32);
static_assert(sizeof(CaptureFrontierSOControl) == 32);
static_assert(sizeof(CaptureFrontierVertexControl) == 40);
static_assert(sizeof(CaptureFrontierTessControl) == 16);
static_assert(sizeof(CaptureFrontierMeshControl) == 16);

} // namespace d3dalloy
