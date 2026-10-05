#pragma once

/*
 * Owned DXIL raytracing pipeline -> AIR/Metal compilation ABI.
 *
 * This is the whole-pipeline ("C/POD") contract between the D3D12 runtime side
 * (state objects, SBT, DispatchRays) and the compiler side. It is deliberately a
 * single POD description in, a single metallib + reflection out: everything that
 * the runtime has to agree on about the *shader* ABI lives in this header.
 *
 * Not derived from, and not compatible with, any vendor shader-identifier table,
 * function mask or SBT converter: the runtime keeps D3D12 semantics (32 byte shader
 * identifiers + verbatim local root data) and this compiler lowers DXIL libraries
 * and exports onto native public Metal functions.
 *
 * Sibling header: dxil_converter.hpp (ModuleInfo/LowerStage/LowerResources/
 * LowerOperations) owns the generic DXIL front end this file builds on.
 */

#include "airconv_public.h"

#ifndef __AIRCONV_DXIL_RAYTRACING_H
#define __AIRCONV_DXIL_RAYTRACING_H

/** Value used in a *_index field to mean "this hit group has no such shader". */
#define SM50_RT_INDEX_NONE 0xffffffffu

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Pipeline description                                                      */
/* ------------------------------------------------------------------------- */

/**
 * DXIL `ShaderKind`, the value of the `!dx.entryPoints` properties tag 8
 * (`kDxilShaderKindTag`). These are the on-disk DXIL ABI numbers, identical to
 * `DXIL::ShaderKind` in DXC's DxilConstants.h; the runtime's OwnedRTStage uses the
 * same numbering, so an export's stage never has to be translated.
 */
enum SM50_RT_STAGE : uint32_t {
  SM50_RT_STAGE_LIBRARY = 6,
  SM50_RT_STAGE_RAYGEN = 7,
  SM50_RT_STAGE_INTERSECTION = 8,
  SM50_RT_STAGE_ANY_HIT = 9,
  SM50_RT_STAGE_CLOSEST_HIT = 10,
  SM50_RT_STAGE_MISS = 11,
  SM50_RT_STAGE_CALLABLE = 12,
  SM50_RT_STAGE_MESH = 13,
  SM50_RT_STAGE_AMPLIFICATION = 14,
};

/** D3D12_HIT_GROUP_TYPE; only meaningful when Stage == SM50_RT_STAGE_INTERSECTION. */
enum SM50_RT_HIT_GROUP_TYPE : uint32_t {
  SM50_RT_HIT_GROUP_TRIANGLES = 0,
  SM50_RT_HIT_GROUP_PROCEDURAL = 1,
};

/** D3D12_RAYTRACING_PIPELINE_FLAG. The runtime passes the D3D12 dword verbatim. */
enum SM50_RT_PIPELINE_FLAG : uint32_t {
  SM50_RT_PIPELINE_FLAG_NONE = 0x0,
  /** Skip triangle primitives entirely during traversal. */
  SM50_RT_PIPELINE_FLAG_SKIP_TRIANGLES = 0x100,
  /** Skip procedural (bounding box) primitives entirely during traversal. */
  SM50_RT_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES = 0x200,
};

struct SM50_RT_LIBRARY {
  /** DXBC container holding a `DXIL` chunk with a shader library. */
  const void *Bytecode;
  uint64_t Size;
};

/**
 * One compiled export.
 *
 * Array order is the *export index* the runtime uses everywhere (identifier
 * payload, hit group indices, callable indices). The runtime resolves hit group
 * references to export indices before calling the compiler, so a hit group is
 * described by exactly one entry whose Stage is the "primary" shader of the group
 * and whose three *_index fields identify the members:
 *
 *   procedural hit group -> Stage = INTERSECTION, IntersectionIndex = self,
 *                           AnyHitIndex / ClosestHitIndex from the group
 *   triangle hit group   -> Stage = CLOSEST_HIT, ClosestHitIndex = self,
 *                           AnyHitIndex from the group, IntersectionIndex = NONE
 *
 * Members are emitted as inlined code inside the group function, so the group is
 * a single Metal function - matching D3D12, where a hit group is the dispatch unit.
 */
