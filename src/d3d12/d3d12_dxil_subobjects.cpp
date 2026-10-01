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

namespace dxmt {

namespace {

/*
 * RDAT part types (DxilRuntimeReflection.h RuntimeDataPartType): the string buffer, the
 * index arrays (each array is { u32 count; u32 elements[count] } addressed by its word
 * offset), the raw bytes and the subobject table.
 *
 * Subobject record payloads after { Kind, Name } (RDAT_SubobjectTypes.inl), verified against
 * DXC 1.9 output of graphics-translator/tests/dxil/raytracing-stateobj.hlsl:
 *   StateObjectConfig            { u32 Flags }
 *   Global/LocalRootSignature    { u32 RawBytesOffset; u32 Size }
 *   SubobjectToExportsAssociation{ u32 Subobject (string); u32 Exports (index array of strings) }
 *   RaytracingShaderConfig       { u32 MaxPayloadSizeInBytes; u32 MaxAttributeSizeInBytes }
 *   RaytracingPipelineConfig     { u32 MaxTraceRecursionDepth }
 *   HitGroup                     { u32 Type; u32 AnyHit; u32 ClosestHit; u32 Intersection }
 *   RaytracingPipelineConfig1    { u32 MaxTraceRecursionDepth; u32 Flags }
 */
constexpr uint32_t kPartStrings = 1, kPartIndexArrays = 2, kPartRawBytes = 5, kPartSubobjects = 6;

struct Part {
  const uint8_t *data = nullptr;
  size_t size = 0;
};

uint32_t ReadU32(const uint8_t *p) {
  uint32_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

/* Names are HLSL identifiers and export names (ASCII); widened code unit by code unit. */
bool ReadString(const Part &strings, uint32_t offset, std::wstring &out) {
  if (!strings.data || offset >= strings.size)
    return false;
  const char *text = reinterpret_cast<const char *>(strings.data + offset);
  const size_t length = strnlen(text, strings.size - offset);
  if (length == strings.size - offset)
    return false;
  out.assign(text, text + length);
  return true;
}

} // namespace

HRESULT ReadDxilLibrarySubobjects(const void *blob, size_t size, std::vector<DxilSubobject> &out) {
  out.clear();
  microsoft::CDXBCParser parser;
  if (!blob || !size || size > UINT32_MAX || FAILED(parser.ReadDXBC(blob, UINT32(size))))
    return E_INVALIDARG;
  const UINT32 index = parser.FindNextMatchingBlob(
      microsoft::DXBCFourCC(UINT('R') | UINT('D') << 8 | UINT('A') << 16 | UINT('T') << 24)
  );
  if (index == DXBC_BLOB_NOT_FOUND)
    return S_OK;
  const auto *rdat = static_cast<const uint8_t *>(parser.GetBlob(index));
  const size_t rdat_size = parser.GetBlobSize(index);
  if (!rdat || rdat_size < 8)
    return E_INVALIDARG;
  const uint32_t part_count = ReadU32(rdat + 4);
  if (8 + uint64_t(part_count) * 4 > rdat_size)
    return E_INVALIDARG;
  Part strings, indices, raw, table;
  for (uint32_t i = 0; i < part_count; ++i) {
    const uint32_t offset = ReadU32(rdat + 8 + 4 * i);
    if (offset > rdat_size || rdat_size - offset < 8)
      return E_INVALIDARG;
    const uint32_t type = ReadU32(rdat + offset), part_size = ReadU32(rdat + offset + 4);
    if (part_size > rdat_size - offset - 8)
      return E_INVALIDARG;
    const Part part{rdat + offset + 8, part_size};
    if (type == kPartStrings)
      strings = part;
    else if (type == kPartIndexArrays)
      indices = part;
    else if (type == kPartRawBytes)
      raw = part;
    else if (type == kPartSubobjects)
      table = part;
  }
  if (!table.data)
    return S_OK;
  if (table.size < 8)
    return E_INVALIDARG;
  const uint32_t count = ReadU32(table.data), stride = ReadU32(table.data + 4);
  if (stride < 24 || 8 + uint64_t(count) * stride > table.size)
    return E_INVALIDARG;

  try {
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *record = table.data + 8 + size_t(i) * stride;
      const uint32_t w[4] = {ReadU32(record + 8), ReadU32(record + 12), ReadU32(record + 16), ReadU32(record + 20)};
      DxilSubobject subobject{};
      subobject.kind = static_cast<DxilSubobjectKind>(ReadU32(record));
      if (!ReadString(strings, ReadU32(record + 4), subobject.name) || subobject.name.empty())
        return E_INVALIDARG;
      switch (subobject.kind) {
      case DxilSubobjectKind::StateObjectConfig:
        subobject.state_object_flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(w[0]);
        break;
      case DxilSubobjectKind::GlobalRootSignature:
      case DxilSubobjectKind::LocalRootSignature:
        if (!raw.data || !w[1] || w[0] > raw.size || w[1] > raw.size - w[0])
          return E_INVALIDARG;
        subobject.root_signature.assign(raw.data + w[0], raw.data + w[0] + w[1]);
        break;
      case DxilSubobjectKind::SubobjectToExportsAssociation: {
        if (!ReadString(strings, w[0], subobject.associated_subobject) || subobject.associated_subobject.empty())
          return E_INVALIDARG;
        /* The export list is an index array: a word offset to { count, string offsets... }. */
        if (!indices.data || uint64_t(w[1]) * 4 + 4 > indices.size)
          return E_INVALIDARG;
        const uint8_t *array = indices.data + size_t(w[1]) * 4;
        const uint32_t export_count = ReadU32(array);
        if ((uint64_t(w[1]) + 1 + export_count) * 4 > indices.size)
          return E_INVALIDARG;
        for (uint32_t j = 0; j < export_count; ++j) {
          std::wstring name;
          if (!ReadString(strings, ReadU32(array + 4 + 4 * j), name) || name.empty())
            return E_INVALIDARG;
          subobject.exports.push_back(std::move(name));
        }
        break;
      }
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
        if (w[0] != D3D12_HIT_GROUP_TYPE_TRIANGLES && w[0] != D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE)
          return E_INVALIDARG;
        subobject.hit_group_type = static_cast<D3D12_HIT_GROUP_TYPE>(w[0]);
        if (!ReadString(strings, w[1], subobject.any_hit) || !ReadString(strings, w[2], subobject.closest_hit) ||
            !ReadString(strings, w[3], subobject.intersection))
          return E_INVALIDARG;
        break;
      default:
        /* A subobject kind this table cannot describe would be dropped silently. */
        return E_NOTIMPL;
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

} // namespace dxmt
