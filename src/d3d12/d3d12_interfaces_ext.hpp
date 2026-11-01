/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * D3D12 declarations newer than the vendored include/native/directx/d3d12.h and the
 * llvm-mingw d3d12.h this project builds against. Layouts are copied from Microsoft
 * DirectX-Headers v1.614.1 (include/directx/d3d12.h). Each owner keeps its own section.
 *
 * Declarations that exist in some but not all of the supported headers (enumerators
 * such as the directly-indexed root signature flags) are provided as `d3dalloy::` constants
 * instead of redeclared, so either header compiles.
 */

#pragma once
#include "d3d12.h"

// ---- QUEUE: dynamic priority ABI (DirectX-Headers adbd6f3b, SDK 1.717.1-preview) ----
#ifndef __ID3D12CommandQueue1_INTERFACE_DEFINED__
#define __ID3D12CommandQueue1_INTERFACE_DEFINED__
typedef enum D3D12_COMMAND_QUEUE_PROCESS_PRIORITY {
  D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_NORMAL = 0,
  D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH = 1
} D3D12_COMMAND_QUEUE_PROCESS_PRIORITY;
typedef enum D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY {
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_IDLE = 0,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_DEFAULT = 1,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_NORMAL_0 = 2,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_0 = 18,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_1 = 19,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_2 = 20,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_3 = 21,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_4 = 22,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_5 = 23,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_6 = 24,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_7 = 25,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_8 = 26,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_9 = 27,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_10 = 28,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_11 = 29,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_12 = 30,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_13 = 31,
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_HARD_REALTIME = 32
} D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY;
typedef struct D3D12_FEATURE_DATA_HARDWARE_SCHEDULING_QUEUE_GROUPINGS {
  UINT ComputeQueuesPer3DQueue;
} D3D12_FEATURE_DATA_HARDWARE_SCHEDULING_QUEUE_GROUPINGS;
#if defined(__cplusplus) && !defined(CINTERFACE)
MIDL_INTERFACE("3a3c3165-0ee7-4b8e-a0af-6356b4c3bbb9")
ID3D12CommandQueue1 : public ID3D12CommandQueue {
  virtual HRESULT STDMETHODCALLTYPE SetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY priority) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY *priority) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY priority) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY *priority) = 0;
};
__CRT_UUID_DECL(ID3D12CommandQueue1, 0x3a3c3165, 0x0ee7, 0x4b8e, 0xa0, 0xaf, 0x63, 0x56, 0xb4, 0xc3, 0xbb, 0xb9)
#endif
#endif
namespace d3dalloy {
constexpr UINT kCommandQueueFlagAllowDynamicPriority = 0x2;
constexpr UINT kFeatureHardwareSchedulingQueueGroupings = 60;
}
// ---- end QUEUE ----

// ---- BINDLESS: root signature 1.2 / static sampler flags ----

#ifndef D3DALLOY_D3D12_ROOT_SIGNATURE_1_2_DECLARED
#define D3DALLOY_D3D12_ROOT_SIGNATURE_1_2_DECLARED

typedef enum D3D12_SAMPLER_FLAGS {
  D3D12_SAMPLER_FLAG_NONE = 0,
  D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR = 0x1,
  D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES = 0x2
} D3D12_SAMPLER_FLAGS;

typedef struct D3D12_STATIC_SAMPLER_DESC1 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU;
  D3D12_TEXTURE_ADDRESS_MODE AddressV;
  D3D12_TEXTURE_ADDRESS_MODE AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  D3D12_STATIC_BORDER_COLOR BorderColor;
  FLOAT MinLOD;
  FLOAT MaxLOD;
  UINT ShaderRegister;
  UINT RegisterSpace;
  D3D12_SHADER_VISIBILITY ShaderVisibility;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_STATIC_SAMPLER_DESC1;

typedef struct D3D12_ROOT_SIGNATURE_DESC2 {
  UINT NumParameters;
  const D3D12_ROOT_PARAMETER1 *pParameters;
  UINT NumStaticSamplers;
  const D3D12_STATIC_SAMPLER_DESC1 *pStaticSamplers;
  D3D12_ROOT_SIGNATURE_FLAGS Flags;
} D3D12_ROOT_SIGNATURE_DESC2;

#endif

namespace d3dalloy {

constexpr D3D_ROOT_SIGNATURE_VERSION kRootSignatureVersion_1_2 = static_cast<D3D_ROOT_SIGNATURE_VERSION>(0x3);

constexpr UINT kRootSignatureFlagLocalRootSignature = 0x80;
constexpr UINT kRootSignatureFlagCbvSrvUavHeapDirectlyIndexed = 0x400;
constexpr UINT kRootSignatureFlagSamplerHeapDirectlyIndexed = 0x800;
constexpr UINT kRootSignatureFlagsDirectlyIndexed =
    kRootSignatureFlagCbvSrvUavHeapDirectlyIndexed | kRootSignatureFlagSamplerHeapDirectlyIndexed;

/* The vendored enum only spans 0..2, so the *_UINT values are plain integers here: a
 * cast past an unfixed enum's range is not a constant expression. */
constexpr UINT kStaticBorderColorOpaqueBlackUint = 3;
constexpr UINT kStaticBorderColorOpaqueWhiteUint = 4;

/**
 * D3D12_VERSIONED_ROOT_SIGNATURE_DESC as declared by DirectX-Headers, with the
 * Desc_1_2 union member the vendored headers lack. It is layout compatible with the
 * application's structure: the union members only differ in their static sampler
 * pointer type.
 */
struct VersionedRootSignatureDesc {
  D3D_ROOT_SIGNATURE_VERSION Version;
  union {
    D3D12_ROOT_SIGNATURE_DESC Desc_1_0;
    D3D12_ROOT_SIGNATURE_DESC1 Desc_1_1;
    D3D12_ROOT_SIGNATURE_DESC2 Desc_1_2;
  };
};

static_assert(sizeof(VersionedRootSignatureDesc) == sizeof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC));
static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC1) == sizeof(D3D12_STATIC_SAMPLER_DESC) + sizeof(UINT));

} // namespace d3dalloy

