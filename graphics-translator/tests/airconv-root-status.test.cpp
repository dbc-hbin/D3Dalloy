#include "dxbc_converter.hpp"
#include "dxbc_root_signature.hpp"
#include "airconv_context.hpp"
#include "DXBCParser/DXBCUtils.h"
#include <llvm/IR/Verifier.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

int main() {
  using namespace dxmt;
  using namespace dxmt::dxbc;
  using namespace llvm;
  unsigned cases = 0;
  bool okay = true;
  for (bool uav : {false, true}) {
    for (unsigned count : {1u, 2u}) {
      for (bool reverse : {false, true}) {
        if (count == 1 && reverse) continue;
        for (bool heaps : {false, true}) {
          for (unsigned visible = 0; visible < count; ++visible) {
            struct Root {
              RawRootSignatureDesc header;
              RawRootParameter parameters[2];
              D3D12_ROOT_DESCRIPTOR1 descriptors[2];
            } root{};
            root.header = {D3D_ROOT_SIGNATURE_VERSION_1_1, count, offsetof(Root, parameters), 0, 0,
                           heaps ? kRootSignatureFlagsDirectlyIndexed : 0u};
            const auto kind = uav ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_SRV;
            for (unsigned i = 0; i < count; ++i) {
              const auto visibility = i == unsigned(reverse) ? D3D12_SHADER_VISIBILITY_VERTEX
                                                            : D3D12_SHADER_VISIBILITY_PIXEL;
              root.parameters[i] = {uint32_t(kind), uint32_t(visibility),
                  uint32_t(offsetof(Root, descriptors) + i * sizeof(D3D12_ROOT_DESCRIPTOR1))};
              root.descriptors[i] = {7, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            }
            ShaderInfo shader;
            if (uav) {
              auto &info = shader.uavMap[0];
              info.range = {0, 7, 1, 3};
              info.resource_type = shader::common::ResourceType::NonApplicable;
              info.scaler_type = shader::common::ScalerDataType::Uint;
              info.read = true;
            } else {
              auto &info = shader.srvMap[0];
              info.range = {0, 7, 1, 3};
              info.resource_type = shader::common::ResourceType::NonApplicable;
              info.scaler_type = shader::common::ScalerDataType::Uint;
              info.read = true;
            }
            LLVMContext context;
            context.setOpaquePointers(false);
            Module module("root-status-regression", context);
            initializeModule(module);
            dxmt::air::FunctionSignatureBuilder signature;
            const bool vertex = root.parameters[visible].ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX;
            auto map = setup_binding_rootsig(&shader, signature, module,
                vertex ? microsoft::D3D10_SB_VERTEX_SHADER : microsoft::D3D10_SB_PIXEL_SHADER,
                &root, sizeof(root), 16, 17, nullptr, 0, false);
            if (!map) return 1;
            auto *function = Function::Create(FunctionType::get(Type::getVoidTy(context),
                {Type::getInt8PtrTy(context, 2)}, false), GlobalValue::ExternalLinkage, "probe", module);
            IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
            llvm::air::AIRBuilder air(builder, errs());
            map->SetGlobalRootArgumentPointer(function->getArg(0));
            auto descriptor = uav ? map->GetUAVBuffer(air, 0, builder.getInt32(7))
                                  : map->GetSRVBuffer(air, 0, builder.getInt32(7));
            if (!descriptor) return 1;
            // Inspect the generated address, not the mapper's internal bookkeeping.
            auto offset = [&](Value *value) -> uint64_t {
              auto *load = dyn_cast_or_null<LoadInst>(value);
              auto *gep = load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand()) : nullptr;
              APInt bytes(64, 0);
              if (!gep || !gep->accumulateConstantOffset(module.getDataLayout(), bytes)) return UINT64_MAX;
              return bytes.getZExtValue();
            };
            const auto data = offset(descriptor->Pointer);
            const auto status = offset(descriptor->StatusBase);
            const uint64_t expected_data = visible * sizeof(uint64_t);
            const uint64_t expected_status = (count + (heaps ? 4 : 0) + visible) * sizeof(uint64_t);
            const bool pass = data == expected_data && status == expected_status;
            okay &= pass;
            builder.CreateRetVoid();
            if (verifyModule(module, &errs())) return 1;
            ++cases;
            std::printf("ROOT_STATUS %s count=%u reverse=%u heaps=%u stage=%s data=%llu/%llu status=%llu/%llu %s\n",
                uav ? "UAV" : "SRV", count, unsigned(reverse), unsigned(heaps), vertex ? "VS" : "PS",
                (unsigned long long)data, (unsigned long long)expected_data,
                (unsigned long long)status, (unsigned long long)expected_status, pass ? "PASS" : "FAIL");
          }
        }
      }
    }
  }
  std::printf("ROOT_STATUS_%s cases=%u\n", okay ? "PASS" : "FAIL", cases);

  {
    RootSignatureDeserializer deserializer;
    const bool pass = deserializer.AllocateBuffer<D3D12_ROOT_PARAMETER1>(
        SIZE_MAX / sizeof(D3D12_ROOT_PARAMETER1) + 1) == nullptr;
    okay &= pass;
    std::printf("ROOT_ALLOCATION_OVERFLOW_%s\n", pass ? "PASS" : "FAIL");
  }

  unsigned deserialize_cases = 0;
  auto test_payload = [&]<typename Descriptor, typename Range, typename Sampler>(uint32_t version) {
    struct Root {
      RawRootSignatureDesc header;
      RawRootParameter parameters[5];
      D3D12_ROOT_CONSTANTS constants;
      Descriptor descriptors[3];
      RawRootDescriptorTable table;
      Range ranges[2];
      Sampler samplers[2];
    } root{};
    root.header = {version, 5, offsetof(Root, parameters), 2, offsetof(Root, samplers),
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    root.parameters[0] = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_SHADER_VISIBILITY_PIXEL,
                          offsetof(Root, table)};
    root.parameters[1] = {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, D3D12_SHADER_VISIBILITY_VERTEX,
                          offsetof(Root, constants)};
    root.constants = {11, 4, 6};
    for (unsigned i = 0; i < 3; ++i) {
      root.parameters[2 + i] = {uint32_t(D3D12_ROOT_PARAMETER_TYPE_CBV) + i, D3D12_SHADER_VISIBILITY_ALL,
                                uint32_t(offsetof(Root, descriptors) + i * sizeof(Descriptor))};
      root.descriptors[i].ShaderRegister = 13 + i;
      root.descriptors[i].RegisterSpace = 5 + i;
      if constexpr (std::is_same_v<Descriptor, D3D12_ROOT_DESCRIPTOR1>)
        root.descriptors[i].Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
    }
    root.table = {2, offsetof(Root, ranges)};
    for (unsigned i = 0; i < 2; ++i) {
      auto &range = root.ranges[i];
      range.RangeType = i ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 3 + i;
      range.BaseShaderRegister = 7 + i;
      range.RegisterSpace = 2 + i;
      range.OffsetInDescriptorsFromTableStart = 1 + i * 4;
      if constexpr (std::is_same_v<Range, D3D12_DESCRIPTOR_RANGE1>)
        range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
      auto &sampler = root.samplers[i];
      sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.MipLODBias = 0.5f;
      sampler.MaxAnisotropy = 1;
      sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
      sampler.MinLOD = 0;
      sampler.MaxLOD = 8 + i;
      sampler.ShaderRegister = 19 + i;
      sampler.RegisterSpace = 9 + i;
      sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      if constexpr (std::is_same_v<Sampler, D3D12_STATIC_SAMPLER_DESC1>) {
        sampler.Flags = i ? D3D12_SAMPLER_FLAG_NONE : D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
        if (i == 0) {
          sampler.MipLODBias = 0;
          sampler.MaxLOD = 0;
        }
      }
    }
    auto report = [&](const char *name, HRESULT hr, HRESULT expected, bool fields = true) {
      const bool pass = hr == expected && fields;
      okay &= pass;
      ++deserialize_cases;
      std::printf("ROOT_DESERIALIZE version=%u %s hr=%08x/%08x fields=%u %s\n", version, name,
                  unsigned(hr), unsigned(expected), unsigned(fields), pass ? "PASS" : "FAIL");
    };
    auto decoded_fields = [&](const RootSignatureDeserializer &deserializer) {
      auto common_fields = [&](const auto &desc) {
        if (desc.NumParameters != 5 || !desc.pParameters || desc.NumStaticSamplers != 2 ||
            !desc.pStaticSamplers || uint32_t(desc.Flags) != root.header.Flags)
          return false;
        for (unsigned i = 0; i < 5; ++i) {
          if (uint32_t(desc.pParameters[i].ParameterType) != root.parameters[i].ParameterType ||
              uint32_t(desc.pParameters[i].ShaderVisibility) != root.parameters[i].ShaderVisibility)
            return false;
        }
        const auto &table = desc.pParameters[0].DescriptorTable;
        if (table.NumDescriptorRanges != 2 || !table.pDescriptorRanges)
          return false;
        for (unsigned i = 0; i < 2; ++i) {
          const auto &range = table.pDescriptorRanges[i];
          const auto &sampler = desc.pStaticSamplers[i];
          if (range.RangeType != root.ranges[i].RangeType || range.NumDescriptors != 3 + i ||
              range.BaseShaderRegister != 7 + i || range.RegisterSpace != 2 + i ||
              range.OffsetInDescriptorsFromTableStart != 1 + i * 4 ||
              sampler.Filter != D3D12_FILTER_MIN_MAG_MIP_POINT ||
              sampler.AddressU != D3D12_TEXTURE_ADDRESS_MODE_CLAMP ||
              sampler.MipLODBias != root.samplers[i].MipLODBias ||
              sampler.BorderColor != D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE ||
              sampler.MaxLOD != root.samplers[i].MaxLOD ||
              sampler.ShaderRegister != 19 + i || sampler.RegisterSpace != 9 + i ||
              sampler.ShaderVisibility != D3D12_SHADER_VISIBILITY_PIXEL)
            return false;
        }
        const auto &constants = desc.pParameters[1].Constants;
        if (constants.ShaderRegister != 11 || constants.RegisterSpace != 4 || constants.Num32BitValues != 6)
          return false;
        for (unsigned i = 0; i < 3; ++i) {
          const auto &descriptor = desc.pParameters[2 + i].Descriptor;
          if (descriptor.ShaderRegister != 13 + i || descriptor.RegisterSpace != 5 + i)
            return false;
        }
        return true;
      };
      if (deserializer.desc_1_0_.Version != D3D_ROOT_SIGNATURE_VERSION_1_0 ||
          deserializer.desc_1_1_.Version != D3D_ROOT_SIGNATURE_VERSION_1_1 ||
          deserializer.desc_1_2_.Version != kRootSignatureVersion_1_2 ||
          !common_fields(deserializer.desc_1_0_.Desc_1_0) || !common_fields(deserializer.desc_1_1_.Desc_1_1) ||
          !common_fields(deserializer.desc_1_2_.Desc_1_2))
        return false;
      const auto descriptor_flags = version == D3D_ROOT_SIGNATURE_VERSION_1_0
                                      ? D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE
                                      : D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
      const auto range_flags = version == D3D_ROOT_SIGNATURE_VERSION_1_0
                                 ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                     D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE
                                 : D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
      const auto &desc_1_1 = deserializer.desc_1_1_.Desc_1_1;
      const auto &desc_1_2 = deserializer.desc_1_2_.Desc_1_2;
      for (unsigned i = 0; i < 3; ++i) {
        if (desc_1_1.pParameters[2 + i].Descriptor.Flags != descriptor_flags ||
            desc_1_2.pParameters[2 + i].Descriptor.Flags != descriptor_flags)
          return false;
      }
      for (unsigned i = 0; i < 2; ++i) {
        const auto expected_sampler_flags = version == uint32_t(kRootSignatureVersion_1_2) && i == 0
                                              ? D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES
                                              : D3D12_SAMPLER_FLAG_NONE;
        if (desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[i].Flags != range_flags ||
            desc_1_2.pParameters[0].DescriptorTable.pDescriptorRanges[i].Flags != range_flags ||
            desc_1_2.pStaticSamplers[i].Flags != expected_sampler_flags)
          return false;
      }
      return true;
    };
    {
      RootSignatureDeserializer deserializer;
      const auto hr = deserializer.Deserialize(&root, sizeof(root));
      report("normal-payload-and-version-conversions", hr, S_OK, hr == S_OK && decoded_fields(deserializer));
    }
    {
      alignas(RawRootSignatureDesc) std::array<unsigned char, sizeof(Root) + 1> bytes{};
      std::memcpy(bytes.data() + 1, &root, sizeof(root));
      RootSignatureDeserializer deserializer;
      const auto hr = deserializer.Deserialize(bytes.data() + 1, sizeof(root));
      std::memset(bytes.data(), 0, bytes.size());
      report("unaligned-base-owned-copy", hr, S_OK, hr == S_OK && decoded_fields(deserializer));
    }
    {
      RawRootSignatureDesc empty{version, 0, UINT32_MAX, 0, UINT32_MAX, 0};
      RootSignatureDeserializer deserializer;
      const auto hr = deserializer.Deserialize(&empty, sizeof(empty));
      auto empty_fields = [](const auto &desc) {
        return desc.NumParameters == 0 && !desc.pParameters && desc.NumStaticSamplers == 0 && !desc.pStaticSamplers;
      };
      report("empty-arrays-ignore-offsets", hr, S_OK,
             hr == S_OK && empty_fields(deserializer.desc_1_0_.Desc_1_0) &&
               empty_fields(deserializer.desc_1_1_.Desc_1_1) && empty_fields(deserializer.desc_1_2_.Desc_1_2));
    }
    auto invalid = [&](const char *name, const Root &bad, size_t size = sizeof(Root)) {
      RootSignatureDeserializer deserializer;
      report(name, deserializer.Deserialize(&bad, size), E_INVALIDARG);
    };
    {
      RootSignatureDeserializer deserializer;
      report("null-input", deserializer.Deserialize(nullptr, sizeof(root)), E_INVALIDARG);
    }
    invalid("empty-input", root, 0);
    invalid("truncated-header", root, sizeof(RawRootSignatureDesc) - 1);
    invalid("truncated-parameter-array", root, offsetof(Root, parameters) + sizeof(root.parameters) - 1);
    invalid("truncated-static-sampler-array", root, sizeof(root) - 1);
    for (uint32_t tag : {0u, 4u, UINT32_MAX}) {
      auto bad = root;
      bad.header.Version = tag;
      invalid("invalid-version-tag", bad);
    }
    for (uint32_t tag : {5u, UINT32_MAX}) {
      auto bad = root;
      bad.parameters[0].ParameterType = tag;
      invalid("invalid-parameter-tag", bad);
    }
    for (uint32_t offset : {uint32_t(sizeof(Root)), UINT32_MAX - 3, uint32_t(offsetof(Root, parameters) + 1)}) {
      auto bad = root;
      bad.header.RootParametersOffset = offset;
      invalid("parameter-array-offset", bad);
    }
    for (uint32_t count : {UINT32_MAX, uint32_t(UINT32_MAX / sizeof(RawRootParameter) + 1),
                          uint32_t((sizeof(Root) - offsetof(Root, parameters)) / sizeof(RawRootParameter) + 1)}) {
      auto bad = root;
      bad.header.NumParameters = count;
      invalid("parameter-array-count", bad);
    }
    for (unsigned i = 0; i < 5; ++i) {
      const auto payload_offset = root.parameters[i].PayloadOffset;
      const auto payload_size = i == 0 ? sizeof(RawRootDescriptorTable)
                                      : i == 1 ? sizeof(D3D12_ROOT_CONSTANTS) : sizeof(Descriptor);
      for (uint32_t offset : {uint32_t(sizeof(Root)), UINT32_MAX - 3, payload_offset + 1}) {
        auto bad = root;
        bad.header.NumParameters = 1;
        bad.parameters[0] = root.parameters[i];
        bad.parameters[0].PayloadOffset = offset;
        invalid("nested-parameter-payload-offset", bad);
      }
      auto bad = root;
      bad.header.NumParameters = 1;
      bad.header.NumStaticSamplers = 0;
      bad.parameters[0] = root.parameters[i];
      invalid("truncated-nested-parameter-payload", bad, payload_offset + payload_size - 1);
    }
    for (uint32_t offset : {uint32_t(sizeof(Root)), UINT32_MAX - 3, uint32_t(offsetof(Root, ranges) + 1)}) {
      auto bad = root;
      bad.table.DescriptorRangesOffset = offset;
      invalid("descriptor-range-array-offset", bad);
    }
    for (uint32_t count : {UINT32_MAX, uint32_t(UINT32_MAX / sizeof(Range) + 1),
                          uint32_t((sizeof(Root) - offsetof(Root, ranges)) / sizeof(Range) + 1)}) {
      auto bad = root;
      bad.table.NumDescriptorRanges = count;
      invalid("descriptor-range-array-count", bad);
    }
    {
      auto bad = root;
      bad.header.NumStaticSamplers = 0;
      invalid("truncated-descriptor-range-array", bad, offsetof(Root, ranges) + sizeof(root.ranges) - 1);
    }
    for (uint32_t offset : {uint32_t(sizeof(Root)), UINT32_MAX - 3, uint32_t(offsetof(Root, samplers) + 1)}) {
      auto bad = root;
      bad.header.StaticSamplersOffset = offset;
      invalid("static-sampler-array-offset", bad);
    }
    for (uint32_t count : {3u, UINT32_MAX, uint32_t(UINT32_MAX / sizeof(Sampler) + 1)}) {
      auto bad = root;
      bad.header.NumStaticSamplers = count;
      invalid("static-sampler-array-count", bad);
    }
    if constexpr (std::is_same_v<Sampler, D3D12_STATIC_SAMPLER_DESC1>) {
      auto bad = root;
      bad.header.NumStaticSamplers = 1;
      invalid("version-1.2-missing-sampler-flags", bad,
              offsetof(Root, samplers) + sizeof(D3D12_STATIC_SAMPLER_DESC));
    }
  };
  test_payload.operator()<D3D12_ROOT_DESCRIPTOR, D3D12_DESCRIPTOR_RANGE, D3D12_STATIC_SAMPLER_DESC>(
      D3D_ROOT_SIGNATURE_VERSION_1_0);
  test_payload.operator()<D3D12_ROOT_DESCRIPTOR1, D3D12_DESCRIPTOR_RANGE1, D3D12_STATIC_SAMPLER_DESC>(
      D3D_ROOT_SIGNATURE_VERSION_1_1);
  test_payload.operator()<D3D12_ROOT_DESCRIPTOR1, D3D12_DESCRIPTOR_RANGE1, D3D12_STATIC_SAMPLER_DESC1>(
      uint32_t(kRootSignatureVersion_1_2));
  std::printf("ROOT_DESERIALIZE_%s cases=%u\n", okay ? "PASS" : "FAIL", deserialize_cases);

  unsigned container_cases = 0;
  const RawRootSignatureDesc empty{2, 0, 0, 0, 0, 0};
  auto check_container = [&](const void *bytes, size_t size, bool valid, const char *name) {
    const void *raw = nullptr;
    UINT raw_size = 0;
    const HRESULT hr = microsoft::DXBCGetRootSignature(bytes, size, &raw, &raw_size);
    const bool pass = valid ? hr == S_OK && raw_size == sizeof(empty) && !std::memcmp(raw, &empty, sizeof(empty))
                            : FAILED(hr);
    okay &= pass;
    ++container_cases;
    std::printf("ROOT_CONTAINER %s hr=%08x %s\n", name, unsigned(hr), pass ? "PASS" : "FAIL");
  };
  // DXBC header, one chunk index, RTS0 chunk header, then an empty signature.
  std::array<uint32_t, 17> container{};
  std::memcpy(container.data(), "DXBC", 4);
  container[5] = 1;
  container[6] = sizeof(container);
  container[7] = 1;
  container[8] = 36;
  std::memcpy(&container[9], "RTS0", 4);
  container[10] = sizeof(empty);
  std::memcpy(&container[11], &empty, sizeof(empty));
  check_container(container.data(), sizeof(container), true, "exact-payload");
  alignas(uint32_t) std::array<unsigned char, sizeof(container) + 8> padded{};
  std::memcpy(padded.data() + 1, container.data(), sizeof(container));
  check_container(padded.data() + 1, sizeof(container), true, "unaligned-container-base");
  check_container(padded.data() + 1, padded.size() - 1, true, "trailing-caller-padding");
  check_container(nullptr, 0, false, "null-container");
  for (size_t length : {size_t(0), size_t(31), size_t(35), sizeof(container) - 1})
    check_container(container.data(), length, false, "truncated-caller-span");
  for (const auto &[field, value] : std::array<std::pair<unsigned, uint32_t>, 8>{{
           {0, 0}, {5, 2}, {6, sizeof(container) + 1}, {7, UINT32_MAX},
           {8, sizeof(container)}, {9, 0}, {10, 0}, {10, sizeof(empty) + 1}}}) {
    auto bad = container;
    bad[field] = value;
    check_container(bad.data(), sizeof(bad), false, "invalid-container-field");
  }
  // A valid first root must not hide an out-of-range later chunk.
  std::array<uint32_t, 20> two_chunks{};
  std::memcpy(two_chunks.data(), container.data(), 32);
  two_chunks[6] = sizeof(two_chunks);
  two_chunks[7] = 2;
  two_chunks[8] = 40;
  two_chunks[9] = 72;
  std::memcpy(&two_chunks[10], &container[9], sizeof(container) - 36);
  std::memcpy(&two_chunks[18], "TEST", 4);
  check_container(two_chunks.data(), sizeof(two_chunks), true, "complete-multiple-chunk-scan");
  two_chunks[19] = 1;
  check_container(two_chunks.data(), sizeof(two_chunks), false, "later-chunk-one-byte-past-end");
  std::printf("ROOT_CONTAINER_%s cases=%u\n", okay ? "PASS" : "FAIL", container_cases);
  return okay ? 0 : 1;
}