struct SM50_RT_EXPORT {
  uint32_t Stage; /* SM50_RT_STAGE */
  uint32_t LibraryIndex;
  uint32_t HitGroupType; /* SM50_RT_HIT_GROUP_TYPE */
  uint32_t ClosestHitIndex;
  uint32_t AnyHitIndex;
  uint32_t IntersectionIndex;
  /**
   * Public D3D12 export name of this export: what D3D12_EXPORT_DESC::Name, the
   * hit-group shader imports and ID3D12StateObjectProperties::GetShaderIdentifier
   * use, and the name the AIR/Metal function is created under. It comes from the
   * library's RDAT unmangled name, never from the mangled entry point.
   */
  const char *EntryPoint;
  /**
   * The export's `!dx.entryPoints` operand 1, i.e. the name the container declares
   * the function as. DXC mangles it (a leading 0x01 marker and the C++ signature),
   * so it is the only name that can select the entry point inside the container:
   * this is what the compiler parses the library with. Required; an export whose
   * library declares no entry point under this name fails compilation.
   */
  const char *ContainerEntryPoint;
  /** Local root signature blob; NULL when the export has none. */
  const void *LocalRootSignature;
  uint64_t LocalRootSignatureSize;
};

struct SM50_RT_PIPELINE_DESC {
  const struct SM50_RT_LIBRARY *Libraries;
  uint32_t LibraryCount;
  const struct SM50_RT_EXPORT *Exports;
  uint32_t ExportCount;
  /** Global root signature blob shared by every export. */
  const void *GlobalRootSignature;
  uint64_t GlobalRootSignatureSize;
  /** Maximum TraceRay recursion depth (D3D12_MAX_RAY_RECURSION_DEPTH=31 upper bound). */
  uint32_t MaxRecursionDepth;
  /** Maximum user payload size in bytes. */
  uint32_t MaxPayloadSize;
  /** Maximum user attribute size in bytes. */
  uint32_t MaxAttributeSize;
  uint32_t Flags; /* SM50_RT_PIPELINE_FLAG */
  /** SM50_SHADER_METAL_VERSION, from SM50_SHADER_COMMON_DATA. */
  uint32_t MetalVersion;
  /** Public library namespace, including ordinary functions; shared with work-graph linking. */
  const struct SM50_WG_EXPORT *LinkExports;
  uint32_t LinkExportCount;
  /** Caller-owned name storage, retained while consuming function reflection (also WoW64-safe). */
  char *FunctionNames;
  uint64_t FunctionNamesSize;
};

/* ------------------------------------------------------------------------- */
/* Compiled shader ABI                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Metal buffer indices the lowered RT shaders expect. These are *argument* indices
 * in the same space as GetArgumentIndex() (ConstantBuffer slot N -> index N), so
 * they line up with MTL_SM50_SHADER_ARGUMENT.SM50BindingSlot when the runtime
 * enumerates the RT exports.
 *
 * Slots 0..7 are already taken by the existing root/argument-table ABI and the
 * constant buffer slot mask is 16 bit, so 20..23 are reserved here and can never
 * alias a user constant buffer.
 */
enum SM50_RT_BINDING_INDEX : uint32_t {
  /** SM50_RT_DISPATCH_ARGUMENTS, constant. */
  SM50_RT_BINDING_INDEX_DISPATCH = 20,
  /** intersection_function_table<triangle_data, instancing>. */
  SM50_RT_BINDING_INDEX_INTERSECTION_FUNCTION_TABLE = 21,
  /**
   * MTLVisibleFunctionTable holding every lowered RT shader. Bound with
   * MTLComputeCommandEncoder setVisibleFunctionTable:atBufferIndex: at index 23.
   * Slot 0 is always the null entry, so the function for export index N lives in
   * slot N + 1; the traversal reads the export index the runtime published and
   * calls through this table.
   */
  SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE = 23,
};

/*
 * There is deliberately no acceleration-structure binding slot. A
 * RaytracingAccelerationStructure is an ordinary root-signature SRV, so the shader
 * resolves the descriptor itself and the traversal takes the structure it named as
 * its own operand: the descriptor holds the D3D12 GPU virtual address, whose first
 * qword is the Metal resource id the runtime's build published. Binding one
 * pipeline-wide structure at a fixed buffer index could only trace the wrong scene
 * when a shader names several (or names them dynamically).
 */