// ---- end BINDLESS ----

// ---- BARRIERS: ID3D12Device11..14, ID3D12GraphicsCommandList8/9, dynamic PSO state ----
//
// Copied from microsoft/DirectX-Headers commit adbd6f3ba40795c46a8d0f33af00bcb57ff0f0a4
// (include/directx/d3d12.h, 2026-09-23). The llvm-mingw d3d12.h ends at ID3D12Device10 /
// ID3D12GraphicsCommandList7; the declarations below extend those in vtable order and are
// skipped when a header already provides them. The vendored native header has no Device10,
// so the whole block is limited to headers that do.

#ifdef __ID3D12Device10_INTERFACE_DEFINED__

#ifndef D3DALLOY_D3D12_SAMPLER_DESC2_DECLARED
#define D3DALLOY_D3D12_SAMPLER_DESC2_DECLARED
typedef struct D3D12_SAMPLER_DESC2 {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU;
  D3D12_TEXTURE_ADDRESS_MODE AddressV;
  D3D12_TEXTURE_ADDRESS_MODE AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union {
    FLOAT FloatBorderColor[4];
    UINT UintBorderColor[4];
  };
  FLOAT MinLOD;
  FLOAT MaxLOD;
  D3D12_SAMPLER_FLAGS Flags;
} D3D12_SAMPLER_DESC2;
#endif

namespace d3dalloy {

// Plain integers: the toolchain enums do not contain these values, so an enum cast is not a
// constant expression.
constexpr UINT kPipelineStateFlagDynamicDepthBias = 0x4;
constexpr UINT kPipelineStateFlagDynamicIndexBufferStripCut = 0x8;
/* D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE; absent from the toolchain enum. */
constexpr UINT kBarrierLayoutDirectQueueGenericReadComputeQueueAccessible = 31;
/* D3D12_HEAP_TYPE_GPU_UPLOAD (D3D12GPUUploadHeaps spec); absent from the toolchain enum. */
constexpr D3D12_HEAP_TYPE kHeapTypeGpuUpload = static_cast<D3D12_HEAP_TYPE>(5);

} // namespace d3dalloy

#if defined(__cplusplus) && !defined(CINTERFACE)

#ifndef __ID3D12Device11_INTERFACE_DEFINED__
#define __ID3D12Device11_INTERFACE_DEFINED__
MIDL_INTERFACE("5405c344-d457-444e-b4dd-2366e45aee39")
ID3D12Device11 : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2 *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE dest_descriptor) = 0;
};
__CRT_UUID_DECL(ID3D12Device11, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39)
#endif

#ifndef __ID3D12Device12_INTERFACE_DEFINED__
#define __ID3D12Device12_INTERFACE_DEFINED__
MIDL_INTERFACE("5af5c532-4c91-4cd0-b541-15a405395fc5")
ID3D12Device12 : public ID3D12Device11 {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  virtual D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo3(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask, UINT resource_descs_count,
      const D3D12_RESOURCE_DESC1 *resource_descs, const UINT32 *castable_formats_counts,
      const DXGI_FORMAT *const *castable_formats, D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) = 0;
#else
  virtual D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo3(
      UINT visible_mask, UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
      const UINT32 *castable_formats_counts, const DXGI_FORMAT *const *castable_formats,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) = 0;
#endif
};
__CRT_UUID_DECL(ID3D12Device12, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5)
#endif

#ifndef __ID3D12Device13_INTERFACE_DEFINED__
#define __ID3D12Device13_INTERFACE_DEFINED__
MIDL_INTERFACE("14eecffc-4df8-40f7-a118-5c816f45695e")
ID3D12Device13 : public ID3D12Device12 {
  virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(const void *address, SIZE_T size, REFIID riid,
                                                                 void **heap) = 0;
};
__CRT_UUID_DECL(ID3D12Device13, 0x14eecffc, 0x4df8, 0x40f7, 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e)
#endif

#ifndef __ID3D12Device14_INTERFACE_DEFINED__
#define __ID3D12Device14_INTERFACE_DEFINED__
MIDL_INTERFACE("5f6e592d-d895-44c2-8e4a-88ad4926d323")
ID3D12Device14 : public ID3D12Device13 {
  virtual HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT node_mask, const void *library_blob, SIZE_T blob_length, LPCWSTR subobject_name, REFIID riid,
      void **root_signature) = 0;
};
__CRT_UUID_DECL(ID3D12Device14, 0x5f6e592d, 0xd895, 0x44c2, 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23)
#endif

