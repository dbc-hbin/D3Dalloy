#pragma once

/*
 * Owned DXIL work graph (lib_6_8 node shader) -> AIR/Metal compilation ABI.
 *
 * The runtime (d3d12_work_graph.cpp) reflects node shaders and ordinary library
 * functions, resolves immutable collection exports, builds the graph, and compiles each
 * node it keeps into one Metal compute kernel. The kernels run under the runtime's
 * own scheduler: a node dispatch processes one bounded batch of the records waiting
 * in its input queue, and the scheduler sized that batch so every record the batch
 * can possibly write fits in the queues it targets (see d3d12_work_graph.cpp).
 *
 * Everything the compiled kernel reads at run time is described here:
 *
 *   binding SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT  -> SM50_WG_NODE_CONTEXT of the
 *                                                     dispatch (device memory)
 *   SM50_WG_NODE_CONTEXT::QueueTable              -> SM50_WG_QUEUE[]
 *   SM50_WG_NODE_CONTEXT::BatchTable              -> SM50_WG_BATCH_RECORD[]
 *                                                     (broadcasting launch only)
 *
 * plus the ordinary per-program global root arguments, each node's local root
 * arguments, and the program's shared global/local static sampler table.
 */

#include "airconv_public.h"

#ifndef __AIRCONV_DXIL_WORKGRAPH_H
#define __AIRCONV_DXIL_WORKGRAPH_H

/** Value of an index / queue field meaning "none". */
#define SM50_WG_NONE 0xffffffffu

/** Metal threadgroup width a thread launch node packs its records into. */
#define SM50_WG_THREAD_LAUNCH_GROUP_SIZE 32u

