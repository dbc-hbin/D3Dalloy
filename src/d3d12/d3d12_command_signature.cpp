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

#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"

namespace dxmt {

constexpr auto kSharedHeader = R"(
#include <metal_stdlib>

using namespace metal;

struct dxmt_compute_command_data {
  command_buffer cmd_buf;
  ulong max_count;
  device uint * max_count_buffer;
  device char * argument_buffer;
  device ulong * predicate;
  uint predicate_op;
  device ulong * static_samplers;
  device ulong * rootsig_qwords;
  device char * immutable_constants;
  uint rootsig_qwords_stride;
  packed_uint3 tgsize;
};

struct d3d12_draw_arguments {
  uint vertex_count_per_instance;
  uint instance_count;
  uint start_vertex_location;
  uint start_instance_location;
};

struct d3d12_draw_indexed_arguments {
  uint index_count_per_instance;
  uint instance_count;
  uint start_index_location;
  int base_vertex_location;
  uint start_instance_location;
};

struct d3d12_vertex_buffer_view {
  device void * buffer;
  uint size_in_bytes;
  uint stride_in_bytes;
};

struct d3d12_index_buffer_view {
  device void * buffer;
  uint size_in_bytes;
  uint format;
};

struct dxmt_vertex_buffer {
  device void * buffer;
  uint stride;
  uint length;
};

struct dxmt_render_command_data {
  command_buffer cmd_buf;
  ulong max_count;
  device uint * max_count_buffer;
  device char * argument_buffer;
  device ulong * predicate;
  uint predicate_op;
  device ulong * static_samplers;
  device ulong * rootsig_qwords;
  device char * vertex_constants;
  device char * fragment_constants;
  uint rootsig_qwords_stride;
  uint primitive_type;
  device char * vertex_buffer;
  device void * index_buffer;
  uint index_buffer_format;
  uint vertex_argbuf_stride;
};

)";

class MTLD3D12CommandSignatureImpl : public MTLD3D12Pageable<MTLD3D12CommandSignature> {

public:
  MTLD3D12CommandSignatureImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12CommandSignature>(pDevice) {}

  HRESULT
  Initialize(const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature) {
    if (!pDesc || !pDesc->NumArgumentDescs || !pDesc->pArgumentDescs || !pDesc->ByteStride ||
        (pDesc->ByteStride & 3))
      return E_INVALIDARG;

    if (pRootSignature) {
      Com<ID3D12Device> owner;
      if (FAILED(pRootSignature->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != device_)
        return E_INVALIDARG;
      if (static_cast<MTLD3D12RootSignature *>(pRootSignature)->IsLocal)
        return E_INVALIDARG;
    }

    std::stringstream source;
    D3D12_INDIRECT_ARGUMENT_TYPE side_effect = ~(D3D12_INDIRECT_ARGUMENT_TYPE){};
    UpdateRootArguments = false;
    UpdateVertexBuffers = false;
    RootParameterMask = 0;
    VertexBufferMask = 0;
    uint32_t ib_index = ~0u;
    uint64_t packed_size = 0;

    source << kSharedHeader;

    source << "struct __attribute__ ((packed)) d3d12_arguments {\n";

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      if (~side_effect != 0u)
        return E_INVALIDARG;
      auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        side_effect = arg.Type;
        packed_size += sizeof(D3D12_DISPATCH_ARGUMENTS);
        source << "packed_uint3 dispatch;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
        side_effect = arg.Type;
        packed_size += sizeof(D3D12_DRAW_ARGUMENTS);
        source << "d3d12_draw_arguments draw;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
        side_effect = arg.Type;
        packed_size += sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        source << "d3d12_draw_indexed_arguments draw_indexed;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
        if (arg.VertexBuffer.Slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
            (VertexBufferMask & (1u << arg.VertexBuffer.Slot)))
          return E_INVALIDARG;
        VertexBufferMask |= 1u << arg.VertexBuffer.Slot;
        UpdateVertexBuffers = true;
        packed_size += sizeof(D3D12_VERTEX_BUFFER_VIEW);
        source << "d3d12_vertex_buffer_view vb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
        if (ib_index != ~0u)
          return E_INVALIDARG;
        ib_index = i;
        packed_size += sizeof(D3D12_INDEX_BUFFER_VIEW);
        source << "d3d12_index_buffer_view ib;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
        if (!arg.Constant.Num32BitValuesToSet)
          return E_INVALIDARG;
        UpdateRootArguments = true;
        packed_size += sizeof(uint32_t) * arg.Constant.Num32BitValuesToSet;
        for (unsigned j = 0; j < arg.Constant.Num32BitValuesToSet; j++) {
          source << "uint constant_" << i << "_" << j << ";\n";
        }
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
        UpdateRootArguments = true;
        packed_size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        source << "ulong cb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
        UpdateRootArguments = true;
        packed_size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        source << "ulong srv_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        UpdateRootArguments = true;
        packed_size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        source << "ulong uav_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
        ERR("D3D12CommandSignatuer: unsupported rays/mesh dispatch");
        return E_NOTIMPL;
      default:
        return E_INVALIDARG;
      }
    }
    source << "};\n\n";

