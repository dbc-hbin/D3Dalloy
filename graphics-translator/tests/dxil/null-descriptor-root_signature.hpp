// Serialized D3D12 root signature writer with descriptor tables.
//
// Same wire layout the airconv RootSignatureDeserializer consumes, extended with
// the descriptor table payload the null-descriptor probe needs. Root parameters
// are laid out in declaration order, so the probe's argument buffer holds one
// qword per parameter in that same order.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace null_probe {

constexpr uint32_t kParameterDescriptorTable = 0;
constexpr uint32_t kParameterUav = 4;

constexpr uint32_t kRangeSrv = 0;
constexpr uint32_t kRangeUav = 1;
constexpr uint32_t kRangeCbv = 2;

constexpr uint32_t kVisibilityAll = 0;

struct RangeSpec {
  uint32_t range_type;
  uint32_t base_shader_register;
  uint32_t register_space;
  uint32_t num_descriptors;
};

/** D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND: each range starts where the last one ended. */
constexpr uint32_t kRangeOffsetAppend = 0xffffffffu;

struct TableSpec {
  uint32_t visibility;
  std::vector<RangeSpec> ranges;
};

struct RootUavSpec {
  uint32_t visibility;
  uint32_t shader_register;
  uint32_t register_space;
};

// One table plus one root UAV: parameter 0 is the table, parameter 1 the root
// UAV. Extra tables are appended in order before the root UAV.
struct SignatureSpec {
  std::vector<TableSpec> tables;
  RootUavSpec root_uav{};
};

struct RawRootParameter {
  uint32_t ParameterType;
  uint32_t ShaderVisibility;
  uint32_t PayloadOffset;
};

struct RawRootDescriptorTable {
  uint32_t NumDescriptorRanges;
  uint32_t DescriptorRangesOffset;
};

struct RawDescriptorRange {
  uint32_t RangeType;
  uint32_t NumDescriptors;
  uint32_t BaseShaderRegister;
  uint32_t RegisterSpace;
  uint32_t OffsetInDescriptorsFromTableStart;
};

struct RawRootDescriptor {
  uint32_t ShaderRegister;
  uint32_t RegisterSpace;
};

inline void WriteU32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline std::vector<uint8_t> BuildRawRootSignature(const SignatureSpec &spec) {
  constexpr size_t kHeaderSize = 24;
  constexpr size_t kParameterSize = 12;

  const uint32_t parameter_count = static_cast<uint32_t>(spec.tables.size() + 1);
  size_t size = kHeaderSize + parameter_count * kParameterSize;
  const size_t tables_offset = size;
  size += spec.tables.size() * sizeof(RawRootDescriptorTable);
  const size_t ranges_offset = size;
  for (const auto &table : spec.tables)
    size += table.ranges.size() * sizeof(RawDescriptorRange);
  const size_t root_uav_offset = size;
  size += sizeof(RawRootDescriptor);

  std::vector<uint8_t> raw(size, 0);
  WriteU32(raw, 0, 1);           // Version
  WriteU32(raw, 4, parameter_count);
  WriteU32(raw, 8, kHeaderSize); // RootParametersOffset
  WriteU32(raw, 12, 0);          // NumStaticSamplers
  WriteU32(raw, 16, 0);          // StaticSamplersOffset
  WriteU32(raw, 20, 0);          // Flags

  size_t ranges_cursor = ranges_offset;
  for (uint32_t index = 0; index < spec.tables.size(); index++) {
    const auto &table = spec.tables[index];
    RawRootParameter parameter{kParameterDescriptorTable, table.visibility,
                               static_cast<uint32_t>(tables_offset + index * sizeof(RawRootDescriptorTable))};
    std::memcpy(raw.data() + kHeaderSize + index * kParameterSize, &parameter, sizeof(parameter));
    RawRootDescriptorTable raw_table{static_cast<uint32_t>(table.ranges.size()),
                                     static_cast<uint32_t>(ranges_cursor)};
    std::memcpy(raw.data() + tables_offset + index * sizeof(RawRootDescriptorTable), &raw_table, sizeof(raw_table));
    for (const auto &range : table.ranges) {
      /* OFFSET_APPEND: the compiler accumulates each range after the previous
       * one, which is the slot layout the heap and this writer both use. */
      RawDescriptorRange raw_range{range.range_type, range.num_descriptors, range.base_shader_register,
                                   range.register_space, kRangeOffsetAppend};
      std::memcpy(raw.data() + ranges_cursor, &raw_range, sizeof(raw_range));
      ranges_cursor += sizeof(raw_range);
    }
  }
  RawRootParameter root_parameter{kParameterUav, spec.root_uav.visibility,
                                  static_cast<uint32_t>(root_uav_offset)};
  std::memcpy(raw.data() + kHeaderSize + spec.tables.size() * kParameterSize, &root_parameter,
              sizeof(root_parameter));
  RawRootDescriptor root_descriptor{spec.root_uav.shader_register, spec.root_uav.register_space};
  std::memcpy(raw.data() + root_uav_offset, &root_descriptor, sizeof(root_descriptor));
  return raw;
}

/** Wrap a serialized root signature in the DXBC container the runtime passes to SM50Compile. */
inline std::vector<uint8_t> WrapRootSignatureBlob(const std::vector<uint8_t> &raw) {
  constexpr size_t kBlobHeaderOffset = 36;
  std::vector<uint8_t> blob(kBlobHeaderOffset + 8 + raw.size(), 0);
  WriteU32(blob, 0, 0x43425844); // 'DXBC'
  WriteU32(blob, 20, 1);         // version 1.0
  WriteU32(blob, 24, static_cast<uint32_t>(blob.size()));
  WriteU32(blob, 28, 1); // one blob
  WriteU32(blob, 32, kBlobHeaderOffset);
  WriteU32(blob, kBlobHeaderOffset, 0x30535452); // 'RTS0'
  WriteU32(blob, kBlobHeaderOffset + 4, static_cast<uint32_t>(raw.size()));
  std::memcpy(blob.data() + kBlobHeaderOffset + 8, raw.data(), raw.size());
  return blob;
}

inline std::vector<uint8_t> BuildRootSignatureBlob(const SignatureSpec &spec) {
  return WrapRootSignatureBlob(BuildRawRootSignature(spec));
}

} // namespace null_probe
