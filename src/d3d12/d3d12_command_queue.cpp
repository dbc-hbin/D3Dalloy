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

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "owned_submission_ring.hpp"
#include "owned_queue_task_fifo.hpp"
#include "d3d12_temporal.hpp"
#include <atomic>
#include <new>
#include <optional>
#include <variant>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

constexpr auto kCommandQueueSize = 32u;

const GUID kD3D12CommandQueueDownlevelUUID = {
    0x38a8c5ef, 0x7ccb, 0x4e81, {0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94}
};

class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory> {

  const uint64_t owned_id_ = owned_graphics::NewObjectId();
  D3D12_COMMAND_QUEUE_DESC desc_;

  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  WMT::Reference<WMT::Event> query_barrier_event_;
  uint64_t query_barrier_value_ = 0;

  owned_graphics::SubmissionRing<kCommandQueueSize> submission_ring_;

  struct OwnedHandle {
    HANDLE handle = nullptr;

    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) : handle(handle) {}
    OwnedHandle(const OwnedHandle &) = delete;
    OwnedHandle &operator=(const OwnedHandle &) = delete;
    OwnedHandle(OwnedHandle &&other) : handle(other.handle) {
      other.handle = nullptr;
    }
    OwnedHandle &operator=(OwnedHandle &&other) {
      if (this != &other) {
        if (handle)
          CloseHandle(handle);
        handle = other.handle;
        other.handle = nullptr;
      }
      return *this;
    }
    ~OwnedHandle() {
      if (handle)
        CloseHandle(handle);
    }
    HANDLE release() {
      auto result = handle;
      handle = nullptr;
      return result;
    }
  };

  struct ListSubmission {
    EncoderData *entry = nullptr;
    uint64_t list_id = 0;
    uint64_t recording = 0;
    Com<ID3D12CommandAllocator> allocator;
  };

  struct ExecuteTask {
    std::vector<ListSubmission> lists;
    bool contains_temporal = false;
  };

  struct SignalTask {
    Rc<Fence> fence;
    Fence::QueueTicket ticket;
    uint64_t fence_id = 0;
  };

  struct WaitTask {
    Rc<Fence> fence;
    Fence::QueueTicket ticket;
    uint64_t fence_id = 0;
  };

  struct PresentTask {
    Rc<Presenter> presenter;
    Com<MTLD3D12Resource> backbuffer;
    std::unique_ptr<Presenter::PresentState> state;
    OwnedHandle completion_handle;
    double after = 0;
  };

  using QueueTask = std::variant<ExecuteTask, SignalTask, WaitTask, PresentTask>;
  owned_graphics::QueueTaskFifo<QueueTask> task_fifo_;
  dxmt::mutex mutex_enqueue_;

  struct InflightCommandBuffer {
    WMT::Reference<WMT::CommandBuffer> cmdbuf{};
    HANDLE semaphore{};
    // Allocator GPU argument storage uses newBufferWithBytesNoCopy. Retaining
    // its MTLBuffer alone does not retain the allocator's malloc backing.
    // Keep that owner until the submitted command buffer has completed.
    std::vector<Com<ID3D12CommandAllocator>> allocators;
    /*
     * Acceleration-structure versions this submission published. When it completes, the version
     * mirror says which of them the GPU actually holds for each address, and the device then
     * releases whatever no in-flight submission can read any more (owned_rtas_lifetime.hpp).
     *
     * Each entry also carries the submission's own recording lease on that version, which is
     * released once the version is known to be live or superseded.
     */
    std::vector<std::pair<raytracing::ArenaKey, uint32_t>> rtas_observations;
    uint64_t rtas_submission = 0;
    // Prepared commands retain feature history, native views, and private
    // scratch until Metal completion, independently of frontend ReleaseFeature.
    std::vector<std::shared_ptr<OwnedTemporalFrame>> temporal_frames;
    std::vector<std::pair<std::shared_ptr<owned_graphics::QueryTimestampProducers>, uint32_t>> timestamp_completions;
  };

  std::array<InflightCommandBuffer, kCommandQueueSize> inflight_cmdbuf_pool_;
  dxmt::thread inflight_cmdbuf_wait_thread_;
  dxmt::thread submission_thread_;

  dxmt::mutex mutex_commit_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("dxmt-cmdbuf-waiting-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t internal_seq = 1;
    for (;;) {
      if (!submission_ring_.wait(internal_seq))
        break;
      auto &inflight = inflight_cmdbuf_pool_[internal_seq % kCommandQueueSize];

      if (inflight.cmdbuf.status() <= WMTCommandBufferStatusScheduled)
        inflight.cmdbuf.waitUntilCompleted();
      if (inflight.cmdbuf.status() == WMTCommandBufferStatusError)
        ERR("Device error: ", inflight.cmdbuf.error().description().getUTF8String());
      else
        for (const auto &[queries, index] : inflight.timestamp_completions)
          queries->completed(index, inflight.cmdbuf);

      owned_graphics::Trace("complete", ",\"queue\":", owned_id_, ",\"submission\":", internal_seq,
                            ",\"status\":", uint32_t(inflight.cmdbuf.status()));
      if (inflight.semaphore) {
        ReleaseSemaphore(inflight.semaphore, 1, nullptr);
        CloseHandle(inflight.semaphore);
      }

      /*
       * The GPU has finished this submission, so every version mirror it wrote now holds the
       * truth for its address: that is the only point at which the device learns which native
       * structure the header holds, and therefore the only point at which the version it replaced
       * may start counting down to release. Reporting before the submission retires keeps the
       * submission's own readers inside the barrier.
       *
       * The submission's own lease is released after the observation, so a version this submission
       * executed is either live (kept) or superseded (released once no reader is left).
       */
      for (const auto &[key, token] : inflight.rtas_observations) {
        device_->ObserveRTAS(key);
        device_->ReleaseRTASLease(key, token);
      }
      if (inflight.rtas_submission)
        device_->RetireRTASSubmission(inflight.rtas_submission);

      inflight = {};
      submission_ring_.retire(internal_seq);

      internal_seq++;
    }
  };

  struct CommittingScope {
    MTLD3D12CommandQueueImpl *queue;
    std::lock_guard<dxmt::mutex> lock;
    uint64_t seq;
    InflightCommandBuffer &inflight;
    WMT::Reference<WMT::Object> pool;
    // This stays locked through the destructor's commit(), then releases. No
    // GPU completion or ring-capacity wait occurs while it is held.
    std::unique_lock<std::mutex> temporal_submission_lock;

    CommittingScope(MTLD3D12CommandQueueImpl *queue) :
        queue(queue),
        lock(queue->mutex_commit_),
        seq(queue->submission_ring_.reserve()),
        inflight(queue->inflight_cmdbuf_pool_[seq % kCommandQueueSize]),
        pool(WMT::MakeAutoreleasePool()) {
      inflight.cmdbuf = queue->queue_.commandBuffer();
    };

    ~CommittingScope() {
      inflight.cmdbuf.commit();
      owned_graphics::Trace("submit", ",\"queue\":", queue->owned_id_, ",\"submission\":", seq);
      queue->submission_ring_.publish(seq);
    }
  };

  CommittingScope
  StartCommitting() {
    return CommittingScope(this);
  }

  void
  SubmissionThread() {
    env::setThreadName("dxmt-d3d12-submit-thread");
    QueueTask task;
    while (task_fifo_.waitPop(task)) {
      std::visit([this](auto &typed) { this->Process(typed); }, task);
      // Release allocator/fence/presenter/resource references as soon as the
      // task has been encoded instead of retaining the last task while idle.
      task = ExecuteTask{};
    }
  }

  void Process(ExecuteTask &task) { ProcessExecute(task); }
  void Process(SignalTask &task) { ProcessSignal(task); }
  void Process(WaitTask &task) { ProcessWait(task); }
  void Process(PresentTask &task) { ProcessPresent(task); }

