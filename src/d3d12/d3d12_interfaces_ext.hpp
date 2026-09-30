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
 * such as the directly-indexed root signature flags) are provided as `dxmt::` constants
 * instead of redeclared, so either header compiles.
 */

#pragma once
#include "d3d12.h"

// ---- BINDLESS: root signature 1.2 / static sampler flags ----

#ifndef DXMT_D3D12_ROOT_SIGNATURE_1_2_DECLARED
#define DXMT_D3D12_ROOT_SIGNATURE_1_2_DECLARED

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

namespace dxmt {

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

} // namespace dxmt

// ---- end BINDLESS ----

// ---- BARRIERS: ID3D12Device11..14, ID3D12GraphicsCommandList8/9, dynamic PSO state ----
//
// Copied from microsoft/DirectX-Headers commit adbd6f3ba40795c46a8d0f33af00bcb57ff0f0a4
// (include/directx/d3d12.h, 2026-09-23). The llvm-mingw d3d12.h ends at ID3D12Device10 /
// ID3D12GraphicsCommandList7; the declarations below extend those in vtable order and are
// skipped when a header already provides them. The vendored native header has no Device10,
// so the whole block is limited to headers that do.

#ifdef __ID3D12Device10_INTERFACE_DEFINED__

#ifndef DXMT_D3D12_SAMPLER_DESC2_DECLARED
#define DXMT_D3D12_SAMPLER_DESC2_DECLARED
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

namespace dxmt {

// Plain integers: the toolchain enums do not contain these values, so an enum cast is not a
// constant expression.
constexpr UINT kPipelineStateFlagDynamicDepthBias = 0x4;
constexpr UINT kPipelineStateFlagDynamicIndexBufferStripCut = 0x8;
/* D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE; absent from the toolchain enum. */
constexpr UINT kBarrierLayoutDirectQueueGenericReadComputeQueueAccessible = 31;
/* D3D12_HEAP_TYPE_GPU_UPLOAD (D3D12GPUUploadHeaps spec); absent from the toolchain enum. */
constexpr D3D12_HEAP_TYPE kHeapTypeGpuUpload = static_cast<D3D12_HEAP_TYPE>(5);

} // namespace dxmt

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
