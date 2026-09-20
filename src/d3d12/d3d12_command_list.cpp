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
#include "d3d12_acceleration_structure.hpp"
#include "d3d12_resource_contract.hpp"
#include "d3d12_state_object.hpp"
#include "d3d12_temporal.hpp"
#include "com/com_pointer.hpp"
#include "dxil_raytracing.h"
#include "dxmt_buffer.hpp"
#include "dxmt_format.hpp"
#include <functional>
#include <new>
#include <vector>

namespace dxmt {

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
  location = {allocation->buffer(), offset, allocation->byteLength() - offset, allocation};
  return true;
}

namespace {

bool
RangeFits(UINT64 offset, UINT64 length, UINT64 total) {
  return resource_contract::RangeFits(offset, length, total);
}

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
  info.block_width = format.Flag & MTL_DXGI_FORMAT_BC ? 4u : 1u;
  info.bytes_per_block = format.BytesPerTexel;
  resource_contract::FootprintLayout layout;
  if (!resource_contract::ValidateFootprintRange(
          footprint.Width, footprint.Height, footprint.Depth, info.bytes_per_block, info.block_width,
          footprint.RowPitch, placed.Offset, buffer_desc.Width, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
          D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, layout
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
    UINT block_width, const D3D12_BOX &box, const D3D12_BOX &src_extent,
    UINT dst_x, UINT dst_y, const D3D12_BOX &dst_extent
) {
  if (block_width == 1)
    return true;
  if ((box.left % block_width) || (box.top % block_width) || (dst_x % block_width) || (dst_y % block_width))
    return false;
  if ((box.right % block_width) && box.right != src_extent.right)
    return false;
  if ((box.bottom % block_width) && box.bottom != src_extent.bottom)
    return false;
  const UINT width = box.right - box.left;
  const UINT height = box.bottom - box.top;
  if (((dst_x + width) % block_width) && dst_x + width != dst_extent.right)
    return false;
  if (((dst_y + height) % block_width) && dst_y + height != dst_extent.bottom)
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
    : public MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList, IMTLD3D12GraphicsCommandListExt> {

  Com<MTLD3D12CommandAllocatorImpl, false> allocator_;
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
    if (SUCCEEDED(recording_error_))
      recording_error_ = hr;
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

  D3D12_PRIMITIVE_TOPOLOGY topology_;

  UINT num_viewports;
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  UINT num_scissors;
  D3D12_RECT
  scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  Com<MTLD3D12GraphicsPipelineState, false> pso_graphics_;
  Com<MTLD3D12RootSignature, false> rootsig_graphics_;
  uint64_t rootarg_graphics_staging_[64];

  Com<MTLD3D12ComputePipelineState, false> pso_compute_;
  Com<MTLD3D12RootSignature, false> rootsig_compute_;
  uint64_t rootarg_compute_staging_[64];

  /* IRaytracing dispatch state, set by SetPipelineState1. */
  Com<MTLD3D12StateObject> rt_state_object_;
  WMT::Reference<WMT::ComputePipelineState> rt_pipeline_;
  WMT::Reference<WMT::IntersectionFunctionTable> rt_intersection_function_table_;
  /* The compiled traversal reaches miss/closest-hit/any-hit/callable exports
   * through this table, so a ray dispatch binds it alongside the intersection
   * function table. */
  WMT::Reference<WMT::VisibleFunctionTable> rt_visible_function_table_;

  FLOAT blend_factor_[4];
  UINT8 stencil_ref_;

  struct ActiveVisibilityQuery {
    D3D12_QUERY_TYPE type;
    UINT index;
    uint64_t heap_id;
    WMT::Reference<WMT::Buffer> destination;
    WMT::Reference<WMT::ComputePipelineState> accumulate_pipeline;
  };
  small_vector<ActiveVisibilityQuery, 4> active_visibility_queries_;

  bool QueryObjectOwnedByThisDevice(ID3D12DeviceChild *object) {
    if (!object)
      return false;
    Com<ID3D12Device> owner;
    return SUCCEEDED(object->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) &&
           owner.ptr() == static_cast<ID3D12Device *>(device_);
  }

public:
  MTLD3D12GraphicsCommandListImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type)
      : MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList, IMTLD3D12GraphicsCommandListExt>(pDevice),
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

    rootsig_graphics_ = nullptr;
    memset(rootarg_graphics_staging_, 0, sizeof(rootarg_graphics_staging_));

    rootsig_compute_ = nullptr;
    memset(rootarg_compute_staging_, 0, sizeof(rootarg_compute_staging_));

    rt_state_object_ = nullptr;
    rt_pipeline_ = {};
    rt_intersection_function_table_ = {};
    rt_visible_function_table_ = {};

    memset(vertex_buffers_.data(), 0, sizeof(vertex_buffers_));

    index_buffer_address = 0;
    index_buffer = {};
    index_type = {};
    index_offset = 0;

    dirty_state_.clrAll();
    descriptor_heaps_ = {};
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
    entry = new_entry;
    encoder_count = std::numeric_limits<size_t>::max();
    active_visibility_queries_.clear();
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
    owned_graphics::Trace("record_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IMTLD3D12GraphicsCommandListExt)) {
      *ppvObject = static_cast<IMTLD3D12GraphicsCommandListExt *>(this);
      AddRef();
      return S_OK;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
        riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4)) {
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
    return allocator_->type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
           allocator_->type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE ? S_OK : E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE TemporalUpscale(IMTLD3D12TemporalUpscaleState *state,
      const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc) final {
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
  }

  HRESULT STDMETHODCALLTYPE
  Close() {
    if (encoder_count < std::numeric_limits<size_t>::max())
      return E_FAIL;
    if (!active_visibility_queries_.empty())
      RecordError();
    auto hr = allocator_->EndRecord(&encoder_count);
    if (SUCCEEDED(hr) && FAILED(recording_error_))
      hr = recording_error_;
    close_error_ = hr;
    owned_closed_valid = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
      owned_graphics::Trace("record_close", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"encoders\":", encoder_count);
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) {
    if (bundle_recording_->unsubmitted.load(std::memory_order_acquire))
      return E_FAIL;
    if (FAILED(close_error_))
      return close_error_;
    if (encoder_count == std::numeric_limits<size_t>::max())
      return E_FAIL;
    return Initialize(pAllocator, pInitialState);
  };

  void STDMETHODCALLTYPE
  ClearState(ID3D12PipelineState *pPipelineState) {
    if (RejectBundleOperation())
      return;
    allocator_->InvalidateCurrentPass();
    ResetState(pPipelineState);
  };

  std::tuple<uint64_t, uint64_t>
  PopulateVertexBufferTable(uint32_t Count) {
    auto slot_mask = pso_graphics_ ? pso_graphics_->slot_mask : 0;
    if (!slot_mask)
      return {0, 0};
    uint32_t max_slot = 32 - __builtin_clz(slot_mask);
    struct VERTEX_BUFFER_ENTRY {
      uint64_t buffer_handle;
      uint32_t stride;
      uint32_t length;
    };
    auto stride = align(sizeof(VERTEX_BUFFER_ENTRY) * max_slot, 16);

    auto [mapped, offset] = allocator_->AllocateGPUHeap(stride * Count, 16);

    for (unsigned i = 0; i < Count; i++) {
      VERTEX_BUFFER_ENTRY *entries = (VERTEX_BUFFER_ENTRY *)(reinterpret_cast<char *>(mapped) + i * stride);
      for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
        if (!(slot_mask & (1 << slot)))
          continue;
        auto &state = vertex_buffers_[slot];
        entries[index].buffer_handle = state.BufferLocation;
        entries[index].stride = state.StrideInBytes;
        entries[index++].length = state.SizeInBytes;
      };
    }

    return {offset, stride};
  }

  DrawCallStatus
  PreDraw(bool SkipResourceBinding = false) {
    if (!pso_graphics_)
      return DrawCallStatus::Invalid;
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) {

      allocator_->InvalidateCurrentPass();
      auto render = allocator_->AllocatePass<RenderEncoderData>();
      render->type = EncoderType::Render;
      render->cmd_head.type = WMTRenderCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
      render->dsv_planar_flags = 0;
      render->dsv_readonly_flags = 0;
      render->render_target_count = num_rtvs;
      if (!active_visibility_queries_.empty()) {
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
        if (!AttachmentDesc.Texture)
          continue;
        auto &rt = render->colors[i];
        rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
        rt.depth_plane = AttachmentDesc.DepthPlane;
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
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
        break;
      }
      render->render_target_width = render_target_width;
      render->render_target_height = render_target_height;
      render->render_target_array_length = render_target_array_length;
      if (effective_rtvs == 0) {
        render->default_raster_sample_count = std::max(1u, pso_graphics_->forced_sample_count);
      }

      dirty_state_.set(DirtyState::VertexBuffer, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
      dirty_state_.set(DirtyState::BlendFactor, DirtyState::StencilRef);
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    }

    if (!pso_graphics_)
      return DrawCallStatus::Invalid;

    if (dirty_state_.test(DirtyState::GraphicsPipelineState)) {
      auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
      cmd_setpso.type = WMTRenderCommandSetPSO;
      cmd_setpso.pso = pso_graphics_->pso;
      auto &retained = static_cast<RenderEncoderData *>(allocator_->encoder_current)->immutable_constant_buffers;
      for (auto [constants, command_type] : {
             std::pair{&pso_graphics_->vertex_constants, WMTRenderCommandSetVertexBuffer},
             std::pair{&pso_graphics_->fragment_constants, WMTRenderCommandSetFragmentBuffer}}) {
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

      auto &cmd_setdsso = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
      cmd_setdsso.type = WMTRenderCommandSetDepthStencilState;
      cmd_setdsso.depth_stencil_state = pso_graphics_->GetDepthStencilState(
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_planar_flags,
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_readonly_flags
      );

      auto &cmd_setrs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      cmd_setrs.type = WMTRenderCommandSetRasterizerState;
      cmd_setrs.cull_mode = pso_graphics_->cull_mode;
      cmd_setrs.depth_clip_mode = pso_graphics_->depth_clip_mode;
      cmd_setrs.fill_mode = pso_graphics_->fill_mode;
      cmd_setrs.depth_bias = pso_graphics_->depth_bias;
      cmd_setrs.depth_bias_clamp = pso_graphics_->depth_bias_clamp;
      cmd_setrs.scole_scale = pso_graphics_->scole_scale;
      cmd_setrs.winding = pso_graphics_->winding;

      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }
    if (dirty_state_.test(DirtyState::VertexBuffer)) {
      auto [Offset, Stride] = PopulateVertexBufferTable(1);
      if (Stride) {
        auto &cmd_vsvb = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsvb.type = WMTRenderCommandSetVertexBuffer;
        cmd_vsvb.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsvb.offset = Offset;
        cmd_vsvb.index = SM50_BINDING_INDEX_VERTEX_BUFFER;
      }
      dirty_state_.clr(DirtyState::VertexBuffer);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootArguments) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto Offset = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_);
        auto &cmd_vsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsargbuf.type = WMTRenderCommandSetVertexBuffer;
        cmd_vsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsargbuf.offset = Offset;
        cmd_vsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        auto &cmd_fsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_fsargbuf.type = WMTRenderCommandSetFragmentBuffer;
        cmd_fsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_fsargbuf.offset = Offset;
        cmd_fsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        auto Offset = EncodeStaticSamplers(rootsig_graphics_.ptr());
        auto &cmd_vsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsargbuf.type = WMTRenderCommandSetVertexBuffer;
        cmd_vsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsargbuf.offset = Offset;
        cmd_vsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        auto &cmd_fsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_fsargbuf.type = WMTRenderCommandSetFragmentBuffer;
        cmd_fsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_fsargbuf.offset = Offset;
        cmd_fsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }

    if (dirty_state_.test(DirtyState::Viewport)) {
      auto metal_viewport = allocator_->AllocateCommandData<WMTViewport>(num_viewports);
      for (auto i = 0u; i < num_viewports; i++) {
        auto &viewport = viewports[i];
        metal_viewport[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                             viewport.Height,   viewport.MinDepth, viewport.MaxDepth};
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
      cmd.type = WMTRenderCommandSetViewports;
      cmd.viewports.set(metal_viewport);
      cmd.viewport_count = num_viewports;
      dirty_state_.clr(DirtyState::Viewport);
    }

    if (dirty_state_.test(DirtyState::ScissorRect)) {
      auto metal_scissors = allocator_->AllocateCommandData<WMTScissorRect>(num_viewports /* yes */);
      for (auto i = 0u; i < num_viewports; i++) {
        if (i < num_scissors) {
          auto &d3d_rect = scissors[i];
          LONG left = std::clamp(d3d_rect.left, (LONG)0, (LONG)16384);
          LONG top = std::clamp(d3d_rect.top, (LONG)0, (LONG)16384);
          LONG right = std::clamp(d3d_rect.right, left, (LONG)16384);
          LONG bottom = std::clamp(d3d_rect.bottom, top, (LONG)16384);
          metal_scissors[i] = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
        } else {
          metal_scissors[i] = {0, 0, 16384, 16384};
        }
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setscissorrects>();
      cmd.type = WMTRenderCommandSetScissorRects;
      cmd.scissor_rects.set(metal_scissors);
      cmd.rect_count = num_viewports;
      dirty_state_.clr(DirtyState::ScissorRect);
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
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref>();
      cmd.type = WMTRenderCommandSetStencilRef;
      cmd.stencil_ref = stencil_ref_;
      dirty_state_.clr(DirtyState::StencilRef);
    }

    return DrawCallStatus::Ordinary;
  }

  void STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::DrawInstanced, VertexCountPerInstance,
                     InstanceCount, StartVertexLocation, StartInstanceLocation))
      return;
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid)
      return;

    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
    cmd_draw.type = WMTRenderCommandDraw;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.base_instance = StartInstanceLocation;
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.vertex_start = StartVertexLocation;
    cmd_draw.vertex_count = VertexCountPerInstance;
    owned_graphics::Trace("draw", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"vertices\":", VertexCountPerInstance, ",\"instances\":", InstanceCount);
  };

  void STDMETHODCALLTYPE
  DrawIndexedInstanced(
      UINT IndexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, INT BaseVertexLocation,
      UINT StartInstanceLocation
  ) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::DrawIndexedInstanced, IndexCountPerInstance,
                     InstanceCount, StartVertexLocation, BaseVertexLocation, StartInstanceLocation))
      return;
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid)
      return;
    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd_draw.type = WMTRenderCommandDrawIndexed;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.index_type = index_type;
    cmd_draw.index_count = IndexCountPerInstance;
    cmd_draw.index_buffer = index_buffer;
    cmd_draw.index_buffer_offset = index_offset + StartVertexLocation * (index_type == WMTIndexTypeUInt32 ? 4 : 2);
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.base_vertex = BaseVertexLocation;
    cmd_draw.base_instance = StartInstanceLocation;
    owned_graphics::Trace("draw_indexed", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"indices\":", IndexCountPerInstance, ",\"instances\":", InstanceCount);
  };

  uint64_t
  EncodeRootArgument(MTLD3D12RootSignature *pRootSig, uint64_t const pStaging[64], UINT Count = 1) {
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(sizeof(uint64_t) * pRootSig->UploadQwords * Count, 64);
    for (unsigned i = 0; i < Count; i++)
      memcpy(
          reinterpret_cast<uint64_t *>(Ptr) + i * pRootSig->UploadQwords, pStaging,
          pRootSig->UploadQwords * sizeof(uint64_t)
      );
    return Offset;
  }

  uint64_t
  EncodeStaticSamplers(MTLD3D12RootSignature *pRootSig) {
    auto static_sampler_encode_size = sizeof(uint64_t) * pRootSig->NumStaticSamplers * 4;
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(static_sampler_encode_size, 64);
    memcpy(Ptr, pRootSig->EncodedStaticSamplers, static_sampler_encode_size);
    return Offset;
  }

  bool
  PreDispatch(bool SkipResourceBinding = false) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute) {
      allocator_->InvalidateCurrentPass();
      auto compute = allocator_->AllocatePass<ComputeEncoderData>();
      compute->type = EncoderType::Compute;
      compute->cmd_head.type = WMTComputeCommandNop;
      compute->cmd_head.next.set(0);
      compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
      dirty_state_.set(DirtyState::ComputePipelineState);
    }

    if (!pso_compute_)
        return false;

    if (dirty_state_.test(DirtyState::ComputePipelineState)) {
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = pso_compute_->pso;
      cmd_setpso.threadgroup_size = pso_compute_->threadgroup_size;
      const auto &constants = pso_compute_->compute_constants;
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
      dirty_state_.clr(DirtyState::ComputePipelineState);
    }

    if (dirty_state_.test(DirtyState::ComputeRootArguments) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_);
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      dirty_state_.clr(DirtyState::ComputeRootArguments);
    }

    if (dirty_state_.test(DirtyState::ComputeRootSignature) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeStaticSamplers(rootsig_compute_.ptr());
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::ComputeRootSignature);
    }

    return true;
  }

  void STDMETHODCALLTYPE
  Dispatch(UINT X, UINT Y, UINT Z) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::Dispatch, X, Y, Z))
      return;
    if (!PreDispatch())
      return;

    auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch.type = WMTComputeCommandDispatch;
    cmd_dispatch.size = {X, Y, Z};
    owned_graphics::Trace("dispatch", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"x\":", X, ",\"y\":", Y, ",\"z\":", Z);
  };

  bool
  PreBlit() {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Blit) {
      allocator_->InvalidateCurrentPass();
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
  ) {
    if (RejectBundleOperation())
      return;
    if (!pDstBuffer || !pSrcBuffer)
      return;
    const auto dst_desc = pDstBuffer->GetDesc();
    const auto src_desc = pSrcBuffer->GetDesc();
    if (dst_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || src_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !RangeFits(DstOffset, ByteCount, dst_desc.Width) || !RangeFits(SrcOffset, ByteCount, src_desc.Width) || !ByteCount)
      return;
    auto *dst_resource = static_cast<MTLD3D12Resource *>(pDstBuffer);
    auto *src_resource = static_cast<MTLD3D12Resource *>(pSrcBuffer);
    if (!dst_resource->buffer || !src_resource->buffer)
      return;
    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd_cp.src = src_resource->buffer->current()->buffer();
    cmd_cp.dst = dst_resource->buffer->current()->buffer();
    cmd_cp.src_offset = SrcOffset;
    cmd_cp.dst_offset = DstOffset;
    cmd_cp.copy_length = ByteCount;
    owned_graphics::Trace("copy_buffer", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"encoder\":", allocator_->encoder_current->id,
                          ",\"src\":", static_cast<MTLD3D12Resource *>(pSrcBuffer)->owned_id,
                          ",\"dst\":", static_cast<MTLD3D12Resource *>(pDstBuffer)->owned_id,
                          ",\"bytes\":", ByteCount);
  };

  void STDMETHODCALLTYPE
  CopyTextureRegion(
      const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc,
      const D3D12_BOX *pSrcBox
  ) {
    if (RejectBundleOperation())
      return;
    if (!pDst || !pSrc || !pDst->pResource || !pSrc->pResource)
      return;
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

    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      if (!ValidatePlacedFootprint(device_->GetMTLDevice(), src_desc, pSrc->PlacedFootprint, src_footprint_info))
        return;
      src_block_width = src_footprint_info.block_width;
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
      src_block_width = src_resource_format.Flag & MTL_DXGI_FORMAT_BC ? 4u : 1u;
      full_src_box = GetResourceExtent(src_desc, src_level);
    }

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      if (!ValidatePlacedFootprint(device_->GetMTLDevice(), dst_desc, pDst->PlacedFootprint, dst_footprint_info))
        return;
      dst_block_width = dst_footprint_info.block_width;
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
      dst_block_width = dst_resource_format.Flag & MTL_DXGI_FORMAT_BC ? 4u : 1u;
      dst_extent = GetResourceExtent(dst_desc, dst_level);
    }

    /* The block-alignment rule lets a box end on a partial block when that end
     * is the logical edge of the region being copied. A texture location's edge
     * is its mip extent; a buffer location's declared extent is padded to the
     * copy pitch, so its edge is the end of the copy region instead. */
    D3D12_BOX src_edge_extent = full_src_box;
    D3D12_BOX dst_edge_extent = dst_extent;
    if (pSrcBox) {
      src_box = *pSrcBox;
    } else if (
        pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
    ) {
      /* A null source box copies the region the two locations share: it starts
       * at the source origin and ends where either location ends. A placed
       * footprint declares the extent GetCopyableFootprints padded to the copy
       * pitch, so a block-compressed tail mip has a footprint extent (4x4) wider
       * than the mip it holds (2x2). Copying that padded extent would reject a
       * valid tail copy and ask Metal for more texels than the destination level
       * holds. */
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
        !BlockCopyAligned(src_block_width, src_box, src_edge_extent, DstX, DstY, dst_edge_extent) ||
        src_block_width != dst_block_width ||
        (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
         pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
         src_footprint_info.bytes_per_block != dst_footprint_info.bytes_per_block))
      return;
    if (pSrc->pResource == pDst->pResource) {
      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
          pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
          pSrc->SubresourceIndex == pDst->SubresourceIndex)
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

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        cmd_cp.src = src->current()->buffer();
        cmd_cp.src_offset = pSrc->PlacedFootprint.Offset + (src_box.left / block_width) * src_format.BytesPerTexel +
                            (src_box.top / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch + src_box.front * src_depth_pitch;
        cmd_cp.bytes_per_row = pSrc->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = src_depth_pitch;
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.level = dst_level;
        cmd_cp.slice = dst_slice;
        cmd_cp.options = (dst_planar_count > 1)
                             ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        cmd_cp.origin = {DstX, DstY, DstZ};
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

        // copy between depth-stencil texture is tricky
        if (dst_planar_count > 1 || src_planar_count > 1) {
          // in this path, one/both of dst/src would be depth-stencil texture

          if (dst_planar_count > 1 && src_planar_count > 1 && dst_planar != src_planar) {
            WARN("CopyTextureRegion: unmatched planar"); // just in case
            return;
          }

          auto texel_size = (dst_planar == 1 || src_planar == 1) ? 1 : 4;
          auto width = src_box.right - src_box.left;
          auto height = src_box.bottom - src_box.top;
          auto bytes_per_row = align(width * texel_size, 256);
          auto bytes_per_image = bytes_per_row * height;

          auto [temp_buffer, temp_buffer_offset] = allocator_->AllocateTempBuffer(bytes_per_image, 256);

          auto &cmd_to_tmp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          cmd_to_tmp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          cmd_to_tmp.src = src->current()->texture();
          cmd_to_tmp.level = src_level;
          cmd_to_tmp.slice = src_slice;
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
          cmd_to_tex.dst = dst->current()->texture();
          cmd_to_tex.level = dst_level;
          cmd_to_tex.slice = dst_slice;
          cmd_to_tex.origin = {DstX, DstY, DstZ};
          cmd_to_tex.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
          cmd_to_tex.options = (dst_planar_count > 1) ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;
          return;
        }

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToTexture;
        cmd_cp.src = src->current()->texture();
        cmd_cp.src_level = src_level;
        cmd_cp.src_slice = src_slice;
        cmd_cp.src_origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.src_size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.dst_level = dst_level;
        cmd_cp.dst_slice = dst_slice;
        cmd_cp.dst_origin = {DstX, DstY, DstZ};
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

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        cmd_cp.src = src->current()->texture();
        cmd_cp.level = src_level;
        cmd_cp.slice = src_slice;
        cmd_cp.origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->buffer();
        cmd_cp.offset = pDst->PlacedFootprint.Offset + (DstX / block_width) * dst_format.BytesPerTexel +
                        (DstY / block_width) * pDst->PlacedFootprint.Footprint.RowPitch + DstZ * dst_depth_pitch;
        cmd_cp.bytes_per_row = pDst->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = dst_depth_pitch;
        cmd_cp.options = (src_planar_count > 1)
                             ? (src_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
      } else {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer;
        if (!src)
          return;

        const UINT block_width = src_footprint_info.block_width;
        const UINT64 row_bytes =
            ((static_cast<UINT64>(src_box.right - src_box.left) + block_width - 1) / block_width) *
            src_footprint_info.bytes_per_block;
        const UINT64 copy_rows =
            (static_cast<UINT64>(src_box.bottom - src_box.top) + block_width - 1) / block_width;
        const UINT64 copy_depth = src_box.back - src_box.front;
        const UINT64 src_row_start = src_box.top / block_width;
        const UINT64 dst_row_start = DstY / block_width;
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
  };

  void STDMETHODCALLTYPE
  CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) {
    if (RejectBundleOperation())
      return;
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);
    if (!pDst || !pSrc || (pDst == pSrc))
      return;

    owned_graphics::Trace("copy_resource", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"src\":", pSrc->owned_id, ",\"dst\":", pDst->owned_id);

    auto DstDesc = pDst->GetDesc();
    auto SrcDesc = pSrc->GetDesc();
    if (DstDesc.Dimension != SrcDesc.Dimension)
      return;

    if (DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (DstDesc.Width != SrcDesc.Width || !pDst->buffer || !pSrc->buffer)
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
      return;
    }

    if (!pDst->texture || !pSrc->texture ||
        DstDesc.Width != SrcDesc.Width || DstDesc.Height != SrcDesc.Height ||
        DstDesc.DepthOrArraySize != SrcDesc.DepthOrArraySize || DstDesc.MipLevels != SrcDesc.MipLevels ||
        DstDesc.SampleDesc.Count != SrcDesc.SampleDesc.Count || DstDesc.SampleDesc.Quality != SrcDesc.SampleDesc.Quality)
      return;

    // TODO: handle reinterpret copy
    if (pDst->texture->pixelFormat() != pSrc->texture->pixelFormat()) {
      WARN("CopyResource: TODO: reinterpret copy");
      return;
    }

    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_texture>();
    cmd_cp.type = WMTBlitCommandCopyTexture;
    cmd_cp.src = pSrc->texture->current()->texture();
    cmd_cp.dst = pDst->texture->current()->texture();
  };

  void STDMETHODCALLTYPE CopyTiles(
      ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset,
      D3D12_TILE_COPY_FLAGS flags
  ) {
    if (RejectBundleOperation())
      return;
    IMPLEMENT_ME
  };

  void STDMETHODCALLTYPE ResolveSubresource(
      ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource,
      DXGI_FORMAT Format
  ) {
    if (RejectBundleOperation())
      return;
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);

    if (!pDst->texture || !pSrc->texture)
      return;

    auto DstMips = pDst->texture->miplevelCount();
    auto DstLevel = DstSubresource % DstMips;
    auto DstSlice = DstSubresource / DstMips;

    allocator_->InvalidateCurrentPass();
    auto resolve = allocator_->AllocatePass<ResolveEncoderData>();
    resolve->type = EncoderType::Resolve;

    MTL_DXGI_FORMAT_DESC format_desc;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc))) {
      ERR("ResolveSubresource: invalid format ", Format);
      return;
    }
    {
      auto format = format_desc.PixelFormat;
      TextureViewDescriptor src_desc;
      auto &src = pSrc->texture;
      auto &dst = pDst->texture;
      src_desc.format = format;
      src_desc.type = src->textureType();
      src_desc.arraySize = 1;
      src_desc.firstArraySlice = SrcSubresource; // src must be a MS(Array) texture which has exactly 1 mipmap level
      src_desc.miplevelCount = 1;
      src_desc.firstMiplevel = 0;

      TextureViewDescriptor dst_desc;
      dst_desc.format = format;
      dst_desc.type = WMTTextureType2D;
      dst_desc.arraySize = 1;
      dst_desc.firstArraySlice = DstSlice;
      dst_desc.miplevelCount = 1;
      dst_desc.firstMiplevel = DstLevel;

      auto src_view = src->createView(src_desc);
      auto dst_view = dst->createView(dst_desc);

      resolve->src = src->view(src_view);
      resolve->dst = dst->view(dst_view);
    }
    allocator_->InvalidateCurrentPass();
  };

  void STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::IASetPrimitiveTopology, Topology))
      return;
    topology_ = Topology;
  };

  void STDMETHODCALLTYPE
  RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) {
    if (RejectBundleOperation())
      return;
    if (NumViewports > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    num_viewports = NumViewports;
    for (auto i = 0u; i < NumViewports; i++) {
      viewports[i] = pViewports[i];
    }
    dirty_state_.set(DirtyState::Viewport);
  };

  void STDMETHODCALLTYPE
  RSSetScissorRects(UINT NumRects, const D3D12_RECT *rects) {
    if (RejectBundleOperation())
      return;
    if (NumRects > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    num_scissors = NumRects;
    for (auto i = 0u; i < NumRects; i++) {
      scissors[i] = rects[i];
    }
    dirty_state_.set(DirtyState::ScissorRect);
  };

  void STDMETHODCALLTYPE
  OMSetBlendFactor(const FLOAT BlendFactors[4]) {
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      std::array<FLOAT, 4> values = {1.0f, 1.0f, 1.0f, 1.0f};
      if (BlendFactors)
        std::copy_n(BlendFactors, values.size(), values.begin());
      AppendBundleCommand([values](auto &parent) { parent.OMSetBlendFactor(values.data()); });
      return;
    }
    if (BlendFactors) {
      memcpy(blend_factor_, BlendFactors, std::size(blend_factor_) * sizeof(blend_factor_[0]));
    } else {
      blend_factor_[0] = 1.0f;
      blend_factor_[1] = 1.0f;
      blend_factor_[2] = 1.0f;
      blend_factor_[3] = 1.0f;
    }
    dirty_state_.set(DirtyState::BlendFactor);
  };

  void STDMETHODCALLTYPE
  OMSetStencilRef(UINT StencilRef) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::OMSetStencilRef, StencilRef))
      return;
    if (stencil_ref_ == (UINT8)StencilRef)
      return;
    stencil_ref_ = (UINT8)StencilRef;
    dirty_state_.set(DirtyState::StencilRef);
  };

  void STDMETHODCALLTYPE
  SetPipelineState(ID3D12PipelineState *pPSO) {
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
    if (pso_graphics_.ptr() == graphics_pso)
      return;
    pso_graphics_ = graphics_pso;
    pso_compute_ = nullptr;
    dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState, DirtyState::VertexBuffer);
  };

  void STDMETHODCALLTYPE ResourceBarrier(UINT Count, const D3D12_RESOURCE_BARRIER *barriers) {
    if (RejectBundleOperation())
      return;
    if (!Count)
      return;
    if (!barriers) {
      ERR("ResourceBarrier: nonzero count with null barriers");
      return;
    }
    // Resources use untracked Metal storage. Consecutive commands in the same
    // encoder therefore cannot ignore a UAV or aliasing dependency. A pass
    // boundary makes the queue emit updateFence/endEncoding followed by the
    // next encoder's waitForFence. This deliberately over-synchronizes valid
    // legacy transition/UAV/aliasing barriers without a CPU or queue-idle wait.
    // Split barriers may complete early; END_ONLY also establishes a boundary.
    // This is not a resource-state validator or enhanced-barrier support.
    for (UINT i = 0; owned_graphics::TraceEnabled() && i < Count; ++i) {
      const auto &barrier = barriers[i];
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        auto resource = static_cast<MTLD3D12Resource *>(barrier.Transition.pResource);
        owned_graphics::Trace("transition", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                              ",\"resource\":", resource ? resource->owned_id : 0,
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
    allocator_->InvalidateCurrentPass();
  };

  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *CommandList) {
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
    for (const auto &command : bundle->bundle_commands_) {
      command(*this);
      if (FAILED(recording_error_))
        break;
    }
    owned_graphics::Trace("execute_bundle", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"bundle\":", bundle->owned_id, ",\"bundle_recording\":", bundle->owned_recording,
                          ",\"commands\":", bundle->bundle_commands_.size());
  };

  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT HeapCount, ID3D12DescriptorHeap *const *Heaps) {
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
    descriptor_heaps_ = heaps;
  };

  void STDMETHODCALLTYPE
  SetComputeRootSignature(ID3D12RootSignature *pRootSignature) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootSignature, pRootSignature))
      return;
    if (rootsig_compute_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_compute_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_compute_->UploadQwords < std::size(rootarg_compute_staging_));
    } else {
      rootsig_compute_ = nullptr;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootSignature, pRootSignature))
      return;
    if (rootsig_graphics_.ptr() == pRootSignature)
      return;
    if (pRootSignature) {
      rootsig_graphics_ = static_cast<MTLD3D12RootSignature *>(pRootSignature);
      assert(rootsig_graphics_->UploadQwords < std::size(rootarg_graphics_staging_));
    } else {
      rootsig_graphics_ = nullptr;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
  };

  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE && !descriptor_heaps_[0] && !descriptor_heaps_[1]) {
      RecordError();
      return;
    }
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootDescriptorTable, Index, BaseDescriptor))
      return;
    if (!rootsig_compute_)
      return;
    if (Index > rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = BaseDescriptor.ptr;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE && !descriptor_heaps_[0] && !descriptor_heaps_[1]) {
      RecordError();
      return;
    }
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootDescriptorTable, Index, BaseDescriptor))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index > rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = BaseDescriptor.ptr;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRoot32BitConstant, Index, Data, DstOffset))
      return;
    if (!rootsig_compute_)
      return;
    if (Index > rootsig_compute_->ParameterSlots)
      return;
    auto dst = reinterpret_cast<uint32_t *>(rootarg_compute_staging_ + rootsig_compute_->SlotQwordOffsets[Index]);
    dst[DstOffset] = Data;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRoot32BitConstant, Index, Data, DstOffset))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index > rootsig_graphics_->ParameterSlots)
      return;
    auto dst = reinterpret_cast<uint32_t *>(rootarg_graphics_staging_ + rootsig_graphics_->SlotQwordOffsets[Index]);
    dst[DstOffset] = Data;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE
  SetComputeRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) {
    if (RecordBundleConstants(false, Index, ConstantCount, pData, DstOffset))
      return;
    if (!rootsig_compute_)
      return;
    if (Index > rootsig_compute_->ParameterSlots)
      return;
    auto src = reinterpret_cast<const uint32_t *>(pData);
    auto dst = reinterpret_cast<uint32_t *>(rootarg_compute_staging_ + rootsig_compute_->SlotQwordOffsets[Index]);
    for (unsigned i = 0; i < ConstantCount; i++) {
      dst[i + DstOffset] = src[i];
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRoot32BitConstants(UINT Index, UINT ConstantCount, const void *pData, UINT DstOffset) {
    if (RecordBundleConstants(true, Index, ConstantCount, pData, DstOffset))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index > rootsig_graphics_->ParameterSlots)
      return;
    auto src = reinterpret_cast<const uint32_t *>(pData);
    auto dst = reinterpret_cast<uint32_t *>(rootarg_graphics_staging_ + rootsig_graphics_->SlotQwordOffsets[Index]);
    for (unsigned i = 0; i < ConstantCount; i++) {
      dst[i + DstOffset] = src[i];
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootConstantBufferView, Index, VA))
      return;
    if (!rootsig_compute_)
      return;
    if (Index > rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootConstantBufferView, Index, VA))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index > rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootShaderResourceView, Index, VA))
      return;
    if (!rootsig_compute_)
      return;
    if (Index > rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootShaderResourceView, Index, VA))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index > rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetComputeRootUnorderedAccessView, Index, VA))
      return;
    if (!rootsig_compute_)
      return;
    if (Index > rootsig_compute_->ParameterSlots)
      return;
    rootarg_compute_staging_[rootsig_compute_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::ComputeRootArguments);
  };

  void STDMETHODCALLTYPE
  SetGraphicsRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetGraphicsRootUnorderedAccessView, Index, VA))
      return;
    if (!rootsig_graphics_)
      return;
    if (Index > rootsig_graphics_->ParameterSlots)
      return;
    rootarg_graphics_staging_[rootsig_graphics_->SlotQwordOffsets[Index]] = VA;
    dirty_state_.set(DirtyState::GraphicsRootArguments);
  };

  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) {
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
  };

  void STDMETHODCALLTYPE
  IASetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW *Views) {
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
    for (unsigned Slot = StartSlot; Slot < StartSlot + Count; Slot++) {
      vertex_buffers_[Slot] = Views ? Views[Slot - StartSlot] : D3D12_VERTEX_BUFFER_VIEW{};
    }
    dirty_state_.set(DirtyState::VertexBuffer);
  };

  void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT Count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *Views) {
    if (RejectBundleOperation())
      return;
    IMPLEMENT_ME
  };

  void STDMETHODCALLTYPE
  OMSetRenderTargets(
      UINT NumRTV, const D3D12_CPU_DESCRIPTOR_HANDLE *RTVs, WINBOOL SingleDescriptor,
      const D3D12_CPU_DESCRIPTOR_HANDLE *DSV
  ) {
    if (RejectBundleOperation())
      return;
    allocator_->InvalidateCurrentPass();

    num_rtvs = NumRTV;
    for (unsigned i = 0; i < NumRTV; i++) {
      auto RTV = SingleDescriptor ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * 32 /* kRTVDSVHeapIncrementalSize */}
                                  : RTVs[i];
      rtvs[i] = RTV;
    }
    dsv = DSV ? *DSV : D3D12_CPU_DESCRIPTOR_HANDLE();
  };

  void STDMETHODCALLTYPE
  ClearDepthStencilView(
      D3D12_CPU_DESCRIPTOR_HANDLE DSV, D3D12_CLEAR_FLAGS Flags, FLOAT Depth, UINT8 Stencil, UINT RectCount,
      const D3D12_RECT *Rects
  ) {
    if (RejectBundleOperation())
      return;
    auto [Heap, Index] = GetRenderTargetHeap(device_, DSV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    auto CheckedFlags = Flags & DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
    if (!CheckedFlags)
      return;
    if (Rects) {
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, 0, CheckedFlags);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, Depth, Stencil);
      }
      allocator_->clear_rtv_.end();
      return;
    }
    allocator_->InvalidateCurrentPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = CheckedFlags;
    encoder_info->depth_stencil = {Depth, Stencil};
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = 0;

    allocator_->InvalidateCurrentPass();
  };

  void STDMETHODCALLTYPE
  ClearRenderTargetView(
      D3D12_CPU_DESCRIPTOR_HANDLE RTV, const FLOAT Color[4], UINT RectCount, const D3D12_RECT *Rects
  ) {
    if (RejectBundleOperation())
      return;
    auto [Heap, Index] = GetRenderTargetHeap(device_, RTV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    if (Rects) {
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, AttachmentDesc.DepthPlane);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(
            rect_offset_x, rect_offset_y, rect_width, rect_height, AttachmentDesc.RenderTargetArrayLength,
            {Color[0], Color[1], Color[2], Color[3]}
        );
      }
      allocator_->clear_rtv_.end();
      return;
    }
    allocator_->InvalidateCurrentPass();
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

    allocator_->InvalidateCurrentPass();
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const UINT Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) {
    if (RejectBundleOperation())
      return;
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view),
          (LONG)Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
    }

    allocator_->clear_uav_.end();
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const float Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) {
    if (RejectBundleOperation())
      return;
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.SRVTexture.texture, Descriptor.SRVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.SRVTexture.texture->width(Descriptor.SRVTexture.view),
          (LONG)Descriptor.SRVTexture.texture->height(Descriptor.SRVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
    }

    allocator_->clear_uav_.end();
  };

  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) {
    if (RejectBundleOperation())
      return;
    // do nothing for now
  };

  void STDMETHODCALLTYPE
  BeginQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (RejectBundleOperation())
      return;
    if (!IsRecording())
      return;
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

    allocator_->InvalidateCurrentPass();
    auto reset = allocator_->AllocatePass<QueryEncoderData>();
    reset->type = EncoderType::Query;
    reset->op = QueryEncoderOp::ResetOcclusion;
    reset->source = heap->visibility_results;
    reset->start_index = Index;
    reset->query_count = 1;
    allocator_->InvalidateCurrentPass();
    active_visibility_queries_.push_back(ActiveVisibilityQuery{
        Type, Index, heap->owned_id, heap->visibility_results, heap->visibility_accumulate_pipeline});
    owned_graphics::Trace("query_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type), ",\"index\":", Index,
                          ",\"active\":", active_visibility_queries_.size());
  };

  void STDMETHODCALLTYPE
  EndQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    if (RejectBundleOperation())
      return;
    if (!IsRecording())
      return;
    if (!QueryObjectOwnedByThisDevice(pHeap)) {
      RecordError();
      return;
    }
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      if ((type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
          heap->desc.Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP || Index >= heap->desc.Count ||
          !heap->timestamp_samples || !heap->timestamp_dummy) {
        RecordError();
        return;
      }
      allocator_->InvalidateCurrentPass();
      auto sample = allocator_->AllocatePass<QueryEncoderData>();
      sample->type = EncoderType::Query;
      sample->op = QueryEncoderOp::SampleTimestamp;
      sample->timestamp_samples = heap->timestamp_samples;
      sample->timestamp_producers = heap->timestamp_producers;
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
      allocator_->InvalidateCurrentPass();
      active_visibility_queries_.erase(it);
      owned_graphics::Trace("query_end", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                            ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type), ",\"index\":", Index,
                            ",\"active\":", active_visibility_queries_.size());
      return;
    }
    RecordError();
  };

  void STDMETHODCALLTYPE
  ResolveQueryData(
      ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT QueryCount, ID3D12Resource *pDstBuffer,
      UINT64 AlignedDstBufferOffset
  ) {
    if (RejectBundleOperation())
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
    if (StartIndex > heap->desc.Count || QueryCount > heap->desc.Count - StartIndex ||
        (AlignedDstBufferOffset & 7) ||
        (timestamp && (heap->desc.Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP ||
                       (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE))) ||
        (occlusion && (heap->desc.Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION ||
                       type_ != D3D12_COMMAND_LIST_TYPE_DIRECT)) || (!timestamp && !occlusion)) {
      RecordError();
      return;
    }
    auto destination = static_cast<MTLD3D12Resource *>(pDstBuffer);
    auto desc = destination->GetDesc();
    const UINT64 bytes = UINT64(QueryCount) * sizeof(uint64_t);
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
    allocator_->InvalidateCurrentPass();
    auto resolve = allocator_->AllocatePass<QueryEncoderData>();
    resolve->type = EncoderType::Query;
    resolve->op = timestamp ? QueryEncoderOp::ResolveTimestamp
        : Type == D3D12_QUERY_TYPE_BINARY_OCCLUSION ? QueryEncoderOp::ResolveBinaryOcclusion
                                                   : QueryEncoderOp::ResolveOcclusion;
    resolve->timestamp_samples = heap->timestamp_samples;
    resolve->timestamp_producers = heap->timestamp_producers;
    resolve->source = heap->visibility_results;
    resolve->destination = destination->buffer->current()->buffer();
    resolve->compute_pipeline = heap->binary_normalize_pipeline;
    resolve->start_index = StartIndex;
    resolve->query_count = QueryCount;
    resolve->destination_offset = AlignedDstBufferOffset;
    allocator_->InvalidateCurrentPass();
    owned_graphics::Trace("query_resolve", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"heap\":", heap->owned_id, ",\"type\":", uint32_t(Type),
                          ",\"start\":", StartIndex, ",\"count\":", QueryCount,
                          ",\"dst\":", destination->owned_id, ",\"offset\":", AlignedDstBufferOffset);
  };

  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Op) {
    if (RejectBundleOperation())
      return;
    IMPLEMENT_ME
  };

  void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *data, UINT size) {
    // The existing diagnostic records metadata and length, not marker payloads.
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::SetMarker, Metadata, static_cast<const void *>(nullptr), size))
      return;
    owned_graphics::Trace("marker", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"metadata\":", Metadata, ",\"bytes\":", size);
  };

  void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *data, UINT size) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::BeginEvent, Metadata, static_cast<const void *>(nullptr), size))
      return;
    owned_graphics::Trace("event_begin", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
                          ",\"metadata\":", Metadata, ",\"bytes\":", size);
  };

  void STDMETHODCALLTYPE EndEvent() {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::EndEvent))
      return;
    owned_graphics::Trace("event_end", ",\"list\":", owned_id, ",\"recording\":", owned_recording);
  };

  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgBuffer,
      UINT64 ArgBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset
  ) {
    auto sig = static_cast<MTLD3D12CommandSignature *>(pCommandSignature);
    if (!sig) {
      RecordError();
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
    auto arg_buffer = static_cast<MTLD3D12Resource *>(pArgBuffer);
    if (!arg_buffer || !arg_buffer->buffer)
      return;
    auto ArgBufferAddress = arg_buffer->buffer->current()->gpuAddress() + ArgBufferOffset;
    uint64_t CountBufferAddress = 0;
    if (auto count_buffer = static_cast<MTLD3D12Resource *>(pCountBuffer)) {
      if (!count_buffer->buffer)
        return;
      CountBufferAddress = count_buffer->buffer->current()->gpuAddress() + CountBufferOffset;
    }
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
      if (!PreDispatch(sig->UpdateRootArguments))
        return;

      auto cmd = allocator_->EncodeIndirectComputeCommand(sig, pso_compute_.ptr(), MaxCommandCount);
      if (!cmd) {
        RecordError(E_FAIL);
        return;
      }
      cmd->max_count_buffer = CountBufferAddress;
      cmd->argument_buffer = ArgBufferAddress;

      if (sig->UpdateRootArguments) {
        cmd->rootsig_qwords = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_, MaxCommandCount);
        cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
        cmd->rootsig_qwords_stride = rootsig_compute_->UploadQwords;
        cmd->static_samplers = EncodeStaticSamplers(rootsig_compute_.ptr());
        cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
      }

      return;
    }
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count))
      return;
    bool encode_binding = sig->UpdateRootArguments || sig->UpdateIndexBuffer || sig->UpdateVertexBuffers;
    DrawCallStatus status = PreDraw(encode_binding);
    if (status == DrawCallStatus::Invalid)
      return;
    if (status != DrawCallStatus::Ordinary) {
      IMPLEMENT_ME // TODO: (potential) emulated pipeline
    }

    auto cmd = allocator_->EncodeIndirectRenderCommand(sig, pso_graphics_.ptr(), MaxCommandCount);
    if (!cmd) {
      RecordError(E_FAIL);
      return;
    }
    cmd->max_count_buffer = CountBufferAddress;
    cmd->argument_buffer = ArgBufferAddress;
    cmd->primitive_type = primitive_type;
    cmd->index_buffer = index_buffer_address;
    cmd->index_buffer_format = index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (!encode_binding)
      return;
    cmd->rootsig_qwords = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_, MaxCommandCount);
    cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
    cmd->rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
    cmd->static_samplers = EncodeStaticSamplers(rootsig_graphics_.ptr());
    cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
    auto [VBOffset, VBStride] = PopulateVertexBufferTable(MaxCommandCount);
    cmd->vertex_buffer = allocator_->gpu_heap_buffer_address_ + VBOffset;
    cmd->vertex_argbuf_stride = VBStride;
  };

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) {
    if (RejectBundleOperation())
      return;
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT64(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) {
    if (RejectBundleOperation())
      return;
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  OMSetDepthBounds(FLOAT Min, FLOAT Max) {
    if (RecordBundle(&MTLD3D12GraphicsCommandListImpl::OMSetDepthBounds, Min, Max))
      return;
    WARN("OMSetDepthBounds: ignoring (", Min, ", ", Max, ")");
  }

  void STDMETHODCALLTYPE
  SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions) {
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      RecordError(E_NOTIMPL);
      return;
    }
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  ResolveSubresourceRegion(
      ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource,
      UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode
  ) {
    if (RejectBundleOperation())
      return;
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  SetViewInstanceMask(UINT Mask) {
    if (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      RecordError(E_NOTIMPL);
      return;
    }
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  WriteBufferImmediate(
      UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes
  ) {
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
      memcpy(memory, &pParams[i].Value, sizeof(UINT));
      auto &cmd = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd.src = allocator_->gpu_heap_buffer_;
      cmd.src_offset = source_offset;
      cmd.dst = allocation->buffer();
      cmd.dst_offset = destination_offset;
      cmd.copy_length = sizeof(UINT);
    }
    allocator_->InvalidateCurrentPass();
  }

  /* ID3D12GraphicsCommandList4 acceleration-structure commands. Everything the GPU needs is
   * resolved here and recorded as one Raytracing encoder for the queue worker to encode on the
   * application's own command buffer. */

  void STDMETHODCALLTYPE
  BuildRaytracingAccelerationStructure(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT postbuild_count,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild
  ) {
    if (!desc || (postbuild_count && !postbuild) || !IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      RecordError();
      return;
    }
    auto *device = static_cast<MTLD3D12Device *>(device_);
    raytracing::BuildInputs inputs;
    raytracing::BufferLocation destination;
    if (FAILED(raytracing::ConvertBuildInputs(desc->Inputs, ResolveRaytracingGPUVA, device, inputs)) ||
        (desc->DestAccelerationStructureData % raytracing::kAccelerationStructureAlignment) ||
        !ResolveRaytracingGPUVA(device, desc->DestAccelerationStructureData, raytracing::kHeaderSize, destination) ||
        destination.available <= raytracing::kHeaderSize) {
      RecordError();
      return;
    }
    // The application's allocation has to hold the native structure plus the header.
    raytracing::NativeSizes sizes;
    if (FAILED(device->QueryRTASSizes(inputs, sizes)) ||
        sizes.accelerationStructureSize > destination.available - raytracing::kHeaderSize) {
      RecordError();
      return;
    }
    const bool update =
        (inputs.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
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
    }
    raytracing::BufferLocation scratch;
    if ((desc->ScratchAccelerationStructureData % raytracing::kAccelerationStructureAlignment) ||
        !ResolveRaytracingGPUVA(
            device, desc->ScratchAccelerationStructureData,
            update ? sizes.refitScratchSize : sizes.buildScratchSize, scratch
        )) {
      RecordError();
      return;
    }
    auto sidecar = device->GetMTLDevice().newAccelerationStructureSidecar(
        sizes.accelerationStructureSize, inputs.instanceCount
    );
    raytracing::NativeDescriptor native;
    if (!sidecar ||
        FAILED(raytracing::CreateNativeDescriptor(
            device->GetMTLDevice(), inputs, sidecar.instanceBuffer(), native
        ))) {
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
    record->descriptor = std::move(native.descriptor);
    record->source = update ? source.sidecar : WMT::AccelerationStructureSidecar{};
    record->destination = sidecar;
    record->header = destination.buffer;
    record->scratch = scratch.buffer;
    record->sourceInstances = inputs.instances.buffer;
    record->sourceInstancesOffset = inputs.instances.offset;
    record->destinationOffset = destination.offset;
    record->scratchOffset = scratch.offset;
    record->instanceCount = inputs.instanceCount;
    record->prepassPipeline = std::move(pipelines.prepass);
    record->writeU64Pipeline = std::move(pipelines.writeU64);
    record->finalizeCompactedSizePipeline = std::move(pipelines.finalizeCompactedSize);
    /*
     * Publish the version before anything is encoded. Residency is declared here, so it covers the
     * whole window in which this structure can be read, and the token tells the device which
     * native structure the address will hold once the prepass executes.
     */
    record->destinationKey = {destination.allocationIdentity, destination.offset};
    if (!device->StoreRTAS(record->destinationKey, {sidecar, sizes.accelerationStructureSize, inputs.instanceCount},
            &record->versionToken, &record->liveMirror, &record->liveMirrorOffset)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    try {
      for (UINT i = 0; i < postbuild_count; ++i) {
        if (postbuild[i].InfoType != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE &&
            postbuild[i].InfoType != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE) {
          RecordError(E_NOTIMPL);
          return;
        }
        raytracing::BufferLocation out;
        if (!ResolveRaytracingGPUVA(device, postbuild[i].DestBuffer, sizeof(uint64_t), out)) {
          RecordError();
          return;
        }
        record->postbuild.push_back({postbuild[i].InfoType, out.buffer, out.offset});
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    allocator_->InvalidateCurrentPass();
  }

  void STDMETHODCALLTYPE
  EmitRaytracingAccelerationStructurePostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT source_count,
      const D3D12_GPU_VIRTUAL_ADDRESS *sources
  ) {
    if (!desc || !source_count || !sources || !IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
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
          !ResolveRaytracingGPUVA(device, desc->DestBuffer + i * sizeof(uint64_t), sizeof(uint64_t), out)) {
        RecordError();
        return;
      }
      allocator_->InvalidateCurrentPass();
      auto *record = allocator_->AllocatePass<RTASEncoderData>();
      record->type = EncoderType::Raytracing;
      record->operation = RTASOperation::Emit;
      record->source = source.sidecar;
      record->writeU64Pipeline = pipelines.writeU64;
      record->finalizeCompactedSizePipeline = pipelines.finalizeCompactedSize;
      record->postbuild.push_back({desc->InfoType, out.buffer, out.offset});
    }
    allocator_->InvalidateCurrentPass();
  }

  void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
      D3D12_GPU_VIRTUAL_ADDRESS destination_address, D3D12_GPU_VIRTUAL_ADDRESS source_address,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode
  ) {
    if (!IsRecording() || type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
        (mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE &&
         mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT)) {
      RecordError();
      return;
    }
    auto *device = static_cast<MTLD3D12Device *>(device_);
    raytracing::BufferLocation source_location, destination;
    RTASState source;
    if ((source_address % raytracing::kAccelerationStructureAlignment) ||
        (destination_address % raytracing::kAccelerationStructureAlignment) ||
        !ResolveRaytracingGPUVA(device, source_address, raytracing::kHeaderSize, source_location) ||
        !device->FindRTAS({source_location.allocationIdentity, source_location.offset}, source) ||
        !ResolveRaytracingGPUVA(device, destination_address, raytracing::kHeaderSize, destination) ||
        destination.available <= raytracing::kHeaderSize) {
      RecordError();
      return;
    }
    // A compacted destination may be smaller than the original AS. The
    // application sizes it from GPU postbuild information; reserve its actual
    // native capacity rather than requiring the original prebuild maximum.
    const uint64_t native_size =
        mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT
            ? std::min(source.nativeSize, destination.available - raytracing::kHeaderSize) : source.nativeSize;
    if (native_size > destination.available - raytracing::kHeaderSize) {
      RecordError();
      return;
    }
    auto sidecar = device->GetMTLDevice().newAccelerationStructureSidecar(native_size, source.instanceCount);
    RTASComputePipelines pipelines;
    if (!sidecar || FAILED(device->GetRTASComputePipelines(pipelines))) {
      RecordError(E_FAIL);
      return;
    }
    allocator_->InvalidateCurrentPass();
    auto *record = allocator_->AllocatePass<RTASEncoderData>();
    record->type = EncoderType::Raytracing;
    record->operation =
        mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE ? RTASOperation::Clone : RTASOperation::Compact;
    record->source = source.sidecar;
    record->destination = sidecar;
    record->header = destination.buffer;
    record->destinationOffset = destination.offset;
    record->instanceCount = source.instanceCount;
    record->prepassPipeline = std::move(pipelines.prepass);
    /* Same publication rule as a build: the copy publishes the version its destination will hold. */
    record->destinationKey = {destination.allocationIdentity, destination.offset};
    if (!device->StoreRTAS(record->destinationKey, {sidecar, native_size, source.instanceCount},
            &record->versionToken, &record->liveMirror, &record->liveMirrorOffset)) {
      RecordError(E_OUTOFMEMORY);
      return;
    }
    allocator_->InvalidateCurrentPass();
  }

  void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession *session) {
    // Protected resources are not implemented; passing no session is the supported case.
    if (session)
      RecordError(E_NOTIMPL);
  }

  void STDMETHODCALLTYPE BeginRenderPass(UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *, D3D12_RENDER_PASS_FLAGS) {
    RecordError(E_NOTIMPL);
  }

  void STDMETHODCALLTYPE EndRenderPass() {
    RecordError(E_NOTIMPL);
  }

  void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) {
    RecordError(E_NOTIMPL);
  }

  void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) {
    RecordError(E_NOTIMPL);
  }

  void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *pStateObject) {
    if (RejectBundleOperation())
      return;
    if (!IsRecording()) {
      RecordError(E_FAIL);
      return;
    }
    if (FAILED(recording_error_))
      return;
    if (!pStateObject) {
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
    rt_state_object_ = state_object;
    rt_pipeline_ = pipeline;
    rt_intersection_function_table_ = intersection_function_table;
    rt_visible_function_table_ = visible_function_table;
  }

  void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *desc) {
    if (RejectBundleOperation())
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

    auto *state_object = static_cast<MTLD3D12StateObject *>(rt_state_object_.ptr());
    auto prepare_pipeline = state_object->GetShaderTablePreparePipeline();
    if (!prepare_pipeline) {
      RecordError(E_FAIL);
      return;
    }

    struct PreparedTable {
      const raytracing::ShaderTable *table;
      WMT::Buffer buffer;
      uint64_t buffer_offset = 0;
      uint32_t count = 0;
      uint64_t index_address = 0;
      uint64_t entry_address = 0;
    };
    PreparedTable tables[4] = {
        {&prepared.ray_generation, {}, 0, prepared.ray_generation.record_count, 0, 0},
        {&prepared.miss, {}, 0, prepared.miss.record_count, 0, 0},
        {&prepared.hit_group, {}, 0, prepared.hit_group.record_count, 0, 0},
        {&prepared.callable, {}, 0, prepared.callable.record_count, 0, 0},
    };
    for (auto &entry : tables) {
      if (!entry.table->present())
        continue;
      uint64_t offset = 0;
      auto *allocation = device_->LookupBufferByVA(entry.table->base, &offset);
      if (!allocation || offset > allocation->byteLength() ||
          entry.table->size > allocation->byteLength() - offset) {
        RecordError();
        return;
      }
      entry.buffer = allocation->buffer();
      entry.buffer_offset = offset;
    }

    const uint32_t export_count = static_cast<uint32_t>(state_object->GetExports().size());
    const uint32_t entry_stride = state_object->GetLocalRootEntryStride();
    const auto &local_root_layouts = state_object->GetLocalRootLayouts();
    const auto &local_root_copies = state_object->GetLocalRootCopies();

    /*
     * One preparation pass per non-empty table turns the application's records into
     * the export index array and the materialized local root entries the compiled
     * traversal reads. Nothing is read back to the CPU and no record is repacked:
     * the identifier resolved on the GPU is the only thing derived from a record.
     */
    auto align16 = [](uint64_t value) { return (value + 15) & ~uint64_t(15); };
    uint64_t index_bytes = 0;
    uint64_t entry_bytes = 0;
    for (const auto &entry : tables) {
      index_bytes += align16(uint64_t(entry.count) * sizeof(uint32_t));
      entry_bytes += align16(uint64_t(entry.count) * entry_stride);
    }
    const uint64_t layout_bytes = uint64_t(local_root_layouts.size()) * sizeof(raytracing::LocalRootLayout);
    const uint64_t copy_bytes = uint64_t(local_root_copies.size()) * sizeof(raytracing::LocalRootCopy);
    const uint64_t parameters_bytes = std::size(tables) * sizeof(raytracing::SbtPrepareParameters);
    const uint64_t dispatch_arguments_bytes = sizeof(SM50_RT_DISPATCH_ARGUMENTS);
    /*
     * The root signature this dispatch actually binds is the state object's *dispatch* signature:
     * the user's global root signature parameters unchanged, plus the union of the global and local
     * static samplers. It is not GetGlobalRootSignature (the application's global signature, which
     * carries no local static samplers) and not rootsig_compute_ (the application's
     * SetComputeRootSignature signature, which is the user's global too). A pipeline may have no
     * user global root signature at all and still have local static samplers to bind, in which case
     * the dispatch signature is parameterless and exists precisely so those samplers can be bound;
     * it is the only correct source for both the staging sizes below and the bindings further down.
     */
    auto *dispatch_rootsig = state_object->GetDispatchRootSignature();
    /* The root arguments and static samplers of the pipeline are staged in the same heap, so
       their worst case has to fit as well. */
    uint64_t global_root_bytes = 0;
    if (dispatch_rootsig) {
      global_root_bytes += uint64_t(dispatch_rootsig->UploadQwords) * sizeof(uint64_t) + 64;
      global_root_bytes += uint64_t(dispatch_rootsig->NumStaticSamplers) * 4 * sizeof(uint64_t) + 64;
    }
    const uint64_t required = index_bytes + entry_bytes + layout_bytes + copy_bytes + parameters_bytes +
        dispatch_arguments_bytes + global_root_bytes;
    if (required > kGPUHeapSize || allocator_->gpu_heap_offset_ > kGPUHeapSize ||
        required > kGPUHeapSize - allocator_->gpu_heap_offset_) {
      RecordError(E_OUTOFMEMORY);
      return;
    }

    auto [index_memory, index_offset] = allocator_->AllocateGPUHeap(index_bytes, 16);
    auto [entry_memory, entry_offset] = allocator_->AllocateGPUHeap(entry_bytes, 16);
    auto [layout_memory, layout_offset] = allocator_->AllocateGPUHeap(layout_bytes, 16);
    auto [copy_memory, copy_offset] = allocator_->AllocateGPUHeap(copy_bytes, 16);
    auto [parameters_memory, parameters_offset] = allocator_->AllocateGPUHeap(parameters_bytes, 16);
    auto [arguments_memory, arguments_offset] = allocator_->AllocateGPUHeap(dispatch_arguments_bytes, 64);
    (void)index_memory;
    (void)entry_memory;
    if (layout_bytes)
      memcpy(layout_memory, local_root_layouts.data(), layout_bytes);
    if (copy_bytes)
      memcpy(copy_memory, local_root_copies.data(), copy_bytes);

    const uint64_t heap_address = allocator_->gpu_heap_buffer_address_;
    auto *parameters = static_cast<raytracing::SbtPrepareParameters *>(parameters_memory);
    uint64_t index_cursor = index_offset;
    uint64_t entry_cursor = entry_offset;
    for (size_t i = 0; i < std::size(tables); ++i) {
      auto &entry = tables[i];
      entry.index_address = heap_address + index_cursor;
      entry.entry_address = heap_address + entry_cursor;
      parameters[i] = {};
      parameters[i].record_count = entry.count;
      /* A stride of 0 means every shader index resolves to the same record. */
      parameters[i].record_stride = static_cast<uint32_t>(entry.table->stride);
      parameters[i].record_local_root_capacity = static_cast<uint32_t>(
          std::min<uint64_t>(entry.table->local_root_capacity(), std::numeric_limits<uint32_t>::max())
      );
      parameters[i].entry_stride = entry_stride;
      parameters[i].export_count = export_count;
      parameters[i].pipeline_token = state_object->GetPipelineToken();
      index_cursor += align16(uint64_t(entry.count) * sizeof(uint32_t));
      entry_cursor += align16(uint64_t(entry.count) * entry_stride);
    }

    auto *arguments = static_cast<SM50_RT_DISPATCH_ARGUMENTS *>(arguments_memory);
    memset(arguments, 0, sizeof(*arguments));
    arguments->RayGenerationRecord = prepared.ray_generation.base;
    arguments->MissRecordBase = prepared.miss.base;
    arguments->HitGroupRecordBase = prepared.hit_group.base;
    arguments->CallableRecordBase = prepared.callable.base;
    arguments->RayGenerationIndexBase = tables[0].index_address;
    arguments->MissRecordIndexBase = tables[1].count ? tables[1].index_address : 0;
    arguments->HitGroupRecordIndexBase = tables[2].count ? tables[2].index_address : 0;
    arguments->CallableRecordIndexBase = tables[3].count ? tables[3].index_address : 0;
    arguments->RayGenerationLocalRootTable = tables[0].entry_address;
    arguments->MissRecordLocalRootTableBase = tables[1].count ? tables[1].entry_address : 0;
    arguments->HitGroupRecordLocalRootTableBase = tables[2].count ? tables[2].entry_address : 0;
    arguments->CallableRecordLocalRootTableBase = tables[3].count ? tables[3].entry_address : 0;
    /*
     * The D3D12 GPU virtual address of this very buffer, which is the block bound at
     * SM50_RT_BINDING_INDEX_DISPATCH. A ray generation or intersection shader has it as an
     * argument, but a visible function has no bound arguments at all and still needs these
     * constants: a hit shader that issues a nested TraceRay has to turn the record it runs as,
     * and the record of the trace it issues, into a local root entry, and the four tables' bases
     * and strides only exist here. Publishing the address is what lets the nested trace context
     * re-derive them from the frame it was handed.
     */
    arguments->DispatchArgumentsAddress = heap_address + arguments_offset;
    arguments->RayGenerationLocalRootTableSize = entry_stride;
    arguments->MissRecordStride = static_cast<uint32_t>(prepared.miss.stride);
    arguments->MissRecordCount = tables[1].count;
    arguments->HitGroupRecordStride = static_cast<uint32_t>(prepared.hit_group.stride);
    arguments->HitGroupRecordCount = tables[2].count;
    arguments->CallableRecordStride = static_cast<uint32_t>(prepared.callable.stride);
    arguments->CallableRecordCount = tables[3].count;
    arguments->MissRecordLocalRootTableStride = entry_stride;
    arguments->HitGroupRecordLocalRootTableStride = entry_stride;
    arguments->CallableRecordLocalRootTableStride = entry_stride;
    arguments->ExportCount = export_count;
    arguments->RecursionDepth = state_object->GetPipelineConfig().MaxTraceRecursionDepth;
    arguments->PayloadSize = state_object->GetMaxPayloadSize();
    arguments->AttributeSize = state_object->GetMaxAttributeSize();
    arguments->DispatchWidth = desc->Width;
    arguments->DispatchHeight = desc->Height;
    arguments->DispatchDepth = desc->Depth;

    /*
     * The intersection function table the traversal will select from is built per dispatch, not
     * reused from the state object.
     *
     * An intersection function the table installs cannot see the compute encoder's argument table,
     * so the dispatch constants it resolves the hit group with - and the visible function table it
     * calls the group's any-hit shader through - have to be bound in the table's own buffer
     * argument table. Those are per-dispatch values: the constants block is an offset into this
     * command list's heap and changes on every dispatch. Copying the state object's function
     * layout into a fresh table per dispatch is what keeps a table that is already in flight
     * immutable, which a table shared between dispatches could not be.
     */
    auto dispatch_intersection_table = state_object->CreateDispatchIntersectionFunctionTable(
        allocator_->gpu_heap_buffer_, arguments_offset, rt_visible_function_table_
    );
    if (!dispatch_intersection_table) {
      RecordError(E_FAIL);
      return;
    }

    bool have_prepare_work = false;
    for (const auto &entry : tables)
      have_prepare_work |= entry.count != 0;

    if (have_prepare_work) {
      allocator_->InvalidateCurrentPass();
      auto *prepare = allocator_->AllocatePass<ComputeEncoderData>();
      prepare->type = EncoderType::Compute;
      prepare->cmd_head.type = WMTComputeCommandNop;
      prepare->cmd_head.next.set(0);
      prepare->cmd_tail = (wmtcmd_base *)&prepare->cmd_head;
      prepare->rt_pipeline = prepare_pipeline;
      try {
        for (const auto &entry : tables) {
          if (entry.count && entry.buffer)
            prepare->rt_buffers.push_back(entry.buffer);
        }
      } catch (const std::bad_alloc &) {
        RecordError(E_OUTOFMEMORY);
        return;
      }
      for (size_t i = 0; i < std::size(tables); ++i) {
        const auto &entry = tables[i];
        if (!entry.count)
          continue;
        auto &set_pso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
        set_pso.type = WMTComputeCommandSetPSO;
        set_pso.pso = prepare_pipeline;
        set_pso.threadgroup_size = {64, 1, 1};
        auto &set_records = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        set_records.type = WMTComputeCommandSetBuffer;
        set_records.buffer = entry.buffer;
        set_records.offset = entry.buffer_offset;
        set_records.index = 0;
        const auto set_heap_buffer = [&](uint8_t index, uint64_t offset) {
          auto &command = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
          command.type = WMTComputeCommandSetBuffer;
          command.buffer = allocator_->gpu_heap_buffer_;
          command.offset = offset;
          command.index = index;
        };
        set_heap_buffer(1, entry.index_address - heap_address);
        set_heap_buffer(2, entry.entry_address - heap_address);
        set_heap_buffer(3, layout_offset);
        set_heap_buffer(4, copy_offset);
        set_heap_buffer(5, parameters_offset + i * sizeof(raytracing::SbtPrepareParameters));
        auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
        dispatch.type = WMTComputeCommandDispatchThreads;
        dispatch.size = {entry.count, 1, 1};
      }
    }

    allocator_->InvalidateCurrentPass();
    auto *traversal = allocator_->AllocatePass<ComputeEncoderData>();
    traversal->type = EncoderType::Compute;
    traversal->cmd_head.type = WMTComputeCommandNop;
    traversal->cmd_head.next.set(0);
    traversal->cmd_tail = (wmtcmd_base *)&traversal->cmd_head;
    traversal->rt_pipeline = rt_pipeline_;
    traversal->rt_intersection_function_table = dispatch_intersection_table;
    traversal->rt_visible_function_table = rt_visible_function_table_;
    try {
      for (const auto &entry : tables) {
        if (entry.count && entry.buffer)
          traversal->rt_buffers.push_back(entry.buffer);
      }
    } catch (const std::bad_alloc &) {
      RecordError(E_OUTOFMEMORY);
      return;
    }

    /*
     * The root arguments and static samplers of a raytracing pipeline are bound through the
     * ordinary compute root arguments, exactly as a compute dispatch binds them - but the signature
     * they come from is the state object's dispatch signature, not rootsig_compute_.
     *
     * The application's SetComputeRootSignature call set up the *user* global root signature, and
     * the local root signatures of the hit groups are not part of it at all: their static samplers
     * only exist in the dispatch signature the state object built, which is also what the exports
     * were compiled against. Binding the dispatch signature's table is what makes those samplers
     * readable, and it keeps the root argument layout compatible because that signature carries the
     * global parameters verbatim. A pipeline with no user global root signature still reaches this
     * path with a parameterless dispatch signature, so its local static samplers are bound even
     * with zero parameters.
     *
     * The application's root argument *values* still come from its own signature: the staged
     * arguments are the ones its SetComputeRoot* calls filled in, and the dispatch signature
     * consumes them at the same slot offsets because its parameters are the user's parameters.
     */
    if (dispatch_rootsig) {
      /*
       * The two bindings are independent, and a pipeline with no user global root signature has
       * zero root argument qwords but may still have local static samplers. Staging a zero-byte
       * root argument block would allocate nothing and bind the heap's first byte, so each binding
       * is emitted only when it actually has something to carry.
       */
      if (dispatch_rootsig->UploadQwords) {
        auto root_arguments_offset = EncodeRootArgument(dispatch_rootsig, rootarg_compute_staging_);
        auto &set_root_arguments = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        set_root_arguments.type = WMTComputeCommandSetBuffer;
        set_root_arguments.buffer = allocator_->gpu_heap_buffer_;
        set_root_arguments.offset = root_arguments_offset;
        set_root_arguments.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      }
      if (dispatch_rootsig->NumStaticSamplers) {
        auto static_samplers_offset = EncodeStaticSamplers(dispatch_rootsig);
        auto &set_static_samplers = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        set_static_samplers.type = WMTComputeCommandSetBuffer;
        set_static_samplers.buffer = allocator_->gpu_heap_buffer_;
        set_static_samplers.offset = static_samplers_offset;
        set_static_samplers.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
    }

    auto &set_pipeline = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    set_pipeline.type = WMTComputeCommandSetPSO;
    set_pipeline.pso = rt_pipeline_;
    set_pipeline.threadgroup_size = {1, 1, 1};

    auto &set_arguments = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    set_arguments.type = WMTComputeCommandSetBuffer;
    set_arguments.buffer = allocator_->gpu_heap_buffer_;
    set_arguments.offset = arguments_offset;
    set_arguments.index = SM50_RT_BINDING_INDEX_DISPATCH;

    auto &set_function_table = allocator_->EncodeComputeCommand<wmtcmd_compute_setfunctiontable>();
    set_function_table.type = WMTComputeCommandSetIntersectionFunctionTable;
    /*
     * The traversal selects its callbacks out of the table the ray generation shader received as
     * its argument, i.e. the one bound here - not out of the state object's own table. So the
     * per-dispatch table is what has to be bound: it is the one whose buffer argument table holds
     * the dispatch constants the callback reads.
     */
    set_function_table.table = dispatch_intersection_table;
    set_function_table.index = SM50_RT_BINDING_INDEX_INTERSECTION_FUNCTION_TABLE;

    /* The lowered traversal reaches miss/closest-hit/any-hit/callable exports
     * through the visible function table, so it has to be bound beside the
     * intersection function table. */
    auto &set_visible_function_table = allocator_->EncodeComputeCommand<wmtcmd_compute_setfunctiontable>();
    set_visible_function_table.type = WMTComputeCommandSetVisibleFunctionTable;
    set_visible_function_table.table = rt_visible_function_table_;
    set_visible_function_table.index = SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE;

    auto &set_local_roots = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    set_local_roots.type = WMTComputeCommandSetBuffer;
    set_local_roots.buffer = allocator_->gpu_heap_buffer_;
    set_local_roots.offset = entry_offset;
    set_local_roots.index = SM50_RT_BINDING_INDEX_LOCAL_ROOT_TABLE;

    auto &dispatch_rays = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch_rays.type = WMTComputeCommandDispatchThreads;
    dispatch_rays.size = {desc->Width, desc->Height, desc->Depth};
    owned_graphics::Trace(
        "dispatch_rays", ",\"list\":", owned_id, ",\"recording\":", owned_recording,
        ",\"encoder\":", allocator_->encoder_current->id, ",\"width\":", desc->Width, ",\"height\":", desc->Height,
        ",\"depth\":", desc->Depth, ",\"exports\":", export_count, ",\"miss\":", tables[1].count,
        ",\"hitgroup\":", tables[2].count, ",\"callable\":", tables[3].count
    );
  }
};

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::CreateCommandList(
    UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
    void **ppCommandList
) {
  if (Type != type_)
    return E_INVALIDARG;

  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(device_, Type));
  HRESULT hr = cmd_list->Initialize(this, pInitialPipelineState);
  if (FAILED(hr))
    return hr;
  return cmd_list->QueryInterface(riid, ppCommandList);
}

HRESULT
CreateClosedCommandList(
    MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandList
) {
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
}

}; // namespace dxmt