#ifndef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
MIDL_INTERFACE("ee936ef9-599d-4d28-938e-23c4ad05ce51")
ID3D12GraphicsCommandList8 : public ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT front_stencil_ref, UINT back_stencil_ref) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList8, 0xee936ef9, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce,
                0x51)
#endif

#ifndef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
MIDL_INTERFACE("34ed2808-ffe6-4c2b-b11a-cabd2b0c59e1")
ID3D12GraphicsCommandList9 : public ID3D12GraphicsCommandList8 {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT depth_bias, FLOAT depth_bias_clamp,
                                                FLOAT slope_scaled_depth_bias) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList9, 0x34ed2808, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59,
                0xe1)
#endif

#endif // __cplusplus && !CINTERFACE

#endif // __ID3D12Device10_INTERFACE_DEFINED__

// ---- end BARRIERS ----

// ---- WORKGRAPH: work graph state objects, ID3D12WorkGraphProperties, ID3D12GraphicsCommandList10 ----
//
// Copied from microsoft/DirectX-Headers commit adbd6f3ba40795c46a8d0f33af00bcb57ff0f0a4
// (include/directx/d3d12.h). The llvm-mingw d3d12.h has none of the work graph declarations;
// enum values it lacks are d3dalloy:: constants so no out-of-range enum cast is needed.

#ifdef __ID3D12Device10_INTERFACE_DEFINED__

#ifndef D3DALLOY_D3D12_WORK_GRAPHS_DECLARED
#define D3DALLOY_D3D12_WORK_GRAPHS_DECLARED

typedef enum D3D12_WORK_GRAPHS_TIER {
  D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED = 0,
  D3D12_WORK_GRAPHS_TIER_1_0 = 10,
} D3D12_WORK_GRAPHS_TIER;
typedef enum D3D12_EXECUTE_INDIRECT_TIER {
  D3D12_EXECUTE_INDIRECT_TIER_1_0 = 10,
  D3D12_EXECUTE_INDIRECT_TIER_1_1 = 11,
} D3D12_EXECUTE_INDIRECT_TIER;
typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS21 {
  D3D12_WORK_GRAPHS_TIER WorkGraphsTier;
  D3D12_EXECUTE_INDIRECT_TIER ExecuteIndirectTier;
  BOOL SampleCmpGradientAndBiasSupported;
  BOOL ExtendedCommandInfoSupported;
} D3D12_FEATURE_DATA_D3D12_OPTIONS21;

typedef struct D3D12_PROGRAM_IDENTIFIER {
  UINT64 OpaqueData[4];
} D3D12_PROGRAM_IDENTIFIER;

typedef struct D3D12_NODE_ID {
  LPCWSTR Name;
  UINT ArrayIndex;
} D3D12_NODE_ID;

typedef struct D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS {
  UINT64 MinSizeInBytes;
  UINT64 MaxSizeInBytes;
  UINT SizeGranularityInBytes;
} D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS;

typedef struct D3D12_NODE_OUTPUT_OVERRIDES {
  UINT OutputIndex;
  const D3D12_NODE_ID *pNewName;
  const BOOL *pAllowSparseNodes;
  const UINT *pMaxRecords;
  const UINT *pMaxRecordsSharedWithOutputIndex;
} D3D12_NODE_OUTPUT_OVERRIDES;

typedef struct D3D12_BROADCASTING_LAUNCH_OVERRIDES {
  const UINT *pLocalRootArgumentsTableIndex;
  const BOOL *pProgramEntry;
  const D3D12_NODE_ID *pNewName;
  const D3D12_NODE_ID *pShareInputOf;
  const UINT *pDispatchGrid;
  const UINT *pMaxDispatchGrid;
  UINT NumOutputOverrides;
  const D3D12_NODE_OUTPUT_OVERRIDES *pOutputOverrides;
} D3D12_BROADCASTING_LAUNCH_OVERRIDES;

/* The coalescing, thread and common-compute override structs share one layout. */
typedef struct D3D12_COMMON_COMPUTE_NODE_OVERRIDES {
  const UINT *pLocalRootArgumentsTableIndex;
  const BOOL *pProgramEntry;
  const D3D12_NODE_ID *pNewName;
  const D3D12_NODE_ID *pShareInputOf;
  UINT NumOutputOverrides;
  const D3D12_NODE_OUTPUT_OVERRIDES *pOutputOverrides;
} D3D12_COMMON_COMPUTE_NODE_OVERRIDES;
typedef D3D12_COMMON_COMPUTE_NODE_OVERRIDES D3D12_COALESCING_LAUNCH_OVERRIDES;
typedef D3D12_COMMON_COMPUTE_NODE_OVERRIDES D3D12_THREAD_LAUNCH_OVERRIDES;

typedef struct D3D12_SHADER_NODE {
  LPCWSTR Shader;
  UINT OverridesType; /* D3D12_NODE_OVERRIDES_TYPE */
  union {
    const D3D12_BROADCASTING_LAUNCH_OVERRIDES *pBroadcastingLaunchOverrides;
    const D3D12_COALESCING_LAUNCH_OVERRIDES *pCoalescingLaunchOverrides;
    const D3D12_THREAD_LAUNCH_OVERRIDES *pThreadLaunchOverrides;
    const D3D12_COMMON_COMPUTE_NODE_OVERRIDES *pCommonComputeNodeOverrides;
  };
} D3D12_SHADER_NODE;