/**
 * Dispatch-time constants, read by every lowered RT stage.
 *
 * Bound as a constant buffer at SM50_RT_BINDING_INDEX_DISPATCH. All GPU addresses
 * are D3D12 device VAs, unmodified from the application's view: the runtime does
 * not rewrite SBT records.
 *
 * Layout is explicit and 8 byte aligned; the compiler emits plain GEPs against it.
 */
struct SM50_RT_DISPATCH_ARGUMENTS {
  /** +0x00 RayGenerationShaderRecord.StartAddress, including the 32 byte identifier. */
  uint64_t RayGenerationRecord;
  /** +0x08 MissShaderTable.StartAddress. */
  uint64_t MissRecordBase;
  /** +0x10 HitGroupTable.StartAddress. */
  uint64_t HitGroupRecordBase;
  /** +0x18 CallableShaderTable.StartAddress. */
  uint64_t CallableRecordBase;
  /**
   * +0x20 D3D12_RAYTRACING_SHADER_TABLE strides (0 = every index reads the first
   * record) and record counts (SizeInBytes / stride, 1 for stride 0, 0 when the
   * table is absent). The traversal resolves a record straight out of the
   * application's table: its identifier names the export and the bytes after it
   * are the export's local root arguments, read in place.
   */
  uint32_t MissRecordStride;
  uint32_t MissRecordCount;
  uint32_t HitGroupRecordStride;
  uint32_t HitGroupRecordCount;
  uint32_t CallableRecordStride;
  uint32_t CallableRecordCount;
  /** +0x38 SM50_RT_EXPORT count of the dispatching state object, for record bounds checks. */
  uint32_t ExportCount;
  /** Max recursion depth from the pipeline description. */
  uint32_t RecursionDepth;
  /** User payload size in bytes (SM50_RT_PIPELINE_DESC::MaxPayloadSize). */
  uint32_t PayloadSize;
  /** User attribute size in bytes (SM50_RT_PIPELINE_DESC::MaxAttributeSize). */
  uint32_t AttributeSize;
  /** D3D12_DISPATCH_RAYS_DESC Width/Height/Depth. */
  uint32_t DispatchWidth;
  uint32_t DispatchHeight;
  uint32_t DispatchDepth;
  /**
   * The buffer's own D3D12 GPU virtual address: where this struct lives.
   *
   * A shader that receives this block as a bound argument (the ray generation
   * shader, the intersection shader, the shared candidate callback) does not need
   * it, but a visible function does: it has no bound arguments at all, and the
   * shader table bases and the recursion limit live here, so the address is what
   * lets the shader that issues a trace hand its
   * callee the constants to resolve that callee's own record - and what a nested
   * trace reads the tables through. 0 when nothing filled it.
   */
  uint64_t DispatchArgumentsAddress;
  /**
   * OwnedShaderIdentifier::pipeline_token of the state object being dispatched. A record
   * whose identifier carries another token names no export of this dispatch.
   */
  uint32_t PipelineToken;
  /** Immutable constant-pool GPU address, retained by the dispatched pipeline. */
  uint64_t ImmutableConstants;
};

/**
 * Per-invocation frame. Lives inline at the head of the payload/attribute block
 * that the compiler builds, so the hardware traversal carries it across TraceRay
 * without any side allocation and without stealing bytes from the user payload.
 *
 * The last six fields are the invocation's *runtime context*: the state a shader
 * needs to resolve its resources and to issue a trace, but cannot derive from its
 * own arguments.
 *
 * Two things force it through here. A shader record's local root arguments are
 * selected by the party that picks the record, and only that party knows which
 * record it picked - a visible function receives its *export* index (which is also
 * its visible function table slot), never the record's position inside its shader
 * table, so the record's local root arguments are resolved by the caller and
 * handed over as SM50_RT_FRAME::LocalRootEntry. And a visible function
 * is reached through the visible function table with a fixed argument list - the
 * payload, the hit state, the record and the depth (see EmitVisibleFunctionCall) -
 * so a miss / closest-hit / any-hit / callable export cannot also be handed the
 * dispatch constants, the two tables or the root arguments as bound arguments:
 * they travel in this frame instead, which the caller writes into the block it
 * allocates for the invocation before calling it.
 *
 * Every pointer field is a D3D12 GPU virtual address, read back with an ordinary
 * device load (the same convention dxil_rt_operations.cpp uses for the structure
 * header and the shader records). They are 0 when nothing published them,
 * which every reader treats as "not bound", never as "use the global root
 * arguments" or any other substitute.
 */
