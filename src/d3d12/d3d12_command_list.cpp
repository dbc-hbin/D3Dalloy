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

#include "d3d12_command_allocator.hpp"
#include "d3d12_enhanced_barrier.hpp"
#include "d3d12_acceleration_structure.hpp"
#include "d3d12_resource_contract.hpp"
#include "d3d12_format_support.hpp"
#include "d3d12_state_object.hpp"
#include "d3d12_work_graph.hpp"
#include "d3d12_matrix.hpp"
#include "d3d12_temporal.hpp"
#include "d3d12_frame_generation.hpp"
#include "d3d12_ray_regeneration.hpp"
#include "com/com_pointer.hpp"
#include "dxil_raytracing.h"
#include "d3dalloy_buffer.hpp"
#include "d3dalloy_format.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace d3dalloy {

bool NativeMSAADescriptorSafe(MTLD3D12Device *device, const ShaderVisibleDescriptorCPUStorage &descriptor) {
  if (descriptor.type == ShaderVisibleDescriptorType::Null) return true;
  if (descriptor.type == ShaderVisibleDescriptorType::ConstantBuffer) {
    uint64_t offset = 0;
    auto *a = device->LookupBufferByVA(descriptor.ConstantBuffer.address, &offset);
    return a && !a->canonicalHeap && !a->reservedResource &&
        !a->flags().test(BufferAllocationFlag::PlacementSparse) &&
        resource_contract::RangeFits(offset, descriptor.ConstantBuffer.size, a->byteLength());
  }
  if (descriptor.type == ShaderVisibleDescriptorType::SRVTexture) {
    auto *texture = descriptor.SRVTexture.texture;
    auto owner = device->LookupDescriptorResource(texture, descriptor.owner_id);
    if (!owner || !texture || !texture->current() || texture->sampleCount() != 1 ||
        texture->reservedResource || texture->reservedVolumeLayoutAddress ||
        device->LookupStandardSwizzleTexture(texture)) return false;
    const auto view = texture->viewDescriptor(descriptor.SRVTexture.view);
    const auto *a = texture->current();
    return !view.castBlockSize && !view.castCompressed && !view.msaaShadow && !view.reservedVolumeRender &&
        !a->canonicalHeap && !a->flags().test(TextureAllocationFlag::Shared) &&
        !a->flags().test(TextureAllocationFlag::PlacementSparse) &&
        !a->flags().test(TextureAllocationFlag::GpuManaged);
  }
  const bool texel = descriptor.type == ShaderVisibleDescriptorType::SRVTexelBuffer;
  if (!texel && descriptor.type != ShaderVisibleDescriptorType::SRVBuffer &&
      descriptor.type != ShaderVisibleDescriptorType::SRVRGB32Buffer) return false;
  auto *buffer = texel ? descriptor.SRVTexelBuffer.buffer : descriptor.SRVBuffer.buffer;
  auto owner = device->LookupDescriptorResource(buffer, descriptor.owner_id);
  if (!owner || !buffer || !buffer->current()) return false;
  const auto *a = buffer->current();
  const auto slice = texel ? descriptor.SRVTexelBuffer.slice : descriptor.SRVBuffer.slice;
  return !a->canonicalHeap && !a->reservedResource && !a->flags().test(BufferAllocationFlag::PlacementSparse) &&
      resource_contract::RangeFits(slice.byteOffset, slice.byteLength, a->byteLength());
}

/* Resolve an application GPU address backing one of the acceleration-structure operands.
 * D3D12 GPU virtual addresses are Metal device addresses here, so the resolved offset is
 * usable directly by the prepass kernel. */
bool
ResolveRaytracingGPUVA(
    void *context, D3D12_GPU_VIRTUAL_ADDRESS address, uint64_t requiredBytes, raytracing::BufferLocation &location
) {
  auto *device = static_cast<MTLD3D12Device *>(context);
  uint64_t offset = 0;
  auto *allocation = device->LookupBufferByVA(address, &offset);
  if (!allocation || offset > allocation->byteLength() || requiredBytes > allocation->byteLength() - offset)
    return false;
  location = {allocation->buffer(), offset, allocation->byteLength() - offset, allocation, address};
  return true;
}

namespace {

D3D12_RECT
ClipUAVClearRect(const D3D12_RECT &rect, const D3D12_RECT &bounds) {
  return {
      std::clamp(rect.left, LONG(0), bounds.right), std::clamp(rect.top, LONG(0), bounds.bottom),
      std::clamp(rect.right, LONG(0), bounds.right), std::clamp(rect.bottom, LONG(0), bounds.bottom)
  };
}

using resource_contract::RangeFits;

bool
GetSubresourceInfo(
    WMT::Device device, const D3D12_RESOURCE_DESC &desc, UINT subresource, UINT &level, UINT &slice, UINT &plane
) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || !desc.MipLevels)
    return false;
  MTL_DXGI_FORMAT_DESC format;
  if (FAILED(MTLQueryDXGIFormat(device, desc.Format, format)) || !format.PlanarCount)
    return false;
  const UINT array_size = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
  const UINT64 subresource_count = static_cast<UINT64>(desc.MipLevels) * array_size * format.PlanarCount;
  if (subresource >= subresource_count)
    return false;
  DecomposeSubresource(desc, subresource, &level, &slice, &plane);
  return plane < format.PlanarCount;
}

struct PlacedFootprintInfo {
  UINT block_width = 1;
  UINT block_height = 1;
  UINT bytes_per_block = 0;
  UINT64 row_size = 0;
  UINT64 row_count = 0;
  UINT64 slice_pitch = 0;
};

bool
ValidatePlacedFootprint(
    WMT::Device device, const D3D12_RESOURCE_DESC &buffer_desc,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &placed, PlacedFootprintInfo &info
) {
  if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
    return false;
  const auto &footprint = placed.Footprint;

  MTL_DXGI_FORMAT_DESC format;
  if (FAILED(MTLQueryDXGIFormat(device, footprint.Format, format)) || !format.BytesPerTexel)
    return false;
  const auto packed = resource_contract::GetEmulatedTextureStorage(footprint.Format);
  info.block_width = format.Flag & MTL_DXGI_FORMAT_BC ? 4u :
      (packed == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
       packed == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8) ? 2u : 1u;
  info.block_height = format.Flag & MTL_DXGI_FORMAT_BC ? 4u : 1u;
  info.bytes_per_block = format.BytesPerTexel;
  resource_contract::FootprintLayout layout;
  // OPTIONS13 permits texel/block-aligned copies without the portable 512/256
  // alignments. Keep portable footprints valid as well (notably RGB32, whose
  // 12-byte unit does not divide the pitch GetCopyableFootprints returns).
  const UINT placement_alignment = placed.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
      ? info.bytes_per_block : 1u;
  const UINT pitch_alignment = footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
      ? info.bytes_per_block : 1u;
  if (!resource_contract::ValidateFootprintRange(
          footprint.Width, footprint.Height, footprint.Depth, info.bytes_per_block, info.block_width, info.block_height,
          footprint.RowPitch, placed.Offset, buffer_desc.Width, placement_alignment, pitch_alignment, layout
      ))
    return false;
  info.row_size = layout.row_size;
  info.row_count = layout.row_count;
  info.slice_pitch = layout.slice_pitch;
  return true;
}

bool
BoxFits(const D3D12_BOX &box, const D3D12_BOX &extent) {
  return box.left < box.right && box.top < box.bottom && box.front < box.back &&
         box.right <= extent.right && box.bottom <= extent.bottom && box.back <= extent.back;
}

bool
DestinationFits(UINT x, UINT y, UINT z, const D3D12_BOX &copy_box, const D3D12_BOX &extent) {
  const UINT width = copy_box.right - copy_box.left;
  const UINT height = copy_box.bottom - copy_box.top;
  const UINT depth = copy_box.back - copy_box.front;
  return x <= extent.right && width <= extent.right - x &&
         y <= extent.bottom && height <= extent.bottom - y &&
         z <= extent.back && depth <= extent.back - z;
}

/* A D3D12 copy with no source box covers the region the source and destination
 * locations share. A placed footprint is described by the pitch-padded extent
 * GetCopyableFootprints reports, so a block-compressed tail mip has a footprint
 * extent (4x4) wider than the mip it holds (2x2); taking that padded extent
 * verbatim as the copy size rejects a valid tail copy and asks Metal for more
 * texels than the destination level holds. The shared region is the smaller
 * extent measured from the destination origin, and a destination origin at or
 * past its extent leaves nothing to copy. */
D3D12_BOX
OverlapBox(const D3D12_BOX &src_extent, const D3D12_BOX &dst_extent, UINT dst_x, UINT dst_y, UINT dst_z) {
  D3D12_BOX box{0, 0, 0, 0, 0, 0};
  box.right = dst_x < dst_extent.right ? std::min(src_extent.right, dst_extent.right - dst_x) : 0;
  box.bottom = dst_y < dst_extent.bottom ? std::min(src_extent.bottom, dst_extent.bottom - dst_y) : 0;
  box.back = dst_z < dst_extent.back ? std::min(src_extent.back, dst_extent.back - dst_z) : 0;
  return box;
}

bool
BlockCopyAligned(
    UINT block_width, UINT block_height, const D3D12_BOX &box, const D3D12_BOX &src_extent,
    UINT dst_x, UINT dst_y, const D3D12_BOX &dst_extent
) {
  if (block_width == 1 && block_height == 1)
    return true;
  if ((box.left % block_width) || (box.top % block_height) ||
      (dst_x % block_width) || (dst_y % block_height))
    return false;
  if ((box.right % block_width) && box.right != src_extent.right)
    return false;
  if ((box.bottom % block_height) && box.bottom != src_extent.bottom)
    return false;
  const UINT width = box.right - box.left;
  const UINT height = box.bottom - box.top;
  if (((dst_x + width) % block_width) && dst_x + width != dst_extent.right)
    return false;
  if (((dst_y + height) % block_height) && dst_y + height != dst_extent.bottom)
    return false;
  return true;
}

} // namespace

enum class DirtyState {
  VertexBuffer,
  GraphicsRootArguments,
  GraphicsRootSignature,
  Viewport,
  ScissorRect,
  ComputeRootArguments,
  ComputeRootSignature,
  BlendFactor,
  StencilRef,
  GraphicsPipelineState,
  ComputePipelineState,
};

enum class DrawCallStatus {
  Invalid,
  Ordinary,
};

// Public DirectX-Headers topology value; the bundled Wine enum predates OPTIONS15.
constexpr D3D12_PRIMITIVE_TOPOLOGY kTriangleFan = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(6);

inline bool
to_metal_primitive_type(D3D12_PRIMITIVE_TOPOLOGY topo, WMTPrimitiveType &primitive, uint32_t &control_point_num) {
  control_point_num = 0;
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    primitive = WMTPrimitiveTypeLine;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    primitive = WMTPrimitiveTypeLineStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
  case kTriangleFan:
    primitive = WMTPrimitiveTypeTriangle;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    primitive = WMTPrimitiveTypeTriangleStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    // geometry
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    primitive = WMTPrimitiveTypePoint;
    control_point_num = topo - 32;
    break;
  default:
    return false;
  }
  return true;
}

// `Graphics`CommandList is a really confusing name
class MTLD3D12GraphicsCommandListImpl
    : public MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList, IMTLD3D12GraphicsCommandListExt, IMTLD3D12FrameGenerationCommandList,
                                 IMTLD3D12RayRegenerationCommandList> {

  Com<MTLD3D12CommandAllocatorImpl, false> allocator_;
  // Immutable device limit; draw-time capture sizing would otherwise make a native call per draw.
  const uint64_t max_buffer_length_ = device_->GetMTLDevice().maxBufferLength();

  struct ResolvePipelineKey {
    uint32_t attachment_format;
    uint32_t raster_sample_count;
    uint8_t fragment_kind;
    uint8_t dsv_plane;
    uint8_t depth_write_enabled;
    uint8_t stencil_enabled;

    bool operator==(const ResolvePipelineKey &other) const {
      return attachment_format == other.attachment_format &&
          raster_sample_count == other.raster_sample_count && fragment_kind == other.fragment_kind &&
          dsv_plane == other.dsv_plane && depth_write_enabled == other.depth_write_enabled &&
          stencil_enabled == other.stencil_enabled;
    }
  };

  struct ResolvePipelineCacheEntry {
    ResolvePipelineKey key{};
    WMT::Reference<WMT::RenderPipelineState> pipeline;
  };

  // Resolve pipelines are device-owned and remain valid across command-list Reset.
  std::array<ResolvePipelineCacheEntry, 8> resolve_pipeline_cache_{};
  size_t resolve_pipeline_cache_count_ = 0;
  size_t resolve_pipeline_cache_next_ = 0;
  const D3D12_COMMAND_LIST_TYPE type_;

  // Bundle calls are expanded through the parent's ordinary command recorder.
  // This resolves inherited state at each ExecuteBundle and stores the resulting
  // Metal commands in the parent's allocator, independently of bundle Reset.
  using BundleCommand = std::function<void(MTLD3D12GraphicsCommandListImpl &)>;
  std::vector<BundleCommand> bundle_commands_;
  ID3D12PipelineState *bundle_initial_pso_ = nullptr;
  uint64_t bundle_allocator_generation_ = 0;
  std::array<ID3D12DescriptorHeap *, 2> descriptor_heaps_ = {};
  bool bundle_heaps_set_ = false;
  bool bundle_pipeline_seen_ = false;
  std::array<DXGI_FORMAT, 8> bundle_rtv_formats_{};
  UINT bundle_rt_count_ = 0;
  DXGI_FORMAT bundle_dsv_format_ = DXGI_FORMAT_UNKNOWN;
  DXGI_SAMPLE_DESC bundle_samples_{};
  std::shared_ptr<owned_graphics::BundleRecordingState> bundle_recording_ =
      std::make_shared<owned_graphics::BundleRecordingState>();
  HRESULT recording_error_ = S_OK;
  HRESULT close_error_ = S_OK;
  WMTProgrammableSamplePositions sample_positions_{};

  bool IsRecording() const {
    return allocator_ && encoder_count == std::numeric_limits<size_t>::max();
  }

  bool ObserveBundlePipeline(ID3D12PipelineState *pipeline) {
    if (!pipeline || type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return true;
    Com<ID3D12Device> pipeline_device;
    if (FAILED(pipeline->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&pipeline_device))) ||
        pipeline_device.ptr() != static_cast<ID3D12Device *>(device_)) {
      RecordError();
      return false;
    }
    if (static_cast<MTLD3D12PipelineState *>(pipeline)->IsComputePipelineState)
      return true;
    const auto *graphics = static_cast<MTLD3D12GraphicsPipelineState *>(pipeline);
    if (!bundle_pipeline_seen_) {
      bundle_rtv_formats_ = graphics->owned_rtv_formats;
      bundle_rt_count_ = graphics->owned_rt_count;
      bundle_dsv_format_ = graphics->owned_dsv_format;
      bundle_samples_ = graphics->owned_sample_desc;
      bundle_pipeline_seen_ = true;
      return true;
    }
    if (bundle_rtv_formats_ != graphics->owned_rtv_formats || bundle_rt_count_ != graphics->owned_rt_count ||
        bundle_dsv_format_ != graphics->owned_dsv_format || bundle_samples_.Count != graphics->owned_sample_desc.Count ||
        bundle_samples_.Quality != graphics->owned_sample_desc.Quality) {
      RecordError();
      return false;
    }
    return true;
  }

  void RecordError(HRESULT hr = E_INVALIDARG) {
    if (SUCCEEDED(recording_error_)) {
      recording_error_ = hr;
      device_->diagnostics.Validation(D3D12_MESSAGE_ID_UNKNOWN,
          "Command list recording failed validation; Close reports the first failure");
    }
  }

  template <typename F>
  void AppendBundleCommand(F &&command) {
    if (!IsRecording()) {
      RecordError(E_FAIL);
      return;
    }
    if (FAILED(recording_error_))
      return;
    try {
      bundle_commands_.emplace_back(std::forward<F>(command));
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
    }
  }

  template <typename Method, typename... Args>
  bool RecordBundle(Method method, Args... args) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return false;
    AppendBundleCommand([method, args...](auto &parent) { (parent.*method)(args...); });
    return true;
  }

  bool RejectBundleOperation() {
    // This guard also protects the allocator arena from commands issued after
    // Close; the closed recording may already be executing on another thread.
    if (!IsRecording())
      return true;
    if (type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return false;
    RecordError();
    return true;
  }

  bool RecordBundleConstants(bool graphics, UINT index, UINT count, const void *data, UINT offset) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return false;
    if (count > 64 || offset > 64 - count || (count && !data)) {
      RecordError();
      return true;
    }
    if (!count)
      return true;
    try {
      const auto *words = static_cast<const UINT *>(data);
      std::vector<UINT> values(words, words + count);
      AppendBundleCommand([graphics, index, offset, values = std::move(values)](auto &parent) {
        if (graphics)
          parent.SetGraphicsRoot32BitConstants(index, values.size(), values.data(), offset);
        else
          parent.SetComputeRoot32BitConstants(index, values.size(), values.data(), offset);
      });
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
    }
    return true;
  }

  /* state */

  Flags<DirtyState> dirty_state_;

  std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_;

  uint64_t index_buffer_address;
  WMT::Buffer index_buffer;
  WMTIndexType index_type;
  uint64_t index_offset;

  UINT num_rtvs;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  /* Epoch in which the bound attachments were last recorded into the device's RenderTargetHistory
   * (0: not recorded). RTV/DSV descriptors are consumed at OMSetRenderTargets, so only
   * OMSetRenderTargets, ResetState and Close invalidate it. */
  uint64_t async_targets_epoch_ = 0;

  D3D12_PRIMITIVE_TOPOLOGY topology_;

  UINT num_viewports;
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  UINT num_scissors;
  D3D12_RECT
  scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  Com<MTLD3D12GraphicsPipelineState, false> pso_graphics_;
  Com<MTLD3D12RootSignature, false> rootsig_graphics_;
  uint64_t rootarg_graphics_staging_[MTLD3D12RootSignature::kMaxRootArgumentQwords];

  Com<MTLD3D12ComputePipelineState, false> pso_compute_;
  Com<MTLD3D12RootSignature, false> rootsig_compute_;
  uint64_t rootarg_compute_staging_[MTLD3D12RootSignature::kMaxRootArgumentQwords];

  WMT::Reference<WMT::ComputePipelineState> predication_snapshot_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> predication_arguments_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> mesh_indirect_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> mesh_strip_cut_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> tess_capture_clear_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> tess_capture_compact_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> predication_copy_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> predication_pitched_copy_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> d24_upload_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> d24_readback_pipeline_;
  std::array<WMT::Reference<WMT::ComputePipelineState>, 4> format_conversion_pipelines_;
  WMT::Reference<WMT::ComputePipelineState> atomic_copy_u32_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> atomic_copy_u64_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> matrix_conversion_pipeline_;
  /* Sampler feedback transcoding: [0] decode, [1] encode (d3dalloy_command.metal). */
  std::array<WMT::Reference<WMT::ComputePipelineState>, 2> feedback_transcode_pipelines_;
  uint64_t predication_snapshot_offset_ = 0;
  D3D12_PREDICATION_OP predication_op_ = D3D12_PREDICATION_OP_EQUAL_ZERO;
  bool predication_enabled_ = false;

  /*
   * View instancing (Tier 1, draw-level looping). `view_instance_mask_` is SetViewInstanceMask state (default 0).
   * While a draw is replayed for one view, `view_replay_` is set and `view_id_` / `view_location_` hold that view;
   * outside a replay they are 0 / {0, 0}, so the root view-id qword reads 0 (SV_ViewID of a PSO without view
   * instancing).
   */
  UINT view_instance_mask_ = 0;
  /* Variable-rate shading state: the base rate, bound image, and {per-primitive, image} combiners. */
  D3D12_SHADING_RATE shading_rate_ = D3D12_SHADING_RATE_1X1;
  Com<MTLD3D12Resource> shading_rate_image_;
  std::array<D3D12_SHADING_RATE_COMBINER, 2> shading_rate_combiners_{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                                    D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
  bool view_replay_ = false;
  UINT view_id_ = 0;
  D3D12_VIEW_INSTANCE_LOCATION view_location_{};

  /* IRaytracing dispatch state, set by SetPipelineState1. */
  Com<MTLD3D12StateObject> rt_state_object_;
  Com<MTLD3D12WorkGraph> work_graph_;
  uint32_t work_graph_index_ = 0;
  D3D12_SET_WORK_GRAPH_DESC work_graph_program_{};
  uint32_t work_graph_max_records_ = UINT32_MAX, work_graph_max_node_inputs_ = UINT32_MAX;
  WMT::Reference<WMT::ComputePipelineState> rt_pipeline_;
  WMT::Reference<WMT::IntersectionFunctionTable> rt_intersection_function_table_;
  /* The compiled traversal reaches miss/closest-hit/any-hit/callable exports
   * through this table, so a ray dispatch binds it alongside the intersection
   * function table. */
  WMT::Reference<WMT::VisibleFunctionTable> rt_visible_function_table_;
  /* ExecuteIndirect DISPATCH_RAYS resolve kernel (raytracing::kRaysIndirectMSL), compiled on first use. */
  WMT::Reference<WMT::ComputePipelineState> rays_indirect_pipeline_;

  FLOAT blend_factor_[4];
  UINT8 stencil_ref_;
  // OMSetFrontAndBackStencilRef; equal to stencil_ref_ after OMSetStencilRef.
  UINT8 back_stencil_ref_;
  float depth_bounds_min_ = 0, depth_bounds_max_ = 1;
  // BeginRenderPass/EndRenderPass state (reset per recording).
  struct RenderPassResolve {
    ID3D12Resource *src, *dst;
    DXGI_FORMAT format;
    D3D12_RESOLVE_MODE mode;
    D3D12_RESOURCE_STATES source_state;
    std::vector<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS> regions;
    bool preserve_source;
  };
  bool render_pass_active_ = false;
  UINT render_pass_flags_ = 0;
  std::array<bool, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> render_pass_discard_colors_{};
  uint32_t render_pass_discard_color_count_ = 0;
  bool render_pass_discard_depth_ = false, render_pass_discard_stencil_ = false;
  // The last pass ended surfaces in PRESERVE_LOCAL_*: only pass setup may come before the next pass.
  bool render_pass_ends_local_ = false, render_pass_pending_local_ = false;
  bool render_pass_local_compute_ = false;
  // The last pass of the list suspended; nothing may follow it before Close.
  bool render_pass_suspended_ = false;
  std::vector<RenderPassResolve> render_pass_resolves_;
  // Begin/EndRenderPass issue their own binds, clears and resolves through the public entry points.
  bool render_pass_internal_ = false;
  D3D12_RESOURCE_STATES render_pass_resolve_source_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  // An ending-access resolve whose source content is not needed afterwards.
  bool render_pass_resolve_discards_source_ = false;
  // Textures the active pass binds for raster (non-conformant barrier detection).
  small_vector<Texture *, 9> render_pass_surfaces_;
  // RSSetDepthBias / IASetIndexBufferStripCutValue, reset from the PSO by SetPipelineState.
  float depth_bias_ = 0, depth_bias_clamp_ = 0, slope_scaled_depth_bias_ = 0;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE ib_strip_cut_value_ = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  // MeshVariantKey of the object/mesh pipeline last bound by PreDraw (GS/HS/DS PSOs only).
  uint32_t mesh_variant_bound_ = ~0u;
  // The claim buffer PrepareStatisticsClaim made for the draw being recorded (empty: none).
  struct {
    WMT::Buffer buffer;
    uint64_t offset = 0;
  } statistics_claim_;
  struct ActiveVisibilityQuery {
    D3D12_QUERY_TYPE type;
    UINT index;
    uint64_t heap_id;
    WMT::Reference<WMT::Buffer> destination;
    WMT::Reference<WMT::ComputePipelineState> accumulate_pipeline;
  };
  small_vector<ActiveVisibilityQuery, 4> active_visibility_queries_;

  struct ActiveSOStatisticsQuery {
    D3D12_QUERY_TYPE type;
    UINT index;
    uint64_t heap_id;
  };
  small_vector<ActiveSOStatisticsQuery, 4> active_so_queries_;
  small_vector<ActiveSOStatisticsQuery, 4> active_pipeline_statistics_;

  /*
   * SOSetTargets state. A bound slot retains its target and filled-size allocations while
   * it stays bound; a draw that consumes them must also retain them on its encoder until
   * GPU retirement. `offset` values are byte offsets inside the allocation's buffer.
   */
  struct StreamOutputTarget {
    Rc<BufferAllocation> buffer;
    uint64_t offset = 0;
    uint64_t size = 0;
    Rc<BufferAllocation> filled_size;
    uint64_t filled_size_offset = 0;
  };
  std::array<StreamOutputTarget, D3D12_SO_BUFFER_SLOT_COUNT> so_targets_;

  WMT::Reference<WMT::ComputePipelineState> so_assemble_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> so_assemble_gs_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> so_positions_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> so_indirect_preflight_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> msaa_assembly_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> msaa_bounds_pipeline_;
  std::shared_ptr<StreamOutputDrawData> so_pending_;
  /* The stream-output draw whose capture was just bound (indexed draws draw over its positions). */
  std::shared_ptr<StreamOutputDrawData> so_bound_;

  /* Per-recording stream counters; see SOStreamCounterBuffer. */
  bool so_counters_allocated_ = false;
  uint64_t so_counters_offset_ = 0;
  bool pipeline_statistics_counters_allocated_ = false;
  uint64_t pipeline_statistics_counters_offset_ = 0;

  /* A recording has one GPU-ordered D3D12_QUERY_DATA_PIPELINE_STATISTICS1 total (the PIPELINE_STATISTICS layout
   * plus AS/MS invocations and MS primitives) shared by all overlapping queries of either type. */
  std::tuple<WMT::Buffer, uint64_t>
  PipelineStatisticsCounterBuffer() {
    if (!pipeline_statistics_counters_allocated_) {
      constexpr size_t bytes = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
      // The capture assembler also binds this as its unused ulong2 SO counter buffer.
      if (!allocator_->ReserveGPUHeap(bytes, 16)) {
        RecordError(E_OUTOFMEMORY);
        return {WMT::Buffer{}, 0};
      }
      auto [memory, offset] = allocator_->AllocateGPUHeap(bytes, 16);
      memset(memory, 0, bytes);
      pipeline_statistics_counters_offset_ = offset;
      pipeline_statistics_counters_allocated_ = true;
      allocator_->InvalidateCurrentPass();
      auto reset = allocator_->AllocatePass<QueryEncoderData>();
      reset->type = EncoderType::Query;
      reset->op = QueryEncoderOp::ResetPipelineStatistics;
      allocator_->BindGPUHeap(reset->counters, reset->counters_offset, offset);
      reset->record_bytes = bytes;
      allocator_->InvalidateCurrentPass();
    }
    const auto allocation = allocator_->ResolveGPUHeap(pipeline_statistics_counters_offset_);
    return {allocation.buffer, allocation.offset};
  }

  /*
   * Internal API for the SO capture/assembly pass.
   *
   * Returns the allocator GPU-heap buffer and byte offset of this recording's stream
   * counters: D3D12_SO_STREAM_COUNT consecutive D3D12_QUERY_DATA_SO_STATISTICS records
   * ({uint64 NumPrimitivesWritten, uint64 PrimitivesStorageNeeded}), stream s at
   * offset + s * 16. They start at zero when first used by a recording and only grow in
   * GPU order. An SO_STATISTICS_STREAMn query snapshots stream n at BeginQuery and
   * writes (counter - snapshot) at EndQuery, so the assembly pass only has to add each
   * draw's (written, needed) primitive counts to its stream's record on the GPU (a GS assembles each captured
   * stream into its own record), ordered between
   * the query passes the same way this recording orders them. Re-executing a closed list
   * keeps the counters' previous GPU values, which the Begin/End difference absorbs.
   *
   * Stream-output draws (PrepareStreamOutputDraw / so_assemble) add each draw's
   * (written, needed) to stream 0 on the GPU after the draw's render pass.
   *
   * Returns {null, 0} and records E_OUTOFMEMORY when the GPU heap is exhausted.
   */
  std::tuple<WMT::Buffer, uint64_t>
  SOStreamCounterBuffer() {
    if (!so_counters_allocated_) {
      constexpr size_t bytes = D3D12_SO_STREAM_COUNT * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
      if (!allocator_->ReserveGPUHeap(bytes, 16)) {
        RecordError(E_OUTOFMEMORY);
        return {WMT::Buffer{}, 0};
      }
      auto [memory, offset] = allocator_->AllocateGPUHeap(bytes, 16);
      memset(memory, 0, bytes);
      so_counters_offset_ = offset;
      so_counters_allocated_ = true;
    }
    const auto allocation = allocator_->ResolveGPUHeap(so_counters_offset_);
    return {allocation.buffer, allocation.offset};
  }

  bool QueryObjectOwnedByThisDevice(ID3D12DeviceChild *object) {
    if (!object)
      return false;
    Com<ID3D12Device> owner;
    return SUCCEEDED(object->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) &&
           owner.ptr() == static_cast<ID3D12Device *>(device_);
  }

  bool ValidateRootParameter(
      MTLD3D12RootSignature *root, UINT index, D3D12_ROOT_PARAMETER_TYPE type,
      UINT constant_count = 0, UINT constant_offset = 0
  ) {
    if (!IsRecording() || !root || index >= root->ParameterSlots || root->ParameterTypes[index] != type ||
        (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
         (constant_offset > root->ParameterConstantCounts[index] ||
          constant_count > root->ParameterConstantCounts[index] - constant_offset))) {
      RecordError();
      return false;
    }
    return true;
  }

  bool ValidateAtomicCopy(
      ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT64 element_size,
      UINT dependencies, ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_ranges
  ) {
    const auto valid_primary = [&](ID3D12Resource *resource, UINT64 offset) {
      if (!QueryObjectOwnedByThisDevice(resource))
        return false;
      const auto desc = resource->GetDesc();
      return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && !(offset % element_size) &&
             RangeFits(offset, element_size, desc.Width) && static_cast<MTLD3D12Resource *>(resource)->buffer;
    };
    if (!valid_primary(dst, dst_offset) || !valid_primary(src, src_offset) ||
        (dependencies && (!dependent_resources || !dependent_ranges))) {
      RecordError();
      return false;
    }
    for (UINT i = 0; i < dependencies; i++) {
      auto *resource = dependent_resources[i];
      const auto &range = dependent_ranges[i];
      if (!QueryObjectOwnedByThisDevice(resource) || range.Range.Begin > range.Range.End) {
        RecordError();
        return false;
      }
      const auto desc = resource->GetDesc();
      UINT level, slice, plane;
      const bool valid_subresource = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                         ? range.Subresource == 0 && range.Range.End <= desc.Width
                                         : GetSubresourceInfo(
                                               device_->GetMTLDevice(), desc, range.Subresource, level, slice, plane
                                           );
      if (!valid_subresource) {
        RecordError();
        return false;
      }
    }
    return true;
  }

  bool EnsurePredicationPipelines() {
    if (predication_snapshot_pipeline_ && predication_arguments_pipeline_ && predication_copy_pipeline_ &&
        predication_pitched_copy_pipeline_ && mesh_indirect_pipeline_ && mesh_strip_cut_pipeline_ && tess_capture_clear_pipeline_ &&
        tess_capture_compact_pipeline_)
      return true;
    constexpr auto source = R"(
#include <metal_stdlib>
using namespace metal;
kernel void d3dalloy_snapshot_predicate(device const ulong *source [[buffer(0)]],
                                    device ulong *destination [[buffer(1)]]) {
  destination[0] = source[0];
}
kernel void d3dalloy_predicate_arguments(device const ulong *predicate [[buffer(0)]],
                                     device const uint *control [[buffer(1)]],
                                     device uint *arguments [[buffer(2)]]) {
  const bool execute = control[0] ? predicate[0] == 0 : predicate[0] != 0;
  for (uint i = 0; i < control[1]; i++)
    arguments[i] = execute ? control[i + 2] : 0;
}
kernel void d3dalloy_predicate_copy_bytes(device const ulong *predicate [[buffer(0)]],
                                      device const ulong *control [[buffer(1)]],
                                      device const uchar *source [[buffer(2)]],
                                      device uchar *destination [[buffer(3)]],
                                      uint index [[thread_position_in_grid]]) {
  const bool execute = control[0] ? predicate[0] == 0 : predicate[0] != 0;
  if (execute && index < control[1])
    destination[index] = source[index];
}
/* Rows x slices of one predicated texture-footprint copy: control = {op, row bytes, source row/slice pitch,
 * destination row/slice pitch}. Bytes between rows (pitch padding) are never written. */
kernel void d3dalloy_predicate_copy_rows(device const ulong *predicate [[buffer(0)]],
                                     device const ulong *control [[buffer(1)]],
                                     device const uchar *source [[buffer(2)]],
                                     device uchar *destination [[buffer(3)]],
                                     uint3 index [[thread_position_in_grid]]) {
  const bool execute = control[0] ? predicate[0] == 0 : predicate[0] != 0;
  if (execute && index.x < control[1])
    destination[ulong(index.z) * control[5] + ulong(index.y) * control[4] + index.x] =
        source[ulong(index.z) * control[3] + ulong(index.y) * control[2] + index.x];
}
/* GS/HS/DS ExecuteIndirect: one thread per command slot copies the command's draw arguments (so the object function
 * reads them at SM50_BINDING_INDEX_DRAW_ARGUMENTS) and writes its object-threadgroup grid. Slots beyond the count
 * buffer, a false predicate, an empty draw or a grid above the device limit dispatch (0, 0, 0). */
struct D3DalloyMeshIndirectControl {
  ulong arguments;       // first command's D3D12_DRAW_(INDEXED_)ARGUMENTS
  ulong count;           // count buffer address or 0
  ulong predicate;       // predicate snapshot address or 0
  ulong max_threadgroups;
  uint stride;
  uint max_commands;
  uint argument_words;   // 4 (draw) or 5 (draw indexed)
  uint predicate_not_equal;
  uint per_group;        // vertices (GS) or patches (TS) advanced per object threadgroup; 0: DISPATCH_MESH grid
  uint control_points;   // TS: vertices per patch, GS: 0
};
kernel void d3dalloy_mesh_indirect_arguments(constant D3DalloyMeshIndirectControl &control [[buffer(0)]],
                                         device uint *arguments_out [[buffer(1)]],
                                         device uint3 *dispatch_out [[buffer(2)]],
                                         uint index [[thread_position_in_grid]]) {
  if (index >= control.max_commands)
    return;
  device const uint *source = (device const uint *)(control.arguments + ulong(index) * control.stride);
  device uint *arguments = arguments_out + index * 8;
  uint limit = control.count ? *(device const uint *)control.count : control.max_commands;
  bool execute = index < limit;
  if (control.predicate) {
    ulong value = *(device const ulong *)control.predicate;
    execute = execute && (control.predicate_not_equal ? value == 0 : value != 0);
  }
  for (uint i = 0; i < control.argument_words; i++)
    arguments[i] = execute ? source[i] : 0;
  if (!control.per_group) {
    /* D3D12_DISPATCH_MESH_ARGUMENTS: the grid itself. Each dimension must be < 64k and the product <= 2^22. */
    uint3 grid = uint3(arguments[0], arguments[1], arguments[2]);
    bool valid = all(grid != 0) && all(grid < 65536) && ulong(grid.x) * grid.y * grid.z <= (1ul << 22);
    dispatch_out[index] = valid ? grid : uint3(0);
    return;
  }
  uint count = arguments[0], instances = arguments[1];
  if (control.control_points)
    count /= control.control_points;
  ulong groups = count ? (count - 1) / control.per_group + 1 : 0;
  if (!count || !instances || groups * instances > control.max_threadgroups)
    dispatch_out[index] = uint3(0);
  else
    dispatch_out[index] = uint3(uint(groups), instances, 1);
}
/* GS strip draws the mesh GS cannot assemble natively: an indexed strip with an active IBStripCutValue (every strip
 * restarts winding parity and continues SV_PrimitiveID from the previous strip) and any triangle strip with
 * adjacency (its end-of-strip adjacency rules are position dependent). One thread rewrites the draw as the equivalent
 * list (D3D11.3 8.10 primitive order, winding and adjacency) of 32-bit index values, writes
 * its D3D12_DRAW_INDEXED_ARGUMENTS and object grid, and moves IAVertices from list to strip accounting. */
struct D3DalloyMeshStripCutControl {
  ulong max_threadgroups;
  uint topology;  // D3D_PRIMITIVE_TOPOLOGY: 3 line strip, 5 triangle strip, 11 line strip adj, 13 triangle strip adj
  uint index_size;
  uint cut_value;
  uint count;
  uint instances;
  int base_vertex;
  uint start_instance;
  uint index_skip;
  uint predicate_enabled;
  uint predicate_not_equal;
  uint statistics_enabled;
  uint per_group;
  uint cut_enabled;
};
// Index size 0: a non-indexed draw, whose IA position k is its own value.
inline uint d3dalloy_mesh_read_index(device const uchar *indices, uint size, uint k) {
  return size == 0 ? k : size == 4 ? uint(indices[4 * k]) | uint(indices[4 * k + 1]) << 8 | uint(indices[4 * k + 2]) << 16 |
                         uint(indices[4 * k + 3]) << 24
                   : uint(indices[2 * k]) | uint(indices[2 * k + 1]) << 8;
}
kernel void d3dalloy_mesh_strip_cut(constant D3DalloyMeshStripCutControl &control [[buffer(0)]],
                                device const uchar *indices [[buffer(1)]],
                                device uint *list [[buffer(2)]],
                                device uint *arguments [[buffer(3)]],
                                device ulong *statistics [[buffer(4)]],
                                device const ulong *predicate [[buffer(5)]]) {
  for (uint i = 0; i < 11; i++)
    arguments[i] = 0;
  if (control.predicate_enabled && (control.predicate_not_equal ? predicate[0] != 0 : predicate[0] == 0))
    return;
  indices += control.index_skip;
  if (control.index_size == 0)
    indices = nullptr;
  const uint topology = control.topology;
  const uint vpp = topology == 3 ? 2 : topology == 5 || topology == 6 ? 3 : topology == 11 ? 4 : 6;
  uint emitted = 0;
  ulong strip_vertices = 0;
  uint start = 0;
  for (uint k = 0; k <= control.count; k++) {
    if (k < control.count && (!control.cut_enabled || d3dalloy_mesh_read_index(indices, control.index_size, k) != control.cut_value))
      continue;
    const uint length = k - start;
    const uint s = start;
    start = k + 1;
    uint n = 0;
    if (topology == 3 && length >= 2) n = length - 1;
    if ((topology == 5 || topology == 6) && length >= 3) n = length - 2;
    if (topology == 11 && length >= 4) n = length - 3;
    if (topology == 13 && length >= 6) n = (length - 4) / 2;
    if (!n) continue;
    strip_vertices += topology == 13 ? 2 * n + 4 : n + vpp - 1;
    for (uint i = 0; i < n; i++) {
      uint v[6];
      if (topology == 3) {
        v[0] = i; v[1] = i + 1;
      } else if (topology == 5) {
        v[0] = i; v[1] = (i & 1) ? i + 2 : i + 1; v[2] = (i & 1) ? i + 1 : i + 2;
      } else if (topology == 6) {
        // D3D9 fan order: moving rim is the leading/provoking vertex.
        v[0] = i + 1; v[1] = i + 2; v[2] = 0;
      } else if (topology == 11) {
        for (uint j = 0; j < 4; j++) v[j] = i + j;
      } else {
        const uint t = 2 * i;
        const bool last = i + 1 == n;
        if (n == 1) {
          v[0] = 0; v[1] = 1; v[2] = 2; v[3] = 5; v[4] = 4; v[5] = 3;
        } else if (i == 0) {
          v[0] = 0; v[1] = 1; v[2] = 2; v[3] = 6; v[4] = 4; v[5] = 3;
        } else if (i & 1) {
          v[0] = t; v[1] = t + 3; v[2] = t + 4; v[3] = last ? t + 5 : t + 6; v[4] = t + 2; v[5] = t - 2;
        } else {
          v[0] = t; v[1] = t - 2; v[2] = t + 2; v[3] = last ? t + 5 : t + 6; v[4] = t + 4; v[5] = t + 3;
        }
      }
      for (uint j = 0; j < vpp; j++)
        list[emitted + j] = d3dalloy_mesh_read_index(indices, control.index_size, s + v[j]);
      emitted += vpp;
    }
  }
  arguments[0] = emitted;
  arguments[1] = control.instances;
  arguments[3] = uint(control.base_vertex);
  arguments[4] = control.start_instance;
  const ulong groups = emitted ? (emitted - 1) / control.per_group + 1 : 0;
  if (emitted && control.instances && groups * control.instances <= control.max_threadgroups) {
    arguments[8] = uint(groups);
    arguments[9] = control.instances;
    arguments[10] = 1;
  }
  // The list variant counts vpp vertices per primitive; IAVertices counts each strip vertex once (FS 20.4).
  if (control.statistics_enabled)
    statistics[0] -= (ulong(emitted) - strip_vertices) * control.instances;
}
/* TS+GS (VS+HS+DS+GS) between the DS capture pass and the GS pass. d3dalloy_tess_capture_clear zeroes the per-slot
 * primitive counts before the capture; d3dalloy_tess_capture_compact (one 1024-thread group) prefix-sums them in slot
 * order (instance, patch, workload rank: the D3D primitive order), writes the flat list (entry = slot * P + p) after
 * the total at list[0] and the GS object grid (ceil(total / per_group), 1, 1). See SM50_TESS_CAPTURE_LAYOUT. */
struct D3DalloyTessCompactControl {
  uint slot_count;
  uint max_primitives;  // P
  uint per_group;       // GS input primitives per object threadgroup
  uint reserved;
};
kernel void d3dalloy_tess_capture_clear(constant D3DalloyTessCompactControl &control [[buffer(0)]],
                                    device uint *counts [[buffer(1)]],
                                    uint index [[thread_position_in_grid]]) {
  if (index < control.slot_count)
    counts[index] = 0;
}
kernel void d3dalloy_tess_capture_compact(constant D3DalloyTessCompactControl &control [[buffer(0)]],
                                      device const uint *counts [[buffer(1)]],
                                      device uint *list [[buffer(2)]],
                                      device uint *dispatch [[buffer(3)]],
                                      uint tid [[thread_index_in_threadgroup]]) {
  threadgroup uint partial[1024];
  uint base = 0;
  for (uint chunk = 0; chunk < control.slot_count; chunk += 1024) {
    const uint slot = chunk + tid;
    const uint n = slot < control.slot_count ? min(counts[slot], control.max_primitives) : 0;
    partial[tid] = n;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint offset = 1; offset < 1024; offset <<= 1) {
      const uint value = tid >= offset ? partial[tid - offset] : 0;
      threadgroup_barrier(mem_flags::mem_threadgroup);
      partial[tid] += value;
      threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    const uint start = base + partial[tid] - n;
    for (uint p = 0; p < n; p++)
      list[4 + start + p] = slot * control.max_primitives + p;
    base += partial[1023];
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (tid == 0) {
    list[0] = base;
    list[1] = list[2] = list[3] = 0;
    dispatch[0] = (base + control.per_group - 1) / control.per_group;
    dispatch[1] = 1;
    dispatch[2] = 1;
  }
}
)";
    WMT::Reference<WMT::Error> error;
    auto library = device_->GetMTLDevice().newLibraryWithSource(source, error);
    if (!library)
      return false;
    auto snapshot = WMT4::Library(library).newFunction("d3dalloy_snapshot_predicate");
    auto arguments = WMT4::Library(library).newFunction("d3dalloy_predicate_arguments");
    auto copy = WMT4::Library(library).newFunction("d3dalloy_predicate_copy_bytes");
    auto copy_rows = WMT4::Library(library).newFunction("d3dalloy_predicate_copy_rows");
    auto mesh_indirect = WMT4::Library(library).newFunction("d3dalloy_mesh_indirect_arguments");
    auto mesh_strip_cut = WMT4::Library(library).newFunction("d3dalloy_mesh_strip_cut");
    auto tess_clear = WMT4::Library(library).newFunction("d3dalloy_tess_capture_clear");
    auto tess_compact = WMT4::Library(library).newFunction("d3dalloy_tess_capture_compact");
    if (!snapshot || !arguments || !copy || !copy_rows || !mesh_indirect || !mesh_strip_cut || !tess_clear || !tess_compact)
      return false;
    tess_capture_clear_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(tess_clear, error);
    if (!tess_capture_clear_pipeline_)
      return false;
    tess_capture_compact_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(tess_compact, error);
    if (!tess_capture_compact_pipeline_)
      return false;
    mesh_indirect_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(mesh_indirect, error);
    if (!mesh_indirect_pipeline_)
      return false;
    mesh_strip_cut_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(mesh_strip_cut, error);
    if (!mesh_strip_cut_pipeline_)
      return false;
    predication_snapshot_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(snapshot, error);
    if (!predication_snapshot_pipeline_)
      return false;
    predication_arguments_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(arguments, error);
    if (!predication_arguments_pipeline_)
      return false;
    predication_pitched_copy_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(copy_rows, error);
    if (!predication_pitched_copy_pipeline_)
      return false;
    predication_copy_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(copy, error);
    return bool(predication_copy_pipeline_);
  }

  void BeginPredicationComputePass(WMT::ComputePipelineState pipeline, uint32_t threadgroup_width = 1) {
    allocator_->InvalidateCurrentPass();
    auto compute = allocator_->AllocatePass<ComputeEncoderData>();
    compute->type = EncoderType::Compute;
    compute->cmd_head.type = WMTComputeCommandNop;
    compute->cmd_head.next.set(0);
    compute->cmd_tail = reinterpret_cast<wmtcmd_base *>(&compute->cmd_head);
    compute->internal_pipeline = pipeline;
    auto &set_pipeline = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    set_pipeline.type = WMTComputeCommandSetPSO;
    set_pipeline.pso = pipeline;
    set_pipeline.threadgroup_size = {threadgroup_width, 1, 1};
  }

  void BindPredicationBuffer(WMT::Buffer buffer, uint64_t offset, uint8_t index) {
    auto &binding = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    binding.type = WMTComputeCommandSetBuffer;
    binding.buffer = buffer;
    binding.offset = offset;
    binding.index = index;
  }

  void BindGPUHeapPredicationBuffer(uint64_t offset, uint8_t index) {
    auto &binding = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    binding.type = WMTComputeCommandSetBuffer;
    allocator_->BindGPUHeap(binding.buffer, binding.offset, offset);
    binding.index = index;
  }

  void DispatchPredicationKernel() {
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {1, 1, 1};
    allocator_->InvalidateCurrentPass();
  }

  /* ClearUnorderedAccessViewUint on a feedback UAV: zero every cell (values and rects are ignored). */
  void ClearSamplerFeedbackMap(MTLD3D12Resource *resource, const UAVBufferCPUStorage &cells) {
    if (!cells.buffer || !resource || !resource->feedback || resource->buffer.ptr() != cells.buffer) {
      RecordError();
      return;
    }
    const std::array<uint32_t, 4> zero{};
    const uint32_t count = cells.slice.elementCount;
    uint64_t indirect_offset = 0;
    if (predication_enabled_) {
      const uint32_t dispatch_arguments[] = {(count + 31) / 32, 1, 1};
      indirect_offset = PreparePredicatedArguments(dispatch_arguments, sizeof(dispatch_arguments));
      if (indirect_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    allocator_->clear_uav_.begin(zero, cells.buffer);
    {
              if (FAILED(recording_error_)) return;
              const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(indirect_offset)
                  : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
              allocator_->clear_uav_.clear(0, 0, count, 1, arguments.buffer, arguments.offset);
            };
    allocator_->clear_uav_.end();
  }

  struct FeedbackTranscode {
    uint32_t cell_base, grid_w, src_x, src_y, width, height, dst_x, dst_y, row_pitch, min_mip;
  };

  /* One decode (cells -> R8 bytes) or encode (R8 bytes -> cells) rect; `bytes` is a linear R8 image. */
  bool DispatchFeedbackTranscode(bool encode, WMT::Buffer cells, WMT::Buffer bytes, uint64_t bytes_offset,
                                 const FeedbackTranscode &control) {
    auto &pipeline = feedback_transcode_pipelines_[encode];
    if (!pipeline) {
      auto function = WMT4::Library(device_->GetLib().getLibrary()).newFunction(encode ? "d3d12_feedback_encode"
                                                                        : "d3d12_feedback_decode");
      WMT::Reference<WMT::Error> error;
      if (function)
        pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!pipeline) {
        RecordError(E_FAIL);
        return false;
      }
    }
    const uint32_t texels = control.width * control.height;
    if (!texels)
      return true;
    if (!allocator_->ReserveGPUHeap(sizeof(control), 16)) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto [storage, control_offset] = allocator_->AllocateGPUHeap(sizeof(control), 16);
    memcpy(storage, &control, sizeof(control));
    uint64_t indirect_offset = 0;
    if (predication_enabled_) {
      const uint32_t dispatch_arguments[] = {(texels + 63) / 64, 1, 1};
      indirect_offset = PreparePredicatedArguments(dispatch_arguments, sizeof(dispatch_arguments));
      if (indirect_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return false;
      }
    }
    BeginPredicationComputePass(pipeline, 64);
    BindPredicationBuffer(encode ? bytes : cells, encode ? bytes_offset : 0, 0);
    BindPredicationBuffer(encode ? cells : bytes, encode ? 0 : bytes_offset, 1);
    BindGPUHeapPredicationBuffer(control_offset, 2);
    if (predication_enabled_) {
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
      dispatch.type = WMTComputeCommandDispatchIndirect;
      allocator_->BindGPUHeap(dispatch.indirect_args_buffer, dispatch.indirect_args_offset, indirect_offset);
    } else {
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {(texels + 63) / 64, 1, 1};
    }
    allocator_->InvalidateCurrentPass();
    return true;
  }

  /*
   * ResolveSubresourceRegion ENCODE/DECODE_SAMPLER_FEEDBACK (SamplerFeedback spec, Transcoding). The
   * non-opaque side is an R8_UINT TEXTURE2D (MIN_MIP: mip count 1, subresource = array slice;
   * MIP_REGION_USED: subresource = the map's mip + slice) or, for a single-slice MIN_MIP map, a buffer
   * with one byte per region, rows of ceil(Width / region width) bytes. The map side's subresource is
   * UINT_MAX for MIN_MIP; UINT_MAX on both sides transcodes every subresource. Coordinates are in the
   * decoded space; a source rect is permitted for MIP_REGION_USED maps only, not with UINT_MAX.
   */
  void TranscodeSamplerFeedback(bool encode, ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
                                ID3D12Resource *pSrcResource, UINT SrcSubresource, const D3D12_RECT *pSrcRect,
                                DXGI_FORMAT Format) {
    auto *map = static_cast<MTLD3D12Resource *>(encode ? pDstResource : pSrcResource);
    auto *plain = static_cast<MTLD3D12Resource *>(encode ? pSrcResource : pDstResource);
    const UINT map_subresource = encode ? DstSubresource : SrcSubresource;
    const UINT plain_subresource = encode ? SrcSubresource : DstSubresource;
    if (!map || !plain || map == plain || !QueryObjectOwnedByThisDevice(map) || !QueryObjectOwnedByThisDevice(plain) ||
        !map->feedback || !map->buffer || Format != DXGI_FORMAT_R8_UINT) {
      RecordError();
      return;
    }
    const auto &layout = *map->feedback;
    const auto plain_desc = plain->GetDesc();
    const bool all = map_subresource == UINT_MAX && plain_subresource == UINT_MAX;
    const bool to_buffer = plain_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    if ((pSrcRect && (!layout.region_used || all)) ||
        (!layout.region_used && map_subresource != UINT_MAX) ||
        (layout.region_used && !all && map_subresource != plain_subresource) ||
        (to_buffer ? (layout.region_used || layout.slices != 1 || !plain->buffer ||
                      (plain_subresource != 0 && plain_subresource != UINT_MAX))
                   : (plain_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !plain->texture ||
                      plain_desc.SampleDesc.Count != 1 || plain_desc.DepthOrArraySize != layout.slices ||
                      plain_desc.MipLevels != layout.decodedMips() ||
                      (plain_desc.Format != DXGI_FORMAT_R8_UINT && plain_desc.Format != DXGI_FORMAT_R8_TYPELESS)))) {
      RecordError();
      return;
    }

    /* The (map mip, slice) pairs to transcode. */
    small_vector<std::pair<UINT, UINT>, 16> subresources;
    if (to_buffer || all || !layout.region_used) {
      const UINT first_slice = (all || to_buffer || plain_subresource == UINT_MAX) ? 0 : plain_subresource;
      const UINT slice_end = (all || to_buffer || plain_subresource == UINT_MAX) ? layout.slices : first_slice + 1;
      if (!layout.region_used && !to_buffer && plain_subresource != UINT_MAX && plain_subresource >= layout.slices) {
        RecordError();
        return;
      }
      for (UINT slice = first_slice; slice < slice_end; ++slice)
        for (UINT mip = 0; mip < layout.decodedMips(); ++mip)
          subresources.push_back({mip, slice});
    } else {
      if (map_subresource >= layout.mips * layout.slices) {
        RecordError();
        return;
      }
      subresources.push_back({map_subresource % layout.mips, map_subresource / layout.mips});
    }

    if (!RecordImplicitUse(pSrcResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_SOURCE) ||
        !RecordImplicitUse(pDstResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST))
      return;
    if (!to_buffer)
      RecordStandardSwizzleUse(plain, false);
    auto cells = map->buffer->current()->buffer();
    for (auto [mip, slice] : subresources) {
      const uint32_t grid_w = layout.gridWidth(mip), grid_h = layout.gridHeight(mip);
      /* The source rect and the destination origin, in decoded space. */
      D3D12_RECT rect{0, 0, LONG(grid_w), LONG(grid_h)};
      UINT plain_w = grid_w, plain_h = grid_h, level = 0, plain_slice = 0;
      if (to_buffer) {
        if (plain_desc.Width < uint64_t(grid_w) * grid_h) {
          RecordError();
          return;
        }
      } else {
        UINT plane = 0;
        const UINT index = layout.region_used ? mip + slice * plain_desc.MipLevels : slice;
        if (!GetSubresourceInfo(device_->GetMTLDevice(), plain_desc, index, level, plain_slice, plane) ||
            level != mip || plain_slice != slice) {
          RecordError();
          return;
        }
        const auto extent = GetResourceExtent(plain_desc, level);
        plain_w = extent.right;
        plain_h = extent.bottom;
      }
      if (pSrcRect)
        rect = *pSrcRect;
      const UINT src_w = encode ? plain_w : grid_w, src_h = encode ? plain_h : grid_h;
      const UINT dst_w = encode ? grid_w : plain_w, dst_h = encode ? grid_h : plain_h;
      if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
          UINT(rect.right) > src_w || UINT(rect.bottom) > src_h || DstX > dst_w ||
          UINT(rect.right - rect.left) > dst_w - DstX || DstY > dst_h || UINT(rect.bottom - rect.top) > dst_h - DstY) {
        RecordError();
        return;
      }
      const UINT width = UINT(rect.right - rect.left), height = UINT(rect.bottom - rect.top);
      FeedbackTranscode control{uint32_t(slice * layout.sliceCells() + layout.mipOffset(mip)),
                                grid_w,
                                uint32_t(rect.left),
                                uint32_t(rect.top),
                                width,
                                height,
                                DstX,
                                DstY,
                                grid_w,
                                !layout.region_used};
      if (to_buffer) {
        if (!DispatchFeedbackTranscode(encode, cells, plain->buffer->current()->buffer(), 0, control))
          return;
        continue;
      }
      /* Texture side: through a linear R8 image of the rect (pitch rounded up to 16 bytes). */
      const uint32_t pitch = (width + 15) & ~15u;
      auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(size_t(pitch) * height, 256);
      if (!scratch) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      auto native = plain->texture->current()->texture();
      if (encode) {
        PreBlit();
        auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        copy.type = WMTBlitCommandCopyFromTextureToBuffer;
        copy.src = native;
        copy.slice = slice;
        copy.level = level;
        copy.origin = {uint64_t(rect.left), uint64_t(rect.top), 0};
        copy.size = {width, height, 1};
        copy.dst = scratch;
        copy.offset = scratch_offset;
        copy.bytes_per_row = pitch;
        copy.bytes_per_image = pitch * height;
        control.src_x = control.src_y = 0;
        control.row_pitch = pitch;
        if (!DispatchFeedbackTranscode(true, cells, scratch, scratch_offset, control))
          return;
      } else {
        control.dst_x = control.dst_y = 0;
        control.row_pitch = pitch;
        if (!DispatchFeedbackTranscode(false, cells, scratch, scratch_offset, control))
          return;
        PreBlit();
        auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        copy.type = WMTBlitCommandCopyFromBufferToTexture;
        copy.src = scratch;
        copy.src_offset = scratch_offset;
        copy.bytes_per_row = pitch;
        copy.bytes_per_image = pitch * height;
        copy.size = {width, height, 1};
        copy.dst = native;
        copy.slice = slice;
        copy.level = level;
        copy.origin = {DstX, DstY, 0};
      }
    }
    if (!to_buffer && !encode)
      RecordStandardSwizzleUse(plain, true);
  }

  bool ConvertD24Plane(WMT::Buffer source, uint64_t source_offset, UINT source_pitch,
                       WMT::Buffer destination, uint64_t destination_offset, UINT destination_pitch,
                       UINT width, UINT height, bool to_native_float) {
    auto &pipeline = to_native_float ? d24_upload_pipeline_ : d24_readback_pipeline_;
    if (!pipeline) {
      auto function = WMT4::Library(device_->GetLib().getLibrary()).newFunction(to_native_float ? "d3d12_d24_to_float" : "d3d12_float_to_d24");
      if (!function) {
        RecordError(E_FAIL);
        return false;
      }
      WMT::Reference<WMT::Error> error;
      pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!pipeline) {
        RecordError(E_FAIL);
        return false;
      }
    }
    if (!allocator_->ReserveGPUHeap(4 * sizeof(uint32_t), 16)) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto [control, control_offset] = allocator_->AllocateGPUHeap(4 * sizeof(uint32_t), 16);
    auto *layout = static_cast<uint32_t *>(control);
    layout[0] = source_pitch;
    layout[1] = destination_pitch;
    layout[2] = width;
    layout[3] = height;
    BeginPredicationComputePass(pipeline, 64);
    BindPredicationBuffer(source, source_offset, 0);
    BindPredicationBuffer(destination, destination_offset, 1);
    BindGPUHeapPredicationBuffer(control_offset, 2);
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {(uint64_t(width) * height + 63) / 64, 1, 1};
    allocator_->InvalidateCurrentPass();
    return true;
  }

  bool ConvertEmulatedTextureRows(resource_contract::EmulatedTextureStorage storage, bool upload,
                                  WMT::Buffer source, uint64_t source_offset, UINT source_pitch,
                                  WMT::Buffer destination, uint64_t destination_offset, UINT destination_pitch,
                                  UINT width, UINT height) {
    const bool rgb = storage == resource_contract::EmulatedTextureStorage::RGB32;
    if (!rgb && storage != resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 &&
        storage != resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8)
      return false;
    const unsigned index = (rgb ? 0u : 2u) + (upload ? 0u : 1u);
    auto &pipeline = format_conversion_pipelines_[index];
    if (!pipeline) {
      const char *name = rgb ? (upload ? "d3d12_rgb32_expand" : "d3d12_rgb32_compact") :
                               (upload ? "d3d12_packed422_expand" : "d3d12_packed422_compact");
      auto function = WMT4::Library(device_->GetLib().getLibrary()).newFunction(name);
      if (!function) {
        RecordError(E_FAIL);
        return false;
      }
      WMT::Reference<WMT::Error> error;
      pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!pipeline) {
        RecordError(E_FAIL);
        return false;
      }
    }
    if (!allocator_->ReserveGPUHeap(5 * sizeof(uint32_t), 16)) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto [control, control_offset] = allocator_->AllocateGPUHeap(5 * sizeof(uint32_t), 16);
    auto *layout = static_cast<uint32_t *>(control);
    layout[0] = source_pitch;
    layout[1] = destination_pitch;
    layout[2] = width;
    layout[3] = height;
    layout[4] = storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8;
    BeginPredicationComputePass(pipeline, 64);
    BindPredicationBuffer(source, source_offset, 0);
    BindPredicationBuffer(destination, destination_offset, 1);
    BindGPUHeapPredicationBuffer(control_offset, 2);
    if (!rgb)
      BindGPUHeapPredicationBuffer(control_offset + 4 * sizeof(uint32_t), 3);
    const uint64_t elements = uint64_t(rgb ? width : (width + 1) / 2) * height;
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {(elements + 63) / 64, 1, 1};
    allocator_->InvalidateCurrentPass();
    return true;
  }

  void EncodePredicatedBufferCopy(WMT::Buffer source, uint64_t source_offset, WMT::Buffer destination,
                                  uint64_t destination_offset, uint64_t byte_count) {
    while (byte_count) {
      const uint64_t chunk = std::min<uint64_t>(byte_count, UINT32_MAX);
      if (!allocator_->ReserveGPUHeap(2 * sizeof(uint64_t), 8)) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      auto [control, control_offset] = allocator_->AllocateGPUHeap(2 * sizeof(uint64_t), 8);
      auto *words = static_cast<uint64_t *>(control);
      words[0] = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
      words[1] = chunk;
      BeginPredicationComputePass(predication_copy_pipeline_, 64);
      BindGPUHeapPredicationBuffer(predication_snapshot_offset_, 0);
      BindGPUHeapPredicationBuffer(control_offset, 1);
      BindPredicationBuffer(source, source_offset, 2);
      BindPredicationBuffer(destination, destination_offset, 3);
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {(chunk + 63) / 64, 1, 1};
      allocator_->InvalidateCurrentPass();
      source_offset += chunk;
      destination_offset += chunk;
      byte_count -= chunk;
    }
  }

  /* One conditional dispatch for `depth` slices of `rows` rows of `row_bytes` each, reading the same predicate
   * snapshot as EncodePredicatedBufferCopy. Returns false (nothing encoded) when the copy does not fit one grid. */
  bool EncodePredicatedPitchedCopy(WMT::Buffer source, uint64_t source_offset, uint64_t source_row_pitch,
                                   uint64_t source_slice_pitch, WMT::Buffer destination, uint64_t destination_offset,
                                   uint64_t destination_row_pitch, uint64_t destination_slice_pitch,
                                   uint64_t row_bytes, uint64_t rows, uint64_t depth) {
    if (!row_bytes || !rows || !depth || row_bytes > UINT32_MAX || rows > UINT32_MAX || depth > UINT32_MAX)
      return false;
    if (!allocator_->ReserveGPUHeap(6 * sizeof(uint64_t), 8)) {
      RecordError(E_OUTOFMEMORY);
      return true;
    }
    auto [control, control_offset] = allocator_->AllocateGPUHeap(6 * sizeof(uint64_t), 8);
    auto *words = static_cast<uint64_t *>(control);
    words[0] = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    words[1] = row_bytes;
    words[2] = source_row_pitch;
    words[3] = source_slice_pitch;
    words[4] = destination_row_pitch;
    words[5] = destination_slice_pitch;
    BeginPredicationComputePass(predication_pitched_copy_pipeline_, 64);
    BindGPUHeapPredicationBuffer(predication_snapshot_offset_, 0);
    BindGPUHeapPredicationBuffer(control_offset, 1);
    BindPredicationBuffer(source, source_offset, 2);
    BindPredicationBuffer(destination, destination_offset, 3);
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {(row_bytes + 63) / 64, rows, depth};
    allocator_->InvalidateCurrentPass();
    return true;
  }

  uint64_t PreparePredicatedArguments(const void *arguments, UINT byte_count) {
    if (!allocator_->ReserveGPUHeap(size_t(byte_count) * 2 + 32, 16)) {
      RecordError(E_OUTOFMEMORY);
      return UINT64_MAX;
    }
    auto [control, control_offset] = allocator_->AllocateGPUHeap(byte_count + 2 * sizeof(uint32_t), 16);
    auto [output, output_offset] = allocator_->AllocateGPUHeap(byte_count, 16);
    auto *words = static_cast<uint32_t *>(control);
    words[0] = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    words[1] = byte_count / sizeof(uint32_t);
    memcpy(words + 2, arguments, byte_count);
    memset(output, 0, byte_count);

    BeginPredicationComputePass(predication_arguments_pipeline_);
    BindGPUHeapPredicationBuffer(predication_snapshot_offset_, 0);
    BindGPUHeapPredicationBuffer(control_offset, 1);
    BindGPUHeapPredicationBuffer(output_offset, 2);
    DispatchPredicationKernel();
    return output_offset;
  }

  void ResetIndirectState(MTLD3D12CommandSignature *sig, MTLD3D12RootSignature *rootsig,
                          uint64_t *root_staging, bool compute) {
    if (sig->UpdateRootArguments && rootsig) {
      for (UINT parameter = 0; parameter < rootsig->ParameterSlots && parameter < 64; parameter++) {
        if (!(sig->RootParameterMask & (uint64_t(1) << parameter)))
          continue;
        const UINT begin = rootsig->SlotQwordOffsets[parameter];
        // The last parameter ends at the parameter qwords: the heap/status/view-id qwords after it are not root
        // arguments and keep their values.
        const UINT end = parameter + 1 < rootsig->ParameterSlots ? rootsig->SlotQwordOffsets[parameter + 1]
                                                                 : rootsig->ParameterQwords;
        if (begin < end && end <= 64)
          memset(root_staging + begin, 0, sizeof(uint64_t) * (end - begin));
      }
      dirty_state_.set(compute ? DirtyState::ComputeRootArguments : DirtyState::GraphicsRootArguments);
    }
    if (!compute && sig->UpdateVertexBuffers) {
      for (UINT slot = 0; slot < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; slot++)
        if (sig->VertexBufferMask & (1u << slot))
          vertex_buffers_[slot] = {};
      dirty_state_.set(DirtyState::VertexBuffer);
    }
    if (!compute && sig->UpdateIndexBuffer) {
      index_buffer_address = 0;
      index_buffer = {};
      index_type = {};
      index_offset = 0;
    }
  }

public:
  MTLD3D12GraphicsCommandListImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type)
      : MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList, IMTLD3D12GraphicsCommandListExt, IMTLD3D12FrameGenerationCommandList,
                            IMTLD3D12RayRegenerationCommandList>(pDevice),
        type_(Type) {
    entry = nullptr;
    encoder_count = 0;
  }

  ~MTLD3D12GraphicsCommandListImpl() {
    // Releasing an unfinished list must release its allocator's recording slot.
    if (IsRecording())
      allocator_->EndRecord(&encoder_count);
  }

  void
  ResetState(ID3D12PipelineState *pInitialPipelineState) {
    pso_graphics_ = nullptr;
    pso_compute_ = nullptr;
    if (auto pso = static_cast<MTLD3D12PipelineState *>(pInitialPipelineState)) {
      if (!pso->IsComputePipelineState)
        pso_graphics_ = static_cast<MTLD3D12GraphicsPipelineState *>(pInitialPipelineState);
      else
        pso_compute_ = static_cast<MTLD3D12ComputePipelineState *>(pInitialPipelineState);
    }

    num_rtvs = {};
    memset(rtvs, 0, sizeof(rtvs));
    dsv = {};
    async_targets_epoch_ = 0;

    topology_ = {};

    num_viewports = {};
    memset(viewports, 0, sizeof(viewports));

    num_scissors = {};
    memset(scissors, 0, sizeof(scissors));

    blend_factor_[0] = 1.0f;
    blend_factor_[1] = 1.0f;
    blend_factor_[2] = 1.0f;
    blend_factor_[3] = 1.0f;
    stencil_ref_ = 0;
    back_stencil_ref_ = 0;
    depth_bounds_min_ = 0;
    depth_bounds_max_ = 1;
    view_instance_mask_ = 0;
    shading_rate_ = D3D12_SHADING_RATE_1X1;
    sample_positions_ = {};
    shading_rate_image_ = nullptr;
    shading_rate_combiners_ = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    render_pass_active_ = false;
    render_pass_flags_ = 0;
    render_pass_discard_colors_.fill(false);
    render_pass_discard_color_count_ = 0;
    render_pass_discard_depth_ = render_pass_discard_stencil_ = false;
    render_pass_ends_local_ = render_pass_pending_local_ = render_pass_local_compute_ = false;
    render_pass_suspended_ = false;
    render_pass_resolves_.clear();
    render_pass_surfaces_.clear();
    AdoptPipelineDynamicState();

    rootsig_graphics_ = nullptr;
    memset(rootarg_graphics_staging_, 0, sizeof(rootarg_graphics_staging_));

    rootsig_compute_ = nullptr;
    memset(rootarg_compute_staging_, 0, sizeof(rootarg_compute_staging_));
    predication_enabled_ = false;
    predication_snapshot_offset_ = 0;
    predication_op_ = D3D12_PREDICATION_OP_EQUAL_ZERO;

    rt_state_object_ = nullptr;
    work_graph_ = nullptr;
    work_graph_program_ = {};
    work_graph_max_records_ = work_graph_max_node_inputs_ = UINT32_MAX;
    rt_pipeline_ = {};
    rt_intersection_function_table_ = {};
    rt_visible_function_table_ = {};

    memset(vertex_buffers_.data(), 0, sizeof(vertex_buffers_));
    so_targets_ = {};
    so_pending_.reset();
    so_bound_.reset();

    index_buffer_address = 0;
    index_buffer = {};
    index_type = {};
    index_offset = 0;

    dirty_state_.clrAll();
    descriptor_heaps_ = {};
    // Cached lookups name allocations retained by the previous recording only.
    recorded_va_lookups_ = {};
    // Drop the plans too, so a list does not keep released pipelines alive across recordings.
    submission_capture_plans_ = {};
    static_sampler_uploads_ = {};
    swizzle_capture_owner_ = nullptr;
    swizzle_capture_keys_.clear();
  }

  HRESULT
  Initialize(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialPipelineState) {
    if (!pAllocator)
      return E_INVALIDARG;

    Com<ID3D12Device> allocator_device;
    if (FAILED(pAllocator->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&allocator_device))) ||
        allocator_device.ptr() != static_cast<ID3D12Device *>(device_))
      return E_INVALIDARG;

    auto allocator = static_cast<MTLD3D12CommandAllocatorImpl *>(pAllocator);
    if (allocator->type_ != type_)
      return E_INVALIDARG;

    if (pInitialPipelineState) {
      Com<ID3D12Device> pipeline_device;
      if (FAILED(pInitialPipelineState->GetDevice(
              __uuidof(ID3D12Device), reinterpret_cast<void **>(&pipeline_device))) ||
          pipeline_device.ptr() != static_cast<ID3D12Device *>(device_))
        return E_INVALIDARG;
    }

    // Claim the new allocator before touching the old closed recording. In
    // particular, a busy-allocator Reset must not discard executable commands
    // or drop references to bundles still pending in that recording.
    EncoderData *new_entry = nullptr;
    HRESULT hr;
    try {
      hr = allocator->StartRecord(&new_entry);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    if (FAILED(hr))
      return hr;

    allocator_ = allocator;
    owned_allocator = allocator;
    owned_allocator_generation = allocator->recording_generation_;
    owned_closed_generation = owned_allocator_generation->load(std::memory_order_acquire);
    entry = new_entry;
    encoder_count = std::numeric_limits<size_t>::max();
    active_visibility_queries_.clear();
    active_so_queries_.clear();
    active_pipeline_statistics_.clear();
    so_counters_allocated_ = false;
    so_counters_offset_ = 0;
    pipeline_statistics_counters_allocated_ = false;
    pipeline_statistics_counters_offset_ = 0;
    ResetState(pInitialPipelineState);
    bundle_commands_.clear();
    bundle_initial_pso_ = pInitialPipelineState;
    bundle_allocator_generation_ = allocator_->recording_generation_->load(std::memory_order_acquire);
    bundle_heaps_set_ = false;
    bundle_pipeline_seen_ = false;
    recording_error_ = S_OK;
    close_error_ = S_OK;
    ObserveBundlePipeline(pInitialPipelineState);
    owned_closed_valid = false;
    owned_bundle_references.clear();
    ++owned_recording;
    owned_graphics::Trace("record_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"allocator\":", allocator_->GetTraceId());
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IMTLD3D12FrameGenerationCommandList)) {
      *ppvObject = static_cast<IMTLD3D12FrameGenerationCommandList *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IMTLD3D12RayRegenerationCommandList)) {
      *ppvObject = static_cast<IMTLD3D12RayRegenerationCommandList *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IMTLD3D12GraphicsCommandListExt)) {
      *ppvObject = static_cast<IMTLD3D12GraphicsCommandListExt *>(this);
      AddRef();
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
        riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4) ||
        riid == __uuidof(ID3D12GraphicsCommandList5) || riid == __uuidof(ID3D12GraphicsCommandList6) ||
        riid == __uuidof(ID3D12GraphicsCommandList7) || riid == __uuidof(ID3D12GraphicsCommandList8) ||
        riid == __uuidof(ID3D12GraphicsCommandList9) || riid == __uuidof(ID3D12GraphicsCommandList10) ||
        riid == __uuidof(ID3D12GraphicsCommandList11) || riid == __uuidof(ID3D12GraphicsCommandListPreview)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid)) {
      WARN("D3D12GraphicsCommandList: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE
  GetType() {
    return type_;
  }

  HRESULT STDMETHODCALLTYPE ValidateRecording() final {
    if (!allocator_ || encoder_count != std::numeric_limits<size_t>::max())
      return E_FAIL;
    if (FAILED(recording_error_))
      return recording_error_;
    return allocator_->type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
           allocator_->type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE ? S_OK : E_INVALIDARG;
  }

  HRESULT RecordFrameGeneration(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
    const auto resources = GetOwnedFrameGenerationResources(frame);
    const auto states = GetOwnedFrameGenerationStates(frame);
    for (size_t i = 0; i < resources.size(); ++i)
      if (resources[i] && !RecordImplicitUse(resources[i], 0, states[i])) return recording_error_;
    // Native encoding leaves application roots, descriptors, predicates and queries untouched.
    allocator_->InvalidateCurrentPass();
    auto pass = allocator_->AllocatePass<TemporalEncoderData>();
    pass->type = EncoderType::Temporal;
    pass->fg_frame = frame;
    allocator_->InvalidateCurrentPass();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DenoiseRayRegeneration(IMTLD3D12RayRegenerationState *state,
      const MTL_RAY_REGENERATION_FRAME_DESC *desc) final try {
    auto hr = ValidateRecording();
    if (FAILED(hr)) return hr;
    std::shared_ptr<OwnedRayRegenerationFrame> frame;
    hr = PrepareOwnedRayRegenerationFrame(device_, state, desc, frame);
    if (FAILED(hr)) return hr;
    const auto resources = GetOwnedRayRegenerationResources(frame);
    const auto states = GetOwnedRayRegenerationStates(frame);
    for (size_t i = 0; i < resources.size(); ++i)
      if (resources[i] && !RecordImplicitUse(resources[i], 0, states[i])) return recording_error_;
    // Native encoding leaves application roots, descriptors, predicates and queries untouched.
    allocator_->InvalidateCurrentPass();
    auto pass = allocator_->AllocatePass<TemporalEncoderData>();
    pass->type = EncoderType::Temporal;
    pass->rr_frame = std::move(frame);
    allocator_->InvalidateCurrentPass();
    return S_OK;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE PrepareFrameGeneration(IMTLD3D12FrameGenerationState *state,
      const MTL_FRAME_GENERATION_PREPARE_DESC *desc) final try {
    auto hr = ValidateRecording();
    if (FAILED(hr)) return hr;
    std::shared_ptr<OwnedFrameGenerationFrame> frame;
    hr = PrepareOwnedFrameGenerationFrame(device_, state, desc, frame);
    if (hr == S_FALSE) return S_OK;
    if (FAILED(hr)) return hr;
    return RecordFrameGeneration(frame);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE GenerateFrames(IMTLD3D12FrameGenerationState *state,
      const MTL_FRAME_GENERATION_GENERATE_DESC *desc) final try {
    auto hr = ValidateRecording();
    if (FAILED(hr)) return hr;
    std::shared_ptr<OwnedFrameGenerationFrame> frame;
    hr = GenerateOwnedFrameGenerationFrame(device_, state, desc, frame);
    if (FAILED(hr)) return hr;
    return RecordFrameGeneration(frame);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }


  HRESULT STDMETHODCALLTYPE TemporalUpscale(IMTLD3D12TemporalUpscaleState *state,
      const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc) final try {
    auto hr = ValidateRecording();
    if (FAILED(hr))
      return hr;
    std::shared_ptr<OwnedTemporalFrame> frame;
    hr = PrepareOwnedTemporalFrame(device_, state, desc, frame);
    if (FAILED(hr))
      return hr;
    allocator_->InvalidateCurrentPass();
    auto pass = allocator_->AllocatePass<TemporalEncoderData>();
    pass->type = EncoderType::Temporal;
    pass->frame = std::move(frame);
    allocator_->InvalidateCurrentPass();
    return S_OK;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE
  Close() try {
    if (encoder_count < std::numeric_limits<size_t>::max())
      return E_FAIL;
    if (!active_visibility_queries_.empty() || !active_so_queries_.empty() ||
        !active_pipeline_statistics_.empty())
      RecordError();
    // A render pass must end before Close, unless it suspends into the next command list; a list
    // cannot end with surfaces left in PRESERVE_LOCAL_* (render passes spec, State At End).
    if ((render_pass_active_ && !(render_pass_flags_ & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS)) ||
        (render_pass_pending_local_ && !render_pass_suspended_)) {
      ERR("Close: render pass not ended");
      RecordError();
    }
    auto hr = allocator_->EndRecord(&encoder_count);
    async_targets_epoch_ = 0;
    for (auto *pass = SUCCEEDED(recording_error_) ? static_cast<EncoderData *>(entry)->next : nullptr;
         pass; pass = pass->next) {
      if (pass->type != EncoderType::Render) continue;
      auto *render = static_cast<RenderEncoderData *>(pass);
      if (!render->visibility_intervals || !render->visibility_interval_count) continue;
      WMTBufferInfo info{};
      info.length = render->visibility_interval_count * sizeof(uint64_t);
      info.options = WMTResourceHazardTrackingModeUntracked;
      render->visibility_scratch = device_->GetMTLDevice().newBuffer(info);
      if (!render->visibility_scratch) RecordError(E_OUTOFMEMORY);
    }
    if (SUCCEEDED(hr) && FAILED(recording_error_))
      hr = recording_error_;
    close_error_ = hr;
    owned_closed_valid = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
      owned_graphics::Trace("record_close", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"encoders\":", encoder_count);
    return hr;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
    if (IsRecording())
      allocator_->EndRecord(&encoder_count);
    close_error_ = recording_error_;
    owned_closed_valid = false;
    return close_error_;
  };

  HRESULT STDMETHODCALLTYPE
  Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) try {
    if (bundle_recording_->unsubmitted.load(std::memory_order_acquire))
      return E_FAIL;
    if (FAILED(close_error_))
      return close_error_;
    if (encoder_count == std::numeric_limits<size_t>::max())
      return E_FAIL;
    return Initialize(pAllocator, pInitialState);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  };

  void STDMETHODCALLTYPE
  ClearState(ID3D12PipelineState *pPipelineState) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    allocator_->InvalidateCurrentPass();
    ResetState(pPipelineState);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  std::tuple<uint64_t, uint64_t>
  PopulateVertexBufferTable(uint32_t Count, uint32_t updated_slots = 0) {
    const auto slot_mask = (pso_graphics_ ? pso_graphics_->slot_mask : 0) | updated_slots;
    if (!slot_mask)
      return {0, 0};
    uint32_t max_slot = 32 - __builtin_clz(slot_mask);
    struct VERTEX_BUFFER_ENTRY {
      uint64_t buffer_handle;
      uint32_t stride;
      uint32_t length;
    };
    auto stride = align(sizeof(VERTEX_BUFFER_ENTRY) * max_slot, 16);

    auto [mapped, offset] = allocator_->AllocateGPUHeap(uint64_t(stride) * Count, 16);
    if (!mapped) {
      RecordError(E_OUTOFMEMORY);
      return {UINT64_MAX, 0};
    }

    for (unsigned i = 0; i < Count; i++) {
      auto *entries = reinterpret_cast<VERTEX_BUFFER_ENTRY *>(reinterpret_cast<char *>(mapped) + i * stride);
      memset(entries, 0, stride);
      for (unsigned slot = 0; slot < max_slot; slot++) {
        if (!(slot_mask & (1u << slot)))
          continue;
        auto &state = vertex_buffers_[slot];
        entries[slot].buffer_handle = state.BufferLocation;
        entries[slot].stride = state.StrideInBytes;
        entries[slot].length = state.SizeInBytes;
      };
    }

    return {offset, stride};
  }

  bool index_pull_pipeline_bound_ = false;
  obj_handle_t direct_pipeline_bound_ = 0;

  void CaptureAtomic64Roots(std::vector<Atomic64RootBinding> &roots,
                           MTLD3D12RootSignature *signature, const uint64_t *arguments,
                           const MTLD3D12PipelineState *pipeline = nullptr) {
    if (!signature) return;
    for (uint32_t parameter = 0; parameter < signature->ParameterSlots; ++parameter) {
      const auto type = signature->ParameterTypes[parameter];
      if (type != D3D12_ROOT_PARAMETER_TYPE_UAV && type != D3D12_ROOT_PARAMETER_TYPE_SRV &&
          type != D3D12_ROOT_PARAMETER_TYPE_CBV) continue;
      if (pipeline) {
        const auto *range = signature->GetRootDescriptorRange(parameter);
        bool used = false;
        if (range) signature->ForEachUsedDescriptorRange({range, 1}, pipeline->resource_bindings,
            [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE) { used = true; });
        if (!used) continue;
      }
      const auto address = arguments[signature->SlotQwordOffsets[parameter]];
      if (!address) continue;
      const bool writable = type == D3D12_ROOT_PARAMETER_TYPE_UAV;
      if (std::none_of(roots.begin(), roots.end(), [&](const auto &root) {
            return root.address == address && root.writable == writable;
          })) roots.push_back({address, UINT64_MAX, writable});
    }
  }

  /* Identity of each captured table use, (rootsig, heap, parameter, base); a direct heap use has base 0. Keys
   * describe the use list `swizzle_capture_owner_` only, rebuilt from its contents when another list is captured. */
  struct SwizzleCaptureKey {
    const void *rootsig, *heap;
    uint64_t base;
    uint32_t parameter;
    bool operator==(const SwizzleCaptureKey &) const = default;
  };
  struct SwizzleCaptureKeyHash {
    size_t operator()(const SwizzleCaptureKey &key) const {
      uint64_t hash = reinterpret_cast<uintptr_t>(key.rootsig) * 0x9e3779b97f4a7c15ull;
      hash ^= reinterpret_cast<uintptr_t>(key.heap) + 0x7f4a7c159e3779b9ull + (hash << 6) + (hash >> 2);
      hash ^= key.base + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
      hash ^= key.parameter + (hash << 6) + (hash >> 2);
      return size_t(hash);
    }
  };
  const std::vector<StandardSwizzleTableUse> *swizzle_capture_owner_ = nullptr;
  std::unordered_set<SwizzleCaptureKey, SwizzleCaptureKeyHash> swizzle_capture_keys_;

  static SwizzleCaptureKey SwizzleUseKey(const StandardSwizzleTableUse &use) {
    return {use.rootsig, use.heap, use.parameter == ~0u ? 0 : use.base, use.parameter};
  }

  void CaptureStandardSwizzleTables(std::unique_ptr<std::vector<StandardSwizzleTableUse>> &uses,
                                    MTLD3D12RootSignature *rootsig, const uint64_t *root_arguments, bool atomic_phase,
                                    const MTLD3D12PipelineState *pipeline = nullptr) {
    if (!rootsig || !descriptor_heaps_[0])
      return;
    owned_graphics::Trace("descriptor_table_capture", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"atomic\":", atomic_phase);
    auto *heap = static_cast<MTLD3D12DescriptorHeap *>(descriptor_heaps_[0]);
    auto bind_keys = [&] {
      if (!uses)
        uses = std::make_unique<std::vector<StandardSwizzleTableUse>>();
      if (swizzle_capture_owner_ == uses.get())
        return;
      swizzle_capture_owner_ = nullptr;
      swizzle_capture_keys_.clear();
      for (const auto &use : *uses)
        swizzle_capture_keys_.insert(SwizzleUseKey(use));
      swizzle_capture_owner_ = uses.get();
    };
    // Publishes the key before the use so a failed vector growth leaves no key without its use.
    auto append = [&](StandardSwizzleTableUse use) {
      const auto key = SwizzleUseKey(use);
      auto [entry, inserted] = swizzle_capture_keys_.insert(key);
      if (!inserted)
        return false;
      try {
        uses->push_back(std::move(use));
      } catch (...) {
        swizzle_capture_keys_.erase(entry);
        throw;
      }
      rootsig->AddRefPrivate();
      heap->AddRefPrivate();
      return true;
    };
    if (rootsig->Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) {
      bind_keys();
      if (!swizzle_capture_keys_.contains({rootsig, heap, 0, ~0u})) {
        D3D12_GPU_DESCRIPTOR_HANDLE start{};
        heap->GetGPUDescriptorHandleForHeapStart(&start);
        append({rootsig, heap, start.ptr, ~0u});
      }
    }
    for (uint32_t parameter = 0; parameter < rootsig->ParameterSlots; ++parameter) {
      const auto ranges = rootsig->GetTableRanges(parameter);
      if (std::none_of(ranges.begin(), ranges.end(), [atomic_phase](const auto &range) {
            return range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
                   range.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ||
                   (atomic_phase && range.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV);
          }))
        continue;
      const uint64_t base = root_arguments[rootsig->SlotQwordOffsets[parameter]];
      if (!base)
        continue;
      bind_keys();
      StandardSwizzleTableUse use{rootsig, heap, base, parameter};
      if (atomic_phase && pipeline) {
        use.shader_bindings = pipeline->resource_bindings;
        use.restrict_to_shader = true;
      }
      append(std::move(use));
    }
  }

  /* Per-recording cache of LookupBufferByVA results. Every filled entry's allocation is already retained by
   * the current recording; an entry is valid only while the device VA map generation is unchanged. */
  struct RecordedVALookup {
    uint64_t begin = 0, end = 0, generation = 0;
    BufferAllocation *allocation = nullptr;
  };
  std::array<RecordedVALookup, 8> recorded_va_lookups_{};

  /* Resolve a buffer VA bound in this recording and retain its allocation for the recording. May throw
   * std::bad_alloc while retaining. */
  BufferAllocation *LookupRecordedBufferByVA(uint64_t VA) {
    if (!VA) return nullptr;
    auto &entry = recorded_va_lookups_[(VA >> 16) & (recorded_va_lookups_.size() - 1)];
    if (entry.allocation && VA >= entry.begin && VA < entry.end &&
        entry.generation == device_->buffer_va_generation.load(std::memory_order_acquire))
      return entry.allocation;
    uint64_t offset = 0, begin = 0, end = 0, generation = 0;
    auto *allocation = device_->LookupBufferRangeByVA(VA, &offset, &begin, &end, &generation);
    if (!allocation) return nullptr;
    if (allocator_->recording_head_->root_allocation_set.insert(allocation).second) {
      try {
        allocator_->recording_head_->root_allocations.emplace_back(allocation);
      } catch (...) {
        allocator_->recording_head_->root_allocation_set.erase(allocation);
        throw;
      }
    }
    entry = {begin, end, generation, allocation};
    return allocation;
  }

  /*
   * CaptureSubmissionAccess work that depends only on the immutable (root signature, PSO bindings, stages)
   * triple, in the original parameter/binding order, plus the last successful capture into the current
   * encoder. A repeated capture with identical root-view/table qwords, heap and VA map adds nothing (every
   * use is deduplicated), so it is skipped. Descriptor contents are never cached: tables are resolved at
   * Execute time from the recorded base.
   */
  struct SubmissionCapturePlan {
    enum class Step : uint8_t { View, Opaque, Incomplete, ParameterEnd, Table };
    struct Entry {
      Step step;
      uint32_t parameter;
      bool writes;
      uint64_t stages;
    };
    Com<MTLD3D12RootSignature, false> signature;
    Com<MTLD3D12GraphicsPipelineState, false> graphics;
    Com<MTLD3D12ComputePipelineState, false> compute;
    uint64_t allowed_stages = 0;
    std::vector<Entry> entries;
    std::vector<uint32_t> argument_offsets;
    uint64_t captured_encoder = 0, captured_generation = 0;
    ID3D12DescriptorHeap *captured_heap = nullptr;
    std::array<uint64_t, 64> captured_arguments{};
  };
  // Per kind (graphics, compute): a few recently bound pipelines, replaced round-robin.
  std::array<std::array<SubmissionCapturePlan, 4>, 2> submission_capture_plans_;
  std::array<uint8_t, 2> submission_capture_plan_next_{};

  /* Depth-stencil and rasterizer state of the bound graphics PSO (both draw paths). */
  template <bool CheckOnly = false>
  bool CaptureSubmissionAccess(std::vector<RecordedBufferAccess> &buffers,
      std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables, MTLD3D12RootSignature *signature,
      const uint64_t *arguments, const MTLD3D12PipelineState &pipeline, uint64_t allowed_stages, bool &budget_exceeded,
      bool &root_srv_opaque, bool current_encoder = false) {
    if (!signature || (signature->Flags & (D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                           D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED))) return false;
    if (pipeline.resource_bindings.size() > kRecordedSubmissionBufferBudget) { budget_exceeded = true; return false; }
    small_vector<RecordedBufferAccess, kRecordedSubmissionBufferBudget> pending_buffers;
    size_t pending_tables = 0;
    try {
      const bool compute = allowed_stages & WMTMetal4StageDispatch;
      const MTLD3D12PipelineState *bound = compute ? static_cast<const MTLD3D12PipelineState *>(pso_compute_.ptr())
                                                   : static_cast<const MTLD3D12PipelineState *>(pso_graphics_.ptr());
      // Only the bound pipeline is planned; the plan's references keep its keys from being reused.
      const bool planned = bound == &pipeline;
      auto &plans = submission_capture_plans_[compute];
      auto matches = [&](const SubmissionCapturePlan &plan) {
        return plan.signature.ptr() == signature && plan.allowed_stages == allowed_stages &&
            (compute ? static_cast<const MTLD3D12PipelineState *>(plan.compute.ptr())
                     : static_cast<const MTLD3D12PipelineState *>(plan.graphics.ptr())) == bound;
      };
      size_t slot = std::find_if(plans.begin(), plans.end(), matches) - plans.begin();
      if (slot == plans.size()) slot = planned ? submission_capture_plan_next_[compute]++ % plans.size() : 0;
      auto &plan = plans[slot];
      if (planned && !matches(plan)) {
        plan.signature = nullptr; plan.graphics = nullptr; plan.compute = nullptr;
        plan.entries.clear(); plan.argument_offsets.clear(); plan.captured_encoder = 0;
        using Step = SubmissionCapturePlan::Step;
        for (uint32_t parameter = 0; parameter < signature->ParameterSlots; ++parameter) {
          const auto type = signature->ParameterTypes[parameter];
          if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) continue;
          if (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            const auto *range = signature->GetRootDescriptorRange(parameter);
            bool referenced = false;
            for (const auto &binding : pipeline.resource_bindings) {
              uint64_t stages = allowed_stages;
              if (!(allowed_stages & WMTMetal4StageDispatch) && !(binding.ShaderVisibilityMask & 1u)) {
                stages = (binding.ShaderVisibilityMask & (1u << D3D12_SHADER_VISIBILITY_VERTEX) ? WMTMetal4StageVertex : 0) |
                    (binding.ShaderVisibilityMask & (1u << D3D12_SHADER_VISIBILITY_PIXEL) ? WMTMetal4StageFragment : 0);
              }
              if (range) signature->ForEachUsedDescriptorRange({range, 1}, {&binding, 1},
                  [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE) {
                // A RaytracingAccelerationStructure SRV reads the structure and
                // every BLAS it references, not the bound range. Never guess.
                if (type == D3D12_ROOT_PARAMETER_TYPE_SRV &&
                    (binding.Flags & MTL_SM50_SHADER_ARGUMENT_ACCELERATION_STRUCTURE)) {
                  plan.entries.push_back({Step::Opaque, parameter, false, 0});
                } else if ((binding.Type != SM50BindingType::ConstantBuffer &&
                            (!(binding.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) ||
                             (binding.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER))) || !stages) {
                  plan.entries.push_back({Step::Incomplete, parameter, false, 0});
                } else {
                  plan.entries.push_back({Step::View, parameter, type == D3D12_ROOT_PARAMETER_TYPE_UAV &&
                      (binding.Flags & MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS) != 0, stages});
                  referenced = true;
                }
              });
            }
            plan.entries.push_back({Step::ParameterEnd, parameter, false, 0});
            if (referenced) plan.argument_offsets.push_back(signature->SlotQwordOffsets[parameter]);
            continue;
          }
          bool used = false;
          signature->ForEachUsedDescriptorRange(signature->GetTableRanges(parameter), pipeline.resource_bindings,
              [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE kind) {
                used |= kind != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
              });
          if (!used) continue;
          plan.entries.push_back({Step::Table, parameter, false, 0});
          plan.argument_offsets.push_back(signature->SlotQwordOffsets[parameter]);
        }
        if (plan.argument_offsets.size() > plan.captured_arguments.size()) {
          plan.entries.clear(); plan.argument_offsets.clear();
        } else {
          plan.signature = signature;
          plan.allowed_stages = allowed_stages;
          if (compute) plan.compute = pso_compute_.ptr();
          else plan.graphics = pso_graphics_.ptr();
        }
      }
      if (!planned || !plan.signature)
        return CaptureSubmissionAccessUnplanned<CheckOnly>(buffers, tables, signature, arguments, pipeline,
            allowed_stages, budget_exceeded, root_srv_opaque);

      const uint64_t generation = device_->buffer_va_generation.load(std::memory_order_acquire);
      const uint64_t encoder = current_encoder && allocator_->encoder_current ? allocator_->encoder_current->id : 0;
      auto same_arguments = [&] {
        for (size_t i = 0; i < plan.argument_offsets.size(); ++i)
          if (plan.captured_arguments[i] != arguments[plan.argument_offsets[i]]) return false;
        return true;
      };
      if (encoder && plan.captured_encoder == encoder && plan.captured_generation == generation &&
          plan.captured_heap == descriptor_heaps_[0] && same_arguments())
        return true;

      bool complete = true;
      using Step = SubmissionCapturePlan::Step;
      for (const auto &entry : plan.entries) {
        switch (entry.step) {
        case Step::Opaque: root_srv_opaque = true; complete = false; break;
        case Step::Incomplete: complete = false; break;
        case Step::ParameterEnd: if (!complete) return false; break;
        case Step::View: {
          auto *allocation = LookupRecordedBufferByVA(arguments[signature->SlotQwordOffsets[entry.parameter]]);
          if (!allocation) { complete = false; break; }
          const auto address = allocation->gpuAddress(); // unknown root-view length covers this whole backing
          const bool writes = entry.writes;
          const uint64_t stages = entry.stages;
          if (std::none_of(buffers.begin(), buffers.end(), [&](const auto &use) {
            return use.address == address && use.length == UINT64_MAX && use.writes == writes && use.stages == stages;
          }) && std::none_of(pending_buffers.begin(), pending_buffers.end(), [&](const auto &use) {
            return use.address == address && use.length == UINT64_MAX && use.writes == writes && use.stages == stages;
          })) {
            if (buffers.size() + pending_buffers.size() >= kRecordedSubmissionBufferBudget) {
              budget_exceeded = true; complete = false; break;
            }
            if constexpr (CheckOnly) pending_buffers.push_back({address, UINT64_MAX, writes, stages});
            else buffers.push_back({address, UINT64_MAX, writes, stages});
          }
          break;
        }
        case Step::Table:
          if (!CaptureSubmissionTable<CheckOnly>(tables, pending_tables, signature, arguments, pipeline,
                                                 entry.parameter, budget_exceeded))
            return false;
          break;
        }
      }
      if constexpr (!CheckOnly) {
        if (encoder) {
          plan.captured_encoder = encoder;
          plan.captured_generation = generation;
          plan.captured_heap = descriptor_heaps_[0];
          for (size_t i = 0; i < plan.argument_offsets.size(); ++i)
            plan.captured_arguments[i] = arguments[plan.argument_offsets[i]];
        }
      }
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return false; }
    return true;
  }

  template <bool CheckOnly>
  bool CaptureSubmissionTable(std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables, size_t &pending_tables,
      MTLD3D12RootSignature *signature, const uint64_t *arguments, const MTLD3D12PipelineState &pipeline,
      uint32_t parameter, bool &budget_exceeded) {
    if (!descriptor_heaps_[0] || !arguments[signature->SlotQwordOffsets[parameter]]) return false;
    if (tables && std::any_of(tables->begin(), tables->end(), [&](const auto &use) {
      return use.rootsig == signature && use.heap == descriptor_heaps_[0] && use.parameter == parameter &&
          use.base == arguments[signature->SlotQwordOffsets[parameter]] &&
          use.submission_bindings.size() == pipeline.resource_bindings.size() &&
          !std::memcmp(use.submission_bindings.data(), pipeline.resource_bindings.data(),
              pipeline.resource_bindings.size() * sizeof(MTL_SM50_SHADER_ARGUMENT));
    })) return true;
    if ((tables ? tables->size() : 0) + pending_tables >= kRecordedSubmissionBufferBudget) {
      budget_exceeded = true; return false;
    }
    if constexpr (CheckOnly) { ++pending_tables; return true; }
    if (!tables) tables = std::make_unique<std::vector<StandardSwizzleTableUse>>();
    StandardSwizzleTableUse use{signature, static_cast<MTLD3D12DescriptorHeap *>(descriptor_heaps_[0]),
        arguments[signature->SlotQwordOffsets[parameter]], parameter};
    use.submission_bindings = pipeline.resource_bindings;
    use.shader_bindings = use.submission_bindings;
    use.restrict_to_shader = true;
    tables->push_back(std::move(use));
    signature->AddRefPrivate();
    tables->back().heap->AddRefPrivate();
    return true;
  }

  /* Capture for a pipeline that is not the bound one (no plan is kept for it). */
  template <bool CheckOnly>
  bool CaptureSubmissionAccessUnplanned(std::vector<RecordedBufferAccess> &buffers,
      std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables, MTLD3D12RootSignature *signature,
      const uint64_t *arguments, const MTLD3D12PipelineState &pipeline, uint64_t allowed_stages, bool &budget_exceeded,
      bool &root_srv_opaque) {
    small_vector<RecordedBufferAccess, kRecordedSubmissionBufferBudget> pending_buffers;
    size_t pending_tables = 0;
    for (uint32_t parameter = 0; parameter < signature->ParameterSlots; ++parameter) {
      const auto type = signature->ParameterTypes[parameter];
      if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) continue;
      if (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const auto *range = signature->GetRootDescriptorRange(parameter);
        bool complete = true;
        for (const auto &binding : pipeline.resource_bindings) {
          uint64_t stages = allowed_stages;
          if (!(allowed_stages & WMTMetal4StageDispatch) && !(binding.ShaderVisibilityMask & 1u)) {
            stages = (binding.ShaderVisibilityMask & (1u << D3D12_SHADER_VISIBILITY_VERTEX) ? WMTMetal4StageVertex : 0) |
                (binding.ShaderVisibilityMask & (1u << D3D12_SHADER_VISIBILITY_PIXEL) ? WMTMetal4StageFragment : 0);
          }
          if (range) signature->ForEachUsedDescriptorRange({range, 1}, {&binding, 1},
              [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE) {
            if (type == D3D12_ROOT_PARAMETER_TYPE_SRV &&
                (binding.Flags & MTL_SM50_SHADER_ARGUMENT_ACCELERATION_STRUCTURE)) {
              root_srv_opaque = true; complete = false; return;
            }
            if (binding.Type != SM50BindingType::ConstantBuffer &&
                (!(binding.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) ||
                 (binding.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER))) { complete = false; return; }
            const auto bound_address = arguments[signature->SlotQwordOffsets[parameter]];
            uint64_t offset = 0;
            auto *allocation = bound_address ? device_->LookupBufferByVA(bound_address, &offset) : nullptr;
            if (!allocation || !stages) { complete = false; return; }
            const auto address = allocation->gpuAddress(); // unknown root-view length covers this whole backing
            const bool writes = type == D3D12_ROOT_PARAMETER_TYPE_UAV &&
                (binding.Flags & MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS);
            if (std::none_of(buffers.begin(), buffers.end(), [&](const auto &use) {
              return use.address == address && use.length == UINT64_MAX && use.writes == writes && use.stages == stages;
            }) && std::none_of(pending_buffers.begin(), pending_buffers.end(), [&](const auto &use) {
              return use.address == address && use.length == UINT64_MAX && use.writes == writes && use.stages == stages;
            })) {
              if (buffers.size() + pending_buffers.size() >= kRecordedSubmissionBufferBudget) {
                budget_exceeded = true; complete = false; return;
              }
              if constexpr (CheckOnly) pending_buffers.push_back({address, UINT64_MAX, writes, stages});
              else buffers.push_back({address, UINT64_MAX, writes, stages});
            }
          });
        }
        if (!complete) return false;
        continue;
      }
      bool used = false;
      signature->ForEachUsedDescriptorRange(signature->GetTableRanges(parameter), pipeline.resource_bindings,
          [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE kind) {
            used |= kind != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
          });
      if (!used) continue;
      if (!CaptureSubmissionTable<CheckOnly>(tables, pending_tables, signature, arguments, pipeline, parameter,
                                             budget_exceeded))
        return false;
    }
    return true;
  }

  void EncodeGraphicsFixedState(bool capture_only) {
    auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    // Encoder state persists within one chain, and every consumer replays a chain from its head, so a
    // repeat of the state already emitted into this encoder is dropped.
    const WMT::DepthStencilState depth_stencil_state =
        capture_only ? device_->default_depth_stencil_state :
        pso_graphics_->owned_depth_bounds && !pso_graphics_->emulated_depth_bounds &&
        render->depth.attachment && depth_bounds_min_ > depth_bounds_max_
            ? WMT::DepthStencilState(pso_graphics_->depth_bounds_reject_state) :
        pso_graphics_->GetDepthStencilState(render->dsv_planar_flags, render->dsv_readonly_flags);
    if (!render->fixed_depth_stencil_state || render->fixed_depth_stencil_state != depth_stencil_state.handle) {
      auto &cmd_setdsso = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
      cmd_setdsso.type = WMTRenderCommandSetDepthStencilState;
      cmd_setdsso.depth_stencil_state = depth_stencil_state;
      render->fixed_depth_stencil_state = depth_stencil_state.handle;
    }
    const bool bounds_enabled = !capture_only && pso_graphics_->owned_depth_bounds &&
                                !pso_graphics_->emulated_depth_bounds &&
                                render->depth.attachment && depth_bounds_min_ <= depth_bounds_max_;
    if (bounds_enabled || render->depth_bounds_active) {
      auto &bounds = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthbounds>();
      bounds.type = WMTRenderCommandSetDepthBounds;
      bounds.min_depth = bounds_enabled ? std::clamp(depth_bounds_min_, 0.0f, 1.0f) : 0.0f;
      bounds.max_depth = bounds_enabled ? std::clamp(depth_bounds_max_, 0.0f, 1.0f) : 1.0f;
      render->depth_bounds_active = bounds_enabled;
    }

    // Bitwise float comparison: only an identical tuple is elided (NaN and -0 included).
    auto same_float = [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); };
    if (render->fixed_raster_valid && render->fixed_cull_mode == pso_graphics_->cull_mode &&
        render->fixed_depth_clip_mode == pso_graphics_->depth_clip_mode &&
        render->fixed_fill_mode == pso_graphics_->fill_mode && render->fixed_winding == pso_graphics_->winding &&
        same_float(render->fixed_depth_bias, depth_bias_) &&
        same_float(render->fixed_depth_bias_clamp, depth_bias_clamp_) &&
        same_float(render->fixed_slope_scale, slope_scaled_depth_bias_))
      return;
    auto &cmd_setrs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
    cmd_setrs.type = WMTRenderCommandSetRasterizerState;
    cmd_setrs.cull_mode = pso_graphics_->cull_mode;
    cmd_setrs.depth_clip_mode = pso_graphics_->depth_clip_mode;
    cmd_setrs.fill_mode = pso_graphics_->fill_mode;
    cmd_setrs.depth_bias = depth_bias_;
    cmd_setrs.depth_bias_clamp = depth_bias_clamp_;
    cmd_setrs.scole_scale = slope_scaled_depth_bias_;
    cmd_setrs.winding = pso_graphics_->winding;
    render->fixed_raster_valid = true;
    render->fixed_cull_mode = cmd_setrs.cull_mode;
    render->fixed_depth_clip_mode = cmd_setrs.depth_clip_mode;
    render->fixed_fill_mode = cmd_setrs.fill_mode;
    render->fixed_winding = cmd_setrs.winding;
    render->fixed_depth_bias = depth_bias_;
    render->fixed_depth_bias_clamp = depth_bias_clamp_;
    render->fixed_slope_scale = slope_scaled_depth_bias_;
  }

  bool NativeMSAABindingsSafe(RenderEncoderData *capture = nullptr) {
    auto *signature = rootsig_graphics_.ptr();
    if (!signature || (signature->Flags & (D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                          D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED)) ||
        pso_graphics_->resource_bindings.size() > kRecordedSubmissionBufferBudget) return false;
    auto *heap = static_cast<MTLD3D12DescriptorHeap *>(descriptor_heaps_[0]);
    uint64_t descriptor_count = 0;
    bool safe = true;
    try {
      for (uint32_t parameter = 0; safe && parameter < signature->ParameterSlots; ++parameter) {
        const auto type = signature->ParameterTypes[parameter];
        if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) continue;
        const auto *root_range = signature->GetRootDescriptorRange(parameter);
        const auto ranges = type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? signature->GetTableRanges(parameter)
            : std::span<const MTLD3D12RootSignature::TableRange>{root_range, root_range ? 1u : 0u};
        if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
          for (const auto &range : ranges) {
            if (!(range.descriptor_flags & D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE)) continue;
            signature->ForEachUsedDescriptorRange(
                std::span<const MTLD3D12RootSignature::TableRange>{&range, 1}, pso_graphics_->resource_bindings,
                [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE kind) {
                  if (kind != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) safe = false;
                });
            if (!safe) break;
          }
          if (!safe) break;
        }
        signature->ForEachUsedDescriptorRange(ranges, pso_graphics_->resource_bindings,
            [&](uint64_t offset, uint64_t count, D3D12_DESCRIPTOR_RANGE_TYPE kind) {
          if (!safe || kind == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) return;
          // Root SRV reflection cannot exclude inline RayQuery's transitive AS reads.
          if (kind == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ||
              (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && type != D3D12_ROOT_PARAMETER_TYPE_CBV)) {
            safe = false; return;
          }
          if (type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
            ShaderVisibleDescriptorCPUStorage descriptor{};
            descriptor.type = ShaderVisibleDescriptorType::ConstantBuffer;
            descriptor.ConstantBuffer = {rootarg_graphics_staging_[signature->SlotQwordOffsets[parameter]], 1};
            safe = NativeMSAADescriptorSafe(device_, descriptor);
            return;
          }
          D3D12_DESCRIPTOR_HEAP_DESC desc{}; D3D12_GPU_DESCRIPTOR_HANDLE start{};
          if (!heap) { safe = false; return; }
          heap->GetDesc(&desc); heap->GetGPUDescriptorHandleForHeapStart(&start);
          const auto address = rootarg_graphics_staging_[signature->SlotQwordOffsets[parameter]];
          if (!start.ptr || address < start.ptr || (address - start.ptr) % 32) { safe = false; return; }
          const auto base = (address - start.ptr) / 32;
          if (base >= desc.NumDescriptors || offset >= desc.NumDescriptors - base ||
              count > desc.NumDescriptors - base - offset || count > kRecordedSubmissionBufferBudget - descriptor_count) {
            safe = false; return;
          }
          descriptor_count += count;
          for (uint64_t i = 0; safe && i < count; ++i) {
            const auto &descriptor = heap->GetDescriptor(base + offset + i);
            safe = NativeMSAADescriptorSafe(device_, descriptor);
            if (!safe || !capture || descriptor.type == ShaderVisibleDescriptorType::Null) continue;
            const bool texture = descriptor.type == ShaderVisibleDescriptorType::SRVTexture;
            const bool cbv = descriptor.type == ShaderVisibleDescriptorType::ConstantBuffer;
            const bool texel = descriptor.type == ShaderVisibleDescriptorType::SRVTexelBuffer;
            if (!cbv) {
              const void *backing = texture ? static_cast<const void *>(descriptor.SRVTexture.texture)
                  : texel ? static_cast<const void *>(descriptor.SRVTexelBuffer.buffer)
                          : static_cast<const void *>(descriptor.SRVBuffer.buffer);
              auto owner = device_->LookupDescriptorResource(backing, descriptor.owner_id);
              if (!owner) { safe = false; break; }
              if (std::none_of(capture->native_msaa_resources.begin(), capture->native_msaa_resources.end(),
                              [&](const auto &resource) { return resource.ptr() == static_cast<ID3D12Resource *>(owner.ptr()); }))
                capture->native_msaa_resources.push_back(static_cast<ID3D12Resource *>(owner.ptr()));
            }
            if (texture) {
              auto *allocation = descriptor.SRVTexture.texture->current();
              if (std::none_of(capture->native_msaa_texture_allocations.begin(), capture->native_msaa_texture_allocations.end(),
                              [&](const auto &retained) { return retained.ptr() == allocation; }))
                capture->native_msaa_texture_allocations.emplace_back(allocation);
            } else {
              uint64_t ignored = 0;
              auto *allocation = cbv ? device_->LookupBufferByVA(descriptor.ConstantBuffer.address, &ignored)
                  : texel ? descriptor.SRVTexelBuffer.buffer->current() : descriptor.SRVBuffer.buffer->current();
              if (allocator_->recording_head_->root_allocation_set.insert(allocation).second)
                allocator_->recording_head_->root_allocations.emplace_back(allocation);
            }
          }
        });
      }
      if (capture && safe)
        safe = CaptureSubmissionAccess(capture->submission_buffers, capture->submission_tables, signature,
            rootarg_graphics_staging_, *pso_graphics_.ptr(), WMTMetal4StageVertex | WMTMetal4StageFragment,
            capture->submission_snapshot_budget, capture->submission_root_srv_opaque);
    } catch (const std::bad_alloc &) { if (capture) RecordError(E_OUTOFMEMORY); return false; }
    return safe;
  }

  bool CanUseNativeMSAA() {
    if (!pso_graphics_ || !pso_graphics_->native_msaa_pso || render_pass_internal_ || predication_enabled_ ||
        !active_visibility_queries_.empty() || !active_pipeline_statistics_.empty() || !active_so_queries_.empty() ||
        sample_positions_.count || shading_rate_image_ || CoarseShadingRateState() ||
        pso_graphics_->view_instancing.count || pso_graphics_->view_location_offsets ||
        (topology_ != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology_ != D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP))
      return false;
    if (!NativeMSAABindingsSafe()) return false;
    const auto count = pso_graphics_->EffectiveRasterSampleCount();
    bool attachment = false;
    for (unsigned i = 0; i <= num_rtvs; ++i) {
      const auto handle = i == num_rtvs ? dsv : rtvs[i];
      if (!handle.ptr) continue;
      attachment = true;
      auto [heap, index] = GetRenderTargetHeap(device_, handle);
      const auto desc = heap->GetRenderTarget(index);
      if (!desc.Texture || desc.BufferResource || desc.Flags || desc.DepthPlane || desc.RenderTargetArrayLength > 1)
        return false;
      const auto *v = &desc.Texture->view(desc.View);
      const auto *a = v->allocation;
      if (v->castBlockSize || v->msaaSampleAddress || v->reservedCast || v->reservedVolumeRender ||
          v->reservedVolumeAtlasCast || a->canonicalHeap || a->flags().test(TextureAllocationFlag::Shared) ||
          a->flags().test(TextureAllocationFlag::PlacementSparse) ||
          a->descriptor->sampleCount() != count || a->descriptor->textureType() != WMTTextureType2DMultisample ||
          v->key.mip_start || v->key.array_start || v->key.array_end != 1 ||
          device_->LookupStandardSwizzleTexture(a->descriptor)) return false;
    }
    if (!attachment) return false;
    std::vector<RecordedBufferAccess> buffers;
    std::unique_ptr<std::vector<StandardSwizzleTableUse>> tables;
    bool budget = false, opaque = false;
    return CaptureSubmissionAccess<true>(buffers, tables, rootsig_graphics_.ptr(), rootarg_graphics_staging_,
        *pso_graphics_.ptr(), WMTMetal4StageVertex | WMTMetal4StageFragment, budget, opaque);
  }

  bool CanRetainVisibilityIntervals() {
    return pso_graphics_ && !render_pass_internal_ && !RequiresMSAAExecution() &&
        pso_graphics_->mesh_kind == MeshPipelineKind::None && !pso_graphics_->uses_atomic64 &&
        !pso_graphics_->stream_output.active && active_pipeline_statistics_.empty() && active_so_queries_.empty() &&
        !predication_enabled_ && !pso_graphics_->view_instancing.count && !pso_graphics_->view_location_offsets &&
        !sample_positions_.count && !shading_rate_image_ && !pso_graphics_->needs_rov_buffer_marker;
  }

  bool RecordVisibilityInterval(RenderEncoderData *render) {
    if (!render->visibility_interval_dirty) return true;
    try {
      const auto offset = render->visibility_interval_count * sizeof(uint64_t);
      if (!active_visibility_queries_.empty()) {
        if (render->visibility_interval_count == UINT64_MAX / sizeof(uint64_t)) {
          RecordError(E_OUTOFMEMORY); return false;
        }
        ++render->visibility_interval_count;
        for (const auto &query : active_visibility_queries_)
          render->visibility_targets.push_back({query.destination, query.accumulate_pipeline, query.heap_id,
                                                query.index, offset, false});
      }
      auto &mode = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
      mode.type = WMTRenderCommandSetVisibilityMode;
      mode.mode = active_visibility_queries_.empty() ? WMTVisibilityResultModeDisabled : WMTVisibilityResultModeCounting;
      mode.offset = active_visibility_queries_.empty() ? 0 : offset;
      render->visibility_interval_dirty = false;
      return true;
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return false; }
  }

  /* d3d12.asyncPipelineCompile: bound attachments as recurrence keys. False when any bound target is
   * not an owned texture view, so buffer or unknown targets never become omittable. */
  bool AsyncTargetKeys(small_vector<owned_graphics::RenderTargetHistory::Key, 9> &keys) {
    auto add = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
      auto [heap, index] = GetRenderTargetHeap(device_, handle);
      const auto desc = heap->GetRenderTarget(index);
      if (!desc.Texture || desc.BufferResource || !desc.Owner)
        return false;
      keys.push_back({desc.Owner, uint64_t(desc.View), desc.DepthPlane, desc.RenderTargetArrayLength});
      return true;
    };
    for (unsigned i = 0; i < num_rtvs; ++i)
      if (rtvs[i].ptr && !add(rtvs[i]))
        return false;
    if (dsv.ptr && !add(dsv))
      return false;
    return !keys.empty();
  }

  /* A direct draw whose PSO is still linking is dropped only when it cannot have side effects
   * (no UAV in any stage, no directly indexed heap, no query/predication/stream output) and every
   * bound target was drawn by emitted draws in the last kRecurringEpochs presentation epochs.
   * States whose PreDraw validation can record an error or a resource-state event (sample
   * positions, shading-rate image, forced samples with a DSV) are never omitted, so omission cannot
   * hide a Close() failure. Dirty state is untouched, so the next emitted draw binds the completed
   * PSO normally. */
  bool OmitPendingDraw() {
    if (!pso_graphics_ || !pso_graphics_->NativePipelinePending() || !pso_graphics_->async_omittable ||
        predication_enabled_ || !active_visibility_queries_.empty() || !active_so_queries_.empty() ||
        !active_pipeline_statistics_.empty() || !rootsig_graphics_ ||
        sample_positions_.count || shading_rate_image_ || (pso_graphics_->forced_sample_count && dsv.ptr) ||
        (rootsig_graphics_->Flags & (D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                     D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED)))
      return false;
    small_vector<owned_graphics::RenderTargetHistory::Key, 9> keys;
    if (!AsyncTargetKeys(keys))
      return false;
    auto &async = device_->async_pipelines;
    const auto epoch = async.Epoch();
    for (const auto &key : keys)
      if (!async.Targets().Recurring(key, epoch))
        return false;
    owned_graphics::Trace("async_pipeline_omit", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"epoch\":", epoch);
    return true;
  }

  /* Only emitted, nonempty direct draws extend recurrence; omitted draws never do. Later draws on the
   * same bound attachments in the same epoch are no-ops for the history, so they skip descriptor
   * resolution and the shared history lock. */
  void RecordAsyncTargets() {
    auto &async = device_->async_pipelines;
    if (!async.Enabled())
      return;
    const auto epoch = async.Epoch();
    if (async_targets_epoch_ == epoch)
      return;
    async_targets_epoch_ = epoch;
    small_vector<owned_graphics::RenderTargetHistory::Key, 9> keys;
    if (!AsyncTargetKeys(keys))
      return;
    for (const auto &key : keys)
      async.Targets().Record(key, epoch);
  }

  DrawCallStatus
  PreDraw(bool SkipResourceBinding = false, bool IndexPull = false, uint32_t MeshKey = ~0u, bool NativeMSAA = false) {
    if (!pso_graphics_)
      return DrawCallStatus::Invalid;
    // Every non-omitted draw path, direct or indirect, joins a deferred native link here.
    if (!pso_graphics_->WaitNativePipeline()) {
      RecordError(E_FAIL);
      return DrawCallStatus::Invalid;
    }
    if (pso_graphics_->uses_atomic64)
      allocator_->InvalidateCurrentPass();
    if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render &&
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->visibility_intervals &&
        !CanRetainVisibilityIntervals()) allocator_->InvalidateCurrentPass();
    if (sample_positions_.count && sample_positions_.count != pso_graphics_->EffectiveRasterSampleCount()) {
      RecordError();
      return DrawCallStatus::Invalid;
    }
    if (shading_rate_image_ &&
        !RecordImplicitUse(shading_rate_image_.ptr(), 0, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE))
      return DrawCallStatus::Invalid;
    const bool mesh = pso_graphics_->mesh_kind != MeshPipelineKind::None;
    if (mesh != (MeshKey != ~0u)) {
      // A GS/HS/DS PSO needs its mesh draw path (and vice versa).
      RecordError(E_INVALIDARG);
      return DrawCallStatus::Invalid;
    }
    if (mesh && mesh_variant_bound_ != MeshKey) {
      // Object/mesh bindings are only emitted for mesh pipelines, so rebind them on entry.
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::VertexBuffer);
      dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      mesh_variant_bound_ = MeshKey;
    } else if (!mesh) {
      mesh_variant_bound_ = ~0u;
    }
    if (pso_graphics_->forced_sample_count && dsv.ptr) {
      RecordError(E_INVALIDARG);
      return DrawCallStatus::Invalid;
    }
    const bool statistics = !active_pipeline_statistics_.empty();
    if (index_pull_pipeline_bound_ != IndexPull) {
      dirty_state_.set(DirtyState::GraphicsPipelineState);
      index_pull_pipeline_bound_ = IndexPull;
    }
    // ForcedSampleCount 16 runs only through the MSAA capture path; its native passes are 8x.
    const uint32_t raster_samples = std::min(pso_graphics_->EffectiveRasterSampleCount(), 8u);
    const bool center_quality = pso_graphics_->owned_sample_desc.Quality == 0xfffffffeu;
    const bool center_sample_pattern = center_quality && !sample_positions_.count;
    auto effective_positions = sample_positions_;
    if (NativeMSAA && !center_sample_pattern) {
      static constexpr D3D12_SAMPLE_POSITION standard2[] = {{4,4}, {-4,-4}};
      static constexpr D3D12_SAMPLE_POSITION standard4[] = {{-2,-6}, {6,-2}, {-6,2}, {2,6}};
      static constexpr D3D12_SAMPLE_POSITION standard8[] = {{1,-3}, {-1,3}, {5,1}, {-3,-5},
                                                           {-5,5}, {-7,-1}, {3,7}, {7,-7}};
      const auto *positions = raster_samples == 2 ? standard2 : raster_samples == 4 ? standard4 : standard8;
      effective_positions.count = raster_samples;
      for (unsigned i = 0; i < raster_samples; ++i) {
        effective_positions.positions[i][0] = positions[i].X;
        effective_positions.positions[i][1] = positions[i].Y;
      }
    }
    if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render) {
      const auto *render = static_cast<const RenderEncoderData *>(allocator_->encoder_current);
      // An attachmentless pass takes its sample count from the PSO. Metal
      // cannot change that count while the render encoder remains open.
      if ((render->default_raster_sample_count && render->default_raster_sample_count != raster_samples) ||
          render->center_sample_pattern != center_sample_pattern ||
          render->native_msaa_sample_count != (NativeMSAA ? raster_samples : 0) ||
          std::memcmp(&render->sample_positions, &effective_positions, sizeof(effective_positions)))
        allocator_->InvalidateCurrentPass();
    }
    if (NativeMSAA && allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      bool budget = false, opaque = false;
      if (!CaptureSubmissionAccess<true>(render->submission_buffers, render->submission_tables,
          rootsig_graphics_.ptr(), rootarg_graphics_staging_, *pso_graphics_.ptr(),
          WMTMetal4StageVertex | WMTMetal4StageFragment, budget, opaque)) {
        if (!budget) { RecordError(E_INVALIDARG); return DrawCallStatus::Invalid; }
        owned_graphics::Trace("native_msaa_budget_rollover", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
            ",\"encoder\":", render->id, ",\"buffer_snapshots\":", render->submission_buffers.size(),
            ",\"table_snapshots\":", render->submission_tables ? render->submission_tables->size() : 0,
            ",\"limit\":", kRecordedSubmissionBufferBudget, ",\"cause\":\"binding_budget\"");
        allocator_->InvalidateCurrentPass();
      }
    }
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) {

      allocator_->EndSchedulingPass();
      auto render = allocator_->AllocatePass<RenderEncoderData>();
      render->type = EncoderType::Render;
      render->submission_group_safe = true;
      render->cmd_head.type = WMTRenderCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
      render->dsv_planar_flags = 0;
      render->dsv_readonly_flags = 0;
      render->render_target_count = num_rtvs;
      render->render_pass_requires_fence = render_pass_active_;
      for (UINT i = 0; i < render_pass_discard_color_count_; ++i)
        if (render_pass_discard_colors_[i]) render->colors[i].store_action = WMTStoreActionDontCare;
      if (render_pass_discard_depth_) render->depth.store_action = WMTStoreActionDontCare;
      if (render_pass_discard_stencil_) render->stencil.store_action = WMTStoreActionDontCare;
      render->center_sample_pattern = center_sample_pattern;
      render->sample_positions = effective_positions;
      render->native_msaa_sample_count = NativeMSAA ? raster_samples : 0;
      render->visibility_intervals = CanRetainVisibilityIntervals();
      render->visibility_interval_dirty = render->visibility_intervals;
      if (!render->visibility_intervals && !active_visibility_queries_.empty()) {
        WMTBufferInfo visibility_info{};
        visibility_info.length = sizeof(uint64_t);
        visibility_info.options = WMTResourceHazardTrackingModeUntracked;
        render->visibility_scratch = device_->GetMTLDevice().newBuffer(visibility_info);
        if (!render->visibility_scratch) {
          RecordError(E_OUTOFMEMORY);
          return DrawCallStatus::Invalid;
        }
        render->visibility_targets.reserve(active_visibility_queries_.size());
        for (const auto &query : active_visibility_queries_)
          render->visibility_targets.push_back(QueryVisibilityTarget{
              query.destination, query.accumulate_pipeline, query.heap_id, query.index});
        auto &visibility = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
        visibility.type = WMTRenderCommandSetVisibilityMode;
        visibility.mode = WMTVisibilityResultModeCounting;
        visibility.offset = 0;
      }

      unsigned render_target_width = 16384, render_target_height = 16384, render_target_array_length = 0;

      unsigned effective_rtvs = 0;
      for (unsigned i = 0; i < num_rtvs; i++) {
        if (!rtvs[i].ptr)
          continue;
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, rtvs[i]);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (AttachmentDesc.Texture && AttachmentDesc.CenterSamplePattern != center_quality) {
          RecordError(E_INVALIDARG);
          return DrawCallStatus::Invalid;
        }
        auto &rt = render->colors[i];
        if (AttachmentDesc.Texture) {
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = AttachmentDesc.DepthPlane;
        } else if (AttachmentDesc.BufferResource) {
          auto *allocation = AttachmentDesc.BufferResource->current();
          const uint32_t staging_width =
              std::min<uint32_t>(AttachmentDesc.Width, D3D12_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH);
          const uint32_t texel_size = MTLGetTexelSize(AttachmentDesc.Format);
          if (!allocation || !staging_width || !texel_size) {
            RecordError(E_INVALIDARG);
            return DrawCallStatus::Invalid;
          }
          WMTTextureInfo info{};
          info.type = WMTTextureType2D;
          info.width = staging_width;
          info.height = 1;
          info.depth = 1;
          info.array_length = 1;
          info.mipmap_level_count = 1;
          info.sample_count = 1;
          info.pixel_format = AttachmentDesc.Format;
          info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
          info.usage = WMTTextureUsageRenderTarget;
          rt.buffer_attachment = device_->GetMTLDevice().newTexture(info);
          if (!rt.buffer_attachment) {
            RecordError(E_OUTOFMEMORY);
            return DrawCallStatus::Invalid;
          }
          rt.buffer_backing = allocation->buffer();
          rt.buffer_offset = allocation->currentSuballocationOffset() + AttachmentDesc.BufferRange.byteOffset;
          rt.buffer_bytes_per_row = staging_width * texel_size;
          rt.buffer_width = staging_width;
          rt.depth_plane = 0;
        } else {
          continue;
        }
        rt.load_action = WMTLoadActionLoad;
        rt.store_action = WMTStoreActionStore;
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
      }
      while (dsv.ptr) {
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, dsv);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (!AttachmentDesc.Texture)
          continue;
        if (AttachmentDesc.CenterSamplePattern != center_quality) {
          RecordError(E_INVALIDARG);
          return DrawCallStatus::Invalid;
        }
        auto dsv_planar_flags = DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
        if (dsv_planar_flags & 1) {
          auto &rt = render->depth;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        if (dsv_planar_flags & 2) {
          auto &rt = render->stencil;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        render->dsv_planar_flags = dsv_planar_flags;
        render->dsv_descriptor_flags = AttachmentDesc.Flags;
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
        break;
      }
      render->render_target_width = render_target_width;
      render->render_target_height = render_target_height;
      render->render_target_array_length = render_target_array_length;
      render->rov_marker_attachmentless = effective_rtvs == 0;
      if (effective_rtvs == 0) {
        render->default_raster_sample_count = raster_samples;
      }

      // Fold while recording, never while executing a replayable command list. Keep the clear
      // object alive so its retained attachment is released by normal allocator destruction.
      if (!render_pass_internal_ && !mesh && !RequiresMSAAExecution() && !pso_graphics_->uses_atomic64 &&
          !pso_graphics_->stream_output.active && active_pipeline_statistics_.empty() &&
          !sample_positions_.count && allocator_->encoder_last->type == EncoderType::Clear) {
        auto *clear = static_cast<ClearEncoderData *>(allocator_->encoder_last);
        if (clear->width == render_target_width && clear->height == render_target_height &&
            clear->array_length == render_target_array_length && !clear->sample_positions.count &&
            !device_->LookupStandardSwizzleTexture(clear->attachment.ptr()->allocation->descriptor)) {
          if (!clear->clear_dsv) {
            for (unsigned i = 0; i < num_rtvs; i++) {
              auto &rt = render->colors[i];
              if (rt.attachment.ptr() != clear->attachment.ptr() || rt.depth_plane != clear->depth_plane ||
                  rt.load_action != WMTLoadActionLoad)
                continue;
              rt.load_action = WMTLoadActionClear;
              rt.clear_color = clear->color;
              clear->folded_into_render = true;
              break;
            }
          } else if ((!(clear->clear_dsv & 1) ||
                      (render->depth.attachment.ptr() == clear->attachment.ptr() &&
                       render->depth.depth_plane == clear->depth_plane &&
                       render->depth.load_action == WMTLoadActionLoad)) &&
                     (!(clear->clear_dsv & 2) ||
                      (render->stencil.attachment.ptr() == clear->attachment.ptr() &&
                       render->stencil.depth_plane == clear->depth_plane &&
                       render->stencil.load_action == WMTLoadActionLoad))) {
            if (clear->clear_dsv & 1) {
              render->depth.load_action = WMTLoadActionClear;
              render->depth.clear_depth = clear->depth_stencil.first;
            }
            if (clear->clear_dsv & 2) {
              render->stencil.load_action = WMTLoadActionClear;
              render->stencil.clear_stencil = clear->depth_stencil.second;
            }
            clear->folded_into_render = true;
          }
          if (clear->folded_into_render)
            owned_graphics::Trace("clear_load_folded", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                                  ",\"clear\":", clear->id, ",\"encoder\":", render->id,
                                  ",\"aspects\":", clear->clear_dsv);
        }
      }

      dirty_state_.set(DirtyState::VertexBuffer, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
      dirty_state_.set(DirtyState::BlendFactor, DirtyState::StencilRef);
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    }

    if (!pso_graphics_)
      return DrawCallStatus::Invalid;

    auto *submission_render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    if (NativeMSAA) {
      if (!NativeMSAABindingsSafe(submission_render)) { RecordError(E_INVALIDARG); return DrawCallStatus::Invalid; }
      try {
        for (unsigned slot = 0; slot < vertex_buffers_.size(); ++slot) {
          if (!(pso_graphics_->slot_mask & (1u << slot))) continue;
          uint64_t offset = 0;
          auto *allocation = device_->LookupBufferByVA(vertex_buffers_[slot].BufferLocation, &offset);
          if (!allocation) { RecordError(E_INVALIDARG); return DrawCallStatus::Invalid; }
          if (allocator_->recording_head_->root_allocation_set.insert(allocation).second)
            allocator_->recording_head_->root_allocations.emplace_back(allocation);
        }
      } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return DrawCallStatus::Invalid; }
    }
    if (submission_render->visibility_intervals && !RecordVisibilityInterval(submission_render))
      return DrawCallStatus::Invalid;
    submission_render->submission_color_output_mask |= pso_graphics_->submission_color_output_mask;
    submission_render->submission_group_safe &= !render_pass_internal_ && !mesh && !SkipResourceBinding &&
        !IndexPull && !predication_enabled_ && !statistics &&
        active_visibility_queries_.empty() && active_so_queries_.empty() &&
        !pso_graphics_->stream_output.active && (NativeMSAA || !RequiresMSAAExecution()) &&
        !pso_graphics_->view_instancing.count &&
        !pso_graphics_->uses_atomic64 && !pso_graphics_->needs_rov_buffer_marker && !shading_rate_image_ &&
        (pso_graphics_->submission_mapping_safe || NativeMSAA) &&
        CaptureSubmissionAccess(submission_render->submission_buffers, submission_render->submission_tables,
            rootsig_graphics_.ptr(), rootarg_graphics_staging_, *pso_graphics_.ptr(), WMTMetal4StageVertex | WMTMetal4StageFragment,
            submission_render->submission_snapshot_budget, submission_render->submission_root_srv_opaque, true);
    if (submission_render->submission_group_safe) {
      try {
        for (unsigned slot = 0; slot < 32; ++slot)
          if (pso_graphics_->slot_mask & (1u << slot)) {
            const auto &input = vertex_buffers_[slot];
            if (!input.BufferLocation || !input.SizeInBytes) submission_render->submission_group_safe = false;
            else if (std::none_of(submission_render->submission_buffers.begin(), submission_render->submission_buffers.end(),
                [&](const auto &use) { return use.address == input.BufferLocation && use.length == input.SizeInBytes &&
                    !use.writes && use.stages == WMTMetal4StageVertex; })) {
              if (submission_render->submission_buffers.size() == kRecordedSubmissionBufferBudget) {
                submission_render->submission_snapshot_budget = true; submission_render->submission_group_safe = false; break;
              }
              submission_render->submission_buffers.push_back({input.BufferLocation, input.SizeInBytes, false, WMTMetal4StageVertex});
            }
          }
      } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return DrawCallStatus::Invalid; }
    }

    if (pso_graphics_->needs_rov_buffer_marker) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      const uint32_t native_samples = pso_graphics_->owned_sample_frequency_pixel_shader
          ? pso_graphics_->EffectiveRasterSampleCount() : 1;
      render->rov_marker_sample_count = std::max<uint32_t>(render->rov_marker_sample_count, native_samples);
      // Attachmentless rendering otherwise advertises a 16384-square pass. The
      // marker only needs pixels that can survive the active viewport/scissor.
      if (render->rov_marker_attachmentless && num_scissors) {
        for (UINT i = 0; i < num_viewports; ++i) {
          const auto &viewport = viewports[i];
          if (!(viewport.Width > 0 && viewport.Height > 0) ||
              !std::isfinite(viewport.TopLeftX) || !std::isfinite(viewport.TopLeftY) ||
              !std::isfinite(viewport.Width) || !std::isfinite(viewport.Height))
            continue;
          const D3D12_RECT scissor = i < num_scissors ? scissors[i] : D3D12_RECT{0, 0, 16384, 16384};
          const auto edge = [](double value) {
            return uint32_t(std::ceil(std::clamp(value, 0.0, 16384.0)));
          };
          const uint32_t right = std::min(edge(double(viewport.TopLeftX) + viewport.Width),
                                          uint32_t(std::clamp(scissor.right, LONG(0), LONG(16384))));
          const uint32_t bottom = std::min(edge(double(viewport.TopLeftY) + viewport.Height),
                                           uint32_t(std::clamp(scissor.bottom, LONG(0), LONG(16384))));
          if (right > std::max(double(viewport.TopLeftX), double(scissor.left)) &&
              bottom > std::max(double(viewport.TopLeftY), double(scissor.top))) {
            render->rov_marker_width = std::max(render->rov_marker_width, right);
            render->rov_marker_height = std::max(render->rov_marker_height, bottom);
          }
        }
      }
    }

    if (dirty_state_.test(DirtyState::GraphicsPipelineState) && mesh) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      const auto *variant = pso_graphics_->GetMeshPipeline(MeshKey);
      if (!variant) {
        RecordError(E_FAIL);
        return DrawCallStatus::Invalid;
      }
      (pso_graphics_->mesh_kind == MeshPipelineKind::Geometry ||
               pso_graphics_->mesh_kind == MeshPipelineKind::MeshShader
           ? render->use_geometry
           : render->use_tessellation) = true;
      auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd_setpso.type = WMTRenderCommandSetPSO;
      cmd_setpso.pso = variant->pso;
      auto &retained = render->immutable_constant_buffers;
      using Binding = std::pair<const ShaderImmutableConstants *, WMTRenderCommandType>;
      for (auto [constants, command_type] : {
             Binding{&variant->object_constants, WMTRenderCommandSetObjectBuffer},
             Binding{&variant->mesh_constants, WMTRenderCommandSetMeshBuffer},
             Binding{statistics ? &pso_graphics_->pipeline_statistics_fragment_constants
                                  : &pso_graphics_->fragment_constants,
                       WMTRenderCommandSetFragmentBuffer}}) {
        if (!constants->buffer)
          continue;
        auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        binding.type = command_type;
        binding.buffer = constants->buffer;
        binding.offset = constants->offset;
        binding.index = SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS;
        if (std::none_of(retained.begin(), retained.end(), [&](const auto &buffer) {
              return buffer.handle == constants->buffer.handle;
            }))
          retained.push_back(constants->buffer);
      }
      if (statistics) {
        auto [buffer, offset] = PipelineStatisticsCounterBuffer();
        if (!buffer)
          return DrawCallStatus::Invalid;
        for (auto type : {WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer,
                          WMTRenderCommandSetFragmentBuffer}) {
          auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          binding.type = type;
          binding.buffer = buffer;
          binding.offset = offset;
          binding.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
        }
      }
      // D3DalloyGeometryDraw/TessellationMeshDraw only rebase the draw-argument binding.
      auto &arguments = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      arguments.type = WMTRenderCommandSetObjectBuffer;
      allocator_->BindGPUHeap(arguments.buffer, arguments.offset, 0);
      arguments.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      // Attachment-free producers must not inherit raster depth tests/writes.
      // RecordMeshPixelReplay preserves the original PSO state for the consumer.
      EncodeGraphicsFixedState((MeshKey & kMeshMSAACapture) != 0);
      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }

    if (dirty_state_.test(DirtyState::GraphicsPipelineState)) {
      // MSAA producers replay only shader bindings; their capture and pixel
      // variants are selected by RecordMSAADraw, never this direct raster PSO.
      if (NativeMSAA || !RequiresMSAAExecution()) {
        auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
        cmd_setpso.type = WMTRenderCommandSetPSO;
        cmd_setpso.pso = NativeMSAA ? pso_graphics_->GetNativeMSAAPipelineState(IndexPull)
                                   : statistics ? pso_graphics_->GetPipelineStatisticsPipelineState(false)
                                   : IndexPull ? pso_graphics_->GetIndexPullPipelineState(false) : pso_graphics_->pso;
        if (!NativeMSAA && !statistics && !IndexPull && pso_graphics_->submission_mapping_safe) {
          uint8_t bound_outputs = 0;
          for (unsigned logical = 0; logical < submission_render->render_target_count; ++logical)
            if (submission_render->colors[logical].attachment) bound_outputs |= 1u << logical;
          const uint8_t disabled = pso_graphics_->submission_color_output_mask & ~bound_outputs;
          if (disabled) {
            try {
              cmd_setpso.pso = pso_graphics_->GetColorWriteMaskPipelineState(disabled);
              if (cmd_setpso.pso) {
                auto &retained_pipelines = submission_render->masked_pipeline_states;
                if (std::none_of(retained_pipelines.begin(), retained_pipelines.end(), [&](const auto &pipeline) {
                      return pipeline.handle == cmd_setpso.pso;
                    })) {
                  retained_pipelines.push_back(WMT::RenderPipelineState{cmd_setpso.pso});
                }
              }
            } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return DrawCallStatus::Invalid; }
          }
        }
        if (!cmd_setpso.pso) {
          RecordError(E_FAIL);
          return DrawCallStatus::Invalid;
        }
        direct_pipeline_bound_ = cmd_setpso.pso;
        if (NativeMSAA) {
          try {
            auto &retained = submission_render->masked_pipeline_states;
            if (std::none_of(retained.begin(), retained.end(), [&](const auto &pso) { return pso.handle == cmd_setpso.pso; }))
              retained.push_back(WMT::RenderPipelineState{cmd_setpso.pso});
          } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return DrawCallStatus::Invalid; }
          owned_graphics::Trace("native_msaa_record", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
              ",\"encoder\":", submission_render->id, ",\"sample_count\":", raster_samples,
              ",\"center\":", center_sample_pattern, ",\"index_pull\":", IndexPull, ",\"pso\":", cmd_setpso.pso);
        }
      }
      auto &retained = static_cast<RenderEncoderData *>(allocator_->encoder_current)->immutable_constant_buffers;
      for (auto [constants, command_type] : {
             std::pair{statistics ? &pso_graphics_->pipeline_statistics_vertex_constants
                                  : IndexPull ? &pso_graphics_->index_pull_vertex_constants : &pso_graphics_->vertex_constants,
                       WMTRenderCommandSetVertexBuffer},
             std::pair{statistics ? &pso_graphics_->pipeline_statistics_fragment_constants : &pso_graphics_->fragment_constants,
                       WMTRenderCommandSetFragmentBuffer}}) {
        if (!constants->buffer)
          continue;
        auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        binding.type = command_type;
        binding.buffer = constants->buffer;
        binding.offset = constants->offset;
        binding.index = SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS;
        if (std::none_of(retained.begin(), retained.end(), [&](const auto &buffer) {
              return buffer.handle == constants->buffer.handle;
            }))
          retained.push_back(constants->buffer);
      }

      if (statistics) {
        auto [buffer, offset] = PipelineStatisticsCounterBuffer();
        if (!buffer)
          return DrawCallStatus::Invalid;
        for (auto type : {WMTRenderCommandSetVertexBuffer, WMTRenderCommandSetFragmentBuffer}) {
          auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          binding.type = type;
          binding.buffer = buffer;
          binding.offset = offset;
          binding.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
        }
      }

      EncodeGraphicsFixedState(false);
      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }
    if (dirty_state_.test(DirtyState::VertexBuffer) && !SkipResourceBinding) {
      auto [Offset, Stride] = PopulateVertexBufferTable(1);
      if (Offset == UINT64_MAX)
        return DrawCallStatus::Invalid;
      if (Stride) {
        // The object stage of GS/HS/DS pipelines pulls vertices from the same table.
        for (auto type : {WMTRenderCommandSetVertexBuffer, WMTRenderCommandSetObjectBuffer}) {
          if (type == WMTRenderCommandSetObjectBuffer && !mesh)
            continue;
          auto &cmd_vsvb = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd_vsvb.type = type;
          allocator_->BindGPUHeap(cmd_vsvb.buffer, cmd_vsvb.offset, Offset);
          cmd_vsvb.index = SM50_BINDING_INDEX_VERTEX_BUFFER;
        }
      }
      dirty_state_.clr(DirtyState::VertexBuffer);
    }

    const bool capture_swizzle_tables = dirty_state_.test(DirtyState::GraphicsRootArguments);
    if (capture_swizzle_tables && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto Offset = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_);
        if (Offset == UINT64_MAX)
          return DrawCallStatus::Invalid;
        for (auto type : {WMTRenderCommandSetVertexBuffer, WMTRenderCommandSetFragmentBuffer,
                          WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer}) {
          // Object/mesh stages exist only on GS/HS/DS pipelines (and MSAA replay records none).
          if (!mesh && (type == WMTRenderCommandSetObjectBuffer || type == WMTRenderCommandSetMeshBuffer))
            continue;
          auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          binding.type = type;
          allocator_->BindGPUHeap(binding.buffer, binding.offset, Offset);
          binding.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        }
      }
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto Offset = EncodeStaticSamplers(rootsig_graphics_.ptr());
        if (Offset == UINT64_MAX)
          return DrawCallStatus::Invalid;
        for (auto type : {WMTRenderCommandSetVertexBuffer, WMTRenderCommandSetFragmentBuffer,
                          WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer}) {
          // Object/mesh stages exist only on GS/HS/DS pipelines (and MSAA replay records none).
          if (!mesh && (type == WMTRenderCommandSetObjectBuffer || type == WMTRenderCommandSetMeshBuffer))
            continue;
          auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          binding.type = type;
          allocator_->BindGPUHeap(binding.buffer, binding.offset, Offset);
          binding.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        }
      }
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }

    if (dirty_state_.any(DirtyState::Viewport, DirtyState::ScissorRect)) {
      // Metal requires at least one viewport and scissor rectangle. D3D12 instead
      // uses an empty viewport/scissor state to discard rasterization, while the
      // vertex stage still runs. Lower empty raster regions to a valid viewport
      // outside the framebuffer, and always restore both arrays together when
      // either D3D12 array changes.
      const UINT metal_count = std::max(num_viewports, 1u);
      auto metal_viewports = allocator_->AllocateCommandData<WMTViewport>(metal_count);
      auto metal_scissors = allocator_->AllocateCommandData<WMTScissorRect>(metal_count);
      const bool empty_raster_state = !num_viewports || !num_scissors;
      for (auto i = 0u; i < metal_count; i++) {
        bool empty_region = empty_raster_state;
        if (!empty_region) {
          auto &viewport = viewports[i];
          // A negative height is an inverted Y transform, not an empty region.
          // Metal accepts signed heights and reversed near/far depths directly.
          empty_region = viewport.Width <= 0.0f || viewport.Height == 0.0f;
          if (!empty_region) {
            metal_viewports[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                                  viewport.Height,   viewport.MinDepth, viewport.MaxDepth};
          }
        }

        if (!empty_region && i < num_scissors) {
          auto &d3d_rect = scissors[i];
          LONG left = std::clamp(d3d_rect.left, (LONG)0, (LONG)16384);
          LONG top = std::clamp(d3d_rect.top, (LONG)0, (LONG)16384);
          LONG right = std::clamp(d3d_rect.right, left, (LONG)16384);
          LONG bottom = std::clamp(d3d_rect.bottom, top, (LONG)16384);
          empty_region = right == left || bottom == top;
          if (!empty_region)
            metal_scissors[i] = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
        } else if (!empty_region) {
          metal_scissors[i] = {0, 0, 16384, 16384};
        }

        if (empty_region) {
          metal_viewports[i] = {-2.0, -2.0, 1.0, 1.0, 0.0, 1.0};
          metal_scissors[i] = {0, 0, 1, 1};
        }
      }

      auto &viewport_cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
      viewport_cmd.type = WMTRenderCommandSetViewports;
      viewport_cmd.viewports.set(metal_viewports);
      viewport_cmd.viewport_count = metal_count;
      auto &scissor_cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setscissorrects>();
      scissor_cmd.type = WMTRenderCommandSetScissorRects;
      scissor_cmd.scissor_rects.set(metal_scissors);
      scissor_cmd.rect_count = metal_count;
      dirty_state_.clr(DirtyState::Viewport, DirtyState::ScissorRect);
    }

    if (dirty_state_.test(DirtyState::BlendFactor)) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setblendcolor>();
      cmd.type = WMTRenderCommandSetBlendFactor;
      cmd.red = blend_factor_[0];
      cmd.green = blend_factor_[1];
      cmd.blue = blend_factor_[2];
      cmd.alpha = blend_factor_[3];
      dirty_state_.clr(DirtyState::BlendFactor);
    }

    if (dirty_state_.test(DirtyState::StencilRef)) {
      if (back_stencil_ref_ == stencil_ref_) {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref>();
        cmd.type = WMTRenderCommandSetStencilRef;
        cmd.stencil_ref = stencil_ref_;
      } else {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref_front_back>();
        cmd.type = WMTRenderCommandSetStencilRefFrontBack;
        cmd.front_stencil_ref = stencil_ref_;
        cmd.back_stencil_ref = back_stencil_ref_;
      }
      dirty_state_.clr(DirtyState::StencilRef);
    }

    // View instance location (view instancing): the pipeline adds the mapping's offsets to the viewport and
    // render target array index of every primitive. Re-sent for each draw of such a pipeline.
    if (pso_graphics_->view_location_offsets && !RequiresMSAAExecution()) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_set_vertex_amplification>();
      cmd.type = WMTRenderCommandSetVertexAmplification;
      cmd.viewport_offset = view_location_.ViewportArrayIndex;
      cmd.render_target_array_offset = view_location_.RenderTargetArrayIndex;
    }

    // Ordinary captures cover the complete table, including later PSOs. A new
    // pass or changed root arguments needs another capture; atomic phases keep
    // their per-PSO capture because those ranges are shader-restricted.
    if (capture_swizzle_tables || pso_graphics_->uses_atomic64)
      CaptureStandardSwizzleTables(
          static_cast<RenderEncoderData *>(allocator_->encoder_current)->swizzle_tables,
          rootsig_graphics_.ptr(), rootarg_graphics_staging_, pso_graphics_->uses_atomic64, pso_graphics_.ptr());
    if (pso_graphics_->uses_atomic64) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      render->atomic64_phase = true;
      render->atomic64_pipeline = pso_graphics_.ptr();
      render->atomic64_signature = rootsig_graphics_.ptr();
      render->atomic64_heap = descriptor_heaps_[0];
      render->atomic64_tess_workloads = pso_graphics_->tess_capture_layout.max_workloads;
      render->atomic64_gs_instances = pso_graphics_->gs_instance_count;
      CaptureAtomic64Roots(render->atomic64_roots, rootsig_graphics_.ptr(), rootarg_graphics_staging_, pso_graphics_.ptr());
    }

    return DrawCallStatus::Ordinary;
  }

  bool EnsureStreamOutputPipelines(bool indirect = false) {
    if (so_assemble_pipeline_ && so_positions_pipeline_ && (!indirect || so_indirect_preflight_pipeline_))
      return true;
    WMT::Reference<WMT::Error> error;
    auto library = device_->GetLib().getLibrary();
    if (!so_assemble_pipeline_)
      so_assemble_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction("so_assemble"), error);
    if (!so_positions_pipeline_)
      so_positions_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction("so_index_positions"), error);
    if (indirect && !so_indirect_preflight_pipeline_)
      so_indirect_preflight_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction("so_indirect_preflight"), error);
    return so_assemble_pipeline_ && so_positions_pipeline_ && (!indirect || so_indirect_preflight_pipeline_);
  }

  /*
   * Stream-output draw, recording side (see StreamOutputDrawData). Records are keyed by IA
   * position, so the capture scratch is sized and created here: VertexCount * InstanceCount
   * records of record_dwords dwords plus one claim word each. A draw whose capture cannot be
   * represented records E_OUTOFMEMORY (surfaced at Close) instead of losing output.
   */
  bool PrepareStreamOutputDraw(UINT VertexCount, UINT InstanceCount, bool Indexed, UINT StartIndexLocation,
                               INT BaseVertexLocation, bool Indirect = false) {
    so_pending_.reset();
    const auto &layout = pso_graphics_->stream_output;
    const bool statistics = !active_pipeline_statistics_.empty();
    const bool msaa_only_indirect = Indirect && RequiresMSAAExecution() &&
                                    !layout.active && !statistics;
    const auto &statistics_layout = pso_graphics_->pipeline_statistics_capture;
    const uint32_t record_dwords = statistics ? statistics_layout.record_dwords : layout.streams[0].record_dwords;
    uint32_t topology;
    switch (topology_) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: topology = 0; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: topology = 1; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: topology = 2; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: topology = 3; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: topology = 4; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: topology = 6; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: topology = 7; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: topology = 8; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: topology = 9; break;
    default:
      // Patch inputs require tessellation, not the VS-only capture path.
      RecordError(E_NOTIMPL);
      return false;
    case kTriangleFan: topology = 5; break;
    }
    // Metal point draws capture adjacency inputs, but cannot rasterize their interior primitives.
    if (topology >= 6 && (!layout.active || layout.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM)) {
      RecordError(E_NOTIMPL);
      return false;
    }
    if (Indexed && !Indirect && !index_buffer) {
      RecordError();
      return false;
    }
    // Indirect preflight still uses the SO count/materialization compute PSO,
    // even when the draw does not need stream-output scratch or assembly.
    if (!EnsureStreamOutputPipelines(Indirect)) {
      RecordError(E_FAIL);
      return false;
    }
    auto metal = device_->GetMTLDevice();
    const uint64_t records = uint64_t(VertexCount) * InstanceCount;
    const uint64_t scratch_bytes = records * (uint64_t(record_dwords) + 1) * sizeof(uint32_t);
    if (records > UINT32_MAX || scratch_bytes > metal.maxBufferLength() ||
        (Indexed && uint64_t(VertexCount) * sizeof(uint32_t) > metal.maxBufferLength())) {
      ERR("StreamOutput: draw of ", VertexCount, "x", InstanceCount, " vertices exceeds the capture limit");
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto [counters, counters_offset] = msaa_only_indirect
        ? std::tuple<WMT::Buffer, uint64_t>{allocator_->ResolveGPUHeap(0).buffer, 0}
        : layout.active || !statistics ? SOStreamCounterBuffer() : PipelineStatisticsCounterBuffer();
    if (!counters)
      return false;
    auto [statistics_buffer, statistics_offset] = statistics ? PipelineStatisticsCounterBuffer()
        : std::tuple<WMT::Buffer, uint64_t>{WMT::Buffer{}, 0};
    if (statistics && !statistics_buffer)
      return false;
    const size_t kExtra = sizeof(StreamOutputAssemblyParams) +
                          (Indirect ? 0 : sizeof(SM50_SO_CAPTURE_CONTROL) + 16 + 64);
    if (!allocator_->ReserveGPUHeap(kExtra, 16)) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    std::shared_ptr<StreamOutputDrawData> draw;
    try {
      draw = std::make_shared<StreamOutputDrawData>();
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    if (!Indirect) {
      WMTBufferInfo info{};
      info.length = std::max<uint64_t>(scratch_bytes, sizeof(uint32_t));
      info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      draw->scratch = metal.newBuffer(info);
      if (Indexed && VertexCount) {
        info = {};
        info.length = uint64_t(VertexCount) * sizeof(uint32_t);
        info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
        draw->positions = metal.newBuffer(info);
      }
      if (!draw->scratch || (Indexed && VertexCount && !draw->positions)) {
        RecordError(E_OUTOFMEMORY);
        return false;
      }
    }
    auto [params_memory, params_offset] = allocator_->AllocateGPUHeap(sizeof(StreamOutputAssemblyParams), 16);
    auto *params = new (params_memory) StreamOutputAssemblyParams{};
    params->topology = topology;
    params->indexed = Indexed;
    params->index_size = index_type == WMTIndexTypeUInt32 ? 4 : 2;
    // D3D12 applies IBStripCutValue to strip topologies only, and only when it matches the index
    // format; list topologies treat 0xFFFF / 0xFFFFFFFF as ordinary indices.
    const auto cut = ib_strip_cut_value_;
    const bool strip = topology == 2 || topology == 4 || topology == 5 || topology == 7 || topology == 9;
    params->cut_enabled =
        Indexed && strip &&
        ((cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF && params->index_size == 2) ||
         (cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF && params->index_size == 4));
    params->cut_value = params->index_size == 4 ? 0xffffffffu : 0xffffu;
    params->vertex_count = VertexCount;
    params->instance_count = InstanceCount;
    params->record_dwords = record_dwords;
    params->stream_output_enabled = layout.active;
    params->statistics_enabled = statistics;
    if (statistics) {
      params->statistics_capture_offset = statistics_layout.capture_offset;
      params->clip_mask = statistics_layout.clip_mask;
      params->cull_mask = statistics_layout.cull_mask;
      params->rasterization_enabled = statistics_layout.rasterization_enabled;
      params->depth_clip_enabled = pso_graphics_->depth_clip_mode == WMTDepthClipModeClip;
      draw->statistics_heap = statistics_buffer;
      draw->statistics_offset = statistics_offset;
    }
    params->element_count = uint32_t(layout.elements.size());
    params->referenced_mask = layout.streams[0].referenced_slot_mask;
    params->predicate_enabled = predication_enabled_;
    params->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    for (uint32_t slot = 0; slot < D3D12_SO_BUFFER_SLOT_COUNT; slot++) {
      params->stride[slot] = layout.slots[slot].stride;
      const auto &target = so_targets_[slot];
      if (!(layout.streams[0].referenced_slot_mask & (1u << slot)) || !target.buffer)
        continue; // unreferenced bound targets are ignored (FS 14.6)
      params->bound_mask |= 1u << slot;
      params->size[slot] = target.size;
      draw->targets[slot] = target.buffer;
      draw->target_offsets[slot] = target.offset;
      draw->filled_sizes[slot] = target.filled_size;
      draw->filled_size_offsets[slot] = target.filled_size_offset;
    }
    for (size_t i = 0; i < layout.elements.size(); i++) {
      const auto &element = layout.elements[i];
      params->elements[i] = {element.slot, element.byte_offset, element.dword_offset, element.component_count};
    }
    draw->assemble_pipeline = so_assemble_pipeline_;
    draw->positions_pipeline = so_positions_pipeline_;
    allocator_->BindGPUHeap(draw->heap, draw->params_offset, params_offset);
    draw->params_cpu = params;
    draw->counters_heap = counters;
    draw->counters_offset = counters_offset;
    draw->predicate_heap = counters;
    draw->predicate_offset = counters_offset;
    if (predication_enabled_)
      allocator_->BindGPUHeap(draw->predicate_heap, draw->predicate_offset, predication_snapshot_offset_);
    draw->indexed = Indexed;
    if (Indirect) {
      so_pending_ = std::move(draw);
      return true;
    }
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(SM50_SO_CAPTURE_CONTROL), 16);
    auto *control = new (control_memory) SM50_SO_CAPTURE_CONTROL{};
    control->positions_per_instance = VertexCount;
    control->first_position = 0;
    control->record_count = uint32_t(records);
    auto [status_memory, status_offset] = allocator_->AllocateGPUHeap(16, 16);
    memset(status_memory, 0, 16);
    if (Indexed) {
      // Index buffers are bound 4-byte aligned; the kernels and the VS skip the remainder.
      const uint64_t first = index_offset + uint64_t(StartIndexLocation) * params->index_size;
      draw->index_offset = first & ~uint64_t(3);
      params->index_skip = uint32_t(first & 3);
      control->index_size = params->index_size;
      control->index_first = params->index_skip / 2;
      control->base_vertex = BaseVertexLocation;
    }
    draw->index_buffer = Indexed ? index_buffer : WMT::Buffer{};
    draw->control_offset = allocator_->ResolveGPUHeap(control_offset).offset;
    draw->status_offset = allocator_->ResolveGPUHeap(status_offset).offset;
    draw->claims_offset = records * record_dwords * sizeof(uint32_t);
    draw->claims_bytes = records * sizeof(uint32_t);
    draw->vertex_count = VertexCount;
    draw->status_cpu = static_cast<const uint32_t *>(status_memory);
    so_pending_ = std::move(draw);
    return true;
  }

  /*
   * Stream output after a GS, recording side. The GS mesh function writes, per invocation key and captured
   * stream, up to dcl_maxout records plus one header word into one scratch buffer (SM50_SHADER_D3D12_GS_STREAMS_DATA,
   * so_assemble_gs); the queue zeroes the header words before the pass (the claim ranges) and runs the ordered
   * assembly of each stream after it, into that stream's slots and SO statistics counters. `primitives` bounds the
   * primitive id per draw instance: the IA primitive count of the draw (list topologies) or of its strip expansion.
   * `so_pending_` is the lowest captured stream; the others follow through StreamOutputDrawData::next_stream.
   */
  bool PrepareGeometryStreamOutput(uint32_t primitives, uint32_t instances) {
    so_pending_.reset();
    const auto &layout = pso_graphics_->stream_output;
    auto metal = device_->GetMTLDevice();
    if (!so_assemble_gs_pipeline_) {
      WMT::Reference<WMT::Error> error;
      so_assemble_gs_pipeline_ =
          device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(device_->GetLib().getLibrary()).newFunction("so_assemble_gs"), error);
      if (!so_assemble_gs_pipeline_) {
        RecordError(E_FAIL);
        return false;
      }
    }
    uint32_t topology;
    // D3D10_SB_PRIMITIVE_TOPOLOGY: 1 point list, 3 line strip, 5 triangle strip.
    switch (pso_graphics_->gs_output_topology) {
    case 1: topology = 0; break;
    case 3: topology = 2; break;
    default: topology = 4; break;
    }
    const uint64_t keys = uint64_t(primitives) * instances * pso_graphics_->gs_instance_count;
    const uint64_t max_vertices = std::max<uint32_t>(pso_graphics_->gs_max_output_vertices, 1);
    // Per key, the dwords of every captured stream's region (records plus one header word).
    uint64_t key_dwords = 0;
    uint32_t stream_count = 0;
    for (uint32_t s = 0; s < D3D12_SO_STREAM_COUNT; s++)
      if (layout.stream_mask & (1u << s)) {
        key_dwords += max_vertices * (uint64_t(layout.streams[s].record_dwords) + 1) + 1;
        stream_count++;
      }
    if (keys > UINT32_MAX || (keys && key_dwords > metal.maxBufferLength() / sizeof(uint32_t) / keys)) {
      ERR("StreamOutput: GS draw of ", keys, " invocations exceeds the capture limit");
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    const uint64_t scratch_bytes = keys * key_dwords * sizeof(uint32_t);
    const size_t extra = stream_count * (sizeof(StreamOutputAssemblyParams) + 16 + 16) +
                         sizeof(SM50_SO_CAPTURE_CONTROL) + 64;
    auto [counters, counters_offset] = SOStreamCounterBuffer();
    if (!counters)
      return false;
    if (!allocator_->ReserveGPUHeap(extra, 16)) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    WMTBufferInfo info{};
    info.length = std::max<uint64_t>(scratch_bytes, sizeof(uint32_t));
    info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
    WMT::Reference<WMT::Buffer> scratch = metal.newBuffer(info);
    if (!scratch) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(SM50_SO_CAPTURE_CONTROL), 16);
    auto *control = new (control_memory) SM50_SO_CAPTURE_CONTROL{};
    control->positions_per_instance = primitives;
    control->record_count = uint32_t(keys);
    std::shared_ptr<StreamOutputDrawData> first, last;
    uint64_t prefix_dwords = 0;
    for (uint32_t s = 0; s < D3D12_SO_STREAM_COUNT; s++) {
      if (!(layout.stream_mask & (1u << s)))
        continue;
      const auto &stream = layout.streams[s];
      std::shared_ptr<StreamOutputDrawData> draw;
      try {
        draw = std::make_shared<StreamOutputDrawData>();
      } catch (const std::bad_alloc &) {
        RecordError(E_OUTOFMEMORY);
        return false;
      }
      auto [params_memory, params_offset] = allocator_->AllocateGPUHeap(sizeof(StreamOutputAssemblyParams), 16);
      auto *params = new (params_memory) StreamOutputAssemblyParams{};
      params->topology = topology;
      params->vertex_count = uint32_t(max_vertices);
      params->record_dwords = stream.record_dwords;
      params->stream_output_enabled = true;
      params->referenced_mask = stream.referenced_slot_mask;
      params->predicate_enabled = predication_enabled_;
      params->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
      for (uint32_t slot = 0; slot < D3D12_SO_BUFFER_SLOT_COUNT; slot++) {
        params->stride[slot] = layout.slots[slot].stride;
        const auto &target = so_targets_[slot];
        if (!(stream.referenced_slot_mask & (1u << slot)) || !target.buffer)
          continue;
        params->bound_mask |= 1u << slot;
        params->size[slot] = target.size;
        draw->targets[slot] = target.buffer;
        draw->target_offsets[slot] = target.offset;
        draw->filled_sizes[slot] = target.filled_size;
        draw->filled_size_offsets[slot] = target.filled_size_offset;
      }
      for (const auto &element : layout.elements)
        if (element.stream == s)
          params->elements[params->element_count++] = {element.slot, element.byte_offset, element.dword_offset,
                                                       element.component_count};
      auto [status_memory, status_offset] = allocator_->AllocateGPUHeap(16, 16);
      memset(status_memory, 0, 16);
      draw->scratch = scratch;
      draw->assemble_pipeline = so_assemble_gs_pipeline_;
      allocator_->BindGPUHeap(draw->heap, draw->params_offset, params_offset);
      draw->params_cpu = params;
      draw->counters_heap = counters;
      draw->predicate_heap = counters;
      draw->control_offset = allocator_->ResolveGPUHeap(control_offset).offset;
      draw->counters_offset = counters_offset + uint64_t(s) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
      draw->predicate_offset = counters_offset;
      if (predication_enabled_)
        allocator_->BindGPUHeap(draw->predicate_heap, draw->predicate_offset, predication_snapshot_offset_);
      draw->status_offset = allocator_->ResolveGPUHeap(status_offset).offset;
      draw->status_cpu = static_cast<const uint32_t *>(status_memory);
      draw->stream = s;
      draw->scratch_prefix_dwords = prefix_dwords;
      draw->scratch_offset = keys * prefix_dwords * sizeof(uint32_t);
      // The per-key header words after this stream's records.
      draw->claims_offset = draw->scratch_offset + keys * max_vertices * (uint64_t(stream.record_dwords) + 1) * 4;
      draw->claims_bytes = keys * sizeof(uint32_t);
      prefix_dwords += max_vertices * (uint64_t(stream.record_dwords) + 1) + 1;
      if (last)
        last->next_stream = draw;
      else
        first = draw;
      last = std::move(draw);
    }
    so_pending_ = std::move(first);
    return true;
  }

  /* Binds the GS capture of `so_pending_` to the mesh stage of the current render pass. */
  bool BindGeometryStreamOutput() {
    auto draw = std::move(so_pending_);
    auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    const std::pair<WMT::Buffer, uint64_t> bindings[] = {{draw->heap, draw->control_offset}, {draw->scratch, 0}};
    const uint8_t indices[] = {SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH};
    for (uint32_t i = 0; i < 2; i++) {
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetMeshBuffer;
      binding.buffer = bindings[i].first;
      binding.offset = bindings[i].second;
      binding.index = indices[i];
    }
    auto &use = allocator_->EncodeRenderCommand<wmtcmd_render_useresource>();
    use.type = WMTRenderCommandUseResource;
    use.resource = draw->scratch;
    use.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    use.stages = WMTRenderStageMesh;
    try {
      // Each captured stream is assembled on its own, in stream order.
      for (auto stream = draw; stream; stream = stream->next_stream)
        render->so_draws.push_back(stream);
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    so_bound_ = std::move(draw);
    return true;
  }

  /* Binds the capture buffers of `so_pending_` on the current render pass. */
  bool BindStreamOutputDraw() {
    auto draw = std::move(so_pending_);
    auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    const std::pair<WMT::Buffer, uint64_t> bindings[] = {
        {draw->indexed ? draw->index_buffer : draw->heap, draw->indexed ? draw->index_offset : 0},
        {draw->heap, draw->control_offset},
        {draw->scratch, 0},
    };
    for (uint32_t i = 0; i < 3; i++) {
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetVertexBuffer;
      binding.buffer = bindings[i].first;
      binding.offset = bindings[i].second;
      binding.index = SM50_BINDING_INDEX_VERTEX_INDICES + i;
    }
    try {
      render->so_draws.push_back(draw);
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    so_bound_ = std::move(draw);
    return true;
  }

  bool RecordMSAADraw(UINT vertices, UINT instances, UINT start_vertex, INT base_vertex,
                      UINT start_instance, bool indexed, bool indirect = false,
                      const MeshPipelineVariant *mesh_capture = nullptr,
                      const SM50_MSAA_MESH_CONTROL *mesh_control = nullptr) {
    auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    const uint32_t shading_rate = CoarseShadingRateState();
    const auto *variants = pso_graphics_->GetMSAAPipelineVariants(!active_pipeline_statistics_.empty(),
                                                                  shading_rate != 0);
    if (!variants) {
      RecordError(E_FAIL);
      return false;
    }
    if (!msaa_assembly_pipeline_) {
      WMT::Reference<WMT::Error> error;
      msaa_assembly_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(device_->GetLib().getLibrary()).newFunction("msaa_assemble_primitives"), error);
      if (!msaa_assembly_pipeline_) {
        RecordError(E_FAIL);
        return false;
      }
    }
    if (!msaa_bounds_pipeline_) {
      WMT::Reference<WMT::Error> error;
      msaa_bounds_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(device_->GetLib().getLibrary()).newFunction("msaa_primitive_bounds"), error);
      if (!msaa_bounds_pipeline_) {
        RecordError(E_FAIL);
        return false;
      }
    }
    uint32_t topology;
    if (mesh_capture) {
      const auto vpp = mesh_capture->capture_layout.vertices_per_primitive;
      topology = vpp == 1 ? 0 : vpp == 2 ? 1 : 3;
    } else switch (topology_) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: topology = 0; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: topology = 1; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: topology = 2; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: topology = 3; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: topology = 4; break;
    default: RecordError(E_INVALIDARG); return false;
    case kTriangleFan: topology = 5; break;
    }
    const uint32_t vpp = topology == 0 ? 1 : topology <= 2 ? 2 : 3;
    const bool strip = topology == 2 || topology == 4 || topology == 5;
    const uint64_t max_per_instance = strip ? vertices >= vpp ? vertices - vpp + 1 : 0 : vertices / vpp;
    const uint64_t max_primitives = max_per_instance * instances;
    const uint64_t record_count = uint64_t(vertices) * instances;
    const auto line_mode = topology == 1 || topology == 2
        ? pso_graphics_->owned_line_mode : SM50_MSAA_LINE_NONE;
    // Conservative rasterization is defined for triangles only; the PSO rejects other topology types.
    const bool conservative = pso_graphics_->owned_conservative && topology >= 3;
    const uint32_t index_stride = MSAAIndexStride(line_mode, conservative);
    const uint64_t index_bytes = max_primitives * index_stride * sizeof(uint32_t);
    const uint64_t draw_bytes = max_primitives * sizeof(MSAAPrimitiveIndirectArguments);
    const auto max_buffer = max_buffer_length_;
    if (record_count > UINT32_MAX ||
        max_primitives > UINT32_MAX / index_stride ||
        index_bytes > max_buffer || draw_bytes > max_buffer ||
        record_count * uint64_t(variants->vertex_layout.record_dwords) * 4 > max_buffer) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto draw = std::make_unique<MSAADrawData>();
    if (mesh_capture) {
      draw->mesh_capture = true;
      draw->mesh_layout = mesh_capture->capture_layout;
      draw->mesh_control = *mesh_control;
    }
    draw->capture_pipeline = variants->capture;
    draw->atomic64_phase = pso_graphics_->uses_atomic64;
    draw->assembly_pipeline = msaa_assembly_pipeline_;
    draw->bounds_pipeline = msaa_bounds_pipeline_;
    draw->coverage_pipeline = variants->coverage;
    draw->coverage_with_depth_pipeline = variants->coverage_with_depth;
    draw->clear_coverage_pipeline = variants->clear_coverage;
    draw->gather_depth_pipeline = variants->gather_depth;
    draw->depth_bounds_probe_pipeline = variants->depth_bounds_probe;
    draw->early_depth_pipeline = variants->early_depth;
    draw->late_depth_pipeline = variants->late_depth;
    draw->sample_pixel_pipeline = variants->sample_pixel;
    draw->capture_depth_state = variants->capture_depth_state;
    draw->pixel_pipeline = variants->pixel;
    draw->replay_pipeline = variants->replay;
    draw->replay_layer_pipeline = variants->replay_layer;
    draw->query_coverage_pipeline = variants->query_coverage;
    const ShaderImmutableConstants *constants[] = {&variants->capture_constants,
        &variants->replay_vertex_constants, &variants->coverage_constants,
        &variants->pixel_constants, &variants->replay_fragment_constants,
        &variants->sample_fragment_constants};
    for (size_t i = 0; i < std::size(constants); i++) {
      draw->constants[i] = constants[i]->buffer;
      draw->constant_offsets[i] = constants[i]->offset;
    }
    draw->original_depth_stencil = pso_graphics_->GetDepthStencilState(render->dsv_planar_flags,
                                                                       render->dsv_readonly_flags);
    if (pso_graphics_->owned_depth_bounds && !pso_graphics_->emulated_depth_bounds &&
        render->depth.attachment && depth_bounds_min_ > depth_bounds_max_)
      draw->original_depth_stencil = pso_graphics_->depth_bounds_reject_state;
    draw->no_depth_stencil = device_->default_depth_stencil_state;
    draw->index_buffer = indexed ? index_buffer : WMT::Buffer{};
    draw->index_offset = indexed ? index_offset + uint64_t(start_vertex) * (index_type == WMTIndexTypeUInt32 ? 4 : 2) : 0;
    if (predication_enabled_)
      allocator_->BindGPUHeap(draw->predicate_buffer, draw->predicate_offset, predication_snapshot_offset_);
    draw->stream_output = render->so_draws.empty() ? nullptr : render->so_draws.back();
    draw->pixel_layout = variants->layout;
    draw->depth_bounds_min = depth_bounds_min_;
    draw->depth_bounds_max = depth_bounds_max_;
    draw->depth_bounds_enabled = pso_graphics_->owned_depth_bounds && render->depth.attachment &&
                                 (pso_graphics_->emulated_depth_bounds || depth_bounds_min_ <= depth_bounds_max_);
    draw->vertex_layout = variants->vertex_layout;
    draw->sample_count = pso_graphics_->EffectiveRasterSampleCount();
    if (pso_graphics_->needs_rov_buffer_marker)
      draw->rov_marker_sample_count = pso_graphics_->owned_sample_frequency_pixel_shader
          ? draw->sample_count : 1;
    draw->center_sample_pattern = pso_graphics_->owned_sample_desc.Quality == 0xfffffffeu && !sample_positions_.count;
    draw->sample_positions = sample_positions_;
    draw->shading_rate = variants->layout.flags & SM50_MSAA_PIXEL_COARSE ? shading_rate : 0;
    if (!CaptureShadingRateImage(*draw)) return false;
    draw->sample_mask = pso_graphics_->owned_sample_mask;
    draw->line_mode = line_mode;
    draw->conservative = conservative;
    draw->depth_clip_enabled = pso_graphics_->depth_clip_mode == WMTDepthClipModeClip;
    draw->single_sample_replay = pso_graphics_->ForcedSampleReplay();
    draw->output_sample_count = pso_graphics_->owned_sample_desc.Count;
    draw->start_vertex = start_vertex;
    draw->start_instance = start_instance;
    draw->base_vertex = base_vertex;
    draw->indexed = indexed;
    draw->indirect = indirect;
    draw->needs_stream_assembly = pso_graphics_->stream_output.active ||
                                  !active_pipeline_statistics_.empty();
    auto &assembly = draw->assembly;
    assembly.topology = topology;
    assembly.line_mode = line_mode;
    assembly.conservative = conservative;
    assembly.indexed = indexed;
    assembly.index_size = index_type == WMTIndexTypeUInt32 ? 4 : 2;
    assembly.vertex_count = vertices;
    assembly.instance_count = instances;
    assembly.positions_per_instance = vertices;
    assembly.max_primitives = uint32_t(max_primitives);
    assembly.predicate_enabled = predication_enabled_;
    assembly.predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    assembly.index_skip = draw->index_offset & 3;
    if (indexed && strip) {
      const auto cut = ib_strip_cut_value_;
      assembly.cut_enabled = (assembly.index_size == 2 && cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
                             (assembly.index_size == 4 && cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF);
      assembly.cut_value = assembly.index_size == 2 ? 0xffff : 0xffffffffu;
    }
    auto &bounds = draw->bounds;
    bounds.record_dwords = variants->vertex_layout.record_dwords;
    bounds.position_offset = variants->vertex_layout.position_offset_dwords;
    bounds.primitive_id_offset = variants->vertex_layout.primitive_id_offset_dwords;
    bounds.viewport_index_offset = variants->vertex_layout.viewport_index_offset_dwords;
    bounds.render_target_array_index_offset = variants->vertex_layout.render_target_array_index_offset_dwords;
    bounds.target_array_length = std::max(1u, uint32_t(render->render_target_array_length));
    bounds.target_width = render->render_target_width;
    bounds.target_height = render->render_target_height;
    bounds.viewport_count = num_viewports && num_scissors ? num_viewports : 0;
    bounds.scissor_count = bounds.viewport_count;
    bounds.max_primitives = assembly.max_primitives;
    bounds.line_mode = line_mode;
    bounds.conservative = conservative;
    bounds.view_offsets_enabled = pso_graphics_->view_location_offsets;
    bounds.view_viewport_offset = view_location_.ViewportArrayIndex;
    bounds.view_layer_offset = view_location_.RenderTargetArrayIndex;
    for (uint32_t i = 0; i < bounds.viewport_count && i < 16; i++) {
      const auto &viewport = viewports[i];
      bounds.viewports[i][0] = viewport.TopLeftX;
      bounds.viewports[i][1] = viewport.TopLeftY;
      bounds.viewports[i][2] = viewport.Width;
      bounds.viewports[i][3] = viewport.Height;
      draw->viewport_depths[i][0] = viewport.MinDepth;
      draw->viewport_depths[i][1] = viewport.MaxDepth;
      const D3D12_RECT rect = i < num_scissors ? scissors[i] : D3D12_RECT{0, 0, 16384, 16384};
      bounds.scissors[i][0] = std::clamp<LONG>(rect.left, 0, 16384);
      bounds.scissors[i][1] = std::clamp<LONG>(rect.top, 0, 16384);
      bounds.scissors[i][2] = std::clamp<LONG>(rect.right, 0, 16384);
      bounds.scissors[i][3] = std::clamp<LONG>(rect.bottom, 0, 16384);
    }
    render->msaa_draw = std::move(draw);
    allocator_->InvalidateCurrentPass();
    return true;
  }

  void STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::DrawInstanced, VertexCountPerInstance,
                     InstanceCount, StartVertexLocation, StartInstanceLocation))
      return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError(E_FAIL);
      return;
    }
    if (!VertexCountPerInstance || !InstanceCount)
      return;
    if (ReplayPerView([&] {
          DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        }))
      return;
    if (pso_graphics_ && pso_graphics_->mesh_kind != MeshPipelineKind::None)
      return RecordMeshDraw(false, VertexCountPerInstance, InstanceCount, StartVertexLocation, 0,
                            StartInstanceLocation);
    if (OmitPendingDraw())
      return;
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    const bool native_msaa = CanUseNativeMSAA();
    const bool msaa_execution = RequiresMSAAExecution() && !native_msaa;
    if (msaa_execution)
      allocator_->InvalidateCurrentPass();
    uint64_t predicated_offset = 0;
    if (predication_enabled_) {
      const D3D12_DRAW_ARGUMENTS arguments = {VertexCountPerInstance, InstanceCount, StartVertexLocation,
                                              StartInstanceLocation};
      predicated_offset = PreparePredicatedArguments(&arguments, sizeof(arguments));
      if (predicated_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    const bool stream_output = pso_graphics_ &&
        (pso_graphics_->stream_output.active || !active_pipeline_statistics_.empty());
    if (stream_output && !PrepareStreamOutputDraw(VertexCountPerInstance, InstanceCount, false, 0, 0))
      return;
    DrawCallStatus status = PreDraw(false, false, ~0u, native_msaa);
    if (status == DrawCallStatus::Invalid)
      return;
    RecordAsyncTargets();
    if (native_msaa) owned_graphics::Trace("native_msaa_draw_record", ",\"list\":", owned_id,
        ",\"recording\":", owned_recording, ",\"encoder\":", allocator_->encoder_current->id,
        ",\"indexed\":0,\"index_pull\":0,\"count\":", VertexCountPerInstance, ",\"instances\":", InstanceCount);
    if (stream_output && !BindStreamOutputDraw())
      return;
    if (msaa_execution) {
      RecordMSAADraw(VertexCountPerInstance, InstanceCount, StartVertexLocation, 0,
                     StartInstanceLocation, false);
      so_bound_.reset();
      return;
    }
    so_bound_.reset();

    if (predication_enabled_) {
      auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
      cmd_draw.type = WMTRenderCommandDrawIndirect;
      cmd_draw.primitive_type = primitive_type;
      allocator_->BindGPUHeap(cmd_draw.indirect_args_buffer, cmd_draw.indirect_args_offset, predicated_offset);
    } else {
      auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
      cmd_draw.type = WMTRenderCommandDraw;
      cmd_draw.primitive_type = primitive_type;
      cmd_draw.base_instance = StartInstanceLocation;
      cmd_draw.instance_count = InstanceCount;
      cmd_draw.vertex_start = StartVertexLocation;
      cmd_draw.vertex_count = VertexCountPerInstance;
    }
    owned_graphics::Trace("draw", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"vertices\":", VertexCountPerInstance, ",\"instances\":", InstanceCount);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  /* {vertices shaded per object threadgroup, vertex advance between threadgroups} of a GS input. */
  /* IA primitives in `count` vertices of `topology` (non-cut strips, list topologies). */
  static uint32_t MeshInputPrimitives(D3D12_PRIMITIVE_TOPOLOGY topology, uint32_t count) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return count;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return count / 2;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return count > 1 ? count - 1 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return count / 3;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return count > 2 ? count - 2 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return count / 4;
    case kTriangleFan: return count > 2 ? count - 2 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return count > 3 ? count - 3 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return count / 6;
    default: return count > 5 ? (count - 4) / 2 : 0;
    }
  }

  static std::pair<uint32_t, uint32_t> GeometryWarpShape(D3D12_PRIMITIVE_TOPOLOGY topology) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return {32, 31};
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return {30, 30};
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return {32, 30};
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return {32, 29};
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return {30, 30};
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return {32, 28};
    default: return {32, 32}; // point, line list, line list adj
    }
  }

  /*
   * SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM (mesh_statistics_claim PSOs under a pipeline-statistics query):
   * a claim buffer for `slots` draws (ExecuteIndirect command slots), one SM50_STATISTICS_CLAIM_HEADER per slot
   * (epoch = slot + 1, so slots never share a key) before the per-sample entries they share. The entries cover the
   * pixels a draw can rasterize: the bound targets clipped to the scissor rectangles (D3D12 always scissors). It
   * is zeroed by its own blit pass, so this runs before PreDraw opens the draw's render pass.
   */
  bool PrepareStatisticsClaim(uint32_t slots) {
    statistics_claim_ = {};
    if (!pso_graphics_->mesh_statistics_claim || active_pipeline_statistics_.empty() || !slots)
      return true;
    uint32_t width = 0, height = 0, layers = 1;
    for (UINT i = 0; i < num_scissors; i++) {
      width = std::max<uint32_t>(width, uint32_t(std::clamp<LONG>(scissors[i].right, 0, 16384)));
      height = std::max<uint32_t>(height, uint32_t(std::clamp<LONG>(scissors[i].bottom, 0, 16384)));
    }
    auto clip = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
      auto [heap, index] = GetRenderTargetHeap(device_, handle);
      const auto attachment = heap->GetRenderTarget(index);
      width = std::min<uint32_t>(width, attachment.Width);
      height = std::min<uint32_t>(height, attachment.Height);
      layers = std::max<uint32_t>(layers, attachment.RenderTargetArrayLength);
    };
    for (UINT i = 0; i < num_rtvs; i++)
      if (rtvs[i].ptr)
        clip(rtvs[i]);
    if (dsv.ptr)
      clip(dsv);
    const uint32_t samples = pso_graphics_->EffectiveRasterSampleCount();
    const uint64_t header_bytes = uint64_t(slots) * sizeof(SM50_STATISTICS_CLAIM_HEADER);
    const uint64_t entry_bytes = std::max<uint64_t>(uint64_t(width) * height * layers * samples * 16, 16);
    if (header_bytes + entry_bytes > max_buffer_length_ ||
        header_bytes / 4 > uint64_t(INT32_MAX) || !allocator_->ReserveGPUHeap(header_bytes, 16)) {
      ERR("Pipeline statistics: the claim buffer of a ", width, "x", height, "x", layers, " target exceeds the limit");
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto [headers, headers_offset] = allocator_->AllocateGPUHeap(header_bytes, 16);
    for (uint32_t i = 0; i < slots; i++) {
      auto &header = static_cast<SM50_STATISTICS_CLAIM_HEADER *>(headers)[i];
      header = {};
      header.width = width;
      header.height = height;
      header.layers = layers;
      header.epoch = i + 1;
      header.samples = samples;
      header.entries = int32_t((slots - i) * (sizeof(SM50_STATISTICS_CLAIM_HEADER) / 4));
    }
    auto [buffer, offset] = allocator_->AllocateTempBuffer(header_bytes + entry_bytes, 16);
    if (!buffer || !PreBlit()) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
    auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    copy.type = WMTBlitCommandCopyFromBufferToBuffer;
    allocator_->BindGPUHeap(copy.src, copy.src_offset, headers_offset);
    copy.dst = buffer;
    copy.dst_offset = offset;
    copy.copy_length = header_bytes;
    auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
    fill.type = WMTBlitCommandFillBuffer;
    fill.buffer = buffer;
    fill.offset = offset + header_bytes;
    fill.length = entry_bytes;
    fill.value = 0;
    allocator_->InvalidateCurrentPass();
    statistics_claim_ = {buffer, offset};
    return true;
  }

  /* Binds slot `slot`'s claim header for the PS, and for an MS PSO's mesh stage (its threadgroup key counter);
   * after PreDraw, no-op without a prepared claim. */
  void BindStatisticsClaim(uint32_t slot) {
    if (!statistics_claim_.buffer)
      return;
    const bool mesh_shader = pso_graphics_->mesh_kind == MeshPipelineKind::MeshShader;
    for (auto type : {WMTRenderCommandSetFragmentBuffer, WMTRenderCommandSetMeshBuffer}) {
      if (type == WMTRenderCommandSetMeshBuffer && !mesh_shader)
        continue;
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = type;
      binding.buffer = statistics_claim_.buffer;
      binding.offset = statistics_claim_.offset + uint64_t(slot) * sizeof(SM50_STATISTICS_CLAIM_HEADER);
      binding.index = SM50_BINDING_INDEX_STATISTICS_CLAIM;
    }
  }

  /*
   * Indexed GS strip draw with an active strip cut: d3dalloy_mesh_strip_cut expands it on the GPU into the equivalent
   * list of index values, drawn with the list variant (primitive ids and winding then match D3D11.3 8.10 exactly).
   * The expanded values are already base-vertex-free index values, so the draw keeps BaseVertexLocation.
   */
  void RecordMeshStripCut(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex,
                          UINT start_instance, bool statistics, bool cut) {
    if (!count || !instances)
      return;
    if (!EnsurePredicationPipelines()) {
      RecordError(E_FAIL);
      return;
    }
    D3D_PRIMITIVE_TOPOLOGY list_topology;
    uint32_t vpp, max_vertices;
    switch (topology_) {
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: list_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; vpp = 2; break;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: list_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; vpp = 3; break;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: list_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; vpp = 4; break;
    case kTriangleFan: list_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; vpp = 3; break;
    default: list_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ; vpp = 6; break;
    }
    // Every strip vertex completes at most one primitive (triangle-strip-adj: one per two vertices).
    const uint64_t list_values = uint64_t(count) * vpp;
    if (list_values > UINT32_MAX || list_values * sizeof(uint32_t) > max_buffer_length_) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    max_vertices = uint32_t(list_values);
    struct Control {
      uint64_t max_threadgroups;
      uint32_t topology, index_size, cut_value, count, instances;
      int32_t base_vertex;
      uint32_t start_instance, index_skip, predicate_enabled, predicate_not_equal, statistics_enabled, per_group;
      uint32_t cut_enabled;
    };
    if (!allocator_->ReserveGPUHeap(sizeof(Control) + 64 + 16, 16)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    const uint32_t index_size = !indexed ? 0 : index_type == WMTIndexTypeUInt32 ? 4 : 2;
    const uint64_t first = indexed ? index_offset + uint64_t(start) * index_size : 0;
    const auto [vertex_per_warp, increment] = GeometryWarpShape(list_topology);
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(Control), 16);
    const auto arguments_offset = std::get<1>(allocator_->AllocateGPUHeap(64, 16));
    auto [list, list_offset] = allocator_->AllocateTempBuffer(uint64_t(max_vertices) * 4, 16);
    auto *control = new (control_memory) Control{};
    control->max_threadgroups = device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7) ? UINT64_MAX : 1024;
    control->topology = uint32_t(topology_);
    control->index_size = index_size;
    control->cut_value = index_size == 4 ? 0xffffffffu : 0xffffu;
    control->cut_enabled = cut;
    control->count = count;
    control->instances = instances;
    // Non-indexed: the list holds IA positions and StartVertexLocation becomes the base vertex.
    control->base_vertex = indexed ? base_vertex : INT(start);
    control->start_instance = start_instance;
    control->index_skip = uint32_t(first & 3);
    control->predicate_enabled = predication_enabled_;
    control->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    control->statistics_enabled = statistics;
    control->per_group = increment;
    const auto control_binding = allocator_->ResolveGPUHeap(control_offset);
    WMT::Buffer statistics_buffer = control_binding.buffer;
    uint64_t statistics_offset = control_binding.offset;
    if (statistics) {
      auto [buffer, offset] = PipelineStatisticsCounterBuffer();
      if (!buffer)
        return;
      statistics_buffer = buffer;
      statistics_offset = offset;
    }
    BeginPredicationComputePass(mesh_strip_cut_pipeline_, 1);
    BindGPUHeapPredicationBuffer(control_offset, 0);
    BindPredicationBuffer(indexed ? index_buffer : control_binding.buffer, indexed ? first & ~uint64_t(3) : 0, 1);
    BindPredicationBuffer(list, list_offset, 2);
    BindGPUHeapPredicationBuffer(arguments_offset, 3);
    BindPredicationBuffer(statistics_buffer, statistics_offset, 4);
    BindGPUHeapPredicationBuffer(predication_enabled_ ? predication_snapshot_offset_ : control_offset, 5);
    DispatchPredicationKernel();

    const bool stream_output = pso_graphics_->stream_output.active;
    // Upper bound of the expansion's primitive ids: every strip vertex completes at most one primitive.
    if (stream_output && !PrepareGeometryStreamOutput(count, instances))
      return;
    const D3D12_PRIMITIVE_TOPOLOGY strip_topology = topology_;
    topology_ = list_topology;
    const bool capture_pixels = (!pso_graphics_->stream_output.active ||
            pso_graphics_->stream_output.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM) &&
            (pso_graphics_->owned_conservative || (pso_graphics_->owned_distance_interpolation || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) || CoarseShadingRateState() != 0);
    const uint32_t key = MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_UINT32, false, statistics) |
        (capture_pixels ? kMeshMSAACapture : 0);
    const auto status = PreDraw(false, false, key);
    topology_ = strip_topology;
    if (status == DrawCallStatus::Invalid)
      return;
    BindStatisticsClaim(0);
    if (stream_output && !BindGeometryStreamOutput())
      return;
    auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect>();
    cmd.type = WMTRenderCommandD3DalloyGeometryDrawIndexedIndirect;
    cmd.index_buffer = list;
    cmd.index_buffer_offset = list_offset;
    allocator_->BindGPUHeap(cmd.indirect_args_buffer, cmd.indirect_args_offset, arguments_offset);
    cmd.imm_draw_arguments = cmd.indirect_args_buffer;
    allocator_->BindGPUHeap(cmd.dispatch_args_buffer, cmd.dispatch_args_offset, arguments_offset + 8 * sizeof(uint32_t));
    cmd.vertex_per_warp = vertex_per_warp;
    if (capture_pixels) RecordMeshPixelReplay(key, count, instances);
  }

  /*
   * VS+HS+DS+GS (tsgs-design): pass 1 runs VS+HS / DS capture (rasterization off) and writes every workload's DS
   * vertices and primitives to slot (instance * patches + patch) * W + rank of the capture scratch; a compute pass
   * compacts the slot primitive counts in D3D order into a flat primitive list and the GS object grid; pass 2 runs
   * the capture-fetching object / GS mesh pipeline indirectly. The scratch is sized for the worst case of the
   * CPU-known patch and instance counts, so no capture write can overflow it.
   */
  void RecordTessellationGeometryDraw(bool indexed, UINT count, UINT instances, uint32_t control_points,
                                      SM50_INDEX_BUFFER_FORMAT format, bool statistics, uint64_t argument_offset,
                                      const void *argument_data, size_t argument_size) {
    const auto &layout = pso_graphics_->tess_capture_layout;
    const uint32_t patches = count / control_points;
    if (!patches)
      return;
    if (!EnsurePredicationPipelines()) {
      RecordError(E_FAIL);
      return;
    }
    const uint64_t slots = uint64_t(instances) * patches * layout.max_workloads;
    const uint64_t vertex_base = (slots + 3) & ~uint64_t(3);
    const uint64_t index_base = vertex_base + slots * layout.max_vertices * layout.vertex_registers * 4;
    const uint64_t scratch_bytes = 4 * (index_base + slots * layout.max_primitives * layout.primitive_vertices);
    const uint64_t list_bytes = 4 * (4 + slots * layout.max_primitives);
    const uint64_t max_length = max_buffer_length_;
    if (slots * layout.max_primitives > UINT32_MAX || scratch_bytes > max_length || list_bytes > max_length) {
      ERR("GS after tessellation: a draw of ", patches, " patches x ", instances, " instances exceeds the capture limit");
      RecordError(E_OUTOFMEMORY);
      return;
    }
    // D3D10_SB_PRIMITIVE point 1, line 2, triangle 3: GS input primitives per object threadgroup.
    const uint32_t per_group = pso_graphics_->mesh_input_primitive == 1 ? 32
                               : pso_graphics_->mesh_input_primitive == 2 ? 16 : 10;
    struct CompactControl {
      uint32_t slot_count, max_primitives, per_group, reserved;
    };
    const size_t heap_bytes = sizeof(SM50_TESS_CAPTURE_CONTROL) + sizeof(CompactControl) + 16 + argument_size + 64;
    if (!allocator_->ReserveGPUHeap(heap_bytes, 16)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    auto [capture_memory, capture_offset] = allocator_->AllocateGPUHeap(sizeof(SM50_TESS_CAPTURE_CONTROL), 16);
    auto *capture = new (capture_memory) SM50_TESS_CAPTURE_CONTROL{};
    capture->slot_count = uint32_t(slots);
    capture->patch_count = patches;
    auto [compact_memory, compact_offset] = allocator_->AllocateGPUHeap(sizeof(CompactControl), 16);
    new (compact_memory) CompactControl{uint32_t(slots), layout.max_primitives, per_group, 0};
    auto [dispatch_memory, dispatch_offset] = allocator_->AllocateGPUHeap(16, 16);
    memset(dispatch_memory, 0, 16);
    if (argument_offset == UINT64_MAX) {
      auto [memory, offset] = allocator_->AllocateGPUHeap(argument_size, 16);
      memcpy(memory, argument_data, argument_size);
      argument_offset = offset;
    }
    auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(scratch_bytes, 16);
    auto [list, list_offset] = allocator_->AllocateTempBuffer(list_bytes, 16);

    // Zero the per-slot primitive counts: slots without a workload keep 0.
    BeginPredicationComputePass(tess_capture_clear_pipeline_, 64);
    BindGPUHeapPredicationBuffer(compact_offset, 0);
    BindPredicationBuffer(scratch, scratch_offset, 1);
    {
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {uint32_t((slots + 63) / 64), 1, 1};
      allocator_->InvalidateCurrentPass();
    }

    // Pass 1: VS+HS object, DS capture mesh.
    if (PreDraw(false, false, MeshVariantKey(format, false, statistics)) == DrawCallStatus::Invalid)
      return;
    const auto capture_binding = allocator_->ResolveGPUHeap(capture_offset);
    const std::pair<WMT::Buffer, uint64_t> pass1[] = {{scratch, scratch_offset},
                                                      {capture_binding.buffer, capture_binding.offset}};
    const uint8_t pass1_indices[] = {SM50_BINDING_INDEX_TESS_CAPTURE, SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL};
    for (uint32_t i = 0; i < 2; i++) {
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetMeshBuffer;
      binding.buffer = pass1[i].first;
      binding.offset = pass1[i].second;
      binding.index = pass1_indices[i];
    }
    const uint32_t threads_per_patch = pso_graphics_->tess_threads_per_patch;
    const uint32_t patch_per_group = 32 / threads_per_patch;
    const uint32_t groups = (patches - 1) / patch_per_group + 1;
    if (indexed) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed>();
      cmd.type = WMTRenderCommandD3DalloyTessellationMeshDrawIndexed;
      cmd.draw_arguments_offset = argument_offset;
      cmd.index_buffer = index_buffer;
      cmd.index_buffer_offset = index_offset;
      cmd.instance_count = instances;
      cmd.threads_per_patch = threads_per_patch;
      cmd.patch_per_group = patch_per_group;
      cmd.patch_per_mesh_instance = groups;
    } else {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_tessellation_mesh_draw>();
      cmd.type = WMTRenderCommandD3DalloyTessellationMeshDraw;
      cmd.draw_arguments_offset = argument_offset;
      cmd.instance_count = instances;
      cmd.threads_per_patch = threads_per_patch;
      cmd.patch_per_group = patch_per_group;
      cmd.patch_per_mesh_instance = groups;
    }

    // Compaction: flat primitive list in D3D order and the GS object grid.
    BeginPredicationComputePass(tess_capture_compact_pipeline_, 1024);
    BindGPUHeapPredicationBuffer(compact_offset, 0);
    BindPredicationBuffer(scratch, scratch_offset, 1);
    BindPredicationBuffer(list, list_offset, 2);
    BindGPUHeapPredicationBuffer(dispatch_offset, 3);
    DispatchPredicationKernel();

    // Pass 2: capture-fetching object, GS mesh. Primitive ids of the flat list bound the stream-output keys.
    const bool stream_output = pso_graphics_->stream_output.active;
    if (stream_output && !PrepareGeometryStreamOutput(uint32_t(slots * layout.max_primitives), 1))
      return;
    const uint32_t key = MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false, statistics) |
                         kMeshTessellationGeometryPass |
                         ((!stream_output || pso_graphics_->stream_output.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM) &&
                          (pso_graphics_->owned_conservative || (pso_graphics_->owned_distance_interpolation || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) ||
                           CoarseShadingRateState() != 0) ? kMeshMSAACapture : 0);
    if (PreDraw(false, false, key) == DrawCallStatus::Invalid)
      return;
    BindStatisticsClaim(0);
    const std::pair<WMT::Buffer, uint64_t> pass2[] = {
        {scratch, scratch_offset}, {list, list_offset}, {capture_binding.buffer, capture_binding.offset}};
    const uint8_t pass2_indices[] = {SM50_BINDING_INDEX_TESS_CAPTURE, SM50_BINDING_INDEX_TESS_CAPTURE_LIST,
                                     SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL};
    for (uint32_t i = 0; i < 3; i++) {
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = pass2[i].first;
      binding.offset = pass2[i].second;
      binding.index = pass2_indices[i];
    }
    if (stream_output && !BindGeometryStreamOutput())
      return;
    auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups_indirect>();
    draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
    allocator_->BindGPUHeap(draw.indirect_args_buffer, draw.indirect_args_offset, dispatch_offset);
    draw.object_threadgroup_size = {32, 1, 1};
    draw.mesh_threadgroup_size = {1, 1, 1};
    owned_graphics::Trace("draw_mesh", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id, ",\"count\":", count,
                          ",\"instances\":", instances, ",\"tessellation_geometry\":1");
    if (key & kMeshMSAACapture) RecordMeshPixelReplay(key, uint32_t(slots * layout.max_primitives), 1);
  }

  /*
   * GS or HS/DS draw: an object+mesh dispatch over D3DALLOY_DRAW_(INDEXED_)ARGUMENTS in the GPU heap.
   * Under predication the arguments are the predicated copy; a false predicate zeroes the vertex
   * count, so every object thread is inactive and nothing is emitted or counted.
   */
  void RecordMeshPixelReplay(uint32_t key, uint32_t primitives, uint32_t instances) {
    const auto *capture = pso_graphics_->GetMeshPipeline(key);
    if (!capture) { RecordError(E_FAIL); return; }
    const uint64_t slots = uint64_t(primitives) * instances * capture->capture_layout.max_workloads;
    const uint64_t records = slots * capture->capture_layout.max_primitives *
        capture->capture_layout.vertices_per_primitive;
    if (slots > UINT32_MAX || records > UINT32_MAX ||
        slots * capture->capture_layout.capture_dwords_per_group * 4 > max_buffer_length_) {
      RecordError(E_OUTOFMEMORY); return;
    }
    SM50_MSAA_MESH_CONTROL control{primitives, instances, 0, uint32_t(slots)};
    RecordMSAADraw(uint32_t(records), 1, 0, 0, 0, false, false, capture, &control);
  }

  void RecordMeshDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance) {
    if (pso_graphics_->mesh_kind == MeshPipelineKind::MeshShader) {
      // A mesh-shader pipeline is drawn with DispatchMesh only.
      RecordError(E_INVALIDARG);
      return;
    }
    uint32_t control_points = 0;
    WMTPrimitiveType primitive;
    to_metal_primitive_type(topology_, primitive, control_points);
    const bool tessellation = pso_graphics_->mesh_kind == MeshPipelineKind::Tessellation ||
                              pso_graphics_->mesh_kind == MeshPipelineKind::TessellationGeometry;
    const bool strip = topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ||
                       topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
                       topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                       (topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ || topology_ == kTriangleFan);
    if (tessellation != (control_points != 0)) {
      RecordError(E_INVALIDARG);
      return;
    }
    const bool statistics = !active_pipeline_statistics_.empty();
    if (!PrepareStatisticsClaim(1))
      return;
    if (indexed && !index_buffer) {
      RecordError(E_INVALIDARG);
      return;
    }
    const bool index32 = index_type == WMTIndexTypeUInt32;
    const bool cut = indexed && strip && !tessellation &&
                     ((ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF && !index32) ||
                      (ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF && index32));
    if (cut || (topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ || topology_ == kTriangleFan)) {
      RecordMeshStripCut(indexed, count, instances, start, base_vertex, start_instance, statistics, cut);
      return;
    }
    const auto format = !indexed ? SM50_INDEX_BUFFER_FORMAT_NONE
                        : index_type == WMTIndexTypeUInt32 ? SM50_INDEX_BUFFER_FORMAT_UINT32
                                                           : SM50_INDEX_BUFFER_FORMAT_UINT16;
    const bool capture_pixels = (!pso_graphics_->stream_output.active ||
            pso_graphics_->stream_output.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM) &&
            (pso_graphics_->owned_conservative || (pso_graphics_->owned_distance_interpolation || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) || CoarseShadingRateState() != 0);
    const uint32_t key = MeshVariantKey(format, strip && !tessellation, statistics) |
        (capture_pixels ? kMeshMSAACapture : 0);
    if (capture_pixels) allocator_->InvalidateCurrentPass();
    // D3D12_DRAW_(INDEXED_)ARGUMENTS match the converter's draw_(indexed_)arguments layout.
    const size_t argument_size = indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    D3D12_DRAW_INDEXED_ARGUMENTS indexed_arguments{count, instances, start, base_vertex, start_instance};
    D3D12_DRAW_ARGUMENTS arguments{count, instances, start, start_instance};
    const void *argument_data = indexed ? static_cast<const void *>(&indexed_arguments) : &arguments;
    uint64_t argument_offset;
    if (predication_enabled_) {
      argument_offset = PreparePredicatedArguments(argument_data, argument_size);
      if (argument_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    if (pso_graphics_->mesh_kind == MeshPipelineKind::TessellationGeometry) {
      RecordTessellationGeometryDraw(indexed, count, instances, control_points, format, statistics,
                                     predication_enabled_ ? argument_offset : UINT64_MAX, argument_data,
                                     argument_size);
      return;
    }
    const bool stream_output = pso_graphics_->stream_output.active;
    if (stream_output) {
      const uint64_t keys = tessellation
          ? uint64_t(count / control_points) * pso_graphics_->tess_capture_layout.max_workloads
          : MeshInputPrimitives(topology_, count);
      if (keys > UINT32_MAX) { RecordError(E_OUTOFMEMORY); return; }
      if (!PrepareGeometryStreamOutput(uint32_t(keys), instances)) return;
    }
    if (PreDraw(false, false, key) == DrawCallStatus::Invalid)
      return;
    BindStatisticsClaim(0);
    if (stream_output && !BindGeometryStreamOutput())
      return;
    if (!predication_enabled_) {
      if (!allocator_->ReserveGPUHeap(argument_size, 16)) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      auto [memory, offset] = allocator_->AllocateGPUHeap(argument_size, 16);
      memcpy(memory, argument_data, argument_size);
      argument_offset = offset;
    }
    const uint64_t index_base = index_offset;
    if (tessellation) {
      const uint32_t threads_per_patch = pso_graphics_->tess_threads_per_patch;
      const uint32_t patch_per_group = 32 / threads_per_patch;
      const uint32_t patches = count / control_points;
      if (!patches)
        return;
      const uint32_t groups = (patches - 1) / patch_per_group + 1;
      if (indexed) {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed>();
        cmd.type = WMTRenderCommandD3DalloyTessellationMeshDrawIndexed;
        cmd.draw_arguments_offset = argument_offset;
        cmd.index_buffer = index_buffer;
        cmd.index_buffer_offset = index_base;
        cmd.instance_count = instances;
        cmd.threads_per_patch = threads_per_patch;
        cmd.patch_per_group = patch_per_group;
        cmd.patch_per_mesh_instance = groups;
      } else {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_tessellation_mesh_draw>();
        cmd.type = WMTRenderCommandD3DalloyTessellationMeshDraw;
        cmd.draw_arguments_offset = argument_offset;
        cmd.instance_count = instances;
        cmd.threads_per_patch = threads_per_patch;
        cmd.patch_per_group = patch_per_group;
        cmd.patch_per_mesh_instance = groups;
      }
    } else {
      const auto [vertex_per_warp, increment] = GeometryWarpShape(topology_);
      const uint32_t warps = (count - 1) / increment + 1;
      if (indexed) {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_geometry_draw_indexed>();
        cmd.type = WMTRenderCommandD3DalloyGeometryDrawIndexed;
        cmd.draw_arguments_offset = argument_offset;
        cmd.index_buffer = index_buffer;
        cmd.index_buffer_offset = index_base;
        cmd.warp_count = warps;
        cmd.instance_count = instances;
        cmd.vertex_per_warp = vertex_per_warp;
      } else {
        auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_d3dalloy_geometry_draw>();
        cmd.type = WMTRenderCommandD3DalloyGeometryDraw;
        cmd.draw_arguments_offset = argument_offset;
        cmd.warp_count = warps;
        cmd.instance_count = instances;
        cmd.vertex_per_warp = vertex_per_warp;
      }
    }
    owned_graphics::Trace("draw_mesh", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id, ",\"count\":", count,
                          ",\"instances\":", instances, ",\"tessellation\":", tessellation ? 1 : 0);
    if (capture_pixels) {
      const uint32_t primitives = tessellation ? count / control_points : MeshInputPrimitives(topology_, count);
      RecordMeshPixelReplay(key, primitives, instances);
    }
  }

  void STDMETHODCALLTYPE
  DrawIndexedInstanced(
      UINT IndexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, INT BaseVertexLocation,
      UINT StartInstanceLocation
  ) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::DrawIndexedInstanced, IndexCountPerInstance,
                     InstanceCount, StartVertexLocation, BaseVertexLocation, StartInstanceLocation))
      return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError(E_FAIL);
      return;
    }
    if (!IndexCountPerInstance || !InstanceCount)
      return;
    if (ReplayPerView([&] {
          DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation,
                               StartInstanceLocation);
        }))
      return;
    if (pso_graphics_ && pso_graphics_->mesh_kind != MeshPipelineKind::None)
      return RecordMeshDraw(true, IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation,
                            StartInstanceLocation);
    if (OmitPendingDraw())
      return;
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    const bool native_msaa = CanUseNativeMSAA();
    const bool msaa_execution = RequiresMSAAExecution() && !native_msaa;
    if (msaa_execution)
      allocator_->InvalidateCurrentPass();
    // A stream-output draw is issued over its generated IA positions (StreamOutputDrawData); the
    // VS reads the application's indices itself and applies BaseVertexLocation.
    const bool stream_output = pso_graphics_ &&
        (pso_graphics_->stream_output.active || !active_pipeline_statistics_.empty());
    const bool strip = primitive_type == WMTPrimitiveTypeLineStrip || primitive_type == WMTPrimitiveTypeTriangleStrip;
    const bool native_cut = pso_graphics_ &&
        ((index_type == WMTIndexTypeUInt16 && ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
         (index_type == WMTIndexTypeUInt32 && ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF));
    const bool pull_indices = !stream_output && strip && !native_cut;
    uint64_t predicated_offset = 0;
    if (predication_enabled_) {
      if (pull_indices) {
        const D3D12_DRAW_ARGUMENTS arguments = {IndexCountPerInstance, InstanceCount, 0, StartInstanceLocation};
        predicated_offset = PreparePredicatedArguments(&arguments, sizeof(arguments));
      } else {
        const D3D12_DRAW_INDEXED_ARGUMENTS arguments = {IndexCountPerInstance, InstanceCount,
                                                        stream_output ? 0u : StartVertexLocation,
                                                        stream_output ? 0 : BaseVertexLocation, StartInstanceLocation};
        predicated_offset = PreparePredicatedArguments(&arguments, sizeof(arguments));
      }
      if (predicated_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    if (stream_output &&
        !PrepareStreamOutputDraw(IndexCountPerInstance, InstanceCount, true, StartVertexLocation, BaseVertexLocation))
      return;
    DrawCallStatus status = PreDraw(false, pull_indices, ~0u, native_msaa);
    if (status == DrawCallStatus::Invalid)
      return;
    RecordAsyncTargets();
    if (native_msaa) owned_graphics::Trace("native_msaa_draw_record", ",\"list\":", owned_id,
        ",\"recording\":", owned_recording, ",\"encoder\":", allocator_->encoder_current->id,
        ",\"indexed\":1,\"index_pull\":", pull_indices, ",\"count\":", IndexCountPerInstance,
        ",\"instances\":", InstanceCount);
    try {
      if (native_msaa) {
        uint64_t offset = 0;
        auto *allocation = device_->LookupBufferByVA(index_buffer_address, &offset);
        if (!allocation) { RecordError(E_INVALIDARG); return; }
        if (allocator_->recording_head_->root_allocation_set.insert(allocation).second)
          allocator_->recording_head_->root_allocations.emplace_back(allocation);
      }
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      if (render->submission_group_safe && render->submission_buffers.size() == kRecordedSubmissionBufferBudget) {
        render->submission_snapshot_budget = true; render->submission_group_safe = false;
      }
      if (render->submission_group_safe)
        render->submission_buffers.push_back({index_buffer_address +
            uint64_t(StartVertexLocation) * (index_type == WMTIndexTypeUInt32 ? 4 : 2),
            uint64_t(IndexCountPerInstance) * (index_type == WMTIndexTypeUInt32 ? 4 : 2), false, WMTMetal4StageVertex});
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
    if (stream_output && !BindStreamOutputDraw())
      return;
    if (msaa_execution) {
      RecordMSAADraw(IndexCountPerInstance, InstanceCount, StartVertexLocation, BaseVertexLocation,
                     StartInstanceLocation, true);
      so_bound_.reset();
      return;
    }
    if (pull_indices) {
      if (!index_buffer || !allocator_->ReserveGPUHeap(sizeof(SM50_INDEX_PULL_CONTROL), 16)) {
        RecordError(index_buffer ? E_OUTOFMEMORY : E_INVALIDARG);
        return;
      }
      const uint32_t index_size = index_type == WMTIndexTypeUInt32 ? 4 : 2;
      const uint64_t first = index_offset + uint64_t(StartVertexLocation) * index_size;
      auto [memory, offset] = allocator_->AllocateGPUHeap(sizeof(SM50_INDEX_PULL_CONTROL), 16);
      new (memory) SM50_INDEX_PULL_CONTROL{index_size, uint32_t((first & 3) / 2), BaseVertexLocation, 0};
      const auto control_binding = allocator_->ResolveGPUHeap(offset);
      for (auto [buffer, byte_offset, slot] : {
               std::tuple{index_buffer, first & ~uint64_t(3), uint32_t(SM50_BINDING_INDEX_VERTEX_INDICES)},
               std::tuple{control_binding.buffer, control_binding.offset,
                          uint32_t(SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL)}}) {
        auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        binding.type = WMTRenderCommandSetVertexBuffer;
        binding.buffer = buffer;
        binding.offset = byte_offset;
        binding.index = slot;
      }
      if (predication_enabled_) {
        auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
        draw.type = WMTRenderCommandDrawIndirect;
        draw.primitive_type = primitive_type;
        allocator_->BindGPUHeap(draw.indirect_args_buffer, draw.indirect_args_offset, predicated_offset);
      } else {
        auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
        draw.type = WMTRenderCommandDraw;
        draw.primitive_type = primitive_type;
        draw.vertex_start = 0;
        draw.vertex_count = IndexCountPerInstance;
        draw.instance_count = InstanceCount;
        draw.base_instance = StartInstanceLocation;
      }
      return;
    }
    const WMTIndexType draw_index_type = stream_output ? WMTIndexTypeUInt32 : index_type;
    const WMT::Buffer draw_index_buffer = stream_output ? WMT::Buffer(so_bound_->positions) : index_buffer;
    const uint64_t draw_index_offset =
        stream_output ? 0 : index_offset + StartVertexLocation * (index_type == WMTIndexTypeUInt32 ? 4 : 2);
    if (stream_output) {
      so_bound_.reset();
      if (!IndexCountPerInstance || !InstanceCount)
        return; // nothing to draw; the assembly still runs and writes nothing
    }
    if (predication_enabled_) {
      auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
      cmd_draw.type = WMTRenderCommandDrawIndexedIndirect;
      cmd_draw.primitive_type = primitive_type;
      cmd_draw.index_type = draw_index_type;
      cmd_draw.index_buffer = draw_index_buffer;
      cmd_draw.index_buffer_offset = stream_output ? 0 : index_offset;
      allocator_->BindGPUHeap(cmd_draw.indirect_args_buffer, cmd_draw.indirect_args_offset, predicated_offset);
    } else {
      auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed>();
      cmd_draw.type = WMTRenderCommandDrawIndexed;
      cmd_draw.primitive_type = primitive_type;
      cmd_draw.index_type = draw_index_type;
      cmd_draw.index_count = IndexCountPerInstance;
      cmd_draw.index_buffer = draw_index_buffer;
      cmd_draw.index_buffer_offset = draw_index_offset;
      cmd_draw.instance_count = InstanceCount;
      cmd_draw.base_vertex = stream_output ? 0 : BaseVertexLocation;
      cmd_draw.base_instance = StartInstanceLocation;
    }
    owned_graphics::Trace("draw_indexed", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"indices\":", IndexCountPerInstance, ",\"instances\":", InstanceCount);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  uint64_t
  EncodeRootArgument(MTLD3D12RootSignature *pRootSig, uint64_t const *pStaging, UINT Count = 1) {
    if (!pRootSig->UploadQwords || !Count)
      return 0;
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(uint64_t(sizeof(uint64_t)) * pRootSig->UploadQwords * Count, 64);
    if (!Ptr) {
      RecordError(E_OUTOFMEMORY);
      return UINT64_MAX;
    }
    for (unsigned i = 0; i < Count; i++)
      memcpy(
          reinterpret_cast<uint64_t *>(Ptr) + i * pRootSig->UploadQwords, pStaging,
          pRootSig->UploadQwords * sizeof(uint64_t)
      );
    return Offset;
  }

  /* Static-sampler uploads of this recording. The payload is immutable per signature and the upload heap is
   * only rewound by allocator Reset (which needs a new recording), so one upload serves every later pass. */
  struct StaticSamplerUpload {
    Com<MTLD3D12RootSignature, false> signature;
    uint64_t offset = 0;
  };
  std::array<StaticSamplerUpload, 4> static_sampler_uploads_;
  uint8_t static_sampler_upload_next_ = 0;

  /* `Reuse` false forces a fresh upload, for callers that require it to share a heap chunk with a following
   * allocation. */
  uint64_t
  EncodeStaticSamplers(MTLD3D12RootSignature *pRootSig, bool Reuse = true) {
    const uint64_t static_sampler_encode_size = uint64_t(sizeof(uint64_t)) * pRootSig->NumStaticSamplers * 8;
    if (!static_sampler_encode_size)
      return 0;
    if (Reuse)
      for (const auto &upload : static_sampler_uploads_)
        if (upload.signature.ptr() == pRootSig)
          return upload.offset;
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(static_sampler_encode_size, 64);
    if (!Ptr) {
      RecordError(E_OUTOFMEMORY);
      return UINT64_MAX;
    }
    memcpy(Ptr, pRootSig->EncodedStaticSamplers, static_sampler_encode_size);
    if (Reuse)
      static_sampler_uploads_[static_sampler_upload_next_++ % static_sampler_uploads_.size()] = {pRootSig, Offset};
    return Offset;
  }

  bool
  PreDispatch(bool SkipResourceBinding = false) {
    const bool statistics = !active_pipeline_statistics_.empty();
    if (pso_compute_ && pso_compute_->uses_atomic64) allocator_->InvalidateCurrentPass();
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute) {
      allocator_->EndSchedulingPass();
      auto compute = allocator_->AllocatePass<ComputeEncoderData>();
      compute->submission_access_complete = true;
      compute->type = EncoderType::Compute;
      compute->cmd_head.type = WMTComputeCommandNop;
      compute->cmd_head.next.set(0);
      compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
      dirty_state_.set(DirtyState::ComputePipelineState);
    }

    if (!pso_compute_)
        return false;
    // Every dispatch path, direct or indirect, joins a deferred native compute link here.
    if (!pso_compute_->WaitNativePipeline()) {
      RecordError(E_FAIL);
      return false;
    }
    auto *submission_compute = static_cast<ComputeEncoderData *>(allocator_->encoder_current);
    submission_compute->submission_access_complete &= !SkipResourceBinding && !statistics &&
        !predication_enabled_ && !pso_compute_->uses_atomic64 &&
        CaptureSubmissionAccess(submission_compute->submission_buffers, submission_compute->submission_tables,
            rootsig_compute_.ptr(), rootarg_compute_staging_, *pso_compute_.ptr(), WMTMetal4StageDispatch,
            submission_compute->submission_snapshot_budget, submission_compute->submission_root_srv_opaque, true);

    if (dirty_state_.test(DirtyState::ComputePipelineState)) {
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = statistics ? pso_compute_->GetPipelineStatisticsPipelineState(false) : pso_compute_->pso;
      if (!cmd_setpso.pso) {
        RecordError(E_FAIL);
        return false;
      }
      cmd_setpso.threadgroup_size = pso_compute_->threadgroup_size;
      static_cast<ComputeEncoderData *>(allocator_->encoder_current)->RetainPipeline(WMT::ComputePipelineState{cmd_setpso.pso});
      const auto &constants = statistics ? pso_compute_->pipeline_statistics_compute_constants : pso_compute_->compute_constants;
      if (constants.buffer) {
        auto &binding = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        binding.type = WMTComputeCommandSetBuffer;
        binding.buffer = constants.buffer;
        binding.offset = constants.offset;
        binding.index = SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS;
        auto &retained = static_cast<ComputeEncoderData *>(allocator_->encoder_current)->immutable_constant_buffers;
        if (std::none_of(retained.begin(), retained.end(), [&](const auto &buffer) {
              return buffer.handle == constants.buffer.handle;
            }))
          retained.push_back(constants.buffer);
      }
      if (statistics) {
        auto [buffer, offset] = PipelineStatisticsCounterBuffer();
        if (!buffer)
          return false;
        auto &binding = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        binding.type = WMTComputeCommandSetBuffer;
        binding.buffer = buffer;
        binding.offset = offset;
        binding.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
      }
      dirty_state_.clr(DirtyState::ComputePipelineState);
    }

    const bool capture_swizzle_tables = dirty_state_.test(DirtyState::ComputeRootArguments);
    if (capture_swizzle_tables && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_);
        if (Offset == UINT64_MAX)
          return false;
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        allocator_->BindGPUHeap(cmd_argbuf.buffer, cmd_argbuf.offset, Offset);
        cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      dirty_state_.clr(DirtyState::ComputeRootArguments);
    }

    if (dirty_state_.test(DirtyState::ComputeRootSignature) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeStaticSamplers(rootsig_compute_.ptr());
        if (Offset == UINT64_MAX)
          return false;
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        allocator_->BindGPUHeap(cmd_argbuf.buffer, cmd_argbuf.offset, Offset);
        cmd_argbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::ComputeRootSignature);
    }

    if (capture_swizzle_tables || pso_compute_->uses_atomic64)
      CaptureStandardSwizzleTables(
          static_cast<ComputeEncoderData *>(allocator_->encoder_current)->swizzle_tables,
          rootsig_compute_.ptr(), rootarg_compute_staging_, pso_compute_->uses_atomic64, pso_compute_.ptr());
    auto *compute = static_cast<ComputeEncoderData *>(allocator_->encoder_current);
    compute->atomic64_phase |= pso_compute_->uses_atomic64;
    if (pso_compute_->uses_atomic64) {
      compute->atomic64_pipeline = pso_compute_.ptr();
      CaptureAtomic64Roots(compute->atomic64_roots, rootsig_compute_.ptr(), rootarg_compute_staging_, pso_compute_.ptr());
    }

    return true;
  }

  void STDMETHODCALLTYPE
  Dispatch(UINT X, UINT Y, UINT Z) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::Dispatch, X, Y, Z))
      return;
    if (!IsRecording() || (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT &&
                           type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)) {
      RecordError(E_FAIL);
      return;
    }
    if (RejectDispatchInRenderPass())
      return;
    uint64_t predicated_offset = 0;
    if (predication_enabled_) {
      const D3D12_DISPATCH_ARGUMENTS arguments = {X, Y, Z};
      predicated_offset = PreparePredicatedArguments(&arguments, sizeof(arguments));
      if (predicated_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    if (!PreDispatch())
      return;

    if (predication_enabled_) {
      auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
      cmd_dispatch.type = WMTComputeCommandDispatchIndirect;
      allocator_->BindGPUHeap(cmd_dispatch.indirect_args_buffer, cmd_dispatch.indirect_args_offset, predicated_offset);
    } else {
      auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      cmd_dispatch.type = WMTComputeCommandDispatch;
      cmd_dispatch.size = {X, Y, Z};
    }
    owned_graphics::Trace("dispatch", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"x\":", X, ",\"y\":", Y, ",\"z\":", Z);
    if (pso_compute_->uses_atomic64) allocator_->InvalidateCurrentPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void RecordStandardSwizzleUse(MTLD3D12Resource *resource, bool to_canonical, bool writable_msaa = false) {
    if (!resource || (!resource->swizzle_layout && !writable_msaa))
      return;
    allocator_->InvalidateCurrentPass();
    auto *record = allocator_->AllocatePass<StandardSwizzleEncoderData>();
    record->type = EncoderType::StandardSwizzle;
    record->resource = resource;
    record->to_canonical = to_canonical;
    resource->AddRefPrivate();
    allocator_->InvalidateCurrentPass();
  }

  // Whole subresource (or whole texture for UINT32_MAX) of an ordinary
  // texture; sparse, swizzled and multisample textures stay untracked.
  void RecordBlitTextureAccess(MTLD3D12Resource *resource, uint32_t level, uint32_t slice, bool writes) {
    auto *blit = static_cast<BlitEncoderData *>(allocator_->encoder_current);
    if (!blit->submission_access_complete) return;
    if (!resource || !resource->texture || resource->swizzle_layout || resource->reserved ||
        blit->submission_textures.size() == kRecordedSubmissionBufferBudget) {
      blit->submission_access_complete = false;
      return;
    }
    const auto &texture = resource->texture;
    TextureViewKey key{};
    const uint32_t mips = texture->miplevelCount(), slices = texture->arrayLength();
    key.mip_start = level == UINT32_MAX ? 0 : level;
    key.mip_end = level == UINT32_MAX ? mips : level + 1;
    key.array_start = slice == UINT32_MAX ? 0 : slice;
    key.array_end = slice == UINT32_MAX ? slices : slice + 1;
    try {
      blit->submission_textures.push_back({texture, key, writes});
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); }
  }

  // A placed-footprint copy touches (conservatively) its whole buffer: an
  // upload reads it and writes the texture, a readback the reverse.
  void RecordBlitFootprintAccess(MTLD3D12Resource *buffer, MTLD3D12Resource *texture, uint32_t level, uint32_t slice,
                                 bool readback) {
    auto *blit = static_cast<BlitEncoderData *>(allocator_->encoder_current);
    if (!blit->submission_access_complete) return;
    if (!buffer || !buffer->buffer || blit->submission_uploads.size() == kRecordedSubmissionBufferBudget) {
      blit->submission_access_complete = false;
      return;
    }
    try {
      blit->submission_uploads.push_back({buffer->buffer->current()->gpuAddress(), UINT64_MAX, readback, 0});
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
    RecordBlitTextureAccess(texture, level, slice, !readback);
  }

  bool
  PreBlit() {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Blit) {
      allocator_->EndSchedulingPass();
      auto render = allocator_->AllocatePass<BlitEncoderData>();
      render->type = EncoderType::Blit;
      render->cmd_head.type = WMTBlitCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
    }
    return true;
  }

  void STDMETHODCALLTYPE
  CopyBufferRegion(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 ByteCount
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (!pDstBuffer || !pSrcBuffer)
      return;
    if (!QueryObjectOwnedByThisDevice(pDstBuffer) || !QueryObjectOwnedByThisDevice(pSrcBuffer)) {
      RecordError();
      return;
    }
    const auto dst_desc = pDstBuffer->GetDesc();
    const auto src_desc = pSrcBuffer->GetDesc();
    if (dst_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || src_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !RangeFits(DstOffset, ByteCount, dst_desc.Width) || !RangeFits(SrcOffset, ByteCount, src_desc.Width) || !ByteCount)
      return;
    auto *dst_resource = static_cast<MTLD3D12Resource *>(pDstBuffer);
    auto *src_resource = static_cast<MTLD3D12Resource *>(pSrcBuffer);
    if (!dst_resource->buffer || !src_resource->buffer)
      return;
    if (pDstBuffer == pSrcBuffer && DstOffset < SrcOffset + ByteCount && SrcOffset < DstOffset + ByteCount) {
      RecordError();
      return;
    }
    if (!RecordImplicitUse(pSrcBuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
        !RecordImplicitUse(pDstBuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST))
      return;
    if (predication_enabled_) {
      if (!EnsurePredicationPipelines()) {
        RecordError(E_FAIL);
        return;
      }
      UINT64 copied = 0;
      while (copied < ByteCount) {
        const UINT chunk = UINT(std::min<UINT64>(ByteCount - copied, UINT32_MAX));
        EncodePredicatedBufferCopy(
            src_resource->buffer->current()->buffer(), SrcOffset + copied,
            dst_resource->buffer->current()->buffer(), DstOffset + copied, chunk
        );
        copied += chunk;
      }
      return;
    }
    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd_cp.src = src_resource->buffer->current()->buffer();
    try {
      auto &uses = static_cast<BlitEncoderData *>(allocator_->encoder_current)->submission_buffers;
      uses.push_back({src_resource->buffer->current()->gpuAddress() + SrcOffset, ByteCount, false});
      uses.push_back({dst_resource->buffer->current()->gpuAddress() + DstOffset, ByteCount, true});
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
    cmd_cp.dst = dst_resource->buffer->current()->buffer();
    cmd_cp.src_offset = SrcOffset;
    cmd_cp.dst_offset = DstOffset;
    cmd_cp.copy_length = ByteCount;
    owned_graphics::Trace("copy_buffer", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"src\":", static_cast<MTLD3D12Resource *>(pSrcBuffer)->owned_id,
                          ",\"dst\":", static_cast<MTLD3D12Resource *>(pDstBuffer)->owned_id,
                          ",\"bytes\":", ByteCount);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  CopyTextureRegion(
      const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc,
      const D3D12_BOX *pSrcBox
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (!pDst || !pSrc || !pDst->pResource || !pSrc->pResource)
      return;
    if (!QueryObjectOwnedByThisDevice(pDst->pResource) || !QueryObjectOwnedByThisDevice(pSrc->pResource)) {
      RecordError();
      return;
    }
    // Feedback maps are opaque: only CopyResource copies them (SamplerFeedback spec, Copying).
    if (static_cast<MTLD3D12Resource *>(pDst->pResource)->feedback ||
        static_cast<MTLD3D12Resource *>(pSrc->pResource)->feedback) {
      RecordError();
      return;
    }
    if ((pDst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
         pDst->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) ||
        (pSrc->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
         pSrc->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT))
      return;

    auto src_desc = pSrc->pResource->GetDesc();
    auto dst_desc = pDst->pResource->GetDesc();
    uint32_t src_level = 0, src_slice = 0, src_planar = 0;
    uint32_t dst_level = 0, dst_slice = 0, dst_planar = 0;
    D3D12_BOX src_box, full_src_box;
    D3D12_BOX dst_extent;
    PlacedFootprintInfo src_footprint_info, dst_footprint_info;
    UINT src_block_width = 1, dst_block_width = 1;
    UINT src_block_height = 1, dst_block_height = 1;

    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      if (!ValidatePlacedFootprint(device_->GetMTLDevice(), src_desc, pSrc->PlacedFootprint, src_footprint_info)) {
        RecordError();
        return;
      }
      src_block_width = src_footprint_info.block_width;
      src_block_height = src_footprint_info.block_height;
      full_src_box = {
          0, 0, 0,
          pSrc->PlacedFootprint.Footprint.Width,
          pSrc->PlacedFootprint.Footprint.Height,
          pSrc->PlacedFootprint.Footprint.Depth
      };
    } else {
      if (!GetSubresourceInfo(
              device_->GetMTLDevice(), src_desc, pSrc->SubresourceIndex, src_level, src_slice, src_planar
          ))
        return;
      MTL_DXGI_FORMAT_DESC src_resource_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_resource_format)))
        return;
      const auto storage = resource_contract::GetEmulatedTextureStorage(src_desc.Format);
      src_block_width = src_resource_format.Flag & MTL_DXGI_FORMAT_BC ? 4u :
          (storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
           storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8) ? 2u : 1u;
      src_block_height = src_resource_format.Flag & MTL_DXGI_FORMAT_BC ? 4u : 1u;
      full_src_box = GetResourceExtent(src_desc, src_level);
    }

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      if (!ValidatePlacedFootprint(device_->GetMTLDevice(), dst_desc, pDst->PlacedFootprint, dst_footprint_info)) {
        RecordError();
        return;
      }
      dst_block_width = dst_footprint_info.block_width;
      dst_block_height = dst_footprint_info.block_height;
      dst_extent = {
          0, 0, 0,
          pDst->PlacedFootprint.Footprint.Width,
          pDst->PlacedFootprint.Footprint.Height,
          pDst->PlacedFootprint.Footprint.Depth
      };
    } else {
      if (!GetSubresourceInfo(
              device_->GetMTLDevice(), dst_desc, pDst->SubresourceIndex, dst_level, dst_slice, dst_planar
          ))
        return;
      MTL_DXGI_FORMAT_DESC dst_resource_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, dst_resource_format)))
        return;
      const auto storage = resource_contract::GetEmulatedTextureStorage(dst_desc.Format);
      dst_block_width = dst_resource_format.Flag & MTL_DXGI_FORMAT_BC ? 4u :
          (storage == resource_contract::EmulatedTextureStorage::PackedR8G8_B8G8 ||
           storage == resource_contract::EmulatedTextureStorage::PackedG8R8_G8B8) ? 2u : 1u;
      dst_block_height = dst_resource_format.Flag & MTL_DXGI_FORMAT_BC ? 4u : 1u;
      dst_extent = GetResourceExtent(dst_desc, dst_level);
    }

    const D3D12_BOX src_logical_extent = full_src_box;
    const D3D12_BOX dst_logical_extent = dst_extent;
    // D3D12 boxes address physical BC blocks; Metal textures keep logical sizes.
    if (src_block_width == 4 && src_block_height == 4) {
      full_src_box.right = (full_src_box.right + 3) & ~3u;
      full_src_box.bottom = (full_src_box.bottom + 3) & ~3u;
    }
    if (dst_block_width == 4 && dst_block_height == 4) {
      dst_extent.right = (dst_extent.right + 3) & ~3u;
      dst_extent.bottom = (dst_extent.bottom + 3) & ~3u;
    }

    // Packed formats retain their existing logical-edge alignment rule.
    D3D12_BOX src_edge_extent = full_src_box;
    D3D12_BOX dst_edge_extent = dst_extent;
    if (pSrcBox) {
      src_box = *pSrcBox;
    } else if (
        pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
    ) {
      // Copy the shared physical block region, then clamp each native texture
      // transfer independently below (also covers BC tail mips).
      src_box = OverlapBox(full_src_box, dst_extent, DstX, DstY, DstZ);
      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        src_edge_extent = src_box;
      }
      if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        dst_edge_extent = {
            0, 0, 0,
            DstX + (src_box.right - src_box.left),
            DstY + (src_box.bottom - src_box.top),
            DstZ + (src_box.back - src_box.front)
        };
      }
    } else {
      src_box = full_src_box;
    }

    if (!BoxFits(src_box, full_src_box) || !DestinationFits(DstX, DstY, DstZ, src_box, dst_extent) ||
        !BlockCopyAligned(src_block_width, src_block_height, src_box, src_edge_extent,
                          DstX, DstY, dst_edge_extent) ||
        src_block_width != dst_block_width || src_block_height != dst_block_height ||
        (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
         pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
         src_footprint_info.bytes_per_block != dst_footprint_info.bytes_per_block)) {
      RecordError();
      return;
    }
    if (pSrc->pResource == pDst->pResource) {
      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
          pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
          pSrc->SubresourceIndex == pDst->SubresourceIndex)
        return;
    }

    const DXGI_FORMAT src_copy_format = pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
        ? src_desc.Format
        : pSrc->PlacedFootprint.Footprint.Format;
    WMTSize source_size{src_box.right - src_box.left, src_box.bottom - src_box.top,
                        src_box.back - src_box.front};
    WMTSize destination_size = source_size;
    if (src_block_width == 4 && src_block_height == 4) {
      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        source_size.width = std::min<UINT64>(source_size.width, src_logical_extent.right - src_box.left);
        source_size.height = std::min<UINT64>(source_size.height, src_logical_extent.bottom - src_box.top);
      }
      if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        destination_size.width = std::min<UINT64>(destination_size.width, dst_logical_extent.right - DstX);
        destination_size.height = std::min<UINT64>(destination_size.height, dst_logical_extent.bottom - DstY);
      }
    }
    const DXGI_FORMAT dst_copy_format = pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
        ? dst_desc.Format : pDst->PlacedFootprint.Footprint.Format;
    if ((resource_contract::GetEmulatedTextureStorage(src_copy_format) != resource_contract::EmulatedTextureStorage::None ||
         resource_contract::GetEmulatedTextureStorage(dst_copy_format) != resource_contract::EmulatedTextureStorage::None) &&
        !resource_contract::AreCopyCompatibleFormats(src_copy_format, dst_copy_format)) {
      RecordError(E_INVALIDARG);
      return;
    }
    MTL_DXGI_FORMAT_DESC native_copy_format{};
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_copy_format, native_copy_format))) {
      RecordError();
      return;
    }
    const auto physical_copy_format = GetD3D12PhysicalPixelFormat(src_copy_format);
    if (physical_copy_format != WMTPixelFormatInvalid)
      native_copy_format.PixelFormat = physical_copy_format;
    if (src_desc.SampleDesc.Count > 1 &&
        DepthStencilPlanarFlags(static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture->pixelFormat()) &&
        pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      DXGI_FORMAT copy_format = src_desc.Format;
      if (copy_format == DXGI_FORMAT_R32_TYPELESS) copy_format = DXGI_FORMAT_D32_FLOAT;
      else if (copy_format == DXGI_FORMAT_R16_TYPELESS) copy_format = DXGI_FORMAT_D16_UNORM;
      else if (copy_format == DXGI_FORMAT_R24G8_TYPELESS) copy_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
      else if (copy_format == DXGI_FORMAT_R32G8X24_TYPELESS) copy_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      D3D12_RECT rect{LONG(src_box.left), LONG(src_box.top), LONG(src_box.right), LONG(src_box.bottom)};
      ResolveTextureRegion(pDst->pResource, pDst->SubresourceIndex, DstX, DstY, pSrc->pResource,
                           pSrc->SubresourceIndex, &rect, copy_format, D3D12_RESOLVE_MODE_DECOMPRESS, true);
      return;
    }
    auto *src_resource = static_cast<MTLD3D12Resource *>(pSrc->pResource);
    auto *dst_resource = static_cast<MTLD3D12Resource *>(pDst->pResource);
    const bool src_volume = src_resource->reserved && src_resource->reserved->volumeLayout();
    const bool dst_volume = dst_resource->reserved && dst_resource->reserved->volumeLayout();
    if (src_volume || dst_volume) {
      if (!resource_contract::AreCopyCompatibleFormats(src_copy_format, dst_copy_format) ||
          !RecordImplicitUse(pSrc->pResource, pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ?
              pSrc->SubresourceIndex : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
          !RecordImplicitUse(pDst->pResource, pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ?
              pDst->SubresourceIndex : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST))
        return;
      if (predication_enabled_ && !EnsurePredicationPipelines()) {
        RecordError(E_FAIL);
        return;
      }
      const uint32_t unit_bytes = native_copy_format.Flag & MTL_DXGI_FORMAT_BC ?
          native_copy_format.BlockSize : native_copy_format.BytesPerTexel;
      if (!unit_bytes) {
        RecordError();
        return;
      }
      auto native_view = [&](MTLD3D12Resource *resource, bool volume, uint32_t level, uint32_t slice,
                             uint32_t &native_level, uint32_t &native_slice) {
        auto &texture = resource->texture;
        native_level = volume ? 0 : level;
        native_slice = volume ? 0 : slice;
        auto result = texture->current()->texture();
        if (texture->pixelFormat() != native_copy_format.PixelFormat) {
          TextureViewDescriptor view{};
          view.format = native_copy_format.PixelFormat;
          view.type = texture->textureType();
          view.firstMiplevel = volume ? 0 : level;
          view.miplevelCount = 1;
          view.firstArraySlice = volume ? 0 : slice;
          view.arraySize = volume ? texture->arrayLength() : 1;
          result = texture->view(texture->createView(view)).texture;
          native_level = native_slice = 0;
        }
        return result;
      };
      uint32_t source_level = 0, source_slice = 0, destination_level = 0, destination_slice = 0;
      WMT::Texture source{}, destination{};
      const bool source_buffer = pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      const bool destination_buffer = pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      if (!source_buffer)
        source = native_view(src_resource, src_volume, src_level, src_slice, source_level, source_slice);
      if (!destination_buffer)
        destination = native_view(dst_resource, dst_volume, dst_level, dst_slice, destination_level, destination_slice);
      if ((!source_buffer && !source) || (!destination_buffer && !destination)) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      // A canonical volume may have been written through another placed/shared
      // alias. Import before reading it, and preserve untouched destination tiles.
      if (!source_buffer) RecordStandardSwizzleUse(src_resource, false);
      if (!destination_buffer) RecordStandardSwizzleUse(dst_resource, false);
      for (UINT z = 0; z < src_box.back - src_box.front; z++)
        for (UINT y = 0; y < src_box.bottom - src_box.top; y += src_block_height)
          for (UINT x = 0; x < src_box.right - src_box.left;) {
            const UINT sx = src_box.left + x, sy = src_box.top + y, sz = src_box.front + z;
            const UINT dx = DstX + x, dy = DstY + y, dz = DstZ + z;
            UINT units = (src_box.right - src_box.left - x + src_block_width - 1) / src_block_width;
            WMTOrigin source_origin{sx, sy, sz}, destination_origin{dx, dy, dz};
            uint32_t ss = source_slice, ds = destination_slice;
            if (src_volume) {
              const auto address = src_resource->reserved->VolumeAddress(src_level, sx, sy, sz);
              if (!address) { RecordError(); return; }
              source_origin = {address->x, address->y, 0}; ss = address->slice;
              units = std::min(units, address->units);
            }
            if (dst_volume) {
              const auto address = dst_resource->reserved->VolumeAddress(dst_level, dx, dy, dz);
              if (!address) { RecordError(); return; }
              destination_origin = {address->x, address->y, 0}; ds = address->slice;
              units = std::min(units, address->units);
            }
            const UINT width = units * src_block_width;
            WMTSize source_chunk{width, src_block_height, 1}, destination_chunk = source_chunk;
            if (!src_volume && !source_buffer) {
              source_chunk.width = std::min<UINT64>(width, src_logical_extent.right - sx);
              source_chunk.height = std::min<UINT64>(src_block_height, src_logical_extent.bottom - sy);
            }
            if (!dst_volume && !destination_buffer) {
              destination_chunk.width = std::min<UINT64>(width, dst_logical_extent.right - dx);
              destination_chunk.height = std::min<UINT64>(src_block_height, dst_logical_extent.bottom - dy);
            }
            const UINT64 row_bytes = UINT64(units) * unit_bytes;
            const UINT64 pitch = (row_bytes + 255) & ~UINT64(255);
            const UINT64 source_offset = source_buffer ? pSrc->PlacedFootprint.Offset +
                UINT64(sz) * src_footprint_info.slice_pitch + UINT64(sy / src_block_height) *
                pSrc->PlacedFootprint.Footprint.RowPitch + UINT64(sx / src_block_width) * unit_bytes : 0;
            const UINT64 destination_offset = destination_buffer ? pDst->PlacedFootprint.Offset +
                UINT64(dz) * dst_footprint_info.slice_pitch + UINT64(dy / src_block_height) *
                pDst->PlacedFootprint.Footprint.RowPitch + UINT64(dx / src_block_width) * unit_bytes : 0;
            if (!PreBlit()) return;
            if (!source_buffer && !destination_buffer && !predication_enabled_ &&
                source_chunk.width == destination_chunk.width && source_chunk.height == destination_chunk.height) {
              auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
              copy.type = WMTBlitCommandCopyFromTextureToTexture;
              copy.src = source; copy.src_level = source_level; copy.src_slice = ss;
              copy.src_origin = source_origin; copy.src_size = source_chunk;
              copy.dst = destination; copy.dst_level = destination_level; copy.dst_slice = ds;
              copy.dst_origin = destination_origin;
            } else {
              WMT::Buffer readable{};
              UINT64 readable_offset = source_offset;
              UINT64 readable_pitch = source_buffer ? pSrc->PlacedFootprint.Footprint.RowPitch : 0;
              if (source_buffer) {
                readable = src_resource->buffer->current()->buffer();
              } else {
                std::tie(readable, readable_offset) = allocator_->AllocateTempBuffer(pitch, 256);
                readable_pitch = pitch;
                if (!readable) { RecordError(E_OUTOFMEMORY); return; }
                auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
                copy.type = WMTBlitCommandCopyFromTextureToBuffer;
                copy.src = source; copy.level = source_level; copy.slice = ss;
                copy.origin = source_origin; copy.size = source_chunk;
                copy.dst = readable; copy.offset = readable_offset;
                copy.bytes_per_row = pitch;
                copy.bytes_per_image = !src_volume && src_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? pitch : 0;
              }
              if (destination_buffer) {
                auto writable = dst_resource->buffer->current()->buffer();
                if (predication_enabled_) {
                  EncodePredicatedBufferCopy(readable, readable_offset, writable, destination_offset, row_bytes);
                } else {
                  auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
                  copy.type = WMTBlitCommandCopyFromBufferToBuffer;
                  copy.src = readable; copy.src_offset = readable_offset;
                  copy.dst = writable; copy.dst_offset = destination_offset; copy.copy_length = row_bytes;
                }
              } else {
                if (predication_enabled_) {
                  auto [saved, saved_offset] = allocator_->AllocateTempBuffer(pitch, 256);
                  if (!saved) { RecordError(E_OUTOFMEMORY); return; }
                  auto &save = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
                  save.type = WMTBlitCommandCopyFromTextureToBuffer;
                  save.src = destination; save.level = destination_level; save.slice = ds;
                  save.origin = destination_origin; save.size = destination_chunk;
                  save.dst = saved; save.offset = saved_offset;
                  save.bytes_per_row = pitch;
                  save.bytes_per_image = !dst_volume && dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? pitch : 0;
                  EncodePredicatedBufferCopy(readable, readable_offset, saved, saved_offset, row_bytes);
                  readable = saved; readable_offset = saved_offset; readable_pitch = pitch;
                  if (!PreBlit()) return;
                }
                auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
                copy.type = WMTBlitCommandCopyFromBufferToTexture;
                copy.src = readable; copy.src_offset = readable_offset;
                copy.bytes_per_row = readable_pitch;
                copy.bytes_per_image = !dst_volume && dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? readable_pitch : 0;
                copy.dst = destination; copy.level = destination_level; copy.slice = ds;
                copy.origin = destination_origin; copy.size = destination_chunk;
              }
            }
            x += width;
          }
      if (!destination_buffer) RecordStandardSwizzleUse(dst_resource, true);
      return;
    }
    WMT::Texture source_native_texture{};
    WMT::Texture destination_native_texture{};
    const bool native_copy_is_depth = DepthStencilPlanarFlags(native_copy_format.PixelFormat);
    uint32_t source_native_level = src_level, source_native_slice = src_slice;
    uint32_t destination_native_level = dst_level, destination_native_slice = dst_slice;
    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      auto &texture = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
      source_native_texture = texture->current()->texture();
      // Placed formats describe raw storage, not a castable depth texture view.
      if (texture->pixelFormat() != native_copy_format.PixelFormat &&
          !native_copy_is_depth && !DepthStencilPlanarFlags(texture->pixelFormat())) {
        TextureViewDescriptor view{};
        view.format = native_copy_format.PixelFormat;
        view.type = texture->textureType();
        view.firstMiplevel = src_level;
        view.miplevelCount = 1;
        view.firstArraySlice = src_slice;
        view.arraySize = 1;
        source_native_texture = texture->view(texture->createView(view)).texture;
        source_native_level = 0;
        source_native_slice = 0;
      }
    }
    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      auto &texture = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
      destination_native_texture = texture->current()->texture();
      if (texture->pixelFormat() != native_copy_format.PixelFormat &&
          !native_copy_is_depth && !DepthStencilPlanarFlags(texture->pixelFormat())) {
        TextureViewDescriptor view{};
        view.format = native_copy_format.PixelFormat;
        view.type = texture->textureType();
        view.firstMiplevel = dst_level;
        view.miplevelCount = 1;
        view.firstArraySlice = dst_slice;
        view.arraySize = 1;
        destination_native_texture = texture->view(texture->createView(view)).texture;
        destination_native_level = 0;
        destination_native_slice = 0;
      }
    }
    if ((pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && !source_native_texture) ||
        (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && !destination_native_texture)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    if (!RecordImplicitUse(pSrc->pResource,
                           pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
                               ? pSrc->SubresourceIndex : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_SOURCE) ||
        !RecordImplicitUse(pDst->pResource,
                           pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
                               ? pDst->SubresourceIndex : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_DEST))
      return;
    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pSrc->pResource), false);
    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pDst->pResource), false);

    if (predication_enabled_) {
      if (!EnsurePredicationPipelines()) {
        RecordError(E_FAIL);
        return;
      }
      if ((pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src_desc.SampleDesc.Count > 1) ||
          (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && dst_desc.SampleDesc.Count > 1)) {
        if (pSrc->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
            pDst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
          RecordError(E_INVALIDARG);
          return;
        }
        auto *source_resource = static_cast<MTLD3D12Resource *>(pSrc->pResource);
        auto *destination_resource = static_cast<MTLD3D12Resource *>(pDst->pResource);
        if (!source_resource->texture || !destination_resource->texture) {
          RecordError(E_INVALIDARG);
          return;
        }
        allocator_->InvalidateCurrentPass();
        auto *copy = allocator_->AllocatePass<PredicatedTextureCopyEncoderData>();
        copy->type = EncoderType::PredicatedTextureCopy;
        copy->source = source_native_texture;
        copy->destination = destination_native_texture;
        const auto predicate = allocator_->ResolveGPUHeap(predication_snapshot_offset_);
        copy->predicate = predicate.buffer;
        copy->predicate_value = static_cast<const uint64_t *>(predicate.memory);
        copy->predicate_offset = predicate.offset;
        copy->source_origin = {src_box.left, src_box.top, src_box.front};
        copy->destination_origin = {DstX, DstY, DstZ};
        copy->size = {src_box.right - src_box.left, src_box.bottom - src_box.top,
                      src_box.back - src_box.front};
        copy->source_level = source_native_level;
        copy->source_slice = source_native_slice;
        copy->destination_level = destination_native_level;
        copy->destination_slice = destination_native_slice;
        copy->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
        allocator_->InvalidateCurrentPass();
        RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pDst->pResource), true);
        return;
      }

      MTL_DXGI_FORMAT_DESC copy_format{};
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_copy_format, copy_format))) {
        RecordError();
        return;
      }
      const UINT block_width = src_block_width;
      const UINT block_height = src_block_height;
      const auto emulated_storage = resource_contract::GetEmulatedTextureStorage(src_copy_format);
      const UINT bytes_per_block = (src_planar == 1 || dst_planar == 1) ? 1u : copy_format.BytesPerTexel;
      const UINT64 width = src_box.right - src_box.left;
      const UINT64 rows = (UINT64(src_box.bottom - src_box.top) + block_height - 1) / block_height;
      const UINT64 depth = src_box.back - src_box.front;
      UINT64 logical_row_bytes = 0, row_bytes = 0, staging_pitch = 0, staging_slice = 0, staging_size = 0;
      if (!resource_contract::CheckedMul((width + block_width - 1) / block_width, bytes_per_block,
                                         logical_row_bytes)) {
        RecordError();
        return;
      }
      if (emulated_storage == resource_contract::EmulatedTextureStorage::None) {
        row_bytes = logical_row_bytes;
      } else if (!resource_contract::CheckedMul(width,
                     emulated_storage == resource_contract::EmulatedTextureStorage::RGB32 ? 16u : 4u, row_bytes)) {
        RecordError();
        return;
      }
      if (!resource_contract::CheckedAlign(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, staging_pitch) ||
          !resource_contract::CheckedMul(staging_pitch, rows, staging_slice) ||
          !resource_contract::CheckedMul(staging_slice, depth, staging_size) || staging_size > SIZE_MAX) {
        RecordError();
        return;
      }
      auto conditional_copy = [&](WMT::Buffer source, UINT64 source_offset, WMT::Buffer destination,
                                  UINT64 destination_offset, UINT64 bytes) {
        while (bytes) {
          const UINT chunk = UINT(std::min<UINT64>(bytes, UINT32_MAX));
          EncodePredicatedBufferCopy(source, source_offset, destination, destination_offset, chunk);
          source_offset += chunk;
          destination_offset += chunk;
          bytes -= chunk;
        }
      };
      const WMTBlitOption src_option = src_planar ? WMTBlitOptionStencilFromDepthStencil :
          (copy_format.PlanarCount > 1 ? WMTBlitOptionDepthFromDepthStencil : WMTBlitOptionNone);
      const WMTBlitOption dst_option = dst_planar ? WMTBlitOptionStencilFromDepthStencil :
          (copy_format.PlanarCount > 1 ? WMTBlitOptionDepthFromDepthStencil : WMTBlitOptionNone);

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        auto src_texture = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        auto [source_staging, source_staging_offset] = allocator_->AllocateTempBuffer(staging_size, 256);
        if (!PreBlit()) {
          RecordError();
          return;
        }
        auto &stage_source = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
        stage_source.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        stage_source.src = source_native_texture;
        stage_source.level = source_native_level;
        stage_source.slice = source_native_slice;
        stage_source.origin = {src_box.left, src_box.top, src_box.front};
        stage_source.size = source_size;
        stage_source.dst = source_staging;
        stage_source.offset = source_staging_offset;
        stage_source.bytes_per_row = staging_pitch;
        stage_source.bytes_per_image = src_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? staging_slice : 0;
        stage_source.options = src_option;
        allocator_->InvalidateCurrentPass();

        if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
          auto dst_texture = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
          auto [destination_staging, destination_staging_offset] = allocator_->AllocateTempBuffer(staging_size, 256);
          if (!PreBlit()) {
            RecordError();
            return;
          }
          auto &stage_destination = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          stage_destination.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          stage_destination.src = destination_native_texture;
          stage_destination.level = destination_native_level;
          stage_destination.slice = destination_native_slice;
          stage_destination.origin = {DstX, DstY, DstZ};
          stage_destination.size = destination_size;
          stage_destination.dst = destination_staging;
          stage_destination.offset = destination_staging_offset;
          stage_destination.bytes_per_row = staging_pitch;
          stage_destination.bytes_per_image = dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? staging_slice : 0;
          stage_destination.options = dst_option;
          allocator_->InvalidateCurrentPass();
          conditional_copy(source_staging, source_staging_offset, destination_staging, destination_staging_offset, staging_size);
          if (!PreBlit()) {
            RecordError();
            return;
          }
          auto &write_destination = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          write_destination.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          write_destination.src = destination_staging;
          write_destination.src_offset = destination_staging_offset;
          write_destination.bytes_per_row = staging_pitch;
          write_destination.bytes_per_image = dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? staging_slice : 0;
          write_destination.size = destination_size;
          write_destination.dst = destination_native_texture;
          write_destination.level = destination_native_level;
          write_destination.slice = destination_native_slice;
          write_destination.origin = {DstX, DstY, DstZ};
          write_destination.options = dst_option;
        } else {
          auto dst_buffer = static_cast<MTLD3D12Resource *>(pDst->pResource)->buffer->current()->buffer();
          WMT::Buffer readable_staging = source_staging;
          UINT64 readable_offset = source_staging_offset;
          UINT64 readable_pitch = staging_pitch, readable_slice = staging_slice;
          if (emulated_storage != resource_contract::EmulatedTextureStorage::None) {
            UINT64 logical_pitch = 0, logical_slice = 0, logical_size = 0;
            if (!resource_contract::CheckedAlign(logical_row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
                                                 logical_pitch) ||
                !resource_contract::CheckedMul(logical_pitch, rows, logical_slice) ||
                !resource_contract::CheckedMul(logical_slice, depth, logical_size) ||
                logical_pitch > UINT_MAX || staging_pitch > UINT_MAX || logical_size > SIZE_MAX) {
              RecordError(E_OUTOFMEMORY);
              return;
            }
            auto [compacted, compacted_offset] = allocator_->AllocateTempBuffer(logical_size, 256);
            if (!compacted) {
              RecordError(E_OUTOFMEMORY);
              return;
            }
            for (UINT64 z = 0; z < depth; ++z)
              if (!ConvertEmulatedTextureRows(emulated_storage, false, source_staging,
                      source_staging_offset + z * staging_slice, UINT(staging_pitch), compacted,
                      compacted_offset + z * logical_slice, UINT(logical_pitch), UINT(width), UINT(rows)))
                return;
            readable_staging = compacted;
            readable_offset = compacted_offset;
            readable_pitch = logical_pitch;
            readable_slice = logical_slice;
          }
          const UINT64 destination_base = pDst->PlacedFootprint.Offset + UINT64(DstZ) * dst_footprint_info.slice_pitch +
              (UINT64(DstY) / block_height) * pDst->PlacedFootprint.Footprint.RowPitch +
              (UINT64(DstX) / block_width) * dst_footprint_info.bytes_per_block;
          if (!EncodePredicatedPitchedCopy(readable_staging, readable_offset, readable_pitch, readable_slice, dst_buffer,
                  destination_base, pDst->PlacedFootprint.Footprint.RowPitch, dst_footprint_info.slice_pitch,
                  logical_row_bytes, rows, depth))
            for (UINT64 z = 0; z < depth; ++z)
              for (UINT64 row = 0; row < rows; ++row) {
                const UINT64 source_offset = readable_offset + z * readable_slice + row * readable_pitch;
                const UINT64 destination_offset = destination_base + z * dst_footprint_info.slice_pitch +
                    row * pDst->PlacedFootprint.Footprint.RowPitch;
                conditional_copy(readable_staging, source_offset, dst_buffer, destination_offset, logical_row_bytes);
              }
        }
      } else {
        auto src_buffer = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer->current()->buffer();
        if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
          auto dst_texture = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
          auto [destination_staging, destination_staging_offset] = allocator_->AllocateTempBuffer(staging_size, 256);
          if (!PreBlit()) {
            RecordError();
            return;
          }
          auto &stage_destination = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          stage_destination.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          stage_destination.src = destination_native_texture;
          stage_destination.level = destination_native_level;
          stage_destination.slice = destination_native_slice;
          stage_destination.origin = {DstX, DstY, DstZ};
          stage_destination.size = destination_size;
          stage_destination.dst = destination_staging;
          stage_destination.offset = destination_staging_offset;
          stage_destination.bytes_per_row = staging_pitch;
          stage_destination.bytes_per_image = dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? staging_slice : 0;
          stage_destination.options = dst_option;
          allocator_->InvalidateCurrentPass();
          if (emulated_storage != resource_contract::EmulatedTextureStorage::None) {
            if (staging_pitch > UINT_MAX || width > UINT_MAX || rows > UINT_MAX) {
              RecordError(E_OUTOFMEMORY);
              return;
            }
            auto [expanded, expanded_offset] = allocator_->AllocateTempBuffer(staging_size, 256);
            if (!expanded) {
              RecordError(E_OUTOFMEMORY);
              return;
            }
            const UINT64 source_base = pSrc->PlacedFootprint.Offset +
                UINT64(src_box.front) * src_footprint_info.slice_pitch +
                (UINT64(src_box.top) / block_height) * pSrc->PlacedFootprint.Footprint.RowPitch +
                (UINT64(src_box.left) / block_width) * src_footprint_info.bytes_per_block;
            for (UINT64 z = 0; z < depth; ++z)
              if (!ConvertEmulatedTextureRows(emulated_storage, true, src_buffer,
                      source_base + z * src_footprint_info.slice_pitch, pSrc->PlacedFootprint.Footprint.RowPitch,
                      expanded, expanded_offset + z * staging_slice, UINT(staging_pitch), UINT(width), UINT(rows)))
                return;
            conditional_copy(expanded, expanded_offset, destination_staging, destination_staging_offset, staging_size);
          } else {
            const UINT64 source_base = pSrc->PlacedFootprint.Offset + UINT64(src_box.front) * src_footprint_info.slice_pitch +
                (UINT64(src_box.top) / block_height) * pSrc->PlacedFootprint.Footprint.RowPitch +
                (UINT64(src_box.left) / block_width) * src_footprint_info.bytes_per_block;
            if (!EncodePredicatedPitchedCopy(src_buffer, source_base, pSrc->PlacedFootprint.Footprint.RowPitch,
                    src_footprint_info.slice_pitch, destination_staging, destination_staging_offset, staging_pitch,
                    staging_slice, row_bytes, rows, depth))
              for (UINT64 z = 0; z < depth; ++z)
                for (UINT64 row = 0; row < rows; ++row) {
                  const UINT64 source_offset = source_base + z * src_footprint_info.slice_pitch +
                      row * pSrc->PlacedFootprint.Footprint.RowPitch;
                  conditional_copy(src_buffer, source_offset, destination_staging,
                                   destination_staging_offset + z * staging_slice + row * staging_pitch, row_bytes);
                }
          }
          if (!PreBlit()) {
            RecordError();
            return;
          }
          auto &write_destination = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          write_destination.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          write_destination.src = destination_staging;
          write_destination.src_offset = destination_staging_offset;
          write_destination.bytes_per_row = staging_pitch;
          write_destination.bytes_per_image = dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? staging_slice : 0;
          write_destination.size = destination_size;
          write_destination.dst = destination_native_texture;
          write_destination.level = destination_native_level;
          write_destination.slice = destination_native_slice;
          write_destination.origin = {DstX, DstY, DstZ};
          write_destination.options = dst_option;
        } else {
          auto dst_buffer = static_cast<MTLD3D12Resource *>(pDst->pResource)->buffer->current()->buffer();
          const UINT64 source_base = pSrc->PlacedFootprint.Offset + UINT64(src_box.front) * src_footprint_info.slice_pitch +
              (UINT64(src_box.top) / block_height) * pSrc->PlacedFootprint.Footprint.RowPitch +
              (UINT64(src_box.left) / block_width) * src_footprint_info.bytes_per_block;
          const UINT64 destination_base = pDst->PlacedFootprint.Offset + UINT64(DstZ) * dst_footprint_info.slice_pitch +
              (UINT64(DstY) / block_height) * pDst->PlacedFootprint.Footprint.RowPitch +
              (UINT64(DstX) / block_width) * dst_footprint_info.bytes_per_block;
          // Rows of one native buffer keep their sequential per-row order: one grid has no defined order.
          if (src_buffer.handle == dst_buffer.handle ||
              !EncodePredicatedPitchedCopy(src_buffer, source_base, pSrc->PlacedFootprint.Footprint.RowPitch,
                  src_footprint_info.slice_pitch, dst_buffer, destination_base, pDst->PlacedFootprint.Footprint.RowPitch,
                  dst_footprint_info.slice_pitch, logical_row_bytes, rows, depth))
            for (UINT64 z = 0; z < depth; ++z)
              for (UINT64 row = 0; row < rows; ++row) {
                const UINT64 source_offset = source_base + z * src_footprint_info.slice_pitch +
                    row * pSrc->PlacedFootprint.Footprint.RowPitch;
                const UINT64 destination_offset = destination_base + z * dst_footprint_info.slice_pitch +
                    row * pDst->PlacedFootprint.Footprint.RowPitch;
                conditional_copy(src_buffer, source_offset, dst_buffer, destination_offset, logical_row_bytes);
              }
        }
      }
      allocator_->InvalidateCurrentPass();
      if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pDst->pResource), true);
      return;
    }

    if (!PreBlit())
      return;

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
      if (!dst)
        return;

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", dst_desc.Format);
        return;
      }
      auto dst_planar_count = dst_format.PlanarCount;

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pSrc->PlacedFootprint.Footprint.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", pSrc->PlacedFootprint.Footprint.Format);
          return;
        }

        const auto block_width = src_footprint_info.block_width;
        const auto src_depth_pitch = dst->textureType() == WMTTextureType3D ? src_footprint_info.slice_pitch : 0;

        WMT::Buffer copy_source = src->current()->buffer();
        UINT64 copy_offset = pSrc->PlacedFootprint.Offset + (src_box.left / block_width) * src_format.BytesPerTexel +
                             (src_box.top / src_footprint_info.block_height) * pSrc->PlacedFootprint.Footprint.RowPitch +
                             src_box.front * src_footprint_info.slice_pitch;
        UINT copy_pitch = pSrc->PlacedFootprint.Footprint.RowPitch;
        uint64_t copy_depth_pitch = src_depth_pitch;
        const auto storage = resource_contract::GetEmulatedTextureStorage(dst_desc.Format);
        if (storage != resource_contract::EmulatedTextureStorage::None) {
          if (!resource_contract::AreCopyCompatibleFormats(dst_desc.Format, pSrc->PlacedFootprint.Footprint.Format)) {
            RecordError(E_INVALIDARG);
            return;
          }
          const UINT width = src_box.right - src_box.left;
          const UINT height = src_box.bottom - src_box.top;
          const UINT depth = src_box.back - src_box.front;
          const UINT native_bytes = storage == resource_contract::EmulatedTextureStorage::RGB32 ? 16 : 4;
          uint64_t pitch = 0, slice = 0, size = 0;
          if (!resource_contract::CheckedMul(width, native_bytes, pitch) ||
              !resource_contract::CheckedAlign(pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch) ||
              !resource_contract::CheckedMul(pitch, height, slice) ||
              !resource_contract::CheckedMul(slice, depth, size) || pitch > UINT_MAX || size > SIZE_MAX) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(size, 256);
          if (!scratch) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          for (UINT z = 0; z < depth; ++z) {
            if (!ConvertEmulatedTextureRows(storage, true, copy_source,
                    copy_offset + uint64_t(z) * src_footprint_info.slice_pitch, copy_pitch,
                    scratch, scratch_offset + uint64_t(z) * slice, UINT(pitch), width, height))
              return;
          }
          copy_source = scratch;
          copy_offset = scratch_offset;
          copy_pitch = UINT(pitch);
          copy_depth_pitch = slice;
          if (!PreBlit())
            return;
        }
        if (!dst_planar && (dst_format.Flag & MTL_DXGI_FORMAT_EMULATED_D24)) {
          const UINT width = src_box.right - src_box.left, height = src_box.bottom - src_box.top;
          UINT64 pitch = 0, size = 0;
          if (!resource_contract::CheckedAlign(UINT64(width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch) ||
              !resource_contract::CheckedMul(pitch, height, size) || size > SIZE_MAX) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(size, 256);
          if (!scratch) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          if (!ConvertD24Plane(copy_source, copy_offset, copy_pitch,
                               scratch, scratch_offset, UINT(pitch), width, height, true))
            return;
          copy_source = scratch;
          copy_offset = scratch_offset;
          copy_pitch = UINT(pitch);
          if (!PreBlit())
            return;
        }

        // Plain uploads read the placed buffer and write one subresource. A
        // converted upload reads scratch written by an earlier pass instead.
        const bool tracked_upload = copy_source == src->current()->buffer() && dst_planar_count == 1;
        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>(tracked_upload);
        cmd_cp.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        cmd_cp.src = copy_source;
        cmd_cp.src_offset = copy_offset;
        cmd_cp.bytes_per_row = copy_pitch;
        cmd_cp.bytes_per_image = copy_depth_pitch;
        cmd_cp.size = destination_size;
        cmd_cp.dst = destination_native_texture;
        cmd_cp.level = destination_native_level;
        cmd_cp.slice = destination_native_slice;
        cmd_cp.options = (dst_planar_count > 1)
                             ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        cmd_cp.origin = {DstX, DstY, DstZ};
        if (tracked_upload)
          RecordBlitFootprintAccess(static_cast<MTLD3D12Resource *>(pSrc->pResource),
                                    static_cast<MTLD3D12Resource *>(pDst->pResource), dst_level, dst_slice, false);
      } else {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        // Depth/color reinterpretation cannot use a Metal texture view. Preserve
        // storage bits through the existing plane-copy buffer path instead.
        const bool different_depth_storage = dst->pixelFormat() != src->pixelFormat() &&
            (DepthStencilPlanarFlags(dst->pixelFormat()) || DepthStencilPlanarFlags(src->pixelFormat()));
        if (dst_planar_count > 1 || src_planar_count > 1 || different_depth_storage) {

          if (dst_planar_count > 1 && src_planar_count > 1 && dst_planar != src_planar) {
            WARN("CopyTextureRegion: unmatched planar"); // just in case
            return;
          }

          auto texel_size = (dst_planar == 1 || src_planar == 1) ? 1u :
              (dst_planar_count > 1 || src_planar_count > 1) ? 4u : src_format.BytesPerTexel;
          auto width = src_box.right - src_box.left;
          auto height = src_box.bottom - src_box.top;
          auto bytes_per_row = align(width * texel_size, 256);
          auto bytes_per_image = bytes_per_row * height;

          auto [temp_buffer, temp_buffer_offset] = allocator_->AllocateTempBuffer(bytes_per_image, 256);
          if (!temp_buffer) {
            RecordError(E_OUTOFMEMORY);
            return;
          }

          auto &cmd_to_tmp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          cmd_to_tmp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          cmd_to_tmp.src = source_native_texture;
          cmd_to_tmp.level = source_native_level;
          cmd_to_tmp.slice = source_native_slice;
          cmd_to_tmp.origin = {src_box.left, src_box.top, src_box.front};
          cmd_to_tmp.size = {width, height, src_box.back - src_box.front};
          cmd_to_tmp.dst = temp_buffer;
          cmd_to_tmp.offset = temp_buffer_offset;
          cmd_to_tmp.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tmp.bytes_per_row = bytes_per_row;
          cmd_to_tmp.options = (src_planar_count > 1) ? (src_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;

          auto &cmd_to_tex = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          cmd_to_tex.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          cmd_to_tex.src = temp_buffer;
          cmd_to_tex.src_offset = temp_buffer_offset;
          cmd_to_tex.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tex.bytes_per_row = bytes_per_row;
          cmd_to_tex.dst = destination_native_texture;
          cmd_to_tex.level = destination_native_level;
          cmd_to_tex.slice = destination_native_slice;
          cmd_to_tex.origin = {DstX, DstY, DstZ};
          cmd_to_tex.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
          cmd_to_tex.options = (dst_planar_count > 1) ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;
          RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pDst->pResource), true);
          return;
        }

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>(true);
        cmd_cp.type = WMTBlitCommandCopyFromTextureToTexture;
        cmd_cp.src = source_native_texture;
        cmd_cp.src_level = source_native_level;
        cmd_cp.src_slice = source_native_slice;
        cmd_cp.src_origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.src_size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = destination_native_texture;
        cmd_cp.dst_level = destination_native_level;
        cmd_cp.dst_slice = destination_native_slice;
        cmd_cp.dst_origin = {DstX, DstY, DstZ};
        RecordBlitTextureAccess(static_cast<MTLD3D12Resource *>(pSrc->pResource), src_level, src_slice, false);
        RecordBlitTextureAccess(static_cast<MTLD3D12Resource *>(pDst->pResource), dst_level, dst_slice, true);
      }
    } else if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->buffer;
      if (!dst)
        return;

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pDst->PlacedFootprint.Footprint.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", pDst->PlacedFootprint.Footprint.Format);
        return;
      }

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        const auto block_width = dst_footprint_info.block_width;
        const auto dst_depth_pitch = src->textureType() == WMTTextureType3D ? dst_footprint_info.slice_pitch : 0;

        const auto storage = resource_contract::GetEmulatedTextureStorage(src_desc.Format);
        if (storage != resource_contract::EmulatedTextureStorage::None) {
          if (!resource_contract::AreCopyCompatibleFormats(src_desc.Format, pDst->PlacedFootprint.Footprint.Format)) {
            RecordError(E_INVALIDARG);
            return;
          }
          const UINT width = src_box.right - src_box.left;
          const UINT height = src_box.bottom - src_box.top;
          const UINT depth = src_box.back - src_box.front;
          const UINT native_bytes = storage == resource_contract::EmulatedTextureStorage::RGB32 ? 16 : 4;
          uint64_t pitch = 0, slice = 0, size = 0;
          if (!resource_contract::CheckedMul(width, native_bytes, pitch) ||
              !resource_contract::CheckedAlign(pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch) ||
              !resource_contract::CheckedMul(pitch, height, slice) ||
              !resource_contract::CheckedMul(slice, depth, size) || pitch > UINT_MAX || size > SIZE_MAX) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(size, 256);
          if (!scratch) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          copy.src = source_native_texture;
          copy.level = source_native_level;
          copy.slice = source_native_slice;
          copy.origin = {src_box.left, src_box.top, src_box.front};
          copy.size = {width, height, depth};
          copy.dst = scratch;
          copy.offset = scratch_offset;
          copy.bytes_per_row = pitch;
          copy.bytes_per_image = slice;
          copy.options = WMTBlitOptionNone;
          const uint64_t dst_offset = pDst->PlacedFootprint.Offset +
              (uint64_t(DstX) / block_width) * dst_format.BytesPerTexel +
              (uint64_t(DstY) / dst_footprint_info.block_height) * pDst->PlacedFootprint.Footprint.RowPitch +
              uint64_t(DstZ) * dst_footprint_info.slice_pitch;
          for (UINT z = 0; z < depth; ++z) {
            if (!ConvertEmulatedTextureRows(storage, false, scratch, scratch_offset + uint64_t(z) * slice,
                    UINT(pitch), dst->current()->buffer(), dst_offset + uint64_t(z) * dst_footprint_info.slice_pitch,
                    pDst->PlacedFootprint.Footprint.RowPitch, width, height))
              return;
          }
          return;
        }

        if (!src_planar && (src_format.Flag & MTL_DXGI_FORMAT_EMULATED_D24)) {
          const UINT width = src_box.right - src_box.left, height = src_box.bottom - src_box.top;
          UINT64 pitch = 0, size = 0;
          if (!resource_contract::CheckedAlign(UINT64(width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch) ||
              !resource_contract::CheckedMul(pitch, height, size) || size > SIZE_MAX) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(size, 256);
          if (!scratch) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          copy.src = source_native_texture;
          copy.level = source_native_level;
          copy.slice = source_native_slice;
          copy.origin = {src_box.left, src_box.top, src_box.front};
          copy.size = {width, height, 1};
          copy.dst = scratch;
          copy.offset = scratch_offset;
          copy.bytes_per_row = pitch;
          copy.bytes_per_image = 0;
          copy.options = WMTBlitOptionDepthFromDepthStencil;
          const UINT64 offset = pDst->PlacedFootprint.Offset + UINT64(DstX) * dst_format.BytesPerTexel +
                                UINT64(DstY) * pDst->PlacedFootprint.Footprint.RowPitch +
                                UINT64(DstZ) * dst_footprint_info.slice_pitch;
          ConvertD24Plane(scratch, scratch_offset, UINT(pitch), dst->current()->buffer(), offset,
                          pDst->PlacedFootprint.Footprint.RowPitch, width, height, false);
          return;
        }

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>(
            src_planar_count == 1);
        cmd_cp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        cmd_cp.src = source_native_texture;
        cmd_cp.level = source_native_level;
        cmd_cp.slice = source_native_slice;
        cmd_cp.origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.size = source_size;
        cmd_cp.dst = dst->current()->buffer();
        cmd_cp.offset = pDst->PlacedFootprint.Offset + (DstX / block_width) * dst_format.BytesPerTexel +
                        (DstY / dst_footprint_info.block_height) * pDst->PlacedFootprint.Footprint.RowPitch +
                        UINT64(DstZ) * dst_footprint_info.slice_pitch;
        cmd_cp.bytes_per_row = pDst->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = dst_depth_pitch;
        cmd_cp.options = (src_planar_count > 1)
                             ? (src_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        if (src_planar_count == 1)
          RecordBlitFootprintAccess(static_cast<MTLD3D12Resource *>(pDst->pResource),
                                    static_cast<MTLD3D12Resource *>(pSrc->pResource), src_level, src_slice, true);
      } else {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer;
        if (!src)
          return;

        const UINT block_width = src_footprint_info.block_width;
        const UINT64 row_bytes =
            ((static_cast<UINT64>(src_box.right - src_box.left) + block_width - 1) / block_width) *
            src_footprint_info.bytes_per_block;
        const UINT64 copy_rows =
            (static_cast<UINT64>(src_box.bottom - src_box.top) + src_block_height - 1) / src_block_height;
        const UINT64 copy_depth = src_box.back - src_box.front;
        const UINT64 src_row_start = src_box.top / src_block_height;
        const UINT64 dst_row_start = DstY / dst_block_height;
        const UINT64 src_x_bytes =
            (static_cast<UINT64>(src_box.left) / block_width) * src_footprint_info.bytes_per_block;
        const UINT64 dst_x_bytes =
            (static_cast<UINT64>(DstX) / block_width) * dst_footprint_info.bytes_per_block;

        for (UINT64 z = 0; z < copy_depth; ++z) {
          for (UINT64 row = 0; row < copy_rows; ++row) {
            auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
            cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
            cmd_cp.src = src->current()->buffer();
            cmd_cp.src_offset = pSrc->PlacedFootprint.Offset +
                                (static_cast<UINT64>(src_box.front) + z) * src_footprint_info.slice_pitch +
                                (src_row_start + row) * pSrc->PlacedFootprint.Footprint.RowPitch + src_x_bytes;
            cmd_cp.dst = dst->current()->buffer();
            cmd_cp.dst_offset = pDst->PlacedFootprint.Offset +
                                (static_cast<UINT64>(DstZ) + z) * dst_footprint_info.slice_pitch +
                                (dst_row_start + row) * pDst->PlacedFootprint.Footprint.RowPitch + dst_x_bytes;
            cmd_cp.copy_length = row_bytes;
          }
        }
      }
    }
    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pDst->pResource), true);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);
    if (!pDst || !pSrc || (pDst == pSrc))
      return;
    if (!QueryObjectOwnedByThisDevice(pDstResource) || !QueryObjectOwnedByThisDevice(pSrcResource)) {
      RecordError();
      return;
    }

    owned_graphics::Trace("copy_resource", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"src\":", pSrc->owned_id, ",\"dst\":", pDst->owned_id);

    auto DstDesc = pDst->GetDesc();
    auto SrcDesc = pSrc->GetDesc();
    if (DstDesc.Dimension != SrcDesc.Dimension)
      return;

    if (pDst->feedback || pSrc->feedback) {
      // Feedback maps copy whole, cell for cell, between maps of the same kind and shape.
      if (!pDst->feedback || !pSrc->feedback || pDst->feedback->word() != pSrc->feedback->word()) {
        RecordError();
        return;
      }
      if (!RecordImplicitUse(pSrcResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
          !RecordImplicitUse(pDstResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST))
        return;
      const uint64_t bytes = pSrc->feedback->totalCells() * 4;
      auto source = pSrc->buffer->current()->buffer(), destination = pDst->buffer->current()->buffer();
      if (predication_enabled_) {
        if (!EnsurePredicationPipelines()) {
          RecordError(E_FAIL);
          return;
        }
        EncodePredicatedBufferCopy(source, 0, destination, 0, bytes);
        return;
      }
      PreBlit();
      auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd_cp.copy_length = bytes;
      cmd_cp.src = source;
      cmd_cp.src_offset = 0;
      cmd_cp.dst = destination;
      cmd_cp.dst_offset = 0;
      return;
    }
    if (DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (DstDesc.Width != SrcDesc.Width || !pDst->buffer || !pSrc->buffer)
        return;
      if (!RecordImplicitUse(pSrcResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
          !RecordImplicitUse(pDstResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST))
        return;
      if (!PreBlit())
        return;
      auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd_cp.copy_length = SrcDesc.Width;
      cmd_cp.src = pSrc->buffer->current()->buffer();
      cmd_cp.src_offset = 0;
      cmd_cp.dst = pDst->buffer->current()->buffer();
      cmd_cp.dst_offset = 0;
      try {
        auto &uses = static_cast<BlitEncoderData *>(allocator_->encoder_current)->submission_buffers;
        uses.push_back({pSrc->buffer->current()->gpuAddress(), SrcDesc.Width, false});
        uses.push_back({pDst->buffer->current()->gpuAddress(), SrcDesc.Width, true});
      } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); }
      return;
    }

    if (!pDst->texture || !pSrc->texture ||
        DstDesc.DepthOrArraySize != SrcDesc.DepthOrArraySize || DstDesc.MipLevels != SrcDesc.MipLevels ||
        DstDesc.SampleDesc.Count != SrcDesc.SampleDesc.Count || DstDesc.SampleDesc.Quality != SrcDesc.SampleDesc.Quality)
      return;

    // Reinterpret-copy is bitwise, not a codec operation. D3D12 defines only
    // these cross-family pairs: one BC block maps to one equally sized integer
    // texel (or one R9G9B9E5 texel to one R32 integer texel).
    auto reinterpret_bytes = [](DXGI_FORMAT packed, DXGI_FORMAT plain) -> UINT {
      if (plain == DXGI_FORMAT_R32_UINT || plain == DXGI_FORMAT_R32_SINT)
        return packed == DXGI_FORMAT_R9G9B9E5_SHAREDEXP ? 4u : 0u;
      if (plain == DXGI_FORMAT_R16G16B16A16_UINT || plain == DXGI_FORMAT_R16G16B16A16_SINT ||
          plain == DXGI_FORMAT_R32G32_UINT || plain == DXGI_FORMAT_R32G32_SINT) {
        switch (packed) {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM: return 8;
        default: return 0;
        }
      }
      if (plain == DXGI_FORMAT_R32G32B32A32_UINT || plain == DXGI_FORMAT_R32G32B32A32_SINT) {
        switch (packed) {
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM: return 16;
        default: return 0;
        }
      }
      return 0;
    };
    const UINT src_packed_bytes = reinterpret_bytes(SrcDesc.Format, DstDesc.Format);
    const UINT dst_packed_bytes = reinterpret_bytes(DstDesc.Format, SrcDesc.Format);
    const UINT reinterpret_size = src_packed_bytes ? src_packed_bytes : dst_packed_bytes;
    if (reinterpret_size) {
      const bool src_bc = src_packed_bytes && SrcDesc.Format != DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
      const bool dst_bc = dst_packed_bytes && DstDesc.Format != DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
      const auto &bc_desc = src_bc ? SrcDesc : DstDesc;
      const auto &raw_desc = src_bc ? DstDesc : SrcDesc;
      if (SrcDesc.SampleDesc.Count != 1 ||
          (src_bc || dst_bc ? (raw_desc.Width > UINT64_MAX / 4 ||
                              bc_desc.Width != raw_desc.Width * 4 ||
                              UINT64(bc_desc.Height) != UINT64(raw_desc.Height) * 4)
                            : (SrcDesc.Width != DstDesc.Width || SrcDesc.Height != DstDesc.Height))) {
        RecordError();
        return;
      }
      if (!RecordImplicitUse(pSrcResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
          !RecordImplicitUse(pDstResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST))
        return;
      RecordStandardSwizzleUse(pSrc, false);
      RecordStandardSwizzleUse(pDst, false);

      const UINT arrays = SrcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : SrcDesc.DepthOrArraySize;
      for (UINT array = 0; array < arrays; ++array) {
        for (UINT mip = 0; mip < SrcDesc.MipLevels; ++mip) {
          const UINT64 source_width = std::max<UINT64>(1, SrcDesc.Width >> mip);
          const UINT64 source_height = std::max<UINT64>(1, SrcDesc.Height >> mip);
          const UINT64 target_width = std::max<UINT64>(1, DstDesc.Width >> mip);
          const UINT64 target_height = std::max<UINT64>(1, DstDesc.Height >> mip);
          const UINT64 source_rows = (source_height + (src_bc ? 3 : 0)) / (src_bc ? 4 : 1);
          const UINT64 target_rows = (target_height + (dst_bc ? 3 : 0)) / (dst_bc ? 4 : 1);
          const UINT64 source_columns = (source_width + (src_bc ? 3 : 0)) / (src_bc ? 4 : 1);
          const UINT64 target_columns = (target_width + (dst_bc ? 3 : 0)) / (dst_bc ? 4 : 1);
          const UINT64 depth = SrcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                   ? std::max<UINT>(1, SrcDesc.DepthOrArraySize >> mip) : 1;
          UINT64 row_bytes = 0, pitch = 0, slice_pitch = 0, size = 0;
          if (source_rows != target_rows || source_columns != target_columns ||
              !resource_contract::CheckedMul(source_columns, reinterpret_size, row_bytes) ||
              !resource_contract::CheckedAlign(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitch) ||
              !resource_contract::CheckedMul(pitch, source_rows, slice_pitch) ||
              !resource_contract::CheckedMul(slice_pitch, depth, size) || size > SIZE_MAX) {
            RecordError();
            return;
          }
          auto [source_staging, source_offset] = allocator_->AllocateTempBuffer(size, 256);
          if (!source_staging || !PreBlit()) {
            RecordError(E_OUTOFMEMORY);
            return;
          }
          auto transfer_volume = [&](MTLD3D12Resource *resource, WMT::Buffer staging, UINT64 offset, bool upload) {
            if (!resource->reserved || !resource->reserved->volumeLayout()) return false;
            const bool compressed = resource->reserved->blockWidth() == 4;
            const UINT block = compressed ? 4 : 1;
            const UINT64 columns = source_columns, rows = source_rows;
            auto native = resource->texture->current()->texture();
            for (UINT64 z = 0; z < depth; ++z)
              for (UINT64 y = 0; y < rows; ++y)
                for (UINT64 x = 0; x < columns;) {
                  const auto address = resource->reserved->VolumeAddress(mip, UINT(x * block), UINT(y * block), UINT(z));
                  if (!address) { RecordError(); return true; }
                  const UINT64 count = std::min<UINT64>(address->units, columns - x);
                  const UINT64 row_offset = offset + z * slice_pitch + y * pitch + x * reinterpret_size;
                  if (upload) {
                    auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
                    copy.type = WMTBlitCommandCopyFromBufferToTexture;
                    copy.src = staging; copy.src_offset = row_offset; copy.dst = native;
                    copy.bytes_per_row = pitch; copy.bytes_per_image = 0;
                    copy.slice = address->slice; copy.level = 0; copy.origin = {address->x, address->y, 0};
                    copy.size = {count * block, block, 1};
                  } else {
                    auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
                    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
                    copy.src = native; copy.dst = staging; copy.offset = row_offset;
                    copy.bytes_per_row = pitch; copy.bytes_per_image = 0;
                    copy.slice = address->slice; copy.level = 0; copy.origin = {address->x, address->y, 0};
                    copy.size = {count * block, block, 1};
                  }
                  x += count;
                }
            return true;
          };
          if (!transfer_volume(pSrc, source_staging, source_offset, false)) {
          auto &read_source = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          read_source.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          read_source.src = pSrc->texture->current()->texture();
          read_source.level = mip;
          read_source.slice = array;
          read_source.origin = {0, 0, 0};
          read_source.size = {source_width, source_height, depth};
          read_source.dst = source_staging;
          read_source.offset = source_offset;
          read_source.bytes_per_row = pitch;
          read_source.bytes_per_image = SrcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? slice_pitch : 0;
          read_source.options = WMTBlitOptionNone;
          }
          if (FAILED(recording_error_)) return;

          WMT::Buffer destination_staging = source_staging;
          UINT64 destination_offset = source_offset;
          if (predication_enabled_) {
            allocator_->InvalidateCurrentPass();
            auto [scratch, scratch_offset] = allocator_->AllocateTempBuffer(size, 256);
            if (!scratch || !PreBlit()) {
              RecordError(E_OUTOFMEMORY);
              return;
            }
            if (!transfer_volume(pDst, scratch, scratch_offset, false)) {
            auto &read_destination = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
            read_destination.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
            read_destination.src = pDst->texture->current()->texture();
            read_destination.level = mip;
            read_destination.slice = array;
            read_destination.origin = {0, 0, 0};
            read_destination.size = {target_width, target_height, depth};
            read_destination.dst = scratch;
            read_destination.offset = scratch_offset;
            read_destination.bytes_per_row = pitch;
            read_destination.bytes_per_image = DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? slice_pitch : 0;
            read_destination.options = WMTBlitOptionNone;
            }
            if (FAILED(recording_error_)) return;
            allocator_->InvalidateCurrentPass();
            for (UINT64 copied = 0; copied < size;) {
              const UINT chunk = UINT(std::min<UINT64>(size - copied, UINT32_MAX));
              EncodePredicatedBufferCopy(source_staging, source_offset + copied, scratch, scratch_offset + copied, chunk);
              copied += chunk;
            }
            destination_staging = scratch;
            destination_offset = scratch_offset;
          }

          if (!PreBlit()) {
            RecordError();
            return;
          }
          if (!transfer_volume(pDst, destination_staging, destination_offset, true)) {
          auto &write_destination = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          write_destination.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          write_destination.src = destination_staging;
          write_destination.src_offset = destination_offset;
          write_destination.bytes_per_row = pitch;
          write_destination.bytes_per_image = DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? slice_pitch : 0;
          write_destination.dst = pDst->texture->current()->texture();
          write_destination.level = mip;
          write_destination.slice = array;
          write_destination.origin = {0, 0, 0};
          write_destination.size = {target_width, target_height, depth};
          write_destination.options = WMTBlitOptionNone;
          }
          if (FAILED(recording_error_)) return;
          allocator_->InvalidateCurrentPass();
        }
      }
      RecordStandardSwizzleUse(pDst, true);
      return;
    }

    if (DstDesc.Width != SrcDesc.Width || DstDesc.Height != SrcDesc.Height)
      return;

    if (!resource_contract::AreCopyCompatibleFormats(DstDesc.Format, SrcDesc.Format)) {
      RecordError();
      return;
    }
    // Metal's whole-texture blit requires identical native pixel formats even
    // when D3D12 permits a bitwise copy between formats in one DXGI type group.
    // The region path creates matching-format views for each subresource (and
    // handles depth/stencil planes); keep the fast blit for identical backing.
    if (predication_enabled_ || pSrc->texture->pixelFormat() != pDst->texture->pixelFormat() ||
        (pSrc->reserved && pSrc->reserved->volumeLayout()) ||
        (pDst->reserved && pDst->reserved->volumeLayout())) {
      MTL_DXGI_FORMAT_DESC copy_format{};
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), SrcDesc.Format, copy_format))) {
        RecordError();
        return;
      }
      const UINT arrays = SrcDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : SrcDesc.DepthOrArraySize;
      const UINT subresources = UINT(SrcDesc.MipLevels) * arrays * copy_format.PlanarCount;
      for (UINT subresource = 0; subresource < subresources; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION source{pSrcResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION destination{pDstResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source.SubresourceIndex = subresource;
        destination.SubresourceIndex = subresource;
        CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        if (FAILED(recording_error_))
          return;
      }
      return;
    }

    RecordStandardSwizzleUse(pSrc, false);
    RecordStandardSwizzleUse(pDst, false);
    if (!RecordImplicitUse(pSrcResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
        !RecordImplicitUse(pDstResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST))
      return;
    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_texture>(true);
    cmd_cp.type = WMTBlitCommandCopyTexture;
    cmd_cp.src = pSrc->texture->current()->texture();
    cmd_cp.dst = pDst->texture->current()->texture();
    RecordBlitTextureAccess(pSrc, UINT32_MAX, UINT32_MAX, false);
    RecordBlitTextureAccess(pDst, UINT32_MAX, UINT32_MAX, true);
    RecordStandardSwizzleUse(pDst, true);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE CopyTiles(
      ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset,
      D3D12_TILE_COPY_FLAGS flags
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    constexpr D3D12_TILE_COPY_FLAGS direction_flags = D3D12_TILE_COPY_FLAGS(
        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE |
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    const auto direction = flags & direction_flags;
    if (!tiled_resource || !buffer || !tile_region_start_coordinate || !tile_region_size ||
        (flags & ~(direction_flags | D3D12_TILE_COPY_FLAG_NO_HAZARD)) || direction == 0 ||
        direction == direction_flags || !QueryObjectOwnedByThisDevice(tiled_resource) ||
        !QueryObjectOwnedByThisDevice(buffer)) {
      RecordError();
      return;
    }
    auto *tiled = static_cast<MTLD3D12Resource *>(tiled_resource);
    auto *linear = static_cast<MTLD3D12Resource *>(buffer);
    const auto buffer_desc = buffer->GetDesc();
    if (!tiled->reserved || (!tiled->reserved->isBuffer() && !tiled->owned_is_reserved) ||
        buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !linear->buffer) {
      RecordError();
      return;
    }
    const auto &reserved = *tiled->reserved;
    std::vector<uint32_t> tiles;
    try {
      if (!reserved.ExpandRegion(*tile_region_start_coordinate, *tile_region_size, tiles)) {
        RecordError();
        return;
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    const uint64_t bytes = uint64_t(tiles.size()) * kReservedTileBytes;
    if (!RangeFits(buffer_offset, bytes, buffer_desc.Width) || tiles.empty())
      return;
    for (auto tile : tiles) {
      /* Packed mips have no defined linear layout; D3D12 leaves such copies undefined, reject them. */
      if (reserved.isPackedTile(tile)) {
        RecordError();
        return;
      }
    }
    const bool to_buffer = direction == D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
    const auto subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!RecordImplicitUse(tiled_resource, subresource,
                           to_buffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST) ||
        !RecordImplicitUse(buffer, subresource,
                           to_buffer ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE))
      return;
    if (!PreBlit())
      return;
    auto linear_buffer = linear->buffer->current()->buffer();
    if (reserved.isBuffer()) {
      /* Buffer tiles are linear already: one copy per run of consecutive tiles. */
      auto tiled_buffer = tiled->buffer->current()->buffer();
      for (size_t i = 0; i < tiles.size();) {
        size_t run = 1;
        while (i + run < tiles.size() && tiles[i + run] == tiles[i] + run)
          run++;
        auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
        copy.type = WMTBlitCommandCopyFromBufferToBuffer;
        copy.src = to_buffer ? tiled_buffer : linear_buffer;
        copy.dst = to_buffer ? linear_buffer : tiled_buffer;
        const uint64_t tiled_offset = uint64_t(tiles[i]) * kReservedTileBytes;
        const uint64_t linear_offset = buffer_offset + i * kReservedTileBytes;
        copy.src_offset = to_buffer ? tiled_offset : linear_offset;
        copy.dst_offset = to_buffer ? linear_offset : tiled_offset;
        copy.copy_length = run * kReservedTileBytes;
        i += run;
      }
      return;
    }
    /*
     * Texture tiles: each 64KB span of the buffer holds one tile row-major, with the pitch of a full
     * tile. Tiles clipped by the mip's edge copy only their valid texels, keeping that pitch.
     */
    auto tiled_texture = tiled->texture->current()->texture();
    if (const auto *volume = reserved.volumeLayout()) {
      const uint32_t pitch = volume->page_w / volume->block_w * reserved.bytesPerUnit();
      for (size_t i = 0; i < tiles.size(); i++) {
        const uint32_t tile = tiles[i];
        const uint32_t slice = tile / (volume->atlas_columns * volume->atlas_rows);
        const WMTOrigin origin{(tile % volume->atlas_columns) * volume->page_w,
            ((tile / volume->atlas_columns) % volume->atlas_rows) * volume->page_h, 0};
        const WMTSize size{volume->page_w, volume->page_h, 1};
        const uint64_t offset = buffer_offset + i * kReservedTileBytes;
        if (to_buffer) {
          auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
          copy.type = WMTBlitCommandCopyFromTextureToBuffer;
          copy.src = tiled_texture; copy.slice = slice; copy.level = 0;
          copy.origin = origin; copy.size = size;
          copy.dst = linear_buffer; copy.offset = offset;
          copy.bytes_per_row = pitch; copy.bytes_per_image = 0;
        } else {
          auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          copy.type = WMTBlitCommandCopyFromBufferToTexture;
          copy.src = linear_buffer; copy.src_offset = offset;
          copy.bytes_per_row = pitch; copy.bytes_per_image = 0; copy.size = size;
          copy.dst = tiled_texture; copy.slice = slice; copy.level = 0; copy.origin = origin;
        }
      }
      return;
    }
    const auto shape = reserved.tileShape();
    const uint32_t block_w = reserved.blockWidth(), block_h = reserved.blockHeight();
    const uint32_t row_pitch = shape.WidthInTexels / block_w * reserved.bytesPerUnit();
    const auto desc = tiled_resource->GetDesc();
    for (size_t i = 0; i < tiles.size(); i++) {
      uint32_t x = 0, y = 0, z = 0;
      const auto *sub = reserved.subresourceOfTile(tiles[i], x, y, z);
      if (!sub)
        continue;
      const uint64_t mip_w = std::max<uint64_t>(desc.Width >> sub->mip, 1);
      const uint64_t mip_h = std::max<uint64_t>(desc.Height >> sub->mip, 1);
      const uint64_t origin_x = uint64_t(x) * shape.WidthInTexels, origin_y = uint64_t(y) * shape.HeightInTexels;
      /* Metal copies BC data in whole blocks of the (block-aligned) level size. */
      const uint64_t level_w = (mip_w + block_w - 1) / block_w * block_w;
      const uint64_t level_h = (mip_h + block_h - 1) / block_h * block_h;
      const uint64_t width = std::min<uint64_t>(shape.WidthInTexels, level_w - origin_x);
      const uint64_t height = std::min<uint64_t>(shape.HeightInTexels, level_h - origin_y);
      const uint64_t linear_offset = buffer_offset + i * kReservedTileBytes;
      if (to_buffer) {
        auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        copy.type = WMTBlitCommandCopyFromTextureToBuffer;
        copy.src = tiled_texture;
        copy.slice = sub->slice;
        copy.level = sub->mip;
        copy.origin = {origin_x, origin_y, 0};
        copy.size = {width, height, 1};
        copy.dst = linear_buffer;
        copy.offset = linear_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = kReservedTileBytes;
      } else {
        auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        copy.type = WMTBlitCommandCopyFromBufferToTexture;
        copy.src = linear_buffer;
        copy.src_offset = linear_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = kReservedTileBytes;
        copy.size = {width, height, 1};
        copy.dst = tiled_texture;
        copy.slice = sub->slice;
        copy.level = sub->mip;
        copy.origin = {origin_x, origin_y, 0};
      }
    }
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE ResolveSubresource(
      ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource,
      DXGI_FORMAT Format
  ) try {
    if (FAILED(recording_error_)) return;
    ResolveSubresourceRegion(
        pDstResource, DstSubresource, 0, 0, pSrcResource, SrcSubresource, nullptr, Format,
        D3D12_RESOLVE_MODE_AVERAGE
    );
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::IASetPrimitiveTopology, Topology))
      return;
    topology_ = Topology;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (NumViewports > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    // A new render encoder dirties every fixed state, so an identical array needs no re-emission.
    if (num_viewports == NumViewports &&
        (!NumViewports || !std::memcmp(viewports, pViewports, sizeof(*pViewports) * NumViewports)))
      return;
    num_viewports = NumViewports;
    for (auto i = 0u; i < NumViewports; i++) {
      viewports[i] = pViewports[i];
    }
    dirty_state_.set(DirtyState::Viewport);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  RSSetScissorRects(UINT NumRects, const D3D12_RECT *rects) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (NumRects > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    if (num_scissors == NumRects && (!NumRects || !std::memcmp(scissors, rects, sizeof(*rects) * NumRects)))
      return;
    num_scissors = NumRects;
    for (auto i = 0u; i < NumRects; i++) {
      scissors[i] = rects[i];
    }
    dirty_state_.set(DirtyState::ScissorRect);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  OMSetBlendFactor(const FLOAT BlendFactors[4]) try {
    if (FAILED(recording_error_)) return;
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      std::array<FLOAT, 4> values = {1.0f, 1.0f, 1.0f, 1.0f};
      if (BlendFactors)
        std::copy_n(BlendFactors, values.size(), values.begin());
      AppendBundleCommand([values](auto &parent) { parent.OMSetBlendFactor(values.data()); });
      return;
    }
    const FLOAT defaults[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const FLOAT *values = BlendFactors ? BlendFactors : defaults;
    if (!std::memcmp(blend_factor_, values, sizeof(blend_factor_)))
      return;
    memcpy(blend_factor_, values, sizeof(blend_factor_));
    dirty_state_.set(DirtyState::BlendFactor);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  OMSetStencilRef(UINT StencilRef) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::OMSetStencilRef, StencilRef))
      return;
    if (stencil_ref_ == (UINT8)StencilRef && back_stencil_ref_ == (UINT8)StencilRef)
      return;
    stencil_ref_ = (UINT8)StencilRef;
    back_stencil_ref_ = (UINT8)StencilRef;
    dirty_state_.set(DirtyState::StencilRef);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  // ID3D12GraphicsCommandList8. The references are 8-bit like OMSetStencilRef.
  void STDMETHODCALLTYPE
  OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::OMSetFrontAndBackStencilRef, FrontStencilRef, BackStencilRef))
      return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError(E_FAIL);
      return;
    }
    if (stencil_ref_ == (UINT8)FrontStencilRef && back_stencil_ref_ == (UINT8)BackStencilRef)
      return;
    stencil_ref_ = (UINT8)FrontStencilRef;
    back_stencil_ref_ = (UINT8)BackStencilRef;
    dirty_state_.set(DirtyState::StencilRef);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  // Depth bias and strip cut follow the bound graphics PSO until an ID3D12GraphicsCommandList9 call
  // overrides them; that call requires the matching D3D12_PIPELINE_STATE_FLAG_DYNAMIC_* on the PSO.
  void AdoptPipelineDynamicState() {
    const auto *pso = pso_graphics_.ptr();
    depth_bias_ = pso ? pso->depth_bias : 0.0f;
    depth_bias_clamp_ = pso ? pso->depth_bias_clamp : 0.0f;
    slope_scaled_depth_bias_ = pso ? pso->scole_scale : 0.0f;
    ib_strip_cut_value_ = pso ? pso->ib_strip_cut_value : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  }

  void STDMETHODCALLTYPE
  RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::RSSetDepthBias, DepthBias, DepthBiasClamp,
                     SlopeScaledDepthBias))
      return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError(E_FAIL);
      return;
    }
    if (!pso_graphics_ || !pso_graphics_->dynamic_depth_bias || !std::isfinite(DepthBias) ||
        !std::isfinite(DepthBiasClamp) || !std::isfinite(SlopeScaledDepthBias)) {
      RecordError();
      return;
    }
    depth_bias_ = DepthBias;
    depth_bias_clamp_ = DepthBiasClamp;
    slope_scaled_depth_bias_ = SlopeScaledDepthBias;
    dirty_state_.set(DirtyState::GraphicsPipelineState);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::IASetIndexBufferStripCutValue, IBStripCutValue))
      return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError(E_FAIL);
      return;
    }
    if (!pso_graphics_ || !pso_graphics_->dynamic_index_buffer_strip_cut ||
        (IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED &&
         IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF &&
         IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF)) {
      RecordError();
      return;
    }
    ib_strip_cut_value_ = IBStripCutValue;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  /*
   * Variable-rate shading: coarse pixel shading on the captured MSAA pixel path (msaa_coarse.hpp).
   * Resolve per-draw, provoking-primitive, and image rates on the GPU at coverage time.
   */
  void STDMETHODCALLTYPE
  RSSetShadingRate(D3D12_SHADING_RATE BaseShadingRate, const D3D12_SHADING_RATE_COMBINER *pCombiners) try {
    if (FAILED(recording_error_)) return;
    std::array<D3D12_SHADING_RATE_COMBINER, 2> combiners{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                         D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    if (pCombiners) combiners = {pCombiners[0], pCombiners[1]};
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      AppendBundleCommand([BaseShadingRate, combiners](auto &parent) {
        parent.RSSetShadingRate(BaseShadingRate, combiners.data());
      });
      return;
    }
    shading_rate_ = BaseShadingRate;
    shading_rate_combiners_ = combiners;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  RSSetShadingRateImage(ID3D12Resource *image) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation()) return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        (image && (!QueryObjectOwnedByThisDevice(image) || !IsShadingRateImageDesc(image->GetDesc())))) {
      RecordError();
      return;
    }
    D3D12_HEAP_PROPERTIES heap{};
    if (image && SUCCEEDED(image->GetHeapProperties(&heap, nullptr)) &&
        (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK)) {
      RecordError();
      return;
    }
    shading_rate_image_ = static_cast<MTLD3D12Resource *>(image);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  bool CaptureShadingRateImage(MSAADrawData &draw) {
    if (!shading_rate_image_) return true;
    draw.shading_rate_image = shading_rate_image_->texture->current()->texture();
    const auto desc = shading_rate_image_->GetDesc();
    draw.shading_rate_image_width = uint32_t(desc.Width);
    draw.shading_rate_image_height = desc.Height;
    return true;
  }

  /* Draw rate state in tile-control encoding (msaa_coarse.hpp kRate*). A 1x1 base rate still needs
   * captured processing when an image/primitive combiner or SV_ShadingRate input is active. */
  uint32_t CoarseShadingRateState() const {
    if (!pso_graphics_ || !pso_graphics_->owned_has_pixel_shader || pso_graphics_->forced_sample_count)
      return 0;
    if (!pso_graphics_->owned_shading_rate_pixel_shader && shading_rate_ == D3D12_SHADING_RATE_1X1 &&
        shading_rate_combiners_[0] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH &&
        shading_rate_combiners_[1] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH) return 0;
    return (uint32_t(shading_rate_) & 0xf) | (uint32_t(shading_rate_combiners_[0]) & 0xf) << 4 |
           (uint32_t(shading_rate_combiners_[1]) & 0xf) << 8 | 1u << 12 |
           (shading_rate_image_ ? 1u << 13 : 0u);
  }

  bool RequiresMSAAExecution() {
    return pso_graphics_ && (pso_graphics_->RequiresMSAAExecution() || CoarseShadingRateState() ||
                            topology_ == kTriangleFan);
  }

  /*
   * ID3D12GraphicsCommandList6::DispatchMesh (MeshShaderTier 1): one Metal object/mesh draw. Each grid
   * dimension must be below 64k and the product at most 2^22 (D3D undefined behaviour otherwise; dropped
   * here, matching an empty draw). Under predication the grid comes from the predicated argument copy, so
   * a false predicate launches no group.
   */
  void STDMETHODCALLTYPE
  DispatchMesh(UINT X, UINT Y, UINT Z) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::DispatchMesh, X, Y, Z))
      return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError(E_FAIL);
      return;
    }
    if (!pso_graphics_ || pso_graphics_->mesh_kind != MeshPipelineKind::MeshShader) {
      RecordError(E_INVALIDARG);
      return;
    }
    if (!X || !Y || !Z || X > 65535 || Y > 65535 || Z > 65535 || uint64_t(X) * Y * Z > (1u << 22))
      return;
    if (ReplayPerView([&] { DispatchMesh(X, Y, Z); }))
      return;
    const bool statistics = !active_pipeline_statistics_.empty();
    // Native mesh raster partitioning adds helper invocations to Metal's counter.
    // Active D3D queries need the captured per-(primitive,pixel) invocation accounting.
    const bool capture_pixels = statistics || CoarseShadingRateState() != 0 ||
        (pso_graphics_->owned_mesh_output_replay || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) || pso_graphics_->owned_conservative;
    const bool capture_object = capture_pixels && pso_graphics_->as_threads[0];
    const uint32_t key = MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false, statistics) |
        (capture_pixels ? kMeshMSAACapture : 0) | (capture_object ? kMeshObjectCapture : 0);
    if (capture_pixels) allocator_->InvalidateCurrentPass();
    if (!PrepareStatisticsClaim(1))
      return;
    const D3D12_DISPATCH_MESH_ARGUMENTS arguments{X, Y, Z};
    uint64_t predicated_offset = UINT64_MAX;
    if (predication_enabled_) {
      predicated_offset = PreparePredicatedArguments(&arguments, sizeof(arguments));
      if (predicated_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    if (PreDraw(false, false, key) ==
        DrawCallStatus::Invalid)
      return;
    BindStatisticsClaim(0);
    const auto arguments_binding = predication_enabled_ ? allocator_->ResolveGPUHeap(predicated_offset)
        : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{nullptr, {}, predicated_offset, 0};
    EncodeMeshShaderDispatch(arguments_binding.buffer, arguments_binding.offset, X, Y, Z);
    const uint64_t encoder_id = allocator_->encoder_current->id;
    if (capture_pixels && !RecordCapturedMeshDraw(key, X, Y, Z)) return;
    owned_graphics::Trace("dispatch_mesh", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", encoder_id, ",\"x\":", X, ",\"y\":", Y,
                          ",\"z\":", Z);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  bool RecordCapturedMeshDraw(uint32_t key, uint32_t x, uint32_t y, uint32_t z,
                              WMT::Buffer indirect_buffer = {}, uint64_t indirect_offset = 0) {
    auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    const auto *capture = pso_graphics_->GetMeshPipeline(key);
    if (!capture) { RecordError(E_FAIL); return false; }
    const MeshPipelineVariant *object_replay = nullptr;
    if (key & kMeshObjectCapture) {
      object_replay = pso_graphics_->GetMeshPipeline((key & ~kMeshObjectCapture) | kMeshObjectReplay);
      if (!object_replay) { RecordError(E_FAIL); return false; }
    }
    const uint64_t slots = uint64_t(x) * y * z;
    const uint64_t records = slots * capture->capture_layout.max_primitives *
                             capture->capture_layout.vertices_per_primitive;
    if (records > UINT32_MAX) { RecordError(E_OUTOFMEMORY); return false; }
    const SM50_MSAA_MESH_CONTROL control{x, y, 0, uint32_t(slots)};
    if (!RecordMSAADraw(uint32_t(records), 1, 0, 0, 0, false, false, capture, &control)) return false;
    auto &draw = *render->msaa_draw;
    if (object_replay) {
      draw.object_layout = capture->object_capture_layout;
      draw.object_replay_pipeline = object_replay->pso;
    }
    draw.mesh_grid_buffer = indirect_buffer;
    draw.mesh_grid_offset = indirect_offset;
    return true;
  }

  /* Encodes the object/mesh draw of a bound MeshShader PSO; `args_offset` UINT64_MAX means direct (X, Y, Z). */
  void EncodeMeshShaderDispatch(WMT::Buffer args_buffer, uint64_t args_offset, UINT X, UINT Y, UINT Z) {
    const auto &as = pso_graphics_->as_threads;
    const auto &ms = pso_graphics_->ms_threads;
    // Without an AS, Metal still takes an object threadgroup size; it is ignored.
    const WMTSize object_size = as[0] ? WMTSize{as[0], as[1], as[2]} : WMTSize{1, 1, 1};
    const WMTSize mesh_size{ms[0], ms[1], ms[2]};
    if (args_offset != UINT64_MAX) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups_indirect>();
      cmd.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
      cmd.indirect_args_buffer = args_buffer;
      cmd.indirect_args_offset = args_offset;
      cmd.object_threadgroup_size = object_size;
      cmd.mesh_threadgroup_size = mesh_size;
    } else {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups>();
      cmd.type = WMTRenderCommandDrawMeshThreadgroups;
      cmd.threadgroup_per_grid = {X, Y, Z};
      cmd.object_threadgroup_size = object_size;
      cmd.mesh_threadgroup_size = mesh_size;
    }
  }

  void STDMETHODCALLTYPE
  SetPipelineState(ID3D12PipelineState *pPSO) try {
    if (FAILED(recording_error_)) return;
    // A bound PSO was validated by this recording; skip the COM ownership query on rebinding it.
    const bool rebind = pPSO && IsRecording() && type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        (pPSO == static_cast<ID3D12PipelineState *>(pso_graphics_.ptr()) ||
         pPSO == static_cast<ID3D12PipelineState *>(pso_compute_.ptr()));
    if (!rebind && (!IsRecording() || (pPSO && !QueryObjectOwnedByThisDevice(pPSO)))) {
      RecordError();
      return;
    }
    if (!ObserveBundlePipeline(pPSO))
      return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetPipelineState, pPSO))
      return;
    if (!pPSO) {
      pso_graphics_ = nullptr;
      pso_compute_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      return;
    }

    auto pso = static_cast<MTLD3D12PipelineState *>(pPSO);
    if (pso->IsComputePipelineState) {
      auto compute_pso = static_cast<MTLD3D12ComputePipelineState *>(pPSO);
      if (pso_compute_.ptr() == compute_pso)
        return;
      pso_compute_ = compute_pso;
      pso_graphics_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      return;
    }

    auto graphics_pso = static_cast<MTLD3D12GraphicsPipelineState *>(pPSO);
    if (pso_graphics_.ptr() == graphics_pso) {
      // Re-setting the PSO restores its depth bias / strip cut over RSSetDepthBias and
      // IASetIndexBufferStripCutValue.
      if (depth_bias_ != graphics_pso->depth_bias || depth_bias_clamp_ != graphics_pso->depth_bias_clamp ||
          slope_scaled_depth_bias_ != graphics_pso->scole_scale)
        dirty_state_.set(DirtyState::GraphicsPipelineState);
      AdoptPipelineDynamicState();
      return;
    }
    pso_graphics_ = graphics_pso;
    pso_compute_ = nullptr;
    AdoptPipelineDynamicState();
    dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState, DirtyState::VertexBuffer);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  uint64_t BarrierSubresourceCount(const D3D12_RESOURCE_DESC &desc) const {
    MTL_DXGI_FORMAT_DESC format{};
    const UINT planes = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || sampler_feedback::IsFeedbackFormat(desc.Format) ? 1 :
        SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(), desc.Format, format)) ? format.PlanarCount : 0;
    const UINT arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    return uint64_t(std::max<UINT16>(desc.MipLevels, 1)) * arrays * planes;
  }

  std::shared_ptr<BarrierTracker> BarrierStateFor(MTLD3D12Resource *resource) {
    if (resource->owned_barrier_state_ready.load(std::memory_order_acquire))
      return resource->owned_barrier_state;
    std::lock_guard<std::mutex> lock(device_->owned_barrier_state_mutex);
    if (!resource->owned_barrier_state) {
      const auto desc = resource->GetDesc();
      D3D12_HEAP_PROPERTIES heap{};
      resource->GetHeapProperties(&heap, nullptr);
      const bool broad = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
                         (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
      resource->owned_barrier_state = std::make_shared<BarrierTracker>(
          resource->owned_id, BarrierSubresourceCount(desc), broad, resource->owned_initial_state, heap.Type,
          resource->owned_initial_layout);
      resource->owned_barrier_state_ready.store(true, std::memory_order_release);
    }
    return resource->owned_barrier_state;
  }

  bool RecordImplicitUse(ID3D12Resource *resource, UINT subresource, D3D12_RESOURCE_STATES state) {
    try {
      static_cast<RecordingHead *>(entry)->state_events.push_back(
          {BarrierStateFor(static_cast<MTLD3D12Resource *>(resource)), subresource, state, state,
           D3D12_RESOURCE_BARRIER_FLAG_NONE, true});
      return true;
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return false;
    }
  }

  void CaptureBoundaryResource(RecordedBoundary &boundary, MTLD3D12Resource *resource) {
    if (!resource || resource->reserved || resource->share || resource->swizzle_layout || resource->feedback) return;
    if (resource->buffer) {
      boundary.buffer = Rc<BufferAllocation>(resource->buffer->current());
      boundary.resource_scoped = bool(boundary.buffer) && !boundary.buffer->canonicalHeap &&
          !boundary.buffer->reservedResource && !boundary.buffer->flags().test(BufferAllocationFlag::PlacementSparse);
    } else if (resource->texture) {
      const auto desc = resource->GetDesc();
      boundary.mip_count = std::max<UINT16>(desc.MipLevels, 1);
      boundary.array_count = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
      boundary.plane_count = BarrierStateFor(resource)->subresource_count / (boundary.mip_count * boundary.array_count);
      if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) boundary.texture_aspects = boundary.plane_count > 1 ? 3 : 1;
      boundary.texture = Rc<TextureAllocation>(resource->texture->current());
      // Multisample allocations are tracked by allocation/mip/slice like any
      // other texture, so their barriers scope to them instead of everything.
      boundary.resource_scoped = bool(boundary.texture) && !boundary.texture->canonicalHeap &&
          !boundary.texture->flags().test(TextureAllocationFlag::Shared) &&
          !boundary.texture->flags().test(TextureAllocationFlag::PlacementSparse) &&
          (boundary.plane_count == 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
    }
  }

  void STDMETHODCALLTYPE ResourceBarrier(UINT Count, const D3D12_RESOURCE_BARRIER *barriers) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (!IsRecording()) {
      RecordError(E_FAIL);
      return;
    }
    if (!Count)
      return;
    if (!barriers) {
      ERR("ResourceBarrier: nonzero count with null barriers");
      RecordError(E_INVALIDARG);
      return;
    }
    // Inside a render pass only barriers on work finished before the pass are conformant: no UAV
    // or aliasing barriers, and no transition of a pass surface out of its write state.
    if (render_pass_active_) {
      for (UINT i = 0; i < Count; ++i) {
        const auto &b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            (IsRenderPassSurface(b.Transition.pResource) &&
             (b.Transition.StateBefore & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE)))) {
          ERR("ResourceBarrier: non-conformant barrier inside a render pass");
          RecordError();
          return;
        }
      }
    }
    constexpr D3D12_RESOURCE_STATES valid_states = D3D12_RESOURCE_STATE_COMMON |
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
        D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
        D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE |
        D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE |
        D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE | D3D12_RESOURCE_STATE_PREDICATION;
    constexpr D3D12_RESOURCE_STATES write_states = D3D12_RESOURCE_STATE_RENDER_TARGET |
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
        D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_COPY_DEST |
        D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    auto valid_state = [](D3D12_RESOURCE_STATES state) {
      if (state & ~valid_states)
        return false;
      const UINT writes = std::popcount(UINT(state & write_states));
      return writes <= 1 && (!writes || !(state & ~write_states));
    };
    for (UINT i = 0; i < Count; ++i) {
      const auto &barrier = barriers[i];
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        if (!QueryObjectOwnedByThisDevice(barrier.Transition.pResource) ||
            (barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE &&
             barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY &&
             barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) ||
            !valid_state(barrier.Transition.StateBefore) || !valid_state(barrier.Transition.StateAfter)) {
          RecordError();
          return;
        }
        const auto desc = barrier.Transition.pResource->GetDesc();
        if (((barrier.Transition.StateBefore | barrier.Transition.StateAfter) &
             D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE) && !IsShadingRateImageDesc(desc)) {
          RecordError();
          return;
        }
        D3D12_HEAP_PROPERTIES heap{};
        // Reserved resources have no heap (GetHeapProperties fails); they have no fixed state.
        if (FAILED(barrier.Transition.pResource->GetHeapProperties(&heap, nullptr)))
          heap = {};
        if ((heap.Type == D3D12_HEAP_TYPE_UPLOAD &&
             (barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_GENERIC_READ ||
              barrier.Transition.StateAfter != D3D12_RESOURCE_STATE_GENERIC_READ)) ||
            (heap.Type == D3D12_HEAP_TYPE_READBACK &&
             (barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_COPY_DEST ||
              barrier.Transition.StateAfter != D3D12_RESOURCE_STATE_COPY_DEST))) {
          RecordError();
          return;
        }
        if (barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
          const UINT64 count = BarrierSubresourceCount(desc);
          if (!count || barrier.Transition.Subresource >= count) {
            RecordError();
            return;
          }
        }
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        if (barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
            (barrier.UAV.pResource && !QueryObjectOwnedByThisDevice(barrier.UAV.pResource))) {
          RecordError();
          return;
        }
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        if (barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
            (barrier.Aliasing.pResourceBefore && !QueryObjectOwnedByThisDevice(barrier.Aliasing.pResourceBefore)) ||
            (barrier.Aliasing.pResourceAfter && !QueryObjectOwnedByThisDevice(barrier.Aliasing.pResourceAfter))) {
          RecordError();
          return;
        }
      } else {
        RecordError();
        return;
      }
    }

    // Resources use untracked Metal storage. Consecutive commands in the same
    // encoder therefore cannot ignore a UAV or aliasing dependency. A pass
    // boundary makes the queue emit updateFence/endEncoding followed by the
    // next encoder's waitForFence. Only unsplit read-to-read transitions need
    // no GPU ordering. COMMON/PRESENT, writes, split barriers and sparse aliases
    // retain the conservative boundary without a CPU or queue-idle wait.
    for (UINT i = 0; owned_graphics::TraceEnabled() && i < Count; ++i) {
      const auto &barrier = barriers[i];
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        auto resource = static_cast<MTLD3D12Resource *>(barrier.Transition.pResource);
        owned_graphics::Trace("transition", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                              ",\"resource\":", resource ? resource->owned_id : 0,
                              ",\"dimension\":", resource ? uint32_t(resource->GetDesc().Dimension) : 0,
                              ",\"before\":", uint32_t(barrier.Transition.StateBefore),
                              ",\"after\":", uint32_t(barrier.Transition.StateAfter),
                              ",\"subresource\":", barrier.Transition.Subresource,
                              ",\"flags\":", uint32_t(barrier.Flags));
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        auto resource = static_cast<MTLD3D12Resource *>(barrier.UAV.pResource);
        owned_graphics::Trace("uav_barrier", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                              ",\"resource\":", resource ? resource->owned_id : 0);
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        auto before = static_cast<MTLD3D12Resource *>(barrier.Aliasing.pResourceBefore);
        auto after = static_cast<MTLD3D12Resource *>(barrier.Aliasing.pResourceAfter);
        owned_graphics::Trace("aliasing_barrier", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                              ",\"before_resource\":", before ? before->owned_id : 0,
                              ",\"after_resource\":", after ? after->owned_id : 0);
      }
    }
    auto &state_events = static_cast<RecordingHead *>(entry)->state_events;
    bool needs_boundary = false;
    try {
      for (UINT i = 0; i < Count; ++i) {
        const auto &barrier = barriers[i];
        auto &scope = allocator_->RecordBoundary(barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? RecordedBoundaryReason::UAV :
            barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING ? RecordedBoundaryReason::Alias :
            barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ? RecordedBoundaryReason::Split :
                                                              RecordedBoundaryReason::StateTransition);
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
          CaptureBoundaryResource(scope, static_cast<MTLD3D12Resource *>(barrier.UAV.pResource));
          scope.access_before = scope.access_after = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        }
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
          needs_boundary = true;
          continue;
        }
        needs_boundary |= barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
            barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON ||
            barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON ||
            ((barrier.Transition.StateBefore | barrier.Transition.StateAfter) & write_states) != 0;
        const auto resource = static_cast<MTLD3D12Resource *>(barrier.Transition.pResource);
        CaptureBoundaryResource(scope, resource);
        scope.subresources.IndexOrFirstMipLevel = barrier.Transition.Subresource;
        scope.access_before = D3D12_BARRIER_ACCESS(enhanced_barrier::AccessFromLegacyState(barrier.Transition.StateBefore));
        scope.access_after = D3D12_BARRIER_ACCESS(enhanced_barrier::AccessFromLegacyState(barrier.Transition.StateAfter));
        state_events.push_back({BarrierStateFor(resource), barrier.Transition.Subresource,
                                barrier.Transition.StateBefore, barrier.Transition.StateAfter, barrier.Flags});
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    if (needs_boundary || ReservedResource::AnyLive()) {
      allocator_->EndSchedulingPass();
      RecordAliasFlush();
    }
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  /*
   * Makes writes through sparse tiles visible through every other tile sharing their heap page, which
   * a pass boundary alone does not (ReservedResource::AnyLive). Recorded after each ordering barrier
   * while reserved resources exist; it costs one event signal/wait inside the command buffer.
   */
  void
  RecordAliasFlush() {
    if (!ReservedResource::AnyLive())
      return;
    auto flush = allocator_->AllocatePass<QueryEncoderData>();
    flush->type = EncoderType::Query;
    flush->op = QueryEncoderOp::AliasFlush;
    allocator_->InvalidateCurrentPass();
  }

  /*
   * ID3D12GraphicsCommandList7::Barrier (Enhanced Barriers).
   *
   * Validation follows the DirectX-Specs queue/layout/access/sync compatibility tables
   * (d3d12_enhanced_barrier.hpp); an invalid barrier fails the recording (Close returns
   * E_INVALIDARG), matching the runtime's removal of the command list.
   *
   * Texture layouts are tracked per subresource on the resource's BarrierTracker: every texture
   * barrier is recorded as a state event, and ExecuteCommandLists replays the events against the
   * layout left by earlier submissions (any queue), removing the device on a LayoutBefore that
   * does not match or on legacy/enhanced mixing that the spec forbids. Buffer barriers record an
   * event only to assert that no non-COMMON legacy state is in effect.
   *
   * Synchronization: resources use untracked Metal storage, so a barrier that orders preceding
   * work (SyncBefore != NONE) against subsequent work (SyncAfter != NONE/SPLIT) ends the current
   * encoder. The queue then emits updateFence/endEncoding followed by waitForFence at the next
   * encoder, which orders and makes visible all prior GPU memory writes. A split BEGIN defers
   * that to its END, which is always a boundary. Layout transitions need no GPU work (Metal
   * manages texture compression internally), and DISCARD leaves contents undefined, which is
   * what the spec permits.
   */
  void STDMETHODCALLTYPE
  Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP *pBarrierGroups) try {
    if (FAILED(recording_error_)) return;
    namespace eb = enhanced_barrier;
    if (RejectBundleOperation())
      return;
    if (!IsRecording()) {
      RecordError(E_FAIL);
      return;
    }
    if (!NumBarrierGroups)
      return;
    if (!pBarrierGroups) {
      ERR("Barrier: nonzero group count with null groups");
      RecordError();
      return;
    }
    // Render pass conformance, as for ResourceBarrier: nothing may consume writes of this pass.
    if (render_pass_active_) {
      for (UINT32 g = 0; g < NumBarrierGroups; ++g) {
        const auto &group = pBarrierGroups[g];
        for (UINT32 i = 0; group.pGlobalBarriers && i < group.NumBarriers; ++i) {
          const UINT before = group.Type == D3D12_BARRIER_TYPE_GLOBAL  ? group.pGlobalBarriers[i].AccessBefore
                              : group.Type == D3D12_BARRIER_TYPE_BUFFER ? group.pBufferBarriers[i].AccessBefore
                                                                        : group.pTextureBarriers[i].AccessBefore;
          const UINT after = group.Type == D3D12_BARRIER_TYPE_GLOBAL  ? group.pGlobalBarriers[i].AccessAfter
                             : group.Type == D3D12_BARRIER_TYPE_BUFFER ? group.pBufferBarriers[i].AccessAfter
                                                                       : group.pTextureBarriers[i].AccessAfter;
          const bool surface = group.Type == D3D12_BARRIER_TYPE_TEXTURE &&
                               IsRenderPassSurface(group.pTextureBarriers[i].pResource);
          // Global barriers are the aliasing form; UAV -> UAV orders writes of this pass (a UAV barrier).
          const bool uav_barrier = (before & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) &&
                                   (after & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
          if (group.Type == D3D12_BARRIER_TYPE_GLOBAL || uav_barrier ||
              (surface && (before & (D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE)))) {
            ERR("Barrier: non-conformant barrier inside a render pass");
            RecordError();
            return;
          }
        }
      }
    }
    const auto queue = type_;
    // One side of a barrier: sync and access allowed on this queue and compatible with each other.
    auto valid_side = [&](UINT sync, UINT access) {
      return eb::ValidSync(sync, queue) && eb::ValidAccess(access, queue) && eb::SyncAccessCompatible(sync, access);
    };
    auto valid_sync_pair = [&](UINT before, UINT after) {
      // SPLIT is exclusive on its side and may appear on only one side.
      return !(before == D3D12_BARRIER_SYNC_SPLIT && after == D3D12_BARRIER_SYNC_SPLIT) &&
             !((before & D3D12_BARRIER_SYNC_SPLIT) && before != D3D12_BARRIER_SYNC_SPLIT) &&
             !((after & D3D12_BARRIER_SYNC_SPLIT) && after != D3D12_BARRIER_SYNC_SPLIT);
    };
    auto no_access_exclusive = [](UINT access) {
      return !(access & D3D12_BARRIER_ACCESS_NO_ACCESS) || access == D3D12_BARRIER_ACCESS_NO_ACCESS;
    };
    auto heap_type_of = [](ID3D12Resource *resource) {
      D3D12_HEAP_PROPERTIES heap{};
      // Reserved resources have no heap; they behave as DEFAULT.
      if (FAILED(resource->GetHeapProperties(&heap, nullptr)))
        return D3D12_HEAP_TYPE_DEFAULT;
      return heap.Type;
    };
    bool boundary = false;
    auto orders = [&](UINT before, UINT after) {
      boundary |= before != D3D12_BARRIER_SYNC_NONE && after != D3D12_BARRIER_SYNC_NONE &&
                  after != D3D12_BARRIER_SYNC_SPLIT;
    };

    // Validate everything before recording anything, so a rejected call has no partial effect.
    for (UINT32 g = 0; g < NumBarrierGroups; ++g) {
      const auto &group = pBarrierGroups[g];
      if (group.NumBarriers && !group.pGlobalBarriers) {
        RecordError();
        return;
      }
      for (UINT32 i = 0; i < group.NumBarriers; ++i) {
        switch (group.Type) {
        case D3D12_BARRIER_TYPE_GLOBAL: {
          const auto &b = group.pGlobalBarriers[i];
          // Global barriers cannot be split.
          if (b.SyncBefore == D3D12_BARRIER_SYNC_SPLIT || b.SyncAfter == D3D12_BARRIER_SYNC_SPLIT ||
              !valid_sync_pair(b.SyncBefore, b.SyncAfter) || !no_access_exclusive(b.AccessBefore) ||
              !no_access_exclusive(b.AccessAfter) || !valid_side(b.SyncBefore, b.AccessBefore) ||
              !valid_side(b.SyncAfter, b.AccessAfter)) {
            ERR("Barrier: invalid global barrier");
            RecordError();
            return;
          }
          break;
        }
        case D3D12_BARRIER_TYPE_BUFFER: {
          const auto &b = group.pBufferBarriers[i];
          if (!QueryObjectOwnedByThisDevice(b.pResource)) {
            RecordError();
            return;
          }
          const auto desc = b.pResource->GetDesc();
          const auto heap = heap_type_of(b.pResource);
          const UINT heap_access = heap == D3D12_HEAP_TYPE_UPLOAD ? eb::kAccessUploadHeap
                                   : heap == D3D12_HEAP_TYPE_READBACK ? eb::kAccessReadbackHeap
                                                                      : ~eb::kAccessNotForBuffers;
          const auto heap_allows = [&](UINT access) {
            return access == D3D12_BARRIER_ACCESS_NO_ACCESS || !(access & ~heap_access);
          };
          if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || b.Offset != 0 ||
              (b.Size != UINT64_MAX && b.Size != desc.Width) || !valid_sync_pair(b.SyncBefore, b.SyncAfter) ||
              !no_access_exclusive(b.AccessBefore) || !no_access_exclusive(b.AccessAfter) ||
              !valid_side(b.SyncBefore, b.AccessBefore) || !valid_side(b.SyncAfter, b.AccessAfter) ||
              !heap_allows(b.AccessBefore) || !heap_allows(b.AccessAfter)) {
            ERR("Barrier: invalid buffer barrier");
            RecordError();
            return;
          }
          break;
        }
        case D3D12_BARRIER_TYPE_TEXTURE: {
          const auto &b = group.pTextureBarriers[i];
          if (!QueryObjectOwnedByThisDevice(b.pResource)) {
            RecordError();
            return;
          }
          const auto desc = b.pResource->GetDesc();
          const bool simultaneous = desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
          const auto valid_layout = [&](UINT layout) {
            if (layout == D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE) return IsShadingRateImageDesc(desc);
            if (layout == D3D12_BARRIER_LAYOUT_UNDEFINED)
              return true;
            // Simultaneous-access textures are immutably in the COMMON layout.
            if (simultaneous)
              return layout == D3D12_BARRIER_LAYOUT_COMMON;
            if (!eb::LayoutAllowedOnQueue(layout, queue))
              return false;
            const bool rt = layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            const bool ds = layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE ||
                            layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
            const bool uav = eb::LayoutAccessMask(layout) == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            return (!rt || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) &&
                   (!ds || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) &&
                   (!uav || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
          };
          const bool both_undefined = b.LayoutBefore == D3D12_BARRIER_LAYOUT_UNDEFINED &&
                                      b.LayoutAfter == D3D12_BARRIER_LAYOUT_UNDEFINED;
          const UINT access_mask_textures = simultaneous ? ~eb::kAccessNotForBuffers : ~0u;
          const auto texture_access = [&](UINT access) {
            return access == D3D12_BARRIER_ACCESS_NO_ACCESS || !(access & ~access_mask_textures);
          };
          if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || !valid_sync_pair(b.SyncBefore, b.SyncAfter) ||
              !no_access_exclusive(b.AccessBefore) || !no_access_exclusive(b.AccessAfter) ||
              !valid_side(b.SyncBefore, b.AccessBefore) || !valid_side(b.SyncAfter, b.AccessAfter) ||
              !valid_layout(b.LayoutBefore) || !valid_layout(b.LayoutAfter) ||
              !eb::LayoutAccessCompatible(b.LayoutBefore, b.AccessBefore, both_undefined) ||
              !eb::LayoutAccessCompatible(b.LayoutAfter, b.AccessAfter, both_undefined) ||
              !texture_access(b.AccessBefore) || !texture_access(b.AccessAfter) ||
              (b.Flags & ~D3D12_TEXTURE_BARRIER_FLAG_DISCARD) ||
              ((b.Flags & D3D12_TEXTURE_BARRIER_FLAG_DISCARD) && b.LayoutBefore != D3D12_BARRIER_LAYOUT_UNDEFINED)) {
            ERR("Barrier: invalid texture barrier");
            RecordError();
            return;
          }
          const UINT64 count = BarrierSubresourceCount(desc);
          const auto &range = b.Subresources;
          if (range.NumMipLevels == 0) {
            if (range.IndexOrFirstMipLevel != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                range.IndexOrFirstMipLevel >= count) {
              RecordError();
              return;
            }
          } else {
            const UINT mips = std::max<UINT16>(desc.MipLevels, 1);
            const UINT arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
            const UINT planes = mips && arrays ? UINT(count / (uint64_t(mips) * arrays)) : 0;
            if (!range.NumArraySlices || !range.NumPlanes || range.IndexOrFirstMipLevel >= mips ||
                range.NumMipLevels > mips - range.IndexOrFirstMipLevel || range.FirstArraySlice >= arrays ||
                range.NumArraySlices > arrays - range.FirstArraySlice || range.FirstPlane >= planes ||
                range.NumPlanes > planes - range.FirstPlane) {
              RecordError();
              return;
            }
          }
          break;
        }
        default:
          RecordError();
          return;
        }
      }
    }

    auto &state_events = static_cast<RecordingHead *>(entry)->state_events;
    try {
      for (UINT32 g = 0; g < NumBarrierGroups; ++g) {
        const auto &group = pBarrierGroups[g];
        for (UINT32 i = 0; i < group.NumBarriers; ++i) {
          if (group.Type == D3D12_BARRIER_TYPE_GLOBAL) {
            const auto &b = group.pGlobalBarriers[i];
            auto &scope = allocator_->RecordBoundary(RecordedBoundaryReason::EnhancedGlobal);
            scope.sync_before = b.SyncBefore; scope.sync_after = b.SyncAfter;
            scope.access_before = b.AccessBefore; scope.access_after = b.AccessAfter;
            orders(b.SyncBefore, b.SyncAfter);
            owned_graphics::Trace("global_barrier", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                                  ",\"sync_before\":", uint32_t(b.SyncBefore),
                                  ",\"sync_after\":", uint32_t(b.SyncAfter));
            continue;
          }
          RecordedStateEvent event{};
          event.enhanced = true;
          event.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          if (group.Type == D3D12_BARRIER_TYPE_BUFFER) {
            const auto &b = group.pBufferBarriers[i];
            auto &scope = allocator_->RecordBoundary(b.SyncBefore == D3D12_BARRIER_SYNC_SPLIT || b.SyncAfter == D3D12_BARRIER_SYNC_SPLIT
                ? RecordedBoundaryReason::Split : RecordedBoundaryReason::EnhancedBuffer);
            CaptureBoundaryResource(scope, static_cast<MTLD3D12Resource *>(b.pResource));
            scope.offset = b.Offset; scope.size = b.Size;
            scope.sync_before = b.SyncBefore; scope.sync_after = b.SyncAfter;
            scope.access_before = b.AccessBefore; scope.access_after = b.AccessAfter;
            orders(b.SyncBefore, b.SyncAfter);
            event.resource = BarrierStateFor(static_cast<MTLD3D12Resource *>(b.pResource));
            event.split_begin = b.SyncAfter == D3D12_BARRIER_SYNC_SPLIT;
            event.split_end = b.SyncBefore == D3D12_BARRIER_SYNC_SPLIT;
            event.access_before = b.AccessBefore;
            event.access_after = b.AccessAfter;
            state_events.push_back(std::move(event));
            continue;
          }
          const auto &b = group.pTextureBarriers[i];
          auto &scope = allocator_->RecordBoundary(b.SyncBefore == D3D12_BARRIER_SYNC_SPLIT || b.SyncAfter == D3D12_BARRIER_SYNC_SPLIT
              ? RecordedBoundaryReason::Split : RecordedBoundaryReason::EnhancedTexture);
          CaptureBoundaryResource(scope, static_cast<MTLD3D12Resource *>(b.pResource));
          scope.subresources = b.Subresources;
          scope.sync_before = b.SyncBefore; scope.sync_after = b.SyncAfter;
          scope.access_before = b.AccessBefore; scope.access_after = b.AccessAfter;
          orders(b.SyncBefore, b.SyncAfter);
          event.resource = BarrierStateFor(static_cast<MTLD3D12Resource *>(b.pResource));
          event.split_begin = b.SyncAfter == D3D12_BARRIER_SYNC_SPLIT;
          event.split_end = b.SyncBefore == D3D12_BARRIER_SYNC_SPLIT;
          event.layout_before = b.LayoutBefore;
          event.layout_after = b.LayoutAfter;
          event.access_before = b.AccessBefore;
          event.access_after = b.AccessAfter;
          owned_graphics::Trace("texture_barrier", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                                ",\"resource\":", static_cast<MTLD3D12Resource *>(b.pResource)->owned_id,
                                ",\"layout_before\":", uint32_t(b.LayoutBefore),
                                ",\"layout_after\":", uint32_t(b.LayoutAfter),
                                ",\"flags\":", uint32_t(b.Flags));
          const auto &range = b.Subresources;
          if (range.NumMipLevels == 0) {
            event.subresource = range.IndexOrFirstMipLevel;
            state_events.push_back(std::move(event));
            continue;
          }
          const auto desc = b.pResource->GetDesc();
          const UINT mips = std::max<UINT16>(desc.MipLevels, 1);
          const UINT arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
          const UINT64 count = event.resource->subresource_count;
          if (uint64_t(range.NumMipLevels) * range.NumArraySlices * range.NumPlanes == count) {
            state_events.push_back(std::move(event));
            continue;
          }
          for (UINT plane = range.FirstPlane; plane < range.FirstPlane + range.NumPlanes; ++plane)
            for (UINT slice = range.FirstArraySlice; slice < range.FirstArraySlice + range.NumArraySlices; ++slice)
              for (UINT mip = range.IndexOrFirstMipLevel; mip < range.IndexOrFirstMipLevel + range.NumMipLevels;
                   ++mip) {
                event.subresource = mip + slice * mips + plane * mips * arrays;
                state_events.push_back(event);
              }
        }
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    if (boundary) {
      allocator_->EndSchedulingPass();
      RecordAliasFlush();
    }
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *CommandList) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || !CommandList ||
        CommandList->GetType() != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      RecordError();
      return;
    }
    Com<ID3D12Device> bundle_device;
    if (FAILED(CommandList->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&bundle_device))) ||
        bundle_device.ptr() != static_cast<ID3D12Device *>(device_)) {
      RecordError();
      return;
    }
    auto bundle = static_cast<MTLD3D12GraphicsCommandListImpl *>(CommandList);
    if (bundle->IsRecording() || FAILED(bundle->close_error_) || FAILED(bundle->recording_error_) ||
        !bundle->allocator_ ||
        bundle->bundle_allocator_generation_ != bundle->allocator_->recording_generation_->load(std::memory_order_acquire) ||
        (bundle->bundle_heaps_set_ && bundle->descriptor_heaps_ != descriptor_heaps_)) {
      RecordError();
      return;
    }

    try {
      owned_bundle_references.emplace_back(bundle->bundle_recording_, bundle->allocator_->recording_generation_,
                                           bundle->bundle_allocator_generation_);
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }

    // DX12 bundles inherit every other state, and their state changes persist
    // in the caller. In particular, do not restore the parent's old state here.
    SetPipelineState(bundle->bundle_initial_pso_);
    IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    view_instance_mask_ = 0;
    for (const auto &command : bundle->bundle_commands_) {
      command(*this);
      if (FAILED(recording_error_))
        break;
    }
    owned_graphics::Trace("execute_bundle", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"bundle\":", bundle->owned_id, ",\"bundle_recording\":", bundle->owned_recording,
                          ",\"commands\":", bundle->bundle_commands_.size());
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT HeapCount, ID3D12DescriptorHeap *const *Heaps) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || HeapCount > descriptor_heaps_.size() || (HeapCount && !Heaps)) {
      RecordError();
      return;
    }
    std::array<ID3D12DescriptorHeap *, 2> heaps = {};
    for (UINT i = 0; i < HeapCount; ++i) {
      if (!Heaps[i]) {
        RecordError();
        return;
      }
      auto desc = Heaps[i]->GetDesc();
      if ((desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
           desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) ||
          !(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || heaps[desc.Type]) {
        RecordError();
        return;
      }
      Com<ID3D12Device> heap_device;
      if (FAILED(Heaps[i]->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&heap_device))) ||
          heap_device.ptr() != static_cast<ID3D12Device *>(device_)) {
        RecordError();
        return;
      }
      heaps[desc.Type] = Heaps[i];
    }
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      if (bundle_heaps_set_ && heaps != descriptor_heaps_) {
        RecordError();
        return;
      }
      bundle_heaps_set_ = true;
    }
    // Root tables contain complete GPU addresses. Keep the binding identities
    // for the ExecuteBundle contract even though Metal needs no separate bind.
    try {
      auto &retained = static_cast<RecordingHead *>(entry)->descriptor_heaps;
      for (auto *heap : heaps) {
        if (!heap) continue;
        if (std::none_of(retained.begin(), retained.end(), [&](const auto &old) { return old.ptr() == heap; }))
          retained.emplace_back(heap);
      }
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
    const bool heaps_changed = descriptor_heaps_ != heaps;
    descriptor_heaps_ = heaps;
    if (StageDirectlyIndexedHeaps(rootsig_compute_.ptr(), rootarg_compute_staging_))
      if (heaps_changed)
        dirty_state_.set(DirtyState::ComputeRootArguments);
    if (StageDirectlyIndexedHeaps(rootsig_graphics_.ptr(), rootarg_graphics_staging_))
      if (heaps_changed)
        dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  /**
   * Publish the bound descriptor heaps to a HEAP_DIRECTLY_INDEXED root signature.
   *
   * SM 6.6 ResourceDescriptorHeap[] / SamplerDescriptorHeap[] read the heaps set by
   * SetDescriptorHeaps. The signature reserves four root qwords for them (base and
   * descriptor count per heap type), so the shader bounds every heap index by the heap
   * that is actually bound. D3D12 requires SetDescriptorHeaps before the signature is
   * set; staging on either call keeps the qwords current in both orders. A heap type
   * that is not bound stages {0, 0}, which the shader resolves to null descriptors.
   */
  bool StageDirectlyIndexedHeaps(MTLD3D12RootSignature *rootsig, uint64_t *staging) {
    if (!rootsig || rootsig->HeapQwordOffset == ~0u)
      return false;
    for (unsigned type = 0; type < descriptor_heaps_.size(); ++type) {
      uint64_t base = 0, count = 0;
      if (auto *heap = descriptor_heaps_[type]) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        D3D12_GPU_DESCRIPTOR_HANDLE start{};
        heap->GetDesc(&desc);
        heap->GetGPUDescriptorHandleForHeapStart(&start);
        base = start.ptr;
        count = base ? desc.NumDescriptors : 0;
      }
      staging[rootsig->HeapQwordOffset + type * 2] = base;
      staging[rootsig->HeapQwordOffset + type * 2 + 1] = count;
    }
    return true;
  }

  void STDMETHODCALLTYPE
  SetComputeRootSignature(ID3D12RootSignature *pRootSignature) try {
    if (FAILED(recording_error_)) return;
    // The bound signature was validated by this recording; rebinding it is a no-op in direct lists.
    if (pRootSignature && IsRecording() && type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        rootsig_compute_.ptr() == pRootSignature)
      return;
    if (!IsRecording() || (pRootSignature && !QueryObjectOwnedByThisDevice(pRootSignature))) {
      RecordError();
      return;
    }
    if (pRootSignature) {
      const auto *root = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      if (root->IsLocal || root->UploadQwords > MTLD3D12RootSignature::kMaxRootArgumentQwords) {
        RecordError();
        return;
      }
    }
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootSignature, pRootSignature))
      return;
    if (rootsig_compute_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_compute_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_compute_->UploadQwords <= std::size(rootarg_compute_staging_));
      StageDirectlyIndexedHeaps(rootsig_compute_.ptr(), rootarg_compute_staging_);
      ClearRootViewStatus(rootsig_compute_.ptr(), rootarg_compute_staging_);
      StageViewId(rootsig_compute_.ptr(), rootarg_compute_staging_, 0);
    } else {
      rootsig_compute_ = nullptr;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) try {
    if (FAILED(recording_error_)) return;
    // The bound signature was validated by this recording; rebinding it is a no-op in direct lists.
    if (pRootSignature && IsRecording() && type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        rootsig_graphics_.ptr() == pRootSignature)
      return;
    if (!IsRecording() || (pRootSignature && !QueryObjectOwnedByThisDevice(pRootSignature))) {
      RecordError();
      return;
    }
    if (pRootSignature) {
      const auto *root = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      if (root->IsLocal || root->UploadQwords > MTLD3D12RootSignature::kMaxRootArgumentQwords) {
        RecordError();
        return;
      }
    }
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootSignature, pRootSignature))
      return;
    if (rootsig_graphics_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_graphics_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_graphics_->UploadQwords <= std::size(rootarg_graphics_staging_));
      StageDirectlyIndexedHeaps(rootsig_graphics_.ptr(), rootarg_graphics_staging_);
      ClearRootViewStatus(rootsig_graphics_.ptr(), rootarg_graphics_staging_);
      StageViewId(rootsig_graphics_.ptr(), rootarg_graphics_staging_, view_id_);
    } else {
      rootsig_graphics_ = nullptr;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) try {
    if (FAILED(recording_error_)) return;
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE && !descriptor_heaps_[0] && !descriptor_heaps_[1]) {
      RecordError();
      return;
    }
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootDescriptorTable, Index, BaseDescriptor))
      return;
    if (!ValidateRootParameter(rootsig_compute_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE))
      return;
    auto &slot = rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]];
    if (slot == BaseDescriptor.ptr)
      return;
    slot = BaseDescriptor.ptr;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) try {
    if (FAILED(recording_error_)) return;
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE && !descriptor_heaps_[0] && !descriptor_heaps_[1]) {
      RecordError();
      return;
    }
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootDescriptorTable, Index, BaseDescriptor))
      return;
    if (!ValidateRootParameter(rootsig_graphics_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE))
      return;
    auto &slot = rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]];
    if (slot == BaseDescriptor.ptr)
      return;
    slot = BaseDescriptor.ptr;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRoot32BitConstant, Index, Data, DstOffset))
      return;
    if (!ValidateRootParameter(rootsig_compute_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 1, DstOffset))
      return;
    auto dst = reinterpret_cast<uint32_t *>(rootarg_compute_staging_ + rootsig_compute_->SlotQwordOffsets[Index]);
    if (dst[DstOffset] == Data)
      return;
    dst[DstOffset] = Data;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRoot32BitConstant, Index, Data, DstOffset))
      return;
    if (!ValidateRootParameter(rootsig_graphics_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 1, DstOffset))
      return;
    auto dst = reinterpret_cast<uint32_t *>(rootarg_graphics_staging_ + rootsig_graphics_->SlotQwordOffsets[Index]);
    if (dst[DstOffset] == Data)
      return;
    dst[DstOffset] = Data;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetComputeRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundleConstants(false, Index, ConstantCount, pData, DstOffset))
      return;
    if (ConstantCount && !pData) {
      RecordError();
      return;
    }
    if (!ValidateRootParameter(rootsig_compute_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                               ConstantCount, DstOffset))
      return;
    auto src = reinterpret_cast<const uint32_t *>(pData);
    auto dst = reinterpret_cast<uint32_t *>(rootarg_compute_staging_ + rootsig_compute_->SlotQwordOffsets[Index]);
    bool changed = false;
    for (unsigned i = 0; i < ConstantCount; i++) {
      changed |= dst[i + DstOffset] != src[i];
      dst[i + DstOffset] = src[i];
    }
    if (changed)
      dirty_state_.set(DirtyState::ComputeRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundleConstants(true, Index, ConstantCount, pData, DstOffset))
      return;
    if (ConstantCount && !pData) {
      RecordError();
      return;
    }
    if (!ValidateRootParameter(rootsig_graphics_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                               ConstantCount, DstOffset))
      return;
    auto src = reinterpret_cast<const uint32_t *>(pData);
    auto dst = reinterpret_cast<uint32_t *>(rootarg_graphics_staging_ + rootsig_graphics_->SlotQwordOffsets[Index]);
    bool changed = false;
    for (unsigned i = 0; i < ConstantCount; i++) {
      changed |= dst[i + DstOffset] != src[i];
      dst[i + DstOffset] = src[i];
    }
    if (changed)
      dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  /*
   * TILED: the residency status base of a root SRV/UAV, kept in the parameter's status qword
   * (MTLD3D12RootSignature::StatusQwordOffsets). It is the reserved buffer's biased base, valid for
   * any address inside the buffer, or 0 (fully mapped) for other buffers and unknown addresses.
   */
  /* Status qwords may alias other data of a previous signature; start them at "fully mapped". */
  void
  ClearRootViewStatus(MTLD3D12RootSignature *rootsig, uint64_t *staging) {
    for (UINT i = 0; i < rootsig->ParameterSlots; i++)
      if (rootsig->StatusQwordOffsets[i] != ~0u)
        staging[rootsig->StatusQwordOffsets[i]] = 0;
  }

  /* The view-id qword (SV_ViewID) of a non-local signature; a previous signature may have left other data there. */
  static void
  StageViewId(MTLD3D12RootSignature *rootsig, uint64_t *staging, UINT view_id) {
    if (rootsig->ViewIdQwordOffset != ~0u)
      staging[rootsig->ViewIdQwordOffset] = view_id;
  }

  // ICB shader-owned routing uses the effective native pass bounds. Keep the
  // logical ViewID in the low half, unchanged for every application shader.
  void StageIndirectViewBounds() {
    if (!rootsig_graphics_ || rootsig_graphics_->ViewIdQwordOffset == ~0u)
      return;
    const auto *render = static_cast<const RenderEncoderData *>(allocator_->encoder_current);
    rootarg_graphics_staging_[rootsig_graphics_->ViewIdQwordOffset] = uint64_t(view_id_) |
        (uint64_t(std::max(1u, num_viewports)) << 32) |
        (uint64_t(std::max(1u, uint32_t(render->render_target_array_length))) << 48);
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  }

  /*
   * View instancing, Tier 1: replay `draw` once per enabled view of the bound PSO, in view order, with SV_ViewID
   * (the root view-id qword) and the view's location set. Returns false when the PSO has no view instancing, and
   * the caller then draws once. A draw with every view masked off draws nothing. The view-instanced draw is
   * logically ViewInstanceCount draws, so per-view pipeline statistics, predication and stream output follow the
   * ordinary draw rules (D3D12 view instancing spec: Tier 1 increases all statistics like a non-instanced draw).
   */
  template <typename Draw>
  bool ReplayPerView(Draw &&draw) {
    if (view_replay_ || !pso_graphics_ || !pso_graphics_->view_instancing.count)
      return false;
    const auto &state = pso_graphics_->view_instancing;
    const UINT all = (1u << state.count) - 1;
    const UINT enabled = state.masking ? view_instance_mask_ & all : all;
    view_replay_ = true;
    for (UINT view = 0; view < state.count && SUCCEEDED(recording_error_); ++view) {
      if (!(enabled & (1u << view)))
        continue;
      view_id_ = view;
      view_location_ = state.locations[view];
      if (rootsig_graphics_) {
        StageViewId(rootsig_graphics_.ptr(), rootarg_graphics_staging_, view);
        dirty_state_.set(DirtyState::GraphicsRootArguments);
      }
      draw();
    }
    view_replay_ = false;
    view_id_ = 0;
    view_location_ = {};
    if (rootsig_graphics_) {
      StageViewId(rootsig_graphics_.ptr(), rootarg_graphics_staging_, 0);
      dirty_state_.set(DirtyState::GraphicsRootArguments);
    }
    return true;
  }

  void
  SetRootViewStatus(MTLD3D12RootSignature *rootsig, uint64_t *staging, UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    const uint32_t slot = rootsig->StatusQwordOffsets[Index];
    BufferAllocation *allocation = nullptr;
    try {
      allocation = LookupRecordedBufferByVA(VA);
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
    if (slot != ~0u) staging[slot] = allocation ? allocation->residencyStatusBase : 0;
  }

  bool UpdateRootView(MTLD3D12RootSignature *rootsig, uint64_t *staging, UINT Index,
                      D3D12_GPU_VIRTUAL_ADDRESS VA) {
    const uint32_t value_slot = rootsig->SlotQwordOffsets[Index];
    const uint32_t status_slot = rootsig->StatusQwordOffsets[Index];
    const uint64_t old_value = staging[value_slot];
    const uint64_t old_status = status_slot == ~0u ? 0 : staging[status_slot];
    staging[value_slot] = VA;
    SetRootViewStatus(rootsig, staging, Index, VA);
    return old_value != VA || (status_slot != ~0u && old_status != staging[status_slot]);
  }

  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootConstantBufferView, Index, VA))
      return;
    if (!ValidateRootParameter(rootsig_compute_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_CBV))
      return;
    if (UpdateRootView(rootsig_compute_.ptr(), rootarg_compute_staging_, Index, VA))
      dirty_state_.set(DirtyState::ComputeRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootConstantBufferView, Index, VA))
      return;
    if (!ValidateRootParameter(rootsig_graphics_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_CBV))
      return;
    if (UpdateRootView(rootsig_graphics_.ptr(), rootarg_graphics_staging_, Index, VA))
      dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootShaderResourceView, Index, VA))
      return;
    if (!ValidateRootParameter(rootsig_compute_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_SRV))
      return;
    if (UpdateRootView(rootsig_compute_.ptr(), rootarg_compute_staging_, Index, VA))
      dirty_state_.set(DirtyState::ComputeRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootShaderResourceView, Index, VA))
      return;
    if (!ValidateRootParameter(rootsig_graphics_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_SRV))
      return;
    if (UpdateRootView(rootsig_graphics_.ptr(), rootarg_graphics_staging_, Index, VA))
      dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootUnorderedAccessView, Index, VA))
      return;
    if (!ValidateRootParameter(rootsig_compute_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_UAV))
      return;
    if (UpdateRootView(rootsig_compute_.ptr(), rootarg_compute_staging_, Index, VA))
      dirty_state_.set(DirtyState::ComputeRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootUnorderedAccessView, Index, VA))
      return;
    if (!ValidateRootParameter(rootsig_graphics_.ptr(), Index, D3D12_ROOT_PARAMETER_TYPE_UAV))
      return;
    if (UpdateRootView(rootsig_graphics_.ptr(), rootarg_graphics_staging_, Index, VA))
      dirty_state_.set(DirtyState::GraphicsRootArguments);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) try {
    if (FAILED(recording_error_)) return;
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      D3D12_INDEX_BUFFER_VIEW view = pView ? *pView : D3D12_INDEX_BUFFER_VIEW{};
      AppendBundleCommand([view, present = pView != nullptr](auto &parent) {
        parent.IASetIndexBuffer(present ? &view : nullptr);
      });
      return;
    }
    auto index_buffer_allocation = pView ? device_->LookupBufferByVA(pView->BufferLocation, &index_offset) : nullptr;
    if (index_buffer_allocation) {
      index_buffer_address = pView->BufferLocation;
      index_buffer = index_buffer_allocation->buffer();
      index_type = pView->Format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    } else {
      index_buffer_address = 0;
      index_buffer = {};
      index_type = {};
      index_offset = {};
    }
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  IASetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW *Views) try {
    if (FAILED(recording_error_)) return;
    if (StartSlot > vertex_buffers_.size() || Count > vertex_buffers_.size() - StartSlot) {
      RecordError();
      return;
    }
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> views = {};
      if (Views)
        std::copy_n(Views, Count, views.begin());
      AppendBundleCommand([StartSlot, Count, views](auto &parent) {
        parent.IASetVertexBuffers(StartSlot, Count, views.data());
      });
      return;
    }
    // The table stores only view fields (resolved at draw time), so an identical rebind leaves it valid.
    bool changed = false;
    for (unsigned Slot = StartSlot; Slot < StartSlot + Count; Slot++) {
      const auto view = Views ? Views[Slot - StartSlot] : D3D12_VERTEX_BUFFER_VIEW{};
      auto &state = vertex_buffers_[Slot];
      if (state.BufferLocation != view.BufferLocation || state.SizeInBytes != view.SizeInBytes ||
          state.StrideInBytes != view.StrideInBytes) {
        state = view;
        changed = true;
      }
    }
    if (changed)
      dirty_state_.set(DirtyState::VertexBuffer);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT Count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *Views) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (!IsRecording())
      return;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || StartSlot > so_targets_.size() ||
        Count > so_targets_.size() - StartSlot) {
      RecordError();
      return;
    }
    // Validate every view before changing any slot, so a rejected call leaves the bindings intact.
    std::array<StreamOutputTarget, D3D12_SO_BUFFER_SLOT_COUNT> targets{};
    for (UINT i = 0; i < Count; i++) {
      // A null array, a zero size or a null location unbinds the slot (the view's fields
      // are unused when SizeInBytes is 0).
      if (!Views || !Views[i].BufferLocation || !Views[i].SizeInBytes)
        continue;
      const auto &view = Views[i];
      uint64_t offset = 0, filled_offset = 0;
      auto *buffer = device_->LookupBufferByVA(view.BufferLocation, &offset);
      auto *filled = (view.BufferFilledSizeLocation & 3)
                         ? nullptr
                         : device_->LookupBufferByVA(view.BufferFilledSizeLocation, &filled_offset);
      if (!buffer || (view.BufferLocation & 3) || !RangeFits(offset, view.SizeInBytes, buffer->byteLength()) ||
          !filled || !RangeFits(filled_offset, sizeof(UINT), filled->byteLength())) {
        RecordError();
        return;
      }
      targets[i] = {buffer, offset, view.SizeInBytes, filled, filled_offset};
    }
    for (UINT i = 0; i < Count; i++)
      so_targets_[StartSlot + i] = std::move(targets[i]);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  OMSetRenderTargets(
      UINT NumRTV, const D3D12_CPU_DESCRIPTOR_HANDLE *RTVs, WINBOOL SingleDescriptor,
      const D3D12_CPU_DESCRIPTOR_HANDLE *DSV
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || NumRTV > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        (NumRTV && !RTVs)) {
      RecordError();
      return;
    }
    bool reuse_render = false;
    const bool trace_reuse = owned_graphics::TraceEnabled();
    const char *reuse_reason = nullptr;
    int failed_rtv = -1;
    // Depth-only passes have no RTVs but can still retain the same render encoder.
    if (!render_pass_internal_ && (NumRTV || (DSV && DSV->ptr)) && allocator_->encoder_current &&
        allocator_->encoder_current->type == EncoderType::Render) {
      const auto *render = static_cast<const RenderEncoderData *>(allocator_->encoder_current);
      reuse_render = render->render_target_count == NumRTV && !render->dsv_readonly_flags &&
          !render->default_raster_sample_count && !render->native_msaa_sample_count &&
          !render->atomic64_phase && !render->msaa_draw &&
          !render->capture_indirect && !render->so_indirect && render->so_draws.empty() &&
          !render->use_tessellation && !render->use_geometry;
      if (trace_reuse && !reuse_render)
        reuse_reason = render->render_target_count != NumRTV ? "count" : "ordinary_flags";
      unsigned width = 16384, height = 16384, array_length = 0;
      for (unsigned i = 0; reuse_render && i < NumRTV; i++) {
        const auto handle = SingleDescriptor
            ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * 32 /* kRTVDSVHeapIncrementalSize */} : RTVs[i];
        const auto &rt = render->colors[i];
        if (!handle.ptr || rt.buffer_attachment || rt.resolve_attachment || rt.level || rt.slice ||
            (rt.load_action != WMTLoadActionLoad && rt.load_action != WMTLoadActionClear) ||
            (rt.store_action != WMTStoreActionStore && rt.store_action != WMTStoreActionDontCare)) {
          reuse_render = false;
          if (trace_reuse) {
            reuse_reason = "rtv_state";
            failed_rtv = i;
          }
          break;
        }
        auto [heap, index] = GetRenderTargetHeap(device_, handle);
        const auto desc = heap->GetRenderTarget(index);
        // Resolve the descriptor now: its CPU slot may have been overwritten since the last draw.
        // TextureView identity includes the allocation, format, mip and array range; 3D planes are separate.
        reuse_render = desc.Texture && !desc.BufferResource &&
            &desc.Texture->view(desc.View) == rt.attachment.ptr() &&
            desc.DepthPlane == rt.depth_plane &&
            desc.CenterSamplePattern == (pso_graphics_ && pso_graphics_->owned_sample_desc.Quality == 0xfffffffeu);
        if (trace_reuse && !reuse_render) {
          reuse_reason = !desc.Texture ? "rtv_no_texture" : desc.BufferResource ? "rtv_buffer" :
              &desc.Texture->view(desc.View) != rt.attachment.ptr() ? "rtv_view" :
              desc.DepthPlane != rt.depth_plane ? "rtv_plane" : "rtv_sample";
          failed_rtv = i;
        }
        width = std::min(width, desc.Width);
        height = std::min(height, desc.Height);
        array_length = std::max(array_length, desc.RenderTargetArrayLength);
      }
      if (reuse_render && DSV && DSV->ptr) {
        auto [heap, index] = GetRenderTargetHeap(device_, *DSV);
        const auto desc = heap->GetRenderTarget(index);
        reuse_render = desc.Texture && !desc.BufferResource && desc.Texture->sampleCount() == 1 &&
            desc.Flags == render->dsv_descriptor_flags &&
            desc.CenterSamplePattern == (pso_graphics_ && pso_graphics_->owned_sample_desc.Quality == 0xfffffffeu);
        if (trace_reuse && !reuse_render)
          reuse_reason = "dsv_descriptor_flags_samples";
        if (reuse_render) {
          const auto aspects = DepthStencilPlanarFlags(desc.Texture->pixelFormat(desc.View));
          const auto *view = &desc.Texture->view(desc.View);
          // Compare retained views and flags, never the old mutable CPU descriptor slots.
          const auto &depth = render->depth;
          const auto &stencil = render->stencil;
          reuse_render = aspects == render->dsv_planar_flags &&
              ((aspects & 1) ? depth.attachment.ptr() == view && !depth.level && !depth.slice &&
                  depth.depth_plane == desc.DepthPlane &&
                  (depth.load_action == WMTLoadActionLoad || depth.load_action == WMTLoadActionClear) &&
                  (depth.store_action == WMTStoreActionStore || depth.store_action == WMTStoreActionDontCare) : !depth.attachment) &&
              ((aspects & 2) ? stencil.attachment.ptr() == view && !stencil.level && !stencil.slice &&
                  stencil.depth_plane == desc.DepthPlane &&
                  (stencil.load_action == WMTLoadActionLoad || stencil.load_action == WMTLoadActionClear) &&
                  (stencil.store_action == WMTStoreActionStore || stencil.store_action == WMTStoreActionDontCare) : !stencil.attachment);
          if (trace_reuse && !reuse_render)
            reuse_reason = "dsv_view_aspects_actions";
          width = std::min(width, desc.Width);
          height = std::min(height, desc.Height);
          array_length = std::max(array_length, desc.RenderTargetArrayLength);
        }
      } else if (reuse_render) {
        reuse_render = !render->depth.attachment && !render->stencil.attachment &&
            !render->dsv_planar_flags && !render->dsv_descriptor_flags;
        if (trace_reuse && !reuse_render)
          reuse_reason = "dsv_removal";
      }
      reuse_render = reuse_render && width == render->render_target_width &&
          height == render->render_target_height && array_length == render->render_target_array_length;
      if (trace_reuse && !reuse_render && !reuse_reason)
        reuse_reason = "dimensions_array";
    }
    if (trace_reuse && !reuse_render && allocator_->encoder_current &&
        allocator_->encoder_current->type == EncoderType::Render) {
      const auto *render = static_cast<const RenderEncoderData *>(allocator_->encoder_current);
      if (!reuse_reason)
        reuse_reason = render_pass_internal_ ? "internal_render_pass" : "zero_rtvs";
      owned_graphics::Trace("render_targets_rebind_rejected", ",\"list\":", owned_id,
                            ",\"recording\":", owned_recording, ",\"encoder\":", render->id,
                            ",\"reason\":\"", reuse_reason, "\",\"incoming_rtvs\":", NumRTV,
                            ",\"retained_rtvs\":", uint32_t(render->render_target_count),
                            ",\"incoming_dsv\":", bool(DSV && DSV->ptr),
                            ",\"retained_dsv\":", bool(render->depth.attachment || render->stencil.attachment),
                            ",\"failed_rtv\":", failed_rtv);
    }
    if (reuse_render)
      owned_graphics::Trace("render_targets_rebind_elided", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"encoder\":", allocator_->encoder_current->id, ",\"rtvs\":", NumRTV);
    else
      allocator_->EndSchedulingPass();

    num_rtvs = NumRTV;
    for (unsigned i = 0; i < NumRTV; i++) {
      auto RTV = SingleDescriptor ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * 32 /* kRTVDSVHeapIncrementalSize */}
                                  : RTVs[i];
      rtvs[i] = RTV;
    }
    dsv = DSV ? *DSV : D3D12_CPU_DESCRIPTOR_HANDLE();
    async_targets_epoch_ = 0;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  ClearDepthStencilView(
      D3D12_CPU_DESCRIPTOR_HANDLE DSV, D3D12_CLEAR_FLAGS Flags, FLOAT Depth, UINT8 Stencil, UINT RectCount,
      const D3D12_RECT *Rects
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError();
      return;
    }
    auto [Heap, Index] = GetRenderTargetHeap(device_, DSV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    auto CheckedFlags = Flags & DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
    if (!CheckedFlags || (RectCount && !Rects))
      return;
    if ((CheckedFlags & D3D12_CLEAR_FLAG_DEPTH) && sample_positions_.count &&
        sample_positions_.count != AttachmentDesc.Texture->sampleCount()) {
      RecordError();
      return;
    }

    for (UINT i = 0; !predication_enabled_ && i < RectCount; i++) {
      if (Rects[i].left <= 0 && Rects[i].top <= 0 &&
          Rects[i].right >= LONG(AttachmentDesc.Width) && Rects[i].bottom >= LONG(AttachmentDesc.Height)) {
        owned_graphics::Trace("clear_rect_normalized", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                              ",\"kind\":\"dsv\",\"width\":", AttachmentDesc.Width, ",\"height\":", AttachmentDesc.Height);
        RectCount = 0;
        break;
      }
    }

    if (predication_enabled_ || RectCount) {
      const D3D12_RECT full_rect = {0, 0, LONG(AttachmentDesc.Width), LONG(AttachmentDesc.Height)};
      const D3D12_RECT *clear_rects = RectCount ? Rects : &full_rect;
      const UINT clear_rect_count = RectCount ? RectCount : 1;

      small_vector<uint64_t, 4> indirect_offsets;
      if (predication_enabled_) {
        indirect_offsets.reserve(clear_rect_count);
        const uint32_t draw_arguments[] = {
            3, std::max(AttachmentDesc.RenderTargetArrayLength, 1u), 0, 0
        };
        for (UINT i = 0; i < clear_rect_count; i++)
          indirect_offsets.push_back(PreparePredicatedArguments(draw_arguments, sizeof(draw_arguments)));
      }

      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, 0, CheckedFlags);
      if (CheckedFlags & D3D12_CLEAR_FLAG_DEPTH)
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->sample_positions = sample_positions_;
      for (UINT i = 0; i < clear_rect_count; i++) {
        const auto &rect = clear_rects[i];
        const LONG rect_offset_x = std::clamp(rect.left, LONG(0), LONG(AttachmentDesc.Width));
        const LONG rect_offset_y = std::clamp(rect.top, LONG(0), LONG(AttachmentDesc.Height));
        const LONG rect_width = std::clamp(rect.right, LONG(0), LONG(AttachmentDesc.Width)) - rect_offset_x;
        const LONG rect_height = std::clamp(rect.bottom, LONG(0), LONG(AttachmentDesc.Height)) - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        {
                  if (FAILED(recording_error_)) return;
                  const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(predication_enabled_ ? indirect_offsets[i] : 0)
                      : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
                  allocator_->clear_rtv_.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, Depth, Stencil, arguments.buffer, arguments.offset);
                };
      }
      allocator_->clear_rtv_.end();
      return;
    }

    allocator_->EndSchedulingPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = CheckedFlags;
    if (CheckedFlags & D3D12_CLEAR_FLAG_DEPTH) encoder_info->sample_positions = sample_positions_;
    encoder_info->depth_stencil = {Depth, Stencil};
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = 0;

    allocator_->EndSchedulingPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  ClearRenderTargetView(
      D3D12_CPU_DESCRIPTOR_HANDLE RTV, const FLOAT Color[4], UINT RectCount, const D3D12_RECT *Rects
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || !Color) {
      RecordError();
      return;
    }
    auto [Heap, Index] = GetRenderTargetHeap(device_, RTV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (RectCount && !Rects)
      return;

    if (AttachmentDesc.BufferResource) {
      WMTClearColor sanitized_color = {Color[0], Color[1], Color[2], Color[3]};
      SanitizeRTVClearColor(AttachmentDesc.Format, sanitized_color);
      const std::array<float, 4> clear_color = {
          float(sanitized_color.r), float(sanitized_color.g), float(sanitized_color.b), float(sanitized_color.a)
      };
      if (!RectCount) {
        uint64_t indirect_offset = 0;
        if (predication_enabled_) {
          const uint32_t dispatch_arguments[] = {
              uint32_t((uint64_t(AttachmentDesc.BufferRange.elementCount) + 31) / 32), 1, 1
          };
          indirect_offset = PreparePredicatedArguments(dispatch_arguments, sizeof(dispatch_arguments));
        }
        allocator_->clear_uav_.beginRenderTarget(
            clear_color, AttachmentDesc.BufferResource, AttachmentDesc.BufferView, AttachmentDesc.BufferRange,
            AttachmentDesc.Format
        );
        {
                  if (FAILED(recording_error_)) return;
                  const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(indirect_offset)
                      : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
                  allocator_->clear_uav_.clear(AttachmentDesc.BufferRange.firstElement, 0, AttachmentDesc.BufferRange.elementCount, 1, arguments.buffer, arguments.offset);
                };
        allocator_->clear_uav_.end();
        return;
      }

      const LONG maximum_x = LONG(std::min<UINT>(AttachmentDesc.BufferRange.elementCount, LONG_MAX));
      small_vector<uint64_t, 4> indirect_offsets;
      if (predication_enabled_) {
        indirect_offsets.reserve(RectCount);
        for (UINT i = 0; i < RectCount; i++) {
          const LONG left = std::clamp(Rects[i].left, LONG(0), maximum_x);
          const LONG right = std::clamp(Rects[i].right, LONG(0), maximum_x);
          if (right <= left || Rects[i].bottom <= 0 || Rects[i].top >= 1) {
            indirect_offsets.push_back(0);
            continue;
          }
          const uint32_t dispatch_arguments[] = {
              uint32_t((uint64_t(uint32_t(right - left)) + 31) / 32), 1, 1
          };
          indirect_offsets.push_back(PreparePredicatedArguments(dispatch_arguments, sizeof(dispatch_arguments)));
        }
      }
      allocator_->clear_uav_.beginRenderTarget(
          clear_color, AttachmentDesc.BufferResource, AttachmentDesc.BufferView, AttachmentDesc.BufferRange,
          AttachmentDesc.Format
      );
      for (UINT i = 0; i < RectCount; i++) {
        const LONG left = std::clamp(Rects[i].left, LONG(0), maximum_x);
        const LONG right = std::clamp(Rects[i].right, LONG(0), maximum_x);
        if (right <= left || Rects[i].bottom <= 0 || Rects[i].top >= 1)
          continue;
        {
                  if (FAILED(recording_error_)) return;
                  const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(predication_enabled_ ? indirect_offsets[i] : 0)
                      : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
                  allocator_->clear_uav_.clear(AttachmentDesc.BufferRange.firstElement + uint32_t(left), 0, uint32_t(right - left), 1, arguments.buffer, arguments.offset);
                };
      }
      allocator_->clear_uav_.end();
      return;
    }

    if (!AttachmentDesc.Texture)
      return;

    for (UINT i = 0; !predication_enabled_ && i < RectCount; i++) {
      if (Rects[i].left <= 0 && Rects[i].top <= 0 &&
          Rects[i].right >= LONG(AttachmentDesc.Width) && Rects[i].bottom >= LONG(AttachmentDesc.Height)) {
        owned_graphics::Trace("clear_rect_normalized", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                              ",\"kind\":\"rtv\",\"width\":", AttachmentDesc.Width, ",\"height\":", AttachmentDesc.Height);
        RectCount = 0;
        break;
      }
    }

    if (predication_enabled_ || RectCount) {
      const D3D12_RECT full_rect = {0, 0, LONG(AttachmentDesc.Width), LONG(AttachmentDesc.Height)};
      const D3D12_RECT *clear_rects = RectCount ? Rects : &full_rect;
      const UINT clear_rect_count = RectCount ? RectCount : 1;

      small_vector<uint64_t, 4> indirect_offsets;
      if (predication_enabled_) {
        indirect_offsets.reserve(clear_rect_count);
        const uint32_t draw_arguments[] = {
            3, std::max(AttachmentDesc.RenderTargetArrayLength, 1u), 0, 0
        };
        for (UINT i = 0; i < clear_rect_count; i++)
          indirect_offsets.push_back(PreparePredicatedArguments(draw_arguments, sizeof(draw_arguments)));
      }

      WMTClearColor sanitized_color = {Color[0], Color[1], Color[2], Color[3]};
      SanitizeRTVClearColor(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View), sanitized_color);
      const std::array<float, 4> clear_color = {
          float(sanitized_color.r), float(sanitized_color.g), float(sanitized_color.b), float(sanitized_color.a)
      };
      auto swizzle_resource = device_->LookupStandardSwizzleTexture(AttachmentDesc.Texture);
      RecordStandardSwizzleUse(swizzle_resource.ptr(), false);
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, AttachmentDesc.DepthPlane);
      for (UINT i = 0; i < clear_rect_count; i++) {
        const auto &rect = clear_rects[i];
        const LONG rect_offset_x = std::clamp(rect.left, LONG(0), LONG(AttachmentDesc.Width));
        const LONG rect_offset_y = std::clamp(rect.top, LONG(0), LONG(AttachmentDesc.Height));
        const LONG rect_width = std::clamp(rect.right, LONG(0), LONG(AttachmentDesc.Width)) - rect_offset_x;
        const LONG rect_height = std::clamp(rect.bottom, LONG(0), LONG(AttachmentDesc.Height)) - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        {
                  if (FAILED(recording_error_)) return;
                  const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(predication_enabled_ ? indirect_offsets[i] : 0)
                      : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
                  allocator_->clear_rtv_.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, AttachmentDesc.RenderTargetArrayLength,
                  clear_color, arguments.buffer, arguments.offset);
                };
      }
      allocator_->clear_rtv_.end();
      RecordStandardSwizzleUse(swizzle_resource.ptr(), true);
      return;
    }

    allocator_->EndSchedulingPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = 0;
    encoder_info->color = {Color[0], Color[1], Color[2], Color[3]};
    SanitizeRTVClearColor(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View), encoder_info->color);
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = AttachmentDesc.DepthPlane;

    allocator_->EndSchedulingPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const UINT Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if ((type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
        !Values || !QueryObjectOwnedByThisDevice(pResource) || (RectCount && !pRects)) {
      RecordError();
      return;
    }
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    if (Descriptor.type == ShaderVisibleDescriptorType::SamplerFeedback) {
      // A feedback map clears to "no mip requested" (all cells 0); the values and rects are ignored.
      ClearSamplerFeedbackMap(static_cast<MTLD3D12Resource *>(pResource), Descriptor.SamplerFeedback);
      return;
    }
    auto color = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    // Rectangles are relative to the UAV, not its backing buffer or aligned native texture-buffer view.
    uint32_t buffer_base_x = 0;
    uint32_t dispatch_depth = 1;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer:
      buffer_base_x = Descriptor.UAVBuffer.slice.byteOffset >> 2;
      full_rect = {0, 0, LONG(Descriptor.UAVBuffer.slice.byteLength >> 2), 1};
      break;
    case ShaderVisibleDescriptorType::UAVTexture:
      full_rect = {
          0, 0, LONG(Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view)),
          LONG(Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view))
      };
      switch (Descriptor.UAVTexture.texture->textureType(Descriptor.UAVTexture.view)) {
      case WMTTextureType1DArray:
      case WMTTextureType2DArray:
        dispatch_depth = Descriptor.UAVTexture.texture->arrayLength(Descriptor.UAVTexture.view);
        break;
      case WMTTextureType3D:
        dispatch_depth = Descriptor.UAVTexture.w_size;
        break;
      default:
        break;
      }
      if (Descriptor.UAVTexture.texture->reservedVolumeLayoutAddress)
        dispatch_depth = Descriptor.UAVTexture.w_size;
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      buffer_base_x = Descriptor.UAVTexelBuffer.slice.firstElement;
      full_rect = {0, 0, LONG(Descriptor.UAVTexelBuffer.slice.elementCount), 1};
      break;
    default:
      return;
    }

    const D3D12_RECT *rects = RectCount ? pRects : &full_rect;
    const UINT rect_count = RectCount ? RectCount : 1;
    small_vector<uint64_t, 4> indirect_offsets;
    if (predication_enabled_) {
      indirect_offsets.reserve(rect_count);
      for (UINT i = 0; i < rect_count; i++) {
        const auto rect = ClipUAVClearRect(rects[i], full_rect);
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
          indirect_offsets.push_back(0);
          continue;
        }
        const uint32_t dispatch_arguments[] = {
            (uint32_t(rect.right - rect.left) + 31) / 32, uint32_t(rect.bottom - rect.top), dispatch_depth
        };
        indirect_offsets.push_back(PreparePredicatedArguments(dispatch_arguments, sizeof(dispatch_arguments)));
      }
    }

    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexture)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pResource), false, Descriptor.UAVTexture.texture->isWritableMSAAView(Descriptor.UAVTexture.view));
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      break;
    case ShaderVisibleDescriptorType::UAVTexture:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view,
                                   Descriptor.UAVTexture.first_w_slice, Descriptor.UAVTexture.w_size);
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      break;
    default:
      return;
    }

    for (UINT i = 0; i < rect_count; i++) {
      const auto rect = ClipUAVClearRect(rects[i], full_rect);
      if (rect.right <= rect.left || rect.bottom <= rect.top)
        continue;
      {
                if (FAILED(recording_error_)) return;
                const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(predication_enabled_ ? indirect_offsets[i] : 0)
                    : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
                allocator_->clear_uav_.clear(buffer_base_x + uint32_t(rect.left), rect.top, rect.right - rect.left, rect.bottom - rect.top, arguments.buffer, arguments.offset);
              };
    }
    allocator_->clear_uav_.end();
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexture)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pResource), true, Descriptor.UAVTexture.texture->isWritableMSAAView(Descriptor.UAVTexture.view));
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const float Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if ((type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
        !Values || !QueryObjectOwnedByThisDevice(pResource) || (RectCount && !pRects)) {
      RecordError();
      return;
    }
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    // Rectangles are relative to the UAV, not its backing buffer or aligned native texture-buffer view.
    uint32_t buffer_base_x = 0;
    uint32_t dispatch_depth = 1;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer:
      buffer_base_x = Descriptor.UAVBuffer.slice.byteOffset >> 2;
      full_rect = {0, 0, LONG(Descriptor.UAVBuffer.slice.byteLength >> 2), 1};
      break;
    case ShaderVisibleDescriptorType::UAVTexture:
      full_rect = {
          0, 0, LONG(Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view)),
          LONG(Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view))
      };
      switch (Descriptor.UAVTexture.texture->textureType(Descriptor.UAVTexture.view)) {
      case WMTTextureType1DArray:
      case WMTTextureType2DArray:
        dispatch_depth = Descriptor.UAVTexture.texture->arrayLength(Descriptor.UAVTexture.view);
        break;
      case WMTTextureType3D:
        dispatch_depth = Descriptor.UAVTexture.w_size;
        break;
      default:
        break;
      }
      if (Descriptor.UAVTexture.texture->reservedVolumeLayoutAddress)
        dispatch_depth = Descriptor.UAVTexture.w_size;
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      buffer_base_x = Descriptor.UAVTexelBuffer.slice.firstElement;
      full_rect = {0, 0, LONG(Descriptor.UAVTexelBuffer.slice.elementCount), 1};
      break;
    default:
      return;
    }

    const D3D12_RECT *rects = RectCount ? pRects : &full_rect;
    const UINT rect_count = RectCount ? RectCount : 1;
    small_vector<uint64_t, 4> indirect_offsets;
    if (predication_enabled_) {
      indirect_offsets.reserve(rect_count);
      for (UINT i = 0; i < rect_count; i++) {
        const auto rect = ClipUAVClearRect(rects[i], full_rect);
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
          indirect_offsets.push_back(0);
          continue;
        }
        const uint32_t dispatch_arguments[] = {
            (uint32_t(rect.right - rect.left) + 31) / 32, uint32_t(rect.bottom - rect.top), dispatch_depth
        };
        indirect_offsets.push_back(PreparePredicatedArguments(dispatch_arguments, sizeof(dispatch_arguments)));
      }
    }

    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexture)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pResource), false, Descriptor.UAVTexture.texture->isWritableMSAAView(Descriptor.UAVTexture.view));
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      break;
    case ShaderVisibleDescriptorType::UAVTexture:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view,
                                   Descriptor.UAVTexture.first_w_slice, Descriptor.UAVTexture.w_size);
      break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      break;
    default:
      return;
    }

    for (UINT i = 0; i < rect_count; i++) {
      const auto rect = ClipUAVClearRect(rects[i], full_rect);
      if (rect.right <= rect.left || rect.bottom <= rect.top)
        continue;
      {
                if (FAILED(recording_error_)) return;
                const auto arguments = predication_enabled_ ? allocator_->ResolveGPUHeap(predication_enabled_ ? indirect_offsets[i] : 0)
                    : MTLD3D12CommandAllocatorImpl::GPUHeapAllocation{};
                allocator_->clear_uav_.clear(buffer_base_x + uint32_t(rect.left), rect.top, rect.right - rect.left, rect.bottom - rect.top, arguments.buffer, arguments.offset);
              };
    }
    allocator_->clear_uav_.end();
    if (Descriptor.type == ShaderVisibleDescriptorType::UAVTexture)
      RecordStandardSwizzleUse(static_cast<MTLD3D12Resource *>(pResource), true, Descriptor.UAVTexture.texture->isWritableMSAAView(Descriptor.UAVTexture.view));
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    // do nothing for now
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  static bool IsSOStatisticsQuery(D3D12_QUERY_TYPE Type) {
    return Type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && Type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3;
  }

  /* Stream n's counter pair in SOStreamCounterBuffer(). */
  static uint64_t SOStreamCounterOffset(D3D12_QUERY_TYPE Type) {
    return uint64_t(Type - D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
  }

  void BeginSOStatisticsQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || !QueryObjectOwnedByThisDevice(pHeap)) {
      RecordError();
      return;
    }
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (heap->desc.Type != D3D12_QUERY_HEAP_TYPE_SO_STATISTICS || Index >= heap->desc.Count ||
        !heap->so_statistics || !heap->so_statistics_end_pipeline) {
      RecordError();
      return;
    }
    for (const auto &active : active_so_queries_) {
      if (active.heap_id == heap->owned_id && active.index == Index) {
        RecordError();
        return;
      }
    }
    auto [counters, counters_offset] = SOStreamCounterBuffer();
    if (!counters)
      return;
    allocator_->InvalidateCurrentPass();
    auto begin = allocator_->AllocatePass<QueryEncoderData>();
    begin->type = EncoderType::Query;
    begin->op = QueryEncoderOp::BeginSOStatistics;
    begin->source = heap->so_statistics;
    begin->counters = counters;
    begin->counters_offset = counters_offset + SOStreamCounterOffset(Type);
    begin->snapshot_offset = (uint64_t(heap->desc.Count) + Index) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    begin->start_index = Index;
    begin->query_count = 1;
    allocator_->InvalidateCurrentPass();
    active_so_queries_.push_back(ActiveSOStatisticsQuery{Type, Index, heap->owned_id});
    owned_graphics::Trace("query_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type), ",\"index\":", Index,
                          ",\"active\":", active_so_queries_.size());
  }

  void EndSOStatisticsQuery(MTLD3D12QueryHeap *heap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || heap->desc.Type != D3D12_QUERY_HEAP_TYPE_SO_STATISTICS ||
        Index >= heap->desc.Count) {
      RecordError();
      return;
    }
    for (auto it = active_so_queries_.begin(); it != active_so_queries_.end(); ++it) {
      if (it->heap_id != heap->owned_id || it->index != Index || it->type != Type)
        continue;
      auto [counters, counters_offset] = SOStreamCounterBuffer();
      if (!counters)
        return;
      allocator_->InvalidateCurrentPass();
      auto end = allocator_->AllocatePass<QueryEncoderData>();
      end->type = EncoderType::Query;
      end->op = QueryEncoderOp::EndSOStatistics;
      end->source = heap->so_statistics;
      end->compute_pipeline = heap->so_statistics_end_pipeline;
      end->counters = counters;
      end->counters_offset = counters_offset + SOStreamCounterOffset(Type);
      end->snapshot_offset = (uint64_t(heap->desc.Count) + Index) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
      end->start_index = Index;
      end->query_count = 1;
      allocator_->InvalidateCurrentPass();
      active_so_queries_.erase(it);
      owned_graphics::Trace("query_end", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type), ",\"index\":", Index,
                            ",\"active\":", active_so_queries_.size());
      return;
    }
    RecordError();
  }

  /* PIPELINE_STATISTICS queries live in PIPELINE_STATISTICS heaps, PIPELINE_STATISTICS1 queries in ..._1 heaps. */
  static bool PipelineStatisticsHeapMatches(const MTLD3D12QueryHeap *heap, D3D12_QUERY_TYPE type) {
    return (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS &&
            heap->desc.Type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS) ||
           (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1 &&
            heap->desc.Type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1);
  }

  void BeginPipelineStatisticsQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || !QueryObjectOwnedByThisDevice(pHeap)) {
      RecordError();
      return;
    }
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (!PipelineStatisticsHeapMatches(heap, Type) || Index >= heap->desc.Count ||
        !heap->pipeline_statistics || !heap->pipeline_statistics_end_pipeline) {
      RecordError();
      return;
    }
    for (const auto &active : active_pipeline_statistics_) {
      if (active.heap_id == heap->owned_id && active.index == Index) {
        RecordError();
        return;
      }
    }
    auto [counters, counters_offset] = PipelineStatisticsCounterBuffer();
    if (!counters)
      return;
    allocator_->InvalidateCurrentPass();
    auto begin = allocator_->AllocatePass<QueryEncoderData>();
    begin->type = EncoderType::Query;
    begin->op = QueryEncoderOp::BeginPipelineStatistics;
    begin->source = heap->pipeline_statistics;
    begin->counters = counters;
    begin->counters_offset = counters_offset;
    begin->record_bytes = heap->pipeline_statistics_record;
    begin->snapshot_offset = (uint64_t(heap->desc.Count) + Index) * heap->pipeline_statistics_record;
    begin->query_count = 1;
    allocator_->InvalidateCurrentPass();
    active_pipeline_statistics_.push_back(ActiveSOStatisticsQuery{Type, Index, heap->owned_id});
  }

  void EndPipelineStatisticsQuery(MTLD3D12QueryHeap *heap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || !PipelineStatisticsHeapMatches(heap, Type) ||
        Index >= heap->desc.Count || !heap->pipeline_statistics || !heap->pipeline_statistics_end_pipeline) {
      RecordError();
      return;
    }
    for (auto it = active_pipeline_statistics_.begin(); it != active_pipeline_statistics_.end(); ++it) {
      if (it->heap_id != heap->owned_id || it->index != Index)
        continue;
      auto [counters, counters_offset] = PipelineStatisticsCounterBuffer();
      if (!counters)
        return;
      allocator_->InvalidateCurrentPass();
      auto end = allocator_->AllocatePass<QueryEncoderData>();
      end->type = EncoderType::Query;
      end->op = QueryEncoderOp::EndPipelineStatistics;
      end->source = heap->pipeline_statistics;
      end->compute_pipeline = heap->pipeline_statistics_end_pipeline;
      end->counters = counters;
      end->counters_offset = counters_offset;
      end->record_bytes = heap->pipeline_statistics_record;
      end->snapshot_offset = (uint64_t(heap->desc.Count) + Index) * heap->pipeline_statistics_record;
      end->start_index = Index;
      allocator_->InvalidateCurrentPass();
      active_pipeline_statistics_.erase(it);
      return;
    }
    RecordError();
  }

  void STDMETHODCALLTYPE
  BeginQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (!IsRecording())
      return;
    if (Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) {
      BeginPipelineStatisticsQuery(pHeap, Type, Index);
      return;
    }
    if (IsSOStatisticsQuery(Type)) {
      BeginSOStatisticsQuery(pHeap, Type, Index);
      return;
    }
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        (Type != D3D12_QUERY_TYPE_OCCLUSION && Type != D3D12_QUERY_TYPE_BINARY_OCCLUSION) ||
        !QueryObjectOwnedByThisDevice(pHeap)) {
      RecordError();
      return;
    }
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (heap->desc.Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION || Index >= heap->desc.Count ||
        !heap->visibility_results || !heap->visibility_accumulate_pipeline) {
      RecordError();
      return;
    }
    for (const auto &active : active_visibility_queries_) {
      if (active.heap_id == heap->owned_id && active.index == Index) {
        RecordError();
        return;
      }
    }

    if (CanRetainVisibilityIntervals() && allocator_->encoder_current &&
        allocator_->encoder_current->type == EncoderType::Render &&
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->visibility_intervals) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      try {
        render->visibility_targets.push_back({heap->visibility_results, {}, heap->owned_id, Index, 0, true});
      } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
      render->visibility_interval_dirty = true;
      render->submission_group_safe = false;
    } else {
      allocator_->InvalidateCurrentPass();
      auto reset = allocator_->AllocatePass<QueryEncoderData>();
      reset->type = EncoderType::Query;
      reset->op = QueryEncoderOp::ResetOcclusion;
      reset->source = heap->visibility_results;
      reset->start_index = Index;
      reset->query_count = 1;
      allocator_->InvalidateCurrentPass();
    }
    active_visibility_queries_.push_back(ActiveVisibilityQuery{
        Type, Index, heap->owned_id, heap->visibility_results, heap->visibility_accumulate_pipeline});
    owned_graphics::Trace("query_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type), ",\"index\":", Index,
                          ",\"active\":", active_visibility_queries_.size());
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  EndQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (!IsRecording())
      return;
    if (!QueryObjectOwnedByThisDevice(pHeap)) {
      RecordError();
      return;
    }
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) {
      EndPipelineStatisticsQuery(heap, Type, Index);
      return;
    }
    if (IsSOStatisticsQuery(Type)) {
      EndSOStatisticsQuery(heap, Type, Index);
      return;
    }
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      if ((type_ == D3D12_COMMAND_LIST_TYPE_COPY
               ? heap->desc.Type != D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP
               : (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
                     heap->desc.Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP) || Index >= heap->desc.Count ||
          !heap->timestamp_samples || !heap->timestamp_dummy) {
        RecordError();
        return;
      }
      allocator_->InvalidateCurrentPass();
      auto sample = allocator_->AllocatePass<QueryEncoderData>();
      sample->type = EncoderType::Query;
      sample->op = QueryEncoderOp::SampleTimestamp;
      sample->timestamp_samples = heap->timestamp_samples;
      sample->timestamp_dummy = heap->timestamp_dummy;
      sample->start_index = Index;
      sample->query_count = 1;
      allocator_->InvalidateCurrentPass();
      owned_graphics::Trace("query_timestamp", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"heap\":", heap->owned_id, ",\"index\":", Index);
      return;
    }
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        (Type != D3D12_QUERY_TYPE_OCCLUSION && Type != D3D12_QUERY_TYPE_BINARY_OCCLUSION) ||
        heap->desc.Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION || Index >= heap->desc.Count) {
      RecordError();
      return;
    }
    for (auto it = active_visibility_queries_.begin(); it != active_visibility_queries_.end(); ++it) {
      if (it->heap_id != heap->owned_id || it->index != Index || it->type != Type)
        continue;
      if (CanRetainVisibilityIntervals() && allocator_->encoder_current &&
          allocator_->encoder_current->type == EncoderType::Render &&
          static_cast<RenderEncoderData *>(allocator_->encoder_current)->visibility_intervals)
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->visibility_interval_dirty = true;
      else allocator_->InvalidateCurrentPass();
      active_visibility_queries_.erase(it);
      owned_graphics::Trace("query_end", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type), ",\"index\":", Index,
                            ",\"active\":", active_visibility_queries_.size());
      return;
    }
    RecordError();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  ResolveQueryData(
      ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT QueryCount, ID3D12Resource *pDstBuffer,
      UINT64 AlignedDstBufferOffset
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (!IsRecording())
      return;
    if (!QueryObjectOwnedByThisDevice(pHeap) || !QueryObjectOwnedByThisDevice(pDstBuffer)) {
      RecordError();
      return;
    }
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    const bool timestamp = Type == D3D12_QUERY_TYPE_TIMESTAMP;
    const bool occlusion = Type == D3D12_QUERY_TYPE_OCCLUSION || Type == D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    const bool so_statistics = IsSOStatisticsQuery(Type);
    const bool pipeline_statistics =
        Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS || Type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1;
    if (StartIndex > heap->desc.Count || QueryCount > heap->desc.Count - StartIndex ||
        (AlignedDstBufferOffset & 7) ||
        (pipeline_statistics && (!PipelineStatisticsHeapMatches(heap, Type) ||
                                 type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || !heap->pipeline_statistics)) ||
        (so_statistics && (heap->desc.Type != D3D12_QUERY_HEAP_TYPE_SO_STATISTICS ||
                           type_ != D3D12_COMMAND_LIST_TYPE_DIRECT)) ||
        (timestamp && (type_ == D3D12_COMMAND_LIST_TYPE_COPY
                           ? heap->desc.Type != D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP
                           : (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
                                 heap->desc.Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP)) ||
        (occlusion && (heap->desc.Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION ||
                       type_ != D3D12_COMMAND_LIST_TYPE_DIRECT)) ||
        (!timestamp && !occlusion && !so_statistics && !pipeline_statistics)) {
      RecordError();
      return;
    }
    auto destination = static_cast<MTLD3D12Resource *>(pDstBuffer);
    auto desc = destination->GetDesc();
    const UINT64 result_size = pipeline_statistics ? heap->pipeline_statistics_record
                                : so_statistics     ? sizeof(D3D12_QUERY_DATA_SO_STATISTICS)
                                                    : sizeof(uint64_t);
    const UINT64 bytes = UINT64(QueryCount) * result_size;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !destination->buffer ||
        !destination->buffer->current() || AlignedDstBufferOffset > desc.Width ||
        bytes > desc.Width - AlignedDstBufferOffset) {
      RecordError();
      return;
    }
    if (!QueryCount)
      return;
    // Resolving an unfinished query is undefined in D3D12. Reject a known
    // active range rather than reading an incomplete GPU result.
    for (const auto &active : active_visibility_queries_) {
      if (active.heap_id == heap->owned_id && active.index >= StartIndex &&
          active.index - StartIndex < QueryCount) {
        RecordError();
        return;
      }
    }
    for (const auto &active : active_so_queries_) {
      if (active.heap_id == heap->owned_id && active.index >= StartIndex &&
          active.index - StartIndex < QueryCount) {
        RecordError();
        return;
      }
    }
    for (const auto &active : active_pipeline_statistics_) {
      if (active.heap_id == heap->owned_id && active.index >= StartIndex &&
          active.index - StartIndex < QueryCount) {
        RecordError();
        return;
      }
    }
    allocator_->InvalidateCurrentPass();
    auto resolve = allocator_->AllocatePass<QueryEncoderData>();
    resolve->type = EncoderType::Query;
    resolve->op = pipeline_statistics ? QueryEncoderOp::ResolvePipelineStatistics
                : so_statistics ? QueryEncoderOp::ResolveSOStatistics
                : timestamp ? QueryEncoderOp::ResolveTimestamp
                : Type == D3D12_QUERY_TYPE_BINARY_OCCLUSION ? QueryEncoderOp::ResolveBinaryOcclusion
                                                              : QueryEncoderOp::ResolveOcclusion;
    resolve->timestamp_samples = heap->timestamp_samples;
    resolve->source = pipeline_statistics ? heap->pipeline_statistics
                    : so_statistics       ? heap->so_statistics
                                          : heap->visibility_results;
    resolve->destination = destination->buffer->current()->buffer();
    resolve->destination_allocation = destination->buffer->current();
    resolve->compute_pipeline = heap->binary_normalize_pipeline;
    resolve->start_index = StartIndex;
    resolve->query_count = QueryCount;
    resolve->destination_offset = AlignedDstBufferOffset;
    resolve->record_bytes = pipeline_statistics ? heap->pipeline_statistics_record
        : so_statistics ? sizeof(D3D12_QUERY_DATA_SO_STATISTICS) : 0;
    allocator_->InvalidateCurrentPass();
    owned_graphics::Trace("query_resolve", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type),
                          ",\"start\":", StartIndex, ",\"count\":", QueryCount,
                          ",\"dst\":", destination->owned_id, ",\"offset\":", AlignedDstBufferOffset);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Op) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (!pBuffer) {
      predication_enabled_ = false;
      return;
    }
    if ((type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         type_ != D3D12_COMMAND_LIST_TYPE_COPY) ||
        !QueryObjectOwnedByThisDevice(pBuffer) || (AlignedBufferOffset & 7) ||
        (Op != D3D12_PREDICATION_OP_EQUAL_ZERO && Op != D3D12_PREDICATION_OP_NOT_EQUAL_ZERO)) {
      RecordError();
      return;
    }
    const auto desc = pBuffer->GetDesc();
    auto resource = static_cast<MTLD3D12Resource *>(pBuffer);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !RangeFits(AlignedBufferOffset, sizeof(uint64_t), desc.Width) || !resource->buffer ||
        !EnsurePredicationPipelines()) {
      RecordError();
      return;
    }

    if (!allocator_->ReserveGPUHeap(sizeof(uint64_t), alignof(uint64_t))) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    auto [snapshot, snapshot_offset] = allocator_->AllocateGPUHeap(sizeof(uint64_t), alignof(uint64_t));
    memset(snapshot, 0, sizeof(uint64_t));
    BeginPredicationComputePass(predication_snapshot_pipeline_);
    BindPredicationBuffer(resource->buffer->current()->buffer(), AlignedBufferOffset, 0);
    BindGPUHeapPredicationBuffer(snapshot_offset, 1);
    DispatchPredicationKernel();
    predication_snapshot_offset_ = snapshot_offset;
    predication_op_ = Op;
    predication_enabled_ = true;
    owned_graphics::Trace("predication", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"resource\":", resource->owned_id, ",\"offset\":", AlignedBufferOffset,
                          ",\"op\":", uint32_t(Op));
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void RecordAnnotation(DiagnosticAnnotation annotation) {
    if (!IsRecording()) { RecordError(E_FAIL); return; }
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      AppendBundleCommand([annotation = std::move(annotation)](auto &parent) { parent.RecordAnnotation(annotation); });
      return;
    }
    allocator_->InvalidateCurrentPass();
    auto *pass = allocator_->AllocatePass<DiagnosticEncoderData>();
    pass->type = EncoderType::Diagnostic; pass->annotation = std::move(annotation);
    allocator_->InvalidateCurrentPass();
  }
  void Annotation(UINT action, UINT metadata, const void *data, UINT size) {
    try { RecordAnnotation(CaptureAnnotation(action, metadata, data, size)); }
    catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); }
    catch (const std::invalid_argument &) { RecordError(); }
    owned_graphics::Trace(action == 0 ? "marker" : action == 1 ? "event_begin" : "event_end",
        ",\"list\":", owned_id, ",\"recording\":", owned_recording, ",\"metadata\":", metadata, ",\"bytes\":", size);
  }
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) try {
    if (FAILED(recording_error_)) return;
    Annotation(0, metadata, data, size);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) try {
    if (FAILED(recording_error_)) return;
    Annotation(1, metadata, data, size);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }
  void STDMETHODCALLTYPE EndEvent() try {
    if (FAILED(recording_error_)) return;
    Annotation(2, 0, nullptr, 0);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void RecordStreamOutputIndirect(MTLD3D12CommandSignature *sig, UINT max_count, uint64_t arguments,
                                  uint64_t count_buffer, WMTPrimitiveType primitive_type) {
    const bool indexed = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    const bool msaa = RequiresMSAAExecution();
    if ((indexed && !sig->UpdateIndexBuffer && !index_buffer) ||
        (sig->UpdateRootArguments && !rootsig_graphics_)) {
      RecordError();
      return;
    }

    // This pass owns one SO ExecuteIndirect. Its GPU sizing pass must see every
    // prior producer, and its assembly must finish before a later draw/query.
    allocator_->InvalidateCurrentPass();
    if (!PrepareStreamOutputDraw(0, 0, indexed, 0, 0, true))
      return;
    const bool statistics = !active_pipeline_statistics_.empty();
    WMT::RenderPipelineState draw_pipeline{};
    if (!msaa) {
      draw_pipeline = statistics ? pso_graphics_->GetPipelineStatisticsPipelineState(true)
                                 : pso_graphics_->GetIndirectCommandBufferPipelineState();
      if (!draw_pipeline) {
        RecordError(E_FAIL);
        return;
      }
    }
    const uint32_t slots = pso_graphics_->slot_mask | sig->VertexBufferMask;
    const uint64_t vertex_bytes = slots ? uint64_t(16) * (32 - __builtin_clz(slots)) : 0;
    const uint64_t root_bytes = rootsig_graphics_ ? uint64_t(rootsig_graphics_->UploadQwords) * 8 : 0;
    const uint64_t sampler_bytes = rootsig_graphics_ ? uint64_t(rootsig_graphics_->NumStaticSamplers) * 64 : 0;
    if (!allocator_->ReserveGPUHeap(vertex_bytes + root_bytes + sampler_bytes + 128, 64)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    std::unique_ptr<SOIndirectEncoderData> indirect;
    try {
      indirect = std::make_unique<SOIndirectEncoderData>();
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    if (PreDraw(true) == DrawCallStatus::Invalid)
      return;
    indirect->draw_template = std::move(so_pending_);
    indirect->assembly_template = indirect->draw_template->params_cpu;
    indirect->preflight_pipeline = so_indirect_preflight_pipeline_;
    indirect->resolver_pipeline = sig->render_resolver;
    indirect->argument_resolver = sig->graphics_argument_resolver;
    indirect->msaa = msaa;
    indirect->shader_view_locations = !msaa && pso_graphics_->view_location_offsets;
    if (indirect->shader_view_locations)
      StageIndirectViewBounds();
    indirect->draw_pipeline = std::move(draw_pipeline);
    indirect->strip_cut = ib_strip_cut_value_;
    auto &command = indirect->command;
    command.max_count = max_count;
    command.max_count_buffer = count_buffer;
    command.argument_buffer = arguments;
    command.predicate = predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    command.predicate_op = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    command.primitive_type = primitive_type;
    command.index_buffer = index_buffer_address;
    command.index_buffer_format = index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    command.index_pull_mode = ~0u;
    auto &vertex_constants = indirect->shader_view_locations
        ? statistics ? pso_graphics_->indirect_view_pipeline_statistics_vertex_constants
                     : pso_graphics_->indirect_view_vertex_constants
        : statistics ? pso_graphics_->pipeline_statistics_vertex_constants : pso_graphics_->vertex_constants;
    command.vertex_constants = vertex_constants.gpu_address;
    if (vertex_constants.buffer) {
      auto &retained = static_cast<RenderEncoderData *>(allocator_->encoder_current)->immutable_constant_buffers;
      if (std::none_of(retained.begin(), retained.end(), [&](const auto &buffer) {
            return buffer.handle == vertex_constants.buffer.handle;
          }))
        retained.push_back(vertex_constants.buffer);
    }
    command.fragment_constants = (statistics ? pso_graphics_->pipeline_statistics_fragment_constants
                                            : pso_graphics_->fragment_constants).gpu_address;
    command.pipeline_statistics = statistics ? allocator_->GPUHeapAddress(pipeline_statistics_counters_offset_) : 0;
    auto &preflight = indirect->preflight;
    preflight.argument_buffer = arguments;
    preflight.count_buffer = count_buffer;
    preflight.predicate = command.predicate;
    preflight.index_buffer = command.index_buffer;
    preflight.max_count = max_count;
    preflight.stride = sig->ByteStride;
    preflight.draw_offset = sig->DrawArgumentOffset;
    preflight.ib_offset = sig->IndexBufferArgumentOffset;
    preflight.predicate_op = command.predicate_op;
    preflight.indexed = indexed;
    preflight.index_format = command.index_buffer_format;
    // Keep one immutable root/VB template in the recording. Each execution
    // clones it per active command before the GPU resolver applies updates.
    if (rootsig_graphics_) {
      if (root_bytes) {
        const auto offset = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_);
        if (offset == UINT64_MAX)
          return;
        indirect->root_arguments = allocator_->ResolveGPUHeap(offset).memory;
        command.rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
      }
      if (sampler_bytes) {
        const auto offset = EncodeStaticSamplers(rootsig_graphics_.ptr());
        if (offset == UINT64_MAX)
          return;
        command.static_samplers = allocator_->GPUHeapAddress(offset);
        allocator_->BindGPUHeap(indirect->static_sampler_buffer, indirect->static_sampler_offset, offset);
      }
    }
    auto [vb_offset, vb_stride] = PopulateVertexBufferTable(1, sig->VertexBufferMask);
    if (vb_offset == UINT64_MAX)
      return;
    if (vb_stride) {
      indirect->vertex_buffers = allocator_->ResolveGPUHeap(vb_offset).memory;
      command.vertex_argbuf_stride = vb_stride;
    }
    static_cast<RenderEncoderData *>(allocator_->encoder_current)->so_indirect = std::move(indirect);
    // Actual draw dimensions and IB bindings are supplied by the execution-owned
    // GPU preflight, never by stale CPU copies of the argument buffer.
    if (msaa && !RecordMSAADraw(0, 0, 0, 0, 0, indexed, true))
      return;
    ResetIndirectState(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_, false);
    allocator_->InvalidateCurrentPass();
  }

  /*
   * ExecuteIndirect whose captures depend on the draw arguments (MeshCaptureIndirectData): a VS+HS+DS+GS PSO, or a
   * VS+GS PSO with stream output. The recording holds the pipeline state of the passes only: the queue runs the GPU
   * argument preflight, waits for it and then sizes every command's captures from its GPU-written counts, exactly
   * as a direct draw of the same arguments, so no capture write can overflow and no command is dropped.
   */
  void RecordMeshCaptureIndirect(MTLD3D12CommandSignature *sig, UINT MaxCommandCount, uint64_t ArgBufferAddress,
                                 uint64_t CountBufferAddress) {
    const bool geometry = pso_graphics_->mesh_kind == MeshPipelineKind::Geometry;
    const bool domain_only = pso_graphics_->mesh_kind == MeshPipelineKind::Tessellation;
    const bool capture_pixels = (!pso_graphics_->stream_output.active ||
            pso_graphics_->stream_output.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM) &&
            (pso_graphics_->owned_conservative || (pso_graphics_->owned_distance_interpolation || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) || CoarseShadingRateState() != 0);
    const bool indexed = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    if (!indexed && sig->CommandType != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW) {
      RecordError(E_INVALIDARG);
      return;
    }
    uint32_t control_points = 0;
    WMTPrimitiveType primitive;
    to_metal_primitive_type(topology_, primitive, control_points);
    if (geometry == (control_points != 0) || (indexed && !sig->UpdateIndexBuffer && !index_buffer)) {
      RecordError(E_INVALIDARG);
      return;
    }
    const bool index32 = index_type == WMTIndexTypeUInt32;
    const bool strip = topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
                       topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                       (topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ || topology_ == kTriangleFan);
    // Direct draws expand these into the equivalent list (RecordMeshStripCut); the queue does the same per command,
    // from its GPU-written arguments and the bound index buffer, before the pass.
    const bool strip_cut = geometry && indexed && strip &&
                           ((ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF && !index32) ||
                            (ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF && index32));
    const bool strip_expand = geometry && (strip_cut ||
        (sig->UpdateIndexBuffer && strip && ib_strip_cut_value_ != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED) ||
        (topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ || topology_ == kTriangleFan));
    const bool statistics = !active_pipeline_statistics_.empty();
    if (!MaxCommandCount)
      return;
    if (!PrepareStatisticsClaim(MaxCommandCount))
      return;
    if (!EnsurePredicationPipelines() || !EnsureStreamOutputPipelines(true)) {
      RecordError(E_FAIL);
      return;
    }
    std::shared_ptr<MeshCaptureIndirectData> data;
    try {
      data = std::make_shared<MeshCaptureIndirectData>();
      data->domain_only = domain_only;
      if (pso_graphics_->stream_output.active) {
        // One recorded SO draw (chained per captured stream) is the template of every command's capture.
        if (!PrepareGeometryStreamOutput(1, 1))
          return;
        data->so_template = std::move(so_pending_);
        for (auto *draw = data->so_template.get(); draw; draw = draw->next_stream.get())
          data->so_params.push_back(*draw->params_cpu);
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    auto &preflight = data->preflight;
    preflight.argument_buffer = ArgBufferAddress;
    preflight.count_buffer = CountBufferAddress;
    preflight.predicate =
        predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    preflight.index_buffer = index_buffer_address;
    preflight.max_count = MaxCommandCount;
    preflight.stride = sig->ByteStride;
    preflight.draw_offset = sig->DrawArgumentOffset;
    preflight.ib_offset = sig->IndexBufferArgumentOffset;
    preflight.predicate_op = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    preflight.indexed = indexed;
    preflight.index_format = index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    data->preflight_pipeline = so_indirect_preflight_pipeline_;
    data->compact_pipeline = tess_capture_compact_pipeline_;
    data->index_buffer = indexed ? index_buffer : WMT::Buffer{};
    data->index_offset = indexed ? index_offset : 0;
    data->update_index = sig->UpdateIndexBuffer;
    data->strip_cut_value = ib_strip_cut_value_;
    if (predication_enabled_)
      data->predicate_heap = allocator_->ResolveGPUHeap(predication_snapshot_offset_).buffer;
    uint64_t root_address = 0, root_virtual_offset = 0;
    if (sig->UpdateRootArguments) {
      data->root_offset = StageSlotRootArguments(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_,
                                               MaxCommandCount, ArgBufferAddress, CountBufferAddress);
      if (data->root_offset == UINT64_MAX)
        return;
      const auto roots = allocator_->ResolveGPUHeap(data->root_offset);
      root_virtual_offset = data->root_offset;
      root_address = roots.address;
      data->root_heap = roots.buffer;
      data->root_memory = static_cast<const uint8_t *>(roots.memory);
      data->root_offset = roots.offset;
      data->root_stride = uint64_t(rootsig_graphics_->UploadQwords) * 8;
      for (uint32_t parameter = 0; parameter < rootsig_graphics_->ParameterSlots; ++parameter) {
        const auto type = rootsig_graphics_->ParameterTypes[parameter];
        if (type == D3D12_ROOT_PARAMETER_TYPE_CBV || type == D3D12_ROOT_PARAMETER_TYPE_SRV ||
            type == D3D12_ROOT_PARAMETER_TYPE_UAV)
          data->root_view_offsets.push_back(rootsig_graphics_->SlotQwordOffsets[parameter]);
      }
    }
    if (sig->UpdateVertexBuffers) {
      const uint32_t slots = pso_graphics_->slot_mask | sig->VertexBufferMask;
      const uint64_t bytes = uint64_t(16) * (32 - __builtin_clz(slots)) * MaxCommandCount;
      if (!allocator_->ReserveGPUHeap(bytes + sizeof(IndirectRenderCommandData) + 64, 16)) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      auto [offset, stride] = PopulateVertexBufferTable(MaxCommandCount, sig->VertexBufferMask);
      if (offset == UINT64_MAX)
        return;
      const auto vertices = allocator_->ResolveGPUHeap(offset);
      data->vertex_heap = vertices.buffer;
      data->vertex_memory = static_cast<const uint8_t *>(vertices.memory);
      data->vertex_offset = vertices.offset;
      data->vertex_stride = stride;
      data->vertex_mask = slots;
      auto [memory, command_offset] = allocator_->AllocateGPUHeap(sizeof(IndirectRenderCommandData), 16);
      auto *command = new (memory) IndirectRenderCommandData{};
      command->max_count = MaxCommandCount;
      command->argument_buffer = ArgBufferAddress;
      BeginPredicationComputePass(sig->graphics_argument_resolver, 64);
      command->vertex_buffer = allocator_->GPUHeapAddress(offset);
      command->vertex_argbuf_stride = stride;
      command->rootsig_qwords = root_address;
      command->rootsig_qwords_stride = data->root_stride / 8;
      if (root_address)
        allocator_->GPUHeapAddress(root_virtual_offset);
      BindGPUHeapPredicationBuffer(command_offset, 30);
      auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {(uint64_t(MaxCommandCount) + 63) / 64, 1, 1};
      allocator_->InvalidateCurrentPass();
    }
    data->gs_instances = pso_graphics_->gs_instance_count;
    data->indexed = indexed;
    data->geometry = geometry;
    data->statistics_claim = statistics_claim_.buffer;
    data->statistics_claim_offset = statistics_claim_.offset;
    if (geometry) {
      D3D12_PRIMITIVE_TOPOLOGY warp_topology = topology_;
      if (strip_expand) {
        uint32_t vpp;
        switch (topology_) {
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: warp_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; vpp = 2; break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: warp_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; vpp = 3; break;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: warp_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; vpp = 4; break;
        case kTriangleFan: warp_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; vpp = 3; break;
        default: warp_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ; vpp = 6; break;
        }
        data->strip_expand = true;

        data->strip_vertices_per_primitive = vpp;
        data->strip_cut_pipeline = mesh_strip_cut_pipeline_;
        data->max_threadgroups = device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7) ? UINT64_MAX : 1024;
        if (statistics) {
          auto [buffer, offset] = PipelineStatisticsCounterBuffer();
          if (!buffer)
            return;
          data->statistics_buffer = buffer;
          data->statistics_offset = offset;
        }
      }
      const auto [vertex_per_warp, increment] = GeometryWarpShape(warp_topology);
      data->topology = uint32_t(topology_);
      data->vertex_per_warp = vertex_per_warp;
      data->warp_increment = increment;
    } else {
      data->layout = pso_graphics_->tess_capture_layout;
      data->control_points = control_points;
      data->threads_per_patch = pso_graphics_->tess_threads_per_patch;
      // D3D10_SB_PRIMITIVE point 1, line 2, triangle 3: GS input primitives per object threadgroup.
      data->per_group = pso_graphics_->mesh_input_primitive == 1   ? 32
                        : pso_graphics_->mesh_input_primitive == 2 ? 16
                                                                   : 10;
    }
    const auto format = !indexed ? SM50_INDEX_BUFFER_FORMAT_NONE
                        : index32 ? SM50_INDEX_BUFFER_FORMAT_UINT32
                                  : SM50_INDEX_BUFFER_FORMAT_UINT16;
    // GS: one pass. TS+GS: pass 1 (VS+HS / DS capture) and pass 2 (capture fetch / GS) each get their own render
    // pass; the queue appends the per-command draws after the recorded state and compacts between them.
    for (uint8_t pass = 1; pass <= (geometry || domain_only ? 1 : 2); pass++) {
      allocator_->InvalidateCurrentPass();
      uint32_t key = data->strip_expand ? MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_UINT32, false, statistics)
                           : geometry       ? MeshVariantKey(format, strip, statistics)
                           : pass == 1      ? MeshVariantKey(format, false, statistics)
                                            : MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false, statistics) |
                                             kMeshTessellationGeometryPass;
      const bool capture_final = capture_pixels && (geometry || domain_only || pass == 2);
      if (capture_final) key |= kMeshMSAACapture;
      // An expanded strip draws the list variant (RecordMeshStripCut swaps the topology the same way).
      const D3D12_PRIMITIVE_TOPOLOGY strip_topology = topology_;
      if (data->strip_expand)
        topology_ = data->strip_vertices_per_primitive == 2   ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                    : data->strip_vertices_per_primitive == 3 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                    : data->strip_vertices_per_primitive == 4 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ
                                                              : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
      const auto status = PreDraw(false, false, key);
      if (sig->UpdateIndexBuffer && !data->strip_expand && pass == 1) {
        for (uint32_t format_index = 0; format_index < 2; ++format_index) {
          const auto *variant = pso_graphics_->GetMeshPipeline(
              MeshVariantKey(format_index ? SM50_INDEX_BUFFER_FORMAT_UINT32 : SM50_INDEX_BUFFER_FORMAT_UINT16,
                             geometry && strip, statistics) | (capture_final ? kMeshMSAACapture : 0));
          if (!variant) { topology_ = strip_topology; RecordError(E_FAIL); return; }
          auto &saved = data->index_variants[format_index];
          saved.pipeline = variant->pso;
          saved.object_constants = variant->object_constants.buffer;
          saved.object_offset = variant->object_constants.offset;
          saved.mesh_constants = variant->mesh_constants.buffer;
          saved.mesh_offset = variant->mesh_constants.offset;
        }
      }
      topology_ = strip_topology;
      if (status == DrawCallStatus::Invalid)
        return;
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      render->capture_indirect = data;
      render->capture_indirect_pass = pass;
      if (capture_final) {
        const auto *capture = pso_graphics_->GetMeshPipeline(key);
        if (!capture) { RecordError(E_FAIL); return; }
        data->msaa_layout = capture->capture_layout;
        SM50_MSAA_MESH_CONTROL control{};
        if (!RecordMSAADraw(0, 1, 0, 0, 0, false, true, capture, &control)) return;
      }
    }
    allocator_->InvalidateCurrentPass();
    ResetIndirectState(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_, false);
    owned_graphics::Trace("draw_mesh_indirect", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"commands\":", MaxCommandCount, ",\"capture\":1,\"geometry\":", geometry ? 1 : 0);
  }

  /*
   * ExecuteIndirect with a GS/HS/DS pipeline. A compute pass copies each command's draw arguments and derives its
   * object-threadgroup grid (d3dalloy_mesh_indirect_arguments), then every command slot is one indirect mesh draw that
   * reads its own argument copy. Empty/predicated-off/uncounted slots dispatch nothing.
   */
  void RecordMeshIndirect(MTLD3D12CommandSignature *sig, UINT MaxCommandCount, uint64_t ArgBufferAddress,
                          uint64_t CountBufferAddress) {
    if (sig->UpdateRootArguments || sig->UpdateVertexBuffers || sig->UpdateIndexBuffer) {
      RecordMeshCaptureIndirect(sig, MaxCommandCount, ArgBufferAddress, CountBufferAddress);
      return;
    }
    const bool index32 = index_type == WMTIndexTypeUInt32;
    const bool cut_strip =
        sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED &&
        (topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
         topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ) &&
        ((ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF && !index32) ||
         (ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF && index32));
    // GS strips the mesh GS cannot assemble natively take the capture path, which expands them per command.
    if (pso_graphics_->owned_conservative || (pso_graphics_->owned_distance_interpolation || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) ||
        CoarseShadingRateState() != 0 ||
        pso_graphics_->mesh_kind == MeshPipelineKind::TessellationGeometry || pso_graphics_->stream_output.active ||
        (pso_graphics_->mesh_kind == MeshPipelineKind::Geometry &&
         (cut_strip || (topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ || topology_ == kTriangleFan)))) {
      RecordMeshCaptureIndirect(sig, MaxCommandCount, ArgBufferAddress, CountBufferAddress);
      return;
    }
    const bool indexed = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    if (!indexed && sig->CommandType != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW) {
      RecordError(E_INVALIDARG);
      return;
    }
    if (!PrepareStatisticsClaim(MaxCommandCount))
      return;
    uint32_t control_points = 0;
    WMTPrimitiveType primitive;
    to_metal_primitive_type(topology_, primitive, control_points);
    const bool tessellation = pso_graphics_->mesh_kind == MeshPipelineKind::Tessellation;
    if (tessellation != (control_points != 0) || (indexed && !index_buffer)) {
      RecordError(E_INVALIDARG);
      return;
    }
    if (!EnsurePredicationPipelines()) {
      RecordError(E_FAIL);
      return;
    }
    struct Control {
      uint64_t arguments, count, predicate, max_threadgroups;
      uint32_t stride, max_commands, argument_words, predicate_not_equal, per_group, control_points;
    };
    const uint64_t argument_bytes = uint64_t(MaxCommandCount) * 32, dispatch_bytes = uint64_t(MaxCommandCount) * 16;
    if (!allocator_->ReserveGPUHeap(sizeof(Control) + argument_bytes + dispatch_bytes + 64, 32)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    const bool strip = topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP || topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
                       topology_ == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                       (topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ || topology_ == kTriangleFan);
    const auto [vertex_per_warp, increment] = GeometryWarpShape(topology_);
    const uint32_t threads_per_patch = pso_graphics_->tess_threads_per_patch;
    const uint32_t patch_per_group = tessellation ? 32 / threads_per_patch : 0;
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(Control), 16);
    const auto arguments_offset = std::get<1>(allocator_->AllocateGPUHeap(argument_bytes, 32));
    const auto dispatch_offset = std::get<1>(allocator_->AllocateGPUHeap(dispatch_bytes, 16));
    auto *control = new (control_memory) Control{};
    control->arguments = ArgBufferAddress;
    control->count = CountBufferAddress;
    control->predicate = predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    // M-series GPUs (Apple7+) have no object-threadgroup grid limit; others are capped at 1024.
    control->max_threadgroups =
        device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7) ? UINT64_MAX : 1024;
    control->stride = sig->ByteStride;
    control->max_commands = MaxCommandCount;
    control->argument_words = indexed ? 5 : 4;
    control->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    control->per_group = tessellation ? patch_per_group : increment;
    control->control_points = tessellation ? control_points : 0;

    BeginPredicationComputePass(mesh_indirect_pipeline_, 64);
    if (predication_enabled_)
      allocator_->GPUHeapAddress(predication_snapshot_offset_);
    BindGPUHeapPredicationBuffer(control_offset, 0);
    BindGPUHeapPredicationBuffer(arguments_offset, 1);
    BindGPUHeapPredicationBuffer(dispatch_offset, 2);
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {(uint64_t(MaxCommandCount) + 63) / 64, 1, 1};
    allocator_->InvalidateCurrentPass();

    const bool statistics = !active_pipeline_statistics_.empty();
    const auto format = !indexed ? SM50_INDEX_BUFFER_FORMAT_NONE
                        : index_type == WMTIndexTypeUInt32 ? SM50_INDEX_BUFFER_FORMAT_UINT32
                                                           : SM50_INDEX_BUFFER_FORMAT_UINT16;
    if (PreDraw(false, false, MeshVariantKey(format, strip && !tessellation, statistics)) == DrawCallStatus::Invalid)
      return;
    if (indexed) {
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = index_buffer;
      binding.offset = index_offset;
      binding.index = SM50_BINDING_INDEX_INDEX_BUFFER;
    }
    for (UINT i = 0; i < MaxCommandCount; ++i) {
      auto &arguments = allocator_->EncodeRenderCommand<wmtcmd_render_setbufferoffset>();
      arguments.type = WMTRenderCommandSetObjectBufferOffset;
      arguments.offset = arguments_offset + uint64_t(i) * 32;
      arguments.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      BindStatisticsClaim(i);
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups_indirect>();
      draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
      allocator_->BindGPUHeap(draw.indirect_args_buffer, draw.indirect_args_offset, dispatch_offset + uint64_t(i) * 16);
      if (tessellation) {
        draw.object_threadgroup_size = {threads_per_patch, patch_per_group, 1};
        draw.mesh_threadgroup_size = {32, 1, 1};
      } else {
        draw.object_threadgroup_size = {vertex_per_warp, 1, 1};
        draw.mesh_threadgroup_size = {1, 1, 1};
      }
    }
    owned_graphics::Trace("draw_mesh_indirect", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"commands\":", MaxCommandCount, ",\"tessellation\":", tessellation ? 1 : 0);
  }

  /*
   * Root-argument-changing DISPATCH_MESH / DISPATCH_RAYS signatures: MaxCommandCount copies of the current root
   * arguments of `rootsig`, then the signature's resolve_root_arguments kernel writes each executed slot's updates
   * into its own copy (skipped slots keep the current values; they dispatch nothing). Returns the heap offset of
   * slot 0's copy (slot i at + i * UploadQwords * 8), or UINT64_MAX on failure (already recorded).
   */
  uint64_t StageSlotRootArguments(MTLD3D12CommandSignature *sig, MTLD3D12RootSignature *rootsig,
                                  const uint64_t *staging, UINT MaxCommandCount, uint64_t ArgBufferAddress,
                                  uint64_t CountBufferAddress) {
    if (!rootsig || !sig->root_argument_resolver || !rootsig->UploadQwords) {
      RecordError(E_INVALIDARG);
      return UINT64_MAX;
    }
    struct Control {
      uint64_t arguments, count, predicate, roots;
      uint32_t max_commands, roots_stride, predicate_not_equal, padding;
    };
    const uint64_t roots_bytes = uint64_t(rootsig->UploadQwords) * sizeof(uint64_t) * MaxCommandCount;
    if (!allocator_->ReserveGPUHeap(roots_bytes + sizeof(Control) + 128, 64)) {
      RecordError(E_OUTOFMEMORY);
      return UINT64_MAX;
    }
    const uint64_t roots_offset = EncodeRootArgument(rootsig, staging, MaxCommandCount);
    if (roots_offset == UINT64_MAX)
      return UINT64_MAX;
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(Control), 16);
    auto *control = new (control_memory) Control{};
    control->arguments = ArgBufferAddress;
    control->count = CountBufferAddress;
    BeginPredicationComputePass(sig->root_argument_resolver, 64);
    control->predicate =
        predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    control->roots = allocator_->GPUHeapAddress(roots_offset);
    control->max_commands = MaxCommandCount;
    control->roots_stride = rootsig->UploadQwords;
    control->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    BindGPUHeapPredicationBuffer(control_offset, 0);
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {(uint64_t(MaxCommandCount) + 63) / 64, 1, 1};
    allocator_->InvalidateCurrentPass();
    return roots_offset;
  }

  /*
   * ExecuteIndirect with D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH on a mesh-shader pipeline: one thread per slot
   * (d3dalloy_mesh_indirect_arguments, per_group 0) writes the slot's grid, zeroed past the count buffer, under a false
   * predicate or out of the D3D limits; each slot is one indirect object/mesh draw.
   */
  void RecordMeshShaderIndirect(MTLD3D12CommandSignature *sig, UINT MaxCommandCount, uint64_t ArgBufferAddress,
                                uint64_t CountBufferAddress) {
    if (pso_graphics_->mesh_kind != MeshPipelineKind::MeshShader ||
        sig->CommandType != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) {
      RecordError(E_INVALIDARG);
      return;
    }
    const bool statistics = !active_pipeline_statistics_.empty();
    if (!PrepareStatisticsClaim(MaxCommandCount))
      return;
    if (!EnsurePredicationPipelines()) {
      RecordError(E_FAIL);
      return;
    }
    uint64_t roots_offset = UINT64_MAX;
    if (sig->UpdateRootArguments) {
      roots_offset = StageSlotRootArguments(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_, MaxCommandCount,
                                            ArgBufferAddress, CountBufferAddress);
      if (roots_offset == UINT64_MAX)
        return;
    }
    struct Control {
      uint64_t arguments, count, predicate, max_threadgroups;
      uint32_t stride, max_commands, argument_words, predicate_not_equal, per_group, control_points;
    };
    const uint64_t argument_bytes = uint64_t(MaxCommandCount) * 32, dispatch_bytes = uint64_t(MaxCommandCount) * 16;
    if (!allocator_->ReserveGPUHeap(sizeof(Control) + argument_bytes + dispatch_bytes + 64, 32)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(Control), 16);
    const auto arguments_offset = std::get<1>(allocator_->AllocateGPUHeap(argument_bytes, 32));
    const auto dispatch_offset = std::get<1>(allocator_->AllocateGPUHeap(dispatch_bytes, 16));
    auto *control = new (control_memory) Control{};
    control->arguments = ArgBufferAddress + sig->DrawArgumentOffset;
    control->count = CountBufferAddress;
    control->predicate = predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    control->max_threadgroups = UINT64_MAX;
    control->stride = sig->ByteStride;
    control->max_commands = MaxCommandCount;
    control->argument_words = 3;
    control->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    control->per_group = 0;

    BeginPredicationComputePass(mesh_indirect_pipeline_, 64);
    if (predication_enabled_)
      allocator_->GPUHeapAddress(predication_snapshot_offset_);
    BindGPUHeapPredicationBuffer(control_offset, 0);
    BindGPUHeapPredicationBuffer(arguments_offset, 1);
    BindGPUHeapPredicationBuffer(dispatch_offset, 2);
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {(uint64_t(MaxCommandCount) + 63) / 64, 1, 1};
    allocator_->InvalidateCurrentPass();

    const bool capture_pixels = statistics || CoarseShadingRateState() != 0 ||
        (pso_graphics_->owned_mesh_output_replay || pso_graphics_->uses_atomic64 || pso_graphics_->emulated_depth_bounds) || pso_graphics_->owned_conservative;
    const uint32_t capture_key = MeshVariantKey(SM50_INDEX_BUFFER_FORMAT_NONE, false, statistics) |
        (capture_pixels ? kMeshMSAACapture : 0) |
        (capture_pixels && pso_graphics_->as_threads[0] ? kMeshObjectCapture : 0);
    const uint64_t roots_stride = rootsig_graphics_ ? uint64_t(rootsig_graphics_->UploadQwords) * 8 : 0;
    for (UINT i = 0; i < MaxCommandCount; ++i) {
      // Metal 4 indirect mesh draws within one render encoder do not preserve
      // the raster overwrite order of consecutive command slots. An encoder
      // boundary carries the prior mesh/fragment fence and loads its stored
      // attachments before the next GPU-resolved draw, including empty grids.
      allocator_->InvalidateCurrentPass();
      if (PreDraw(false, false, capture_key) == DrawCallStatus::Invalid) return;
      if (roots_offset != UINT64_MAX) {
        for (auto type : {WMTRenderCommandSetObjectBuffer, WMTRenderCommandSetMeshBuffer,
                          WMTRenderCommandSetFragmentBuffer}) {
          auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          binding.type = type;
          allocator_->BindGPUHeap(binding.buffer, binding.offset, roots_offset + uint64_t(i) * roots_stride);
          binding.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        }
      }
      BindStatisticsClaim(i);
      const auto arguments = allocator_->ResolveGPUHeap(dispatch_offset + uint64_t(i) * 16);
      EncodeMeshShaderDispatch(arguments.buffer, arguments.offset, 0, 0, 0);
      if (capture_pixels && !RecordCapturedMeshDraw(capture_key, 1, 1, 1, arguments.buffer, arguments.offset)) return;
    }
    // The application's root arguments are back in effect after the call (with the updated ones zeroed).
    ResetIndirectState(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_, false);
    owned_graphics::Trace("dispatch_mesh_indirect", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"commands\":", MaxCommandCount);
  }

  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgBuffer,
      UINT64 ArgBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset
  ) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording())
      return;
    if (!pCommandSignature || !pArgBuffer || !QueryObjectOwnedByThisDevice(pCommandSignature) ||
        !QueryObjectOwnedByThisDevice(pArgBuffer) || (pCountBuffer && !QueryObjectOwnedByThisDevice(pCountBuffer)) ||
        (ArgBufferOffset & 3) || (CountBufferOffset & 3)) {
      RecordError();
      return;
    }
    auto sig = static_cast<MTLD3D12CommandSignature *>(pCommandSignature);
    const auto arg_desc = pArgBuffer->GetDesc();
    if (arg_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || ArgBufferOffset > arg_desc.Width ||
        uint64_t(MaxCommandCount) > (arg_desc.Width - ArgBufferOffset) / sig->ByteStride) {
      RecordError();
      return;
    }
    if (pCountBuffer) {
      const auto count_desc = pCountBuffer->GetDesc();
      if (count_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !RangeFits(CountBufferOffset, sizeof(UINT), count_desc.Width)) {
        RecordError();
        return;
      }
    }
    const bool compute_command = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH ||
                                 sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    if ((compute_command &&
         type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE) ||
        (!compute_command &&
         type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)) {
      RecordError();
      return;
    }
    if (compute_command && RejectDispatchInRenderPass()) {
      return;
    }
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      if (pCountBuffer || sig->UpdateRootArguments || sig->UpdateVertexBuffers || sig->UpdateIndexBuffer) {
        RecordError();
        return;
      }
      RecordBundle(&MTLD3D12GraphicsCommandListImpl::ExecuteIndirect, pCommandSignature, MaxCommandCount,
                   pArgBuffer, ArgBufferOffset, pCountBuffer, CountBufferOffset);
      return;
    }
    if (!MaxCommandCount) {
      const bool compute = compute_command;
      ResetIndirectState(sig, compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr(),
                         compute ? rootarg_compute_staging_ : rootarg_graphics_staging_, compute);
      return;
    }
    auto arg_buffer = static_cast<MTLD3D12Resource *>(pArgBuffer);
    if (!arg_buffer->buffer) {
      RecordError();
      return;
    }
    auto ArgBufferAddress = arg_buffer->buffer->current()->gpuAddress() + ArgBufferOffset;
    const bool simple_indirect = !pCountBuffer && !sig->UpdateRootArguments && !sig->UpdateVertexBuffers &&
        !sig->UpdateIndexBuffer && !sig->DrawArgumentOffset && !predication_enabled_ &&
        active_pipeline_statistics_.empty() && active_so_queries_.empty();
    uint64_t CountBufferAddress = 0;
    if (auto count_buffer = static_cast<MTLD3D12Resource *>(pCountBuffer)) {
      if (!count_buffer->buffer) {
        RecordError();
        return;
      }
      CountBufferAddress = count_buffer->buffer->current()->gpuAddress() + CountBufferOffset;
    }
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS) {
      RecordDispatchRaysIndirect(sig, MaxCommandCount, ArgBufferAddress, CountBufferAddress);
      return;
    }
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
      // Join a deferred link before reading `pso`; a failed link takes the path below, whose PreDispatch records E_FAIL.
      if (simple_indirect && MaxCommandCount == 1 && pso_compute_ && !pso_compute_->uses_atomic64 &&
          pso_compute_->WaitNativePipeline() && pso_compute_->pso) {
        if (!PreDispatch(false)) return;
        auto *allocation = arg_buffer->buffer->current();
        try {
          if (allocator_->recording_head_->root_allocation_set.insert(allocation).second)
            allocator_->recording_head_->root_allocations.emplace_back(allocation);
          auto *compute = static_cast<ComputeEncoderData *>(allocator_->encoder_current);
          compute->submission_access_complete = false;
        } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
        if (!RecordImplicitUse(arg_buffer, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) return;
        auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
        dispatch.type = WMTComputeCommandDispatchIndirect;
        dispatch.indirect_args_buffer = allocation->buffer();
        dispatch.indirect_args_offset = allocation->currentSuballocationOffset() + ArgBufferOffset;
        ResetIndirectState(sig, rootsig_compute_.ptr(), rootarg_compute_staging_, true);
        owned_graphics::Trace("simple_indirect_record", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
            ",\"path\":\"native_dispatch\",\"commands\":", MaxCommandCount, ",\"stride\":", sig->ByteStride,
            ",\"pso\":", pso_compute_->pso.handle);
        return;
      }
      if (!PreDispatch(sig->UpdateRootArguments))
        return;
      if (sig->UpdateRootArguments && !rootsig_compute_) {
        RecordError();
        return;
      }

      uint64_t required_heap = sizeof(IndirectComputeCommandData);
      if (!sig->UpdateRootArguments && !allocator_->ReserveGPUHeap(required_heap, 16)) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      if (sig->UpdateRootArguments) {
        const uint64_t per_command = uint64_t(rootsig_compute_->UploadQwords) * sizeof(uint64_t);
        if ((per_command && MaxCommandCount > (SIZE_MAX - required_heap) / per_command) ||
            !allocator_->ReserveGPUHeap(required_heap + per_command * MaxCommandCount +
                                                uint64_t(rootsig_compute_->NumStaticSamplers) * 8 * sizeof(uint64_t) + 128,
                                            16)) {
          RecordError(E_OUTOFMEMORY);
          return;
        }
      }
      const bool statistics = !active_pipeline_statistics_.empty();
      auto cmd = allocator_->EncodeIndirectComputeCommand(sig, pso_compute_.ptr(), MaxCommandCount, statistics);
      if (!cmd) {
        RecordError(E_FAIL);
        return;
      }
      cmd->max_count_buffer = CountBufferAddress;
      cmd->argument_buffer = ArgBufferAddress;
      cmd->pipeline_statistics = statistics ? allocator_->GPUHeapAddress(pipeline_statistics_counters_offset_) : 0;
      if (pso_compute_->uses_atomic64) {
        auto *compute = static_cast<ComputeEncoderData *>(allocator_->encoder_current);
        compute->atomic64_indirect.push_back({cmd, sig->ByteStride, sig->DrawArgumentOffset, sig->atomic64_root_fields});
        auto *signature = static_cast<MTLD3D12RootSignature *>(rootsig_compute_.ptr());
        std::erase_if(compute->atomic64_indirect.back().root_fields, [&](const auto &field) {
          const auto *range = signature->GetRootDescriptorRange(field.root_parameter);
          bool used = false;
          if (range) signature->ForEachUsedDescriptorRange({range, 1}, pso_compute_->resource_bindings,
              [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE) { used = true; });
          return !used;
        });
        if (predication_enabled_) {
          auto binding = allocator_->ResolveGPUHeap(predication_snapshot_offset_);
          compute->atomic64_indirect.back().predicate_buffer = binding.buffer;
          compute->atomic64_indirect.back().predicate_offset = binding.offset;
        }
      }
      cmd->predicate = predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
      cmd->predicate_op = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;

      if (sig->UpdateRootArguments) {
        cmd->rootsig_qwords = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_, MaxCommandCount);
        if (cmd->rootsig_qwords == UINT64_MAX)
          return;
        if (rootsig_compute_->UploadQwords)
          cmd->rootsig_qwords = allocator_->GPUHeapAddress(cmd->rootsig_qwords);
        cmd->rootsig_qwords_stride = rootsig_compute_->UploadQwords;
        cmd->static_samplers = EncodeStaticSamplers(rootsig_compute_.ptr());
        if (cmd->static_samplers == UINT64_MAX)
          return;
        if (rootsig_compute_->NumStaticSamplers)
          cmd->static_samplers = allocator_->GPUHeapAddress(cmd->static_samplers);
      }
      ResetIndirectState(sig, rootsig_compute_.ptr(), rootarg_compute_staging_, true);
      if (pso_compute_->uses_atomic64) allocator_->InvalidateCurrentPass();

      return;
    }
    // View instancing: Tier 1 may loop the entire ExecuteIndirect buffer per view. Root-argument changes made by
    // the commands are reset afterwards (ResetIndirectState), so every view starts from the same root state.
    if (ReplayPerView([&] {
          ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgBuffer, ArgBufferOffset, pCountBuffer,
                          CountBufferOffset);
        }))
      return;
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH ||
        (pso_graphics_ && pso_graphics_->mesh_kind == MeshPipelineKind::MeshShader)) {
      // Mesh shaders do not use the IA topology. Root argument updates apply per slot (RecordMeshShaderIndirect).
      if (!pso_graphics_) {
        RecordError(E_INVALIDARG);
        return;
      }
      RecordMeshShaderIndirect(sig, MaxCommandCount, ArgBufferAddress, CountBufferAddress);
      return;
    }
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    if (pso_graphics_ && pso_graphics_->mesh_kind != MeshPipelineKind::None) {
      RecordMeshIndirect(sig, MaxCommandCount, ArgBufferAddress, CountBufferAddress);
      return;
    }
    if (pso_graphics_ && (pso_graphics_->stream_output.active || !active_pipeline_statistics_.empty() ||
                          RequiresMSAAExecution())) {
      RecordStreamOutputIndirect(sig, MaxCommandCount, ArgBufferAddress, CountBufferAddress, primitive_type);
      return;
    }
    const bool strip = primitive_type == WMTPrimitiveTypeLineStrip || primitive_type == WMTPrimitiveTypeTriangleStrip;
    const bool matching_cut = pso_graphics_ &&
        ((index_type == WMTIndexTypeUInt16 && ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
         (index_type == WMTIndexTypeUInt32 && ib_strip_cut_value_ == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF));
    const bool index_pull = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED && strip &&
                            (sig->UpdateIndexBuffer || !matching_cut);
    uint64_t ib_offset = 0;
    auto *ib_allocation = index_buffer_address ? device_->LookupBufferByVA(index_buffer_address, &ib_offset) : nullptr;
    const uint32_t index_width = index_type == WMTIndexTypeUInt32 ? 4 : 2;
    const bool indexed = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    const bool native_index = !indexed || (index_buffer && ib_allocation && !(ib_offset % index_width) &&
        ib_offset <= ib_allocation->byteLength() && !((ib_allocation->byteLength() - ib_offset) % index_width));
    // ponytail: cap CPU/native command expansion; larger batches use the GPU ICB resolver.
    if (simple_indirect && MaxCommandCount <= 64 && pso_graphics_ && !pso_graphics_->uses_atomic64 &&
        !index_pull && native_index &&
        !pso_graphics_->view_instancing.count && !pso_graphics_->view_location_offsets &&
        (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW || indexed)) {
      if (PreDraw(false) != DrawCallStatus::Ordinary) return;
      auto *allocation = arg_buffer->buffer->current();
      try {
        for (auto *retained : {allocation, indexed ? ib_allocation : nullptr})
          if (retained && allocator_->recording_head_->root_allocation_set.insert(retained).second)
            allocator_->recording_head_->root_allocations.emplace_back(retained);
        auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
        if (std::none_of(render->masked_pipeline_states.begin(), render->masked_pipeline_states.end(),
                        [&](const auto &pso) { return pso.handle == direct_pipeline_bound_; }))
          render->masked_pipeline_states.push_back(WMT::RenderPipelineState{direct_pipeline_bound_});
        render->submission_group_safe = false;
      } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); return; }
      if (!RecordImplicitUse(arg_buffer, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) return;
      for (UINT i = 0; i < MaxCommandCount; ++i) {
        const uint64_t offset = allocation->currentSuballocationOffset() + ArgBufferOffset + uint64_t(i) * sig->ByteStride;
        if (indexed) {
          auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
          draw.type = WMTRenderCommandDrawIndexedIndirect;
          draw.primitive_type = primitive_type;
          draw.index_type = index_type;
          draw.index_buffer = index_buffer;
          draw.index_buffer_offset = index_offset;
          draw.indirect_args_buffer = allocation->buffer();
          draw.indirect_args_offset = offset;
        } else {
          auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
          draw.type = WMTRenderCommandDrawIndirect;
          draw.primitive_type = primitive_type;
          draw.indirect_args_buffer = allocation->buffer();
          draw.indirect_args_offset = offset;
        }
      }
      ResetIndirectState(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_, false);
      owned_graphics::Trace("simple_indirect_record", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
          ",\"path\":\"native_draw\",\"indexed\":", indexed, ",\"commands\":", MaxCommandCount,
          ",\"stride\":", sig->ByteStride, ",\"pso\":", direct_pipeline_bound_);
      return;
    }
    bool encode_binding = index_pull || sig->UpdateRootArguments || sig->UpdateIndexBuffer ||
                          sig->UpdateVertexBuffers || (pso_graphics_ && pso_graphics_->view_location_offsets);
    DrawCallStatus status = PreDraw(encode_binding);
    if (status == DrawCallStatus::Invalid)
      return;
    if (sig->UpdateRootArguments && !rootsig_graphics_) {
      RecordError();
      return;
    }
    if (status != DrawCallStatus::Ordinary) {
      RecordError(E_FAIL);
      return;
    }
    if (pso_graphics_->view_location_offsets)
      StageIndirectViewBounds();

    uint64_t required_heap = sizeof(IndirectRenderCommandData) +
                             (index_pull ? uint64_t(MaxCommandCount) * sizeof(SM50_INDEX_PULL_CONTROL) : 0);
    if (!encode_binding && !allocator_->ReserveGPUHeap(required_heap, 16)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    if (encode_binding) {
      uint64_t per_command = 0;
      if (rootsig_graphics_)
        per_command += uint64_t(rootsig_graphics_->UploadQwords) * sizeof(uint64_t);
      const uint32_t slot_mask = pso_graphics_->slot_mask | sig->VertexBufferMask;
      if (slot_mask)
        per_command += align(uint64_t(16) * (32 - __builtin_clz(slot_mask)), uint64_t(16));
      const uint64_t sampler_bytes = rootsig_graphics_
          ? uint64_t(rootsig_graphics_->NumStaticSamplers) * 8 * sizeof(uint64_t)
          : 0;
      if ((per_command && MaxCommandCount > (SIZE_MAX - required_heap - sampler_bytes) / per_command) ||
          !allocator_->ReserveGPUHeap(required_heap + sampler_bytes + per_command * MaxCommandCount + 128, 16)) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    auto cmd = allocator_->EncodeIndirectRenderCommand(sig, pso_graphics_.ptr(), MaxCommandCount, index_pull);
    if (!cmd) {
      RecordError(E_FAIL);
      return;
    }
    cmd->max_count_buffer = CountBufferAddress;
    cmd->argument_buffer = ArgBufferAddress;
    cmd->predicate = predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    cmd->predicate_op = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    cmd->primitive_type = primitive_type;
    cmd->index_buffer = index_buffer_address;
    cmd->index_buffer_format = index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (index_pull) {
      auto [controls, offset] = allocator_->AllocateGPUHeap(
          uint64_t(MaxCommandCount) * sizeof(SM50_INDEX_PULL_CONTROL), 16);
      memset(controls, 0, uint64_t(MaxCommandCount) * sizeof(SM50_INDEX_PULL_CONTROL));
      cmd->index_pull_controls = allocator_->GPUHeapAddress(offset);
      cmd->index_pull_mode = ib_strip_cut_value_;
    }
    if (!encode_binding)
      return;
    if (rootsig_graphics_) {
      cmd->rootsig_qwords = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_, MaxCommandCount);
      if (cmd->rootsig_qwords == UINT64_MAX)
        return;
      if (rootsig_graphics_->UploadQwords)
        cmd->rootsig_qwords = allocator_->GPUHeapAddress(cmd->rootsig_qwords);
      cmd->rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
      cmd->static_samplers = EncodeStaticSamplers(rootsig_graphics_.ptr());
      if (cmd->static_samplers == UINT64_MAX)
        return;
      if (rootsig_graphics_->NumStaticSamplers)
        cmd->static_samplers = allocator_->GPUHeapAddress(cmd->static_samplers);
    }
    auto [VBOffset, VBStride] = PopulateVertexBufferTable(MaxCommandCount, sig->VertexBufferMask);
    if (VBOffset == UINT64_MAX)
      return;
    cmd->vertex_buffer = allocator_->GPUHeapAddress(VBOffset);
    cmd->vertex_argbuf_stride = VBStride;
    ResetIndirectState(sig, rootsig_graphics_.ptr(), rootarg_graphics_staging_, false);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  };

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (!ValidateAtomicCopy(
            pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, sizeof(UINT), Dependencies, ppDependentResources,
            pDependentSubresourceRanges
        ))
      return;
    if (!atomic_copy_u32_pipeline_) {
      constexpr auto source = R"(
#include <metal_stdlib>
using namespace metal;
kernel void d3dalloy_atomic_copy_u32(device atomic_uint *source [[buffer(0)]],
                                 device atomic_uint *destination [[buffer(1)]]) {
  atomic_store_explicit(destination, atomic_load_explicit(source, memory_order_relaxed), memory_order_relaxed);
}
)";
      WMT::Reference<WMT::Error> error;
      auto library = device_->GetMTLDevice().newLibraryWithSource(source, error);
      auto function = library ? WMT4::Library(library).newFunction("d3dalloy_atomic_copy_u32") : WMT::Reference<WMT4::Function>();
      if (function)
        atomic_copy_u32_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!atomic_copy_u32_pipeline_) {
        ERR("AtomicCopyBufferUINT: failed to create atomic copy pipeline");
        RecordError(E_FAIL);
        return;
      }
    }
    // Each typed pass is its own Metal encoder fenced against the previous and
    // next one, so every earlier write in this list and queue (including the
    // dependent payload) completes before the single-thread atomic publish, and
    // later work observes the published element.
    BeginPredicationComputePass(atomic_copy_u32_pipeline_);
    BindPredicationBuffer(static_cast<MTLD3D12Resource *>(pSrcBuffer)->buffer->current()->buffer(), SrcOffset, 0);
    BindPredicationBuffer(static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer(), DstOffset, 1);
    DispatchPredicationKernel();
    owned_graphics::Trace("atomic_copy_u32", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"src\":", static_cast<MTLD3D12Resource *>(pSrcBuffer)->owned_id,
                          ",\"dst\":", static_cast<MTLD3D12Resource *>(pDstBuffer)->owned_id,
                          ",\"dependencies\":", Dependencies);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT64(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (!ValidateAtomicCopy(
            pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, sizeof(UINT64), Dependencies, ppDependentResources,
            pDependentSubresourceRanges
        ))
      return;
    if (!atomic_copy_u64_pipeline_) {
      // Apple8 on macOS provides the two indivisible ulong operations used below.
      if (!device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple8)) {
        RecordError(E_NOTIMPL);
        return;
      }
      constexpr auto source = R"(
#include <metal_stdlib>
using namespace metal;
kernel void d3dalloy_atomic_copy_u64(device const ulong *source [[buffer(0)]],
                                 device atomic_ulong *destination [[buffer(1)]]) {
  const ulong value = source[0];
  atomic_min_explicit(destination, value, memory_order_relaxed);
  atomic_max_explicit(destination, value, memory_order_relaxed);
}
)";
      WMT::Reference<WMT::Error> error;
      auto library = device_->GetMTLDevice().newLibraryWithSource(source, error);
      auto function = library ? WMT4::Library(library).newFunction("d3dalloy_atomic_copy_u64") : WMT::Reference<WMT4::Function>();
      if (function)
        atomic_copy_u64_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!atomic_copy_u64_pipeline_) {
        ERR("AtomicCopyBufferUINT64: failed to create atomic copy pipeline");
        RecordError(E_FAIL);
        return;
      }
    }
    // D3D requires writers of a resource on different queues to be synchronized.
    // With that exclusive publisher, min(old, value) is old or value and the
    // following max produces value: neither operation exposes a third value.
    // This is not a general multi-writer atomic store. The source is stable,
    // and the encoder fence completes prior dependent writes before publishing.
    BeginPredicationComputePass(atomic_copy_u64_pipeline_);
    BindPredicationBuffer(static_cast<MTLD3D12Resource *>(pSrcBuffer)->buffer->current()->buffer(), SrcOffset, 0);
    BindPredicationBuffer(static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer(), DstOffset, 1);
    DispatchPredicationKernel();
    owned_graphics::Trace("atomic_copy_u64", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"src\":", static_cast<MTLD3D12Resource *>(pSrcBuffer)->owned_id,
                          ",\"dst\":", static_cast<MTLD3D12Resource *>(pDstBuffer)->owned_id,
                          ",\"dependencies\":", Dependencies);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  OMSetDepthBounds(FLOAT Min, FLOAT Max) try {
    if (FAILED(recording_error_)) return;
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::OMSetDepthBounds, Min, Max))
      return;
    depth_bounds_min_ = std::isnan(Min) ? 0.0f : Min;
    depth_bounds_max_ = std::isnan(Max) ? 0.0f : Max;
    dirty_state_.set(DirtyState::GraphicsPipelineState);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording()) {
      RecordError(E_FAIL);
      return;
    }
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      RecordError();
      return;
    }
    if (RejectInRenderPass())
      return;
    if (!device_->SupportsCenterSamplePattern()) {
      RecordError(E_NOTIMPL);
      return;
    }
    WMTProgrammableSamplePositions pattern{};
    if (NumSamplesPerPixel || NumPixels || pSamplePositions) {
      if (NumPixels != 1 || (NumSamplesPerPixel != 2 && NumSamplesPerPixel != 4 && NumSamplesPerPixel != 8) ||
          !pSamplePositions || !device_->GetMTLDevice().supportsTextureSampleCount(NumSamplesPerPixel)) {
        RecordError();
        return;
      }
      pattern.count = NumSamplesPerPixel;
      for (UINT i = 0; i < NumSamplesPerPixel; ++i) {
        if (pSamplePositions[i].X < -8 || pSamplePositions[i].X > 7 ||
            pSamplePositions[i].Y < -8 || pSamplePositions[i].Y > 7) {
          RecordError();
          return;
        }
        pattern.positions[i][0] = pSamplePositions[i].X;
        pattern.positions[i][1] = pSamplePositions[i].Y;
      }
    }
    if (std::memcmp(&sample_positions_, &pattern, sizeof(pattern)) == 0)
      return;
    allocator_->InvalidateCurrentPass();
    sample_positions_ = pattern;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  ResolveSubresourceRegion(
      ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource,
      UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode
  ) try {
    if (FAILED(recording_error_)) return;
    ResolveTextureRegion(pDstResource, DstSubresource, DstX, DstY, pSrcResource, SrcSubresource,
                         pSrcRect, Format, ResolveMode, false);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  // Copying an MSAA depth/stencil plane needs the same sample-preserving raster operation as DECOMPRESS.
  void ResolveTextureRegion(
      ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource,
      UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode, bool copy_states
  ) {
    if (RejectBundleOperation())
      return;
    if (RejectInRenderPass())
      return;
    if (ResolveMode == D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK ||
        ResolveMode == D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK) {
      TranscodeSamplerFeedback(ResolveMode == D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK, pDstResource,
                               DstSubresource, DstX, DstY, pSrcResource, SrcSubresource, pSrcRect, Format);
      return;
    }
    if (!QueryObjectOwnedByThisDevice(pDstResource) || !QueryObjectOwnedByThisDevice(pSrcResource) ||
        (ResolveMode != D3D12_RESOLVE_MODE_AVERAGE && ResolveMode != D3D12_RESOLVE_MODE_MIN &&
         ResolveMode != D3D12_RESOLVE_MODE_MAX && ResolveMode != D3D12_RESOLVE_MODE_DECOMPRESS)) {
      RecordError();
      return;
    }

    auto *dst_resource = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *src_resource = static_cast<MTLD3D12Resource *>(pSrcResource);
    const auto dst_resource_desc = dst_resource->GetDesc();
    const auto src_resource_desc = src_resource->GetDesc();
    MTL_DXGI_FORMAT_DESC format_desc;
    MTL_DXGI_FORMAT_DESC source_format{}, destination_format{};
    if (!dst_resource->texture || !src_resource->texture ||
        dst_resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        src_resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc)) ||
        FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_resource_desc.Format, source_format)) ||
        FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_resource_desc.Format, destination_format)) ||
        !resource_contract::AreCopyCompatibleFormats(dst_resource_desc.Format, Format) ||
        !resource_contract::AreCopyCompatibleFormats(src_resource_desc.Format, Format)) {
      RecordError();
      return;
    }
    const bool source_typeless = (source_format.Flag & MTL_DXGI_FORMAT_TYPELESS) &&
                                 !IsD3D12DepthStencilPlaneView(src_resource_desc.Format);
    const bool destination_typeless = (destination_format.Flag & MTL_DXGI_FORMAT_TYPELESS) &&
                                      !IsD3D12DepthStencilPlaneView(dst_resource_desc.Format);
    if (!copy_states && (((format_desc.Flag & MTL_DXGI_FORMAT_TYPELESS) && !IsD3D12DepthStencilPlaneView(Format)) ||
        (!source_typeless && src_resource_desc.Format != Format) ||
        (!destination_typeless && dst_resource_desc.Format != Format))) {
      RecordError();
      return;
    }
    const auto physical_resolve_format = GetD3D12PhysicalPixelFormat(Format);
    if (physical_resolve_format != WMTPixelFormatInvalid)
      format_desc.PixelFormat = physical_resolve_format;

    UINT dst_level = 0, dst_slice = 0, dst_plane = 0;
    UINT src_level = 0, src_slice = 0, src_plane = 0;
    if (!GetSubresourceInfo(device_->GetMTLDevice(), dst_resource_desc, DstSubresource, dst_level, dst_slice, dst_plane) ||
        !GetSubresourceInfo(device_->GetMTLDevice(), src_resource_desc, SrcSubresource, src_level, src_slice, src_plane) ||
        dst_plane != src_plane || (src_level && src_resource_desc.SampleDesc.Count > 1) ||
        (src_resource_desc.SampleDesc.Count <= 1 && ResolveMode != D3D12_RESOLVE_MODE_DECOMPRESS)) {
      RecordError();
      return;
    }

    const auto src_extent = GetResourceExtent(src_resource_desc, src_level);
    const auto dst_extent = GetResourceExtent(dst_resource_desc, dst_level);
    D3D12_RECT rect = pSrcRect ? *pSrcRect : D3D12_RECT{0, 0, LONG(src_extent.right), LONG(src_extent.bottom)};
    if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
        UINT(rect.right) > src_extent.right || UINT(rect.bottom) > src_extent.bottom ||
        DstX > dst_extent.right || UINT(rect.right - rect.left) > dst_extent.right - DstX ||
        DstY > dst_extent.bottom || UINT(rect.bottom - rect.top) > dst_extent.bottom - DstY) {
      RecordError();
      return;
    }

    const bool decompress = ResolveMode == D3D12_RESOLVE_MODE_DECOMPRESS;
    if (decompress) {
      if (dst_resource_desc.SampleDesc.Count != src_resource_desc.SampleDesc.Count) {
        RecordError();
        return;
      }
      if (pDstResource == pSrcResource && DstSubresource == SrcSubresource) {
        if (DstX != UINT(rect.left) || DstY != UINT(rect.top)) RecordError();
        RecordImplicitUse(pSrcResource, SrcSubresource, render_pass_internal_ ? render_pass_resolve_source_state_ :
                          D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        // Stored Metal samples are already accessible without decompression.
        return;
      }
    }
    const uint8_t dsv_flags = uint8_t(DepthStencilPlanarFlags(src_resource->texture->pixelFormat()));
    const uint8_t dsv_plane = dsv_flags ? (src_plane ? 2 : 1) : 0;
    if (decompress && dsv_plane == 1 && src_resource_desc.SampleDesc.Count > 1 &&
        sample_positions_.count && sample_positions_.count != src_resource_desc.SampleDesc.Count) {
      RecordError();
      return;
    }
    if ((!decompress && dst_resource_desc.SampleDesc.Count != 1) ||
        ((dsv_plane == 2 || IsIntegerFormat(format_desc.PixelFormat)) && ResolveMode == D3D12_RESOLVE_MODE_AVERAGE)) {
      RecordError();
      return;
    }
    // Ending-access resolves consume the attachment in its pass state, not a public RESOLVE_SOURCE transition.
    if (!RecordImplicitUse(pSrcResource, SrcSubresource, render_pass_internal_ ? render_pass_resolve_source_state_ :
                           copy_states ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RESOLVE_SOURCE) ||
        !RecordImplicitUse(pDstResource, DstSubresource, copy_states ? D3D12_RESOURCE_STATE_COPY_DEST :
                           D3D12_RESOURCE_STATE_RESOLVE_DEST))
      return;

    auto &src = src_resource->texture;
    auto &dst = dst_resource->texture;
    const auto sampling_format = dsv_plane == 2 && dsv_flags == 3 ? WMTPixelFormatX32_Stencil8 :
                                 dsv_plane == 1 ? src->pixelFormat() : format_desc.PixelFormat;
    const uint8_t destination_dsv_plane = DepthStencilPlanarFlags(dst->pixelFormat()) ? dsv_plane : 0;
    const auto attachment_format = destination_dsv_plane ? dst->pixelFormat() :
        copy_states && dsv_plane == 1 ?
        (src_resource_desc.Format == DXGI_FORMAT_D16_UNORM || src_resource_desc.Format == DXGI_FORMAT_R16_TYPELESS
         ? WMTPixelFormatR16Unorm : WMTPixelFormatR32Float) : format_desc.PixelFormat;
    TextureViewDescriptor src_view_desc;
    src_view_desc.format = sampling_format;
    src_view_desc.type = src_resource_desc.SampleDesc.Count > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
    src_view_desc.arraySize = 1;
    src_view_desc.firstArraySlice = src_slice;
    src_view_desc.miplevelCount = 1;
    src_view_desc.firstMiplevel = src_level;
    TextureViewDescriptor dst_view_desc;
    dst_view_desc.format = attachment_format;
    dst_view_desc.type = decompress && dst_resource_desc.SampleDesc.Count > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
    dst_view_desc.arraySize = 1;
    dst_view_desc.firstArraySlice = dst_slice;
    dst_view_desc.miplevelCount = 1;
    dst_view_desc.firstMiplevel = dst_level;
    const auto src_view_id = src->createView(src_view_desc);
    const auto dst_view_id = dst->createView(dst_view_desc);
    auto &src_view = src->view(src_view_id);
    auto &dst_view = dst->view(dst_view_id);
    if (!src_view.texture || !dst_view.texture) {
      RecordError(E_OUTOFMEMORY);
      return;
    }

    // Closing the pass here is a scheduling point, not an opaque endpoint:
    // the folded resolve below is a tracked attachment store of that pass.
    allocator_->EndSchedulingPass();
    // Ending-access resolves carry a non-null full rectangle even when they cover the
    // complete native MSAA attachment. Treat that spelling as equivalent only while the
    // pass is being closed; this keeps emulated, sparse, cast, and separated resolves on
    // the existing raster path and preserves ordering boundaries.
    const bool native_full_rect = render_pass_internal_ && allocator_->encoder_last &&
        allocator_->encoder_last->type == EncoderType::Render &&
        static_cast<RenderEncoderData *>(allocator_->encoder_last)->native_msaa_sample_count > 1 &&
        pSrcRect && rect.left == 0 && rect.top == 0 && UINT(rect.right) == src_extent.right &&
        UINT(rect.bottom) == src_extent.bottom && !DstX && !DstY;
    const bool whole_average = !dsv_plane && !predication_enabled_ && ResolveMode == D3D12_RESOLVE_MODE_AVERAGE &&
                               (!pSrcRect || native_full_rect) &&
                               !DstX && !DstY && src_extent.right == dst_extent.right &&
                               src_extent.bottom == dst_extent.bottom &&
                               (Format != DXGI_FORMAT_R32G32B32_FLOAT ||
                                any_bit_set(device_->GetMTLPixelFormatCapability(physical_resolve_format) &
                                            FormatCapability::Resolve));
    // A whole-surface AVERAGE ending-access resolve of a color attachment the
    // closing native MSAA pass stores folds into that pass as
    // StoreAndMultisampleResolve: the tile resolves before it is written back,
    // so the separate ResolvePass that reloads the MSAA surface disappears
    // (D3Dalloy's D3D11 encoder optimizer does the same; D3DMetal 4.0b3 still
    // encodes a separate load + MultisampleResolve pass).
    if (whole_average && native_full_rect && !decompress) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_last);
      for (unsigned i = 0; i < render->render_target_count; ++i) {
        auto &color = render->colors[i];
        if (!color.attachment || color.buffer_attachment || color.resolve_attachment ||
            color.store_action != WMTStoreActionStore || color.level || color.slice || color.depth_plane ||
            color.attachment->allocation != src_view.allocation ||
            color.attachment->key.mip_start != src_view.key.mip_start ||
            color.attachment->key.mip_end != src_view.key.mip_end ||
            color.attachment->key.array_start != src_view.key.array_start ||
            color.attachment->key.array_end != src_view.key.array_end ||
            color.attachment.texture().pixelFormat() != src_view_desc.format)
          continue;
        color.store_action = render_pass_resolve_discards_source_ ? WMTStoreActionMultisampleResolve
                                                                  : WMTStoreActionStoreAndMultisampleResolve;
        color.resolve_attachment = &dst_view;
        color.resolve_level = 0;
        color.resolve_slice = 0;
        color.resolve_depth_plane = 0;
        owned_graphics::Trace("resolve_folded", ",\"encoder\":", render->id, ",\"slot\":", i,
                              ",\"store_source\":", !render_pass_resolve_discards_source_);
        return;
      }
    }
    auto *resolve = allocator_->AllocatePass<ResolveEncoderData>();
    resolve->type = EncoderType::Resolve;
    resolve->src = src_view;
    resolve->dst = dst_view;
    resolve->dsv_flags = destination_dsv_plane;
    if (decompress && src_resource_desc.SampleDesc.Count > 1) resolve->sample_positions = sample_positions_;
    if (!whole_average) {
      auto library = device_->GetLib().getLibrary();
      auto vertex = WMT4::Library(library).newFunction("d3dalloy_resolve_vertex");
      const bool integer = IsIntegerFormat(format_desc.PixelFormat);
      const bool unsigned_integer = integer && MTLGetUnsignedIntegerFormat(format_desc.PixelFormat) == format_desc.PixelFormat;
      const char *fragment_name = dsv_plane == 1 ?
                                  (destination_dsv_plane ? "d3dalloy_resolve_depth" : "d3dalloy_resolve_depth_color") :
                                  dsv_plane == 2 ? (destination_dsv_plane ? "d3dalloy_resolve_stencil" : "d3dalloy_resolve_uint") :
                                  unsigned_integer ? "d3dalloy_resolve_uint" :
                                  integer ? "d3dalloy_resolve_sint" : "d3dalloy_resolve_float";
      if (src_resource_desc.SampleDesc.Count == 1)
        fragment_name = dsv_plane == 1 ?
                        (destination_dsv_plane ? "d3dalloy_decompress_depth_single" : "d3dalloy_decompress_depth_color_single") :
                        dsv_plane == 2 ? (destination_dsv_plane ? "d3dalloy_decompress_stencil_single" : "d3dalloy_decompress_uint_single") :
                        unsigned_integer ? "d3dalloy_decompress_uint_single" :
                        integer ? "d3dalloy_decompress_sint_single" : "d3dalloy_decompress_float_single";
      const uint8_t fragment_kind = (dsv_plane == 1 ? (destination_dsv_plane ? 1 : 2) :
                                     dsv_plane == 2 ? (destination_dsv_plane ? 3 : 4) :
                                     unsigned_integer ? 5 : integer ? 6 : 7) + (decompress ? 8 : 0);
      auto fragment = WMT4::Library(library).newFunction(fragment_name);
      WMTRenderPipelineInfo pipeline_info;
      WMT::InitializeRenderPipelineInfo(pipeline_info);
      pipeline_info.vertex_function = vertex.handle;
      pipeline_info.fragment_function = fragment.handle;
      if (destination_dsv_plane & 1)
        pipeline_info.depth_pixel_format = attachment_format;
      else if (destination_dsv_plane & 2)
        pipeline_info.stencil_pixel_format = attachment_format;
      else
        pipeline_info.colors[0].pixel_format = attachment_format;
      pipeline_info.raster_sample_count = decompress ? src_resource_desc.SampleDesc.Count : 1;
      pipeline_info.rasterization_enabled = true;
      pipeline_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
      const ResolvePipelineKey pipeline_key{
          static_cast<uint32_t>(attachment_format), pipeline_info.raster_sample_count, fragment_kind,
          destination_dsv_plane, uint8_t(destination_dsv_plane & 1), uint8_t(destination_dsv_plane & 2)};
      for (size_t i = 0; i < resolve_pipeline_cache_count_; ++i) {
        if (resolve_pipeline_cache_[i].key == pipeline_key) {
          resolve->pipeline = resolve_pipeline_cache_[i].pipeline;
          break;
        }
      }
      if (!resolve->pipeline) {
        WMT::Reference<WMT::Error> error;
        resolve->pipeline = device_->GetMetal4Compiler().newRenderPipelineState(pipeline_info, error);
        if (resolve->pipeline) {
          auto &entry = resolve_pipeline_cache_[resolve_pipeline_cache_count_ < resolve_pipeline_cache_.size()
              ? resolve_pipeline_cache_count_++ : resolve_pipeline_cache_next_++ % resolve_pipeline_cache_.size()];
          entry.key = pipeline_key;
          entry.pipeline = resolve->pipeline;
        }
      }
      if (destination_dsv_plane) {
        WMTDepthStencilInfo depth_stencil_info{};
        depth_stencil_info.depth_compare_function = WMTCompareFunctionAlways;
        depth_stencil_info.depth_write_enabled = destination_dsv_plane & 1;
        if (destination_dsv_plane & 2) {
          depth_stencil_info.front_stencil.enabled = true;
          depth_stencil_info.front_stencil.depth_stencil_pass_op = WMTStencilOperationReplace;
          depth_stencil_info.front_stencil.depth_fail_op = WMTStencilOperationReplace;
          depth_stencil_info.front_stencil.stencil_fail_op = WMTStencilOperationReplace;
          depth_stencil_info.front_stencil.stencil_compare_function = WMTCompareFunctionAlways;
          depth_stencil_info.front_stencil.read_mask = 0xff;
          depth_stencil_info.front_stencil.write_mask = 0xff;
          depth_stencil_info.back_stencil = depth_stencil_info.front_stencil;
        }
        resolve->depth_stencil_state = device_->GetMTLDevice().newDepthStencilState(depth_stencil_info);
      }
      if (!vertex || !fragment || !resolve->pipeline || (destination_dsv_plane && !resolve->depth_stencil_state)) {
        resolve->~ResolveEncoderData();
        allocator_->encoder_current = nullptr;
        RecordError(E_FAIL);
        return;
      }
      resolve->destination_x = DstX;
      resolve->destination_y = DstY;
      resolve->width = rect.right - rect.left;
      resolve->height = rect.bottom - rect.top;
      resolve->parameters.source_delta_x = rect.left - LONG(DstX);
      resolve->parameters.source_delta_y = rect.top - LONG(DstY);
      resolve->parameters.sample_count = src_resource_desc.SampleDesc.Count;
      resolve->parameters.mode = decompress ? 3 : ResolveMode == D3D12_RESOLVE_MODE_AVERAGE ? 0 :
                                 ResolveMode == D3D12_RESOLVE_MODE_MIN ? 1 : 2;
      resolve->parameters.predicate_enabled = predication_enabled_;
      resolve->parameters.predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
      allocator_->BindGPUHeap(resolve->predicate, resolve->predicate_offset, predication_enabled_ ? predication_snapshot_offset_ : 0);
    }
    allocator_->InvalidateCurrentPass();
  }

  void STDMETHODCALLTYPE
  SetViewInstanceMask(UINT Mask) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE || type_ == D3D12_COMMAND_LIST_TYPE_COPY) {
      RecordError();
      return;
    }
    // A bundle's mask state starts at 0 at ExecuteBundle and what it sets persists in the caller.
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetViewInstanceMask, Mask))
      return;
    // Honored only by PSOs that declare D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING.
    view_instance_mask_ = Mask;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  WriteBufferImmediate(
      UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes
  ) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || (Count && !pParams)) {
      RecordError();
      return;
    }
    if (!Count)
      return;
    for (UINT i = 0; i < Count; ++i) {
      const auto mode = pModes ? pModes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      uint64_t offset = 0;
      auto allocation = device_->LookupBufferByVA(pParams[i].Dest, &offset);
      if ((mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT &&
           mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN &&
           mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT) ||
          (pParams[i].Dest & 3) || !allocation || allocation->byteLength() < 4 ||
          offset > allocation->byteLength() - 4) {
        RecordError();
        return;
      }
    }
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      try {
        std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> parameters(pParams, pParams + Count);
        std::vector<D3D12_WRITEBUFFERIMMEDIATE_MODE> modes(Count, D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT);
        if (pModes)
          std::copy_n(pModes, Count, modes.begin());
        AppendBundleCommand([parameters = std::move(parameters), modes = std::move(modes)](auto &parent) {
          parent.WriteBufferImmediate(parameters.size(), parameters.data(), modes.data());
        });
      } catch (const std::bad_alloc &) {
        RecordError(E_OUTOFMEMORY);
      }
      return;
    }

    // A pass boundary on each side gives all three modes their required ordering.
    // The marker modes are deliberately stronger than their minimum guarantee.
    // Values live in immutable allocator storage until GPU completion; never
    // perform these GPU writes by modifying the destination on the CPU.
    allocator_->InvalidateCurrentPass();
    PreBlit();
    for (UINT i = 0; i < Count; ++i) {
      uint64_t destination_offset = 0;
      auto allocation = device_->LookupBufferByVA(pParams[i].Dest, &destination_offset);
      auto [memory, source_offset] = allocator_->AllocateGPUHeap(sizeof(UINT), alignof(UINT));
      if (!memory) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      memcpy(memory, &pParams[i].Value, sizeof(UINT));
      auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
      allocator_->BindGPUHeap(cmd.src, cmd.src_offset, source_offset);
      cmd.dst = allocation->buffer();
      cmd.dst_offset = destination_offset;
      cmd.copy_length = sizeof(UINT);
    }
    allocator_->InvalidateCurrentPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  /* ID3D12GraphicsCommandList4 acceleration-structure commands. Everything the GPU needs is
   * resolved here and recorded as one Raytracing encoder for the queue worker to encode on the
   * application's own command buffer. */

  void STDMETHODCALLTYPE
  BuildRaytracingAccelerationStructure(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT postbuild_count,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild
  ) try {
    if (FAILED(recording_error_)) return;
    if (!desc || (postbuild_count && !postbuild) || !IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      RecordError();
      return;
    }
    auto *device = static_cast<MTLD3D12Device *>(device_);
    raytracing::BuildInputs inputs;
    raytracing::BufferLocation destination;
    if (FAILED(raytracing::ConvertBuildInputs(desc->Inputs, ResolveRaytracingGPUVA, device, inputs)) ||
        (desc->DestAccelerationStructureData % (uint32_t(inputs.type) == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY ? 128 : raytracing::kAccelerationStructureAlignment)) ||
        !ResolveRaytracingGPUVA(device, desc->DestAccelerationStructureData, raytracing::kHeaderSize, destination) ||
        destination.available <= raytracing::kHeaderSize) {
      RecordError();
      return;
    }
    raytracing::ShadowPlan shadow;
    if (!raytracing::PlanShadow(inputs, shadow)) {
      RecordError();
      return;
    }
    const bool update =
        (inputs.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
    // The application's allocation also stores an ordered GPU copy of the build inputs.
    raytracing::NativeSizes sizes;
    if (FAILED(device->QueryRTASSizes(inputs, sizes)) ||
        shadow.size > destination.available - raytracing::kHeaderSize ||
        (!update && sizes.accelerationStructureSize > destination.available - raytracing::kHeaderSize - shadow.size)) {
      RecordError();
      return;
    }
    RTASState source;
    if (update) {
      raytracing::BufferLocation location;
      if (!desc->SourceAccelerationStructureData ||
          !ResolveRaytracingGPUVA(
              device, desc->SourceAccelerationStructureData, raytracing::kHeaderSize, location
          ) ||
          !device->FindRTAS({location.allocationIdentity, location.offset}, source)) {
        RecordError();
        return;
      }
      if (source.info && !raytracing::IsCompatibleUpdate(*source.info, inputs, shadow)) {
        RecordError();
        return;
      }
    }
    raytracing::BufferLocation scratch;
    const bool ommArray = uint32_t(inputs.type) == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
    if (!ommArray && ((desc->ScratchAccelerationStructureData % raytracing::kAccelerationStructureAlignment) ||
        (!(update && !sizes.refitScratchSize && !desc->ScratchAccelerationStructureData) && !ResolveRaytracingGPUVA(
            device, desc->ScratchAccelerationStructureData,
            update ? sizes.refitScratchSize : sizes.buildScratchSize, scratch
        )))) {
      RecordError();
      return;
    }
    // Metal supports native in-place refits. Out-of-place updates allocate a distinct
    // sidecar so the source structure remains unmodified.
    auto sidecar = update && desc->SourceAccelerationStructureData == desc->DestAccelerationStructureData
        ? source.sidecar : device->GetMTLDevice().newAccelerationStructureSidecar(
            update ? 0 : sizes.accelerationStructureSize, inputs.instanceCount);
    raytracing::NativeDescriptor native;
    if (!sidecar ||
        (!update && !ommArray && FAILED(raytracing::CreateNativeDescriptor(
            device->GetMTLDevice(), inputs, sidecar.instanceBuffer(), native
        )))) {
      RecordError(E_FAIL);
      return;
    }
    RTASComputePipelines pipelines;
    if (FAILED(device->GetRTASComputePipelines(pipelines))) {
      RecordError(E_FAIL);
      return;
    }

    allocator_->InvalidateCurrentPass();
    auto *record = allocator_->AllocatePass<RTASEncoderData>();
    record->type = EncoderType::Raytracing;
    record->operation = update ? RTASOperation::Refit : RTASOperation::Build;
    record->pipelines = pipelines;
    if (update) {
      ResolveRaytracingGPUVA(device, desc->SourceAccelerationStructureData, raytracing::kHeaderSize, record->sourceLocation);
      record->deferredUpdate = true;
      record->nativeCapacity = sizes.accelerationStructureSize;
    }
    record->destinationLocation = destination;
    uint64_t currentSize, buildScratch, updateScratch;
    raytracing::CalculatePrebuildInfo(sizes, shadow.size, &currentSize, &buildScratch, &updateScratch);
    record->metadata = {raytracing::kHeaderMagic, uint32_t(inputs.type), uint32_t(inputs.flags),
        inputs.type == 0 ? inputs.instanceCount : ommArray ? inputs.ommCount : uint32_t(inputs.geometries.size()), shadow.size, currentSize,
        0, uint32_t(std::ranges::any_of(inputs.geometries, [](const raytracing::Geometry &g) {
          return uint32_t(g.type) == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES; })), currentSize};
    auto structureInfo = raytracing::DescribeStructure(inputs, shadow);
    record->shadowPlan = std::move(shadow);
    if (record->deferredUpdate || ommArray)
      record->deferredInputs = inputs;
    record->descriptor = std::move(native.descriptor);
    if (inputs.ommData.buffer) record->ommInputBuffers.emplace_back(inputs.ommData.buffer);
    if (inputs.ommDescs.buffer) record->ommInputBuffers.emplace_back(inputs.ommDescs.buffer);
    for (const auto &geometry : inputs.geometries) {
      if (geometry.ommIndices.buffer) record->ommInputBuffers.emplace_back(geometry.ommIndices.buffer);
      if (geometry.ommArray.buffer) record->ommInputBuffers.emplace_back(geometry.ommArray.buffer);
    }
    record->source = update ? source.sidecar : WMT::AccelerationStructureSidecar{};
    record->destination = sidecar;
    record->header = destination.buffer;
    record->scratch = scratch.buffer;
    record->sourceInstances = inputs.instances.buffer;
    record->sourceInstancesOffset = inputs.instances.offset;
    record->destinationOffset = destination.offset;
    record->scratchOffset = scratch.offset;
    record->instanceCount = inputs.instanceCount;
    record->instanceLayout = static_cast<uint32_t>(inputs.instanceLayout);
    /*
     * Publish the version before anything is encoded. Residency is declared here, so it covers the
     * whole window in which this structure can be read, and the token tells the device which
     * native structure the address will hold once the prepass executes.
     */
    record->destinationKey = {destination.allocationIdentity, destination.offset};
    if (!device->StoreRTAS(record->destinationKey, {sidecar, update ? 0 : sizes.accelerationStructureSize, inputs.instanceCount,
            std::make_shared<const raytracing::StructureInfo>(std::move(structureInfo))},
            &record->versionToken, &record->liveMirror, &record->liveMirrorOffset)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    try {
      for (UINT i = 0; i < postbuild_count; ++i) {
        if (uint32_t(postbuild[i].InfoType) > (ommArray ? 1u : 3u)) {
          RecordError();
          return;
        }
        raytracing::BufferLocation out;
        if (!ResolveRaytracingGPUVA(device, postbuild[i].DestBuffer,
                postbuild[i].InfoType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION ? 16 : 8, out)) {
          RecordError();
          return;
        }
        record->postbuild.push_back({postbuild[i].InfoType, out.buffer, out.offset, out.address});
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    allocator_->InvalidateCurrentPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE
  EmitRaytracingAccelerationStructurePostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT source_count,
      const D3D12_GPU_VIRTUAL_ADDRESS *sources
  ) try {
    if (FAILED(recording_error_)) return;
    if (!desc || !source_count || !sources || uint32_t(desc->InfoType) > 3 || !IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      RecordError();
      return;
    }
    auto *device = static_cast<MTLD3D12Device *>(device_);
    RTASComputePipelines pipelines;
    if (FAILED(device->GetRTASComputePipelines(pipelines))) {
      RecordError(E_FAIL);
      return;
    }
    for (UINT i = 0; i < source_count; ++i) {
      raytracing::BufferLocation location, out;
      RTASState source;
      if (!ResolveRaytracingGPUVA(device, sources[i], raytracing::kHeaderSize, location) ||
          !device->FindRTAS({location.allocationIdentity, location.offset}, source) ||
          !ResolveRaytracingGPUVA(device, desc->DestBuffer + i * (desc->InfoType == 2 ? 16 : 8),
              desc->InfoType == 2 ? 16 : 8, out)) {
        RecordError();
        return;
      }
      allocator_->InvalidateCurrentPass();
      auto *record = allocator_->AllocatePass<RTASEncoderData>();
      record->type = EncoderType::Raytracing;
      record->operation = RTASOperation::Emit;
      record->pipelines = pipelines;
      record->sourceLocation = location;
      record->source = source.sidecar;
      record->postbuild.push_back({desc->InfoType, out.buffer, out.offset, out.address});
    }
    allocator_->InvalidateCurrentPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
      D3D12_GPU_VIRTUAL_ADDRESS destination_address, D3D12_GPU_VIRTUAL_ADDRESS source_address,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode
  ) try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
        uint32_t(mode) > 4) {
      RecordError();
      return;
    }
    auto *device = static_cast<MTLD3D12Device *>(device_);
    raytracing::BufferLocation source_location, destination;
    RTASState source;
    const bool deserialize = mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE;
    const bool export_data = mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE ||
                             mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS;
    if ((source_address % (deserialize ? 256 : 128)) ||
        (destination_address % (export_data ? (mode == 3 ? 256 : 4) : 128)) ||
        !ResolveRaytracingGPUVA(device, source_address, deserialize ? raytracing::kSerializedHeaderSize : raytracing::kHeaderSize, source_location) ||
        (!deserialize && !device->FindRTAS({source_location.allocationIdentity, source_location.offset}, source)) ||
        !ResolveRaytracingGPUVA(device, destination_address, export_data ? 8 : raytracing::kHeaderSize, destination)) {
      RecordError();
      return;
    }
    // A compacted destination may be smaller than the original AS. The
    // application sizes it from GPU postbuild information; reserve its actual
    // native capacity rather than requiring the original prebuild maximum.
    const uint64_t overhead = raytracing::kHeaderSize + (source.info ? source.info->shadowSize : 0);
    const uint64_t native_size = mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT && destination.available >= overhead
        ? std::min(source.nativeSize, destination.available - overhead) : source.nativeSize;
    if (!deserialize && !export_data && (destination.available < overhead || native_size > destination.available - overhead)) {
      RecordError();
      return;
    }
    auto sidecar = export_data ? WMT::Reference<WMT::AccelerationStructureSidecar>{} :
        device->GetMTLDevice().newAccelerationStructureSidecar(native_size, source.instanceCount);
    RTASComputePipelines pipelines;
    if ((!export_data && !sidecar) || FAILED(device->GetRTASComputePipelines(pipelines))) {
      RecordError(E_FAIL);
      return;
    }
    allocator_->InvalidateCurrentPass();
    auto *record = allocator_->AllocatePass<RTASEncoderData>();
    record->type = EncoderType::Raytracing;
    const RTASOperation operations[] = {RTASOperation::Clone, RTASOperation::Compact, RTASOperation::Visualize,
        RTASOperation::Serialize, RTASOperation::Deserialize};
    record->operation = operations[uint32_t(mode)];
    record->pipelines = pipelines;
    record->sourceLocation = source_location;
    record->destinationLocation = destination;
    record->source = source.sidecar;
    record->destination = sidecar;
    record->header = destination.buffer;
    record->destinationOffset = destination.offset;
    record->instanceCount = source.instanceCount;
    /* Same publication rule as a build: the copy publishes the version its destination will hold. */
    record->destinationKey = {destination.allocationIdentity, destination.offset};
    if (!export_data && !device->StoreRTAS(record->destinationKey, {sidecar, native_size, source.instanceCount, source.info},
            &record->versionToken, &record->liveMirror, &record->liveMirrorOffset)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    allocator_->InvalidateCurrentPass();
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession *session) try {
    if (FAILED(recording_error_)) return;
    if (RejectInRenderPass())
      return;
    // Protected resources are not implemented; passing no session is the supported case.
    if (session)
      RecordError(E_NOTIMPL);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  /*
   * Render passes (RenderPassesTier 0: the spec's own emulation through non-pass commands).
   *
   * BeginRenderPass binds every surface whose beginning access is bound for raster (DISCARD,
   * PRESERVE, CLEAR, PRESERVE_LOCAL_RENDER, and PRESERVE_LOCAL_SRV with a read-only DSV flag), in
   * order without gaps, as if by OMSetRenderTargets, then performs the CLEARs. EndRenderPass performs
   * the RESOLVEs and unbinds the surfaces. DISCARD and PRESERVE need no work: discarded contents are
   * undefined, so keeping them is conformant. A RESUMING pass continues the suspended pass of the
   * previous command list, so it skips its beginning loads, and a SUSPENDING pass skips its ending
   * resolves; the matching pass that finally ends the sequence repeats them (vkd3d-proton does the
   * same). The APIs the spec disallows inside a pass fail the recording (RejectInRenderPass).
   */
  static constexpr UINT kBeginPreserveLocalRender = 4, kBeginPreserveLocalSrv = 5, kBeginPreserveLocalUav = 6;
  static constexpr UINT kEndPreserveLocalRender = 4, kEndPreserveLocalUav = 6;

  static bool
  BindsForRaster(UINT type, bool read_only_flag) {
    switch (type) {
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD:
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE:
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR:
    case kBeginPreserveLocalRender: return true;
    case kBeginPreserveLocalSrv: return read_only_flag;
    default: return false;
    }
  }

  static bool
  ValidBeginningAccess(UINT type) {
    return type <= kBeginPreserveLocalUav;
  }

  static bool
  ValidEndingAccess(const D3D12_RENDER_PASS_ENDING_ACCESS &access) {
    if (access.Type > kEndPreserveLocalUav)
      return false;
    if (access.Type != D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
      return true;
    const auto &r = access.Resolve;
    return r.pSrcResource && r.pDstResource && (!r.SubresourceCount || r.pSubresourceParameters) &&
           (r.ResolveMode == D3D12_RESOLVE_MODE_AVERAGE || r.ResolveMode == D3D12_RESOLVE_MODE_MIN ||
            r.ResolveMode == D3D12_RESOLVE_MODE_MAX || r.ResolveMode == D3D12_RESOLVE_MODE_DECOMPRESS);
  }

  // The APIs the render passes spec disallows inside a pass, and between a pass that ends in
  // PRESERVE_LOCAL_* (or suspends) and the next pass, where only pass setup is valid.
  bool
  RejectInRenderPass() {
    if (render_pass_internal_ || (!render_pass_active_ && !render_pass_pending_local_ && !render_pass_suspended_))
      return false;
    ERR("command not allowed inside a render pass");
    RecordError();
    return true;
  }

  bool
  IsRenderPassSurface(ID3D12Resource *resource) const {
    auto *texture = resource ? static_cast<MTLD3D12Resource *>(resource)->texture.ptr() : nullptr;
    return texture && std::find(render_pass_surfaces_.begin(), render_pass_surfaces_.end(), texture) !=
                          render_pass_surfaces_.end();
  }

  // Dispatch is disallowed in a pass, except the local compute access mode of passes whose
  // surfaces begin in PRESERVE_LOCAL_SRV/UAV.
  bool
  RejectDispatchInRenderPass() {
    if (render_pass_active_ && render_pass_local_compute_)
      return false;
    return RejectInRenderPass();
  }

  void STDMETHODCALLTYPE
  BeginRenderPass(
      UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *pRenderTargets,
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags
  ) try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    constexpr UINT known_flags = D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES | D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS |
                                 D3D12_RENDER_PASS_FLAG_RESUMING_PASS | 0x8 /* BIND_READ_ONLY_DEPTH */ |
                                 0x10 /* BIND_READ_ONLY_STENCIL */;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT || render_pass_active_ ||
        NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT || (NumRenderTargets && !pRenderTargets) ||
        (UINT(Flags) & ~known_flags)) {
      ERR("BeginRenderPass: invalid call");
      RecordError();
      return;
    }
    for (UINT i = 0; i < NumRenderTargets; ++i)
      if (!ValidBeginningAccess(pRenderTargets[i].BeginningAccess.Type) ||
          !ValidEndingAccess(pRenderTargets[i].EndingAccess)) {
        RecordError();
        return;
      }
    if (pDepthStencil &&
        (!ValidBeginningAccess(pDepthStencil->DepthBeginningAccess.Type) ||
         !ValidBeginningAccess(pDepthStencil->StencilBeginningAccess.Type) ||
         !ValidEndingAccess(pDepthStencil->DepthEndingAccess) ||
         !ValidEndingAccess(pDepthStencil->StencilEndingAccess))) {
      RecordError();
      return;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> bound{};
    UINT bound_count = 0;
    for (UINT i = 0; i < NumRenderTargets; ++i)
      if (BindsForRaster(pRenderTargets[i].BeginningAccess.Type, false))
        bound[bound_count++] = pRenderTargets[i].cpuDescriptor;
    const bool bind_depth =
        pDepthStencil &&
        (BindsForRaster(pDepthStencil->DepthBeginningAccess.Type, UINT(Flags) & 0x8) ||
         BindsForRaster(pDepthStencil->StencilBeginningAccess.Type, UINT(Flags) & 0x10));
    const bool resuming = UINT(Flags) & D3D12_RENDER_PASS_FLAG_RESUMING_PASS;
    render_pass_surfaces_.clear();
    for (UINT i = 0; i < bound_count; ++i) {
      auto [heap, index] = GetRenderTargetHeap(device_, bound[i]);
      if (auto *texture = heap->GetRenderTarget(index).Texture)
        render_pass_surfaces_.push_back(texture);
    }
    if (bind_depth) {
      auto [heap, index] = GetRenderTargetHeap(device_, pDepthStencil->cpuDescriptor);
      if (auto *texture = heap->GetRenderTarget(index).Texture)
        render_pass_surfaces_.push_back(texture);
    }

    try {
      render_pass_resolves_.clear();
      for (UINT i = 0; i < NumRenderTargets; ++i)
        if (pRenderTargets[i].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
          render_pass_resolves_.push_back(CopyResolve(pRenderTargets[i].EndingAccess.Resolve, D3D12_RESOURCE_STATE_RENDER_TARGET));
      if (pDepthStencil) {
        if (pDepthStencil->DepthEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
          render_pass_resolves_.push_back(CopyResolve(pDepthStencil->DepthEndingAccess.Resolve,
              UINT(Flags) & 0x8 ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE));
        if (pDepthStencil->StencilEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
          render_pass_resolves_.push_back(CopyResolve(pDepthStencil->StencilEndingAccess.Resolve,
              UINT(Flags) & 0x10 ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE));
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    // A pass that ends a surface in PRESERVE_LOCAL_* must be followed by another pass in this list.
    bool ends_local = false;
    for (UINT i = 0; i < NumRenderTargets; ++i)
      ends_local |= pRenderTargets[i].EndingAccess.Type >= kEndPreserveLocalRender;
    if (pDepthStencil)
      ends_local |= pDepthStencil->DepthEndingAccess.Type >= kEndPreserveLocalRender ||
                    pDepthStencil->StencilEndingAccess.Type >= kEndPreserveLocalRender;
    bool local_compute = false;
    for (UINT i = 0; i < NumRenderTargets; ++i)
      local_compute |= pRenderTargets[i].BeginningAccess.Type == kBeginPreserveLocalSrv ||
                       pRenderTargets[i].BeginningAccess.Type == kBeginPreserveLocalUav;

    // Beginning a pass is valid in the pending-local/suspended windows; the checks above cover it.
    render_pass_pending_local_ = render_pass_suspended_ = false;
    render_pass_internal_ = true;
    OMSetRenderTargets(bound_count, bound.data(), FALSE,
                       bind_depth ? &pDepthStencil->cpuDescriptor : nullptr);
    if (!resuming) {
      for (UINT i = 0; i < NumRenderTargets; ++i) {
        const auto &access = pRenderTargets[i].BeginningAccess;
        if (access.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
          ClearRenderTargetView(pRenderTargets[i].cpuDescriptor, access.Clear.ClearValue.Color, 0, nullptr);
      }
      if (pDepthStencil) {
        UINT clear_flags = 0;
        if (pDepthStencil->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
          clear_flags |= D3D12_CLEAR_FLAG_DEPTH;
        if (pDepthStencil->StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
          clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
        if (clear_flags)
          ClearDepthStencilView(pDepthStencil->cpuDescriptor, D3D12_CLEAR_FLAGS(clear_flags),
                                pDepthStencil->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth,
                                pDepthStencil->StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil, 0,
                                nullptr);
      }
    }
    render_pass_internal_ = false;
    render_pass_active_ = true;
    render_pass_pending_local_ = render_pass_suspended_ = false;
    render_pass_flags_ = UINT(Flags);
    render_pass_ends_local_ = ends_local;
    render_pass_local_compute_ = local_compute;
    render_pass_discard_colors_.fill(false);
    render_pass_discard_color_count_ = 0;
    render_pass_discard_depth_ = render_pass_discard_stencil_ = false;
    for (UINT i = 0; i < NumRenderTargets; ++i) {
      if (!BindsForRaster(pRenderTargets[i].BeginningAccess.Type, false)) continue;
      render_pass_discard_colors_[render_pass_discard_color_count_++] =
          pRenderTargets[i].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
    }
    if (pDepthStencil) {
      const bool bind_depth = BindsForRaster(pDepthStencil->DepthBeginningAccess.Type, UINT(Flags) & 0x8);
      const bool bind_stencil = BindsForRaster(pDepthStencil->StencilBeginningAccess.Type, UINT(Flags) & 0x10);
      render_pass_discard_depth_ = bind_depth &&
          pDepthStencil->DepthEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
      render_pass_discard_stencil_ = bind_stencil &&
          pDepthStencil->StencilEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
    }
    if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render) {
      auto *render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
      for (UINT i = 0; i < render_pass_discard_color_count_; ++i)
        if (render_pass_discard_colors_[i]) render->colors[i].store_action = WMTStoreActionDontCare;
      if (render_pass_discard_depth_) render->depth.store_action = WMTStoreActionDontCare;
      if (render_pass_discard_stencil_) render->stencil.store_action = WMTStoreActionDontCare;
    }
    owned_graphics::Trace("render_pass_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"rtvs\":", bound_count, ",\"dsv\":", bind_depth, ",\"flags\":", UINT(Flags));
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  static RenderPassResolve
  CopyResolve(const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS &params, D3D12_RESOURCE_STATES source_state) {
    RenderPassResolve resolve{params.pSrcResource, params.pDstResource, params.Format, params.ResolveMode, source_state, {},
                              bool(params.PreserveResolveSource)};
    resolve.regions.assign(params.pSubresourceParameters, params.pSubresourceParameters + params.SubresourceCount);
    return resolve;
  }

  void STDMETHODCALLTYPE
  EndRenderPass() try {
    if (FAILED(recording_error_)) return;
    if (RejectBundleOperation())
      return;
    if (!render_pass_active_) {
      ERR("EndRenderPass: no active render pass");
      RecordError();
      return;
    }
    render_pass_active_ = false;
    const bool suspending = render_pass_flags_ & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS;
    render_pass_internal_ = true;
    if (!suspending) {
      for (auto &resolve : render_pass_resolves_) {
        render_pass_resolve_source_state_ = resolve.source_state;
        // Without PreserveResolveSource the source is undefined after the
        // pass. Only a single-region resolve can then skip storing it: a
        // later region of the same source still reads the stored samples.
        render_pass_resolve_discards_source_ = !resolve.preserve_source && resolve.regions.size() == 1;
        for (auto &region : resolve.regions)
          ResolveSubresourceRegion(resolve.dst, region.DstSubresource, region.DstX, region.DstY, resolve.src,
                                   region.SrcSubresource, &region.SrcRect, resolve.format, resolve.mode);
        render_pass_resolve_discards_source_ = false;
      }
    }
    render_pass_resolves_.clear();
    render_pass_surfaces_.clear();
    // The pass surfaces are unbound after the pass.
    OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    render_pass_internal_ = false;
    render_pass_pending_local_ = render_pass_ends_local_;
    render_pass_suspended_ = suspending;
    render_pass_discard_colors_.fill(false);
    render_pass_discard_color_count_ = 0;
    render_pass_discard_depth_ = render_pass_discard_stencil_ = false;
    owned_graphics::Trace("render_pass_end", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"suspending\":", suspending);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) try {
    if (FAILED(recording_error_)) return;
    RecordError(E_NOTIMPL);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) try {
    if (FAILED(recording_error_)) return;
    RecordError(E_NOTIMPL);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE ConvertLinearAlgebraMatrix(const D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO *descs, UINT count) override try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || (count && !descs) || RejectInRenderPass() ||
        (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)) { RecordError(); return; }
    if (!count) return;
    try {
      if (!matrix_conversion_pipeline_) {
        WMT::Reference<WMT::Error> error;
        auto library = device_->GetMTLDevice().newLibraryWithSource(matrix::kConversionSource, error);
        auto function = library ? WMT4::Library(library).newFunction("d3dalloy_convert_matrix") : WMT::Reference<WMT4::Function>();
        if (function) matrix_conversion_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(function, error);
        if (!matrix_conversion_pipeline_) { RecordError(E_FAIL); return; }
      }
      for (UINT i = 0; i < count; ++i) {
        const auto &d = descs[i];
        uint32_t dest_bytes, source_bytes, dest_stride, source_stride;
        raytracing::BufferLocation source, destination;
        if (d.DestInfo.DestDataType != d.SrcInfo.SrcDataType ||
            !matrix::Footprint(d.DestInfo.NumRows, d.DestInfo.NumColumns, d.DestInfo.DestDataType,
                              d.DestInfo.DestLayout, d.DestInfo.DestStride, true, dest_bytes, dest_stride) ||
            !matrix::Footprint(d.DestInfo.NumRows, d.DestInfo.NumColumns, d.SrcInfo.SrcDataType,
                              d.SrcInfo.SrcLayout, d.SrcInfo.SrcStride, false, source_bytes, source_stride) ||
            d.DestInfo.DestSize < dest_bytes || d.DestInfo.DestSize % 16 || d.SrcInfo.SrcSize < source_bytes ||
            d.DataDesc.DestVA % matrix::ComponentBytes(d.DestInfo.DestDataType) ||
            d.DataDesc.SrcVA % matrix::ComponentBytes(d.SrcInfo.SrcDataType) ||
            !ResolveRaytracingGPUVA(device_, d.DataDesc.SrcVA, d.SrcInfo.SrcSize, source) ||
            !ResolveRaytracingGPUVA(device_, d.DataDesc.DestVA, d.DestInfo.DestSize, destination) ||
            (source.buffer == destination.buffer && source.offset < destination.offset + dest_bytes &&
             destination.offset < source.offset + source_bytes)) { RecordError(); return; }
        const matrix::ConversionParams control{d.DestInfo.NumRows, d.DestInfo.NumColumns,
            uint32_t(d.SrcInfo.SrcDataType), uint32_t(d.SrcInfo.SrcLayout),
            uint32_t(d.DestInfo.DestLayout), source_stride, dest_stride};
        if (!allocator_->ReserveGPUHeap(sizeof(control), 16)) { RecordError(E_OUTOFMEMORY); return; }
        auto [storage, offset] = allocator_->AllocateGPUHeap(sizeof(control), 16);
        memcpy(storage, &control, sizeof(control));
        const uint32_t arguments[]{control.columns / 64 + (control.columns % 64 != 0), control.rows, 1};
        uint64_t indirect = 0;
        if (predication_enabled_) {
          indirect = PreparePredicatedArguments(arguments, sizeof(arguments));
          if (indirect == UINT64_MAX) { RecordError(E_OUTOFMEMORY); return; }
        }
        BeginPredicationComputePass(matrix_conversion_pipeline_, 64);
        auto *compute = static_cast<ComputeEncoderData *>(allocator_->encoder_current);
        compute->internal_buffer_owners.emplace_back(const_cast<BufferAllocation *>(static_cast<const BufferAllocation *>(source.allocationIdentity)));
        compute->internal_buffer_owners.emplace_back(const_cast<BufferAllocation *>(static_cast<const BufferAllocation *>(destination.allocationIdentity)));
        BindPredicationBuffer(source.buffer, source.offset, 0);
        BindPredicationBuffer(destination.buffer, destination.offset, 1);
        BindGPUHeapPredicationBuffer(offset, 2);
        if (predication_enabled_) {
          auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
          dispatch.type = WMTComputeCommandDispatchIndirect;
          allocator_->BindGPUHeap(dispatch.indirect_args_buffer, dispatch.indirect_args_offset, indirect);
        } else {
          auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
          dispatch.type = WMTComputeCommandDispatch; dispatch.size = {arguments[0], arguments[1], 1};
        }
        allocator_->InvalidateCurrentPass();
        dirty_state_.set(DirtyState::ComputePipelineState);
      }
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); }
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void RecordFenceBarrier(bool signal, UINT groups, const D3D12_BARRIER_GROUP *barriers,
                          UINT count, ID3D12Fence *const *fences, const UINT64 *values) {
    if (!IsRecording() || RejectInRenderPass() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
        (groups && !barriers) || (count && (!fences || !values))) { RecordError(); return; }
    try {
      std::vector<FenceBarrierEncoderData::Operand> operands;
      operands.reserve(count);
      for (UINT i = 0; i < count; ++i) {
        Com<ID3D12Device> owner;
        if (!fences[i] || FAILED(fences[i]->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
            owner.ptr() != static_cast<ID3D12Device *>(device_)) { RecordError(); return; }
        operands.push_back({Com<ID3D12Fence>(fences[i]), values[i]});
      }
      if (!signal && count) {
        allocator_->InvalidateCurrentPass();
        auto *record = allocator_->AllocatePass<FenceBarrierEncoderData>();
        record->type = EncoderType::FenceBarrier; record->operands = std::move(operands);
        record->state_event_index = static_cast<RecordingHead *>(entry)->state_events.size();
        allocator_->InvalidateCurrentPass();
      }
      Barrier(groups, barriers);
      if (FAILED(recording_error_)) return;
      if (signal) {
        allocator_->InvalidateCurrentPass();
        auto *record = allocator_->AllocatePass<FenceBarrierEncoderData>();
        record->type = EncoderType::FenceBarrier; record->signal = true; record->operands = std::move(operands);
        record->state_event_index = static_cast<RecordingHead *>(entry)->state_events.size();
        allocator_->InvalidateCurrentPass();
      }
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); }
  }

  void STDMETHODCALLTYPE WaitBarrier(UINT groups, const D3D12_BARRIER_GROUP *barriers,
      UINT count, ID3D12Fence *const *fences, const UINT64 *values) override try {
    if (FAILED(recording_error_)) return;
    RecordFenceBarrier(false, groups, barriers, count, fences, values);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }
  void STDMETHODCALLTYPE SignalBarrier(UINT groups, const D3D12_BARRIER_GROUP *barriers,
      ID3D12Fence *fence, UINT64 value) override try {
    if (FAILED(recording_error_)) return;
    RecordFenceBarrier(true, groups, barriers, 1, &fence, &value);
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }
  void STDMETHODCALLTYPE SetWorkGraphMaximumGPUInputRecords(UINT records, UINT nodes) override try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || RejectInRenderPass() ||
        (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)) { RecordError(); return; }
    work_graph_max_records_ = records; work_graph_max_node_inputs_ = nodes;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }


  void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC *desc) override try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || !desc || RejectInRenderPass() ||
        (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)) { RecordError(); return; }
    if (desc->Type != kProgramTypeWorkGraph || (desc->WorkGraph.Flags & ~kSetWorkGraphFlagInitialize)) { RecordError(); return; }
    uint32_t index = 0; auto object = MTLD3D12WorkGraph::Resolve(device_, desc->WorkGraph.ProgramIdentifier, index);
    if (!object) { RecordError(); return; }
    const auto *graph = object->GetGraph(index);
    const auto &backing = desc->WorkGraph.BackingMemory;
    uint64_t offset = 0; auto *allocation = device_->LookupBufferByVA(backing.StartAddress, &offset);
    if (backing.StartAddress % kWorkGraphsBackingMemoryAlignment || backing.SizeInBytes < graph->memory.MinSizeInBytes ||
        !allocation || offset > allocation->byteLength() || backing.SizeInBytes > allocation->byteLength() - offset) { RecordError(); return; }
    const auto &local = desc->WorkGraph.NodeLocalRootArgumentsTable;
    for (const auto &node : graph->nodes) if (node.local_root && node.local_root->ParameterQwords) {
      const uint64_t needed = uint64_t(node.info.LocalRootArgumentsTableIndex) * local.StrideInBytes +
          uint64_t(node.local_root->ParameterQwords) * 8;
      if (!local.StartAddress || needed > local.SizeInBytes) { RecordError(); return; }
    }
    work_graph_ = object; work_graph_index_ = index; work_graph_program_ = desc->WorkGraph;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC *desc) override try {
    if (FAILED(recording_error_)) return;
    if (!IsRecording() || !desc || !work_graph_ || desc->Mode > kDispatchModeMultiNodeGPUInput || RejectInRenderPass()) { RecordError(); return; }
    try {
      auto dispatch = std::make_shared<WorkGraphDispatch>();
      dispatch->object = work_graph_; dispatch->graph = work_graph_index_; dispatch->program = work_graph_program_; dispatch->mode = desc->Mode;
      dispatch->max_gpu_records = work_graph_max_records_; dispatch->max_gpu_node_inputs = work_graph_max_node_inputs_;
      const auto *graph = work_graph_->GetGraph(work_graph_index_);
      auto copy_input = [&](const D3D12_NODE_CPU_INPUT &input) {
        if (input.EntrypointIndex >= graph->entries.size()) return false;
        const auto &node = graph->nodes[graph->entries[input.EntrypointIndex]];
        const uint64_t size = node.info.Input.RecordSize;
        if ((size && input.NumRecords && !input.pRecords) || (input.RecordStrideInBytes && input.RecordStrideInBytes < size)) return false;
        WorkGraphDispatch::Input copy{}; copy.entry = input.EntrypointIndex; copy.count = input.NumRecords; copy.stride = size;
        copy.records.resize(size * input.NumRecords + (!size ? 1 : 0));
        for (uint32_t r = 0; r < input.NumRecords; ++r) if (size)
          std::memcpy(copy.records.data() + size * r, static_cast<const uint8_t *>(input.pRecords) + input.RecordStrideInBytes * r, size);
        dispatch->inputs.push_back(std::move(copy)); return true;
      };
      if (desc->Mode == kDispatchModeNodeCPUInput) { if (!copy_input(desc->NodeCPUInput)) { RecordError(); return; } }
      else if (desc->Mode == kDispatchModeMultiNodeCPUInput) {
        const auto &multi = desc->MultiNodeCPUInput;
        if (multi.NumNodeInputs && !multi.pNodeInputs) { RecordError(); return; }
        for (UINT i = 0; i < multi.NumNodeInputs; ++i) {
          D3D12_NODE_CPU_INPUT input; std::memcpy(&input, reinterpret_cast<const uint8_t *>(multi.pNodeInputs) + multi.NodeInputStrideInBytes * i, sizeof(input));
          if (!copy_input(input)) { RecordError(); return; }
        }
      } else dispatch->gpu_input = desc->Mode == kDispatchModeNodeGPUInput ? desc->NodeGPUInput : desc->MultiNodeGPUInput;
      auto *root = work_graph_->RootSignature(work_graph_index_);
      const bool atomic_phase = std::any_of(graph->nodes.begin(), graph->nodes.end(), [](const auto &node) { return node.atomic64_phase; });
      if (atomic_phase) {
        CaptureStandardSwizzleTables(dispatch->atomic64_tables, root, rootarg_compute_staging_, true);
        CaptureAtomic64Roots(dispatch->atomic64_roots, root, rootarg_compute_staging_);
        if (descriptor_heaps_[0]) dispatch->atomic64_heap = static_cast<MTLD3D12DescriptorHeap *>(descriptor_heaps_[0]);
      }
      const uint64_t bytes = uint64_t(root->UploadQwords) * 8 + uint64_t(root->NumStaticSamplers) * 64 + 128;
      if (!allocator_->ReserveGPUHeap(bytes, 64)) { RecordError(E_OUTOFMEMORY); return; }
      dispatch->roots = EncodeRootArgument(root, rootarg_compute_staging_);
      if (dispatch->roots == UINT64_MAX) return;
      if (root->NumStaticSamplers) {
        // The roots and samplers must share the chunk reserved above.
        dispatch->samplers = EncodeStaticSamplers(root, false);
        if (dispatch->samplers == UINT64_MAX) return;
      }
      if (root->UploadQwords || root->NumStaticSamplers) {
        const auto binding = allocator_->ResolveGPUHeap(root->UploadQwords ? dispatch->roots : dispatch->samplers);
        dispatch->arguments = binding.buffer;
        if (root->UploadQwords)
          dispatch->roots = binding.offset;
        if (root->NumStaticSamplers)
          dispatch->samplers = allocator_->ResolveGPUHeap(dispatch->samplers).offset;
      }
      allocator_->InvalidateCurrentPass();
      auto *pass = allocator_->AllocatePass<WorkGraphEncoderData>(); pass->type = EncoderType::WorkGraph; pass->dispatch = std::move(dispatch);
      allocator_->InvalidateCurrentPass();
      dirty_state_.set(DirtyState::ComputePipelineState);
      owned_graphics::Trace("dispatch_graph", ",\"mode\":", desc->Mode, ",\"graph\":", work_graph_index_);
    } catch (const std::bad_alloc &) { RecordError(E_OUTOFMEMORY); }
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *pStateObject) try {
    if (FAILED(recording_error_)) return;
    work_graph_ = nullptr;
    if (!IsRecording()) {
      RecordError(E_FAIL);
      return;
    }
    if (FAILED(recording_error_))
      return;
    if (!pStateObject) {
      if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetPipelineState1, pStateObject))
        return;
      rt_state_object_ = nullptr;
      rt_pipeline_ = {};
      rt_intersection_function_table_ = {};
      rt_visible_function_table_ = {};
      return;
    }
    if (!QueryObjectOwnedByThisDevice(pStateObject)) {
      RecordError();
      return;
    }
    auto *state_object = dynamic_cast<MTLD3D12StateObject *>(pStateObject);
    if (!state_object) {
      RecordError();
      return;
    }
    auto pipeline = state_object->GetPipelineState();
    auto intersection_function_table = state_object->GetIntersectionFunctionTable();
    auto visible_function_table = state_object->GetVisibleFunctionTable();
    /* A collection has no compiled pipeline, so DispatchRays cannot run with it. */
    if (!pipeline || !intersection_function_table || !visible_function_table) {
      RecordError();
      return;
    }
    /*
     * D3D12 allows SetPipelineState1 and DispatchRays in bundles. The bundle keeps its
     * own reference, so the replay at ExecuteBundle binds the same compiled pipeline.
     */
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      AppendBundleCommand([retained = Com<ID3D12StateObject>(pStateObject)](auto &parent) {
        parent.SetPipelineState1(retained.ptr());
      });
      return;
    }
    rt_state_object_ = state_object;
    rt_pipeline_ = pipeline;
    rt_intersection_function_table_ = intersection_function_table;
    rt_visible_function_table_ = visible_function_table;
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *desc) try {
    if (FAILED(recording_error_)) return;
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      /* The description is copied: the application may reuse its memory once the call returns. */
      if (!desc) {
        RecordError();
        return;
      }
      AppendBundleCommand([copy = *desc](auto &parent) { parent.DispatchRays(&copy); });
      return;
    }
    if (RejectInRenderPass())
      return;
    if (!IsRecording() || FAILED(recording_error_)) {
      RecordError(E_FAIL);
      return;
    }
    if (!desc || !rt_state_object_ || !rt_pipeline_ || !rt_intersection_function_table_ ||
        !rt_visible_function_table_) {
      RecordError();
      return;
    }

    raytracing::DispatchDescription description{};
    description.ray_generation = {
        desc->RayGenerationShaderRecord.StartAddress, desc->RayGenerationShaderRecord.SizeInBytes, 0
    };
    description.miss = {desc->MissShaderTable.StartAddress, desc->MissShaderTable.SizeInBytes,
                        desc->MissShaderTable.StrideInBytes};
    description.hit_group = {desc->HitGroupTable.StartAddress, desc->HitGroupTable.SizeInBytes,
                             desc->HitGroupTable.StrideInBytes};
    description.callable = {desc->CallableShaderTable.StartAddress, desc->CallableShaderTable.SizeInBytes,
                            desc->CallableShaderTable.StrideInBytes};
    description.width = desc->Width;
    description.height = desc->Height;
    description.depth = desc->Depth;

    raytracing::PreparedDispatch prepared{};
    if (raytracing::PrepareDispatch(description, prepared) != raytracing::SbtStatus::Ok) {
      RecordError();
      return;
    }
    /* Every declared table must lie in one GPU allocation; the traversal reads its records in place. */
    for (const auto *table : {&prepared.ray_generation, &prepared.miss, &prepared.hit_group, &prepared.callable}) {
      if (!table->present())
        continue;
      uint64_t offset = 0;
      auto *allocation = device_->LookupBufferByVA(table->base, &offset);
      if (!allocation || offset > allocation->byteLength() || table->size > allocation->byteLength() - offset) {
        RecordError();
        return;
      }
    }

    auto *state_object = static_cast<MTLD3D12StateObject *>(rt_state_object_.ptr());
    if (!CanStageRaytracingDispatch(1)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    auto [arguments_memory, arguments_offset] =
        allocator_->AllocateGPUHeap(sizeof(SM50_RT_DISPATCH_ARGUMENTS), 64);
    auto *arguments = static_cast<SM50_RT_DISPATCH_ARGUMENTS *>(arguments_memory);
    memset(arguments, 0, sizeof(*arguments));
    arguments->RayGenerationRecord = prepared.ray_generation.base;
    arguments->MissRecordBase = prepared.miss.base;
    arguments->HitGroupRecordBase = prepared.hit_group.base;
    arguments->CallableRecordBase = prepared.callable.base;
    arguments->MissRecordStride = static_cast<uint32_t>(prepared.miss.stride);
    arguments->MissRecordCount = prepared.miss.record_count;
    arguments->HitGroupRecordStride = static_cast<uint32_t>(prepared.hit_group.stride);
    arguments->HitGroupRecordCount = prepared.hit_group.record_count;
    arguments->CallableRecordStride = static_cast<uint32_t>(prepared.callable.stride);
    arguments->CallableRecordCount = prepared.callable.record_count;
    FillRaytracingDispatchConstants(*arguments, *state_object, allocator_->GPUHeapAddress(arguments_offset));
    arguments->DispatchWidth = desc->Width;
    arguments->DispatchHeight = desc->Height;
    arguments->DispatchDepth = desc->Depth;

    /*
     * Rays share threadgroups (raygen threads past the grid return at once). A direct dispatch knows its
     * grid, so the group is shaped to it; under predication the grid is the predicated copy of the
     * threadgroup counts, so a culled dispatch launches zero threadgroups.
     */
    const WMTSize group = RayGenerationThreadgroup(*state_object, desc->Height, desc->Depth);
    uint64_t predicated_grid_offset = ~0ull;
    if (predication_enabled_) {
      const uint32_t grid[3] = {uint32_t((desc->Width + group.width - 1) / group.width),
                                uint32_t((desc->Height + group.height - 1) / group.height),
                                uint32_t((desc->Depth + group.depth - 1) / group.depth)};
      predicated_grid_offset = PreparePredicatedArguments(grid, sizeof(grid));
      if (predicated_grid_offset == UINT64_MAX) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
    }
    auto *traversal = EncodeRaytracingTraversal(arguments_offset, {desc->Width, desc->Height, desc->Depth}, group,
                                               predicated_grid_offset);
    if (!traversal)
      return;
    owned_graphics::Trace(
        "dispatch_rays", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
        ",\"encoder\":", traversal->id, ",\"width\":", desc->Width, ",\"height\":", desc->Height,
        ",\"depth\":", desc->Depth, ",\"exports\":", arguments->ExportCount, ",\"miss\":", arguments->MissRecordCount,
        ",\"hitgroup\":", arguments->HitGroupRecordCount, ",\"callable\":", arguments->CallableRecordCount
    );
  } catch (const std::bad_alloc &) {
    RecordError(E_OUTOFMEMORY);
  }

  /*
   * ExecuteIndirect with D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS. The descriptions are GPU data, so the
   * DispatchRays validation runs on the GPU: one thread per slot (d3dalloy_rays_indirect) turns the slot's
   * D3D12_DISPATCH_RAYS_DESC into the table fields and dimensions of the slot's own dispatch constants
   * and writes its threadgroup grid - zero past the count buffer, under a false predicate, or for a
   * description DispatchRays would reject. Each slot is then one traversal dispatched indirectly from
   * that grid; the traversal resolves the records out of the application's tables itself, so nothing
   * else about a slot depends on what the GPU wrote.
   */
  void RecordDispatchRaysIndirect(MTLD3D12CommandSignature *sig, UINT MaxCommandCount, uint64_t ArgBufferAddress,
                                  uint64_t CountBufferAddress) {
    if (!rt_state_object_ || !rt_pipeline_ || !rt_intersection_function_table_ || !rt_visible_function_table_) {
      RecordError();
      return;
    }
    if (!EnsureRaysIndirectPipeline()) {
      RecordError(E_FAIL);
      return;
    }
    constexpr uint64_t kBlockBytes = (sizeof(SM50_RT_DISPATCH_ARGUMENTS) + 63) & ~uint64_t(63);
    const uint64_t blocks_bytes = kBlockBytes * MaxCommandCount, grids_bytes = uint64_t(MaxCommandCount) * 16;
    auto *state_object = static_cast<MTLD3D12StateObject *>(rt_state_object_.ptr());
    /* Root-argument-changing signatures: one root copy per slot, laid out by the dispatch root signature (the
     * global parameters at their slot offsets), as EncodeRaytracingTraversal binds them. */
    auto *dispatch_rootsig = state_object->GetDispatchRootSignature();
    const uint64_t slot_root_bytes =
        sig->UpdateRootArguments && dispatch_rootsig
            ? uint64_t(dispatch_rootsig->UploadQwords) * sizeof(uint64_t) * MaxCommandCount + 128
            : 0;
    if (!CanStageRaytracingDispatch(MaxCommandCount) ||
        !allocator_->ReserveGPUHeap(sizeof(raytracing::RaysIndirectControl) + blocks_bytes + grids_bytes +
                                            slot_root_bytes + 192,
                                        64)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    uint64_t roots_offset = ~0ull;
    if (sig->UpdateRootArguments) {
      roots_offset = StageSlotRootArguments(sig, dispatch_rootsig, rootarg_compute_staging_, MaxCommandCount,
                                            ArgBufferAddress, CountBufferAddress);
      if (roots_offset == UINT64_MAX)
        return;
    }
    auto [control_memory, control_offset] = allocator_->AllocateGPUHeap(sizeof(raytracing::RaysIndirectControl), 16);
    auto [blocks_memory, blocks_offset] = allocator_->AllocateGPUHeap(blocks_bytes, 64);
    const auto grids_offset = std::get<1>(allocator_->AllocateGPUHeap(grids_bytes, 16));
    BeginPredicationComputePass(rays_indirect_pipeline_, 64);
    const uint64_t blocks_address = allocator_->GPUHeapAddress(blocks_offset);
    const uint64_t grids_address = allocator_->GPUHeapAddress(grids_offset);
    for (UINT i = 0; i < MaxCommandCount; ++i) {
      auto *arguments = reinterpret_cast<SM50_RT_DISPATCH_ARGUMENTS *>(static_cast<char *>(blocks_memory) + i * kBlockBytes);
      memset(arguments, 0, sizeof(*arguments));
      FillRaytracingDispatchConstants(*arguments, *state_object, blocks_address + i * kBlockBytes);
    }
    auto *control = new (control_memory) raytracing::RaysIndirectControl{};
    control->arguments = ArgBufferAddress + sig->DrawArgumentOffset;
    control->count = CountBufferAddress;
    control->predicate = predication_enabled_ ? allocator_->GPUHeapAddress(predication_snapshot_offset_) : 0;
    control->dispatch_blocks = blocks_address;
    control->grids = grids_address;
    control->stride = sig->ByteStride;
    control->max_commands = MaxCommandCount;
    control->arguments_stride = static_cast<uint32_t>(kBlockBytes);
    control->predicate_not_equal = predication_op_ == D3D12_PREDICATION_OP_NOT_EQUAL_ZERO;
    /* The slot dimensions are GPU data, so every slot takes the 2D tile; the resolve kernel divides each
     * slot's dimensions by it. */
    const WMTSize group = RayGenerationThreadgroup(*state_object, 0, 0);
    control->group_width = uint32_t(group.width);
    control->group_height = uint32_t(group.height);

    BindGPUHeapPredicationBuffer(control_offset, 0);
    auto &resolve = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    resolve.type = WMTComputeCommandDispatch;
    resolve.size = {(uint64_t(MaxCommandCount) + 63) / 64, 1, 1};
    uint64_t encoder_id = allocator_->encoder_current->id;
    allocator_->InvalidateCurrentPass();

    const uint64_t roots_stride = dispatch_rootsig ? uint64_t(dispatch_rootsig->UploadQwords) * 8 : 0;
    for (UINT i = 0; i < MaxCommandCount; ++i) {
      auto *traversal = EncodeRaytracingTraversal(blocks_offset + i * kBlockBytes, {}, group, grids_offset + uint64_t(i) * 16,
                                                 roots_offset == ~0ull ? ~0ull : roots_offset + uint64_t(i) * roots_stride);
      if (!traversal)
        return;
      // An atomic phase closes the current pass; the recorded traversal ID remains valid.
      encoder_id = traversal->id;
    }
    // The updated root parameters are zeroed afterwards, as for the other argument-changing signatures.
    ResetIndirectState(sig, rootsig_compute_.ptr(), rootarg_compute_staging_, true);
    owned_graphics::Trace(
        "dispatch_rays_indirect", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
        ",\"encoder\":", encoder_id, ",\"max_commands\":", MaxCommandCount,
        ",\"count_buffer\":", CountBufferAddress ? 1 : 0, ",\"stride\":", sig->ByteStride,
        ",\"exports\":", static_cast<uint32_t>(state_object->GetExports().size())
    );
  }

  bool EnsureRaysIndirectPipeline() {
    if (rays_indirect_pipeline_)
      return true;
    /* The kernel writes SM50_RT_DISPATCH_ARGUMENTS fields at offsets taken from the struct itself. */
    std::string source;
    auto define = [&](const char *name, size_t offset) {
      source += "#define ";
      source += name;
      source += ' ';
      source += std::to_string(offset);
      source += "u\n";
    };
    define("D3DALLOY_RAYGEN_RECORD", offsetof(SM50_RT_DISPATCH_ARGUMENTS, RayGenerationRecord));
    define("D3DALLOY_MISS_BASE", offsetof(SM50_RT_DISPATCH_ARGUMENTS, MissRecordBase));
    define("D3DALLOY_HIT_BASE", offsetof(SM50_RT_DISPATCH_ARGUMENTS, HitGroupRecordBase));
    define("D3DALLOY_CALLABLE_BASE", offsetof(SM50_RT_DISPATCH_ARGUMENTS, CallableRecordBase));
    define("D3DALLOY_MISS_STRIDE", offsetof(SM50_RT_DISPATCH_ARGUMENTS, MissRecordStride));
    define("D3DALLOY_MISS_COUNT", offsetof(SM50_RT_DISPATCH_ARGUMENTS, MissRecordCount));
    define("D3DALLOY_HIT_STRIDE", offsetof(SM50_RT_DISPATCH_ARGUMENTS, HitGroupRecordStride));
    define("D3DALLOY_HIT_COUNT", offsetof(SM50_RT_DISPATCH_ARGUMENTS, HitGroupRecordCount));
    define("D3DALLOY_CALLABLE_STRIDE", offsetof(SM50_RT_DISPATCH_ARGUMENTS, CallableRecordStride));
    define("D3DALLOY_CALLABLE_COUNT", offsetof(SM50_RT_DISPATCH_ARGUMENTS, CallableRecordCount));
    define("D3DALLOY_WIDTH", offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchWidth));
    define("D3DALLOY_HEIGHT", offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchHeight));
    define("D3DALLOY_DEPTH", offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchDepth));
    define("D3DALLOY_SELF", offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchArgumentsAddress));
    source += raytracing::kRaysIndirectMSL;
    WMT::Reference<WMT::Error> error;
    auto library = device_->GetMTLDevice().newLibraryWithSource(source, error);
    if (!library) {
      ERR("ExecuteIndirect: DispatchRays resolve library failed: ",
          error ? error.description().getUTF8String() : "no Metal error");
      return false;
    }
    auto function = WMT4::Library(library).newFunction(raytracing::kRaysIndirectFunction);
    if (!function)
      return false;
    rays_indirect_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(function, error);
    return bool(rays_indirect_pipeline_);
  }

  /*
   * The heap a raytracing dispatch stages: per dispatch one SM50_RT_DISPATCH_ARGUMENTS block (and, for an
   * indirect one, its 12 byte threadgroup grid), plus the dispatch signature's root arguments and static samplers.
   */
  bool CanStageRaytracingDispatch(uint64_t dispatches) {
    auto *dispatch_rootsig = static_cast<MTLD3D12StateObject *>(rt_state_object_.ptr())->GetDispatchRootSignature();
    uint64_t root_bytes = 0;
    if (dispatch_rootsig)
      root_bytes = uint64_t(dispatch_rootsig->UploadQwords) * sizeof(uint64_t) + 64 +
                   uint64_t(dispatch_rootsig->NumStaticSamplers) * 8 * sizeof(uint64_t) + 64;
    const uint64_t per_dispatch = sizeof(SM50_RT_DISPATCH_ARGUMENTS) + 64 + 16;
    const uint64_t maximum = std::min<uint64_t>(SIZE_MAX, device_->GetMTLDevice().maxBufferLength());
    return root_bytes + 64 <= maximum && dispatches <= (maximum - root_bytes - 64) / per_dispatch &&
           allocator_->ReserveGPUHeap(per_dispatch * dispatches + root_bytes + 64, 64);
  }

  /**
   * Threadgroup shape for a ray generation dispatch: the state object's thread count laid out along x for a
   * one-row grid, else as a near-square 2D tile so neighbouring rays - which tend to traverse alike - share a
   * SIMD group.
   */
  static WMTSize RayGenerationThreadgroup(const MTLD3D12StateObject &state_object, uint64_t height, uint64_t depth) {
    const uint64_t threads = state_object.GetRayGenerationThreadgroupThreads();
    if (height == 1 && depth == 1)
      return {threads, 1, 1};
    uint64_t width = 1;
    while (width * width < threads)
      width *= 2;
    return {width, threads / width, 1};
  }

  /** The dispatch constants every raytracing dispatch of `state_object` shares (the tables are the caller's). */
  static void FillRaytracingDispatchConstants(
      SM50_RT_DISPATCH_ARGUMENTS &arguments, MTLD3D12StateObject &state_object, uint64_t own_address
  ) {
    /*
     * The D3D12 GPU virtual address of this very block, which is what is bound at
     * SM50_RT_BINDING_INDEX_DISPATCH. A ray generation or intersection shader has it as an argument,
     * but a visible function has no bound arguments at all and still needs these constants to resolve
     * the records of a nested TraceRay or CallShader out of the tables named here.
     */
    arguments.DispatchArgumentsAddress = own_address;
    arguments.ExportCount = static_cast<uint32_t>(state_object.GetExports().size());
    arguments.PipelineToken = state_object.GetPipelineToken();
    arguments.RecursionDepth = state_object.GetPipelineConfig().MaxTraceRecursionDepth;
    arguments.PayloadSize = state_object.GetMaxPayloadSize();
    arguments.AttributeSize = state_object.GetMaxAttributeSize();
    arguments.ImmutableConstants = state_object.immutable_constants.gpu_address;
  }

  /*
   * Encode one traversal pass over the dispatch constants at `arguments_offset` in this list's heap.
   * The traversal resolves every shader record on the GPU straight out of the application's tables,
   * so a direct and an indirect dispatch differ only in where the grid comes from: `grid` threads, or
   * (when `indirect_grid_offset` is not ~0) the threadgroup counts written at that heap offset. `group` is the
   * threadgroup shape; raygen threads past the dispatch's Width/Height/Depth return in the kernel prologue.
   */
  ComputeEncoderData *EncodeRaytracingTraversal(uint64_t arguments_offset, WMTSize grid, WMTSize group,
                                               uint64_t indirect_grid_offset, uint64_t root_arguments_offset = ~0ull) {
    auto *state_object = static_cast<MTLD3D12StateObject *>(rt_state_object_.ptr());
    /*
     * The intersection function table the traversal will select from is bound per dispatch, not
     * shared with the state object: an intersection function cannot see the compute encoder's
     * argument table, so the dispatch constants it resolves the hit group with - and the visible
     * function table it calls the group's any-hit shader through - are bound in the table's own
     * buffer argument table, and those are per-dispatch values. Each dispatch of this recording
     * epoch takes its own table from the allocator, which hands a table out again only after a
     * retired Reset, so a table that is in flight is never rebound.
     */
    const auto arguments_binding = allocator_->ResolveGPUHeap(arguments_offset);
    auto &dispatch_intersection_slot = allocator_->DispatchIntersectionTableSlot(rt_pipeline_);
    if (!state_object->BindDispatchIntersectionFunctionTable(
            dispatch_intersection_slot, arguments_binding.buffer, arguments_binding.offset, rt_visible_function_table_
        )) {
      RecordError(E_FAIL);
      return nullptr;
    }
    WMT::Reference<WMT::IntersectionFunctionTable> dispatch_intersection_table = dispatch_intersection_slot;

    allocator_->InvalidateCurrentPass();
    auto *traversal = allocator_->AllocatePass<ComputeEncoderData>();
    traversal->type = EncoderType::Compute;
    traversal->cmd_head.type = WMTComputeCommandNop;
    traversal->cmd_head.next.set(0);
    traversal->cmd_tail = (wmtcmd_base *)&traversal->cmd_head;
    traversal->rt_pipeline = rt_pipeline_;
    traversal->rt_intersection_function_table = dispatch_intersection_table;
    traversal->rt_visible_function_table = rt_visible_function_table_;
    traversal->atomic64_phase = state_object->uses_atomic64;
    if (traversal->atomic64_phase) {
      auto &use = traversal->atomic64_ray.emplace();
      use.state_object = rt_state_object_.ptr();
      use.heap = descriptor_heaps_[0];
      use.arguments = arguments_binding.buffer;
      use.arguments_offset = arguments_binding.offset;
    }

    /*
     * The root arguments and static samplers of a raytracing pipeline are bound through the
     * ordinary compute root arguments, exactly as a compute dispatch binds them - but the signature
     * they come from is the state object's dispatch signature, not rootsig_compute_: the user's
     * global root signature parameters unchanged, plus the union of the global and local static
     * samplers, which is what the exports were compiled against. The application's root argument
     * *values* still come from its own SetComputeRoot* calls, at the same slot offsets. A pipeline
     * with no user global root signature has zero root argument qwords but may still have local
     * static samplers, so each binding is emitted only when it carries something.
     */
    if (auto *dispatch_rootsig = state_object->GetDispatchRootSignature()) {
      if (dispatch_rootsig->UploadQwords) {
        // An argument-changing ExecuteIndirect slot passes its own root copy.
        if (root_arguments_offset == ~0ull)
          root_arguments_offset = EncodeRootArgument(dispatch_rootsig, rootarg_compute_staging_);
        if (root_arguments_offset == UINT64_MAX)
          return nullptr;
        auto &set_root_arguments = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        set_root_arguments.type = WMTComputeCommandSetBuffer;
        allocator_->BindGPUHeap(set_root_arguments.buffer, set_root_arguments.offset, root_arguments_offset);
        set_root_arguments.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        if (traversal->atomic64_ray) {
          const auto binding = allocator_->ResolveGPUHeap(root_arguments_offset);
          traversal->atomic64_ray->roots = binding.buffer;
          traversal->atomic64_ray->roots_offset = binding.offset;
        }
      }
      if (dispatch_rootsig->NumStaticSamplers) {
        auto static_samplers_offset = EncodeStaticSamplers(dispatch_rootsig);
        if (static_samplers_offset == UINT64_MAX)
          return nullptr;
        auto &set_static_samplers = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        set_static_samplers.type = WMTComputeCommandSetBuffer;
        allocator_->BindGPUHeap(set_static_samplers.buffer, set_static_samplers.offset, static_samplers_offset);
        set_static_samplers.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
    }

    auto &set_pipeline = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    set_pipeline.type = WMTComputeCommandSetPSO;
    set_pipeline.pso = rt_pipeline_;
    set_pipeline.threadgroup_size = group;

    auto &set_arguments = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    set_arguments.type = WMTComputeCommandSetBuffer;
    allocator_->BindGPUHeap(set_arguments.buffer, set_arguments.offset, arguments_offset);
    set_arguments.index = SM50_RT_BINDING_INDEX_DISPATCH;

    if (state_object->immutable_constants.buffer) {
      const auto &constants = state_object->immutable_constants;
      auto &set_constants = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
      set_constants.type = WMTComputeCommandSetBuffer;
      set_constants.buffer = constants.buffer;
      set_constants.offset = constants.offset;
      set_constants.index = SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS;
      traversal->immutable_constant_buffers.push_back(constants.buffer);
    }
    /* The ray generation shader receives the per-dispatch table as its argument, so it is the one bound. */
    auto &set_function_table = allocator_->EncodeComputeCommand<wmtcmd_compute_setfunctiontable>();
    set_function_table.type = WMTComputeCommandSetIntersectionFunctionTable;
    set_function_table.table = dispatch_intersection_table;
    set_function_table.index = SM50_RT_BINDING_INDEX_INTERSECTION_FUNCTION_TABLE;

    /* Miss / closest-hit / any-hit / callable exports are reached through the visible function table. */
    auto &set_visible_function_table = allocator_->EncodeComputeCommand<wmtcmd_compute_setfunctiontable>();
    set_visible_function_table.type = WMTComputeCommandSetVisibleFunctionTable;
    set_visible_function_table.table = rt_visible_function_table_;
    set_visible_function_table.index = SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE;

    if (indirect_grid_offset == ~0ull) {
      auto &dispatch_rays = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch_rays.type = WMTComputeCommandDispatchThreads;
      dispatch_rays.size = grid;
    } else {
      /* d3dalloy_rays_indirect wrote the slot's threadgroup counts for `group`. */
      auto &dispatch_rays = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
      dispatch_rays.type = WMTComputeCommandDispatchIndirect;
      allocator_->BindGPUHeap(dispatch_rays.indirect_args_buffer, dispatch_rays.indirect_args_offset, indirect_grid_offset);
    }
    if (traversal->atomic64_phase)
      allocator_->InvalidateCurrentPass();
    return traversal;
  }
};

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::CreateCommandList(
    UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
    void **ppCommandList
) try {
  InitReturnPtr(ppCommandList);
  if (!ppCommandList)
    return E_POINTER;
  if (Type != type_)
    return E_INVALIDARG;

  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(device_, Type));
  HRESULT hr = cmd_list->Initialize(this, pInitialPipelineState);
  if (FAILED(hr))
    return hr;
  return cmd_list->QueryInterface(riid, ppCommandList);
} catch (const std::bad_alloc &) {
  return E_OUTOFMEMORY;
}

HRESULT
CreateClosedCommandList(
    MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandList
) try {
  InitReturnPtr(ppCommandList);
  if (!ppCommandList)
    return E_POINTER;

  switch (Type) {
  case D3D12_COMMAND_LIST_TYPE_DIRECT:
  case D3D12_COMMAND_LIST_TYPE_BUNDLE:
  case D3D12_COMMAND_LIST_TYPE_COMPUTE:
  case D3D12_COMMAND_LIST_TYPE_COPY:
    break;
  default:
    return E_INVALIDARG;
  }

  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(pDevice, Type));
  return cmd_list->QueryInterface(riid, ppCommandList);
} catch (const std::bad_alloc &) {
  return E_OUTOFMEMORY;
}

}; // namespace d3dalloy