typedef struct D3D12_NODE {
  UINT NodeType; /* D3D12_NODE_TYPE */
  union {
    D3D12_SHADER_NODE Shader;
  };
} D3D12_NODE;

typedef struct D3D12_WORK_GRAPH_DESC {
  LPCWSTR ProgramName;
  UINT Flags; /* D3D12_WORK_GRAPH_FLAGS */
  UINT NumEntrypoints;
  const D3D12_NODE_ID *pEntrypoints;
  UINT NumExplicitlyDefinedNodes;
  const D3D12_NODE *pExplicitlyDefinedNodes;
} D3D12_WORK_GRAPH_DESC;

typedef struct D3D12_SET_WORK_GRAPH_DESC {
  D3D12_PROGRAM_IDENTIFIER ProgramIdentifier;
  UINT Flags; /* D3D12_SET_WORK_GRAPH_FLAGS */
  D3D12_GPU_VIRTUAL_ADDRESS_RANGE BackingMemory;
  D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE NodeLocalRootArgumentsTable;
} D3D12_SET_WORK_GRAPH_DESC;

typedef struct D3D12_SET_PROGRAM_DESC {
  UINT Type; /* D3D12_PROGRAM_TYPE */
  union {
    D3D12_PROGRAM_IDENTIFIER GenericPipeline;
    D3D12_PROGRAM_IDENTIFIER RaytracingPipeline;
    D3D12_SET_WORK_GRAPH_DESC WorkGraph;
  };
} D3D12_SET_PROGRAM_DESC;

typedef struct D3D12_NODE_CPU_INPUT {
  UINT EntrypointIndex;
  UINT NumRecords;
  const void *pRecords;
  UINT64 RecordStrideInBytes;
} D3D12_NODE_CPU_INPUT;

typedef struct D3D12_NODE_GPU_INPUT {
  UINT EntrypointIndex;
  UINT NumRecords;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE Records;
} D3D12_NODE_GPU_INPUT;

typedef struct D3D12_MULTI_NODE_CPU_INPUT {
  UINT NumNodeInputs;
  const D3D12_NODE_CPU_INPUT *pNodeInputs;
  UINT64 NodeInputStrideInBytes;
} D3D12_MULTI_NODE_CPU_INPUT;

typedef struct D3D12_MULTI_NODE_GPU_INPUT {
  UINT NumNodeInputs;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE NodeInputs;
} D3D12_MULTI_NODE_GPU_INPUT;

typedef struct D3D12_DISPATCH_GRAPH_DESC {
  UINT Mode; /* D3D12_DISPATCH_MODE */
  union {
    D3D12_NODE_CPU_INPUT NodeCPUInput;
    D3D12_GPU_VIRTUAL_ADDRESS NodeGPUInput;
    D3D12_MULTI_NODE_CPU_INPUT MultiNodeCPUInput;
    D3D12_GPU_VIRTUAL_ADDRESS MultiNodeGPUInput;
  };
} D3D12_DISPATCH_GRAPH_DESC;

#endif // D3DALLOY_D3D12_WORK_GRAPHS_DECLARED

namespace d3dalloy {

constexpr UINT kFeatureD3D12Options21 = 53;
constexpr UINT kWorkGraphsTier1_0 = 10;
constexpr UINT kStateObjectTypeExecutable = 4;
constexpr UINT kStateSubobjectTypeWorkGraph = 13;
constexpr UINT kWorkGraphFlagIncludeAllAvailableNodes = 0x1;
constexpr UINT kNodeTypeShader = 0;
constexpr UINT kNodeOverridesNone = 0;
constexpr UINT kNodeOverridesBroadcasting = 1;
constexpr UINT kNodeOverridesCoalescing = 2;
constexpr UINT kNodeOverridesThread = 3;
constexpr UINT kNodeOverridesCommonCompute = 4;
constexpr UINT kProgramTypeWorkGraph = 5;
constexpr UINT kSetWorkGraphFlagInitialize = 0x1;
constexpr UINT kDispatchModeNodeCPUInput = 0;
constexpr UINT kDispatchModeNodeGPUInput = 1;
constexpr UINT kDispatchModeMultiNodeCPUInput = 2;
constexpr UINT kDispatchModeMultiNodeGPUInput = 3;
constexpr UINT kWorkGraphsBackingMemoryAlignment = 8;
constexpr UINT kWorkGraphsMaxNodeDepth = 32;

} // namespace d3dalloy

#if defined(__cplusplus) && !defined(CINTERFACE)

#ifndef __ID3D12StateObjectProperties1_INTERFACE_DEFINED__
#define __ID3D12StateObjectProperties1_INTERFACE_DEFINED__
MIDL_INTERFACE("460caac7-1d24-446a-a184-ca67db494138")
ID3D12StateObjectProperties1 : public ID3D12StateObjectProperties {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  virtual D3D12_PROGRAM_IDENTIFIER *STDMETHODCALLTYPE GetProgramIdentifier(D3D12_PROGRAM_IDENTIFIER *__ret,
                                                                          LPCWSTR program_name) = 0;
#else
  virtual D3D12_PROGRAM_IDENTIFIER STDMETHODCALLTYPE GetProgramIdentifier(LPCWSTR program_name) = 0;
#endif
};
__CRT_UUID_DECL(ID3D12StateObjectProperties1, 0x460caac7, 0x1d24, 0x446a, 0xa1, 0x84, 0xca, 0x67, 0xdb, 0x49, 0x41,
                0x38)