struct SM50_RT_FRAME {
  /** Export index of the shader currently running (visible function slot - 1). */
  uint32_t RecordIndex;
  /** Linear dispatch index: DispatchRaysIndex().x + y * DispatchWidth. */
  uint32_t LinearIndex;
  /** Current recursion depth, 0 in the raygen. */
  uint32_t Depth;
  uint32_t Flags;
  /**
   * The local root arguments of the record this invocation runs as: the address of
   * the bytes right after the record's 32 byte identifier in the application's own
   * shader table, read in place in the D3D12 local root layout - what a D3D12
   * shader reads as its local root signature's arguments.
   *
   * The caller resolves it (EmitShaderRecordLookup) as
   *   table base + record position * table stride + 32
   * out of the four per-table bases and strides of SM50_RT_DISPATCH_ARGUMENTS: the
   * record position is the miss index for a miss shader, the callable index for a
   * callable, the hit group index
   * (`InstanceContributionToHitGroupIndex + RayContributionToHitGroupIndex +
   * GeometryIndex * MultiplierForGeometryContributionToHitGroupIndex`) for a hit
   * group, and 0 for the ray generation shader, which is the single record of its
   * own table. 0 means "no record", which is what an out of range or foreign
   * record resolves to - never the global root arguments.
   */
  uint64_t LocalRootEntry;
  /**
   * D3D12 GPU virtual address of SM50_RT_DISPATCH_ARGUMENTS, as the shader that
   * issued the active trace knows it: the address of the buffer the runtime binds at
   * SM50_RT_BINDING_INDEX_DISPATCH (the runtime publishes it in the block itself,
   * see SM50_RT_DISPATCH_ARGUMENTS::DispatchArgumentsAddress).
   *
   * A ray generation or intersection shader has that buffer as an argument, but a
   * visible function has no bound arguments at all, and it needs the dispatch
   * constants to turn the record of a trace it issues into an export index and into
   * a local root entry (the four tables' bases and strides live there). The address
   * is therefore published alongside the record.
   */
  uint64_t DispatchArguments;
  /**
   * D3D12 GPU virtual address of the per-instance contribution array of the
   * acceleration structure the active trace is tracing: the second qword of that
   * structure's header, which the runtime's acceleration-structure prepass fills
   * with the application's `InstanceContributionToHitGroupIndex` words. 0 when the
   * trace has no such array, i.e. when the structure contributes nothing.
   *
   * D3D12's hit group index includes the instance contribution and Metal has no
   * per-instance field for it, so this is where the tracing shader, the shared
   * candidate callback and a procedural intersection shader read it.
   */
  uint64_t ContributionArray;
  /**
   * The pipeline's two function tables, as the ray generation shader received them
   * (its own arguments, so their GPU addresses are simply the pointer values).
   *
   * A visible function cannot declare them: they are arguments of the ray
   * generation / intersection function, and a call through the visible function
   * table has a fixed argument list. They are published here so a miss /
   * closest-hit / any-hit / callable export can issue its own TraceRay (the
   * intersection function table operand of the traversal) and reach a
   * miss / hit-group / callable record (the visible function table operand).
   */
  uint64_t IntersectionFunctionTable;
  uint64_t VisibleFunctionTable;
  /**
   * The global root arguments and static samplers the runtime bound for this
   * dispatch (SM50_BINDING_INDEX_ROOT_ARGUMENTS / _STATIC_SAMPLERS), as the ray
   * generation shader received them.
   *
   * Every root signature of a dispatch shares one global root signature, so its
   * arguments are the same for every stage of the pipeline; a visible function
   * cannot declare them as arguments, so it reads them through these addresses -
   * which is what lets a hit / miss / callable shader resolve the resources the
   * *global* root signature binds (a UAV it writes, a constant buffer it reads, the
   * acceleration structure it traces) without a second binding ABI.
   */
  uint64_t GlobalRootArguments;
  uint64_t StaticSamplers;
  /** Traced TLAS GPU address; software opacity classification reads its immutable shadow. */
  uint64_t AccelerationStructureAddress;
};

