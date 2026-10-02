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
#include "d3d12_temporal.hpp"
#include "com/com_pointer.hpp"

namespace dxmt {

MTLD3D12CommandAllocatorImpl::MTLD3D12CommandAllocatorImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type) :
    MTLD3D12Pageable<MTLD3D12CommandAllocator>(pDevice),
    type_(Type),
    clear_uav_(device_->GetMTLDevice(), *this),
    clear_rtv_(device_->GetMTLDevice(), *this, device_->GetMetal4Compiler()),
    copy_temp_allocator_(
        {device_->GetMTLDevice(), WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}
    ),
    copy_temp_version_(0) {}

HRESULT
CreateCommandAllocator(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
  switch (Type) {
  case D3D12_COMMAND_LIST_TYPE_DIRECT:
  case D3D12_COMMAND_LIST_TYPE_BUNDLE:
  case D3D12_COMMAND_LIST_TYPE_COMPUTE:
  case D3D12_COMMAND_LIST_TYPE_COPY:
    break;
  default:
    return E_INVALIDARG;
  }
  auto command_allocator = Com(new MTLD3D12CommandAllocatorImpl(pDevice, Type));
  HRESULT hr = command_allocator->Initialize();
  if (FAILED(hr))
    return hr;
  return command_allocator->QueryInterface(riid, ppCommandAllocator);
}

HRESULT
MTLD3D12CommandAllocatorImpl::Initialize() {

  if (!cpu_heap_)
    cpu_heap_ = malloc(kCPUHeapSize);
  if (!cpu_heap_)
    return E_OUTOFMEMORY;
  cpu_heap_offset_ = 0;

  for (size_t i = 1; i < gpu_heap_chunks_.size(); ++i) {
    gpu_heap_chunks_[i].buffer = {};
    free(gpu_heap_chunks_[i].memory);
  }
  if (!gpu_heap_chunks_.empty()) {
    gpu_heap_chunks_.resize(1);
    gpu_heap_chunks_.front().used = 0;
  }
  if (!ReserveGPUHeap(kGPUHeapSize, 16))
    return E_OUTOFMEMORY;

  encoder_current = nullptr;
  encoder_last = nullptr;
  encoder_count_ = 0;

  icb_.clear();

  copy_temp_allocator_.free_blocks(copy_temp_version_++);

  return S_OK;
}

HRESULT
STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::QueryInterface(REFIID riid, void **ppvObject) {
  if (ppvObject == nullptr)
    return E_POINTER;

  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
      riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandAllocator)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
    WARN("D3D12CommandAllocator: Unknown interface query ", str::format(riid));
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::Reset() {
  std::lock_guard<dxmt::mutex> lock(submission_mutex_);
  if (outstanding_submissions_)
    return E_FAIL;
  if (encoder_last)
    return E_FAIL;

  ClearRecordedCommands();
  return Initialize();
};

MTLD3D12CommandAllocatorImpl::~MTLD3D12CommandAllocatorImpl() {
  // A caller may release an unfinished list instead of closing/resetting it.
  // Link its last pass before visiting and destroying the recorded payloads.
  InvalidateCurrentPass();
  ClearRecordedCommands();
  free(cpu_heap_);
  cpu_heap_ = nullptr;
  for (auto &chunk : gpu_heap_chunks_) {
    chunk.buffer = {};
    free(chunk.memory);
  }
}