#endif

#ifndef __ID3D12WorkGraphProperties_INTERFACE_DEFINED__
#define __ID3D12WorkGraphProperties_INTERFACE_DEFINED__
MIDL_INTERFACE("065acf71-f863-4b89-82f4-02e4d5886757")
ID3D12WorkGraphProperties : public IUnknown {
  virtual UINT STDMETHODCALLTYPE GetNumWorkGraphs() = 0;
  virtual LPCWSTR STDMETHODCALLTYPE GetProgramName(UINT work_graph_index) = 0;
  virtual UINT STDMETHODCALLTYPE GetWorkGraphIndex(LPCWSTR program_name) = 0;
  virtual UINT STDMETHODCALLTYPE GetNumNodes(UINT work_graph_index) = 0;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  virtual D3D12_NODE_ID *STDMETHODCALLTYPE GetNodeID(D3D12_NODE_ID *__ret, UINT work_graph_index,
                                                     UINT node_index) = 0;
#else
  virtual D3D12_NODE_ID STDMETHODCALLTYPE GetNodeID(UINT work_graph_index, UINT node_index) = 0;
#endif
  virtual UINT STDMETHODCALLTYPE GetNodeIndex(UINT work_graph_index, D3D12_NODE_ID node_id) = 0;
  virtual UINT STDMETHODCALLTYPE GetNodeLocalRootArgumentsTableIndex(UINT work_graph_index, UINT node_index) = 0;
  virtual UINT STDMETHODCALLTYPE GetNumEntrypoints(UINT work_graph_index) = 0;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  virtual D3D12_NODE_ID *STDMETHODCALLTYPE GetEntrypointID(D3D12_NODE_ID *__ret, UINT work_graph_index,
                                                           UINT entrypoint_index) = 0;
#else
  virtual D3D12_NODE_ID STDMETHODCALLTYPE GetEntrypointID(UINT work_graph_index, UINT entrypoint_index) = 0;
#endif
  virtual UINT STDMETHODCALLTYPE GetEntrypointIndex(UINT work_graph_index, D3D12_NODE_ID node_id) = 0;
  virtual UINT STDMETHODCALLTYPE GetEntrypointRecordSizeInBytes(UINT work_graph_index, UINT entrypoint_index) = 0;
  virtual void STDMETHODCALLTYPE GetWorkGraphMemoryRequirements(
      UINT work_graph_index, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *requirements) = 0;
  virtual UINT STDMETHODCALLTYPE GetEntrypointRecordAlignmentInBytes(UINT work_graph_index,
                                                                     UINT entrypoint_index) = 0;
};
__CRT_UUID_DECL(ID3D12WorkGraphProperties, 0x065acf71, 0xf863, 0x4b89, 0x82, 0xf4, 0x02, 0xe4, 0xd5, 0x88, 0x67,
                0x57)
#endif

#ifndef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
MIDL_INTERFACE("7013c015-d161-4b63-a08c-238552dd8acc")
ID3D12GraphicsCommandList10 : public ID3D12GraphicsCommandList9 {
  virtual void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC *desc) = 0;
  virtual void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC *desc) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList10, 0x7013c015, 0xd161, 0x4b63, 0xa0, 0x8c, 0x23, 0x85, 0x52, 0xdd, 0x8a,
                0xcc)
#endif

#endif // __cplusplus && !CINTERFACE

#endif // __ID3D12Device10_INTERFACE_DEFINED__

// ---- end WORKGRAPH ----