#ifdef __cplusplus
static_assert(
  sizeof(SM50_RT_FRAME) == 80, "SM50_RT_FRAME layout is part of the shader ABI"
);
#endif

/**
 * The hit state a hit shader reads its system values from.
 *
 * The traversal result carries the primitive / instance / geometry / distance /
 * barycentrics, and the ray generation shader publishes them here together with the
 * ray parameters it knows, so a closest-hit or any-hit shader can read exactly the
 * D3D12 hit system values. It is written by the lowered raygen and passed to the
 * hit function as its second argument (see the visible-function ABI in
 * dxil_rt_operations.cpp).
 *
 * Both the compiler and the runtime ABI header use this layout; it is POD and 16
 * byte aligned.
 */
struct SM50_RT_HIT_STATE {
  /** WorldRayOrigin / WorldRayDirection, padded to 4 components. */
  float WorldRayOrigin[4];
  float WorldRayDirection[4];
  /** RayTMin and RayTCurrent. */
  float RayTMin;
  float RayTCurrent;
  /** PrimitiveIndex, InstanceID, InstanceIndex and GeometryIndex. */
  uint32_t PrimitiveIndex;
  uint32_t InstanceID;
  uint32_t InstanceIndex;
  uint32_t GeometryIndex;
  /** HitKind and RayFlags. */
  uint32_t HitKind;
  uint32_t RayFlags;
  /** BuiltInTriangleIntersectionAttributes barycentrics and the front-face flag. */
  float Barycentrics[2];
  uint32_t FrontFace;
  /**
   * ObjectToWorld3x4 / WorldToObject3x4 of the instance being hit, column-major (four
   * columns of three floats, column 3 the translation) exactly as the world_space_data
   * traversal and intersection functions report them. Element [r][c] of the D3D12 3x4 matrix
   * is ObjectToWorld[c * 3 + r].
   */
  float ObjectToWorld[12];
  float WorldToObject[12];
  /** ObjectRayOrigin / ObjectRayDirection: the ray in the hit instance's object space. */
  float ObjectRayOrigin[3];
  float ObjectRayDirection[3];
};

#ifdef __cplusplus
static_assert(
  sizeof(SM50_RT_HIT_STATE) == 196, "SM50_RT_HIT_STATE layout is part of the shader ABI"
);
#endif

/* ------------------------------------------------------------------------- */
/* Reflection                                                                */
/* ------------------------------------------------------------------------- */

enum SM50_RT_FUNCTION_KIND : uint32_t {
  /** The raygen: `[[kernel]]`, launched as the dispatch's compute function. */
  SM50_RT_FUNCTION_RAYGEN = 0,
  /** Intersection export inlined into the GPU-selected bounding-box dispatcher. */
  SM50_RT_FUNCTION_INTERSECTION = 1,
  /** `[[visible]]`; installed at ExportIndex + 1 in the visible function table. */
  SM50_RT_FUNCTION_VISIBLE = 2,
  /**
   * Hardware callback selected by geometry kind. Slot 0 handles triangles and
   * slot 1 bounding boxes. Both select the candidate's hit group from GPU SBT
   * contents; no CPU shader-record snapshot is involved.
   */
  SM50_RT_FUNCTION_DISPATCH = 3,
};

enum SM50_RT_FUNCTION_FLAG {
  SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY = 1u,
  SM50_RT_FUNCTION_FLAG_DEVICE_ATOMIC64_PHASE = 2u,
};