bool
MTLD3D12CommandAllocatorImpl::ReserveGPUHeap(uint64_t Length, size_t Alignment) {
  const auto maximum = std::min<uint64_t>(device_->GetMTLDevice().maxBufferLength(), SIZE_MAX);
  if (!Alignment || (Alignment & (Alignment - 1)) || Length > maximum)
    return false;
  if (!gpu_heap_chunks_.empty()) {
    const auto &chunk = gpu_heap_chunks_.back();
    const size_t adjustment = (-chunk.used) & (Alignment - 1);
    if (adjustment <= chunk.capacity - chunk.used && Length <= chunk.capacity - chunk.used - adjustment)
      return true;
  }
  const uint64_t capacity = std::max<uint64_t>(Length, kGPUHeapSize);
  if (capacity > maximum)
    return false;
  uint64_t begin = 0;
  if (!gpu_heap_chunks_.empty()) {
    const auto &previous = gpu_heap_chunks_.back();
    begin = previous.begin + previous.capacity;
  }
  if (capacity > SIZE_MAX - begin)
    return false;
  // Grow metadata first: neither a native allocation nor the current cursor
  // changes when vector allocation fails. CPU pointers live outside the vector.
  try {
    if (gpu_heap_chunks_.size() == gpu_heap_chunks_.capacity())
      gpu_heap_chunks_.reserve(gpu_heap_chunks_.size() + std::max<size_t>(gpu_heap_chunks_.size(), 1));
  } catch (const std::bad_alloc &) {
    return false;
  }
  void *memory = malloc(size_t(capacity));
  if (!memory)
    return false;
  WMTBufferInfo info{};
  info.memory.set(memory);
  info.length = capacity;
  info.options = WMTResourceHazardTrackingModeUntracked;
  auto buffer = device_->GetMTLDevice().newBuffer(info);
  if (!buffer) {
    free(memory);
    return false;
  }
  gpu_heap_chunks_.push_back({std::move(buffer), memory, begin, info.gpu_address, size_t(capacity)});
  return true;
}

MTLD3D12CommandAllocatorImpl::GPUHeapAllocation
MTLD3D12CommandAllocatorImpl::ResolveGPUHeap(uint64_t Offset) const {
  assert(!gpu_heap_chunks_.empty());
  const auto *chunk = &gpu_heap_chunks_.back();
  if (Offset < chunk->begin) {
    const auto next = std::upper_bound(gpu_heap_chunks_.begin(), gpu_heap_chunks_.end(), Offset,
        [](uint64_t offset, const GPUHeapChunk &candidate) { return offset < candidate.begin; });
    assert(next != gpu_heap_chunks_.begin());
    chunk = &*std::prev(next);
  }
  const auto local = Offset - chunk->begin;
  assert(local < chunk->capacity);
  return {ptr_add(chunk->memory, local), chunk->buffer, local, chunk->address + local};
}

void
MTLD3D12CommandAllocatorImpl::ClearRecordedCommands() {
  recording_generation_->fetch_add(1, std::memory_order_release);
  for (auto &encoder_list : encoder_lists_) {
    EncoderData *next = encoder_list.next;
    while (next) {
      auto following = next->next; // Read linkage before ending the object's lifetime.
      switch (next->type) {
      case EncoderType::Null:
        break;
      case EncoderType::Clear:
        reinterpret_cast<ClearEncoderData *>(next)->~ClearEncoderData();
        break;
      case EncoderType::Render:
        if (auto &uses = reinterpret_cast<RenderEncoderData *>(next)->swizzle_tables; uses)
          for (const auto &use : *uses) {
            use.rootsig->ReleasePrivate();
            use.heap->ReleasePrivate();
          }
        reinterpret_cast<RenderEncoderData *>(next)->~RenderEncoderData();
        break;
      case EncoderType::Blit:
        reinterpret_cast<BlitEncoderData *>(next)->~BlitEncoderData();
        break;
      case EncoderType::Compute:
        if (auto &uses = reinterpret_cast<ComputeEncoderData *>(next)->swizzle_tables; uses)
          for (const auto &use : *uses) {
            use.rootsig->ReleasePrivate();
            use.heap->ReleasePrivate();
          }
        reinterpret_cast<ComputeEncoderData *>(next)->~ComputeEncoderData();
        break;
      case EncoderType::Resolve:
        reinterpret_cast<ResolveEncoderData *>(next)->~ResolveEncoderData();
        break;
      case EncoderType::PredicatedTextureCopy:
        reinterpret_cast<PredicatedTextureCopyEncoderData *>(next)->~PredicatedTextureCopyEncoderData();
        break;
      case EncoderType::Temporal:
        reinterpret_cast<TemporalEncoderData *>(next)->~TemporalEncoderData();
        break;
      case EncoderType::Query:
        reinterpret_cast<QueryEncoderData *>(next)->~QueryEncoderData();
        break;
      case EncoderType::WorkGraph:
        reinterpret_cast<WorkGraphEncoderData *>(next)->~WorkGraphEncoderData();
        break;
      case EncoderType::StandardSwizzle: {
        auto *record = reinterpret_cast<StandardSwizzleEncoderData *>(next);
        if (record->resource)
          record->resource->ReleasePrivate();
        record->~StandardSwizzleEncoderData();
        break;
      }
      case EncoderType::Raytracing: {
        auto *record = reinterpret_cast<RTASEncoderData *>(next);
        /*
         * The recording that published this version is going away. Its lease is what keeps an
         * abandoned recording from accumulating: a version that was never submitted and is not the
         * one the GPU holds is released here instead of waiting for an observation that may never
         * come (owned_rtas_lifetime.hpp).
         */
        if (record->versionToken)
          device_->ReleaseRTASLease(record->destinationKey, record->versionToken);
        record->~RTASEncoderData();
        break;
      }
      }
      next = following;
    }
  }
  encoder_lists_.clear();

  for (auto ptr : spilled_cpu_heap_)
    free(ptr);
  spilled_cpu_heap_.clear();
}

