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

#include "d3d12_device.hpp"
#include "Metal4Pipeline.hpp"
#include "d3d12_format_support.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "d3d12_shader_failure.hpp"
#include "dxmt_format.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include <atomic>
#include <bit>

namespace dxmt {

namespace {

constexpr uint32_t kShaderProbeLimit = 16;
std::atomic_uint32_t g_shader_probe_count = 0;

void
TraceShaderProbe(const char *stage, D3D12_SHADER_BYTECODE bytecode, const char *chunk, HRESULT hr) {
  uint32_t probe_index = g_shader_probe_count.load(std::memory_order_relaxed);
  do {
    if (probe_index >= kShaderProbeLimit)
      return;
  } while (!g_shader_probe_count.compare_exchange_weak(probe_index, probe_index + 1, std::memory_order_relaxed));

  const auto digest = bytecode.pShaderBytecode && bytecode.BytecodeLength
                        ? Sha1HashState::compute(bytecode.pShaderBytecode, bytecode.BytecodeLength).string()
                        : std::string("none");
  Logger::info(str::format(
      "D3D12 shader probe[", probe_index, "]: stage=", stage ? stage : "unknown",
      " bytes=", bytecode.BytecodeLength, " sha1=", digest, " chunk=", chunk,
      " hr=", uint32_t(hr)
  ));
}

} // namespace

constexpr WMTCompareFunction kCompareFunctionMap[] = {
    WMTCompareFunctionNever, // padding 0
    WMTCompareFunctionNever, // 1 - 1
    WMTCompareFunctionLess,    WMTCompareFunctionEqual,    WMTCompareFunctionLessEqual,
    WMTCompareFunctionGreater, WMTCompareFunctionNotEqual, WMTCompareFunctionGreaterEqual,
    WMTCompareFunctionAlways // 8 - 1
};

constexpr WMTStencilOperation kStencilOperationMap[] = {
    WMTStencilOperationZero, // invalid
    WMTStencilOperationKeep,
    WMTStencilOperationZero,
    WMTStencilOperationReplace,
    // D3D11_STENCIL_OP_INCR_SAT: Increment the stencil value by 1, and clamp
    // the result.
    WMTStencilOperationIncrementClamp,
    WMTStencilOperationDecrementClamp,
    WMTStencilOperationInvert,
    // D3D11_STENCIL_OP_INCR:Increment the stencil value by 1, and wrap the
    // result if necessary.

    WMTStencilOperationIncrementWrap,
    WMTStencilOperationDecrementWrap,

};

constexpr WMTBlendOperation kBlendOpMap[] = {
    WMTBlendOperationAdd, // padding 0
    WMTBlendOperationAdd, WMTBlendOperationSubtract, WMTBlendOperationReverseSubtract,
    WMTBlendOperationMin, WMTBlendOperationMax,
};

/* Metal 4 has no native logic-op pipeline state. The owned PS epilogue
 * applies the operation to UINT outputs; other target formats retain their output. */
inline bool LogicOpNeedsFetch(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
  return !pDesc->BlendState.IndependentBlendEnable && pDesc->BlendState.RenderTarget[0].LogicOpEnable;
}

/* Component widths of a UINT render-target format for the fetch logic op; false for any non-UINT format. */
inline bool LogicOpComponentBits(DXGI_FORMAT format, uint8_t (&bits)[4]) {
  const auto set = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    bits[0] = r, bits[1] = g, bits[2] = b, bits[3] = a;
    return true;
  };
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_UINT: return set(32, 32, 32, 32);
  case DXGI_FORMAT_R32G32B32_UINT: return set(32, 32, 32, 0);
  case DXGI_FORMAT_R16G16B16A16_UINT: return set(16, 16, 16, 16);
  case DXGI_FORMAT_R32G32_UINT: return set(32, 32, 0, 0);
  case DXGI_FORMAT_R10G10B10A2_UINT: return set(10, 10, 10, 2);
  case DXGI_FORMAT_R8G8B8A8_UINT: return set(8, 8, 8, 8);
  case DXGI_FORMAT_R16G16_UINT: return set(16, 16, 0, 0);
  case DXGI_FORMAT_R32_UINT: return set(32, 0, 0, 0);
  case DXGI_FORMAT_R8G8_UINT: return set(8, 8, 0, 0);
  case DXGI_FORMAT_R16_UINT: return set(16, 0, 0, 0);
  case DXGI_FORMAT_R8_UINT: return set(8, 0, 0, 0);
  default: return false;
  }
}

constexpr WMTBlendFactor kBlendFactorMap[] = {
    WMTBlendFactorZero, // padding 0
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,       // invalid,12
    WMTBlendFactorZero,       // invalid,13
    WMTBlendFactorBlendColor, // BLEND_FACTOR
    WMTBlendFactorOneMinusBlendColor,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
    WMTBlendFactorBlendAlpha,
    WMTBlendFactorOneMinusBlendAlpha,
};

constexpr WMTBlendFactor kBlendAlphaFactorMap[] = {
    WMTBlendFactorZero, // padding 0
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,       // invalid,12
    WMTBlendFactorZero,       // invalid,13
    WMTBlendFactorBlendAlpha, // BLEND_FACTOR
    WMTBlendFactorOneMinusBlendAlpha,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
    WMTBlendFactorBlendAlpha,
    WMTBlendFactorOneMinusBlendAlpha,
};

constexpr WMTColorWriteMask kColorWriteMaskMap[] = {
    // 0000
    WMTColorWriteMaskNone,
    // 0001
    WMTColorWriteMaskRed,
    // 0010
    WMTColorWriteMaskGreen,
    // 0011,
    WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    // 0100
    WMTColorWriteMaskBlue,
    // 0101
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    // 0110
    WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    // 0111
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,

    // 1000
    WMTColorWriteMaskAlpha,
    // 1001
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed,
    // 1010
    WMTColorWriteMaskAlpha | WMTColorWriteMaskGreen,
    // 1011,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    // 1100
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue,
    // 0101
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    // 1110
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    // 1111
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
};

// OPTIONS13 validates the final address, not AlignedByteOffset in isolation.
// Packed formats require four-byte alignment; byte/half formats use their
// component alignment even when the vertex element has four components.
constexpr UINT
VertexComponentAlignment(WMTAttributeFormat format) {
  if ((format >= WMTAttributeFormatUChar2 && format <= WMTAttributeFormatChar4Normalized) ||
      format == WMTAttributeFormatUChar4Normalized_BGRA ||
      (format >= WMTAttributeFormatUChar && format <= WMTAttributeFormatCharNormalized))
    return 1;
  if ((format >= WMTAttributeFormatUShort2 && format <= WMTAttributeFormatHalf4) ||
      (format >= WMTAttributeFormatUShort && format <= WMTAttributeFormatHalf))
    return 2;
  return 4;
}

HRESULT
ExtractMTLInputLayoutElements(
    MTLD3D12Device *device, const void *pShaderBytecodeWithInputSignature,
    const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs, uint32_t NumElements, SM50_IA_INPUT_ELEMENT *pInputLayout,
    uint32_t *pNumElementsOut
) {

  using namespace microsoft;
  uint64_t append_offset[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {0};
  UINT slot_alignment[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT slot_residue[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  uint32_t register_mask = 0;

  CSignatureParser parser;
  HRESULT hr = DXBCGetInputSignature(pShaderBytecodeWithInputSignature, &parser);
  if (FAILED(hr)) {
    return hr;
  }
  const D3D11_SIGNATURE_PARAMETER *pParameters;
  auto num_parameters = parser.GetParameters(&pParameters);

  UINT attribute_count = 0;
  for (UINT j = 0; j < NumElements; j++) {
    auto &desc = pInputElementDescs[j];
    if (desc.InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
        (desc.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
         desc.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) ||
        (desc.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA && desc.InstanceDataStepRate))
      return E_INVALIDARG;

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, metal_format))) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_FAIL;
    }

    if (!metal_format.AttributeFormat) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_INVALIDARG;
    }
    if (!metal_format.BytesPerTexel) {
      ERR("CreateInputLayout: not an ordinary or packed format: ", desc.Format);
      return E_INVALIDARG;
    }
    const uint64_t aligned_byte_offset = desc.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
                                             ? align(append_offset[desc.InputSlot],
                                                     uint64_t(std::min(4u, metal_format.BytesPerTexel)))
                                             : desc.AlignedByteOffset;
    if (aligned_byte_offset > UINT32_MAX - metal_format.BytesPerTexel)
      return E_INVALIDARG;
    append_offset[desc.InputSlot] = aligned_byte_offset + metal_format.BytesPerTexel;

    const UINT component_alignment = VertexComponentAlignment(metal_format.AttributeFormat);
    const UINT common_alignment = std::min(slot_alignment[desc.InputSlot], component_alignment);
    if (common_alignment && ((slot_residue[desc.InputSlot] ^ UINT(aligned_byte_offset)) & (common_alignment - 1)))
      return E_INVALIDARG; // No vertex-buffer base can align every element in this slot.
    if (component_alignment > slot_alignment[desc.InputSlot]) {
      slot_alignment[desc.InputSlot] = component_alignment;
      slot_residue[desc.InputSlot] = UINT(aligned_byte_offset);
    }

    auto pSig = std::find_if(pParameters, pParameters + num_parameters, [&](const D3D11_SIGNATURE_PARAMETER &inputSig) {
      return desc.SemanticIndex == inputSig.SemanticIndex && strcasecmp(desc.SemanticName, inputSig.SemanticName) == 0;
    });
    if (pSig == pParameters + num_parameters)
      continue; // shader has no such input register, so skip it
    auto &inputSig = *pSig;
    if (inputSig.Register >= 32)
      return E_INVALIDARG;
    auto &attribute = pInputLayout[attribute_count++];

    attribute.format = metal_format.AttributeFormat;

    attribute.slot = desc.InputSlot;
    attribute.reg = inputSig.Register;
    attribute.aligned_byte_offset = aligned_byte_offset;
    // the layout stride is provided in IASetVertexBuffer
    attribute.step_function = desc.InputSlotClass;
    attribute.step_rate =
        desc.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? desc.InstanceDataStepRate : 1;
    register_mask |= (1u << inputSig.Register);
  }
  for (UINT i = 0; i < num_parameters; i++) {
    auto &inputSig = pParameters[i];
    if (inputSig.SystemValue != D3D10_SB_NAME_UNDEFINED)
      continue; // ignore SIV & SGV
    if (inputSig.Register >= 32 || !(register_mask & (1u << inputSig.Register))) {
      WARN(
          "CreateInputLayout: Vertex shader expects ", inputSig.SemanticName, "_", inputSig.SemanticIndex,
          " but it's not in input layout element descriptors"
      );
      return E_INVALIDARG;
    }
  }
  *pNumElementsOut = attribute_count;

  return S_OK;
}

/*
 * Validate a D3D12_STREAM_OUTPUT_DESC against the last active pre-rasterization stage.
 * Rules follow D3D11.3 FS ch.14 (14.1, 14.3 DDI comments) and the D3D12 SO structure
 * docs; the D3D12 runtime reports every violation as E_INVALIDARG.
 * StartComponent/ComponentCount address the components of the named output register,
 * so the requested range must lie inside that element's signature mask.
 */
HRESULT
BuildStreamOutputLayout(
    const D3D12_STREAM_OUTPUT_DESC &desc, D3D12_SHADER_BYTECODE vs, StreamOutputLayout &layout, bool geometry
) {
  using namespace microsoft;
  layout = {};
  if (!desc.NumEntries)
    return S_OK;
  if (desc.NumEntries > D3D12_SO_STREAM_COUNT * D3D12_SO_OUTPUT_COMPONENT_COUNT ||
      desc.NumStrides > D3D12_SO_BUFFER_SLOT_COUNT) {
    WARN("CreatePipelineState: SO entry/stride count out of range");
    return E_INVALIDARG;
  }
  if (desc.RasterizedStream >= D3D12_SO_STREAM_COUNT && desc.RasterizedStream != D3D12_SO_NO_RASTERIZED_STREAM) {
    WARN("CreatePipelineState: invalid RasterizedStream ", desc.RasterizedStream);
    return E_INVALIDARG;
  }
  if (!geometry && desc.RasterizedStream != 0 && desc.RasterizedStream != D3D12_SO_NO_RASTERIZED_STREAM) {
    // Streams 1..3 only exist when a GS emits them.
    WARN("CreatePipelineState: RasterizedStream ", desc.RasterizedStream, " requires a geometry shader");
    return E_INVALIDARG;
  }
  for (UINT i = 0; i < desc.NumStrides; i++) {
    const UINT stride = desc.pBufferStrides[i];
    if (stride > D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES || (stride & 3)) {
      WARN("CreatePipelineState: invalid SO stride ", stride, " for slot ", i);
      return E_INVALIDARG;
    }
  }

  // `vs` is the last pre-rasterization stage: GS, otherwise DS, otherwise VS.
  CSignatureParser5 parser;
  if (FAILED(DXBCGetOutputSignature(vs.pShaderBytecode, &parser)) || !parser.Signature(0)) {
    WARN("CreatePipelineState: SO requires an output signature");
    return E_INVALIDARG;
  }

  std::array<uint32_t, D3D12_SO_BUFFER_SLOT_COUNT> slot_components{};
  std::array<uint32_t, D3D12_SO_BUFFER_SLOT_COUNT> slot_elements{};
  // The stream each slot belongs to; every output buffer is written by exactly one stream.
  std::array<uint32_t, D3D12_SO_BUFFER_SLOT_COUNT> slot_stream{~0u, ~0u, ~0u, ~0u};
  std::array<uint32_t, D3D12_SO_STREAM_COUNT> stream_components{};
  try {
    layout.elements.reserve(desc.NumEntries);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  for (UINT i = 0; i < desc.NumEntries; i++) {
    const auto &entry = desc.pSODeclaration[i];
    if (entry.Stream >= D3D12_SO_STREAM_COUNT || (entry.Stream != 0 && !geometry)) {
      WARN("CreatePipelineState: SO stream ", entry.Stream, geometry ? " is out of range" : " requires a geometry shader");
      return E_INVALIDARG;
    }
    if (entry.OutputSlot >= D3D12_SO_BUFFER_SLOT_COUNT || !entry.ComponentCount) {
      WARN("CreatePipelineState: invalid SO slot/component count at entry ", i);
      return E_INVALIDARG;
    }
    const uint32_t slot = entry.OutputSlot;
    const uint32_t stream = entry.Stream;
    if (slot_stream[slot] != ~0u && slot_stream[slot] != stream) {
      WARN("CreatePipelineState: SO slot ", slot, " is written by streams ", slot_stream[slot], " and ", stream);
      return E_INVALIDARG;
    }
    slot_stream[slot] = stream;
    auto &stream_layout = layout.streams[stream];
    if (!entry.SemanticName) {
      // A gap skips ComponentCount dwords (may exceed 4) and has no semantic.
      if (entry.SemanticIndex || entry.StartComponent) {
        WARN("CreatePipelineState: invalid SO gap at entry ", i);
        return E_INVALIDARG;
      }
    } else {
      if (entry.StartComponent > 3 || entry.ComponentCount > 4 || entry.StartComponent + entry.ComponentCount > 4) {
        WARN("CreatePipelineState: invalid SO component range at entry ", i);
        return E_INVALIDARG;
      }
      const D3D11_SIGNATURE_PARAMETER *parameters;
      const UINT parameter_count = parser.Signature(stream)->GetParameters(&parameters);
      auto signature = std::find_if(parameters, parameters + parameter_count, [&](const auto &parameter) {
        return parameter.Stream == stream && parameter.SemanticIndex == entry.SemanticIndex &&
               strcasecmp(parameter.SemanticName, entry.SemanticName) == 0;
      });
      // StartComponent counts from the element's first component, not from .x of the packed
      // register (a float2 packed into .zw is addressed as components 0..1).
      const uint32_t element_mask = signature == parameters + parameter_count ? 0 : uint32_t(signature->Mask) & 0xf;
      const uint32_t first = element_mask ? uint32_t(__builtin_ctz(element_mask)) : 0;
      const uint32_t mask = ((1u << entry.ComponentCount) - 1) << (first + entry.StartComponent);
      if (!element_mask || first + entry.StartComponent + entry.ComponentCount > 4 || (mask & ~element_mask)) {
        WARN("CreatePipelineState: SO element ", entry.SemanticName, entry.SemanticIndex,
             " is not written to stream ", stream, " by the shader");
        return E_INVALIDARG;
      }
      for (const auto &other : layout.elements) {
        const uint32_t other_mask = ((1u << other.component_count) - 1) << (first + other.start_component);
        if (other.stream == stream && other.semantic_index == entry.SemanticIndex &&
            strcasecmp(other.semantic_name.c_str(), entry.SemanticName) == 0 && (other_mask & mask)) {
          WARN("CreatePipelineState: overlapping SO element at entry ", i);
          return E_INVALIDARG;
        }
      }
      try {
        layout.elements.push_back(StreamOutputLayout::Element{
            entry.SemanticName, entry.SemanticIndex, entry.StartComponent, entry.ComponentCount, slot,
            slot_components[slot] * 4, stream_layout.record_dwords, stream});
      } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
      }
      stream_layout.record_dwords += entry.ComponentCount;
      slot_elements[slot]++;
    }
    slot_components[slot] += entry.ComponentCount;
    stream_components[stream] += entry.ComponentCount;
    stream_layout.referenced_slot_mask |= 1u << slot;
    layout.stream_mask |= 1u << stream;
    if (stream_components[stream] > D3D12_SO_OUTPUT_COMPONENT_COUNT) {
      WARN("CreatePipelineState: SO declaration of stream ", stream, " exceeds ", D3D12_SO_OUTPUT_COMPONENT_COUNT,
           " components");
      return E_INVALIDARG;
    }
  }
  for (uint32_t slot = 0; slot < D3D12_SO_BUFFER_SLOT_COUNT; slot++) {
    if (slot_stream[slot] == ~0u)
      continue;
    const uint32_t bytes = slot_components[slot] * 4;
    // 14.1: at most 512 bytes written per vertex to one buffer; the stride must cover it.
    if (!slot_elements[slot] || bytes > 512 || slot >= desc.NumStrides || desc.pBufferStrides[slot] < bytes) {
      WARN("CreatePipelineState: invalid SO layout for slot ", slot);
      return E_INVALIDARG;
    }
    layout.slots[slot] = {desc.pBufferStrides[slot], bytes};
  }
  layout.rasterized_stream = desc.RasterizedStream;
  layout.active = true;
  return S_OK;
}