    if (~side_effect == 0 || packed_size > pDesc->ByteStride)
      return E_INVALIDARG;
    bool is_compute = side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    if ((is_compute && (UpdateVertexBuffers || ib_index != ~0u)) ||
        (ib_index != ~0u && side_effect != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED))
      return E_INVALIDARG;
    CommandType = side_effect;
    ByteStride = pDesc->ByteStride;
    UpdateIndexBuffer = ib_index != ~0u;

    if (is_compute)
      source
          << "[[kernel]] void resolve_indirect_commands([[thread_position_in_grid]] uint x, constant dxmt_compute_command_data";
    else
      source << "[[vertex]] void resolve_indirect_commands(constant dxmt_render_command_data";
    source << " &command_data [[buffer(30)]]) {\n";

    if (is_compute)
      source << "if (x !=0 ) return;\n";

    source << "uint count = command_data.max_count_buffer ? "
              "command_data.max_count_buffer[0] : command_data.max_count;\n";
    source << "if (command_data.predicate && (command_data.predicate_op ? command_data.predicate[0] != 0 : "
              "command_data.predicate[0] == 0)) count = 0;\n";
    source << "for (uint i = 0; i < command_data.max_count; i++) {\n";
    source << "device d3d12_arguments& arg = reinterpret_cast<device d3d12_arguments *>("
              "command_data.argument_buffer + i * "
           << pDesc->ByteStride << ")[0];\n";
    if (is_compute) {
      source << "compute_command cmd(command_data.cmd_buf, i);\n";
    } else {
      source << "render_command cmd(command_data.cmd_buf, i);\n";
    }
    source << "cmd.reset();\n";
    source << "if (i >= count) continue;\n";
    source << "device ulong * rootsig_qwords = command_data.rootsig_qwords ? command_data.rootsig_qwords + "
              "(i * command_data.rootsig_qwords_stride) : nullptr;\n";
    if (!is_compute)
      source << "device dxmt_vertex_buffer * vertex_buffer = "
                "reinterpret_cast<device dxmt_vertex_buffer *>(command_data.vertex_buffer + "
                "(i * command_data.vertex_argbuf_stride));\n";