struct SM50_RT_FUNCTION_INFO {
  /** Metal function name, as stored in the metallib. */
  const char *Name;
  /** Matching SM50_RT_EXPORT::EntryPoint (the public export name). */
  const char *ExportName;
  uint32_t Kind; /* SM50_RT_FUNCTION_KIND */
  /**
   * Export index this function belongs to, or SM50_RT_INDEX_NONE when it is not an
   * export (the shared dispatch callback). A DISPATCH record is skipped by the
   * runtime's export bookkeeping and by the visible function table.
   */
  uint32_t ExportIndex;
  /**
   * DISPATCH: intersection table slot (0 triangle, 1 bounding box).
   * VISIBLE: explicit visible table slot for a kernel-side intersection export,
   * or SM50_RT_INDEX_NONE for its usual ExportIndex + 1 slot.
   *
   * Native INTERSECTION bodies remain internal to their hardware dispatcher.
   */
  uint32_t TableIndex;
  /** SM50_RT_FUNCTION_FLAG; atomic phase is identical on every pipeline record. */
  uint32_t Flags;
};

/* ------------------------------------------------------------------------- */
/* Entry points                                                              */
/* ------------------------------------------------------------------------- */

/** One export discovered inside a DXIL library container. */
struct sm50_rt_library_export_info {
  /** SM50_RT_STAGE of the export. */
  uint32_t stage;
  /**
   * Offset and length of the UTF-8 public D3D12 export name inside the caller's
   * pNames buffer. This is the library's RDAT unmangled name for the entry point -
   * the name D3D12_EXPORT_DESC and the hit-group shader imports refer to.
   */
  uint32_t name_offset;
  uint32_t name_length;
  /**
   * Offset and length of the DXIL container entry point name (the library's
   * `!dx.entryPoints` operand 1, DXC-mangled) in the same pNames buffer. The
   * runtime needs both spellings: the public one is what the application names,
   * the container one is what the export must be compiled from.
   */
  uint32_t entry_point_offset;
  uint32_t entry_point_length;
};

/**
 * Enumerate the shader exports of a DXIL library container, in container order,
 * so the runtime can honour D3D12_DXIL_LIBRARY_DESC with NumExports == 0 and
 * validate ExportToRename without any DXIL knowledge of its own.
 *
 * Each export reports two names, both written into pNames: the public D3D12 export
 * name (the container's RDAT unmangled name, which is what D3D12_EXPORT_DESC and the
 * hit-group shader imports name) and the container's own entry point name
 * (`!dx.entryPoints` operand 1, which DXC mangles and which is therefore the only
 * name that selects the entry point inside the container). A container compiled
 * without reflection reports its entry point name as both, because that is then the
 * export's exact identity; nothing is derived from the mangled spelling.
 *
 * Two-call protocol: query with pExports == NULL (NameCapacity 0) to learn
 * *pExportCount and *pNamesSize, then call again with sized buffers. On success
 * returns 1 and sets both counts; on failure returns 0 and sets *ppError.
 */
AIRCONV_API int SM50RTEnumerateLibraryExports(
  const void *pBytecode, uint64_t BytecodeSize, struct sm50_rt_library_export_info *pExports,
  uint32_t ExportCapacity, char *pNames, uint64_t NamesSize, uint32_t *pExportCount,
  uint64_t *pNamesSize, sm50_error_t *ppError
);

/**
 * Compile a whole raytracing pipeline.
 *
 * On success returns 1 and writes the AIR bitcode handle (owned by the caller,
 * destroyed with SM50DestroyBitcode) and the serialized metallib. `pFunctions`
 * receives the reflection in the order the runtime must install them; `pFunctions`
 * must hold `FunctionCapacity` records, and `FunctionCapacity` must cover the export
 * count, procedural closest-hit variants, geometry callbacks, and the optional
 * multi-raygen dispatcher, or the call is refused. On failure
 * returns 0 and sets *ppError - the two entry points of this header share the
 * convention, so `!SM50CompileRaytracingPipeline(...)` is the failure test.
 * Unsupported DXIL in an export is an error, never a silently dropped shader.
 */
AIRCONV_API int SM50CompileRaytracingPipeline(
  const struct SM50_RT_PIPELINE_DESC *pDesc, sm50_bitcode_t *ppBitcode,
  struct SM50_COMPILED_BITCODE *pMetallib, struct SM50_RT_FUNCTION_INFO *pFunctions,
  uint32_t FunctionCapacity, uint32_t *pFunctionCount, sm50_error_t *ppError
);

#ifdef __cplusplus
};
#endif

#endif /* __AIRCONV_DXIL_RAYTRACING_H */