HRESULT
ShaderImmutableConstants::Initialize(
    MTLD3D12Device *device, const SM50_COMPILED_BITCODE &compiled, sm50_bitcode_t bitcode
) {
  if (!compiled.ConstantDataSize)
    return S_OK;
  auto metal = device->GetMTLDevice();
  const uint64_t alignment = compiled.ConstantDataAlignment;
  if (!alignment || (alignment & (alignment - 1)) || compiled.ConstantDataSize > UINT32_MAX ||
      alignment > UINT32_MAX || !compiled.ConstantData)
    return E_INVALIDARG;
  /* Directly addressable relocations (every 64-bit image) are read in place; WoW64 copies them once. */
  std::vector<SM50_CONSTANT_RELOCATION> copied_relocations;
  auto *relocations = static_cast<const SM50_CONSTANT_RELOCATION *>(SM50AccessiblePointer(compiled.ConstantRelocations));
  if (compiled.ConstantRelocationCount && !relocations) {
    if (compiled.ConstantRelocationCount > UINT32_MAX)
      return E_INVALIDARG;
    copied_relocations.resize(compiled.ConstantRelocationCount);
    if (!SM50ReadCompiledBitcode(bitcode, SM50_COMPILED_BITCODE_PART_RELOCATIONS, compiled.ConstantRelocations,
                                 copied_relocations.data(),
                                 compiled.ConstantRelocationCount * sizeof(SM50_CONSTANT_RELOCATION)))
      return E_INVALIDARG;
    relocations = copied_relocations.data();
  }
  WMTBufferInfo info{};
  info.length = compiled.ConstantDataSize + alignment - 1;
  if (info.length > metal.maxBufferLength())
    return E_INVALIDARG;
  info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
  buffer = metal.newBuffer(info);
  if (!buffer)
    return E_OUTOFMEMORY;
  auto *memory = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
  if (!memory)
    return E_FAIL;
  offset = (-info.gpu_address) & (alignment - 1);
  gpu_address = info.gpu_address + offset;
  if (!SM50ReadCompiledBitcode(bitcode, SM50_COMPILED_BITCODE_PART_CONSTANTS, compiled.ConstantData, memory + offset,
                               compiled.ConstantDataSize))
    return E_INVALIDARG;
  for (uint64_t i = 0; i < compiled.ConstantRelocationCount; ++i) {
    if (compiled.ConstantDataSize < sizeof(uint64_t) ||
        relocations[i].Offset > compiled.ConstantDataSize - sizeof(uint64_t))
      return E_INVALIDARG;
    uint64_t address = gpu_address + relocations[i].TargetOffset;
    if (relocations[i].TargetOffset == SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE) {
      /* 64 bit atomic emulation: the device-global lock table, shared by every PSO. */
      address = device->GetAtomicLockTableAddress();
      if (!address)
        return E_OUTOFMEMORY;
    } else if (relocations[i].TargetOffset == SM50_CONSTANT_RELOCATION_TARGET_ATOMIC64_CONTEXT) {
      address = device->GetAtomic64ContextAddress();
      if (!address)
        return E_OUTOFMEMORY;
    }
    memcpy(memory + offset + relocations[i].Offset, &address, sizeof(address));
  }
  return S_OK;
}

HRESULT
MTLD3D12PipelineState::InitializeShader(
    DeviceShaderArtifactCache &artifacts, const char *Stage, D3D12_SHADER_BYTECODE Bytecode,
    sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl,
    std::vector<MTL_SM50_SHADER_ARGUMENT> *resource_bindings
) {
  using namespace microsoft;
  if (pRefl) {
    HRESULT hr = artifacts.Reflect(Bytecode, ppShader, *pRefl, resource_bindings);
    TraceShaderProbe(Stage, Bytecode, "reflection-cache", hr);
    return hr;
  }
  HRESULT hr;
  CDXBCParser Parser;
  if (FAILED(hr = Parser.ReadDXBC(Bytecode.pShaderBytecode, Bytecode.BytecodeLength))) {
    TraceShaderProbe(Stage, Bytecode, "invalid-container", hr);
    return hr;
  }

  // DXIL containers are handed to the same SM50 entry points as SHEX: the
  // compiler recognizes the DXIL blob and runs the owned DXIL front end, so the
  // raster contract (requested entry name, reflection, root signature) is
  // identical for both container formats.
  const char *chunk = "unknown";
  if (Parser.FindNextMatchingBlob(microsoft::DXBC_DXIL) != DXBC_BLOB_NOT_FOUND)
    chunk = "DXIL";
  else if (Parser.FindNextMatchingBlob(microsoft::DXBC_GenericShaderEx) != DXBC_BLOB_NOT_FOUND)
    chunk = "SHEX";
  else if (Parser.FindNextMatchingBlob(microsoft::DXBC_GenericShader) != DXBC_BLOB_NOT_FOUND)
    chunk = "SHDR";

  SM50Error error;
  if (SM50Initialize(Bytecode.pShaderBytecode, Bytecode.BytecodeLength, ppShader, pRefl, &error)) {
    ERR("Failed to initialize shader: ", SM50GetErrorMessageString(error));
    TraceShaderProbe(Stage, Bytecode, chunk, E_FAIL);
    return E_FAIL;
  }

  TraceShaderProbe(Stage, Bytecode, chunk, S_OK);
  return S_OK;
}

class MTLD3D12GraphicsPipelineStateImpl : public MTLD3D12Pageable<MTLD3D12GraphicsPipelineState> {
  friend HRESULT CreateGraphicsPipelineState(MTLD3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                              REFIID, void **, const GraphicsPipelineExtras *);
protected:
  PipelineCache cache_;
  std::mutex native_owners_mutex;
  std::vector<std::shared_ptr<const NativePipelineArtifact>> native_owners;
  WMT::Reference<WMT::ComputePipelineState> CreateNativePipeline(const WMTComputePipelineInfo &info, WMT::Error &error) {
    auto artifact = device_->GetShaderArtifactCache().NativePipeline(device_->GetMetal4Compiler(), info, error);
    if (!artifact->compute) return {};
    std::lock_guard lock(native_owners_mutex);
    native_owners.push_back(artifact);
    return artifact->compute;
  }
  WMT::Reference<WMT::RenderPipelineState> CreateNativePipeline(const WMTRenderPipelineInfo &info, WMT::Error &error) {
    auto artifact = device_->GetShaderArtifactCache().NativePipeline(device_->GetMetal4Compiler(), info, error);
    if (!artifact->render) return {};
    std::lock_guard lock(native_owners_mutex);
    native_owners.push_back(artifact);
    return artifact->render;
  }
  WMT::Reference<WMT::RenderPipelineState> CreateNativePipeline(const WMTMeshRenderPipelineInfo &info, WMT::Error &error) {
    auto artifact = device_->GetShaderArtifactCache().NativePipeline(device_->GetMetal4Compiler(), info, error);
    if (!artifact->render) return {};
    std::lock_guard lock(native_owners_mutex);
    native_owners.push_back(artifact);
    return artifact->render;
  }
  MTL_SHADER_REFLECTION ref_vs{};
  MTL_SHADER_REFLECTION ref_ps{};

  WMT::Reference<WMT::DepthStencilState> dsso;
  WMT::Reference<WMT::DepthStencilState> dsso_depth_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_stencil_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_no_stencil;
  WMT::Reference<WMT::DepthStencilState> dsso_readonly_no_stencil;

  /* Everything the indirect-command-buffer variant of this pipeline is rebuilt from.
   * The functions are held so their handles stay valid until that variant exists. */
  WMTRenderPipelineInfo indirect_info{};
  WMT::Reference<WMT4::Function> indirect_vertex_function;
  WMT::Reference<WMT4::Function> indirect_fragment_function;
  WMT::Reference<WMT::RenderPipelineState> indirect_pso;
  std::mutex indirect_pso_mutex;
  std::mutex color_write_mask_pso_mutex;
  std::map<uint8_t, WMT::Reference<WMT::RenderPipelineState>> color_write_mask_psos;
  std::vector<uint8_t> pull_vs_bytecode;
  // The explicit root signature is shared by every lazy graphics variant.
  std::vector<uint8_t> pull_root_blob;
  std::vector<SM50_IA_INPUT_ELEMENT> pull_ia_elements;
  bool supports_index_pull = false;
  WMT::Reference<WMT4::Function> pull_vertex_function;
  WMT::Reference<WMT::RenderPipelineState> pull_pso;
  WMT::Reference<WMT::RenderPipelineState> pull_indirect_pso;
  std::mutex pull_pso_mutex;
  std::vector<StreamOutputLayout::Element> statistics_capture_elements;
  std::vector<uint8_t> statistics_ps_bytecode;
  std::string statistics_ps_name;
  uint32_t statistics_ps_sample_mask = 0;
  bool sample_mask_constant = false;
  bool statistics_ps_dual_source_blending = false;
  std::array<uint32_t, 8> statistics_ps_formats{};
  /* Framebuffer-fetch logic op (LogicOpNeedsFetch); chained into every PS variant that writes render targets. */
  SM50_SHADER_D3D12_LOGIC_OP_DATA fetch_logic_op{};
  bool use_fetch_logic_op = false;
  WMT::Reference<WMT4::Function> statistics_vertex_function;
  WMT::Reference<WMT4::Function> statistics_fragment_function;
  WMT::Reference<WMT::RenderPipelineState> statistics_pso;
  WMT::Reference<WMT::RenderPipelineState> statistics_indirect_pso;
  WMT::Reference<WMT4::Function> view_indirect_vertex_function;
  WMT::Reference<WMT4::Function> view_pull_indirect_vertex_function;
  WMT::Reference<WMT4::Function> view_statistics_indirect_vertex_function;
  std::mutex statistics_pso_mutex;
  std::mutex msaa_pso_mutex;
  std::unique_ptr<MSAAPipelineVariants> msaa_variants;
  bool msaa_variants_attempted = false;
  std::unique_ptr<MSAAPipelineVariants> msaa_statistics_variants;
  bool msaa_statistics_variants_attempted = false;
  /* Coarse shading variants, indexed by statistics_enabled. */
  std::array<std::unique_ptr<MSAAPipelineVariants>, 2> msaa_coarse_variants;
  std::array<bool, 2> msaa_coarse_variants_attempted{};
  WMTRenderPipelineInfo msaa_original_info{};
  bool msaa_dual_source = false;
  std::mutex mesh_pso_mutex;
  std::map<uint32_t, MeshPipelineVariant> mesh_variants;
  /* Everything a mesh variant is compiled from. Stage bytecode is owned: variants are built lazily. */
  std::vector<uint8_t> mesh_gs_bytecode, mesh_hs_bytecode, mesh_ds_bytecode;
  /* MeshShader: AS (empty without one) and MS bytecode, and the root signature blob (empty: embedded in MS). */
  std::vector<uint8_t> mesh_as_bytecode, mesh_ms_bytecode;
  WMTMeshRenderPipelineInfo mesh_info{};
  uint32_t mesh_max_potential_tess_factor = 0;
  bool mesh_rasterization_disabled = false;

