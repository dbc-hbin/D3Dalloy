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

#include "d3d12_dxil_subobjects.hpp"

#include "DXBCParser/BlobContainer.h"

#include <cstring>
#include <new>

namespace d3dalloy {

namespace {

/*
 * Rust (d3dalloy_shader_rdat_subobjects) validates the RDAT parts and each subobject
 * record of DxilRuntimeReflection.h RuntimeDataPartType::SubobjectTable = 6.
 * Record payloads after { Kind, Name } (RDAT_SubobjectTypes.inl), verified against
 * DXC 1.9 output of graphics-translator/tests/dxil/raytracing-stateobj.hlsl:
 *   StateObjectConfig            { u32 Flags }
 *   Global/LocalRootSignature    { u32 RawBytesOffset; u32 Size }
 *   SubobjectToExportsAssociation{ u32 Subobject (string); u32 Exports (index array of strings) }
 *   RaytracingShaderConfig       { u32 MaxPayloadSizeInBytes; u32 MaxAttributeSizeInBytes }
 *   RaytracingPipelineConfig     { u32 MaxTraceRecursionDepth }
 *   HitGroup                     { u32 Type; u32 AnyHit; u32 ClosestHit; u32 Intersection }
 *   RaytracingPipelineConfig1    { u32 MaxTraceRecursionDepth; u32 Flags }
 */

/* Names are HLSL identifiers and export names (ASCII); widened code unit by code unit. */
std::wstring Widen(const uint8_t *bytes, D3DalloyShaderSpan span) {
  const char *text = reinterpret_cast<const char *>(bytes + span.offset);
  return std::wstring(text, text + span.size);
}

} // namespace

HRESULT ReadDxilLibrarySubobjects(const void *blob, size_t size, std::vector<DxilSubobject> &out) {
  out.clear();
  D3DalloyRdatSubobjects table;
  if (!blob || !size || size > UINT32_MAX || d3dalloy_shader_rdat_subobjects(blob, size, &table))
    return E_INVALIDARG;
  if (!table.present)
    return S_OK;
  const auto *bytes = static_cast<const uint8_t *>(blob);
  try {
    out.reserve(table.record_count);
    for (uint32_t i = 0; i < table.record_count; ++i) {
      D3DalloyRdatSubobject record;
      switch (d3dalloy_shader_rdat_subobject(blob, size, &table, i, &record)) {
      case D3DALLOY_SHADER_BYTECODE_SUCCESS:
        break;
      case D3DALLOY_SHADER_BYTECODE_RDAT_KIND:
        /* A subobject kind this table cannot describe would be dropped silently. */
        return E_NOTIMPL;
      default:
        return E_INVALIDARG;
      }
      const uint32_t *w = record.words;
      DxilSubobject subobject{};
      subobject.kind = static_cast<DxilSubobjectKind>(record.kind);
      subobject.name = Widen(bytes, record.name);
      switch (subobject.kind) {
      case DxilSubobjectKind::StateObjectConfig:
        subobject.state_object_flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(w[0]);
        break;
      case DxilSubobjectKind::GlobalRootSignature:
      case DxilSubobjectKind::LocalRootSignature:
        subobject.root_signature.assign(bytes + record.root_signature.offset,
                                        bytes + record.root_signature.offset + record.root_signature.size);
        break;
      case DxilSubobjectKind::SubobjectToExportsAssociation:
        subobject.associated_subobject = Widen(bytes, record.strings[0]);
        subobject.exports.reserve(record.export_count);
        for (uint32_t j = 0; j < record.export_count; ++j) {
          D3DalloyShaderSpan name;
          if (d3dalloy_shader_rdat_export(blob, size, &table, &record, j, &name))
            return E_INVALIDARG;
          subobject.exports.push_back(Widen(bytes, name));
        }
        break;
      case DxilSubobjectKind::RaytracingShaderConfig:
        subobject.shader_config = {w[0], w[1]};
        break;
      case DxilSubobjectKind::RaytracingPipelineConfig:
        subobject.pipeline_config = {w[0], D3D12_RAYTRACING_PIPELINE_FLAG_NONE};
        break;
      case DxilSubobjectKind::RaytracingPipelineConfig1:
        subobject.pipeline_config = {w[0], static_cast<D3D12_RAYTRACING_PIPELINE_FLAGS>(w[1])};
        break;
      case DxilSubobjectKind::HitGroup:
        subobject.hit_group_type = static_cast<D3D12_HIT_GROUP_TYPE>(w[0]);
        subobject.any_hit = Widen(bytes, record.strings[0]);
        subobject.closest_hit = Widen(bytes, record.strings[1]);
        subobject.intersection = Widen(bytes, record.strings[2]);
        break;
      }
      out.push_back(std::move(subobject));
    }
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

HRESULT WrapRootSignaturePayload(const void *data, size_t size, std::vector<uint8_t> &container) {
  if (!data || !size || size > UINT32_MAX / 2)
    return E_INVALIDARG;
  try {
    container.assign(sizeof(microsoft::DXBCHeader) + 4 + sizeof(microsoft::DXBCBlobHeader) + size, 0);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  microsoft::DXBCHeader header{};
  header.DXBCHeaderFourCC = microsoft::DXBC_FOURCC_NAME;
  header.Version = {DXBC_MAJOR_VERSION, DXBC_MINOR_VERSION};
  header.ContainerSizeInBytes = UINT32(container.size());
  header.BlobCount = 1;
  const UINT32 part_offset = sizeof(header) + 4;
  const microsoft::DXBCBlobHeader part{microsoft::DXBC_RootSignature, UINT32(size)};
  std::memcpy(container.data(), &header, sizeof(header));
  std::memcpy(container.data() + sizeof(header), &part_offset, 4);
  std::memcpy(container.data() + part_offset, &part, sizeof(part));
  std::memcpy(container.data() + part_offset + sizeof(part), data, size);
  return S_OK;
}

} // namespace d3dalloy