IndirectComputeCommandData *
MTLD3D12CommandAllocatorImpl::EncodeIndirectComputeCommand(
    MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount, bool statistics) {
  auto indirect_pso = statistics ? pPSO->GetPipelineStatisticsPipelineState(true)
                                 : pPSO->GetIndirectCommandBufferPipelineState();
  if (!indirect_pso)
    return nullptr;
  WMTIndirectCommandBufferInfo info;
  info.inherit_buffers = !pCmdSig->UpdateRootArguments;
  info.inherit_pso = 1;
  info.inherit_cull_mode = 0;
  info.inherit_fill_mode = 0;
  info.inherit_front_facing = 0;
  info.inherit_depth_bias = 0;
  info.inherit_depth_clip_mode = 0;
  info.inherit_depth_stencil_state = 0;
  info.support_color_attachment_mapping = 0;
  info.support_dynamic_attribute_stride = 0;
  info.support_ray_tracing = 0;
  info.type = WMTIndirectCommandTypeConcurrentDispatch;
  info.max_vertex_buffer_binding = 0;
  info.max_fragment_buffer_binding = 0;
  info.max_object_buffer_binding = 0;
  info.max_mesh_buffer_binding = 0;
  info.max_kernel_buffer_binding = 31;
  info.max_kernel_threadgroup_memory_binding = 0;
  info.max_object_threadgroup_memory_binding = 0;
  info.gpu_resource_id = 0;

  auto icb = device_->GetMTLDevice().newIndirectCommandBuffer(info, MaxCount, WMTResourceStorageModeShared);

  auto [Ptr, Offset] = AllocateGPUHeap(sizeof(IndirectComputeCommandData), 16);
  if (!Ptr)
    return nullptr;

  auto data = reinterpret_cast<IndirectComputeCommandData *>(Ptr);

  data->cmd_buf = info.gpu_resource_id;
  data->max_count = MaxCount;
  data->tgsize_x = pPSO->threadgroup_size.width;
  data->tgsize_y = pPSO->threadgroup_size.height;
  data->tgsize_z = pPSO->threadgroup_size.depth;
  const auto &constants = statistics ? pPSO->pipeline_statistics_compute_constants : pPSO->compute_constants;
  data->immutable_constants = constants.gpu_address;
  data->pipeline_statistics = 0;
  if (constants.buffer) {
    auto &resource = EncodeComputeCommand<wmtcmd_compute_useresource>();
    resource.type = WMTComputeCommandUseResource;
    resource.usage = WMTResourceUsageRead;
    resource.resource = constants.buffer;
  }

  {
    // populated outside
    data->max_count_buffer = 0;
    data->argument_buffer = 0;
    data->rootsig_qwords = 0;
    data->rootsig_qwords_stride = 0;
    data->static_samplers = 0;
  }

  {
    /**
     * TODO: move these out?
     */

    auto &cmd_use_icb = EncodeComputeCommand<wmtcmd_compute_useresource>();
    cmd_use_icb.type = WMTComputeCommandUseResource;
    cmd_use_icb.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    cmd_use_icb.resource = icb;

    auto &cmd_setpso_res = EncodeComputeCommand<wmtcmd_compute_setpso>();
    cmd_setpso_res.type = WMTComputeCommandSetPSO;
    cmd_setpso_res.pso = pCmdSig->compute_resolver;
    cmd_setpso_res.threadgroup_size = {1, 1, 1};

    auto &cmd_argbuf_res = EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    cmd_argbuf_res.type = WMTComputeCommandSetBuffer;
    BindGPUHeap(cmd_argbuf_res.buffer, cmd_argbuf_res.offset, Offset);
    cmd_argbuf_res.index = 30;

    auto &cmd_dispatch_res = EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch_res.type = WMTComputeCommandDispatch;
    cmd_dispatch_res.size = {1, 1, 1};

    auto &cmd_setpso = EncodeComputeCommand<wmtcmd_compute_setpso>();
    cmd_setpso.type = WMTComputeCommandSetPSO;
    cmd_setpso.pso = indirect_pso;
    cmd_setpso.threadgroup_size = pPSO->threadgroup_size; // not really used
  }

  auto &cmd = EncodeComputeCommand<wmtcmd_compute_executecommands>();
  cmd.type = WMTComputeCommandExecuteCommandsInBuffer;
  cmd.indirect_command_buffer = icb;
  cmd.location = 0;
  cmd.length = MaxCount;
  auto &restore = EncodeComputeCommand<wmtcmd_compute_setpso>();
  restore.type = WMTComputeCommandSetPSO;
  restore.pso = statistics ? pPSO->GetPipelineStatisticsPipelineState(false) : pPSO->pso;
  restore.threadgroup_size = pPSO->threadgroup_size;

  icb_.push_back(std::move(icb));

  return data;
}