// ---- OPACITY MICROMAPS: DirectX-Headers v1.619.5 final ABI ----
#ifdef __ID3D12Device5_INTERFACE_DEFINED__
// Existing enums/unions cannot be extended in C++. The namespaced union views below
// preserve the public layout, including the pointer pair (not an invented DESC1 API).
#ifndef D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT
#define D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT 128
#define D3D12_RAYTRACING_OPACITY_MICROMAP_DESCS_BYTE_ALIGNMENT 4
#define D3D12_RAYTRACING_OPACITY_MICROMAP_OC1_MAX_SUBDIVISION_LEVEL 12
inline constexpr D3D12_RAYTRACING_TIER D3D12_RAYTRACING_TIER_1_2 = D3D12_RAYTRACING_TIER(12);
// Numeric constants extend the ABI without constructing out-of-range constexpr
// values of the older headers' unscoped enums. Cast when filling ABI fields.
inline constexpr UINT D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY = 2;
inline constexpr UINT D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES = 2;
inline constexpr UINT D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE = 0x40;
inline constexpr UINT D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS = 0x80;
inline constexpr UINT D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS = 0x400;
inline constexpr UINT D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE = 0x10;
inline constexpr UINT D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS = 0x20;
inline constexpr UINT D3D12_RAY_FLAG_FORCE_OMM_2_STATE = 0x400;
typedef enum D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT {
  D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE = 1,
  D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE = 2
} D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT;
typedef enum D3D12_RAYTRACING_OPACITY_MICROMAP_STATE {
  D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_TRANSPARENT = 0,
  D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_OPAQUE = 1,
  D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_UNKNOWN_TRANSPARENT = 2,
  D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_UNKNOWN_OPAQUE = 3
} D3D12_RAYTRACING_OPACITY_MICROMAP_STATE;
typedef enum D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX {
  D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT = -1,
  D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE = -2,
  D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_TRANSPARENT = -3,
  D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE = -4
} D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX;
typedef struct D3D12_RAYTRACING_OPACITY_MICROMAP_DESC {
  UINT ByteOffset;
  UINT SubdivisionLevel : 16;
  D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT Format : 16;
} D3D12_RAYTRACING_OPACITY_MICROMAP_DESC;
typedef struct D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY {
  UINT Count, SubdivisionLevel;
  D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT Format;
} D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY;
typedef struct D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC {
  UINT NumOmmHistogramEntries;
  const D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY *pOmmHistogram;
  D3D12_GPU_VIRTUAL_ADDRESS InputBuffer;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE PerOmmDescs;
} D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC;
typedef struct D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC {
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE OpacityMicromapIndexBuffer;
  DXGI_FORMAT OpacityMicromapIndexFormat;
  UINT OpacityMicromapBaseLocation;
  D3D12_GPU_VIRTUAL_ADDRESS OpacityMicromapArray;
} D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC;
typedef struct D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC {
  const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC *pTriangles;
  const D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC *pOmmLinkage;
} D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC;
typedef enum D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER_POSTAMBLE_TYPE {
  D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER_POSTAMBLE_TYPE_NONE = 0,
  D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER_POSTAMBLE_TYPE_BOTTOM_LEVEL_POINTERS = 0,
  D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER_POSTAMBLE_TYPE_BLOCKS = 0xffffffffu
} D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER_POSTAMBLE_TYPE;
typedef struct D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER1 {
  D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER DriverMatchingIdentifier;
  UINT64 SerializedSizeInBytesIncludingHeader, DeserializedSizeInBytes;
  union { UINT NumBottomLevelAccelerationStructurePointersAfterHeader; UINT NumBlocks; };
  D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER_POSTAMBLE_TYPE HeaderPostambleType;
} D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER1;
typedef enum D3D12_SERIALIZED_BLOCK_TYPE {
  D3D12_RAYTRACING_SERIALIZED_BLOCK_TYPE_OPACITY_MICROMAPS = 0
} D3D12_RAYTRACING_SERIALIZED_BLOCK_TYPE;
typedef struct D3D12_RAYTRACING_SERIALIZED_BLOCK {
  D3D12_RAYTRACING_SERIALIZED_BLOCK_TYPE Type;
  UINT64 NumBlockPointersAfterHeader;
} D3D12_RAYTRACING_SERIALIZED_BLOCK;
typedef enum D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_TYPE {
  D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_CURRENT_SIZE = 0,
  D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_TOOLS_VISUALIZATION = 1
} D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_TYPE;
typedef struct D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_DESC {
  D3D12_GPU_VIRTUAL_ADDRESS DestBuffer;
  D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_TYPE InfoType;
} D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_DESC;
typedef struct D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_CURRENT_SIZE_DESC {
  UINT64 CurrentSizeInBytes;
} D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_CURRENT_SIZE_DESC;
typedef struct D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_TOOLS_VISUALIZATION_DESC {
  UINT64 DecodedSizeInBytes;
} D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_POSTBUILD_INFO_TOOLS_VISUALIZATION_DESC;
#endif
namespace d3dalloy {
struct OmmGeometryDesc {
  D3D12_RAYTRACING_GEOMETRY_TYPE Type;
  D3D12_RAYTRACING_GEOMETRY_FLAGS Flags;
  union {
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC Triangles;
    D3D12_RAYTRACING_GEOMETRY_AABBS_DESC AABBs;
    D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC OmmTriangles;
  };
};
struct OmmBuildInputs {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE Type;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags;
  UINT NumDescs;
  D3D12_ELEMENTS_LAYOUT DescsLayout;
  union {
    D3D12_GPU_VIRTUAL_ADDRESS InstanceDescs;
    const OmmGeometryDesc *pGeometryDescs;
    const OmmGeometryDesc *const *ppGeometryDescs;
    const D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC *pOpacityMicromapArrayDesc;
  };
};
static_assert(sizeof(OmmGeometryDesc) == sizeof(D3D12_RAYTRACING_GEOMETRY_DESC));
static_assert(sizeof(OmmBuildInputs) == sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS));
static_assert(sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC) == 8);
static_assert(sizeof(D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC) == 32);
static_assert(sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER1) == 56);
static_assert(sizeof(D3D12_RAYTRACING_SERIALIZED_BLOCK) == 16);
} // namespace d3dalloy
// ---- end OPACITY MICROMAPS ----
#endif // __ID3D12Device5_INTERFACE_DEFINED__

// Final SDK1.619.5 OPTIONS22 ABI; selected native/llvm-mingw headers predate it.
#if !defined(D3D12_SDK_VERSION) || D3D12_SDK_VERSION < 619
inline constexpr UINT D3D12_FEATURE_D3D12_OPTIONS22 = 65;
typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS22 {
  BOOL ShaderExecutionReorderingActuallyReorders;
  BOOL CreateByteOffsetViewsSupported;
  UINT Max1DDispatchSize;
  UINT Max1DDispatchMeshSize;
} D3D12_FEATURE_DATA_D3D12_OPTIONS22;
#endif
static_assert(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS22) == 16);

