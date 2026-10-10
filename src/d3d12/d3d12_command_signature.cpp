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
  device uint * pipeline_statistics;
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

struct dxmt_index_pull_control {
  uint index_size;
  uint index_first;
  int base_vertex;
  uint reserved;
};

struct dxmt_so_draw_arguments {
  uint vertex_count;
  uint instance_count;
  uint start_location;
  uint start_instance;
  int base_vertex;
  uint index_format;
  ulong index_address;
};

struct dxmt_so_capture_binding {
  device ushort * index_address;
  device uint * control_address;
  device uint * scratch_address;
  device uint * positions_address;
  dxmt_so_draw_arguments args;
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
  device dxmt_index_pull_control * index_pull_controls;
  uint index_pull_mode;
  uint index_pull_padding;
  device dxmt_so_capture_binding * so_draws;
  device uint * pipeline_statistics;
};

)";

class MTLD3D12CommandSignatureImpl : public MTLD3D12Pageable<MTLD3D12CommandSignature> {

public:
  MTLD3D12CommandSignatureImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12CommandSignature>(pDevice) {}

  /*
   * DISPATCH_MESH / DISPATCH_RAYS. `source` holds the shared header and the packed `d3d12_arguments` struct. With
   * root argument updates, `resolve_root_arguments` (one thread per slot) writes each executed slot's constants
   * and root views into that slot's copy of the root arguments (`dxmt_slot_root_control`); the command list
   * binds slot i's copy for slot i's dispatch. Validation matches the ICB path.
   */
  HRESULT
  InitializeSlotDispatch(const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature,
                         std::stringstream source) {
    if (!UpdateRootArguments)
      return S_OK;
    if (!pRootSignature)
      return E_INVALIDARG;
    auto rootsig = static_cast<MTLD3D12RootSignature *>(pRootSignature);
    source << "struct dxmt_slot_root_control {\n"
              "  device char *arguments;\n"
              "  device const uint *count;\n"
              "  device const ulong *predicate;\n"
              "  device ulong *roots;\n"
              "  uint max_commands;\n"
              "  uint roots_stride;\n"
              "  uint predicate_not_equal;\n"
              "};\n"
              "kernel void resolve_root_arguments(uint i [[thread_position_in_grid]],\n"
              "    constant dxmt_slot_root_control &control [[buffer(0)]]) {\n"
              "  if (i >= control.max_commands) return;\n"
              "  uint limit = control.count ? control.count[0] : control.max_commands;\n"
              "  if (i >= limit) return;\n"
              "  if (control.predicate && (control.predicate_not_equal ? control.predicate[0] != 0 "
              ": control.predicate[0] == 0)) return;\n"
              "  device d3d12_arguments &arg = *reinterpret_cast<device d3d12_arguments *>(control.arguments + "
              "ulong(i) * " << pDesc->ByteStride << ");\n"
              "  device ulong *rootsig_qwords = control.roots + ulong(i) * control.roots_stride;\n";
    auto view = [&](UINT parameter_index, D3D12_ROOT_PARAMETER_TYPE type, const char *field, unsigned i) {
      if (parameter_index >= rootsig->ParameterSlots || parameter_index >= 64 ||
          rootsig->ParameterTypes[parameter_index] != type)
        return false;
      RootParameterMask |= uint64_t(1) << parameter_index;
      source << "  rootsig_qwords[" << rootsig->SlotQwordOffsets[parameter_index] << "] = arg." << field << i
             << ";\n";
      /* TILED: an indirect root view reports fully mapped, as on the ICB path. */
      if (type != D3D12_ROOT_PARAMETER_TYPE_CBV && rootsig->StatusQwordOffsets[parameter_index] != ~0u)
        source << "  rootsig_qwords[" << rootsig->StatusQwordOffsets[parameter_index] << "] = 0;\n";
      return true;
    };
    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      const auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
        const auto parameter_index = arg.Constant.RootParameterIndex;
        if (parameter_index >= rootsig->ParameterSlots || parameter_index >= 64 ||
            rootsig->ParameterTypes[parameter_index] != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
            arg.Constant.DestOffsetIn32BitValues > rootsig->ParameterConstantCounts[parameter_index] ||
            arg.Constant.Num32BitValuesToSet >
                rootsig->ParameterConstantCounts[parameter_index] - arg.Constant.DestOffsetIn32BitValues)
          return E_INVALIDARG;
        RootParameterMask |= uint64_t(1) << parameter_index;
        for (unsigned j = 0; j < arg.Constant.Num32BitValuesToSet; j++)
          source << "  reinterpret_cast<device uint *>(rootsig_qwords + " << rootsig->SlotQwordOffsets[parameter_index]
                 << ")[" << (j + arg.Constant.DestOffsetIn32BitValues) << "] = arg.constant_" << i << "_" << j
                 << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        if (!view(arg.ConstantBufferView.RootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_CBV, "cb_", i))
          return E_INVALIDARG;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        if (!view(arg.ShaderResourceView.RootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_SRV, "srv_", i))
          return E_INVALIDARG;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        if (!view(arg.UnorderedAccessView.RootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_UAV, "uav_", i))
          return E_INVALIDARG;
        break;
      default:
        break;
      }
    }
    source << "}\n";
    WMT::Reference<WMT::Error> err;
    auto lib = device_->GetMTLDevice().newLibraryWithSource(source.view(), err);
    if (!lib) {
      ERR("Failed to compile command signature root argument resolver: ", err.description().getUTF8String());
      return E_FAIL;
    }
    auto function = WMT4::Library(lib).newFunction("resolve_root_arguments");
    if (!function)
      return E_FAIL;
    root_argument_resolver = device_->GetMetal4Compiler().newComputePipelineState(function, err);
    if (!root_argument_resolver) {
      ERR("Failed to create command signature root argument resolver pso");
      return E_FAIL;
    }
    return S_OK;
  }

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
    DrawArgumentOffset = ~0u;
    IndexBufferArgumentOffset = ~0u;

    /*
     * DISPATCH_MESH and DISPATCH_RAYS use per-slot dispatch arguments rather than an indirect command buffer.
     * Their root updates, and the roots of GS/HS/DS draw signatures, are applied by root_argument_resolver
     * to the slot's complete root table, including status and view-id suffixes. Vertex/index views remain draw-only.
     * DrawArgumentOffset locates the final draw or dispatch arguments after the signature's binding mutations.
     */
    bool slot_dispatch = false;
    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      const auto type = pDesc->pArgumentDescs[i].Type;
      slot_dispatch |= type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH ||
                       type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    }

    source << kSharedHeader;

    source << "struct __attribute__ ((packed)) d3d12_arguments {\n";

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      if (~side_effect != 0u)
        return E_INVALIDARG;
      auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        side_effect = arg.Type;
        DrawArgumentOffset = packed_size;
        packed_size += sizeof(D3D12_DISPATCH_ARGUMENTS);
        source << "packed_uint3 dispatch;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: {
        side_effect = arg.Type;
        DrawArgumentOffset = packed_size;
        packed_size += sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
        source << "packed_uint3 dispatch_mesh;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: {
        side_effect = arg.Type;
        DrawArgumentOffset = packed_size;
        packed_size += sizeof(D3D12_DISPATCH_RAYS_DESC);
        source << "char dispatch_rays[" << sizeof(D3D12_DISPATCH_RAYS_DESC) << "];\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
        side_effect = arg.Type;
        DrawArgumentOffset = packed_size;
        packed_size += sizeof(D3D12_DRAW_ARGUMENTS);
        source << "d3d12_draw_arguments draw;\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
        side_effect = arg.Type;
        DrawArgumentOffset = packed_size;
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
        IndexBufferArgumentOffset = packed_size;
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
        atomic64_root_fields.push_back({uint32_t(packed_size), false, arg.ConstantBufferView.RootParameterIndex});
        packed_size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        source << "ulong cb_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
        UpdateRootArguments = true;
        atomic64_root_fields.push_back({uint32_t(packed_size), false, arg.ShaderResourceView.RootParameterIndex});
        packed_size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        source << "ulong srv_" << i << ";\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
        UpdateRootArguments = true;
        atomic64_root_fields.push_back({uint32_t(packed_size), true, arg.UnorderedAccessView.RootParameterIndex});
        packed_size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        source << "ulong uav_" << i << ";\n";
        break;
      }
      default:
        return E_INVALIDARG;
      }
    }
    source << "};\n\n";

    if (~side_effect == 0 || packed_size > pDesc->ByteStride)
      return E_INVALIDARG;
    bool is_compute = side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    if ((is_compute && (UpdateVertexBuffers || ib_index != ~0u)) ||
        (slot_dispatch && (UpdateVertexBuffers || ib_index != ~0u)) ||
        (ib_index != ~0u && side_effect != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED))
      return E_INVALIDARG;
    CommandType = side_effect;
    ByteStride = pDesc->ByteStride;
    UpdateIndexBuffer = ib_index != ~0u;
    if (slot_dispatch)
      return InitializeSlotDispatch(pDesc, pRootSignature, std::move(source));
    if (!is_compute && UpdateRootArguments) {
      std::stringstream root_source;
      root_source << source.view();
      const HRESULT hr = InitializeSlotDispatch(pDesc, pRootSignature, std::move(root_source));
      if (FAILED(hr))
        return hr;
    }
    const bool updates_graphics_bindings = !is_compute && (UpdateRootArguments || UpdateVertexBuffers);
    std::stringstream graphics_updates;
    if (updates_graphics_bindings)
      source << "void update_graphics_bindings(device d3d12_arguments &arg, device ulong *rootsig_qwords, "
                "device dxmt_vertex_buffer *vertex_buffer);\n";

    if (is_compute)
      source
          << "[[kernel]] void resolve_indirect_commands([[thread_position_in_grid]] uint i, constant dxmt_compute_command_data";
    else
      source << "[[vertex]] void resolve_indirect_commands(uint i [[vertex_id]], constant dxmt_render_command_data";
    source << " &command_data [[buffer(30)]]) {\n";

    if (is_compute)
      source << "if (ulong(i) >= command_data.max_count) return;\n";

    source << "uint count = command_data.max_count_buffer ? "
              "command_data.max_count_buffer[0] : command_data.max_count;\n";
    source << "if (command_data.predicate && (command_data.predicate_op ? command_data.predicate[0] != 0 : "
              "command_data.predicate[0] == 0)) count = 0;\n";
    source << "device d3d12_arguments& arg = reinterpret_cast<device d3d12_arguments *>("
              "command_data.argument_buffer + " << (is_compute ? "ulong(i)" : "i") << " * "
           << pDesc->ByteStride << ")[0];\n";
    if (is_compute) {
      source << "compute_command cmd(command_data.cmd_buf, i);\n";
    } else {
      source << "render_command cmd(command_data.cmd_buf, i);\n";
    }
    source << "cmd.reset();\n";
    source << "if (i >= count) return;\n";
    if (!is_compute)
      source << "if (command_data.so_draws && (!command_data.so_draws[i].args.vertex_count || "
                "!command_data.so_draws[i].args.instance_count)) return;\n";
    source << "device ulong * rootsig_qwords = command_data.rootsig_qwords ? command_data.rootsig_qwords + "
           << "(" << (is_compute ? "ulong(i)" : "i") << " * command_data.rootsig_qwords_stride) : nullptr;\n";
    if (!is_compute)
      source << "device dxmt_vertex_buffer * vertex_buffer = "
                "reinterpret_cast<device dxmt_vertex_buffer *>(command_data.vertex_buffer + "
                "(i * command_data.vertex_argbuf_stride));\n";
    if (updates_graphics_bindings)
      source << "update_graphics_bindings(arg, rootsig_qwords, vertex_buffer);\n";

    if (UpdateRootArguments || UpdateVertexBuffers || UpdateIndexBuffer ||
        side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
        side_effect == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW) {
      if (!is_compute) {
        if (!(UpdateRootArguments || UpdateVertexBuffers || UpdateIndexBuffer))
          source << "if (command_data.rootsig_qwords || command_data.so_draws || command_data.index_pull_mode != ~0u) {\n";
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
        if (!(UpdateRootArguments || UpdateVertexBuffers || UpdateIndexBuffer))
          source << "}\n";
        source << "if (command_data.pipeline_statistics) {\n"
                  "cmd.set_vertex_buffer(command_data.pipeline_statistics,25);\n"
                  "cmd.set_fragment_buffer(command_data.pipeline_statistics,25);\n}\n";
      } else {
        source << "if (rootsig_qwords) cmd.set_kernel_buffer(rootsig_qwords, " << SM50_BINDING_INDEX_ROOT_ARGUMENTS << ");\n";
        source << "cmd.set_kernel_buffer(command_data.static_samplers," << SM50_BINDING_INDEX_STATIC_SAMPLERS << ");\n";
        source << "if (command_data.immutable_constants) cmd.set_kernel_buffer(command_data.immutable_constants,"
               << SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS << ");\n";
        source << "if (command_data.pipeline_statistics) cmd.set_kernel_buffer(command_data.pipeline_statistics,25);\n";
      }
    }

    for (unsigned i = 0; i < pDesc->NumArgumentDescs; i++) {
      auto &arg = pDesc->pArgumentDescs[i];
      switch (arg.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
        source << "if (command_data.so_draws) {\n"
                  "device dxmt_so_capture_binding &so = command_data.so_draws[i];\n"
                  "cmd.set_vertex_buffer(so.index_address,26);\n"
                  "cmd.set_vertex_buffer(so.control_address,27);\n"
                  "cmd.set_vertex_buffer(so.scratch_address,28);\n"
                  "cmd.draw_primitives((primitive_type)command_data.primitive_type, "
                  "so.args.start_location,so.args.vertex_count,so.args.instance_count,"
                  "so.args.start_instance);\n"
                  "} else {\n";
        source << "cmd.draw_primitives((primitive_type)command_data.primitive_type, "
                  "arg.draw.start_vertex_location, "
                  "arg.draw.vertex_count_per_instance, arg.draw.instance_count, "
                  "arg.draw.start_instance_location);\n"
                  "}\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
        if (ib_index == ~0u) {
          source << "bool ib32bit = command_data.index_buffer_format == 42;\n";
          source << "device void* ib = command_data.index_buffer;\n";
        }
        source << "if (command_data.so_draws) {\n"
                  "device dxmt_so_capture_binding &so = command_data.so_draws[i];\n"
                  "cmd.set_vertex_buffer(so.index_address,26);\n"
                  "cmd.set_vertex_buffer(so.control_address,27);\n"
                  "cmd.set_vertex_buffer(so.scratch_address,28);\n"
                  "cmd.draw_indexed_primitives((primitive_type)command_data.primitive_type, "
                  "so.args.vertex_count,so.positions_address,"
                  "so.args.instance_count,0,so.args.start_instance);\n"
                  "} else {\n";
        source << "if (command_data.index_pull_mode != ~0u) {\n"
                  "bool pull = command_data.index_pull_mode != (ib32bit ? 2u : 1u);\n"
                  "device dxmt_index_pull_control *control = command_data.index_pull_controls + i;\n"
                  "if (pull) {\n"
                  "uint size = ib32bit ? 4u : 2u;\n"
                  "ulong first = (ulong)ib + (ulong)arg.draw_indexed.start_index_location * size;\n"
                  "control->index_size = size;\n"
                  "control->index_first = uint((first & 3ul) / 2ul);\n"
                  "control->base_vertex = arg.draw_indexed.base_vertex_location;\n"
                  "cmd.set_vertex_buffer(reinterpret_cast<device ushort *>(first & ~3ul),"
               << SM50_BINDING_INDEX_VERTEX_INDICES << ");\n"
                  "cmd.set_vertex_buffer(control,"
               << SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL << ");\n"
                  "cmd.draw_primitives((primitive_type)command_data.primitive_type, 0, "
                  "arg.draw_indexed.index_count_per_instance, arg.draw_indexed.instance_count, "
                  "arg.draw_indexed.start_instance_location);\n"
                  "} else {\n"
                  "control->index_size = 0;\n"
                  "cmd.set_vertex_buffer(control,"
               << SM50_BINDING_INDEX_VERTEX_INDICES << ");\n"
                  "cmd.set_vertex_buffer(control,"
               << SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL << ");\n"
                  "}\n";
        source << "}\n";
        source << "if (command_data.index_pull_mode == ~0u || "
                  "command_data.index_pull_mode == (ib32bit ? 2u : 1u)) {\n";
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
        source << "}\n";
        source << "}\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
        source << "cmd.concurrent_dispatch_threadgroups(arg.dispatch, command_data.tgsize);\n";
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
        auto slot = arg.VertexBuffer.Slot;
        graphics_updates << "vertex_buffer[" << slot << "] = {arg.vb_" << i << ".buffer,arg.vb_" << i
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
          (is_compute ? source : graphics_updates)
              << "reinterpret_cast<device uint *>(rootsig_qwords + " << offset << ")["
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
        (is_compute ? source : graphics_updates) << "rootsig_qwords[" << offset << "] = arg.cb_" << i << ";\n";
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
        (is_compute ? source : graphics_updates) << "rootsig_qwords[" << offset << "] = arg.srv_" << i << ";\n";
        /* TILED: the GPU cannot map an address to its reserved buffer, so an indirect root view
           reports fully mapped (its status qword is reset instead of keeping an older base). */
        if (rootsig->StatusQwordOffsets[parameter_index] != ~0u)
          (is_compute ? source : graphics_updates)
              << "rootsig_qwords[" << rootsig->StatusQwordOffsets[parameter_index] << "] = 0;\n";
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
        (is_compute ? source : graphics_updates) << "rootsig_qwords[" << offset << "] = arg.uav_" << i << ";\n";
        /* TILED: the GPU cannot map an address to its reserved buffer, so an indirect root view
           reports fully mapped (its status qword is reset instead of keeping an older base). */
        if (rootsig->StatusQwordOffsets[parameter_index] != ~0u)
          (is_compute ? source : graphics_updates)
              << "rootsig_qwords[" << rootsig->StatusQwordOffsets[parameter_index] << "] = 0;\n";
        break;
      }
      default:
        return E_INVALIDARG;
      }
    }

    source << "};\n";
    if (updates_graphics_bindings) {
      source << "void update_graphics_bindings(device d3d12_arguments &arg, device ulong *rootsig_qwords, "
                "device dxmt_vertex_buffer *vertex_buffer) {\n"
             << graphics_updates.view() << "}\n";
      // MSAA execution consumes these bindings in a compute VS, without an ICB draw.
      source << "kernel void resolve_graphics_arguments(uint i [[thread_position_in_grid]], "
                "constant dxmt_render_command_data &command_data [[buffer(30)]]) {\n"
                "if (i >= command_data.max_count) return;\n"
                "device d3d12_arguments &arg = *reinterpret_cast<device d3d12_arguments *>("
                "command_data.argument_buffer + ulong(i) * " << pDesc->ByteStride << ");\n"
                "device ulong *roots = command_data.rootsig_qwords ? command_data.rootsig_qwords + "
                "ulong(i) * command_data.rootsig_qwords_stride : nullptr;\n"
                "device dxmt_vertex_buffer *vertices = reinterpret_cast<device dxmt_vertex_buffer *>("
                "command_data.vertex_buffer + ulong(i) * command_data.vertex_argbuf_stride);\n"
                "update_graphics_bindings(arg, roots, vertices);\n}\n";
    }

    WMT::Reference<WMT::Error> err;
    auto lib = device_->GetMTLDevice().newLibraryWithSource(source.view(), err);

    if (!lib) {
      ERR("Failed to compile command signature resolve shader: ", err.description().getUTF8String());
      return E_FAIL;
    }

    auto function = WMT4::Library(lib).newFunction("resolve_indirect_commands");

    if (!function) {
      ERR("Failed to create command signature resolve shader");
      return E_FAIL;
    }

    if (is_compute) {
      compute_resolver = device_->GetMetal4Compiler().newComputePipelineState(function, err);
    } else {
      WMTRenderPipelineInfo info;
      WMT::InitializeRenderPipelineInfo(info);
      info.rasterization_enabled = false;
      info.vertex_function = function;
      render_resolver = device_->GetMetal4Compiler().newRenderPipelineState(info, err);
    }

    if (err) {
      ERR("Failed to compile command signature resolve pso: ", err.description().getUTF8String());
      return E_FAIL;
    }
    if (updates_graphics_bindings) {
      auto bindings = WMT4::Library(lib).newFunction("resolve_graphics_arguments");
      if (!bindings) {
        ERR("Failed to create graphics argument resolver shader");
        return E_FAIL;
      }
      graphics_argument_resolver = device_->GetMetal4Compiler().newComputePipelineState(bindings, err);
      if (!graphics_argument_resolver) {
        ERR("Failed to compile graphics argument resolver pso: ", err.description().getUTF8String());
        return E_FAIL;
      }
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