    if (UpdateRootArguments || UpdateVertexBuffers || UpdateIndexBuffer) {
      if (!is_compute) {
        source << "cmd.set_vertex_buffer(vertex_buffer," << SM50_BINDING_INDEX_VERTEX_BUFFER << ");\n";
        source << "if (rootsig_qwords) cmd.set_vertex_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
        source << "cmd.set_vertex_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
        source << "if (rootsig_qwords) cmd.set_fragment_buffer(rootsig_qwords," << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
        source << "cmd.set_fragment_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS
               << ");\n";
        source << "if (command_data.vertex_constants) cmd.set_vertex_buffer(command_data.vertex_constants,"
               << SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS << ");\n";
        source << "if (command_data.fragment_constants) cmd.set_fragment_buffer(command_data.fragment_constants,"
               << SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS << ");\n";
      } else {
        source << "if (rootsig_qwords) cmd.set_kernel_buffer(rootsig_qwords, " << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
        source << "cmd.set_kernel_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
        source << "if (command_data.immutable_constants) cmd.set_kernel_buffer(command_data.immutable_constants,"
               << SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS << ");\n";
      }
    }

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
        source << "cmd.draw_primitives((primitive_type)command_data.primitive_type, "
                  "arg.draw.start_vertex_location, "
                  "arg.draw.vertex_count_per_instance, arg.draw.instance_count, "
                  "arg.draw.start_instance_location);\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
        if (ib_index == ~0u) {
          source << "bool ib32bit = command_data.index_buffer_format == 42;\n";
          source << "device void* ib = command_data.index_buffer;\n";
        }
        source << "if (ib32bit) {\n";
        source << "cmd.draw_indexed_primitives((primitive_type)command_data.primitive_type, "
                  "arg.draw_indexed.index_count_per_instance, "
                  "reinterpret_cast<device uint *>(ib) + arg.draw_indexed.start_index_location, "
                  "arg.draw_indexed.instance_count, arg.draw_indexed.base_vertex_location, "
                  "arg.draw_indexed.start_instance_location);\n";
        source << "} else {\n";
        source << "cmd.draw_indexed_primitives((primitive_type)command_data.primitive_type, "
                  "arg.draw_indexed.index_count_per_instance, "
                  "reinterpret_cast<device ushort *>(ib) + arg.draw_indexed.start_index_location, "
                  "arg.draw_indexed.instance_count, arg.draw_indexed.base_vertex_location, "
                  "arg.draw_indexed.start_instance_location);\n";
        source << "}\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        source << "cmd.concurrent_dispatch_threadgroups(arg.dispatch, command_data.tgsize);\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
        auto slot = arg.VertexBuffer.Slot;
        source << "vertex_buffer[" << slot << "] = {arg.vb_" << i << ".buffer,arg.vb_" << i
               << ".stride_in_bytes,arg.vb_" << i << ".size_in_bytes};\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
        source << "bool ib32bit = arg.ib.format == 42;\n";
        source << "device void* ib = arg.ib.buffer;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
        auto parameter_index = arg.Constant.RootParameterIndex;
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots || parameter_index >= 64 ||
            rootsig->ParameterTypes[parameter_index] != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
            arg.Constant.DestOffsetIn32BitValues > rootsig->ParameterConstantCounts[parameter_index] ||
            arg.Constant.Num32BitValuesToSet >
                rootsig->ParameterConstantCounts[parameter_index] - arg.Constant.DestOffsetIn32BitValues)
          return E_INVALIDARG;
        RootParameterMask |= uint64_t(1) << parameter_index;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        for (unsigned j = 0; j < arg.Constant.Num32BitValuesToSet; j++) {
          source << "reinterpret_cast<device uint *>(rootsig_qwords + " << offset << ")["
                 << (j + arg.Constant.DestOffsetIn32BitValues) << "] = arg.constant_" << i << "_" << j << ";\n";
        }
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
        auto parameter_index = arg.ConstantBufferView.RootParameterIndex;
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots || parameter_index >= 64 ||
            rootsig->ParameterTypes[parameter_index] != D3D12_ROOT_PARAMETER_TYPE_CBV)
          return E_INVALIDARG;
        RootParameterMask |= uint64_t(1) << parameter_index;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        source << "rootsig_qwords[" << offset << "] = arg.cb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
        auto parameter_index = arg.ShaderResourceView.RootParameterIndex;
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots || parameter_index >= 64 ||
            rootsig->ParameterTypes[parameter_index] != D3D12_ROOT_PARAMETER_TYPE_SRV)
          return E_INVALIDARG;
        RootParameterMask |= uint64_t(1) << parameter_index;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        source << "rootsig_qwords[" << offset << "] = arg.srv_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        auto parameter_index = arg.UnorderedAccessView.RootParameterIndex;
        if (!pRootSignature)
          return E_INVALIDARG;
        auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
        if (parameter_index >= rootsig->ParameterSlots || parameter_index >= 64 ||
            rootsig->ParameterTypes[parameter_index] != D3D12_ROOT_PARAMETER_TYPE_UAV)
          return E_INVALIDARG;
        RootParameterMask |= uint64_t(1) << parameter_index;
        auto offset = rootsig->SlotQwordOffsets[parameter_index];
        source << "rootsig_qwords[" << offset << "] = arg.uav_" << i << ";\n";
        break;
      }
      default:
        return E_INVALIDARG;
      }
    }

    source << "}\n"
              "};\n";

    WMT::Reference<WMT::Error> err;
    auto lib = device_->GetMTLDevice().newLibraryWithSource(source.view(), err);

    if (!lib) {
      ERR("Failed to compile command signature resolve shader: ", err.description().getUTF8String());
      return E_FAIL;
    }

    auto function = lib.newFunction("resolve_indirect_commands");

    if (!function) {
      ERR("Failed to create command signature resolve shader");
      return E_FAIL;
    }

    if (is_compute) {
      compute_resolver = device_->GetMTLDevice().newComputePipelineState(function, err);
    } else {
      WMTRenderPipelineInfo info;
      WMT::InitializeRenderPipelineInfo(info);
      info.rasterization_enabled = false;
      info.vertex_function = function;
      render_resolver = device_->GetMTLDevice().newRenderPipelineState(info, err);
    }

    if (err) {
      ERR("Failed to compile command signature resolve pso: ", err.description().getUTF8String());
      return E_FAIL;
    }

    return S_OK;
  };

  ~MTLD3D12CommandSignatureImpl() {}

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12CommandSignature: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }
};

HRESULT
CreateCommandSignature(
    MTLD3D12Device *pDevice, const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature,
    REFIID riid, void **ppCommandSignature
) {
  if (!ppCommandSignature)
    return E_POINTER;
  *ppCommandSignature = nullptr;
  if (!pDesc)
    return E_INVALIDARG;
  auto sig = Com(new MTLD3D12CommandSignatureImpl(pDevice));
  HRESULT hr = sig->Initialize(pDesc, pRootSignature);
  if (FAILED(hr))
    return hr;
  return sig->QueryInterface(riid, ppCommandSignature);
}

} // namespace dxmt