public:
  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory>(pDevice),
      inflight_cmdbuf_wait_thread_([this]() { this->CommandBufferWaitingThread(); }),
      submission_thread_([this]() { this->SubmissionThread(); }) {}

  ~MTLD3D12CommandQueueImpl() {
    task_fifo_.close();
    submission_thread_.join();
    const uint64_t last_submission = submission_ring_.lastPublished();
    if (last_submission)
      submission_ring_.waitUntilRetired(last_submission);
    std::lock_guard<dxmt::mutex> lock(mutex_commit_);
    submission_ring_.stop();
    inflight_cmdbuf_wait_thread_.join();
  }

  HRESULT
  Initialize(const D3D12_COMMAND_QUEUE_DESC *pDesc) {
    // TODO: validate and normalize
    desc_ = *pDesc;
    desc_.NodeMask = 1; // typically 1 GPU only

    auto metal_device = device_->GetMTLDevice();
    queue_ = metal_device.newCommandQueue(kCommandQueueSize);
    if (!queue_)
      return E_FAIL;
    queue_.addResidencySet(device_->GetGlobalResidencySet());

    fence_ = metal_device.newFence();
    if (!fence_)
      return E_FAIL;
    query_barrier_event_ = metal_device.newEvent();
    if (!query_barrier_event_)
      return E_FAIL;
    owned_graphics::Trace("queue_create", ",\"queue\":", owned_id_, ",\"type\":", uint32_t(desc_.Type));

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLSwapChainFactory>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12CommandQueueDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
      WARN("D3D12CommandQueue: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    IMPLEMENT_ME
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    IMPLEMENT_ME
  };

  void
  ProcessExecute(ExecuteTask &task) {
    std::optional<CommittingScope> scope;
    auto begin_segment = [&] {
      scope.emplace(this);
      scope->inflight.allocators.reserve(task.lists.size());
      for (auto &list : task.lists)
        scope->inflight.allocators.push_back(list.allocator);
      /*
       * Mark this submission active before anything is encoded. Every acceleration-structure
       * version superseded while it is in flight is held until it retires, because this
       * submission may already have encoded a dispatch that reads the old structure.
       *
       * begin_segment also runs again after a timestamp-resolve segment, so the id is only taken
       * once per submission; the inflight record survives the segment boundary.
       */
      if (!scope->inflight.rtas_submission)
        scope->inflight.rtas_submission = device_->BeginRTASSubmission();
      /*
       * Reserve the AS observation list before encoding: a throw from the encoder path would
       * leave an encoded submission whose version publication is never reported, which is a
       * lifetime bug rather than a crash. Every acceleration-structure pass in this task is
       * counted here, and Emit passes are simply never added.
       */
      size_t rtas_passes = 0;
      for (auto &list : task.lists)
        for (auto *encoder = list.entry; encoder; encoder = encoder->next)
          rtas_passes += encoder->type == EncoderType::Raytracing;
      scope->inflight.rtas_observations.reserve(rtas_passes);
      if (task.contains_temporal)
        scope->temporal_submission_lock = std::unique_lock(device_->owned_temporal_submission_mutex);
    };
    begin_segment();
    for (auto &list : task.lists) {
      owned_graphics::Trace("execute_list", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                            ",\"list\":", list.list_id,
                            ",\"recording\":", list.recording);
      EncoderData *current = list.entry;
      while (current) {
        if (current->type == EncoderType::Query &&
            static_cast<QueryEncoderData *>(current)->op == QueryEncoderOp::ResolveTimestamp) {
          auto query = static_cast<QueryEncoderData *>(current);
          const uint64_t producer_submission = scope->seq;
          // This is the FIFO worker, never the application's Execute caller.
          // Release both queue and temporal locks before any completion wait.
          scope.reset();
          submission_ring_.waitUntilRetired(producer_submission);
          const auto producers = query->timestamp_producers->snapshot(query->start_index, query->query_count);
          for (auto producer : producers) {
            if (!producer)
              continue;
            if (producer.status() < WMTCommandBufferStatusCommitted) {
              ERR("ResolveQueryData: timestamp producer has not been submitted");
              return;
            }
            if (producer.status() <= WMTCommandBufferStatusScheduled)
              producer.waitUntilCompleted();
            if (producer.status() == WMTCommandBufferStatusError) {
              ERR("ResolveQueryData: timestamp producer failed");
              return;
            }
          }
          owned_graphics::Trace("query_resolve_ready", ",\"queue\":", owned_id_,
                                ",\"completed_submission\":", producer_submission,
                                ",\"start\":", query->start_index, ",\"count\":", query->query_count);
          begin_segment();
        }
        auto &cmdbuf = scope->inflight.cmdbuf;
        if (current->type != EncoderType::Null)
          owned_graphics::Trace("encode", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                                ",\"list\":", list.list_id,
                                ",\"recording\":", list.recording,
                                ",\"encoder\":", current->id, ",\"type\":", uint32_t(current->type));
        switch (current->type) {
        case EncoderType::Null:
          break;
        case EncoderType::Clear: {
          auto data = static_cast<ClearEncoderData *>(current);
          {
            WMTRenderPassInfo info;
            WMT::InitializeRenderPassInfo(info);
            if (data->clear_dsv) {
              if (data->clear_dsv & 1) {
                info.depth.clear_depth = data->depth_stencil.first;
                info.depth.texture = data->attachment.texture();
                info.depth.load_action = WMTLoadActionClear;
                info.depth.store_action = WMTStoreActionStore;
                info.depth.depth_plane = data->depth_plane;
              }
              if (data->clear_dsv & 2) {
                info.stencil.clear_stencil = data->depth_stencil.second;
                info.stencil.texture = data->attachment.texture();
                info.stencil.load_action = WMTLoadActionClear;
                info.stencil.store_action = WMTStoreActionStore;
                info.stencil.depth_plane = data->depth_plane;
              }
              info.render_target_width = data->width;
              info.render_target_height = data->height;
            } else {
              info.colors[0].clear_color = data->color;
              info.colors[0].texture = data->attachment.texture();
              info.colors[0].load_action = WMTLoadActionClear;
              info.colors[0].store_action = WMTStoreActionStore;
              info.colors[0].depth_plane = data->depth_plane;
            }
            info.render_target_array_length = data->array_length;
            auto encoder = cmdbuf.renderCommandEncoder(info);
            encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
            encoder.waitForFence(fence_, WMTRenderStageFragment);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          break;
        }
        case EncoderType::Render: {
          auto data = static_cast<RenderEncoderData *>(current);
          WMTRenderPassInfo render_pass_info;
          WMT::InitializeRenderPassInfo(render_pass_info);
          {
            for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
              auto &color_data = data->colors[i];
              if (!color_data.attachment)
                continue;
              auto &color_info = render_pass_info.colors[i];
              color_info.texture = color_data.attachment.texture();
              color_info.load_action = color_data.load_action;
              color_info.store_action = color_data.store_action;
              color_info.level = color_data.level;
              color_info.slice = color_data.slice;
              color_info.depth_plane = color_data.depth_plane;
              color_info.clear_color = color_data.clear_color;
              color_info.resolve_texture = color_data.resolve_attachment.texture();
              color_info.resolve_level = color_data.resolve_level;
              color_info.resolve_slice = color_data.resolve_slice;
              color_info.resolve_depth_plane = color_data.resolve_depth_plane;
            }
            if (data->depth.attachment) {
              auto &depth_info = render_pass_info.depth;
              auto &depth_data = data->depth;
              depth_info.texture = depth_data.attachment.texture();
              depth_info.load_action = depth_data.load_action;
              depth_info.store_action = depth_data.store_action;
              depth_info.level = depth_data.level;
              depth_info.slice = depth_data.slice;
              depth_info.depth_plane = depth_data.depth_plane;
              depth_info.clear_depth = depth_data.clear_depth;
            }
            if (data->stencil.attachment) {
              auto &stencil_info = render_pass_info.stencil;
              auto &stencil_data = data->stencil;
              stencil_info.texture = stencil_data.attachment.texture();
              stencil_info.load_action = stencil_data.load_action;
              stencil_info.store_action = stencil_data.store_action;
              stencil_info.level = stencil_data.level;
              stencil_info.slice = stencil_data.slice;
              stencil_info.depth_plane = stencil_data.depth_plane;
              stencil_info.clear_stencil = stencil_data.clear_stencil;
            }
            render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
            render_pass_info.render_target_array_length = data->render_target_array_length;
            render_pass_info.render_target_width = data->render_target_width;
            render_pass_info.render_target_height = data->render_target_height;
            render_pass_info.visibility_buffer = data->visibility_scratch;
          }
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          encoder.waitForFence(fence_, WMTRenderStageVertex);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();
          for (const auto &target : data->visibility_targets) {
            auto accumulate = cmdbuf.computeCommandEncoder(false);
            accumulate.waitForFence(fence_);
            wmtcmd_compute_nop head{};
            head.type = WMTComputeCommandNop;
            wmtcmd_compute_setpso set_pso{};
            set_pso.type = WMTComputeCommandSetPSO;
            set_pso.pso = target.accumulate_pipeline;
            set_pso.threadgroup_size = {1, 1, 1};
            wmtcmd_compute_setbuffer set_source{};
            set_source.type = WMTComputeCommandSetBuffer;
            set_source.buffer = data->visibility_scratch;
            set_source.offset = 0;
            set_source.index = 0;
            wmtcmd_compute_setbuffer set_destination{};
            set_destination.type = WMTComputeCommandSetBuffer;
            set_destination.buffer = target.destination;
            set_destination.offset = uint64_t(target.index) * sizeof(uint64_t);
            set_destination.index = 1;
            wmtcmd_compute_dispatch dispatch{};
            dispatch.type = WMTComputeCommandDispatchThreads;
            dispatch.size = {1, 1, 1};
            head.next.set(&set_pso);
            set_pso.next.set(&set_source);
            set_source.next.set(&set_destination);
            set_destination.next.set(&dispatch);
            dispatch.next.set(nullptr);
            accumulate.encodeCommands(&head);
            accumulate.updateFence(fence_);
            accumulate.endEncoding();
            owned_graphics::Trace("query_accumulate", ",\"queue\":", owned_id_,
                                  ",\"submission\":", scope->seq, ",\"heap\":", target.heap_id,
                                  ",\"index\":", target.index);
          }
          break;
        }
        case EncoderType::Raytracing: {
          auto data = static_cast<RTASEncoderData *>(current);
          owned_graphics::Trace("as_encode", ",\"list\":", list.list_id, ",\"recording\":", list.recording,
                                ",\"encoder\":", current->id, ",\"op\":", uint32_t(data->operation),
                                ",\"instance_count\":", data->instanceCount,
                                ",\"postbuild\":", data->postbuild.size());
          if (data->operation != RTASOperation::Emit) {
            // The header and the instance translation are written by the GPU on this same
            // command buffer, so they can never race an in-flight frame that still reads the
            // acceleration structure the CPU address now points at.
            auto compute = cmdbuf.computeCommandEncoder(false);
            compute.waitForFence(fence_);
            WMTASPrepassInfo info{data->destination.resourceID(), data->destination.contributionGPUAddress(), 0,
                raytracing::kD3DInstanceDescriptorSize, raytracing::kMetalIndirectInstanceDescriptorSize,
                data->instanceCount, data->instanceCount != 0};
            info.version_token = data->versionToken;
            /* The kernel indexes the page it is given, so the slot is relative to the page. */
            info.live_mirror_slot = static_cast<uint32_t>(data->liveMirrorOffset / sizeof(uint32_t));
            if (!compute.encodeASPrepass(data->prepassPipeline, data->sourceInstances, data->sourceInstancesOffset,
                    data->destination.instanceBuffer(), 0, data->header, data->destinationOffset,
                    data->destination.contributionBuffer(), 0, data->liveMirror, 0, info)) {
              ERR("Raytracing AS prepass encoding failed");
              compute.endEncoding();
              break;
            }
            /*
             * This submission now carries the version publication for the address, so when it
             * completes the device reads the mirror back and learns which structure is live.
             *
             * The submission also takes its own lease: the recording that published the version
             * may be reset as soon as the GPU is done with the list, and the version must survive
             * until this submission retires, because this is the submission that executes it.
             */
            scope->inflight.rtas_observations.push_back({data->destinationKey, data->versionToken});
            device_->HoldRTASLease(data->destinationKey, data->versionToken);
            compute.updateFence(fence_);
            compute.endEncoding();
            if ((data->operation == RTASOperation::Clone || data->operation == RTASOperation::Compact) &&
                data->instanceCount) {
              // The instance translation is per-copy state, not part of the copied structure.
              auto blit = cmdbuf.blitCommandEncoder();
              blit.waitForFence(fence_);
              wmtcmd_blit_copy_from_buffer_to_buffer copy{};
              copy.type = WMTBlitCommandCopyFromBufferToBuffer;
              copy.next.set(nullptr);
              copy.src = data->source.contributionBuffer();
              copy.src_offset = 0;
              copy.dst = data->destination.contributionBuffer();
              copy.dst_offset = 0;
              copy.copy_length = static_cast<uint64_t>(data->instanceCount) * sizeof(uint32_t);
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
              blit.updateFence(fence_);
              blit.endEncoding();
            }
            auto encoder = cmdbuf.accelerationStructureCommandEncoder();
            encoder.waitForFence(fence_);
            const bool encoded = data->operation == RTASOperation::Build
                ? encoder.build(data->descriptor, data->destination, data->scratch, data->scratchOffset)
                : data->operation == RTASOperation::Refit
                    ? encoder.refit(
                          data->descriptor, data->source, data->destination, data->scratch, data->scratchOffset
                      )
                    : encoder.copy(
                          data->source, data->destination,
                          data->operation == RTASOperation::Clone ? 0u : 1u
                      );
            if (!encoded)
              ERR("Raytracing AS native encoding failed");
            for (const auto &entry : data->postbuild)
              if (entry.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE)
                encoder.compactedSize(data->destination, entry.destination, entry.offset);
            encoder.updateFence(fence_);
            encoder.endEncoding();
          } else if (!data->postbuild.empty() &&
                     data->postbuild[0].type ==
                         D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE) {
            auto encoder = cmdbuf.accelerationStructureCommandEncoder();
            encoder.waitForFence(fence_);
            encoder.compactedSize(data->source, data->postbuild[0].destination, data->postbuild[0].offset);
            encoder.updateFence(fence_);
            encoder.endEncoding();
          }
          if (!data->postbuild.empty()) {
            // Reading the compacted-size result the AS encoder just wrote needs an explicit
            // cross-encoder dependency; the same pass publishes the current-size value.
            auto compute = cmdbuf.computeCommandEncoder(false);
            compute.waitForFence(fence_);
            auto sidecar = data->operation == RTASOperation::Emit ? data->source : data->destination;
            const uint64_t current =
                (sidecar.size() + raytracing::kHeaderSize + raytracing::kAccelerationStructureAlignment - 1) &
                ~(raytracing::kAccelerationStructureAlignment - 1);
            for (const auto &entry : data->postbuild) {
              bool encoded = false;
              if (entry.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE)
                encoded =
                    compute.encodeASFinalizeCompactedSize(data->finalizeCompactedSizePipeline, entry.destination, entry.offset);
              else
                encoded = compute.encodeASWriteU64(data->writeU64Pipeline, entry.destination, entry.offset, current);
              owned_graphics::Trace("as_postbuild", ",\"list\":", list.list_id, ",\"recording\":", list.recording,
                                    ",\"op\":", uint32_t(data->operation), ",\"type\":", uint32_t(entry.type),
                                    ",\"offset\":", entry.offset, ",\"value\":", current,
                                    ",\"sidecar_size\":", sidecar.size(), ",\"encoded\":", uint32_t(encoded));
              if (!encoded)
                ERR("Raytracing AS postbuild compute encoding failed");
            }
            compute.updateFence(fence_);
            compute.endEncoding();
          }
          break;
        }
        case EncoderType::Blit: {
          auto data = static_cast<BlitEncoderData *>(current);
          auto encoder = cmdbuf.blitCommandEncoder();
          encoder.waitForFence(fence_);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Compute: {
          auto data = static_cast<ComputeEncoderData *>(current);
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(fence_);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Temporal: {
          auto data = static_cast<TemporalEncoderData *>(current);
          scope->inflight.temporal_frames.push_back(data->frame);
          EncodeOwnedTemporalFrame(data->frame, cmdbuf, fence_, owned_id_, scope->seq, current->id);
          break;
        }
        case EncoderType::Resolve: {
          auto data = static_cast<ResolveEncoderData *>(current);

          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          info.colors[0].texture = data->src.texture();
          info.colors[0].load_action = WMTLoadActionLoad;
          info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
          info.colors[0].resolve_texture = data->dst.texture();

          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(fence_, WMTRenderStageFragment);
          encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();

          break;
        }
        case EncoderType::Query: {
          auto data = static_cast<QueryEncoderData *>(current);
          switch (data->op) {
          case QueryEncoderOp::ResetOcclusion: {
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            wmtcmd_blit_fillbuffer fill{};
            fill.type = WMTBlitCommandFillBuffer;
            fill.next.set(nullptr);
            fill.buffer = data->source;
            fill.offset = uint64_t(data->start_index) * sizeof(uint64_t);
            fill.length = sizeof(uint64_t);
            fill.value = 0;
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::SampleTimestamp: {
            const uint64_t before = ++query_barrier_value_;
            cmdbuf.encodeSignalEvent(query_barrier_event_, before);
            cmdbuf.encodeWaitForEvent(query_barrier_event_, before);

            WMTSampleBufferAttachmentInfo sample{};
            sample.sample_buffer = data->timestamp_samples;
            sample.start_of_encoder_sample_index = data->start_index;
            sample.end_of_encoder_sample_index = ~0ull;
            auto encoder = cmdbuf.blitCommandEncoderWithSampleBuffers(&sample, 1);
            wmtcmd_blit_fillbuffer fill{};
            fill.type = WMTBlitCommandFillBuffer;
            fill.next.set(nullptr);
            fill.buffer = data->timestamp_dummy;
            fill.offset = 0;
            fill.length = 4;
            fill.value = 0;
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
            encoder.updateFence(fence_);
            encoder.endEncoding();

            const uint64_t after = ++query_barrier_value_;
            cmdbuf.encodeSignalEvent(query_barrier_event_, after);
            cmdbuf.encodeWaitForEvent(query_barrier_event_, after);
            data->timestamp_producers->record(data->start_index, cmdbuf);
            scope->inflight.timestamp_completions.emplace_back(data->timestamp_producers, data->start_index);
            break;
          }
          case QueryEncoderOp::ResolveOcclusion: {
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            wmtcmd_blit_copy_from_buffer_to_buffer copy{};
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.next.set(nullptr);
            copy.src = data->source;
            copy.src_offset = uint64_t(data->start_index) * sizeof(uint64_t);
            copy.dst = data->destination;
            copy.dst_offset = data->destination_offset;
            copy.copy_length = uint64_t(data->query_count) * sizeof(uint64_t);
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::ResolveBinaryOcclusion: {
            auto encoder = cmdbuf.computeCommandEncoder(false);
            encoder.waitForFence(fence_);
            wmtcmd_compute_nop head{};
            head.type = WMTComputeCommandNop;
            wmtcmd_compute_setpso set_pso{};
            set_pso.type = WMTComputeCommandSetPSO;
            set_pso.pso = data->compute_pipeline;
            set_pso.threadgroup_size = {64, 1, 1};
            wmtcmd_compute_setbuffer set_source{};
            set_source.type = WMTComputeCommandSetBuffer;
            set_source.buffer = data->source;
            set_source.offset = uint64_t(data->start_index) * sizeof(uint64_t);
            set_source.index = 0;
            wmtcmd_compute_setbuffer set_destination{};
            set_destination.type = WMTComputeCommandSetBuffer;
            set_destination.buffer = data->destination;
            set_destination.offset = data->destination_offset;
            set_destination.index = 1;
            wmtcmd_compute_dispatch dispatch{};
            dispatch.type = WMTComputeCommandDispatchThreads;
            dispatch.size = {data->query_count, 1, 1};
            head.next.set(&set_pso);
            set_pso.next.set(&set_source);
            set_source.next.set(&set_destination);
            set_destination.next.set(&dispatch);
            dispatch.next.set(nullptr);
            encoder.encodeCommands(&head);
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::ResolveTimestamp: {
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            encoder.resolveCounters(data->timestamp_samples, data->start_index, data->query_count,
                                    data->destination, data->destination_offset);
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          }
          break;
        }
        }
        current = current->next;
      }
    }
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    if (Count && !ppCommandLists) {
      ERR("ExecuteCommandLists: null command list array");
      return;
    }
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      ExecuteTask task;
      task.lists.reserve(Count);
      for (unsigned i = 0; i < Count; i++) {
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
        if (!list || !list->owned_allocator || !list->owned_closed_valid ||
            list->encoder_count == std::numeric_limits<size_t>::max() || list->GetType() != desc_.Type) {
          ERR("ExecuteCommandLists: invalid command list or allocator");
          return;
        }
        for (const auto &bundle : list->owned_bundle_references) {
          if (!bundle.valid()) {
            ERR("ExecuteCommandLists: bundle allocator was reset after recording");
            return;
          }
        }
        task.lists.push_back(ListSubmission{
            list->entry, list->owned_id, list->owned_recording, Com<ID3D12CommandAllocator>(list->owned_allocator)});
        if (!task.contains_temporal) {
          for (auto *encoder = list->entry; encoder; encoder = encoder->next) {
            if (encoder->type == EncoderType::Temporal) {
              task.contains_temporal = true;
              break;
            }
          }
        }
      }
      if (!task_fifo_.push(QueueTask{std::move(task)})) {
        ERR("ExecuteCommandLists: queue submission worker is closed");
      } else {
        for (unsigned i = 0; i < Count; ++i) {
          auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
          for (auto &bundle : list->owned_bundle_references)
            bundle.submitted();
        }
      }
    } catch (const std::bad_alloc &) {
      ERR("ExecuteCommandLists: out of memory while capturing submission");
    } catch (...) {
      ERR("ExecuteCommandLists: failed to capture submission");
    }
  }

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  void
  ProcessSignal(SignalTask &task) {
    uint64_t submission = 0;
    {
      auto scope = StartCommitting();
      submission = scope.seq;
      task.fence->encodeQueueSignal(scope.inflight.cmdbuf, task.ticket);
      owned_graphics::Trace("signal", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                            ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                            ",\"generation\":", task.ticket.generation);
    }
    // A consumer queue may start CPU encoding after the producer command
    // buffer is committed. Native GPU ordering is still enforced by its wait.
    task.fence->markQueueSignalCommitted(task.ticket);
    owned_graphics::Trace("signal_committed", ",\"queue\":", owned_id_, ",\"submission\":", submission,
                          ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                          ",\"generation\":", task.ticket.generation);
  }

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    if (!pFence)
      return E_INVALIDARG;
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      auto native = static_cast<MTLD3D12Fence *>(pFence);
      SignalTask task;
      task.fence = native->fence;
      task.fence_id = native->owned_id;
      auto preparation = task.fence->prepareQueueSignal(owned_id_, Value);
      if (!preparation) {
        owned_graphics::Trace("signal_order_unsupported", ",\"queue\":", owned_id_,
                              ",\"fence\":", task.fence_id, ",\"value\":", Value);
        return E_NOTIMPL;
      }
      task.ticket = preparation->ticket;
      try {
        if (task_fifo_.push(QueueTask{std::move(task)})) {
          native->fence->commitQueueSignal(*preparation);
          return S_OK;
        }
      } catch (const std::bad_alloc &) {
        native->fence->cancelQueueSignal(*preparation);
        return E_OUTOFMEMORY;
      } catch (...) {
        native->fence->cancelQueueSignal(*preparation);
        return E_FAIL;
      }
      native->fence->cancelQueueSignal(*preparation);
      return E_FAIL;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  };

  void
  ProcessWait(WaitTask &task) {
    if (!task.fence->queueWaitReady(task.ticket))
      owned_graphics::Trace("wait_defer", ",\"queue\":", owned_id_, ",\"fence\":", task.fence_id,
                            ",\"value\":", task.ticket.value, ",\"generation\":", task.ticket.generation);
    task.fence->waitForQueueProducer(task.ticket);

    auto scope = StartCommitting();
    task.fence->encodeQueueWait(scope.inflight.cmdbuf, task.ticket);
    owned_graphics::Trace("wait", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                          ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                          ",\"generation\":", task.ticket.generation);
  }

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    if (!pFence)
      return E_INVALIDARG;
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      auto native = static_cast<MTLD3D12Fence *>(pFence);
      WaitTask task;
      task.fence = native->fence;
      task.fence_id = native->owned_id;
      task.ticket = task.fence->prepareQueueWait(Value);
      return task_fifo_.push(QueueTask{std::move(task)}) ? S_OK : E_FAIL;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  };

  HRESULT STDMETHODCALLTYPE
  GetTimestampFrequency(UINT64 *pFrequency) {
    if (!pFrequency)
      return E_INVALIDARG;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_FAIL;
    // Metal timestamp counter samples are MTLTimestamp values, expressed in
    // nanoseconds. D3D12 reports counter ticks per second, so one tick/ns is 1 GHz.
    *pFrequency = 1000000000ull;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    return E_NOTIMPL;
  };

  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) {
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  void
  ProcessPresent(PresentTask &task) {
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.semaphore = task.completion_handle.release();

    auto g = task.backbuffer.ptr();
    auto &view = g->texture->view(g->texture->fullView);

    auto drawable = task.presenter->encodeCommands(
        cmdbuf, view.texture, task.state->metadata,
        [&](auto encoder) { encoder.waitForFence(fence_, WMTRenderStageFragment); },
        [&](auto encoder) { encoder.updateFence(fence_, WMTRenderStageFragment); }
    );

    if (task.after > 0)
      cmdbuf.presentDrawableAfterMinimumDuration(drawable, task.after);
    else
      cmdbuf.presentDrawable(drawable);
    owned_graphics::Trace("present_encode", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                          ",\"resource\":", g->owned_id);
    // Match the existing synchronous/D3D11 behavior: publish presenter CPU
    // completion after encode, before this command buffer is committed.
    task.state.reset();
  }

  HRESULT
  Present(Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after) {
    if (!presenter || !backbuffer)
      return E_INVALIDARG;
    try {
      // The swapchain owns its public handle and may be released while this
      // task is waiting behind a queue fence. Keep a duplicate until the
      // resulting GPU command buffer completes.
      HANDLE completion_handle = nullptr;
      if (hLantecyWaitable && !DuplicateHandle(GetCurrentProcess(), hLantecyWaitable,
                                              GetCurrentProcess(), &completion_handle, 0,
                                              FALSE, DUPLICATE_SAME_ACCESS))
        return HRESULT_FROM_WIN32(GetLastError());
      OwnedHandle retained_completion_handle(completion_handle);

      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      // D3D11 captures PresentState on the Present caller before deferring the
      // Metal encode. Keep D3D12's presenter/layer state on the same side of
      // the thread boundary; only the GPU encoding belongs to the queue worker.
      // Do this under the queue capture lock so concurrent Present calls cannot
      // snapshot layer/frame state in an order different from their FIFO tasks.
      auto state = std::make_unique<Presenter::PresentState>(presenter->synchronizeLayerProperties());
      PresentTask task;
      task.presenter = Rc<Presenter>(presenter);
      task.backbuffer = static_cast<MTLD3D12Resource *>(backbuffer);
      task.state = std::move(state);
      task.completion_handle = std::move(retained_completion_handle);
      task.after = after;
      return task_fifo_.push(QueueTask{std::move(task)}) ? S_OK : E_FAIL;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }
};

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  auto command_queue = Com(new MTLD3D12CommandQueueImpl(pDevice));
  HRESULT hr = command_queue->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  return command_queue->QueryInterface(riid, ppCommandQueue);
};

} // namespace dxmt