// ---- LINEAR ALGEBRA: pinned September 2026 preview ABI (not the later V2 draft) ----
#if defined(__ID3D12GraphicsCommandList10_INTERFACE_DEFINED__) && defined(__cplusplus) && !defined(CINTERFACE)
#ifndef __ID3D12DevicePreview_INTERFACE_DEFINED__
#define __ID3D12DevicePreview_INTERFACE_DEFINED__
enum D3D12_LINEAR_ALGEBRA_TIER { D3D12_LINEAR_ALGEBRA_TIER_NOT_SUPPORTED = 0, D3D12_LINEAR_ALGEBRA_TIER_1_0 = 0x10 };
enum D3D12_LINEAR_ALGEBRA_DATATYPE {
  D3D12_LINEAR_ALGEBRA_DATATYPE_SINT16 = 2, D3D12_LINEAR_ALGEBRA_DATATYPE_UINT16 = 3,
  D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32 = 4, D3D12_LINEAR_ALGEBRA_DATATYPE_UINT32 = 5,
  D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16 = 7, D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32 = 8,
  D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8 = 18, D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8 = 19,
  D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT8_E4M3FN = 20, D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT8_E5M2 = 21
};
enum D3D12_LINEAR_ALGEBRA_OPERATION_TYPE {
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_MATRIX_CONSTRUCTION = 0,
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY = 1,
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREADGROUP_MATRIX_MULTIPLY = 2,
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY = 3,
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_OUTER_PRODUCT = 4,
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_ATOMIC_ACCUMULATE_STORE = 5
};
enum D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAGS {
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_NONE = 0,
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED = 1,
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_EMULATED_INPUTS = 2,
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_EMULATED_OUTPUTS = 4,
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_TRANSPOSE = 8
};
struct D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT { D3D12_LINEAR_ALGEBRA_TIER LinearAlgebraTier; };
struct D3D12_LINEAR_ALGEBRA_MATRIX_CONSTRUCTION_SUPPORT { D3D12_LINEAR_ALGEBRA_DATATYPE ComponentType; UINT WaveSize, MinM, MinK, MinN; };
struct D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE { UINT M, K, N; };
struct D3D12_LINEAR_ALGEBRA_WAVE_MATRIX_MULTIPLY_INPUTS {
  UINT WaveSize; D3D12_LINEAR_ALGEBRA_DATATYPE MatrixAComponentType, MatrixBComponentType, AccumulatorComponentType;
};
struct D3D12_LINEAR_ALGEBRA_WAVE_MATRIX_MULTIPLY_SUPPORT {
  D3D12_LINEAR_ALGEBRA_WAVE_MATRIX_MULTIPLY_INPUTS Inputs;
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAGS SupportFlags;
  UINT NumShapes; D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE *Shapes;
};
struct D3D12_LINEAR_ALGEBRA_THREADGROUP_MATRIX_MULTIPLY_SUPPORT {
  D3D12_LINEAR_ALGEBRA_WAVE_MATRIX_MULTIPLY_INPUTS WaveInputs;
  D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE Shape;
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAGS SupportFlags;
  UINT MinThreadGroupSize, MaxThreadGroupSize, PreferredThreadGroupSize;
};
struct D3D12_LINEAR_ALGEBRA_THREAD_VECTOR_MATRIX_MULTIPLY_SUPPORT {
  D3D12_LINEAR_ALGEBRA_DATATYPE VectorInputType, MatrixInputType, BiasInputType, VectorResultType;
  D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAGS SupportFlags;
};
struct D3D12_LINEAR_ALGEBRA_THREAD_OUTER_PRODUCT_SUPPORT {
  D3D12_LINEAR_ALGEBRA_DATATYPE InputComponentType, ResultComponentType; BOOL Supported;
};
struct D3D12_LINEAR_ALGEBRA_ATOMIC_ACCUMULATE_STORE_SUPPORT {
  D3D12_LINEAR_ALGEBRA_DATATYPE ComponentType; BOOL RWByteAddressBufferSupported, GroupSharedSupported;
};
struct D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT {
  D3D12_LINEAR_ALGEBRA_OPERATION_TYPE OperationType;
  union {
    D3D12_LINEAR_ALGEBRA_MATRIX_CONSTRUCTION_SUPPORT MatrixConstruction;
    D3D12_LINEAR_ALGEBRA_WAVE_MATRIX_MULTIPLY_SUPPORT WaveMatrixMultiply;
    D3D12_LINEAR_ALGEBRA_THREADGROUP_MATRIX_MULTIPLY_SUPPORT ThreadGroupMatrixMultiply;
    D3D12_LINEAR_ALGEBRA_THREAD_VECTOR_MATRIX_MULTIPLY_SUPPORT ThreadVectorMatrixMultiply;
    D3D12_LINEAR_ALGEBRA_THREAD_OUTER_PRODUCT_SUPPORT ThreadOuterProductSupport;
    D3D12_LINEAR_ALGEBRA_ATOMIC_ACCUMULATE_STORE_SUPPORT AccumulateStore;
  };
};
enum D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT {
  D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR = 0, D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_COLUMN_MAJOR = 1,
  D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL = 2, D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_OUTER_PRODUCT_OPTIMAL = 3
};
struct D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO {
  UINT DestSize; D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT DestLayout; UINT DestStride, NumRows, NumColumns;
  D3D12_LINEAR_ALGEBRA_DATATYPE DestDataType;
};
struct D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_SRC_INFO {
  UINT SrcSize; D3D12_LINEAR_ALGEBRA_DATATYPE SrcDataType; D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT SrcLayout; UINT SrcStride;
};
struct D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DATA { D3D12_GPU_VIRTUAL_ADDRESS DestVA, SrcVA; };
struct D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO {
  D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO DestInfo;
  D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_SRC_INFO SrcInfo;
  D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DATA DataDesc;
};
typedef UINT (*D3D12_DUMPFILEBEGINCALLBACK)(UINT64 flags);
typedef void (*D3D12_DUMPFILEENDCALLBACK)(const wchar_t *path);
enum D3D12_DUMP_FILE_DRIVER_OPTIONS {
  D3D12_DUMP_FILE_DRIVER_OPTION_NO_OVERHEAD = 1, D3D12_DUMP_FILE_DRIVER_OPTION_MEDIUM_OVERHEAD = 2,
  D3D12_DUMP_FILE_DRIVER_OPTION_HIGH_OVERHEAD = 4, D3D12_DUMP_FILE_DRIVER_OPTION_NO_DATA = 8,
  D3D12_DUMP_FILE_DRIVER_OPTION_SHADER_REGISTERS = 16, D3D12_DUMP_FILE_DRIVER_OPTION_RESOURCES = 32,
  D3D12_DUMP_FILE_DRIVER_OPTION_EVENT_MARKERS = 64
};
enum D3D12_DEVICE_ERROR_CODE {
  D3D12_DEVICE_ERROR_NONE = 0, D3D12_DEVICE_ERROR_DEVICE_HUNG = 7, D3D12_DEVICE_ERROR_PAGE_FAULT = 9,
  D3D12_DEVICE_ERROR_UNKNOWN = 0x88000000
};
MIDL_INTERFACE("8f0856ad-e37c-4d75-be18-fbb3d8c04ced")
ID3D12DevicePreview : public IUnknown {
  virtual void STDMETHODCALLTYPE GetLinearAlgebraMatrixConversionDestinationInfo(D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO *) = 0;
  virtual HRESULT STDMETHODCALLTYPE ConfigureDumpFile(D3D12_DUMP_FILE_DRIVER_OPTIONS) = 0;
  virtual void STDMETHODCALLTYPE SetDumpFileCallbacks(D3D12_DUMPFILEBEGINCALLBACK, D3D12_DUMPFILEENDCALLBACK) = 0;
  virtual HRESULT STDMETHODCALLTYPE AddBlobToDumpFile(void *, UINT, UINT64) = 0;
  virtual void STDMETHODCALLTYPE RetainDumpFile(BOOL) = 0;
  virtual D3D12_DEVICE_ERROR_CODE STDMETHODCALLTYPE GetDeviceErrorCode() = 0;
};
__CRT_UUID_DECL(ID3D12DevicePreview, 0x8f0856ad, 0xe37c, 0x4d75, 0xbe, 0x18, 0xfb, 0xb3, 0xd8, 0xc0, 0x4c, 0xed)
#endif
#ifndef __ID3D12GraphicsCommandList11_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandList11_INTERFACE_DEFINED__
MIDL_INTERFACE("823aee5b-c932-4b15-b4c4-4a28e7c34dea")
ID3D12GraphicsCommandList11 : public ID3D12GraphicsCommandList10 {
  virtual void STDMETHODCALLTYPE WaitBarrier(UINT, const D3D12_BARRIER_GROUP *, UINT, ID3D12Fence *const *, const UINT64 *) = 0;
  virtual void STDMETHODCALLTYPE SignalBarrier(UINT, const D3D12_BARRIER_GROUP *, ID3D12Fence *, UINT64) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandList11, 0x823aee5b, 0xc932, 0x4b15, 0xb4, 0xc4, 0x4a, 0x28, 0xe7, 0xc3, 0x4d, 0xea)
#endif
#ifndef __ID3D12GraphicsCommandListPreview_INTERFACE_DEFINED__
#define __ID3D12GraphicsCommandListPreview_INTERFACE_DEFINED__
MIDL_INTERFACE("447e915c-c6f9-47e8-84d9-dc4f4495d230")
ID3D12GraphicsCommandListPreview : public ID3D12GraphicsCommandList11 {
  virtual void STDMETHODCALLTYPE SetWorkGraphMaximumGPUInputRecords(UINT, UINT) = 0;
  virtual void STDMETHODCALLTYPE ConvertLinearAlgebraMatrix(const D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO *, UINT) = 0;
};
__CRT_UUID_DECL(ID3D12GraphicsCommandListPreview, 0x447e915c, 0xc6f9, 0x47e8, 0x84, 0xd9, 0xdc, 0x4f, 0x44, 0x95, 0xd2, 0x30)
#endif
static_assert(sizeof(D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT) == (sizeof(void *) == 8 ? 56 : 48));
static_assert(sizeof(D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO) == 56);
#endif
// ---- end LINEAR ALGEBRA ----