#ifdef __cplusplus
extern "C" {
#endif

/** DXIL NodeLaunchType (`!dx.entryPoints` tag 13). */
enum SM50_WG_LAUNCH : uint32_t {
  SM50_WG_LAUNCH_BROADCASTING = 1,
  SM50_WG_LAUNCH_COALESCING = 2,
  SM50_WG_LAUNCH_THREAD = 3,
};

/** DXIL NodeIOFlags (metadata tag 1 of a node input/output). */
enum SM50_WG_IO_FLAG : uint32_t {
  SM50_WG_IO_INPUT = 0x1,
  SM50_WG_IO_OUTPUT = 0x2,
  SM50_WG_IO_READ_WRITE = 0x4,
  SM50_WG_IO_EMPTY_RECORD = 0x8,
  SM50_WG_IO_NODE_ARRAY = 0x10,
  SM50_WG_IO_GRANULARITY_MASK = 0x60,
  SM50_WG_IO_TRACK_RW_INPUT_SHARING = 0x100,
  SM50_WG_IO_GLOBALLY_COHERENT = 0x200,
};

/* ------------------------------------------------------------------------- */
/* Library reflection                                                        */
/* ------------------------------------------------------------------------- */

/** One node input or output declaration. Names are (offset, length) in pNames. */
struct SM50_WG_NODE_IO {
  uint32_t Flags; /* SM50_WG_IO_FLAG */
  /** Record size in bytes (0 for an empty record). */
  uint32_t RecordSize;
  /** Record alignment in bytes (at least 1). */
  uint32_t RecordAlignment;
  /** Inputs: coalescing MaxRecords (1 when not declared). Outputs: MaxRecords, or 0 when shared. */
  uint32_t MaxRecords;
  /** Outputs: index of the output whose MaxRecords budget this one shares, else SM50_WG_NONE. */
  uint32_t MaxRecordsSharedWith;
  /** Outputs: node array size, 1 for a single node, SM50_WG_NONE for [UnboundedSparseNodes]. */
  uint32_t ArraySize;
  uint32_t AllowSparseNodes;
  /** SV_DispatchGrid in the record: byte offset, DXIL component type (5 u32, 3 u16) and count; 0 count if absent. */
  uint32_t DispatchGridOffset;
  uint32_t DispatchGridComponentType;
  uint32_t DispatchGridComponents;
  /** Outputs: the NodeID the output targets. */
  uint32_t NodeNameOffset;
  uint32_t NodeNameLength;
  uint32_t NodeArrayIndex;
  uint32_t Reserved;
};

/** One node shader or ordinary function export of a library. */
struct SM50_WG_NODE_INFO {
  /** Public RDAT export name; the exact container definition is recorded below. */
  uint32_t NameOffset;
  uint32_t NameLength;
  uint32_t Launch; /* SM50_WG_LAUNCH */
  uint32_t IsProgramEntry;
  /** The node's own NodeID. */
  uint32_t NodeNameOffset;
  uint32_t NodeNameLength;
  uint32_t NodeArrayIndex;
  /** NodeShareInputOf target; HasShareInputOf is 0 when not declared. */
  uint32_t HasShareInputOf;
  uint32_t ShareInputNameOffset;
  uint32_t ShareInputNameLength;
  uint32_t ShareInputArrayIndex;
  /** [NodeLocalRootArgumentsTableIndex], SM50_WG_NONE when automatic. */
  uint32_t LocalRootArgumentsTableIndex;
  uint32_t NumThreads[3];
  /** [NodeDispatchGrid]; all 0 when the grid comes from the record. */
  uint32_t DispatchGrid[3];
  /** [NodeMaxDispatchGrid]; all 0 when not declared. */
  uint32_t MaxDispatchGrid[3];
  uint32_t MaxRecursionDepth;
  uint32_t HasInput;
  struct SM50_WG_NODE_IO Input;
  /** Range of this node's outputs in the pOutputs array, in declaration order. */
  uint32_t OutputOffset;
  uint32_t OutputCount;
  /** A fixed grid can contain zero dimensions; zeros must not imply a dynamic grid. */
  uint32_t HasFixedDispatchGrid;
  /** Exact container function name; NameOffset is its public RDAT name. Launch=0 denotes a library function. */
  uint32_t EntryPointOffset;
  uint32_t EntryPointLength;
};

/**
 * Reflect every node shader and ordinary library function export (Launch=0) of one DXIL library container (two-call
 * protocol). With capacities too small (or null arrays) the counts are still
 * written and 1 is returned; the arrays are filled only when everything fits.
 * Other shader stages are skipped. Returns 0 (with *ppError) for an invalid library.
 */
AIRCONV_API int SM50WGReflectLibrary(
  const void *pBytecode, uint64_t BytecodeSize, struct SM50_WG_NODE_INFO *pNodes, uint32_t NodeCapacity,
  struct SM50_WG_NODE_IO *pOutputs, uint32_t OutputCapacity, char *pNames, uint64_t NamesSize,
  uint32_t *pNodeCount, uint32_t *pOutputCount, uint64_t *pNamesSize, sm50_error_t *ppError
);

/* ------------------------------------------------------------------------- */
/* Run time ABI of a compiled node kernel                                    */
/* ------------------------------------------------------------------------- */

/** One record queue (a ring of fixed-stride records). */
struct SM50_WG_QUEUE {
  /** Device address of the ring storage (Mask + 1 records of Stride bytes). */
  uint64_t Records;
  /** Device address of Mask + 1 uint32 FinishedCrossGroupSharing counters, or 0. */
  uint64_t Sharing;
  uint32_t Mask;
  uint32_t Stride;
  /** Monotonic count of records ever reserved; producers reserve with an atomic add. */
  uint32_t Tail;
  uint32_t Reserved;
};

/** Broadcasting batch entry: the record's grid and the batch-cumulative group count through it. */
struct SM50_WG_BATCH_RECORD {
  uint32_t GroupEnd;
  uint32_t Grid[3];
};

/**
 * The per-dispatch context a node kernel reads at SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT.
 *
 * Broadcasting: the dispatch runs BatchGroups threadgroups; threadgroup t is group
 *   GroupStart + t of the batch, counted from group 0 of record BatchStart.
 * Coalescing: threadgroup t takes records [BatchStart + t*M, +M) clamped to BatchRecords,
 *   M being the input's MaxRecords.
 * Thread: thread t of the dispatch (SM50_WG_THREAD_LAUNCH_GROUP_SIZE per threadgroup) takes
 *   record BatchStart + t when t < BatchRecords.
 */
struct SM50_WG_NODE_CONTEXT {
  uint64_t QueueTable;
  uint64_t BatchTable;
  /** Device address of this node's materialized local root arguments, or 0. */
  uint64_t LocalRootArguments;
  uint32_t InputQueue;
  uint32_t BatchStart;
  uint32_t BatchRecords;
  uint32_t BatchGroups;
  uint32_t GroupStart;
  uint32_t RemainingRecursionLevels;
  uint32_t OutputElementCount;
  uint32_t Reserved;
  /** Output element (SM50_WG_NODE_COMPILE_DESC::OutputElementBase + array index) -> queue, or SM50_WG_NONE. */
  uint32_t Outputs[1];
};

/* ------------------------------------------------------------------------- */
/* Node compilation                                                          */
/* ------------------------------------------------------------------------- */

struct SM50_WG_LIBRARY {
  const void *Bytecode;
  uint64_t Size;
};

struct SM50_WG_EXPORT {
  const char *Name;
  const char *EntryPoint;
  uint32_t LibraryIndex;
  /** Collection scope, zero for direct libraries. Functions may call across scopes only with the flags below. */
  uint32_t Scope;
  uint32_t Flags; /* D3D12_STATE_OBJECT_FLAGS of the defining collection */
  /** Bit 0: retained private dependency, visible only to functions in Scope. */
  uint32_t Reserved;
  const void *GlobalRootSignature;
  uint64_t GlobalRootSignatureSize;
  const void *LocalRootSignature;
  uint64_t LocalRootSignatureSize;
};

struct SM50_WG_NODE_COMPILE_DESC {
  const void *Library;
  uint64_t LibrarySize;
  /** Exact container definition of a node shader or collection function. */
  const char *EntryPoint;
  /** Name of the AIR kernel to create. */
  const char *FunctionName;
  const void *GlobalRootSignature;
  uint64_t GlobalRootSignatureSize;
  /** Local root signature blob, or null. */
  const void *LocalRootSignature;
  uint64_t LocalRootSignatureSize;
  uint32_t MetalVersion; /* SM50_SHADER_METAL_VERSION */
  /** Effective launch (the shader's; launch overrides must match it). */
  uint32_t Launch;
  /** Number of node outputs the shader declares. */
  uint32_t OutputCount;
  /** 1 validates a collection export's dependencies and roots without emitting AIR; 0 compiles a node. */
  uint32_t Reserved;
  /** Per output: first element in SM50_WG_NODE_CONTEXT::Outputs. */
  const uint32_t *OutputElementBase;
  /** Per output: number of elements (indices at or past it are invalid outputs). */
  const uint32_t *OutputElementCount;
  const struct SM50_WG_LIBRARY *Libraries;
  const struct SM50_WG_EXPORT *Exports;
  uint32_t LibraryCount;
  uint32_t ExportCount;
  uint32_t EntryLibraryIndex;
  uint32_t AllowUnresolved; /* collection validation only; no AIR is emitted */
  /** Exact public export being compiled, disambiguating aliases of the same container definition. */
  uint32_t EntryExportIndex;
};

/** Reflection of one compiled node kernel. */
struct SM50_WG_NODE_COMPILED_INFO {
  /** Metal threadgroup size the kernel must be dispatched with. */
  uint32_t ThreadgroupSize[3];
  uint32_t Reserved;
};

/**
 * Compile one node shader into a metallib whose kernel is `FunctionName`.
 * Returns 1 on success; *ppBitcode is released with SM50DestroyBitcode and
 * *pMetallib (including immutable constants) borrows from it.
 */
AIRCONV_API int SM50WGCompileNode(
  const struct SM50_WG_NODE_COMPILE_DESC *pDesc, sm50_bitcode_t *ppBitcode, struct SM50_COMPILED_BITCODE *pMetallib,
  struct SM50_WG_NODE_COMPILED_INFO *pInfo, sm50_error_t *ppError
);

#ifdef __cplusplus
}
#endif

#endif /* __AIRCONV_DXIL_WORKGRAPH_H */