IndirectRenderCommandData *
MTLD3D12CommandAllocatorImpl::EncodeIndirectRenderCommand(
    MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, size_t MaxCount, bool index_pull
) {
  auto indirect_pso = index_pull ? pPSO->GetIndexPullPipelineState(true)
                                 : pPSO->GetIndirectCommandBufferPipelineState();
  if (!indirect_pso)
    return nullptr;
  WMTIndirectCommandBufferInfo info;
  info.inherit_buffers = !(index_pull || pCmdSig->UpdateVertexBuffers || pCmdSig->UpdateIndexBuffer ||
                           pCmdSig->UpdateRootArguments || pPSO->view_location_offsets);
  info.inherit_pso = 1;
  info.inherit_cull_mode = 1;
  info.inherit_fill_mode = 1;
  info.inherit_front_facing = 1;
  info.inherit_depth_bias = 1;
  info.inherit_depth_clip_mode = 1;
  info.inherit_depth_stencil_state = 1;
  info.support_color_attachment_mapping = 0;
  info.support_dynamic_attribute_stride = 0;
  info.support_ray_tracing = 0;
  info.type = index_pull ? WMTIndirectCommandTypeDraw | WMTIndirectCommandTypeDrawIndexed
              : pCmdSig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? WMTIndirectCommandTypeDrawIndexed
                                                                                : WMTIndirectCommandTypeDraw;
  info.max_vertex_buffer_binding = 31;
  info.max_fragment_buffer_binding = 31;
  info.max_object_buffer_binding = 0;
  info.max_mesh_buffer_binding = 0;
  info.max_kernel_buffer_binding = 0;
  info.max_kernel_threadgroup_memory_binding = 0;
  info.max_object_threadgroup_memory_binding = 0;
  info.gpu_resource_id = 0;

  auto icb = device_->GetMTLDevice().newIndirectCommandBuffer(info, MaxCount, WMTResourceStorageModePrivate);

  auto [Ptr, Offset] = AllocateGPUHeap(sizeof(IndirectRenderCommandData), 16);
  if (!Ptr)
    return nullptr;

  auto data = reinterpret_cast<IndirectRenderCommandData *>(Ptr);

  data->cmd_buf = info.gpu_resource_id;
  data->max_count = MaxCount;
  auto &vertex_constants = pPSO->view_location_offsets
      ? index_pull ? pPSO->indirect_view_index_pull_vertex_constants : pPSO->indirect_view_vertex_constants
      : index_pull ? pPSO->index_pull_vertex_constants : pPSO->vertex_constants;
  data->vertex_constants = vertex_constants.gpu_address;
  data->fragment_constants = pPSO->fragment_constants.gpu_address;
  for (auto [constants, stage] : {
         std::pair{&vertex_constants, WMTRenderStageVertex},
         std::pair{&pPSO->fragment_constants, WMTRenderStageFragment}}) {
    if (!constants->buffer)
      continue;
    auto &resource = EncodeRenderCommand<wmtcmd_render_useresource>();
    resource.type = WMTRenderCommandUseResource;
    resource.stages = stage;
    resource.usage = WMTResourceUsageRead;
    resource.resource = constants->buffer;
    auto &retained = static_cast<RenderEncoderData *>(encoder_current)->immutable_constant_buffers;
    if (std::none_of(retained.begin(), retained.end(), [&](const auto &buffer) {
          return buffer.handle == constants->buffer.handle;
        }))
      retained.push_back(constants->buffer);
  }

  {
    // populated outside
    data->max_count_buffer = 0;
    data->argument_buffer = 0;
    data->rootsig_qwords = 0;
    data->rootsig_qwords_stride = 0;
    data->static_samplers = 0;
    data->vertex_buffer = 0;
    data->vertex_argbuf_stride = 0;
    data->primitive_type = 0;
    data->index_buffer = 0;
    data->index_buffer_format = {};
    data->index_pull_controls = 0;
    data->index_pull_mode = ~0u;
    data->so_draws = 0;
    data->pipeline_statistics = 0;
  }

  {
    /**
     * TODO: move these out?
     */

    auto &cmd_use_icb = EncodeRenderCommand<wmtcmd_render_useresource>();
    cmd_use_icb.type = WMTRenderCommandUseResource;
    cmd_use_icb.stages = WMTRenderStageVertex;
    cmd_use_icb.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    cmd_use_icb.resource = icb;

    // ICB draws bind root arguments, static samplers and vertex tables inside the GPU heap by address; the
    // fragment stage only sees them when the heap is resident for it (the resolver binds it to the vertex stage).
    auto &cmd_use_heap = EncodeRenderCommand<wmtcmd_render_useresource>();
    cmd_use_heap.type = WMTRenderCommandUseResource;
    cmd_use_heap.stages = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
    cmd_use_heap.usage = WMTResourceUsageRead;
    cmd_use_heap.resource = ResolveGPUHeap(Offset).buffer;

    auto &cmd_setpso_res = EncodeRenderCommand<wmtcmd_render_setpso>();
    cmd_setpso_res.type = WMTRenderCommandSetPSO;
    cmd_setpso_res.pso = pCmdSig->render_resolver;

    auto &cmd_argbuf_res = EncodeRenderCommand<wmtcmd_render_setbuffer>();
    cmd_argbuf_res.type = WMTRenderCommandSetVertexBuffer;
    BindGPUHeap(cmd_argbuf_res.buffer, cmd_argbuf_res.offset, Offset);
    cmd_argbuf_res.index = 30;

    auto &cmd_draw_res = EncodeRenderCommand<wmtcmd_render_draw>();
    cmd_draw_res.type = WMTRenderCommandDraw;
    cmd_draw_res.primitive_type = WMTPrimitiveTypePoint;
    cmd_draw_res.vertex_start = 0;
    cmd_draw_res.vertex_count = MaxCount;
    cmd_draw_res.base_instance = 0;
    cmd_draw_res.instance_count = 1;

    // The vertex resolver writes both the ICB and any per-command bindings.
    // Publish those writes before Metal 4 fetches the indirect draw commands.
    auto &resolved = EncodeRenderCommand<wmtcmd_render_memory_barrier>();
    resolved.type = WMTRenderCommandMemoryBarrier;
    resolved.scope = WMTBarrierScopeBuffers;
    resolved.stages_after = WMTRenderStageVertex;
    resolved.stages_before = WMTRenderStageVertex;

    auto &cmd_setpso = EncodeRenderCommand<wmtcmd_render_setpso>();
    cmd_setpso.type = WMTRenderCommandSetPSO;
    cmd_setpso.pso = indirect_pso;
  }

  if (pPSO->view_location_offsets) {
    // Routing is shader-owned for ICB draws. Never add native offsets a second time,
    // even if a future Metal implementation starts inheriting viewMappings.
    auto &mapping = EncodeRenderCommand<wmtcmd_render_set_vertex_amplification>();
    mapping.type = WMTRenderCommandSetVertexAmplification;
    mapping.viewport_offset = 0;
    mapping.render_target_array_offset = 0;
  }
  auto &cmd = EncodeRenderCommand<wmtcmd_render_executecommands>();
  cmd.type = WMTRenderCommandExecuteCommandsInBuffer;
  cmd.indirect_command_buffer = icb;
  cmd.location = 0;
  cmd.length = MaxCount;
  auto &restore = EncodeRenderCommand<wmtcmd_render_setpso>();
  restore.type = WMTRenderCommandSetPSO;
  restore.pso = pPSO->pso;

  icb_.push_back(std::move(icb));

  return data;
}