  HRESULT CompileMeshStage(const char *name, sm50_bitcode_t *compiled, ShaderImmutableConstants &constants,
                           WMT::Reference<WMT4::Function> &function,
                           std::initializer_list<D3D12_SHADER_BYTECODE> sources,
                           SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                           const std::function<int(sm50_bitcode_t *, sm50_error_t *)> &compile) {
    SM50Error error;
    std::shared_ptr<const ShaderStageArtifact> artifact;
    if (device_->GetShaderArtifactCache().Compile(name, sources, args, name, compiled, &error, artifact, compile)) {
      ERR("Failed to compile ", name, ": ", SM50GetErrorMessageString(error));
      return E_FAIL;
    }
    SM50_COMPILED_BITCODE bytecode{};
    if (artifact) bytecode = artifact->view;
    else SM50GetCompiledBitcode(*compiled, &bytecode);
    HRESULT hr;
    if (FAILED(hr = constants.Initialize(device_, bytecode, *compiled)))
      return hr;
    WMT::Reference<WMT::Error> err;
    auto lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, *compiled, err);
    if (!lib)
      return E_FAIL;
    function = device_->GetShaderArtifactCache().Function(lib, name);
    return function ? S_OK : E_FAIL;
  }

  /* Validates and retains the AS/MS of a mesh-shader pipeline (MeshShaderTier 1). */
  HRESULT InitializeMeshShaderStages(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, const GraphicsPipelineExtras &extras) {
    auto stage = [&](const char *name, const D3D12_SHADER_BYTECODE &bytecode, std::vector<uint8_t> &out,
                     std::array<uint32_t, 3> &threads) -> HRESULT {
      MTL_SHADER_REFLECTION reflection{};
      SM50Shader shader;
      if (HRESULT hr = InitializeShader(device_->GetShaderArtifactCache(), name, bytecode, &shader, &reflection, &resource_bindings); FAILED(hr))
        return hr;
      threads = {reflection.ThreadgroupSize[0], reflection.ThreadgroupSize[1], reflection.ThreadgroupSize[2]};
      uses_atomic64 |= reflection.HasAtomic64 != 0;
      const auto *source = static_cast<const uint8_t *>(bytecode.pShaderBytecode);
      out.assign(source, source + bytecode.BytecodeLength);
      return S_OK;
    };
    HRESULT hr;
    if (extras.as.pShaderBytecode && FAILED(hr = stage("AS", extras.as, mesh_as_bytecode, as_threads)))
      return hr;
    if (FAILED(hr = stage("MS", extras.ms, mesh_ms_bytecode, ms_threads)))
      return hr;
    uint32_t native_scalars = 0;
    microsoft::CDXBCParser container;
    if (FAILED(hr = container.ReadDXBC(extras.ms.pShaderBytecode, extras.ms.BytecodeLength))) return hr;
    for (bool primitive : {false, true}) {
      // A valid MS with no primitive attributes has no PSG1 chunk at all.
      if (primitive && container.FindNextMatchingBlob(microsoft::DXBC_PatchConstantSignature11_1) == DXBC_BLOB_NOT_FOUND &&
          container.FindNextMatchingBlob(microsoft::DXBC_PatchConstantSignature) == DXBC_BLOB_NOT_FOUND) continue;
      microsoft::CSignatureParser signature;
      hr = primitive ? microsoft::DXBCGetPatchConstantSignature(extras.ms.pShaderBytecode, &signature)
                     : microsoft::DXBCGetOutputSignature(extras.ms.pShaderBytecode, &signature);
      if (FAILED(hr)) return hr;
      const microsoft::D3D11_SIGNATURE_PARAMETER *parameters;
      const uint32_t count = signature.GetParameters(&parameters);
      for (uint32_t i = 0; i < count; ++i) {
        const auto &parameter = parameters[i];
        if (primitive && !std::strcmp(parameter.SemanticName, "SV_CullPrimitive")) continue;
        if (!parameter.Mask) continue;
        // CullPrimitive is not packed into the D3D output signature. All other fields
        // (including Position) must fit the combined vertex/primitive 32-row signature.
        if (parameter.Register >= 32) return E_INVALIDARG;
        const uint32_t lanes = std::popcount(uint32_t(parameter.Mask & 15));
        // User rows are expanded to float4 AIR fields even for a packed scalar.
        // Count the actual native signature, not only the D3D write mask.
        native_scalars += parameter.SystemValue == microsoft::D3D10_SB_NAME_UNDEFINED ? 4 : lanes;
        if (parameter.SystemValue == microsoft::D3D10_SB_NAME_CLIP_DISTANCE)
          native_scalars += 4; // Native clip distance plus its PS-readable float4 copy.
        if (parameter.SystemValue == microsoft::D3D10_SB_NAME_CULL_DISTANCE)
          owned_mesh_output_replay = true;
      }
    }
    owned_mesh_output_replay |= native_scalars > 124;
    // Mesh/amplification shaders are DXIL-only (ms_6_5 / as_6_5); InitializeShader rejects other stages there.
    return ms_threads[0] ? S_OK : E_INVALIDARG;
  }

  /* Validates and retains the GS or HS/DS stages of a mesh pipeline. */
  HRESULT InitializeMeshStages(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
    auto copy = [](const D3D12_SHADER_BYTECODE &bytecode, std::vector<uint8_t> &out) {
      const auto *source = static_cast<const uint8_t *>(bytecode.pShaderBytecode);
      out.assign(source, source + bytecode.BytecodeLength);
    };
    MTL_SHADER_REFLECTION reflection{};
    SM50Shader shader;
    HRESULT hr;
    if (mesh_kind == MeshPipelineKind::Geometry) {
      if (FAILED(hr = InitializeShader(device_->GetShaderArtifactCache(), "GS", pDesc->GS, &shader, &reflection, &resource_bindings)))
        return hr;
      mesh_input_primitive = reflection.GeometryShader.Primitive;
      uses_atomic64 |= reflection.HasAtomic64 != 0;
      gs_max_output_vertices = reflection.GeometryShader.MaxOutputVertexCount;
      gs_output_topology = reflection.GeometryShader.OutputTopology;
      gs_instance_count = std::max<uint32_t>(1, reflection.GeometryShader.InstanceCount);
      D3D12_PRIMITIVE_TOPOLOGY_TYPE expected;
      switch (mesh_input_primitive) {
      case microsoft::D3D10_SB_PRIMITIVE_POINT:
        expected = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
      case microsoft::D3D10_SB_PRIMITIVE_LINE:
      case microsoft::D3D10_SB_PRIMITIVE_LINE_ADJ:
        expected = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
      case microsoft::D3D10_SB_PRIMITIVE_TRIANGLE:
      case microsoft::D3D10_SB_PRIMITIVE_TRIANGLE_ADJ:
        expected = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
      default:
        // A GS reading patches needs HS/DS in front of it.
        return E_INVALIDARG;
      }
      if (pDesc->PrimitiveTopologyType != expected)
        return E_INVALIDARG;
      copy(pDesc->GS, mesh_gs_bytecode);
      return S_OK;
    }
    if (FAILED(hr = InitializeShader(device_->GetShaderArtifactCache(), "HS", pDesc->HS, &shader, &reflection, &resource_bindings)))
      return hr;
    tess_threads_per_patch = reflection.ThreadsPerPatch;
    uses_atomic64 |= reflection.HasAtomic64 != 0;
    if (mesh_kind == MeshPipelineKind::TessellationGeometry) {
      // The GS reads the tessellator output primitives: point, line (isoline) or triangle (either winding).
      const auto output = reflection.Tessellator.OutputPrimitive;
      MTL_SHADER_REFLECTION gs_reflection{};
      SM50Shader geometry;
      if (FAILED(hr = InitializeShader(device_->GetShaderArtifactCache(), "GS", pDesc->GS, &geometry, &gs_reflection, &resource_bindings)))
        return hr;
      mesh_input_primitive = gs_reflection.GeometryShader.Primitive;
      uses_atomic64 |= gs_reflection.HasAtomic64 != 0;
      gs_max_output_vertices = gs_reflection.GeometryShader.MaxOutputVertexCount;
      gs_output_topology = gs_reflection.GeometryShader.OutputTopology;
      gs_instance_count = std::max<uint32_t>(1, gs_reflection.GeometryShader.InstanceCount);
      const uint32_t expected = output == MTL_TESSELLATOR_OUTPUT_POINT  ? microsoft::D3D10_SB_PRIMITIVE_POINT
                                : output == MTL_TESSELLATOR_OUTPUT_LINE ? microsoft::D3D10_SB_PRIMITIVE_LINE
                                                                        : microsoft::D3D10_SB_PRIMITIVE_TRIANGLE;
      if (mesh_input_primitive != expected)
        return E_INVALIDARG;
      copy(pDesc->GS, mesh_gs_bytecode);
    }
    SM50Shader domain;
    MTL_SHADER_REFLECTION domain_reflection{};
    if (FAILED(hr = InitializeShader(device_->GetShaderArtifactCache(), "DS", pDesc->DS, &domain, &domain_reflection, &resource_bindings)))
      return hr;
    mesh_max_potential_tess_factor = domain_reflection.PostTessellator.MaxPotentialTessFactor;
    uses_atomic64 |= domain_reflection.HasAtomic64 != 0;
    if (!device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple9))
      mesh_max_potential_tess_factor = std::min(8u, mesh_max_potential_tess_factor);
    copy(pDesc->HS, mesh_hs_bytecode);
    copy(pDesc->DS, mesh_ds_bytecode);
    return S_OK;
  }

  /*
   * D3D12 mesh-shader pipeline: the AS (if any) is the object function, the MS the mesh function, both through
   * the ordinary SM50Compile DXIL path (dxil_mesh.cpp). Both read the graphics root signature.
   */
  HRESULT BuildMeshShaderPipeline(MeshPipelineVariant &variant, uint32_t key,
                                  const SM50_MSAA_INTERPOLATION_LAYOUT *interpolation) {
    const bool statistics = key & 8;
    const bool capture_pixels = key & kMeshMSAACapture;
    const bool replay_pixels = key & kMeshMSAAReplay;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
    atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
    SM50_SHADER_COMMON_DATA common{};
    common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    // The statistics variant counts AS/MS invocations, MS primitives and the clipper (dxil_mesh.cpp).
    SM50_SHADER_COMPILATION_ARGUMENT_DATA stats{};
    stats.type = SM50_SHADER_D3D12_PIPELINE_STATS;
    stats.next = &common;
    // The MS emits the per-primitive key the claiming statistics PS reads (the AS ignores it).
    SM50_SHADER_COMPILATION_ARGUMENT_DATA claim{};
    claim.type = SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM;
    claim.next = &common;
    if (mesh_statistics_claim)
      stats.next = &claim;
    auto compile = [&](const char *stage, const char *name, const std::vector<uint8_t> &bytecode,
                       ShaderImmutableConstants &constants, SM50ShaderBitcode &bitcode,
                       WMT::Reference<WMT4::Function> &function) -> HRESULT {
      SM50Shader shader;
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = pull_root_blob.empty() ? bytecode.data() : pull_root_blob.data();
      root.bytecode_length = pull_root_blob.empty() ? bytecode.size() : pull_root_blob.size();
      root.next = statistics && !(stage[0] == 'A' && (key & kMeshObjectReplay))
          ? static_cast<void *>(&stats) : static_cast<void *>(&common);
      common.metal_version = uses_atomic64 ? SM50_SHADER_METAL_410 : kD3D12ShaderMetalVersion;
      if (uses_atomic64) { atomic_phase.next = root.next; root.next = &atomic_phase; }
      SM50_SHADER_COMPILATION_ARGUMENT_DATA conservative{};
      conservative.type = SM50_SHADER_D3D12_CONSERVATIVE_RASTER;
      conservative.next = root.next;
      if (stage[0] == 'M' && replay_pixels && owned_conservative) root.next = &conservative;
      SM50_SHADER_D3D12_MSAA_MESH_DATA capture{};
      if (stage[0] == 'M' && (capture_pixels || replay_pixels)) {
        capture.type = SM50_SHADER_D3D12_MSAA_MESH;
        capture.variant = capture_pixels ? SM50_MSAA_VERTEX_CAPTURE : SM50_MSAA_VERTEX_REPLAY;
        capture.layout_out = &variant.vertex_layout;
        capture.mesh_layout_out = &variant.capture_layout;
        capture.line_mode = owned_line_mode;
        capture.interpolation = interpolation;
        capture.depth_clip_enable = depth_clip_mode == WMTDepthClipModeClip;
        capture.front_counter_clockwise = winding == WMTWindingCounterClockwise;
        capture.next = root.next;
        root.next = &capture;
        variant.capture_layout.threads_per_group = ms_threads[0] * ms_threads[1] * ms_threads[2];
      }
      if (stage[0] == 'A' && (key & (kMeshObjectCapture | kMeshObjectReplay))) {
        capture.type = SM50_SHADER_D3D12_MSAA_MESH;
        capture.variant = key & kMeshObjectCapture ? SM50_MSAA_OBJECT_CAPTURE : SM50_MSAA_OBJECT_REPLAY;
        capture.object_layout_out = &variant.object_capture_layout;
        capture.next = root.next;
        root.next = &capture;
      }
      auto lower = [&](sm50_bitcode_t *out, sm50_error_t *error) {
        if (int result = device_->GetShaderArtifactCache().InitializeCompiler(
              {bytecode.data(), bytecode.size()}, &shader, error)) return result;
        return SM50Compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&root, name, out, error);
      };
      return CompileMeshStage(name, &bitcode, constants, function, {{bytecode.data(), bytecode.size()}},
                              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root), std::ref(lower));
    };
    SM50ShaderBitcode object_bitcode, mesh_bitcode;
    WMT::Reference<WMT4::Function> object_function, mesh_function;
    HRESULT hr;
    if (!replay_pixels && !mesh_as_bytecode.empty() &&
        FAILED(hr = compile("AS", "as_main", mesh_as_bytecode, variant.object_constants, object_bitcode,
                            object_function)))
      return hr;
    if (FAILED(hr = compile("MS", "ms_main", mesh_ms_bytecode, variant.mesh_constants, mesh_bitcode, mesh_function)))
      return hr;
    if (replay_pixels) {
      variant.replay_function = mesh_function;
      return S_OK;
    }
    auto info = mesh_info;
    info.object_function = object_function.handle;
    info.mesh_function = mesh_function.handle;
    info.fragment_function = indirect_fragment_function.handle;
    if (statistics && !capture_pixels) {
      if (!EnsureStatisticsFragmentFunction())
        return E_FAIL;
      info.fragment_function = statistics_fragment_function.handle;
    }
    
    
    
    
    if (capture_pixels) {
      info.rasterization_enabled = false;
      info.fragment_function = {};
      
      for (auto &color : info.colors) color = {};
      info.depth_pixel_format = WMTPixelFormatInvalid;
      info.stencil_pixel_format = WMTPixelFormatInvalid;
      info.raster_sample_count = 1;
      info.alpha_to_coverage_enabled = false;
    }
    WMT::Reference<WMT::Error> err;
    variant.pso = CreateNativePipeline(info, err);
    if (!variant.pso) {
      ERR("Failed to create mesh shader PSO: ", err ? err.description().getUTF8String() : "");
      return E_INVALIDARG;
    }
    return S_OK;
  }

  /*
   * One object+mesh variant: key = MeshVariantKey(index format, GS strip input, statistics).
   * The VS (+HS) runs as the object function, the GS or DS as the mesh function.
   */
  HRESULT BuildMeshPipeline(uint32_t key, MeshPipelineVariant &variant,
                            const SM50_MSAA_INTERPOLATION_LAYOUT *interpolation = nullptr) {
    if (mesh_kind == MeshPipelineKind::MeshShader)
      return BuildMeshShaderPipeline(variant, key, interpolation);
    const auto index_format = SM50_INDEX_BUFFER_FORMAT(key & 3);
    const bool strip = key & 4;
    const bool statistics = key & 8;
    SM50Shader vs, gs, hs, ds;
    const bool geometry = mesh_kind == MeshPipelineKind::Geometry;
    const bool tess_geometry = mesh_kind == MeshPipelineKind::TessellationGeometry;
    // TS+GS: pass 1 is VS+HS / DS capture, pass 2 (kMeshTessellationGeometryPass) capture fetch / GS.
    const bool gs_pass = geometry || (tess_geometry && (key & kMeshTessellationGeometryPass));
    auto initialize = [&](SM50Shader &shader, const std::vector<uint8_t> &source, sm50_error_t *error) {
      return sm50_shader_t(shader) ? 0 : device_->GetShaderArtifactCache().InitializeCompiler(
          {source.data(), source.size()}, &shader, error);
    };

    SM50_SHADER_COMMON_DATA common{};
    common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA stats{};
    stats.type = SM50_SHADER_D3D12_PIPELINE_STATS;
    stats.next = &common;
    // The GS/DS mesh function emits the per-primitive key the claiming statistics PS reads.
    SM50_SHADER_COMPILATION_ARGUMENT_DATA claim{};
    claim.type = SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM;
    claim.next = &common;
    if (mesh_statistics_claim)
      stats.next = &claim;
    void *tail = statistics ? static_cast<void *>(&stats) : static_cast<void *>(&common);
    const bool capture_pixels = key & kMeshMSAACapture;
    const bool replay_vertices = key & kMeshMSAAReplay;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
    atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
    if (uses_atomic64) {
      atomic_phase.next = tail; tail = &atomic_phase; common.metal_version = SM50_SHADER_METAL_410;
    }
    SM50_SHADER_D3D12_MSAA_MESH_DATA msaa_mesh{};
    msaa_mesh.type = SM50_SHADER_D3D12_MSAA_MESH;
    msaa_mesh.variant = replay_vertices ? SM50_MSAA_VERTEX_REPLAY : SM50_MSAA_VERTEX_CAPTURE;
    msaa_mesh.layout_out = &variant.vertex_layout;
    msaa_mesh.mesh_layout_out = &variant.capture_layout;
    variant.capture_layout.threads_per_group = gs_pass ? 1 : 32;
    variant.capture_layout.max_workloads = gs_pass ? gs_instance_count : 0;
    msaa_mesh.line_mode = owned_line_mode;
    msaa_mesh.interpolation = interpolation;
    msaa_mesh.depth_clip_enable = depth_clip_mode == WMTDepthClipModeClip;
    msaa_mesh.front_counter_clockwise = winding == WMTWindingCounterClockwise;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA conservative{};
    conservative.type = SM50_SHADER_D3D12_CONSERVATIVE_RASTER;
    conservative.next = tail;
    msaa_mesh.next = owned_conservative && replay_vertices ? static_cast<void *>(&conservative) : tail;
    const void *root_blob = pull_root_blob.empty() ? nullptr : pull_root_blob.data();
    const size_t root_length = pull_root_blob.size();
    auto root = [&](SM50_SHADER_ROOT_SIGNATURE_DATA &data, SM50_SHADER_COMPILATION_ARGUMENT_TYPE type,
                    const std::vector<uint8_t> &embedded, void *next) {
      data.type = type;
      data.bytecode = root_blob ? root_blob : embedded.data();
      data.bytecode_length = root_blob ? root_length : embedded.size();
      data.next = next;
    };

    SM50_SHADER_IA_INPUT_LAYOUT_DATA layout{};
    layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    layout.index_buffer_format = index_format;
    layout.slot_mask = slot_mask;
    layout.num_elements = pull_ia_elements.size();
    layout.elements = pull_ia_elements.data();

    SM50ShaderBitcode object_bitcode, mesh_bitcode;
    WMT::Reference<WMT4::Function> object_function, mesh_function;
    HRESULT hr;
    if (gs_pass) {
      // Pass 2 of TS+GS: the DS stands in for the VS (its outputs are the GS inputs, fetched from the capture).
      SM50Shader &first = geometry ? vs : ds;
      const char *object_name = geometry ? "vsgs_main" : "dsgs_main";
      SM50_SHADER_GS_TESSELLATION_INPUT_DATA tess_input{};
      tess_input.type = SM50_SHADER_GS_TESSELLATION_INPUT;
      tess_input.layout = tess_capture_layout;
      tess_input.next = tail;
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso_gs{};
      pso_gs.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
      pso_gs.strip_topology = strip;
      pso_gs.next = geometry ? tail : static_cast<void *>(&tess_input);
      layout.next = &pso_gs;
      SM50_SHADER_ROOT_SIGNATURE_DATA vs_root{};
      root(vs_root, SM50_SHADER_ROOT_SIGNATURE, geometry ? pull_vs_bytecode : mesh_ds_bytecode, &layout);
      auto lower_object = [&](sm50_bitcode_t *out, sm50_error_t *error) {
        if (int result = initialize(first, geometry ? pull_vs_bytecode : mesh_ds_bytecode, error)) return result;
        if (int result = initialize(gs, mesh_gs_bytecode, error)) return result;
        return SM50CompileGeometryPipelineVertex(
            first, gs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&vs_root,
            object_name, out, error);
      };
            if (FAILED(hr = CompileMeshStage(object_name, &object_bitcode, variant.object_constants, object_function, {{geometry ? pull_vs_bytecode.data() : mesh_ds_bytecode.data(),
              geometry ? pull_vs_bytecode.size() : mesh_ds_bytecode.size()},
             {mesh_gs_bytecode.data(), mesh_gs_bytecode.size()}}, reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&vs_root), std::ref(lower_object))))
              return hr;
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA pso_gs_mesh = pso_gs;
      // Stream output after the GS: the mesh function captures every declared stream and rasterizes the
      // selected one (see so_assemble_gs and SM50_SHADER_D3D12_GS_STREAMS_DATA).
      std::vector<SM50_SO_CAPTURE_ELEMENT> capture_elements;
      SM50_SHADER_D3D12_GS_STREAMS_DATA streams{};
      if (stream_output.active) {
        try {
          capture_elements.reserve(stream_output.elements.size());
        } catch (const std::bad_alloc &) {
          return E_OUTOFMEMORY;
        }
        // Grouped by stream, so each stream's elements are one contiguous run.
        for (uint32_t s = 0; s < D3D12_SO_STREAM_COUNT; s++) {
          const size_t first = capture_elements.size();
          for (const auto &element : stream_output.elements)
            if (element.stream == s)
              capture_elements.push_back(SM50_SO_CAPTURE_ELEMENT{
                  element.semantic_name.c_str(), element.semantic_index, element.start_component,
                  element.component_count, element.dword_offset});
          streams.streams[s].record_dwords = stream_output.streams[s].record_dwords;
          streams.streams[s].num_elements = uint32_t(capture_elements.size() - first);
          streams.streams[s].elements = capture_elements.data() + first;
        }
        streams.type = SM50_SHADER_D3D12_GS_STREAMS;
        streams.rasterized_stream = stream_output.rasterized_stream == D3D12_SO_NO_RASTERIZED_STREAM
                                        ? ~0u : stream_output.rasterized_stream;
        streams.next = pso_gs_mesh.next;
        pso_gs_mesh.next = &streams;
      }
      SM50_SHADER_GS_PASS_THROUGH_DATA gs_no_raster{};
      if (mesh_rasterization_disabled) {
        gs_no_raster.type = SM50_SHADER_GS_PASS_THROUGH;
        gs_no_raster.DataEncoded = ~0u;
        gs_no_raster.RasterizationDisabled = true;
        gs_no_raster.next = pso_gs_mesh.next;
        pso_gs_mesh.next = &gs_no_raster;
      }
      SM50_SHADER_ROOT_SIGNATURE_DATA gs_root{};
      if (capture_pixels || replay_vertices) {
        msaa_mesh.next = pso_gs_mesh.next;
        if (owned_conservative && replay_vertices) {
          conservative.next = msaa_mesh.next;
          msaa_mesh.next = &conservative;
        }
        pso_gs_mesh.next = &msaa_mesh;
      }
      root(gs_root, SM50_SHADER_ROOT_SIGNATURE, mesh_gs_bytecode, &pso_gs_mesh);
      auto lower_mesh = [&](sm50_bitcode_t *out, sm50_error_t *error) {
        if (int result = initialize(first, geometry ? pull_vs_bytecode : mesh_ds_bytecode, error)) return result;
        if (int result = initialize(gs, mesh_gs_bytecode, error)) return result;
        return SM50CompileGeometryPipelineGeometry(
            first, gs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&gs_root,
            "gs_main", out, error);
      };
            if (FAILED(hr = CompileMeshStage("gs_main", &mesh_bitcode, variant.mesh_constants, mesh_function, {{geometry ? pull_vs_bytecode.data() : mesh_ds_bytecode.data(),
              geometry ? pull_vs_bytecode.size() : mesh_ds_bytecode.size()},
             {mesh_gs_bytecode.data(), mesh_gs_bytecode.size()}}, reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&gs_root), std::ref(lower_mesh))))
              return hr;
    } else {
      SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess{};
      pso_tess.type = SM50_SHADER_PSO_TESSELLATOR;
      pso_tess.max_potential_tess_factor = mesh_max_potential_tess_factor;
      pso_tess.next = tail;
      layout.next = &pso_tess;
      // HS bindings use ROOT_SIGNATURE, the VS part of the object function ROOT_SIGNATURE2.
      SM50_SHADER_ROOT_SIGNATURE_DATA vs_root{}, hs_root{};
      root(vs_root, SM50_SHADER_ROOT_SIGNATURE2, pull_vs_bytecode, &layout);
      root(hs_root, SM50_SHADER_ROOT_SIGNATURE, mesh_hs_bytecode, &vs_root);
      auto lower_hull = [&](sm50_bitcode_t *out, sm50_error_t *error) {
        if (int result = initialize(vs, pull_vs_bytecode, error)) return result;
        if (int result = initialize(hs, mesh_hs_bytecode, error)) return result;
        return SM50CompileTessellationPipelineHull(
            vs, hs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&hs_root,
            "vshs_main", out, error);
      };
            if (FAILED(hr = CompileMeshStage("vshs_main", &object_bitcode, variant.object_constants, object_function, {{pull_vs_bytecode.data(), pull_vs_bytecode.size()},
             {mesh_hs_bytecode.data(), mesh_hs_bytecode.size()}}, reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&hs_root), std::ref(lower_hull))))
              return hr;
      SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess_mesh = pso_tess;
      // TS+GS pass 1: the DS writes its vertices and primitives to the capture scratch instead of rasterizing.
      SM50_TESS_CAPTURE_LAYOUT captured{};
      SM50_SHADER_D3D12_TESS_CAPTURE_DATA capture{};
      capture.type = SM50_SHADER_D3D12_TESS_CAPTURE;
      capture.layout_out = &captured;
      capture.next = &pso_tess_mesh;
      const bool domain_so = stream_output.active && !tess_geometry;
      std::vector<SM50_SO_CAPTURE_ELEMENT> so_elements;
      SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA so_capture{};
      if (domain_so && !replay_vertices) {
        try {
          so_elements.reserve(stream_output.elements.size());
        } catch (const std::bad_alloc &) {
          return E_OUTOFMEMORY;
        }
        for (const auto &element : stream_output.elements)
          so_elements.push_back({element.semantic_name.c_str(), element.semantic_index, element.start_component,
                                 element.component_count, element.dword_offset});
        so_capture.type = SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE;
        so_capture.record_dwords = stream_output.streams[0].record_dwords;
        so_capture.num_elements = so_elements.size();
        so_capture.elements = so_elements.data();
        so_capture.next = &capture;
      }
      SM50_SHADER_GS_PASS_THROUGH_DATA pass_through{};
      pass_through.type = SM50_SHADER_GS_PASS_THROUGH;
      pass_through.DataEncoded = ~0u;
      pass_through.RasterizationDisabled = mesh_rasterization_disabled || tess_geometry;
      pass_through.next = tess_geometry ? static_cast<void *>(&capture) :
          domain_so && !replay_vertices ? static_cast<void *>(&so_capture) : &pso_tess_mesh;
      if ((capture_pixels || replay_vertices) && !tess_geometry) {
        msaa_mesh.next = pass_through.next;
        if (owned_conservative && replay_vertices) {
          conservative.next = msaa_mesh.next;
          msaa_mesh.next = &conservative;
        }
        pass_through.next = &msaa_mesh;
      }
      const char *mesh_name = tess_geometry ? "ds_capture" : "ds_main";
      SM50_SHADER_ROOT_SIGNATURE_DATA ds_root{};
      root(ds_root, SM50_SHADER_ROOT_SIGNATURE, mesh_ds_bytecode, &pass_through);
      auto lower_domain = [&](sm50_bitcode_t *out, sm50_error_t *error) {
        if (int result = initialize(hs, mesh_hs_bytecode, error)) return result;
        if (int result = initialize(ds, mesh_ds_bytecode, error)) return result;
        return SM50CompileTessellationPipelineDomain(
            hs, ds, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ds_root,
            mesh_name, out, error);
      };
            if (FAILED(hr = CompileMeshStage(mesh_name, &mesh_bitcode, variant.mesh_constants, mesh_function, {{mesh_hs_bytecode.data(), mesh_hs_bytecode.size()},
             {mesh_ds_bytecode.data(), mesh_ds_bytecode.size()}}, reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&ds_root), std::ref(lower_domain))))
              return hr;
      if (tess_geometry || (domain_so && !replay_vertices))
        tess_capture_layout = captured;
      if (domain_so && !replay_vertices) {
        gs_max_output_vertices = captured.max_vertices;
        gs_output_topology = captured.primitive_vertices == 1 ? 1 : captured.primitive_vertices == 2 ? 3 : 5;
      }
    }

    if (replay_vertices) {
      variant.replay_function = mesh_function;
      return S_OK;
    }
    auto info = mesh_info;
    info.object_function = object_function.handle;
    info.mesh_function = mesh_function.handle;
    info.fragment_function = indirect_fragment_function.handle;
    if (statistics) {
      if (!EnsureStatisticsFragmentFunction())
        return E_FAIL;
      info.fragment_function = statistics_fragment_function.handle;
    }
    
    
    if (capture_pixels || (tess_geometry && !gs_pass)) {
      // The capture pass never rasterizes; its render pass still carries the draw's attachments.
      info.rasterization_enabled = false;
      info.fragment_function = {};
      
      if (capture_pixels) {
        for (auto &color : info.colors) color = {};
        info.depth_pixel_format = WMTPixelFormatInvalid;
        info.stencil_pixel_format = WMTPixelFormatInvalid;
        info.raster_sample_count = 1;
      }
    }
    if (tess_geometry && gs_pass) {
      // The GS pass has the GS pipeline shape: 16256-byte payload, (1, 1, 1) mesh threadgroups.
      info.payload_memory_length = 16256;
      info.mesh_tgsize_is_multiple_of_sgwidth = false;
      info.object_tgsize_is_multiple_of_sgwidth = false;
    }
    
    
    WMT::Reference<WMT::Error> err;
    variant.pso = CreateNativePipeline(info, err);
    if (!variant.pso) {
      ERR("Failed to create mesh PSO: ", err ? err.description().getUTF8String() : "");
      return E_FAIL;
    }
    return S_OK;
  }

