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

#pragma once

#include "d3d12.h"

#include <cstdint>
#include <string>
#include <vector>

namespace d3dalloy {

/*
 * Subobjects a DXIL library declares in HLSL (GlobalRootSignature, LocalRootSignature,
 * SubobjectToExportsAssociation, RaytracingShaderConfig, RaytracingPipelineConfig[1],
 * TriangleHitGroup / ProceduralPrimitiveHitGroup, StateObjectConfig), read from the
 * container's RDAT subobject table (DXC DxilRuntimeReflection.h RuntimeDataPartType::
 * SubobjectTable = 6; records are RDAT_SubobjectTypes.inl RuntimeDataSubobjectInfo:
 * { u32 Kind; u32 Name; union { ... } }, kinds from DxilConstants.h SubobjectKind).
 */
enum class DxilSubobjectKind : uint32_t {
  StateObjectConfig = 0,
  GlobalRootSignature = 1,
  LocalRootSignature = 2,
  SubobjectToExportsAssociation = 8,
  RaytracingShaderConfig = 9,
  RaytracingPipelineConfig = 10,
  HitGroup = 11,
  RaytracingPipelineConfig1 = 12,
};

struct DxilSubobject {
  DxilSubobjectKind kind;
  std::wstring name;
  /** StateObjectConfig flags. */
  D3D12_STATE_OBJECT_FLAGS state_object_flags = D3D12_STATE_OBJECT_FLAG_NONE;
  /** Root signatures: the bare RTS0 payload, copied out of the container. */
  std::vector<uint8_t> root_signature;
  /** SubobjectToExportsAssociation. */
  std::wstring associated_subobject;
  std::vector<std::wstring> exports;
  D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
  /** RaytracingPipelineConfig (Flags stays NONE) and RaytracingPipelineConfig1. */
  D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline_config{};
  /** HitGroup; empty names stand for an absent shader. */
  D3D12_HIT_GROUP_TYPE hit_group_type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  std::wstring any_hit, closest_hit, intersection;
};

/**
 * Every subobject a DXIL library container declares, in table order. A container with no
 * RDAT subobject table declares none and succeeds with an empty list; a malformed table is
 * E_INVALIDARG.
 */
HRESULT ReadDxilLibrarySubobjects(const void *blob, size_t size, std::vector<DxilSubobject> &out);

/**
 * Wrap a bare RTS0 payload in a one-part DXBC container, which is what
 * CreateRootSignature consumes. The container hash is not validated by this runtime.
 */
HRESULT WrapRootSignaturePayload(const void *data, size_t size, std::vector<uint8_t> &container);

} // namespace d3dalloy