template <>
WMT::Reference<WMT::ComputePipelineState>
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::getComputePipeline(std::string name) {
  auto lib = ctx.device_->GetLib().getLibrary();
  auto func = WMT4::Library(lib).newFunction(name.c_str());
  if (!func)
    return {};
  WMT::Reference<WMT::Error> err;
  auto pso = ctx.device_->GetMetal4Compiler().newComputePipelineState(func, err);
  if (err) {
    ERR("Failed to create compute PSO: ", err.description().getUTF8String());
  }
  return pso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::startComputePass() {
  ctx.InvalidateCurrentPass();
  auto compute = ctx.AllocatePass<ComputeEncoderData>();
  compute->type = EncoderType::Compute;
  compute->cmd_head.type = WMTComputeCommandNop;
  compute->cmd_head.next.set(0);
  compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::endPass() {
  ctx.InvalidateCurrentPass();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputePSO(WMT::ComputePipelineState pso, WMTSize tgsize) {
  auto &setpso = ctx.EncodeComputeCommand<wmtcmd_compute_setpso>();
  setpso.type = WMTComputeCommandSetPSO;
  setpso.pso = pso;
  setpso.threadgroup_size = tgsize;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::dispatch(WMTSize size) {
  auto &dispatch = ctx.EncodeComputeCommand<wmtcmd_compute_dispatch>();
  dispatch.type = WMTComputeCommandDispatchThreads;
  dispatch.size = size;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::dispatchIndirect(WMT::Buffer arguments, uint64_t offset) {
  auto &dispatch = ctx.EncodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
  dispatch.type = WMTComputeCommandDispatchIndirect;
  dispatch.indirect_args_buffer = arguments;
  dispatch.indirect_args_offset = offset;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeTexture(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId, int flags
) {
  auto &dst_ = texture->view(viewId);
  auto &settex = ctx.EncodeComputeCommand<wmtcmd_compute_settexture>();
  settex.type = WMTComputeCommandSetTexture;
  settex.texture = dst_.texture;
  settex.index = index;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeTexelBuffer(
    uint32_t index, const Rc<Buffer> &buffer, uint64_t viewId, int flags
) {
  auto &dst_ = buffer->view_(viewId);
  auto &settexbuf = ctx.EncodeComputeCommand<wmtcmd_compute_settexture>();
  settexbuf.type = WMTComputeCommandSetTexture;
  settexbuf.texture = dst_.texture;
  settexbuf.index = index;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeBuffer(
    uint32_t index, const Rc<Buffer> &buffer, uint32_t offset, uint32_t length, int flags
) {
  auto dst_ = buffer->current();
  auto &setbuf = ctx.EncodeComputeCommand<wmtcmd_compute_setbuffer>();
  setbuf.type = WMTComputeCommandSetBuffer;
  setbuf.buffer = dst_->buffer();
  setbuf.index = index;
  setbuf.offset = 0; // the `offset` and `length` parameter of this function are just for (potential) hazard tracking,
                     // which we don't need here
}

template <>
void *
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeBytes(uint32_t index, uint32_t length) {
  auto &setmeta = ctx.EncodeComputeCommand<wmtcmd_compute_setbytes>();
  setmeta.type = WMTComputeCommandSetBytes;
  void *temp = ctx.AllocateCPUHeap(length, 16);
  setmeta.bytes.set(temp);
  setmeta.length = length;
  setmeta.index = index;
  return temp;
}

template <>
WMT::Library
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::getDefaultLibrary() {
  return ctx.device_->GetLib().getLibrary();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::startRenderPass() {
  ctx.InvalidateCurrentPass();
  auto render = ctx.AllocatePass<RenderEncoderData>();
  render->type = EncoderType::Render;
  render->cmd_head.type = WMTRenderCommandNop;
  render->cmd_head.next.set(0);
  render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
  render->dsv_planar_flags = 0;
  render->dsv_readonly_flags = 0;
  render->render_target_count = 0;
  render->render_target_width = 16384;
  render->render_target_height = 16384;
  render->render_target_array_length = 0;
  render->default_raster_sample_count = 1;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setColorAttachment(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId, uint32_t depth_plane
) {
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  encoder->colors[index].attachment = texture->view(viewId);
  encoder->colors[index].depth_plane = depth_plane;
  encoder->colors[index].load_action = WMTLoadActionLoad;
  encoder->colors[index].store_action = WMTStoreActionStore;
  encoder->render_target_count = std::max<uint32_t>(encoder->render_target_count, index + 1);
  encoder->render_target_width = std::min<uint32_t>(encoder->render_target_width, texture->width(viewId));
  encoder->render_target_height = std::min<uint32_t>(encoder->render_target_height, texture->height(viewId));
  encoder->render_target_array_length = texture->textureType() == WMTTextureType3D
                                            ? (texture->depth(viewId) - depth_plane)
                                            : texture->arrayLength(viewId);
  encoder->default_raster_sample_count = texture->sampleCount();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setDepthStencilAttachment(
    const Rc<Texture> &texture, uint64_t viewId, uint32_t dsv_flag
) {
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  if (dsv_flag & 1) {
    encoder->depth.attachment = texture->view(viewId);
    encoder->depth.load_action = WMTLoadActionLoad;
    encoder->depth.store_action = WMTStoreActionStore;
  }
  if (dsv_flag & 2) {
    encoder->stencil.attachment = texture->view(viewId);
    encoder->stencil.load_action = WMTLoadActionLoad;
    encoder->stencil.store_action = WMTStoreActionStore;
  }
  encoder->dsv_planar_flags = dsv_flag;
  encoder->render_target_width = std::min<uint32_t>(encoder->render_target_width, texture->width(viewId));
  encoder->render_target_height = std::min<uint32_t>(encoder->render_target_height, texture->height(viewId));
  encoder->render_target_array_length = texture->arrayLength(viewId);
  encoder->default_raster_sample_count = texture->sampleCount();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setRenderPSO(WMT::RenderPipelineState pso) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setpso>();
  cmd.type = WMTRenderCommandSetPSO;
  cmd.pso = pso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setViewport(WMTViewport viewport) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setviewport>();
  cmd.type = WMTRenderCommandSetViewport;
  cmd.viewport = viewport;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setDepthStencilState(WMT::DepthStencilState dsso) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
  cmd.type = WMTRenderCommandSetDepthStencilState;
  cmd.depth_stencil_state = dsso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setScissorRect(WMTScissorRect rect) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setscissorrect>();
  cmd.type = WMTRenderCommandSetScissorRect;
  cmd.scissor_rect = rect;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setStencilReference(uint8_t stencil_ref) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setstencilref>();
  cmd.type = WMTRenderCommandSetStencilRef;
  cmd.stencil_ref = stencil_ref;
}

template <>
void *
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setFragmentBytes(uint32_t index, uint32_t length) {
  auto &setmeta = ctx.EncodeRenderCommand<wmtcmd_render_setbytes>();
  setmeta.type = WMTRenderCommandSetFragmentBytes;
  void *temp = ctx.AllocateCPUHeap(length, 16);
  setmeta.bytes.set(temp);
  setmeta.length = length;
  setmeta.index = index;
  return temp;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::draw(
    WMTPrimitiveType primitive, uint32_t vertex_start, uint32_t vertex_count, int32_t base_instance,
    uint32_t instance_count
) {
  auto &draw = ctx.EncodeRenderCommand<wmtcmd_render_draw>();
  draw.type = WMTRenderCommandDraw;
  draw.primitive_type = primitive;
  draw.vertex_start = vertex_start;
  draw.vertex_count = vertex_count;
  draw.base_instance = base_instance;
  draw.instance_count = instance_count;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::drawIndirect(
    WMTPrimitiveType primitive, WMT::Buffer arguments, uint64_t offset
) {
  auto &draw = ctx.EncodeRenderCommand<wmtcmd_render_draw_indirect>();
  draw.type = WMTRenderCommandDrawIndirect;
  draw.primitive_type = primitive;
  draw.indirect_args_buffer = arguments;
  draw.indirect_args_offset = offset;
}

}; // namespace dxmt