public:
  MTLD3D12GraphicsPipelineStateImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12GraphicsPipelineState>(pDevice) {
    IsComputePipelineState = FALSE;
  }
  // The deferred job captures this object: retire unstarted work or join a running one first.
  // During process detach the OS has already terminated the workers, so a claimed job can never
  // finish; abandon it (the process is exiting) instead of waiting forever.
  ~MTLD3D12GraphicsPipelineStateImpl() {
    if (async_job && !dxmt::this_thread::isInModuleDetachment())
      async_job->Cancel();
  }

  HRESULT InitializeStatisticsCapture(D3D12_SHADER_BYTECODE vs, bool rasterization_enabled) {
    using namespace microsoft;
    CSignatureParser parser;
    if (FAILED(DXBCGetOutputSignature(vs.pShaderBytecode, &parser)))
      return E_INVALIDARG;
    const D3D11_SIGNATURE_PARAMETER *parameters;
    const UINT count = parser.GetParameters(&parameters);
    pipeline_statistics_capture.capture_offset = stream_output.streams[0].record_dwords;
    pipeline_statistics_capture.record_dwords = stream_output.streams[0].record_dwords + 20;
    pipeline_statistics_capture.rasterization_enabled = rasterization_enabled;
    statistics_capture_elements = stream_output.elements;

    const D3D11_SIGNATURE_PARAMETER *position = nullptr;
    std::vector<const D3D11_SIGNATURE_PARAMETER *> clip, cull;
    for (UINT i = 0; i < count; ++i) {
      const auto &parameter = parameters[i];
      if (parameter.Stream != 0 || !parameter.SemanticName)
        continue;
      if (strcasecmp(parameter.SemanticName, "SV_Position") == 0)
        position = &parameter;
      else if (strcasecmp(parameter.SemanticName, "SV_ClipDistance") == 0)
        clip.push_back(&parameter);
      else if (strcasecmp(parameter.SemanticName, "SV_CullDistance") == 0)
        cull.push_back(&parameter);
    }
    // Signature rows may be packed into arbitrary register components. Order by
    // semantic index, then emit only components actually declared in each row.
    auto append = [&](const D3D11_SIGNATURE_PARAMETER &parameter, uint32_t offset, uint32_t &mask,
                      uint32_t &plane) {
      const uint32_t row_mask = uint32_t(parameter.Mask) & 0xf;
      if (!row_mask)
        return;
      const uint32_t first = __builtin_ctz(row_mask);
      for (uint32_t component = first; component < 4; ++component) {
        if (!(row_mask & (1u << component)))
          continue;
        statistics_capture_elements.push_back(StreamOutputLayout::Element{
            parameter.SemanticName, parameter.SemanticIndex, component - first, 1, 0, 0, offset + plane});
        mask |= 1u << plane++;
      }
    };
    if (position) {
      uint32_t position_mask = 0, plane = 0;
      append(*position, pipeline_statistics_capture.capture_offset, position_mask, plane);
    }
    auto append_planes = [&](std::vector<const D3D11_SIGNATURE_PARAMETER *> &rows,
                             uint32_t base, uint32_t &mask) {
      std::stable_sort(rows.begin(), rows.end(), [](auto *a, auto *b) {
        if (a->SemanticIndex != b->SemanticIndex)
          return a->SemanticIndex < b->SemanticIndex;
        const uint32_t a_mask = uint32_t(a->Mask) & 0xf;
        const uint32_t b_mask = uint32_t(b->Mask) & 0xf;
        return (a_mask ? __builtin_ctz(a_mask) : 4) < (b_mask ? __builtin_ctz(b_mask) : 4);
      });
      uint32_t plane = 0;
      for (auto *row : rows) {
        if (plane + __builtin_popcount(uint32_t(row->Mask) & 0xf) > 8)
          return E_INVALIDARG;
        append(*row, base, mask, plane);
      }
      return S_OK;
    };
    if (FAILED(append_planes(clip, pipeline_statistics_capture.capture_offset + 4,
                             pipeline_statistics_capture.clip_mask)) ||
        FAILED(append_planes(cull, pipeline_statistics_capture.capture_offset + 12,
                             pipeline_statistics_capture.cull_mask)))
      return E_INVALIDARG;
    return S_OK;
  }

  bool
  BlendFactorIsDualSource(D3D12_BLEND Blend) {
    return (Blend >= D3D12_BLEND_SRC1_COLOR) && (Blend <= D3D12_BLEND_INV_SRC1_ALPHA);
  }

  template <typename RenderPipelineInfo>
  HRESULT
  InitializePSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, RenderPipelineInfo &info, bool &dual_source_blending) {
    HRESULT hr;
    MTL_DXGI_FORMAT_DESC format_desc;
    uint32_t effective_dual_source_rtvs = 0;
    for (unsigned i = 0; i < pDesc->NumRenderTargets; i++) {
      if (pDesc->RTVFormats[i] == DXGI_FORMAT_UNKNOWN)
        continue;
      if (i >= 2 && dual_source_blending)
        break;
      auto &rt = info.colors[i];
      auto Format = pDesc->RTVFormats[i];
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc))) {
        return hr;
      }
      const auto physical_format = GetD3D12PhysicalPixelFormat(Format);
      const auto render_format = physical_format != WMTPixelFormatInvalid ? physical_format : format_desc.PixelFormat;
      if ((format_desc.Flag & MTL_DXGI_FORMAT_TYPELESS) ||
          !any_bit_set(device_->GetMTLPixelFormatCapability(render_format) & FormatCapability::Color) ||
          !IsD3D12TextureSampleSupported(device_, Format, format_desc,
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         pDesc->SampleDesc.Count))
        return E_INVALIDARG;
      rt.pixel_format = render_format;

      auto renderTarget = pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0];

      if (renderTarget.BlendEnable && renderTarget.LogicOpEnable)
        return E_INVALIDARG;

      if (pDesc->BlendState.IndependentBlendEnable && renderTarget.LogicOpEnable)
        return E_INVALIDARG;

      rt.write_mask = kColorWriteMaskMap[renderTarget.RenderTargetWriteMask];
      const bool rgb32 = Format == DXGI_FORMAT_R32G32B32_FLOAT;
      if (rgb32)
        rt.write_mask = rt.write_mask & ~WMTColorWriteMaskAlpha;

      if (rt.pixel_format == WMTPixelFormatRGB9E5Float)
        rt.write_mask = (rt.write_mask & ~WMTColorWriteMaskAlpha) ? WMTColorWriteMaskAll : 0;

      if (renderTarget.BlendEnable) {
        if (!any_bit_set(device_->GetMTLPixelFormatCapability(rt.pixel_format) & FormatCapability::Blend)) {
          WARN("CreateGraphicsPipelineState: pixel format ", rt.pixel_format, " is not blendable");
          return E_INVALIDARG;
        }
        if (BlendFactorIsDualSource(renderTarget.SrcBlendAlpha) || BlendFactorIsDualSource(renderTarget.SrcBlend) ||
            BlendFactorIsDualSource(renderTarget.DestBlendAlpha) || BlendFactorIsDualSource(renderTarget.DestBlend)) {
          dual_source_blending = true;
        }
        rt.alpha_blend_operation = kBlendOpMap[renderTarget.BlendOpAlpha];
        rt.rgb_blend_operation = kBlendOpMap[renderTarget.BlendOp];
        rt.blending_enabled = renderTarget.BlendEnable;
        rt.src_alpha_blend_factor = kBlendAlphaFactorMap[renderTarget.SrcBlendAlpha];
        // RGB32 has no destination alpha, even when backed by native RGBA32.
        // Its logical alpha is one regardless of the padding's stored value.
        const auto rgb_factor = [rgb32](D3D12_BLEND factor) {
          if (rgb32) {
            if (factor == D3D12_BLEND_DEST_ALPHA) return WMTBlendFactorOne;
            if (factor == D3D12_BLEND_INV_DEST_ALPHA || factor == D3D12_BLEND_SRC_ALPHA_SAT)
              return WMTBlendFactorZero;
          }
          return kBlendFactorMap[factor];
        };
        rt.src_rgb_blend_factor = rgb_factor(renderTarget.SrcBlend);
        rt.dst_alpha_blend_factor = kBlendAlphaFactorMap[renderTarget.DestBlendAlpha];
        rt.dst_rgb_blend_factor = rgb_factor(renderTarget.DestBlend);
      }
      if (i < 2)
        effective_dual_source_rtvs++;
    }

    if (dual_source_blending && effective_dual_source_rtvs > 1)
      return E_INVALIDARG;

    // Logic operations are compiled into the owned framebuffer-fetch epilogue.
    info.logic_operation_enabled = false;
    info.logic_operation = WMTLogicOperationClear;

    if (pDesc->DSVFormat != DXGI_FORMAT_UNKNOWN) {
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), pDesc->DSVFormat, format_desc))) {
        return hr;
      }
      if (!IsD3D12DepthStencilStorageFormat(pDesc->DSVFormat) ||
          !IsD3D12TextureSampleSupported(device_, pDesc->DSVFormat, format_desc,
                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                         pDesc->SampleDesc.Count))
        return E_INVALIDARG;
      auto dsv_flags = DepthStencilPlanarFlags(format_desc.PixelFormat);
      if (dsv_flags & 1)
        info.depth_pixel_format = format_desc.PixelFormat;
      if (dsv_flags & 2)
        info.stencil_pixel_format = format_desc.PixelFormat;
    }

    if constexpr (std::is_same_v<RenderPipelineInfo, WMTRenderPipelineInfo>) {
      switch (pDesc->PrimitiveTopologyType) {
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        info.input_primitive_topology = WMTPrimitiveTopologyClassPoint;
        break;
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        info.input_primitive_topology = WMTPrimitiveTopologyClassLine;
        break;
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH:
        info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
        break;
      default:
        break;
      }
    }

    // ForcedSampleCount 16 has no native raster: it always runs the MSAA capture path as two programmed 8x halves.
    info.raster_sample_count = pDesc->RasterizerState.ForcedSampleCount
                                   ? std::min(pDesc->RasterizerState.ForcedSampleCount, 8u) : pDesc->SampleDesc.Count;
    /* The indirect-command-buffer variant is built separately, on demand:
     * GetIndirectCommandBufferPipelineState. */
    info.support_indirect_command_buffers = false;

    info.alpha_to_coverage_enabled = pDesc->BlendState.AlphaToCoverageEnable && !ref_ps.PixelShader.HasCoverageOutput;

    return S_OK;
  }

  void
  InitializeDSSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, const GraphicsPipelineExtras &extras) {
    if (extras.depth_bounds && !emulated_depth_bounds) {
      // An inverted D3D range always fails. Metal requires ordered bounds;
      // NEVER with writes/stencil disabled preserves the early/late PS contract.
      WMTDepthStencilInfo reject{};
      reject.depth_compare_function = WMTCompareFunctionNever;
      depth_bounds_reject_state = device_->GetMTLDevice().newDepthStencilState(reject);
    }
    WMTDepthStencilInfo info;
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = false;
    info.front_stencil.enabled = false;
    info.back_stencil.enabled = false;
    if (pDesc->DepthStencilState.DepthEnable) {
      info.depth_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.DepthFunc];
      info.depth_write_enabled = pDesc->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    }

    if (pDesc->DepthStencilState.StencilEnable) {
      info.front_stencil.enabled = true;
      info.back_stencil.enabled = true;

      info.front_stencil.depth_stencil_pass_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilPassOp];
      info.front_stencil.stencil_fail_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilFailOp];
      info.front_stencil.depth_fail_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilDepthFailOp];
      info.front_stencil.stencil_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.FrontFace.StencilFunc];
      info.front_stencil.write_mask = pDesc->DepthStencilState.StencilWriteMask;
      info.front_stencil.read_mask = pDesc->DepthStencilState.StencilReadMask;

      info.back_stencil.depth_stencil_pass_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilPassOp];
      info.back_stencil.stencil_fail_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilFailOp];
      info.back_stencil.depth_fail_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilDepthFailOp];
      info.back_stencil.stencil_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.BackFace.StencilFunc];
      // D3D12_DEPTH_STENCIL_DESC2 carries per-face masks; Metal descriptors are per-face too.
      const auto back_masks = extras.back_stencil_read_write_mask.value_or(std::pair<UINT8, UINT8>{
          pDesc->DepthStencilState.StencilReadMask, pDesc->DepthStencilState.StencilWriteMask});
      info.back_stencil.read_mask = back_masks.first;
      info.back_stencil.write_mask = back_masks.second;
    }

    auto metal = device_->GetMTLDevice();
    dsso = metal.newDepthStencilState(info);
    if (info.depth_write_enabled) {
      auto info_depth_readonly = info;
      info_depth_readonly.depth_write_enabled = false;
      dsso_depth_readonly = metal.newDepthStencilState(info_depth_readonly);
    } else {
      dsso_depth_readonly = dsso;
    }
    if (!pDesc->DepthStencilState.StencilEnable) {
      dsso_stencil_readonly = dsso_no_stencil = dsso;
      dsso_readonly = dsso_readonly_no_stencil = dsso_depth_readonly;
      return;
    }
    {
      auto info_stencil_readonly = info;
      info_stencil_readonly.back_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.back_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_stencil_readonly.back_stencil.depth_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.depth_fail_op = WMTStencilOperationKeep;
      dsso_stencil_readonly = metal.newDepthStencilState(info_stencil_readonly);
      if (info.depth_write_enabled) {
        info_stencil_readonly.depth_write_enabled = false;
        dsso_readonly = metal.newDepthStencilState(info_stencil_readonly);
      } else {
        dsso_readonly = dsso_stencil_readonly;
      }
    }
    {
      auto info_nostencil = info;
      info_nostencil.back_stencil.enabled = false;
      info_nostencil.front_stencil.enabled = false;
      dsso_no_stencil = metal.newDepthStencilState(info_nostencil);
      if (info.depth_write_enabled) {
        info_nostencil.depth_write_enabled = false;
        dsso_readonly_no_stencil = metal.newDepthStencilState(info_nostencil);
      } else {
        dsso_readonly_no_stencil = dsso_no_stencil;
      }
    }
  }

  void
  InitializeRasterizerState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, const GraphicsPipelineExtras &extras) {
    fill_mode =
        pDesc->RasterizerState.FillMode == D3D12_FILL_MODE_SOLID ? WMTTriangleFillModeFill : WMTTriangleFillModeLines;
    switch (pDesc->RasterizerState.CullMode) {
    case D3D12_CULL_MODE_BACK:
      cull_mode = WMTCullModeBack;
      break;
    case D3D12_CULL_MODE_FRONT:
      cull_mode = WMTCullModeFront;
      break;
    case D3D12_CULL_MODE_NONE:
      cull_mode = WMTCullModeNone;
      break;
    }
    depth_clip_mode = pDesc->RasterizerState.DepthClipEnable ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
    // D3D12_RASTERIZER_DESC1/2 carry a FLOAT DepthBias; the spec already converts INT to float.
    depth_bias = extras.depth_bias.value_or(float(pDesc->RasterizerState.DepthBias));
    scole_scale = pDesc->RasterizerState.SlopeScaledDepthBias;
    depth_bias_clamp = pDesc->RasterizerState.DepthBiasClamp;
    winding = pDesc->RasterizerState.FrontCounterClockwise ? WMTWindingCounterClockwise : WMTWindingClockwise;
    forced_sample_count = pDesc->RasterizerState.ForcedSampleCount;
    // The D3D line-mode flags have no effect at 1x. At MSAA rates the
    // multisample flag takes precedence over alpha-antialiasing.
    owned_line_mode = pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
                      (forced_sample_count ? forced_sample_count : pDesc->SampleDesc.Count) > 1
        ? pDesc->RasterizerState.MultisampleEnable ? SM50_MSAA_LINE_QUADRILATERAL
        : pDesc->RasterizerState.AntialiasedLineEnable ? SM50_MSAA_LINE_ALPHA_AA
        : SM50_MSAA_LINE_ALIASED
        : SM50_MSAA_LINE_NONE;
    owned_conservative = pDesc->RasterizerState.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
  }

  virtual HRESULT
  Initialize(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, const GraphicsPipelineExtras &extras) {
    if (pDesc->NumRenderTargets > owned_rtv_formats.size())
      return E_INVALIDARG;
    dynamic_depth_bias = static_cast<UINT>(pDesc->Flags) & kPipelineStateFlagDynamicDepthBias;
    dynamic_index_buffer_strip_cut = static_cast<UINT>(pDesc->Flags) & kPipelineStateFlagDynamicIndexBufferStripCut;
    // Capture selection and initial mesh variants consume forced sampling and
    // conservative/line state, so establish them before any pipeline is built.
    InitializeRasterizerState(pDesc, extras);
    owned_depth_bounds = extras.depth_bounds;
    emulated_depth_bounds = owned_depth_bounds && !device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple10);
    owned_rt_count = pDesc->NumRenderTargets;
    std::copy_n(pDesc->RTVFormats, owned_rt_count, owned_rtv_formats.begin());
    owned_dsv_format = pDesc->DSVFormat;
    owned_sample_desc = pDesc->SampleDesc;
    owned_sample_mask = pDesc->SampleMask;
    // FS 3.5.6.4: with UAV-only forced rasterization (no RTVs) the SampleMask does not affect rasterization.
    if (pDesc->RasterizerState.ForcedSampleCount &&
        std::all_of(pDesc->RTVFormats, pDesc->RTVFormats + pDesc->NumRenderTargets,
                    [](DXGI_FORMAT format) { return format == DXGI_FORMAT_UNKNOWN; }))
      owned_sample_mask = UINT_MAX;
    if (extras.view_instancing) {
      view_instancing = *extras.view_instancing;
      for (UINT i = 0; i < view_instancing.count; ++i)
        view_location_offsets |= view_instancing.locations[i].ViewportArrayIndex ||
                                 view_instancing.locations[i].RenderTargetArrayIndex;
    }
    // A one-view amplification mapping adds the view location to the primitive's viewport / render target array
    // index only on pipelines whose maximum amplification count exceeds 1 (native probe, see the view-instancing
    // suite); the draw still runs with an amplification count of 1.
    const uint8_t max_vertex_amplification = view_location_offsets ? 2 : 0;
    owned_has_pixel_shader = pDesc->PS.pShaderBytecode != nullptr;
    ib_strip_cut_value = pDesc->IBStripCutValue;
    HRESULT hr;
    if (extras.ms.pShaderBytecode) {
      // The stream parser rejected VS/HS/DS/GS/IA/SO next to an MS.
      mesh_kind = MeshPipelineKind::MeshShader;
      if (FAILED(hr = InitializeMeshShaderStages(pDesc, extras)))
        return hr;
    } else if (pDesc->HS.pShaderBytecode || pDesc->DS.pShaderBytecode) {
      // HS and DS come as a pair on a patch-list pipeline.
      if (!pDesc->HS.pShaderBytecode || !pDesc->DS.pShaderBytecode ||
          pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
        return E_INVALIDARG;
      mesh_kind = pDesc->GS.pShaderBytecode ? MeshPipelineKind::TessellationGeometry : MeshPipelineKind::Tessellation;
    } else if (pDesc->GS.pShaderBytecode) {
      if (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
        return E_INVALIDARG;
      mesh_kind = MeshPipelineKind::Geometry;
    } else if (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
      return E_INVALIDARG;
    }
    if (mesh_kind != MeshPipelineKind::None && mesh_kind != MeshPipelineKind::MeshShader &&
        FAILED(hr = InitializeMeshStages(pDesc)))
      return hr;
    const bool mesh = mesh_kind != MeshPipelineKind::None;
    const bool geometry_so =
        mesh_kind == MeshPipelineKind::Geometry || mesh_kind == MeshPipelineKind::TessellationGeometry;
    if (FAILED(hr = BuildStreamOutputLayout(pDesc->StreamOutput, geometry_so ? pDesc->GS :
                                            pDesc->DS.pShaderBytecode ? pDesc->DS : pDesc->VS,
                                            stream_output, geometry_so)))
      return hr;
    // D3D11.3 13.5.2: a GS using more than one stream (streamed out or rasterized) outputs point lists only.
    if (stream_output.active) {
      uint32_t used = stream_output.stream_mask;
      if (stream_output.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM)
        used |= 1u << stream_output.rasterized_stream;
      if ((used & (used - 1)) && gs_output_topology != microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST) {
        WARN("CreatePipelineState: GS stream output to multiple streams requires point-list output");
        return E_INVALIDARG;
      }
    }
    if (stream_output.active) {
      // Stream output requires D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT on the explicit or
      // the VS-embedded root signature.
      Com<ID3D12RootSignature> embedded;
      MTLD3D12RootSignature *root = static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature);
      if (!root) {
        if (FAILED(CreateRootSignature(device_, 0, pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength,
                                       __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&embedded))))
          return E_INVALIDARG;
        root = static_cast<MTLD3D12RootSignature *>(embedded.ptr());
      }
      if (!(root->Flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT)) {
        WARN("CreatePipelineState: stream output without D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT");
        return E_INVALIDARG;
      }
    }
    const bool rasterization_disabled =
        stream_output.active && stream_output.rasterized_stream == D3D12_SO_NO_RASTERIZED_STREAM;
    if (!mesh && FAILED(hr = InitializeStatisticsCapture(
            pDesc->VS, !rasterization_disabled && (pDesc->PS.BytecodeLength ||
                                                   pDesc->DepthStencilState.DepthEnable ||
                                                   pDesc->DepthStencilState.StencilEnable))))
      return hr;

    SM50Shader shader_vs, shader_ps;
    SM50Error sm50_err;
    auto metal = device_->GetMTLDevice();
    Sha1Digest key;
    if (FAILED(hr = PipelineCacheKey(*pDesc, key, &extras)))
      return hr;
    // Without a rasterized stream the pipeline has no fragment function (Metal requires it).
    const bool use_fragment = pDesc->PS.pShaderBytecode && !rasterization_disabled;
    if (pDesc->PS.pShaderBytecode && FAILED(hr = InitializeShader(device_->GetShaderArtifactCache(), "PS", pDesc->PS, &shader_ps, &ref_ps, &resource_bindings)))
      return hr;
    uses_atomic64 |= use_fragment && ref_ps.HasAtomic64;
    // Mesh variants use the device artifact cache, not the serialized ordinary-stage PSO blob.
    const uint32_t stages = (mesh ? 0u : 1u << PipelineCache::Vertex) |
                            (use_fragment ? 1u << PipelineCache::Fragment : 0);
    if (FAILED(hr = cache_.Initialize(pDesc->CachedPSO, key, stages, metal.registryID())))
      return hr;
    WMT::Reference<WMT::Error> err;
    WMT::Reference<WMT4::Function> vs_func, ps_func;

    SM50_SHADER_COMMON_DATA common;
    common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    common.next = nullptr;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
    atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
    if (uses_atomic64) { common.next = &atomic_phase; common.metal_version = SM50_SHADER_METAL_410; }

    if (pDesc->VS.pShaderBytecode) {
      if (FAILED(hr = InitializeShader(device_->GetShaderArtifactCache(), "VS", pDesc->VS, &shader_vs, &ref_vs, &resource_bindings)))
        return hr;
      uses_atomic64 |= ref_vs.HasAtomic64 != 0;
      if (uses_atomic64) { common.next = &atomic_phase; common.metal_version = SM50_SHADER_METAL_410; }
      SM50_SHADER_IA_INPUT_LAYOUT_DATA data_ia_layout;
      data_ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
      data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
      std::vector<SM50_IA_INPUT_ELEMENT> elements(pDesc->InputLayout.NumElements);
      hr = ExtractMTLInputLayoutElements(
          device_, pDesc->VS.pShaderBytecode, pDesc->InputLayout.pInputElementDescs, pDesc->InputLayout.NumElements,
          elements.data(), &data_ia_layout.num_elements
      );
      if (FAILED(hr)) {
        return hr;
      }
      elements.resize(data_ia_layout.num_elements);
      data_ia_layout.elements = elements.data();
      slot_mask = 0;
      for (auto &element : elements) {
        slot_mask |= (1 << element.slot);
      }
      data_ia_layout.slot_mask = slot_mask;
      data_ia_layout.next = &common;
      supports_index_pull = !stream_output.active &&
                            (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE ||
                             pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
      const auto *source = static_cast<const uint8_t *>(pDesc->VS.pShaderBytecode);
      pull_vs_bytecode.assign(source, source + pDesc->VS.BytecodeLength);
      pull_ia_elements = elements;

      // Stream output: the VS epilogue writes each declared element into its capture record.
      std::vector<SM50_SO_CAPTURE_ELEMENT> capture_elements;
      SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA data_capture{};
      SM50_SHADER_GS_PASS_THROUGH_DATA data_no_raster{};
      if (stream_output.active) {
        try {
          capture_elements.reserve(stream_output.elements.size());
        } catch (const std::bad_alloc &) {
          return E_OUTOFMEMORY;
        }
        for (const auto &element : stream_output.elements)
          capture_elements.push_back(SM50_SO_CAPTURE_ELEMENT{
              element.semantic_name.c_str(), element.semantic_index, element.start_component,
              element.component_count, element.dword_offset});
        data_capture.type = SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE;
        data_capture.record_dwords = stream_output.streams[0].record_dwords;
        data_capture.num_elements = capture_elements.size();
        data_capture.elements = capture_elements.data();
        data_capture.next = data_ia_layout.next;
        data_ia_layout.next = &data_capture;
      }
      if (rasterization_disabled) {
        // A rasterization-disabled Metal pipeline requires a void vertex function.
        data_no_raster.type = SM50_SHADER_GS_PASS_THROUGH;
        data_no_raster.DataEncoded = ~0u; // no render-target-array / viewport index register
        data_no_raster.RasterizationDisabled = true;
        data_no_raster.next = data_ia_layout.next;
        data_ia_layout.next = &data_no_raster;
      }

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
      if (pDesc->pRootSignature) {
        rootsig.bytecode_length =
            static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
      } else {
        rootsig.bytecode = pDesc->VS.pShaderBytecode;
        rootsig.bytecode_length = pDesc->VS.BytecodeLength;
      }
      rootsig.next = &data_ia_layout;
      const auto *root_source = static_cast<const uint8_t *>(rootsig.bytecode);
      if (pDesc->pRootSignature)
        pull_root_blob.assign(root_source, root_source + rootsig.bytecode_length);

      if (mesh) {
        // The VS is compiled into each mesh variant's object function (BuildMeshPipeline).
      } else if (cache_.Compile(
              PipelineCache::Vertex, device_->GetShaderArtifactCache(), pDesc->VS,
              shader_vs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig, "vs_main", &sm50_err
          )) {
        const auto error = SM50GetErrorMessageString(sm50_err);
        const bool first = RecordShaderCompileFailure(
            "VS", pDesc->VS, rootsig, reinterpret_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&rootsig),
            "vs_main", error, pDesc->pRootSignature != nullptr, &cache_.stageKey(PipelineCache::Vertex));
        if (SM50GetErrorKind(sm50_err) != SM50_ERROR_ROOT_BINDING || first)
          ERR("Failed to compile vs shader: ", error);
        return E_FAIL;
      }

      if (!mesh) {
        const auto vs_bitcode_compiled = cache_.view(PipelineCache::Vertex);
        if (FAILED(hr = vertex_constants.Initialize(device_, vs_bitcode_compiled,
                                                    cache_.bitcode(PipelineCache::Vertex))))
          return hr;
        auto vs_lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), vs_bitcode_compiled,
                                                              cache_.bitcode(PipelineCache::Vertex), err);
        if (!vs_lib)
          return cache_.loaded() ? E_INVALIDARG : E_FAIL;
        vs_func = device_->GetShaderArtifactCache().Function(vs_lib, "vs_main");
        if (!vs_func)
          return cache_.loaded() ? E_INVALIDARG : E_FAIL;
      }
    } else if (mesh_kind == MeshPipelineKind::MeshShader) {
      // The MS root signature: explicit, else embedded in the MS (BuildMeshShaderPipeline reads either).
      if (pDesc->pRootSignature) {
        const void *blob = nullptr;
        const size_t size = static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&blob);
        pull_root_blob.assign(static_cast<const uint8_t *>(blob), static_cast<const uint8_t *>(blob) + size);
      }
    } else {
      ERR("no vertex shader");
      return E_INVALIDARG;
    }

    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);

    bool dual_source_blending = false;

    // Pipeline alpha-to-coverage state consumes reflection even when the
    // pixel function itself is restored from a cache.
    owned_sample_frequency_pixel_shader = ref_ps.PixelShader.SampleFrequency;
    owned_force_early_pixel_shader = pDesc->PS.pShaderBytecode && ref_ps.PSForceEarlyDepthStencil;
    mesh_statistics_claim =
        mesh_kind != MeshPipelineKind::None && EffectiveRasterSampleCount() > 1 &&
        owned_force_early_pixel_shader && !owned_sample_frequency_pixel_shader &&
        (pDesc->DepthStencilState.DepthEnable || pDesc->DepthStencilState.StencilEnable);
    const bool coverage_input = (ref_ps.PSCoverageInputs & SM50_PIXEL_COVERAGE_INPUT) != 0;
    const bool inner_coverage_input = (ref_ps.PSCoverageInputs & SM50_PIXEL_INNER_COVERAGE_INPUT) != 0;
    bool sample_position_input = false;
    if (pDesc->PS.pShaderBytecode) {
      microsoft::CSignatureParser input_signature;
      if (FAILED(hr = microsoft::DXBCGetInputSignature(pDesc->PS.pShaderBytecode, &input_signature)))
        return hr;
      const microsoft::D3D11_SIGNATURE_PARAMETER *parameters;
      // Without a rasterized stream the PS never runs, so its distance inputs must not
      // select the captured-pixel draw path.
      const auto count = use_fragment ? input_signature.GetParameters(&parameters) : 0u;
      for (uint32_t i = 0; i < count; ++i) {
        // Metal's position builtin remains pixel-centered even at sample frequency.
        // Captured replay reconstructs D3D's sample position, including Z/W.
        if (owned_sample_frequency_pixel_shader && parameters[i].Mask &&
            parameters[i].SystemValue == microsoft::D3D10_SB_NAME_POSITION)
          sample_position_input = true;
        if (uint32_t(parameters[i].SystemValue) == 24) owned_shading_rate_pixel_shader = true;
        if (parameters[i].Mask &&
            (parameters[i].SystemValue == microsoft::D3D10_SB_NAME_CLIP_DISTANCE ||
             parameters[i].SystemValue == microsoft::D3D10_SB_NAME_CULL_DISTANCE)) {
          owned_distance_interpolation = true;
          continue;
        }
      }
      const void *root_blob = pDesc->PS.pShaderBytecode;
      if (coverage_input && inner_coverage_input) return E_INVALIDARG;
      sample_mask_constant = !coverage_input;
      size_t root_length = pDesc->PS.BytecodeLength;
      if (pDesc->pRootSignature)
        root_length = static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&root_blob);
      needs_rov_buffer_marker = device_->GetShaderArtifactCache().RasterOrderedMarker(
          pDesc->PS, {root_blob, root_length}, shader_ps);
    }
    // D3D12 forbids sample-frequency execution with the fixed center pattern.
    if (owned_sample_desc.Quality == 0xfffffffeu && owned_sample_frequency_pixel_shader)
      return E_INVALIDARG;
    if (pDesc->RasterizerState.ForcedSampleCount && owned_sample_frequency_pixel_shader)
      return E_INVALIDARG;
    if (FAILED(hr = InitializePSO(pDesc, info, dual_source_blending)))
      return hr;
    msaa_original_info = info;
    msaa_dual_source = dual_source_blending;
    if (rasterization_disabled) {
      info.rasterization_enabled = false;
      info.alpha_to_coverage_enabled = false;
    }

    if (use_fragment) {
      const auto *source = static_cast<const uint8_t *>(pDesc->PS.pShaderBytecode);
      statistics_ps_bytecode.assign(source, source + pDesc->PS.BytecodeLength);
      auto sha1 = Sha1HashState::compute(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);

      std::string ps_name = "ps_main" + sha1.string().substr(0, 8);

      SM50_SHADER_PSO_PIXEL_SHADER_DATA data_ps;
      data_ps.dual_source_blending = dual_source_blending;
      data_ps.disable_depth_output = false;
      data_ps.unorm_output_reg_mask = 0;
      data_ps.sample_mask = owned_sample_mask;
      data_ps.native_sample_count = owned_sample_frequency_pixel_shader ? EffectiveRasterSampleCount() : 1;
      data_ps.type = SM50_SHADER_PSO_PIXEL_SHADER;
      data_ps.next = &common;
      if (LogicOpNeedsFetch(pDesc)) {
        use_fetch_logic_op = true;
        fetch_logic_op.type = SM50_SHADER_D3D12_LOGIC_OP;
        fetch_logic_op.op = pDesc->BlendState.RenderTarget[0].LogicOp;
        for (UINT i = 0; i < pDesc->NumRenderTargets && i < 8; ++i) {
          // Logic ops apply to UINT targets only (FS 17.7.1); other formats keep the plain PS output.
          if (!LogicOpComponentBits(pDesc->RTVFormats[i], fetch_logic_op.component_bits[i]))
            continue;
          fetch_logic_op.rt_mask |= 1u << i;
        }
        fetch_logic_op.next = &common;
        data_ps.next = &fetch_logic_op;
      }
      statistics_ps_name = ps_name;
      statistics_ps_sample_mask = owned_sample_mask;
      statistics_ps_dual_source_blending = dual_source_blending;

      memset(data_ps.pixel_formats, 0, sizeof(data_ps.pixel_formats));
      for (unsigned i = 0; i < pDesc->NumRenderTargets; i++) {
        data_ps.pixel_formats[i] = ORIGINAL_FORMAT(info.colors[i].pixel_format);
      }
      std::copy_n(data_ps.pixel_formats, statistics_ps_formats.size(), statistics_ps_formats.begin());

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
      if (pDesc->pRootSignature) {
        rootsig.bytecode_length =
            static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
      } else {
        rootsig.bytecode = pDesc->PS.pShaderBytecode;
        rootsig.bytecode_length = pDesc->PS.BytecodeLength;
      }
      rootsig.next = &data_ps;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA mask_constant{};
      mask_constant.type = SM50_SHADER_D3D12_SAMPLE_MASK_CONSTANT;
      mask_constant.next = rootsig.next;
      sample_mask_constant = sample_mask_constant && !mesh && !RequiresMSAAExecution() &&
                             !owned_sample_frequency_pixel_shader && !needs_rov_buffer_marker &&
                             owned_sample_mask != UINT_MAX;
      if (sample_mask_constant) rootsig.next = &mask_constant;

      if (cache_.Compile(
              PipelineCache::Fragment, device_->GetShaderArtifactCache(), pDesc->PS,
              shader_ps, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig, ps_name.c_str(), &sm50_err
          )) {
        const auto error = SM50GetErrorMessageString(sm50_err);
        const bool first = RecordShaderCompileFailure(
            "PS", pDesc->PS, rootsig, reinterpret_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&rootsig),
            ps_name.c_str(), error, pDesc->pRootSignature != nullptr, &cache_.stageKey(PipelineCache::Fragment));
        if (SM50GetErrorKind(sm50_err) != SM50_ERROR_ROOT_BINDING || first)
          ERR("Failed to compile ps shader: ", error);
        return E_FAIL;
      }
      const auto ps_bitcode_compiled = cache_.view(PipelineCache::Fragment);
      if (FAILED(hr = fragment_constants.Initialize(device_, ps_bitcode_compiled, cache_.bitcode(PipelineCache::Fragment))))
        return hr;
      // Wide MS output is linked only to the compact captured-pixel variants.
      // Retain its compiled AIR/cache data, but never ask Metal to link the
      // original over-limit fragment signature.
      if (!owned_mesh_output_replay) {
        auto ps_lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), ps_bitcode_compiled,
                                                              cache_.bitcode(PipelineCache::Fragment), err);
        if (!ps_lib)
          return cache_.loaded() ? E_INVALIDARG : E_FAIL;
        ps_func = sample_mask_constant
          ? device_->GetShaderArtifactCache().SampleMaskFunction(ps_lib, ps_name.c_str(),
              {rootsig.bytecode, rootsig.bytecode_length}, owned_sample_mask, err)
          : device_->GetShaderArtifactCache().Function(ps_lib, ps_name.c_str());
        if (!ps_func)
          return cache_.loaded() ? E_INVALIDARG : E_FAIL;
      }
    }

    // PSO
    if (mesh) {
      WMT::InitializeMeshRenderPipelineInfo(mesh_info);
      std::copy(std::begin(info.colors), std::end(info.colors), std::begin(mesh_info.colors));
      mesh_info.alpha_to_coverage_enabled = info.alpha_to_coverage_enabled;
      mesh_info.logic_operation_enabled = info.logic_operation_enabled;
      mesh_info.logic_operation = info.logic_operation;
      mesh_info.rasterization_enabled = info.rasterization_enabled;
      mesh_info.raster_sample_count = info.raster_sample_count;
      mesh_info.depth_pixel_format = info.depth_pixel_format;
      mesh_info.stencil_pixel_format = info.stencil_pixel_format;
      if (mesh_kind == MeshPipelineKind::Geometry) {
        mesh_info.payload_memory_length = 16256;
      } else if (mesh_kind == MeshPipelineKind::MeshShader) {
        // Arbitrary [numthreads]; Metal infers the payload size from the object/mesh functions.
      } else {
        mesh_info.mesh_tgsize_is_multiple_of_sgwidth = true;
        mesh_info.object_tgsize_is_multiple_of_sgwidth = true;
        if (!metal.supportsFamily(WMTGPUFamilyApple7))
          mesh_info.payload_memory_length = 16384;
      }
      mesh_rasterization_disabled = !info.rasterization_enabled;
      mesh_info.max_vertex_amplification_count = max_vertex_amplification;
      indirect_fragment_function = ps_func;
      indirect_info = info;
      // Compile the default (non-indexed) variant now so invalid shader combinations fail creation.
      if (!GetMeshPipeline(MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false, false) |
                           (RequiresMSAAExecution() ? kMeshMSAACapture : 0)))
        return E_INVALIDARG;
      // TS+GS: pass 2 is built from the capture layout pass 1 just reported.
      if (mesh_kind == MeshPipelineKind::TessellationGeometry &&
          !GetMeshPipeline(MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false, false) | kMeshTessellationGeometryPass |
                           (RequiresMSAAExecution() ? kMeshMSAACapture : 0)))
        return E_INVALIDARG;
    } else {
      info.vertex_function = vs_func.handle;
      info.fragment_function = ps_func.handle;
      info.inherit_color_attachment_mapping = !RequiresMSAAExecution();
      submission_mapping_safe = info.inherit_color_attachment_mapping && !use_fetch_logic_op && !dual_source_blending;
      submission_color_output_mask = submission_mapping_safe && use_fragment
                                         ? static_cast<uint8_t>(ref_ps.PixelShader.ValidRenderTargets) : 0;
      
      
      info.max_vertex_amplification_count = max_vertex_amplification;

      if (!RequiresMSAAExecution()) {
        // Shader conversion and libraries above stay synchronous so invalid shaders still fail creation;
        // only the native link of this ordinary pipeline is deferred.
        if (device_->async_pipelines.Enabled() && !stream_output.active && !uses_atomic64 &&
            !needs_rov_buffer_marker && !view_instancing.count) {
          async_omittable = std::none_of(resource_bindings.begin(), resource_bindings.end(),
              [](const auto &binding) { return binding.Type == SM50BindingType::UAV; });
          // Submitted by CreateGraphicsPipelineState after Initialize succeeds. Creation completes the
          // cache flight without retaining it; the link result decides retention and disk publication,
          // so a failed link is never published as a good PSO.
          async_job = std::make_shared<owned_graphics::AsyncCompileJob>([this, info] {
            WMT::Reference<WMT::Error> error;
            auto native = CreateNativePipeline(info, error);
            cache_.CommitDeferred(bool(native));
            if (!native) {
              ERR("Failed to create deferred PSO: ", error ? error.description().getUTF8String() : "");
              return false;
            }
            pso = std::move(native);
            return true;
          });
        } else {
          pso = CreateNativePipeline(info, err);
          if (!pso) {
            ERR("Failed to create PSO: ", err.description().getUTF8String());
            return E_FAIL;
          }
        }
      } else {
        const auto count = owned_sample_desc.Count;
        const auto quality = owned_sample_desc.Quality;
        const bool native_msaa =
            (count == 2 || count == 4 || count == 8) && metal.supportsTextureSampleCount(count) &&
            (quality == 0 || quality == 0xffffffffu || quality == 0xfffffffeu) &&
            pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
            use_fragment && !stream_output.active && !rasterization_disabled &&
            !forced_sample_count && !ForcedSampleReplay() && !owned_conservative &&
            !owned_distance_interpolation && !owned_mesh_output_replay && !uses_atomic64 &&
            !needs_rov_buffer_marker && !owned_depth_bounds && !sample_position_input &&
            !coverage_input && !inner_coverage_input && !owned_shading_rate_pixel_shader &&
            !pDesc->BlendState.AlphaToCoverageEnable && !ref_ps.PixelShader.HasCoverageOutput &&
            owned_line_mode == SM50_MSAA_LINE_NONE && !use_fetch_logic_op &&
            !dual_source_blending && !owned_force_early_pixel_shader && !pDesc->DepthStencilState.StencilEnable &&
            !view_instancing.count;
        if (native_msaa) {
          // Identity mapping, original VS/PS and their constants; no capture or ICB epilogue.
          native_msaa_pso = CreateNativePipeline(info, err);
          if (!native_msaa_pso) {
            ERR("Failed to create native MSAA PSO: ", err ? err.description().getUTF8String() : "");
            return E_FAIL;
          }
        }
      }

      indirect_vertex_function = vs_func;
      indirect_fragment_function = ps_func;
      indirect_info = info;
    }

    // DSSO
    InitializeDSSO(pDesc, extras);
    if (owned_depth_bounds && !emulated_depth_bounds && !depth_bounds_reject_state)
      return E_OUTOFMEMORY;

    // Captured pipelines never execute the original raster PSO. Validate the
    // actual capture/pixel/replay combination at creation, rather than linking
    // an unused fetch shader that would repeat pixel-frequency side effects.
    if (RequiresMSAAExecution() && !GetMSAAPipelineVariants(false, false))
      return E_FAIL;

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12GraphicsPipelineState: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) override {
    // The deferred link commits the cache; export only a PSO whose link actually succeeded.
    if (!WaitNativePipeline()) {
      if (blob)
        *blob = nullptr;
      return E_FAIL;
    }
    return cache_.GetBlob(blob);
  }

  // Caller holds the corresponding ordinary/index-pull/statistics variant mutex.
  // These uncached ICB entries must not reuse the cached direct VS: Metal 4 ICB
  // execution does not inherit encoder vertex-amplification view mappings.
  bool EnsureViewIndirectVertexFunction(bool index_pull, bool statistics) {
    auto &function = statistics ? view_statistics_indirect_vertex_function
                               : index_pull ? view_pull_indirect_vertex_function : view_indirect_vertex_function;
    if (function)
      return true;
    auto &constants = statistics ? indirect_view_pipeline_statistics_vertex_constants
                                : index_pull ? indirect_view_index_pull_vertex_constants : indirect_view_vertex_constants;
    SM50Error compile_error;
    SM50_SHADER_COMMON_DATA common{};
    common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA locations{};
    locations.type = SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS;
    locations.next = &common;
    locations.view_count = view_instancing.count;
    for (uint32_t i = 0; i < view_instancing.count; ++i) {
      locations.viewport_offsets[i] = view_instancing.locations[i].ViewportArrayIndex;
      locations.render_target_offsets[i] = view_instancing.locations[i].RenderTargetArrayIndex;
    }
    void *tail = &locations;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA pull{};
    if (index_pull) {
      pull.type = SM50_SHADER_D3D12_INDEX_PULL;
      pull.next = tail;
      tail = &pull;
    }
    SM50_SHADER_COMPILATION_ARGUMENT_DATA statistics_arg{};
    if (statistics) {
      statistics_arg.type = SM50_SHADER_D3D12_PIPELINE_STATS;
      statistics_arg.next = tail;
      tail = &statistics_arg;
    }
    std::vector<SM50_SO_CAPTURE_ELEMENT> elements;
    SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA capture{};
    if (statistics || stream_output.active) {
      const auto &source = statistics ? statistics_capture_elements : stream_output.elements;
      elements.reserve(source.size());
      for (const auto &element : source)
        elements.push_back(SM50_SO_CAPTURE_ELEMENT{element.semantic_name.c_str(), element.semantic_index,
            element.start_component, element.component_count, element.dword_offset});
      capture.type = SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE;
      capture.record_dwords = statistics ? pipeline_statistics_capture.record_dwords
                                        : stream_output.streams[0].record_dwords;
      capture.num_elements = elements.size();
      capture.elements = elements.data();
      capture.next = tail;
      tail = &capture;
    }
    SM50_SHADER_GS_PASS_THROUGH_DATA no_raster{};
    if (!indirect_info.rasterization_enabled) {
      no_raster.type = SM50_SHADER_GS_PASS_THROUGH;
      no_raster.DataEncoded = ~0u;
      no_raster.RasterizationDisabled = true;
      no_raster.next = tail;
      tail = &no_raster;
    }
    SM50_SHADER_IA_INPUT_LAYOUT_DATA layout{};
    layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
    layout.slot_mask = slot_mask;
    layout.num_elements = pull_ia_elements.size();
    layout.elements = pull_ia_elements.data();
    layout.next = tail;
    SM50_SHADER_ROOT_SIGNATURE_DATA root{};
    root.type = SM50_SHADER_ROOT_SIGNATURE;
    root.bytecode = pull_root_blob.empty() ? pull_vs_bytecode.data() : pull_root_blob.data();
    root.bytecode_length = pull_root_blob.empty() ? pull_vs_bytecode.size() : pull_root_blob.size();
    root.next = &layout;
    SM50ShaderBitcode compiled;
    std::shared_ptr<const ShaderStageArtifact> artifact;
    if (device_->GetShaderArtifactCache().CompileShader({pull_vs_bytecode.data(), pull_vs_bytecode.size()},
                    reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                    "vs_main", &compiled, &compile_error, artifact)) {
      ERR("Failed to compile indirect view VS: ", SM50GetErrorMessageString(compile_error));
      return false;
    }
    SM50_COMPILED_BITCODE bytecode{};
    if (artifact) bytecode = artifact->view;
    else SM50GetCompiledBitcode(compiled, &bytecode);
    if (FAILED(constants.Initialize(device_, bytecode, compiled)))
      return false;
    WMT::Reference<WMT::Error> error;
    auto library = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
    if (!library)
      return false;
    function = device_->GetShaderArtifactCache().Function(library, "vs_main");
    return bool(function);
  }

  WMT::RenderPipelineState GetColorWriteMaskPipelineState(uint8_t disabled_outputs) override {
    if (!WaitNativePipeline())
      return {};
    if (!disabled_outputs)
      return pso;
    if (!submission_mapping_safe || !pso)
      return {};
    disabled_outputs &= submission_color_output_mask;
    if (!disabled_outputs)
      return pso;
    std::lock_guard<std::mutex> lock(color_write_mask_pso_mutex);
    if (auto found = color_write_mask_psos.find(disabled_outputs); found != color_write_mask_psos.end())
      return found->second;
    auto info = indirect_info;
    for (unsigned i = 0; i < 8; ++i)
      if (disabled_outputs & (1u << i))
        info.colors[i].write_mask = 0;
    WMT::Reference<WMT::Error> error;
    auto variant = CreateNativePipeline(info, error);
    if (!variant) {
      ERR("Failed to create logical color write-mask PSO: ",
          error ? error.description().getUTF8String() : "");
      return {};
    }
    return color_write_mask_psos.emplace(disabled_outputs, std::move(variant)).first->second;
  }

  virtual WMT::RenderPipelineState
  GetIndirectCommandBufferPipelineState() override {
    std::lock_guard<std::mutex> lock(indirect_pso_mutex);
    if (!indirect_pso) {
      WMT::Reference<WMT::Error> error;
      auto info = indirect_info;
      info.vertex_function = indirect_vertex_function.handle;
      if (view_location_offsets) {
        if (!EnsureViewIndirectVertexFunction(false, false))
          return {};
        info.vertex_function = view_indirect_vertex_function.handle;
      }
      info.fragment_function = indirect_fragment_function.handle;
      info.support_indirect_command_buffers = true;
      indirect_pso = CreateNativePipeline(info, error);
      if (!indirect_pso)
        ERR("Failed to create indirect command buffer PSO: ", error.description().getUTF8String());
    }
    return indirect_pso;
  }

  WMT::RenderPipelineState GetNativeMSAAPipelineState(bool index_pull) override {
    if (!native_msaa_pso)
      return {};
    return index_pull ? GetIndexPullPipelineState(false) : native_msaa_pso;
  }

  WMT::RenderPipelineState GetIndexPullPipelineState(bool indirect) override {
    std::lock_guard<std::mutex> lock(pull_pso_mutex);
    if (!supports_index_pull)
      return {};
    WMT::Reference<WMT::Error> error;
    if (!pull_vertex_function && !(indirect && view_location_offsets)) {
      SM50Error compile_error;
      SM50_SHADER_COMMON_DATA common{};
      common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
      common.type = SM50_SHADER_COMMON;
      common.metal_version = kD3D12ShaderMetalVersion;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA pull{};
      pull.type = SM50_SHADER_D3D12_INDEX_PULL;
      pull.next = &common;
      SM50_SHADER_IA_INPUT_LAYOUT_DATA layout{};
      layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
      layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
      layout.slot_mask = slot_mask;
      layout.num_elements = pull_ia_elements.size();
      layout.elements = pull_ia_elements.data();
      layout.next = &pull;
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = pull_root_blob.empty() ? pull_vs_bytecode.data() : pull_root_blob.data();
      root.bytecode_length = pull_root_blob.empty() ? pull_vs_bytecode.size() : pull_root_blob.size();
      root.next = &layout;
      SM50ShaderBitcode compiled;
      std::shared_ptr<const ShaderStageArtifact> artifact;
      if (device_->GetShaderArtifactCache().CompileShader({pull_vs_bytecode.data(), pull_vs_bytecode.size()},
                      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                      "vs_main", &compiled, &compile_error, artifact)) {
        ERR("Failed to compile indexed pull VS: ", SM50GetErrorMessageString(compile_error));
        return {};
      }
      SM50_COMPILED_BITCODE bytecode{};
      if (artifact) bytecode = artifact->view;
      else SM50GetCompiledBitcode(compiled, &bytecode);
      if (FAILED(index_pull_vertex_constants.Initialize(device_, bytecode, compiled)))
        return {};
      auto lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
      if (!lib)
        return {};
      pull_vertex_function = device_->GetShaderArtifactCache().Function(lib, "vs_main");
      if (!pull_vertex_function)
        return {};
    }
    auto &variant = indirect ? pull_indirect_pso : pull_pso;
    if (!variant) {
      auto info = indirect_info;
      info.vertex_function = pull_vertex_function.handle;
      if (indirect && view_location_offsets) {
        if (!EnsureViewIndirectVertexFunction(true, false))
          return {};
        info.vertex_function = view_pull_indirect_vertex_function.handle;
      }
      info.support_indirect_command_buffers = indirect;
      
      variant = CreateNativePipeline(info, error);
      if (!variant)
        ERR("Failed to create indexed pull PSO: ", error.description().getUTF8String());
    }
    return variant;
  }

  WMT::RenderPipelineState GetPipelineStatisticsPipelineState(bool indirect) override {
    std::lock_guard<std::mutex> lock(statistics_pso_mutex);
    WMT::Reference<WMT::Error> error;
    if (!statistics_vertex_function && !(indirect && view_location_offsets)) {
      SM50Error compile_error;
      SM50_SHADER_COMMON_DATA common{};
      common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
      common.type = SM50_SHADER_COMMON;
      common.metal_version = kD3D12ShaderMetalVersion;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA statistics{};
      statistics.type = SM50_SHADER_D3D12_PIPELINE_STATS;
      statistics.next = &common;
      std::vector<SM50_SO_CAPTURE_ELEMENT> elements;
      elements.reserve(statistics_capture_elements.size());
      for (const auto &element : statistics_capture_elements)
        elements.push_back(SM50_SO_CAPTURE_ELEMENT{
            element.semantic_name.c_str(), element.semantic_index, element.start_component,
            element.component_count, element.dword_offset});
      SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA capture{};
      capture.type = SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE;
      capture.record_dwords = pipeline_statistics_capture.record_dwords;
      capture.num_elements = elements.size();
      capture.elements = elements.data();
      capture.next = &statistics;
      SM50_SHADER_GS_PASS_THROUGH_DATA no_raster{};
      no_raster.type = SM50_SHADER_GS_PASS_THROUGH;
      no_raster.DataEncoded = ~0u;
      no_raster.RasterizationDisabled = true;
      no_raster.next = &capture;
      SM50_SHADER_IA_INPUT_LAYOUT_DATA layout{};
      layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
      layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
      layout.slot_mask = slot_mask;
      layout.num_elements = pull_ia_elements.size();
      layout.elements = pull_ia_elements.data();
      layout.next = pipeline_statistics_capture.rasterization_enabled ||
                            stream_output.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM
                        ? static_cast<void *>(&capture) : static_cast<void *>(&no_raster);
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = pull_root_blob.empty() ? pull_vs_bytecode.data() : pull_root_blob.data();
      root.bytecode_length = pull_root_blob.empty() ? pull_vs_bytecode.size() : pull_root_blob.size();
      root.next = &layout;
      SM50ShaderBitcode compiled;
      std::shared_ptr<const ShaderStageArtifact> artifact;
      if (device_->GetShaderArtifactCache().CompileShader({pull_vs_bytecode.data(), pull_vs_bytecode.size()},
                      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                      "vs_main", &compiled, &compile_error, artifact)) {
        ERR("Failed to compile statistics VS: ", SM50GetErrorMessageString(compile_error));
        return {};
      }
      SM50_COMPILED_BITCODE bytecode{};
      if (artifact) bytecode = artifact->view;
      else SM50GetCompiledBitcode(compiled, &bytecode);
      if (FAILED(pipeline_statistics_vertex_constants.Initialize(device_, bytecode, compiled)))
        return {};
      auto lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
      if (!lib)
        return {};
      statistics_vertex_function = device_->GetShaderArtifactCache().Function(lib, "vs_main");
      if (!statistics_vertex_function)
        return {};
    }
    if (!EnsureStatisticsFragmentFunction())
      return {};
    auto &variant = indirect ? statistics_indirect_pso : statistics_pso;
    if (!variant) {
      auto info = indirect_info;
      info.vertex_function = statistics_vertex_function.handle;
      if (indirect && view_location_offsets) {
        if (!EnsureViewIndirectVertexFunction(false, true))
          return {};
        info.vertex_function = view_statistics_indirect_vertex_function.handle;
      }
      info.fragment_function = statistics_fragment_function.handle;
      info.support_indirect_command_buffers = indirect;
      
      
      
      
      variant = CreateNativePipeline(info, error);
      if (!variant)
        ERR("Failed to create statistics graphics PSO: ", error.description().getUTF8String());
    }
    return variant;
  }

  /* The counting PS variant (PSInvocations); caller holds statistics_pso_mutex. */
  bool EnsureStatisticsFragmentFunction() {
    if (!statistics_ps_bytecode.empty() && !statistics_fragment_function) {
      WMT::Reference<WMT::Error> error;
      SM50Error compile_error;
      SM50_SHADER_COMMON_DATA common{};
      common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
      common.type = SM50_SHADER_COMMON;
      common.metal_version = kD3D12ShaderMetalVersion;
      SM50_SHADER_D3D12_LOGIC_OP_DATA logic_op = fetch_logic_op;
      logic_op.next = &common;
      void *tail = use_fetch_logic_op ? static_cast<void *>(&logic_op) : static_cast<void *>(&common);
      SM50_SHADER_COMPILATION_ARGUMENT_DATA statistics{};
      statistics.type = SM50_SHADER_D3D12_PIPELINE_STATS;
      statistics.next = tail;
      /* Only mesh draws run this variant natively multisampled (other MSAA draws use the captured-pixel path):
       * a pixel-frequency PS then counts once per covered pixel, not once per covered sample. */
      SM50_SHADER_COMPILATION_ARGUMENT_DATA per_pixel{};
      if (EffectiveRasterSampleCount() > 1 && !owned_sample_frequency_pixel_shader) {
        // Forced-early depth/stencil: count once per (pixel, primitive) by the claim buffer.
        per_pixel.type = mesh_statistics_claim ? SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM
                                               : SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL;
        per_pixel.next = tail;
        statistics.next = &per_pixel;
      }
      SM50_SHADER_PSO_PIXEL_SHADER_DATA pixel{};
      pixel.type = SM50_SHADER_PSO_PIXEL_SHADER;
      pixel.dual_source_blending = statistics_ps_dual_source_blending;
      pixel.sample_mask = statistics_ps_sample_mask;
      pixel.native_sample_count = owned_sample_frequency_pixel_shader ? EffectiveRasterSampleCount() : 1;
      std::copy(statistics_ps_formats.begin(), statistics_ps_formats.end(), pixel.pixel_formats);
      pixel.next = &statistics;
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = pull_root_blob.empty() ? statistics_ps_bytecode.data() : pull_root_blob.data();
      root.bytecode_length = pull_root_blob.empty() ? statistics_ps_bytecode.size() : pull_root_blob.size();
      root.next = &pixel;
      SM50ShaderBitcode compiled;
      std::shared_ptr<const ShaderStageArtifact> artifact;
      if (device_->GetShaderArtifactCache().CompileShader({statistics_ps_bytecode.data(), statistics_ps_bytecode.size()},
                      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                      statistics_ps_name.c_str(), &compiled, &compile_error, artifact)) {
        ERR("Failed to compile statistics PS: ", SM50GetErrorMessageString(compile_error));
        return {};
      }
      SM50_COMPILED_BITCODE bytecode{};
      if (artifact) bytecode = artifact->view;
      else SM50GetCompiledBitcode(compiled, &bytecode);
      if (FAILED(pipeline_statistics_fragment_constants.Initialize(device_, bytecode, compiled)))
        return {};
      auto lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
      if (!lib)
        return {};
      statistics_fragment_function = device_->GetShaderArtifactCache().Function(lib, statistics_ps_name.c_str());
      if (!statistics_fragment_function)
        return false;
    }
    return true;
  }

  const MeshPipelineVariant *GetMeshPipeline(uint32_t key) override {
    // Only mesh-kind pipelines have variants; ordinary pipelines never reach mesh draws.
    if (mesh_kind == MeshPipelineKind::None)
      return nullptr;
    std::lock_guard<std::mutex> lock(mesh_pso_mutex);
    auto it = mesh_variants.find(key);
    if (it != mesh_variants.end())
      return it->second.pso ? &it->second : nullptr;
    auto &variant = mesh_variants[key];
    if (HRESULT hr = BuildMeshPipeline(key, variant); FAILED(hr)) {
      variant.pso = {};
      return nullptr;
    }
    return &variant;
  }

  const MSAAPipelineVariants *GetMSAAPipelineVariants(bool statistics_enabled, bool coarse) override {
    std::lock_guard<std::mutex> lock(msaa_pso_mutex);
    auto &cached = coarse ? msaa_coarse_variants[statistics_enabled]
        : statistics_enabled ? msaa_statistics_variants : msaa_variants;
    auto &attempted = coarse ? msaa_coarse_variants_attempted[statistics_enabled]
        : statistics_enabled ? msaa_statistics_variants_attempted : msaa_variants_attempted;
    if (attempted)
      return cached.get();
    attempted = true;
    const uint32_t sample_count = EffectiveRasterSampleCount();

    auto variants = std::make_unique<MSAAPipelineVariants>();
    const bool has_pixel_shader = !statistics_ps_bytecode.empty();
    variants->sample_frequency = has_pixel_shader && owned_sample_frequency_pixel_shader;
    if (!has_pixel_shader) {
      variants->layout.input_claim_dword = 0;
      variants->layout.input_coverage_dword = 1;
      variants->layout.z_claim_dword = 2;
      variants->layout.center_z_dword = 3;
      variants->layout.sample_z_dword = 4;
      variants->layout.z_anchor_dword = 4 + sample_count;
      variants->layout.input_record_dwords = variants->layout.z_anchor_dword +
                                             2 * (sample_count + 1);
      variants->layout.line_mode = owned_line_mode;
      // Conservative gathers publish their exact full/empty geometric mask like aliased lines.
      variants->layout.line_mask_dword = owned_conservative || owned_line_mode == SM50_MSAA_LINE_ALIASED ||
          owned_line_mode == SM50_MSAA_LINE_ALPHA_AA ? variants->layout.input_record_dwords++ : ~0u;
      variants->layout.line_alpha_dword = owned_line_mode == SM50_MSAA_LINE_ALPHA_AA
          ? variants->layout.input_record_dwords++ : ~0u;
      variants->layout.inner_mask_dword = owned_conservative ? variants->layout.input_record_dwords++ : ~0u;
      variants->layout.depth_bounds_mask_dword = emulated_depth_bounds ? variants->layout.input_record_dwords++ : ~0u;
      variants->layout.output_record_dwords = 2;
      variants->layout.input_record_count = 128;
      variants->layout.helper_record_base = 64;
      variants->layout.tile_width = 8;
    }
    auto metal = device_->GetMTLDevice();
    WMT::Reference<WMT::Error> error;
    SM50_SHADER_COMMON_DATA common{};
    common.flags = SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    const auto *vs_root_blob = pull_root_blob.empty() ? pull_vs_bytecode.data() : pull_root_blob.data();
    const size_t vs_root_size = pull_root_blob.empty() ? pull_vs_bytecode.size() : pull_root_blob.size();
    const auto *ps_root_blob = pull_root_blob.empty() ? statistics_ps_bytecode.data() : pull_root_blob.data();
    const size_t ps_root_size = pull_root_blob.empty() ? statistics_ps_bytecode.size() : pull_root_blob.size();
    WMT::Reference<WMT4::Function> vertex_replay;
    WMT::Reference<WMT4::Function> fragments[4];
    SM50_MSAA_INTERPOLATION_LAYOUT line_interpolation{};
    ShaderImmutableConstants *fragment_constants[] = {&variants->coverage_constants,
        &variants->pixel_constants, &variants->replay_fragment_constants,
        &variants->sample_fragment_constants};
    auto captured_topology = msaa_original_info.input_primitive_topology;
    auto compile_pixel = [&]() -> bool {
      for (uint32_t i = 0; has_pixel_shader && i < (variants->sample_frequency ? 4u : 3u); i++) {
      if (variants->sample_frequency && (i == 1 || (i == 2 && !uses_atomic64))) continue;
      SM50Error compile_error;
      SM50_SHADER_D3D12_MSAA_PIXEL_DATA msaa{};
      msaa.type = SM50_SHADER_D3D12_MSAA_PIXEL;
      msaa.variant = i == 3 ? SM50_MSAA_SAMPLE_APP : SM50_MSAA_COVERAGE + i;
      if (uses_atomic64) msaa.variant |= SM50_MSAA_ATOMIC_HELPERS;
      msaa.sample_count = sample_count;
      msaa.center_pattern = owned_sample_desc.Quality == 0xfffffffeu;
      msaa.layout_out = &variants->layout;
      msaa.coarse_vertex_layout = &variants->vertex_layout;
      msaa.depth_attachment = msaa_original_info.depth_pixel_format != WMTPixelFormatInvalid;
      msaa.single_sample_target = i == 2 && ForcedSampleReplay();
      msaa.line_mode = owned_line_mode;
      msaa.interpolation_out = owned_line_mode != SM50_MSAA_LINE_NONE ? &line_interpolation : nullptr;
      msaa.line_vertex_layout = owned_line_mode != SM50_MSAA_LINE_NONE
          ? &variants->vertex_layout : nullptr;
      const bool captured_inputs = owned_distance_interpolation || variants->vertex_layout.spill_count;
      msaa.distance_vertex_layout = captured_inputs ? &variants->vertex_layout : nullptr;
      msaa.depth_clip_enable = depth_clip_mode == WMTDepthClipModeClip;
      msaa.depth_bounds_emulated = emulated_depth_bounds;
      msaa.distance_vertex_count = !captured_inputs ? 0
          : captured_topology == WMTPrimitiveTopologyClassPoint ? 1
          : captured_topology == WMTPrimitiveTopologyClassLine ? 2 : 3;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA statistics{};
      statistics.type = SM50_SHADER_D3D12_PIPELINE_STATS;
      statistics.next = &common;
      SM50_SHADER_D3D12_LOGIC_OP_DATA logic_op = fetch_logic_op;
      logic_op.next = &common;
      // Replay (2) and sample-frequency (3) variants write the application RTs; logic ops apply per sample.
      void *tail = use_fetch_logic_op && (i == 2 || (i == 3 && !uses_atomic64)) ? static_cast<void *>(&logic_op)
                                                             : static_cast<void *>(&common);
      SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
      atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
      // DXIL lowering precedes coverage/replay extraction, so even the variants
      // that later discard the application body must first lower its atomics safely.
      common.metal_version = uses_atomic64 ? SM50_SHADER_METAL_410 : kD3D12ShaderMetalVersion;
      if (uses_atomic64) { atomic_phase.next = tail; tail = &atomic_phase; }
      statistics.next = tail;
      msaa.next = statistics_enabled && (i == 1 || i == 3) ? static_cast<void *>(&statistics) : tail;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA coarse_shading{};
      coarse_shading.type = SM50_SHADER_D3D12_COARSE_SHADING;
      coarse_shading.next = msaa.next;
      if (coarse) msaa.next = &coarse_shading;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA conservative_raster{};
      conservative_raster.type = SM50_SHADER_D3D12_CONSERVATIVE_RASTER;
      conservative_raster.next = msaa.next;
      if (owned_conservative) msaa.next = &conservative_raster;
      SM50_SHADER_PSO_PIXEL_SHADER_DATA pixel{};
      pixel.type = SM50_SHADER_PSO_PIXEL_SHADER;
      pixel.dual_source_blending = (i == 2 || i == 3) && msaa_dual_source;
      pixel.disable_depth_output = msaa_original_info.depth_pixel_format == WMTPixelFormatInvalid;
      pixel.sample_mask = statistics_ps_sample_mask;
      pixel.native_sample_count = i == 3 && owned_sample_frequency_pixel_shader && !uses_atomic64 ? sample_count : 1;
      if (i == 0)
        pixel.pixel_formats[0] = emulated_depth_bounds ? WMTPixelFormatR8Uint : WMTPixelFormatR32Uint;
      else if (i == 2 || i == 3)
        std::copy(statistics_ps_formats.begin(), statistics_ps_formats.end(), pixel.pixel_formats);
      pixel.next = &msaa;
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = ps_root_blob;
      root.bytecode_length = ps_root_size;
      root.next = &pixel;
      SM50ShaderBitcode compiled;
      std::shared_ptr<const ShaderStageArtifact> artifact;
      if (device_->GetShaderArtifactCache().CompileShader({statistics_ps_bytecode.data(), statistics_ps_bytecode.size()},
                      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                      statistics_ps_name.c_str(), &compiled, &compile_error, artifact)) {
        ERR("Failed to compile MSAA PS variant ", i, ": ", SM50GetErrorMessageString(compile_error));
        return false;
      }
      SM50_COMPILED_BITCODE bytecode{};
      if (artifact) bytecode = artifact->view;
      else SM50GetCompiledBitcode(compiled, &bytecode);
      if (FAILED(fragment_constants[i]->Initialize(device_, bytecode, compiled)))
        return false;
      auto library = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
      if (!library || !(fragments[i] = device_->GetShaderArtifactCache().Function(library, statistics_ps_name.c_str()))) {
        ERR("Failed to load MSAA PS variant ", i);
        return false;
      }
      }
      return true;
    };
    auto compile_vertex = [&](uint32_t i) -> bool {
      common.metal_version = uses_atomic64 && !i ? SM50_SHADER_METAL_410 : kD3D12ShaderMetalVersion;
      if (mesh_kind != MeshPipelineKind::None) {
        const uint32_t pass = mesh_kind == MeshPipelineKind::TessellationGeometry
            ? kMeshTessellationGeometryPass : 0;
        if (!i) {
          const auto *capture = GetMeshPipeline(MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false,
                                                              statistics_enabled) | pass | kMeshMSAACapture);
          if (!capture) return false;
          variants->vertex_layout = capture->vertex_layout;
          captured_topology = capture->capture_layout.vertices_per_primitive == 1 ? WMTPrimitiveTopologyClassPoint
              : capture->capture_layout.vertices_per_primitive == 2 ? WMTPrimitiveTopologyClassLine
                                                                    : WMTPrimitiveTopologyClassTriangle;
          return true;
        }
        MeshPipelineVariant replay;
        if (FAILED(BuildMeshPipeline(pass | (statistics_enabled ? 8 : 0) | kMeshMSAAReplay, replay, &line_interpolation))) return false;
        variants->vertex_layout = replay.vertex_layout;
        variants->replay_vertex_constants = std::move(replay.mesh_constants);
        vertex_replay = replay.replay_function;
        return bool(vertex_replay);
      }
      SM50Error compile_error;
      SM50_SHADER_D3D12_MSAA_VERTEX_DATA msaa{};
      msaa.type = SM50_SHADER_D3D12_MSAA_VERTEX;
      msaa.variant = i ? SM50_MSAA_VERTEX_REPLAY : SM50_MSAA_VERTEX_CAPTURE;
      msaa.sample_count = sample_count;
      msaa.layout_out = &variants->vertex_layout;
      msaa.line_mode = owned_line_mode;
      msaa.interpolation = owned_line_mode != SM50_MSAA_LINE_NONE ? &line_interpolation : nullptr;
      msaa.depth_clip_enable = depth_clip_mode == WMTDepthClipModeClip;
      msaa.front_counter_clockwise = winding == WMTWindingCounterClockwise;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA statistics{};
      statistics.type = SM50_SHADER_D3D12_PIPELINE_STATS;
      statistics.next = &common;
      msaa.next = statistics_enabled && !i ? static_cast<void *>(&statistics)
                                              : static_cast<void *>(&common);
      SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
      atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
      // Original DXIL is lowered before replay removes the application body.
      common.metal_version = uses_atomic64 ? SM50_SHADER_METAL_410 : kD3D12ShaderMetalVersion;
      if (uses_atomic64) { atomic_phase.next = msaa.next; msaa.next = &atomic_phase; }
      SM50_SHADER_COMPILATION_ARGUMENT_DATA conservative_raster{};
      conservative_raster.type = SM50_SHADER_D3D12_CONSERVATIVE_RASTER;
      conservative_raster.next = msaa.next;
      if (owned_conservative && i) msaa.next = &conservative_raster;
      SM50_SHADER_IA_INPUT_LAYOUT_DATA layout{};
      layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
      layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
      layout.slot_mask = slot_mask;
      layout.num_elements = pull_ia_elements.size();
      layout.elements = pull_ia_elements.data();
      layout.next = &msaa;
      std::vector<SM50_SO_CAPTURE_ELEMENT> capture_elements;
      SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA capture{};
      if (!i && (stream_output.active || statistics_enabled)) {
        const auto &elements = statistics_enabled ? statistics_capture_elements : stream_output.elements;
        capture_elements.reserve(elements.size());
        for (const auto &element : elements)
          capture_elements.push_back(SM50_SO_CAPTURE_ELEMENT{
              element.semantic_name.c_str(), element.semantic_index, element.start_component,
              element.component_count, element.dword_offset});
        capture.type = SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE;
        capture.record_dwords = statistics_enabled ? pipeline_statistics_capture.record_dwords
                                                   : stream_output.streams[0].record_dwords;
        capture.num_elements = capture_elements.size();
        capture.elements = capture_elements.data();
        capture.next = &msaa;
        layout.next = &capture;
      }
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = vs_root_blob;
      root.bytecode_length = vs_root_size;
      root.next = &layout;
      SM50ShaderBitcode compiled;
      std::shared_ptr<const ShaderStageArtifact> artifact;
      if (device_->GetShaderArtifactCache().CompileShader({pull_vs_bytecode.data(), pull_vs_bytecode.size()},
                      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                      "vs_main", &compiled, &compile_error, artifact)) {
        ERR("Failed to compile MSAA VS variant ", i, ": ", SM50GetErrorMessageString(compile_error));
        return false;
      }
      SM50_COMPILED_BITCODE bytecode{};
      if (artifact) bytecode = artifact->view;
      else SM50GetCompiledBitcode(compiled, &bytecode);
      auto &constants = i ? variants->replay_vertex_constants : variants->capture_constants;
      if (FAILED(constants.Initialize(device_, bytecode, compiled)))
        return false;
      auto library = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
      auto function = library ? device_->GetShaderArtifactCache().Function(library, "vs_main") : WMT::Reference<WMT4::Function>();
      if (!function)
        return false;
      if (i) vertex_replay = function;
      else {
        WMTComputePipelineInfo info;
        WMT::InitializeComputePipelineInfo(info);
        info.compute_function = function.handle;
        variants->capture = CreateNativePipeline(info, error);
      }
      if (!i && !variants->capture) return false;
      return true;
    };
    if (!compile_vertex(0) || !compile_pixel() || !compile_vertex(1))
      return nullptr;
    if (!variants->vertex_layout.record_dwords || !variants->layout.input_record_dwords ||
        !variants->layout.output_record_dwords || variants->layout.input_record_count < 128 ||
        variants->layout.helper_record_base < 64 ||
        variants->layout.helper_record_base + 64 > variants->layout.input_record_count)
      return nullptr;
    auto coverage_info = msaa_original_info;
    coverage_info.input_primitive_topology = captured_topology;
    for (auto &color : coverage_info.colors) color = {};
    // The application logic op applies to replayed RT writes only, never to internal coverage targets.
    coverage_info.logic_operation_enabled = false;
    // Every native sample carries only exact 0/1 coverage. R8Uint supports MSAA on Apple7/8 too.
    coverage_info.colors[0].pixel_format = emulated_depth_bounds ? WMTPixelFormatR8Uint : WMTPixelFormatR32Uint;
    coverage_info.colors[0].write_mask = WMTColorWriteMaskAll;
    coverage_info.vertex_function = vertex_replay;
    // Captured-vertex replay pipelines apply the view location through the amplification mapping (see
    // view_location_offsets); fullscreen variants below reset it to a maximum of 1.
    coverage_info.max_vertex_amplification_count = view_location_offsets ? 2 : 0;
    if (owned_line_mode != SM50_MSAA_LINE_NONE)
      coverage_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    auto internal = WMT4::Library(device_->GetLib().getLibrary());
    auto gather_fragment = device_->GetShaderArtifactCache().Function(internal, owned_conservative ? "msaa_gather_conservative_depth"
        : owned_line_mode == SM50_MSAA_LINE_NONE ? "msaa_gather_depth" : "msaa_gather_line_depth");
    auto explicit_depth_fragment = device_->GetShaderArtifactCache().Function(internal, sample_count == 1
        ? "msaa_explicit_depth_single" : "msaa_explicit_depth");
    auto depth_only_fragment = device_->GetShaderArtifactCache().Function(internal, sample_count == 1
        ? "msaa_explicit_depth_only_single" : "msaa_explicit_depth_only");
    if (!gather_fragment || !explicit_depth_fragment || !depth_only_fragment)
      return nullptr;
    if (emulated_depth_bounds) {
      auto probe = device_->GetShaderArtifactCache().Function(internal, sample_count == 1 ? "msaa_depth_bounds_probe_single"
                                                          : "msaa_depth_bounds_probe");
      if (!probe) return nullptr;
      WMTComputePipelineInfo info;
      WMT::InitializeComputePipelineInfo(info);
      info.compute_function = probe.handle;
      variants->depth_bounds_probe = CreateNativePipeline(info, error);
      if (!variants->depth_bounds_probe) return nullptr;
    }
    coverage_info.fragment_function = has_pixel_shader ? fragments[0] : gather_fragment;
    coverage_info.alpha_to_coverage_enabled = false;
    
    
    
    
    variants->coverage_with_depth = CreateNativePipeline(coverage_info, error);
    if (!variants->coverage_with_depth) {
      ERR("MSAA coverage-with-depth PSO failed: ", error.description().getUTF8String());
      return nullptr;
    }
    // The depthless variant is only for draws with no DSV bound. When a DSV is
    // bound, both early and late capture use the original depth format:
    // fixed D16 and floating D32 have different native depth-bias units.
    coverage_info.depth_pixel_format = WMTPixelFormatInvalid;
    coverage_info.stencil_pixel_format = WMTPixelFormatInvalid;
    variants->coverage = CreateNativePipeline(coverage_info, error);
    if (!variants->coverage) {
      ERR("MSAA depthless coverage PSO failed: ", error.description().getUTF8String());
      return nullptr;
    }
    auto clear_info = coverage_info;
    auto fullscreen_vertex = device_->GetShaderArtifactCache().Function(internal, "msaa_fullscreen_vertex");
    auto clear_fragment = device_->GetShaderArtifactCache().Function(internal, "msaa_clear_coverage");
    if (!fullscreen_vertex || !clear_fragment)
      return nullptr;
    clear_info.vertex_function = fullscreen_vertex;
    clear_info.max_vertex_amplification_count = 0;
    clear_info.fragment_function = clear_fragment;
    clear_info.depth_pixel_format = WMTPixelFormatInvalid;
    clear_info.stencil_pixel_format = WMTPixelFormatInvalid;
    clear_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    
    
    variants->clear_coverage = CreateNativePipeline(clear_info, error);
    if (!variants->clear_coverage) {
      ERR("MSAA clear PSO failed: ", error.description().getUTF8String());
      return nullptr;
    }
    // Every draw, regardless of PS frequency or presence, derives Z from the
    // same native fragment and the original read-only DSV format/bias.
    auto gather_info = coverage_info;
    gather_info.depth_pixel_format = msaa_original_info.depth_pixel_format;
    gather_info.stencil_pixel_format = msaa_original_info.stencil_pixel_format;
    gather_info.fragment_function = gather_fragment;
    
    variants->gather_depth = CreateNativePipeline(gather_info, error);
    WMTDepthStencilInfo capture_state{};
    capture_state.depth_compare_function = WMTCompareFunctionAlways;
    capture_state.depth_write_enabled = false;
    variants->capture_depth_state = metal.newDepthStencilState(capture_state);
    if (!variants->gather_depth || !variants->capture_depth_state)
      return nullptr;
    if (msaa_original_info.depth_pixel_format != WMTPixelFormatInvalid) {
      auto early_info = gather_info;
      early_info.fragment_function = explicit_depth_fragment;
      variants->early_depth = CreateNativePipeline(early_info, error);
      auto late_info = gather_info;
      for (auto &color : late_info.colors) color = {};
      late_info.fragment_function = depth_only_fragment;
      variants->late_depth = CreateNativePipeline(late_info, error);
      if (!variants->early_depth || !variants->late_depth)
        return nullptr;
    }
    auto pixel_info = coverage_info;
    for (auto &color : pixel_info.colors) color = {};
    pixel_info.depth_pixel_format = WMTPixelFormatInvalid;
    pixel_info.stencil_pixel_format = WMTPixelFormatInvalid;
    pixel_info.raster_sample_count = 1;
    pixel_info.vertex_function = fullscreen_vertex;
    WMT::Reference<WMT4::Function> atomic_tile_vertex;
    if (uses_atomic64) {
      atomic_tile_vertex = device_->GetShaderArtifactCache().Function(internal, "msaa_atomic_tile_vertex");
      if (!atomic_tile_vertex) return nullptr;
      pixel_info.vertex_function = atomic_tile_vertex;
    }
    pixel_info.max_vertex_amplification_count = 0;
    pixel_info.fragment_function = fragments[1];
    pixel_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    
    
    
    if (has_pixel_shader && !variants->sample_frequency)
      variants->pixel = CreateNativePipeline(pixel_info, error);
    if (has_pixel_shader && !variants->sample_frequency && !variants->pixel) {
      ERR("MSAA pixel PSO failed: ", error.description().getUTF8String());
      return nullptr;
    }
    auto replay_info = msaa_original_info;
    replay_info.input_primitive_topology = captured_topology;
    if (owned_line_mode != SM50_MSAA_LINE_NONE)
      replay_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    const bool single_sample_replay = ForcedSampleReplay();
    if (single_sample_replay) {
      // The fullscreen replay rasterizes at the RTV sample count (1 for forced > 1, N for forced 1 on N-sample RTVs).
      replay_info.raster_sample_count = owned_sample_desc.Count;
      replay_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
      
    }
    replay_info.vertex_function = single_sample_replay ? fullscreen_vertex : vertex_replay;
    // Fullscreen (single-sample) replay takes the already offset layer from the primitive bounds.
    replay_info.max_vertex_amplification_count = view_location_offsets && !single_sample_replay ? 2 : 0;
    replay_info.fragment_function = fragments[2];
    
    
    
    
    if (has_pixel_shader && (!variants->sample_frequency || uses_atomic64))
      variants->replay = CreateNativePipeline(replay_info, error);
    if (has_pixel_shader && (!variants->sample_frequency || uses_atomic64) && !variants->replay) {
      ERR("MSAA replay PSO failed: ", error.description().getUTF8String());
      return nullptr;
    }
    if (single_sample_replay && has_pixel_shader && !variants->sample_frequency) {
      auto query_info = clear_info;
      for (auto &color : query_info.colors) color = {};
      auto query_fragment = device_->GetShaderArtifactCache().Function(internal, sample_count == 1
          ? "msaa_query_coverage_single" : "msaa_query_coverage");
      if (!query_fragment) return nullptr;
      query_info.fragment_function = query_fragment;
      variants->query_coverage = CreateNativePipeline(query_info, error);
      if (!variants->query_coverage) {
        ERR("MSAA query coverage PSO failed: ", error.description().getUTF8String());
        return nullptr;
      }
      auto layered_vertex = device_->GetShaderArtifactCache().Function(internal, "msaa_fullscreen_layer_vertex");
      if (!layered_vertex) return nullptr;
      replay_info.vertex_function = layered_vertex;
      variants->replay_layer = CreateNativePipeline(replay_info, error);
      if (!variants->replay_layer) {
        ERR("MSAA layered fullscreen replay PSO failed: ", error.description().getUTF8String());
        return nullptr;
      }
    }
    if (variants->sample_frequency) {
      auto sample_info = uses_atomic64 ? pixel_info : replay_info;
      sample_info.raster_sample_count = uses_atomic64 ? 1 : sample_count;
      sample_info.fragment_function = fragments[3];
      
      
      variants->sample_pixel = CreateNativePipeline(sample_info, error);
      if (!variants->sample_pixel) {
        ERR("MSAA sample-frequency PSO failed: ", error.description().getUTF8String());
        return nullptr;
      }
    }
    cached = std::move(variants);
    return cached.get();
  }

  virtual WMT::DepthStencilState
  GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) override {
    if (!DSVPlanar)
      return device_->default_depth_stencil_state;

    if (DSVPlanar == 1) {
      if (DSVReadonlyFlags & 1)
        return dsso_readonly_no_stencil;
      else
        return dsso_no_stencil;
    }

    assert(DSVPlanar == 3);

    switch (DSVReadonlyFlags) {
    case 3:
      return dsso_readonly;
    case 2:
      return dsso_stencil_readonly;
    case 1:
      return dsso_depth_readonly;
    default:
      return dsso;
    }
  }
};

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState,
    const GraphicsPipelineExtras *extras
) {
  InitReturnPtr(ppPipelineState);
  if (!ppPipelineState || !pDesc || pDesc->NodeMask > 1 || pDesc->NumRenderTargets > 8 ||
      pDesc->InputLayout.NumElements > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
      (static_cast<UINT>(pDesc->Flags) & ~(static_cast<UINT>(D3D12_PIPELINE_STATE_FLAG_DEBUG) |
                                           kPipelineStateFlagDynamicDepthBias |
                                           kPipelineStateFlagDynamicIndexBufferStripCut)) ||
      !IsD3D12SampleCountSupported(pDevice, pDesc->SampleDesc.Count) ||
      !IsD3D12SampleQualitySupported(pDevice, pDesc->SampleDesc.Count, pDesc->SampleDesc.Quality) ||
      ((!pDesc->VS.pShaderBytecode || !pDesc->VS.BytecodeLength) && !(extras && extras->ms.pShaderBytecode)) ||
      (!pDesc->PS.pShaderBytecode != !pDesc->PS.BytecodeLength) ||
      (!pDesc->DS.pShaderBytecode != !pDesc->DS.BytecodeLength) ||
      (!pDesc->HS.pShaderBytecode != !pDesc->HS.BytecodeLength) ||
      (!pDesc->GS.pShaderBytecode != !pDesc->GS.BytecodeLength) ||
      (pDesc->InputLayout.NumElements && !pDesc->InputLayout.pInputElementDescs) ||
      (pDesc->StreamOutput.NumEntries && !pDesc->StreamOutput.pSODeclaration) ||
      (pDesc->StreamOutput.NumStrides && !pDesc->StreamOutput.pBufferStrides) ||
      (pDesc->CachedPSO.CachedBlobSizeInBytes && !pDesc->CachedPSO.pCachedBlob))
    return E_INVALIDARG;
  const auto &raster = pDesc->RasterizerState;
  if (pDesc->SampleDesc.Quality == 0xfffffffeu && raster.AntialiasedLineEnable &&
      pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
    return E_INVALIDARG;
  const UINT forced_samples = raster.ForcedSampleCount;
  // FS 3.5.6: ForcedSampleCount 1, 4, 8 or 16 (16 is emulated by two programmed 8x halves). Counts above 1 need
  // single-sample RTVs; 1 allows any RTV sample count. No DSV, depth test or PS depth output.
  if (forced_samples &&
      ((forced_samples != 1 && forced_samples != 4 && forced_samples != 8 && forced_samples != 16) ||
       (forced_samples != 16 && !IsD3D12SampleCountSupported(pDevice, forced_samples)) ||
       (forced_samples > 1 && pDesc->SampleDesc.Count != 1) || pDesc->DSVFormat != DXGI_FORMAT_UNKNOWN ||
       pDesc->DepthStencilState.DepthEnable))
    return E_INVALIDARG;
  if (forced_samples && pDesc->PS.pShaderBytecode) {
    microsoft::CSignatureParser output;
    if (FAILED(microsoft::DXBCGetOutputSignature(pDesc->PS.pShaderBytecode, &output)))
      return E_INVALIDARG;
    const microsoft::D3D11_SIGNATURE_PARAMETER *parameters;
    const UINT count = output.GetParameters(&parameters);
    for (UINT i = 0; i < count; ++i) {
      // DXBCParser maps depth output system values to UNDEFINED; retain the
      // signature's semantic name instead of trusting its converted enum.
      const char *semantic = parameters[i].SemanticName;
      if (semantic && (!strcasecmp(semantic, "SV_Depth") ||
                       !strcasecmp(semantic, "SV_DepthGreaterEqual") ||
                       !strcasecmp(semantic, "SV_DepthLessEqual")))
        return E_INVALIDARG;
    }
  }
  if ((raster.FillMode != D3D12_FILL_MODE_SOLID && raster.FillMode != D3D12_FILL_MODE_WIREFRAME) ||
      (raster.CullMode < D3D12_CULL_MODE_NONE || raster.CullMode > D3D12_CULL_MODE_BACK) ||
      raster.ConservativeRaster > D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON ||
      pDesc->IBStripCutValue > D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF ||
      (pDesc->PrimitiveTopologyType < D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT &&
       !(extras && extras->ms.pShaderBytecode)) || // the MS declares its own output topology
      pDesc->PrimitiveTopologyType > D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
    return E_INVALIDARG;
  // Conservative rasterization applies to the final solid triangles, including GS/DS/MS outputs.
  if (raster.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON) {
    if (raster.FillMode != D3D12_FILL_MODE_SOLID ||
        (pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
         pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH &&
         !(extras && extras->ms.pShaderBytecode)))
      return E_INVALIDARG;
  }
  const auto valid_compare = [](D3D12_COMPARISON_FUNC value) {
    return value >= D3D12_COMPARISON_FUNC_NEVER && value <= D3D12_COMPARISON_FUNC_ALWAYS;
  };
  const auto valid_stencil = [](D3D12_STENCIL_OP value) {
    return value >= D3D12_STENCIL_OP_KEEP && value <= D3D12_STENCIL_OP_DECR;
  };
  const auto &depth = pDesc->DepthStencilState;
  if ((depth.DepthEnable &&
       (depth.DepthWriteMask > D3D12_DEPTH_WRITE_MASK_ALL || !valid_compare(depth.DepthFunc))) ||
      (depth.StencilEnable &&
       ((!valid_compare(depth.FrontFace.StencilFunc) || !valid_compare(depth.BackFace.StencilFunc)) ||
        !valid_stencil(depth.FrontFace.StencilFailOp) || !valid_stencil(depth.FrontFace.StencilDepthFailOp) ||
        !valid_stencil(depth.FrontFace.StencilPassOp) || !valid_stencil(depth.BackFace.StencilFailOp) ||
        !valid_stencil(depth.BackFace.StencilDepthFailOp) || !valid_stencil(depth.BackFace.StencilPassOp))))
    return E_INVALIDARG;
  const auto valid_blend = [](D3D12_BLEND value) {
    const auto n = static_cast<UINT>(value);
    return n >= D3D12_BLEND_ZERO && n < std::size(kBlendFactorMap) && n != 12 && n != 13;
  };
  const auto &logic_target = pDesc->BlendState.RenderTarget[0];
  if (logic_target.LogicOpEnable &&
      (logic_target.BlendEnable || pDesc->BlendState.IndependentBlendEnable ||
       logic_target.LogicOp < D3D12_LOGIC_OP_CLEAR || logic_target.LogicOp > D3D12_LOGIC_OP_OR_INVERTED))
    return E_INVALIDARG;
  for (UINT i = 0; i < pDesc->NumRenderTargets; ++i) {
    if (pDesc->RTVFormats[i] == DXGI_FORMAT_UNKNOWN)
      continue;
    const auto &rt = pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0];
    if (rt.RenderTargetWriteMask & ~D3D12_COLOR_WRITE_ENABLE_ALL)
      return E_INVALIDARG;
    // Logic ops are configured only through RenderTarget[0] with IndependentBlendEnable FALSE.
    if (pDesc->BlendState.IndependentBlendEnable && rt.LogicOpEnable)
      return E_INVALIDARG;
    if (rt.BlendEnable &&
        (!valid_blend(rt.SrcBlend) || !valid_blend(rt.DestBlend) ||
         !valid_blend(rt.SrcBlendAlpha) || !valid_blend(rt.DestBlendAlpha) ||
         rt.BlendOp < D3D12_BLEND_OP_ADD || rt.BlendOp > D3D12_BLEND_OP_MAX ||
         rt.BlendOpAlpha < D3D12_BLEND_OP_ADD || rt.BlendOpAlpha > D3D12_BLEND_OP_MAX))
      return E_INVALIDARG;
  }
  if (pDesc->pRootSignature) {
    Com<ID3D12Device> owner;
    if (FAILED(pDesc->pRootSignature->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
        owner.ptr() != static_cast<ID3D12Device *>(pDevice))
      return E_INVALIDARG;
  }
  auto pso = Com(new MTLD3D12GraphicsPipelineStateImpl(pDevice));
  HRESULT hr = pso->Initialize(pDesc, extras ? *extras : GraphicsPipelineExtras{});
  if (pso->cache_.CanRetryAutomatic(hr)) {
    auto fresh = Com(new MTLD3D12GraphicsPipelineStateImpl(pDevice));
    pso->cache_.RetryAutomatic(fresh->cache_);
    pso = std::move(fresh); // Never reinitialize mutated bindings or native state.
    hr = pso->Initialize(pDesc, extras ? *extras : GraphicsPipelineExtras{});
  }
  // Same-key creators wait on this flight, so it completes now; a deferred link holds only the
  // entry's retention/publication (PipelineCache::CommitDeferred) until the link succeeds.
  pso->cache_.Complete(hr, !pso->async_job);
  if (FAILED(hr))
    return hr;
  if (pso->async_job)
    pDevice->async_pipelines.Submit(pso->async_job);
  return pso->QueryInterface(riid, ppPipelineState);
};

} // namespace dxmt
