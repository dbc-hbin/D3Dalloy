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
#include "d3d12_command_allocator.hpp"
#include "d3d12_enhanced_barrier.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_standard_swizzle_gpu.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "owned_submission_ring.hpp"
#include "owned_queue_task_fifo.hpp"
#include "d3d12_temporal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

constexpr auto kCommandQueueSize = 32u;

const GUID kD3D12CommandQueueDownlevelUUID = {
    0x38a8c5ef, 0x7ccb, 0x4e81, {0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94}
};

// The layout an enhanced barrier sees for one subresource, or nullopt when the subresource
// holds a legacy state other than COMMON (it must be transitioned to COMMON first).
// A creation state has the layout of the translation table; a legacy state reached only by
// promotion from COMMON is still in the COMMON layout.
static std::optional<UINT>
EnhancedLayoutOf(const BarrierSubresourceState &slot, const BarrierTracker &tracker) {
  if (slot.split)
    return std::nullopt;
  if (slot.layout)
    return UINT(*slot.layout);
  if (!slot.current || *slot.current == D3D12_RESOURCE_STATE_COMMON)
    return UINT(D3D12_BARRIER_LAYOUT_COMMON);
  if (tracker.broad_promotion)
    return slot.implicitly_promoted || slot.creation_state ? std::optional<UINT>(D3D12_BARRIER_LAYOUT_COMMON)
                                                           : std::nullopt;
  if (slot.implicitly_promoted &&
      !(enhanced_barrier::AccessFromLegacyState(*slot.current) & ~enhanced_barrier::LayoutAccessMask(
          D3D12_BARRIER_LAYOUT_COMMON)))
    return UINT(D3D12_BARRIER_LAYOUT_COMMON);
  if (slot.creation_state)
    return enhanced_barrier::LayoutFromLegacyState(*slot.current);
  return std::nullopt;
}

// One enhanced (ID3D12GraphicsCommandList7::Barrier) event against one subresource slot.
static bool
ApplyEnhancedToSlot(BarrierSubresourceState &slot, const RecordedStateEvent &event, const BarrierTracker &tracker,
                    bool allow_prior_split) {
  // Buffers, simultaneous-access textures and fixed-state heaps have no layout. Their barriers
  // synchronize only, but any explicit legacy state must have been returned to COMMON.
  if (tracker.broad_promotion || tracker.fixed_state)
    return tracker.fixed_state || (allow_prior_split && !slot.current) || EnhancedLayoutOf(slot, tracker).has_value();
  const BarrierLayoutSplit split{event.layout_before, event.layout_after, event.access_before, event.access_after};
  if (event.split_end && slot.layout_split) {
    if (*slot.layout_split != split)
      return false;
    slot.layout_split.reset();
  } else {
    // An intervening barrier on a subresource with a pending split, or an END with no BEGIN
    // outside of the API-capture pass (where the BEGIN may be in an earlier submission).
    if (slot.layout_split || (event.split_end && !allow_prior_split))
      return false;
    // At API capture (allow_prior_split) a subresource this batch has not touched yet has an
    // unknown layout: another queue may produce it behind a Wait. The FIFO worker always knows it.
    const bool unknown = allow_prior_split && ((!slot.current && !slot.layout) || slot.promoted_from_unknown);
    const auto current = EnhancedLayoutOf(slot, tracker);
    if (!unknown && !current)
      return false;
    if (!unknown && event.layout_before != D3D12_BARRIER_LAYOUT_UNDEFINED && UINT(event.layout_before) != *current)
      return false;
    if (event.split_begin) {
      slot.layout_split = split;
      return true;
    }
  }
  // UNDEFINED -> UNDEFINED is an access-only barrier and keeps the layout.
  if (event.layout_before == D3D12_BARRIER_LAYOUT_UNDEFINED && event.layout_after == D3D12_BARRIER_LAYOUT_UNDEFINED)
    return true;
  slot.implicitly_promoted = false;
  slot.creation_state = false;
  slot.promoted_from_unknown = false;
  if (event.layout_after == D3D12_BARRIER_LAYOUT_COMMON) {
    slot.layout.reset();
    slot.current = D3D12_RESOURCE_STATE_COMMON;
  } else {
    slot.layout = event.layout_after;
    slot.current.reset();
  }
  return true;
}

static bool
ApplyEnhancedEvent(BarrierResourceState &resource, const RecordedStateEvent &event, bool allow_prior_split) {
  const auto &tracker = *event.resource;
  if (event.subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    if (resource.all.layout_split || resource.all.split)
      return false;
    auto [it, inserted] = resource.subresources.try_emplace(event.subresource);
    if (inserted)
      it->second = resource.all;
    return ApplyEnhancedToSlot(it->second, event, tracker, allow_prior_split);
  }
  const bool partial = resource.subresources.size() < tracker.subresource_count;
  if (partial && !ApplyEnhancedToSlot(resource.all, event, tracker, allow_prior_split))
    return false;
  for (auto &[index, slot] : resource.subresources)
    if (!ApplyEnhancedToSlot(slot, event, tracker, allow_prior_split))
      return false;
  // A completed whole-resource barrier leaves every subresource in the same layout.
  if (!event.split_begin && !tracker.broad_promotion && !tracker.fixed_state &&
      !(event.layout_before == D3D12_BARRIER_LAYOUT_UNDEFINED &&
        event.layout_after == D3D12_BARRIER_LAYOUT_UNDEFINED)) {
    if (!partial)
      resource.all = resource.subresources.begin()->second;
    resource.subresources.clear();
  }
  return true;
}

// API capture knows only this Execute batch. The FIFO worker reuses the same
// check against persistent state after preceding submissions and queue Waits.
static bool
ApplyRecordedStateEvent(BarrierResourceState &resource, const RecordedStateEvent &transition,
                        bool allow_prior_split, D3D12_COMMAND_LIST_TYPE queue) {
  if (transition.enhanced)
    return ApplyEnhancedEvent(resource, transition, allow_prior_split);
  if (transition.implicit_use) {
    const auto required = transition.after;
    constexpr D3D12_RESOURCE_STATES read_states = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
        D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
        D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PREDICATION;
    auto use = [&](BarrierSubresourceState &slot) {
      if (slot.split || slot.layout_split)
        return false;
      // Barrier-free access to an enhanced layout: the access must be one the layout allows.
      if (slot.layout)
        return enhanced_barrier::LayoutPermitsAccess(*slot.layout, enhanced_barrier::AccessFromLegacyState(required),
                                                     queue);
      if (!slot.current || *slot.current == D3D12_RESOURCE_STATE_COMMON) {
        slot.promoted_from_unknown = !slot.current;
        slot.current = required;
        slot.implicitly_promoted = true;
        return true;
      }
      if ((*slot.current & required) == required)
        return true;
      if (slot.implicitly_promoted && !(*slot.current & ~read_states) && !(required & ~read_states)) {
        slot.current = D3D12_RESOURCE_STATES(*slot.current | required);
        return true;
      }
      return false;
    };
    if (resource.all.split)
      return false;
    if (transition.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
      if (resource.subresources.size() < transition.resource->subresource_count && !use(resource.all))
        return false;
      for (auto &[index, slot] : resource.subresources)
        if (!use(slot))
          return false;
    } else {
      if (resource.all.layout_split)
        return false;
      auto [it, inserted] = resource.subresources.try_emplace(transition.subresource);
      if (inserted)
        it->second = resource.all;
      if (!use(it->second))
        return false;
    }
    return true;
  }
  auto matches = [&](D3D12_RESOURCE_STATES current, bool implicitly_promoted) {
    if (current == transition.before)
      return true;
    constexpr D3D12_RESOURCE_STATES texture_reads = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr D3D12_RESOURCE_STATES texture_writes = D3D12_RESOURCE_STATE_COPY_DEST;
    constexpr D3D12_RESOURCE_STATES broad_reads = texture_reads |
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
        D3D12_RESOURCE_STATE_PREDICATION;
    constexpr D3D12_RESOURCE_STATES broad_writes = texture_writes | D3D12_RESOURCE_STATE_RENDER_TARGET |
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_STREAM_OUT |
        D3D12_RESOURCE_STATE_RESOLVE_DEST;
    const auto reads = transition.resource->broad_promotion ? broad_reads : texture_reads;
    const auto writes = transition.resource->broad_promotion ? broad_writes : texture_writes;
    if (current != D3D12_RESOURCE_STATE_COMMON) {
      // Read-only implicit promotions can accumulate across shader accesses
      // that are not represented as explicit barriers in this recording.
      return implicitly_promoted && !(current & ~reads) && !(transition.before & ~reads) &&
             (transition.before & current) == current;
    }
    const auto write_bits = transition.before & writes;
    return !(transition.before & ~(reads | writes)) &&
           (!write_bits || (write_bits == transition.before && std::has_single_bit(UINT(write_bits))));
  };
  const bool all = transition.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (!all && !resource.subresources.count(transition.subresource) && resource.all.layout)
    resource.subresources[transition.subresource] = resource.all;
  auto &slot = all ? resource.all : resource.subresources[transition.subresource];
  // A legacy barrier needs every affected subresource in the COMMON layout: an enhanced layout
  // other than COMMON (or a pending enhanced split) is a mismatch.
  const auto enhanced_layout = [](const BarrierSubresourceState &state) {
    return state.layout.has_value() || state.layout_split.has_value();
  };
  bool overlap = resource.all.split.has_value() || resource.all.layout_split.has_value();
  if (all) {
    for (const auto &[index, state] : resource.subresources)
      overlap |= state.split.has_value() || enhanced_layout(state);
    if (resource.subresources.size() < transition.resource->subresource_count)
      overlap |= enhanced_layout(resource.all);
  } else {
    overlap |= slot.split.has_value() || enhanced_layout(slot);
  }
  bool before_matches = true;
  if (all) {
    if (resource.subresources.size() < transition.resource->subresource_count && resource.all.current &&
        !matches(*resource.all.current, resource.all.implicitly_promoted))
      before_matches = false;
    for (const auto &[index, state] : resource.subresources)
      if (state.current && !matches(*state.current, state.implicitly_promoted))
        before_matches = false;
  } else if (slot.current) {
    before_matches = matches(*slot.current, slot.implicitly_promoted);
  } else if (resource.all.current) {
    before_matches = matches(*resource.all.current, resource.all.implicitly_promoted);
  }
  if (transition.flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) {
    if (overlap || !before_matches)
      return false;
    slot.split.emplace(transition.before, transition.after);
    return true;
  }
  if (transition.flags == D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) {
    if (slot.split) {
      if (*slot.split != std::pair{transition.before, transition.after})
        return false;
      slot.split.reset();
    } else if (!allow_prior_split || overlap || !before_matches) {
      return false;
    }
  } else if (overlap || !before_matches) {
    return false;
  }
  slot.current = transition.after;
  slot.implicitly_promoted = false;
  slot.creation_state = false;
  if (all)
    resource.subresources.clear();
  return true;
}

static bool CopyQueueStartsCommon(const BarrierResourceState &state, const RecordedStateEvent &event) {
  if (event.resource->fixed_state)
    return true;
  const auto common = [](const BarrierSubresourceState &slot) {
    return slot.current && *slot.current == D3D12_RESOURCE_STATE_COMMON && !slot.split && !slot.layout_split;
  };
  if (event.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    if (state.subresources.size() < event.resource->subresource_count && !common(state.all))
      return false;
    for (const auto &[index, slot] : state.subresources)
      if (!common(slot))
        return false;
    return true;
  }
  if (state.all.split)
    return false;
  auto it = state.subresources.find(event.subresource);
  return common(it == state.subresources.end() ? state.all : it->second);
}

class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLD3D12SwapChainFactory> {

  const uint64_t owned_id_ = owned_graphics::NewObjectId();
  D3D12_COMMAND_QUEUE_DESC desc_;

  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  WMT::Reference<WMT::Event> query_barrier_event_;
  WMT::Reference<WMT::SharedEvent> tile_mapping_event_;
  uint64_t tile_mapping_value_ = 0; // submission thread only
  /* Signal/wait pairs of this shared event flush sparse page aliases (a plain MTLEvent does not). */
  WMT::Reference<WMT::SharedEvent> alias_flush_event_;
  uint64_t alias_flush_value_ = 0; // submission thread only

  void
  EncodeAliasFlush(WMT::CommandBuffer cmdbuf) {
    const uint64_t value = ++alias_flush_value_;
    cmdbuf.encodeSignalEvent(alias_flush_event_, value);
    cmdbuf.encodeWaitForEvent(alias_flush_event_, value);
  }
  uint64_t query_barrier_value_ = 0;
  WMT::Reference<WMT::ComputePipelineState> swizzle_import_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_export_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_packed422_import_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_packed422_export_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_dirty_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_export_if_dirty_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_packed422_dirty_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> swizzle_packed422_export_if_dirty_pipeline_;

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

  // An allocator cannot discard recorded payloads while a FIFO task or any
  // Metal segment that reads them remains outstanding.  The lease is moved
  // into each segment without allocating a separate control block per list.
  class SubmissionLease {
    Com<ID3D12CommandAllocator> allocator_;
    bool active_ = false;

    SubmissionLease(Com<ID3D12CommandAllocator> allocator, bool active) :
        allocator_(std::move(allocator)), active_(active) {}

  public:
    SubmissionLease() = default;
    SubmissionLease(MTLD3D12CommandAllocatorImpl *allocator, uint64_t generation) :
        allocator_(allocator), active_(allocator->CaptureSubmission(generation)) {}
    SubmissionLease(const SubmissionLease &) = delete;
    SubmissionLease &operator=(const SubmissionLease &) = delete;
    SubmissionLease(SubmissionLease &&other) :
        allocator_(std::move(other.allocator_)), active_(std::exchange(other.active_, false)) {}
    SubmissionLease &operator=(SubmissionLease &&other) {
      if (this != &other) {
        if (active_)
          static_cast<MTLD3D12CommandAllocatorImpl *>(allocator_.ptr())->ReleaseSubmission();
        allocator_ = std::move(other.allocator_);
        active_ = std::exchange(other.active_, false);
      }
      return *this;
    }
    ~SubmissionLease() {
      if (active_)
        static_cast<MTLD3D12CommandAllocatorImpl *>(allocator_.ptr())->ReleaseSubmission();
    }
    explicit operator bool() const { return active_; }
    SubmissionLease Retain() const {
      auto *allocator = static_cast<MTLD3D12CommandAllocatorImpl *>(allocator_.ptr());
      allocator->RetainSubmission();
      return SubmissionLease(allocator_, true);
    }
  };

  // An entry head is one immutable closed recording, not the reusable COM
  // command-list object. A second submission of that same recording before
  // retirement is invalid, even across queues or within one API batch.
  class RecordingLease {
    std::atomic<uint32_t> *in_flight_ = nullptr;

    RecordingLease(std::atomic<uint32_t> *in_flight, bool) : in_flight_(in_flight) {}

  public:
    RecordingLease() = default;
    explicit RecordingLease(RecordingHead *head) : in_flight_(&head->in_flight) {
      uint32_t expected = 0;
      if (!in_flight_->compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        in_flight_ = nullptr;
    }
    RecordingLease(const RecordingLease &) = delete;
    RecordingLease &operator=(const RecordingLease &) = delete;
    RecordingLease(RecordingLease &&other) noexcept : in_flight_(std::exchange(other.in_flight_, nullptr)) {}
    RecordingLease &operator=(RecordingLease &&other) noexcept {
      if (this != &other) {
        if (in_flight_) {
          assert(in_flight_->load(std::memory_order_relaxed));
          in_flight_->fetch_sub(1, std::memory_order_acq_rel);
        }
        in_flight_ = std::exchange(other.in_flight_, nullptr);
      }
      return *this;
    }
    ~RecordingLease() {
      if (in_flight_) {
        assert(in_flight_->load(std::memory_order_relaxed));
        in_flight_->fetch_sub(1, std::memory_order_acq_rel);
      }
    }
    explicit operator bool() const { return in_flight_; }
    RecordingLease Retain() const {
      assert(in_flight_);
      in_flight_->fetch_add(1, std::memory_order_relaxed);
      return RecordingLease(in_flight_, true);
    }
  };

  struct SubmissionProtection {
    SubmissionLease allocator;
    RecordingLease recording; // Released before its owning allocator.
  };

  struct ListSubmission {
    EncoderData *entry = nullptr;
    uint64_t list_id = 0;
    uint64_t recording = 0;
    SubmissionLease allocator;
    RecordingLease recording_lease;
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
    WMT::Reference<WMT::PresentationObserver> observer;
    std::shared_ptr<std::atomic<bool>> observer_fault;
    std::shared_ptr<DXMTPresentLatency> latency;
    DXMTPresentComposition composition;
    uint64_t present_id = 0;
    uint64_t restart_epoch = 0;
    std::shared_ptr<std::atomic<bool>> credit;
    // Private: a pending present must not count as an application reference
    // when ResizeBuffers checks for outstanding backbuffer references.
    Com<MTLD3D12Resource, false> backbuffer;
    std::optional<Presenter::PresentState> state;
    OwnedHandle completion_handle;
    double after = 0;
  };

  /*
   * UpdateTileMappings / CopyTileMappings. Assignments are resolved at API time except for copies,
   * whose source mappings are read when the task runs, i.e. in queue order.
   */
  struct TileMappingTask {
    Com<MTLD3D12Resource, false> resource;
    std::vector<std::pair<uint32_t, ReservedTileTarget>> assignments;
    std::vector<WMT::Reference<WMT::Heap>> heaps; // keeps mapped heaps alive until applied
    Com<MTLD3D12Resource, false> copy_source;
    std::vector<uint32_t> copy_source_tiles;
    bool no_hazard = false;
  };

  using QueueTask = std::variant<ExecuteTask, SignalTask, WaitTask, PresentTask, TileMappingTask>;
  owned_graphics::QueueTaskFifo<QueueTask> task_fifo_;
  dxmt::mutex mutex_enqueue_;
  std::atomic<bool> stopping_ = false;
  bool cancel_unsubmitted_ = false; // Submission thread only, after an unresolved shutdown wait.
  std::mutex wait_mutex_;
  std::shared_ptr<owned_graphics::FenceCompletionLatch> blocked_wait_;

  struct InflightCommandBuffer {
    WMT::Reference<WMT::CommandBuffer> cmdbuf{};
    HANDLE semaphore{};
    std::shared_ptr<DXMTPresentLatency> latency;
    // Allocator GPU argument storage uses newBufferWithBytesNoCopy. Retaining
    // its MTLBuffer alone does not retain the allocator's malloc backing.
    // Keep that owner until the submitted command buffer has completed.
    std::vector<SubmissionProtection> protections;
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
    std::optional<SignalTask> fence_completion;
    // Retain the presenter until GPU completion, not merely task encoding.
    Rc<Presenter> present_completion;
    std::shared_ptr<DXMTPresentHistory> present_history;
    // Stream-output and pipeline-statistics draws of this submission: their capture buffers
    // stay alive until completion, which checks each assembly's invariant status word.
    std::vector<std::shared_ptr<StreamOutputDrawData>> so_draws;
    std::vector<WMT::Reference<WMT::Buffer>> so_indirect_buffers;
    std::vector<WMT::Reference<WMT::IndirectCommandBuffer>> so_indirect_icbs;
    std::vector<WMT::Reference<WMT::Buffer>> msaa_buffers;
    std::vector<WMT::Reference<WMT::Texture>> msaa_textures;
    struct ROVMarkerTexture {
      WMT::Reference<WMT::Texture> texture;
      uint32_t width, height, layers;
    };
    std::vector<ROVMarkerTexture> rov_markers;
    std::vector<WMT::Reference<WMT::Buffer>> swizzle_buffers;
    uint64_t swizzle_scratch_length = 0;
    std::vector<WMT::Reference<WMT::Buffer>> swizzle_dirty_flags;
    std::vector<WMT::Reference<WMT::Buffer>> swizzle_baselines;
    std::vector<Com<MTLD3D12Resource, false>> swizzle_resources;
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
      const bool metal_error = inflight.cmdbuf.status() == WMTCommandBufferStatusError;
      if (metal_error) {
        ERR("Device error: ", inflight.cmdbuf.error().description().getUTF8String());
        device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      } else {
        for (const auto &draw : inflight.so_draws)
          if (draw->status_cpu && draw->status_cpu[0]) {
            ERR("Draw capture: ", draw->status_cpu[0], " primitives lost an unclaimed capture record");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
          }
        for (const auto &[queries, index] : inflight.timestamp_completions)
          queries->completed(index, inflight.cmdbuf);
      }

      owned_graphics::Trace("complete", ",\"queue\":", owned_id_, ",\"submission\":", internal_seq,
                            ",\"status\":", uint32_t(inflight.cmdbuf.status()));
      if (inflight.present_completion) {
        if (metal_error)
          inflight.present_completion->cancelPresentOrder();
        else
          inflight.present_completion->notifyPresentCompleted();
      }
      if (inflight.semaphore) {
        inflight.latency->complete(inflight.semaphore);
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
        if (!metal_error)
          device_->ObserveRTAS(key);
        device_->ReleaseRTASLease(key, token);
      }
      if (inflight.rtas_submission)
        device_->RetireRTASSubmission(inflight.rtas_submission);

      // A public fence completion is an allocator-retirement watermark too:
      // after the app observes it, Reset of every preceding allocator must be
      // immediately legal. Release all native and allocator leases first.
      auto fence_completion = std::move(inflight.fence_completion);
      inflight = {};
      submission_ring_.retire(internal_seq);
      if (fence_completion && device_->RemovedReason() == S_OK)
        fence_completion->fence->markQueueSignalCompleted(fence_completion->ticket);

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
      if (cancel_unsubmitted_) {
        if (auto *present = std::get_if<PresentTask>(&task))
          CancelPresent(*present);
        else if (auto *wait = std::get_if<WaitTask>(&task))
          wait->fence->cancelQueueWait(wait->ticket);
      } else if (device_->RemovedReason() == S_OK)
        std::visit([this](auto &typed) { this->Process(typed); }, task);
      else if (auto *present = std::get_if<PresentTask>(&task))
        CancelPresent(*present);
      // Release allocator/fence/presenter/resource references as soon as the
      // task has been encoded instead of retaining the last task while idle.
      task = ExecuteTask{};
    }
  }

  void Process(ExecuteTask &task) { ProcessExecute(task); }
  void Process(SignalTask &task) { ProcessSignal(task); }
  void Process(WaitTask &task) { ProcessWait(task); }
  void Process(PresentTask &task) { ProcessPresent(task); }
  void Process(TileMappingTask &task) { ProcessTileMapping(task); }

  /*
   * Mapping updates are ordered on the CPU: Metal applies MTL4 sparse mapping updates when they are
   * enqueued, ignoring event waits on the MTL4 queue. Unless NO_HAZARD, every submission this queue
   * made earlier retires first, so it observed the old mapping; the update is then complete before
   * this worker commits anything later. This serializes the queue worker at each mapping update.
   */
  void
  ProcessTileMapping(TileMappingTask &task) {
    if (device_->RemovedReason() != S_OK)
      return;
    if (!task.no_hazard) {
      const uint64_t last = submission_ring_.lastPublished();
      if (last)
        submission_ring_.waitUntilRetired(last);
    }
    auto &reserved = *task.resource->reserved;
    if (task.copy_source) {
      for (size_t i = 0; i < task.copy_source_tiles.size(); i++)
        task.assignments[i].second = task.copy_source->reserved->CurrentTarget(task.copy_source_tiles[i]);
    }
    static std::mutex sparse_queue_mutex; // one device-wide MTL4 queue serves every D3D12 queue
    std::lock_guard<std::mutex> lock(sparse_queue_mutex);
    auto queue = device_->GetSparseMappingQueue();
    if (!queue || !tile_mapping_event_) {
      ERR("UpdateTileMappings: no Metal 4 queue for sparse mapping updates");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return;
    }
    WMT::Buffer buffer{};
    WMT::Texture texture{};
    if (task.resource->buffer)
      buffer = task.resource->buffer->current()->buffer();
    else
      texture = task.resource->texture->current()->texture();
    reserved.Apply(queue, buffer, texture, task.assignments);
    queue.signalEvent(tile_mapping_event_, ++tile_mapping_value_);
    if (!tile_mapping_event_.waitUntilSignaledValue(tile_mapping_value_, 10000)) {
      ERR("UpdateTileMappings: Metal did not complete a sparse mapping update");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_HUNG);
    }
    owned_graphics::Trace("tile_mapping", ",\"queue\":", owned_id_, ",\"resource\":", task.resource->owned_id,
                          ",\"tiles\":", task.assignments.size());
  }

  /* The reserved resource behind an API pointer of this device, or null. */
  MTLD3D12Resource *
  ReservedResourceOf(ID3D12Resource *resource) {
    if (!resource)
      return nullptr;
    Com<ID3D12Device> owner;
    if (FAILED(resource->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
        owner.ptr() != static_cast<ID3D12Device *>(device_))
      return nullptr;
    auto *native = static_cast<MTLD3D12Resource *>(resource);
    return native->reserved ? native : nullptr;
  }

  bool
  PushTileMapping(TileMappingTask &&task) {
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      if (device_->RemovedReason() != S_OK)
        return false;
      return task_fifo_.push(QueueTask{std::move(task)});
    } catch (const std::bad_alloc &) {
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return false;
    }
  }

  void CancelPresent(PresentTask &task) {
    if (task.observer) {
      task.observer.cancel(task.present_id);
      task.observer_fault->store(true, std::memory_order_release);
    }
    // A later present on another queue may already be waiting on this frame's GPU order event.
    // The duplicated semaphore is the frame-latency credit normally returned at GPU completion.
    task.presenter->cancelPresentOrder();
    if (task.state)
      task.presenter->unregisterQueuedPresent(task.state->frame_id);
    if (task.completion_handle.handle && task.credit &&
        task.credit->exchange(false, std::memory_order_acq_rel))
      task.latency->complete(task.completion_handle.handle);
  }

public:
  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLD3D12SwapChainFactory>(pDevice),
      inflight_cmdbuf_wait_thread_([this]() { this->CommandBufferWaitingThread(); }),
      submission_thread_([this]() { this->SubmissionThread(); }) {}

  ~MTLD3D12CommandQueueImpl() {
    stopping_.store(true, std::memory_order_release);
    task_fifo_.close();
    std::shared_ptr<owned_graphics::FenceCompletionLatch> blocked;
    {
      std::lock_guard<std::mutex> lock(wait_mutex_);
      blocked = blocked_wait_;
    }
    if (blocked)
      blocked->wake();
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
    if (!pDesc ||
        (pDesc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT && pDesc->Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         pDesc->Type != D3D12_COMMAND_LIST_TYPE_COPY) ||
        (pDesc->Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
         pDesc->Priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH &&
         pDesc->Priority != D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) ||
        (pDesc->Flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT) || pDesc->NodeMask > 1)
      return E_INVALIDARG;
    desc_ = *pDesc;
    desc_.NodeMask = 1; // one physical node

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
    tile_mapping_event_ = metal_device.newSharedEvent();
    if (!tile_mapping_event_)
      return E_FAIL;
    alias_flush_event_ = metal_device.newSharedEvent();
    if (!alias_flush_event_)
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

    if (riid == __uuidof(IMTLD3D12SwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLD3D12SwapChainFactory>(this);
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

  /*
   * Invalid parameters are dropped with an error, as the D3D12 runtime does: the mapping state is left
   * unchanged and the device is not removed.
   */
  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (device_->RemovedReason() != S_OK)
      return;
    auto *native = ReservedResourceOf(resource);
    if (!native || (flags & ~D3D12_TILE_MAPPING_FLAG_NO_HAZARD)) {
      ERR("UpdateTileMappings: not a reserved resource of this device, or invalid flags");
      return;
    }
    const auto &reserved = *native->reserved;
    try {
      /* Regions: the tiles to update, in D3D12 walk order. */
      std::vector<uint32_t> tiles;
      if (region_count > 1 && !region_start_coordinates) {
        ERR("UpdateTileMappings: region coordinates are required for more than one region");
        return;
      }
      for (UINT i = 0; i < region_count; i++) {
        const D3D12_TILED_RESOURCE_COORDINATE start =
            region_start_coordinates ? region_start_coordinates[i] : D3D12_TILED_RESOURCE_COORDINATE{};
        D3D12_TILE_REGION_SIZE size{1, FALSE, 0, 0, 0};
        if (region_sizes)
          size = region_sizes[i];
        else if (!region_start_coordinates)
          size.NumTiles = reserved.totalTiles(); // the whole resource
        if (!reserved.ExpandRegion(start, size, tiles)) {
          ERR("UpdateTileMappings: region ", i, " is outside the reserved resource");
          return;
        }
      }

      /* Ranges: where the tiles go. */
      MTLD3D12Heap *native_heap = nullptr;
      uint64_t heap_tiles = 0;
      if (heap) {
        Com<ID3D12Device> heap_owner;
        native_heap = dynamic_cast<MTLD3D12Heap *>(heap);
        if (!native_heap || FAILED(heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&heap_owner))) ||
            heap_owner.ptr() != static_cast<ID3D12Device *>(device_)) {
          ERR("UpdateTileMappings: heap of another device");
          return;
        }
        const auto heap_desc = native_heap->GetDesc();
        const auto desc = resource->GetDesc();
        const bool rt_ds = desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        const D3D12_HEAP_FLAGS deny = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_HEAP_FLAG_DENY_BUFFERS
                                      : rt_ds ? D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES
                                              : D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        if (heap_desc.Flags & deny) {
          ERR("UpdateTileMappings: heap flags deny this resource kind");
          return;
        }
#ifdef __i386__
        /* A 32-bit process backs CPU-visible heaps with host memory the Metal heap does not alias. */
        if (native_heap->host_memory) {
          ERR("UpdateTileMappings: CPU-visible heaps cannot back tiles in a 32-bit process");
          return;
        }
#endif
        heap_tiles = heap_desc.SizeInBytes / kReservedTileBytes;
      }
      TileMappingTask task;
      task.no_hazard = flags & D3D12_TILE_MAPPING_FLAG_NO_HAZARD;
      if (native_heap)
        task.heaps.push_back(native_heap->heap);
      size_t next = 0;
      for (UINT i = 0; i < range_count; i++) {
        const D3D12_TILE_RANGE_FLAGS range = range_flags ? range_flags[i] : D3D12_TILE_RANGE_FLAG_NONE;
        const uint64_t count = range_tile_counts ? range_tile_counts[i] : range_count == 1 ? tiles.size() : 1;
        if ((range != D3D12_TILE_RANGE_FLAG_NONE && range != D3D12_TILE_RANGE_FLAG_NULL &&
             range != D3D12_TILE_RANGE_FLAG_SKIP && range != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) ||
            count > tiles.size() - next) {
          ERR("UpdateTileMappings: range ", i, " is invalid or covers more tiles than the regions");
          return;
        }
        const bool uses_heap = range == D3D12_TILE_RANGE_FLAG_NONE || range == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
        uint64_t offset = 0;
        if (uses_heap) {
          if (!native_heap || !heap_range_offsets) {
            ERR("UpdateTileMappings: range ", i, " maps tiles without a heap or heap offsets");
            return;
          }
          offset = heap_range_offsets[i];
          const uint64_t used = range == D3D12_TILE_RANGE_FLAG_NONE ? count : 1;
          if ((count || range == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) && (offset >= heap_tiles || used > heap_tiles - offset)) {
            ERR("UpdateTileMappings: range ", i, " is outside the heap");
            return;
          }
        }
        for (uint64_t k = 0; k < count; k++, next++) {
          switch (range) {
          case D3D12_TILE_RANGE_FLAG_SKIP:
            break;
          case D3D12_TILE_RANGE_FLAG_NULL:
            task.assignments.emplace_back(tiles[next], ReservedTileTarget{});
            break;
          case D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE:
            task.assignments.emplace_back(tiles[next], ReservedTileTarget{native_heap->heap, uint32_t(offset)});
            break;
          default:
            task.assignments.emplace_back(tiles[next], ReservedTileTarget{native_heap->heap, uint32_t(offset + k)});
            break;
          }
        }
      }
      if (next != tiles.size()) {
        ERR("UpdateTileMappings: ranges cover ", next, " tiles but regions ", tiles.size());
        return;
      }
      task.resource = native;
      PushTileMapping(std::move(task));
    } catch (const std::bad_alloc &) {
      ERR("UpdateTileMappings: out of memory");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
    }
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (device_->RemovedReason() != S_OK)
      return;
    auto *dst = ReservedResourceOf(dst_resource);
    auto *src = ReservedResourceOf(src_resource);
    if (!dst || !src || !dst_region_start_coordinate || !src_region_start_coordinate || !region_size ||
        (flags & ~D3D12_TILE_MAPPING_FLAG_NO_HAZARD)) {
      ERR("CopyTileMappings: invalid parameters");
      return;
    }
    try {
      std::vector<uint32_t> dst_tiles, src_tiles;
      if (!dst->reserved->ExpandRegion(*dst_region_start_coordinate, *region_size, dst_tiles) ||
          !src->reserved->ExpandRegion(*src_region_start_coordinate, *region_size, src_tiles) ||
          dst_tiles.size() != src_tiles.size()) {
        ERR("CopyTileMappings: region is outside a reserved resource");
        return;
      }
      /* Buffer and texture mappings share heap pages but not tile kinds. */
      if (dst->reserved->isBuffer() != src->reserved->isBuffer()) {
        ERR("CopyTileMappings: source and destination kinds differ");
        return;
      }
      TileMappingTask task;
      task.resource = dst;
      task.copy_source = src;
      task.copy_source_tiles = std::move(src_tiles);
      task.no_hazard = flags & D3D12_TILE_MAPPING_FLAG_NO_HAZARD;
      for (auto tile : dst_tiles)
        task.assignments.emplace_back(tile, ReservedTileTarget{});
      PushTileMapping(std::move(task));
    } catch (const std::bad_alloc &) {
      ERR("CopyTileMappings: out of memory");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
    }
  };

  /*
   * Per-execution setup of the captured draws of `render`, encoded before its render pass:
   * zero the capture claim words and, for indexed draws, regenerate the IA position buffer the
   * draw is issued over (StreamOutputDrawData). The draws are retained by the submission, which
   * checks their assembly status when it completes.
   */
  void
  EncodeStreamOutputSetup(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                          const std::vector<std::shared_ptr<StreamOutputDrawData>> &draws) {
    auto blit = cmdbuf.blitCommandEncoder();
    blit.waitForFence(fence_);
    for (const auto &draw : draws) {
      if (!draw->claims_bytes)
        continue;
      wmtcmd_blit_fillbuffer fill{};
      fill.type = WMTBlitCommandFillBuffer;
      fill.next.set(nullptr);
      fill.buffer = draw->scratch;
      fill.offset = draw->claims_offset;
      fill.length = draw->claims_bytes;
      fill.value = 0;
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
    }
    blit.updateFence(fence_);
    blit.endEncoding();
    for (const auto &draw : draws) {
      inflight.so_draws.push_back(draw);
      if (!draw->indexed || !draw->vertex_count)
        continue;
      auto encoder = cmdbuf.computeCommandEncoder(false);
      encoder.waitForFence(fence_);
      wmtcmd_compute_nop head{};
      head.type = WMTComputeCommandNop;
      wmtcmd_compute_setpso set_pso{};
      set_pso.type = WMTComputeCommandSetPSO;
      set_pso.pso = draw->positions_pipeline;
      set_pso.threadgroup_size = {64, 1, 1};
      std::array<wmtcmd_compute_setbuffer, 3> buffers{};
      const std::pair<WMT::Buffer, uint64_t> bound[] = {
          {draw->heap, draw->params_offset}, {draw->index_buffer, draw->index_offset}, {draw->positions, 0}};
      for (uint8_t i = 0; i < 3; i++) {
        buffers[i].type = WMTComputeCommandSetBuffer;
        buffers[i].buffer = bound[i].first;
        buffers[i].offset = bound[i].second;
        buffers[i].index = i;
      }
      wmtcmd_compute_dispatch dispatch{};
      dispatch.type = WMTComputeCommandDispatchThreads;
      dispatch.size = {draw->vertex_count, 1, 1};
      head.next.set(&set_pso);
      set_pso.next.set(&buffers[0]);
      buffers[0].next.set(&buffers[1]);
      buffers[1].next.set(&buffers[2]);
      buffers[2].next.set(&dispatch);
      dispatch.next.set(nullptr);
      encoder.encodeCommands(&head);
      encoder.updateFence(fence_);
      encoder.endEncoding();
    }
  }

  /* Runs the serial assembly of one captured draw after its render pass (API order). */
  void
  EncodeStreamOutputAssembly(WMT::CommandBuffer cmdbuf, const StreamOutputDrawData &draw) {
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_nop head{};
    head.type = WMTComputeCommandNop;
    wmtcmd_compute_setpso set_pso{};
    set_pso.type = WMTComputeCommandSetPSO;
    set_pso.pso = draw.assemble_pipeline;
    set_pso.threadgroup_size = {1, 1, 1};
    std::array<wmtcmd_compute_setbuffer, 16> buffers{};
    auto bind = [&](uint8_t index, WMT::Buffer buffer, uint64_t offset) {
      auto &binding = buffers[index];
      binding.type = WMTComputeCommandSetBuffer;
      binding.buffer = buffer;
      binding.offset = offset;
      binding.index = index;
    };
    bind(0, draw.heap, draw.params_offset);
    bind(1, draw.heap, draw.control_offset);
    bind(2, draw.scratch, 0);
    bind(3, draw.indexed ? draw.index_buffer : draw.heap, draw.indexed ? draw.index_offset : 0);
    bind(4, draw.counters_heap, draw.counters_offset);
    bind(5, draw.predicate_heap, draw.predicate_offset);
    bind(14, draw.heap, draw.status_offset);
    // Ordinary stream output has no statistics heap; the assembler ignores this slot then.
    bind(15, draw.statistics_heap ? draw.statistics_heap : draw.heap,
         draw.statistics_heap ? draw.statistics_offset : 0);
    for (uint8_t slot = 0; slot < 4; slot++) {
      // Unbound slots are never dereferenced; bind the heap so every argument is a valid buffer.
      if (draw.targets[slot]) {
        bind(6 + slot, draw.targets[slot]->buffer(), draw.target_offsets[slot]);
        bind(10 + slot, draw.filled_sizes[slot]->buffer(), draw.filled_size_offsets[slot]);
      } else {
        bind(6 + slot, draw.heap, 0);
        bind(10 + slot, draw.heap, 0);
      }
    }
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {1, 1, 1};
    head.next.set(&set_pso);
    set_pso.next.set(&buffers[0]);
    for (size_t i = 0; i + 1 < buffers.size(); i++)
      buffers[i].next.set(&buffers[i + 1]);
    buffers.back().next.set(&dispatch);
    dispatch.next.set(nullptr);
    encoder.encodeCommands(&head);
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

  struct SOIndirectExecution {
    WMT::Reference<WMT::Buffer> storage;
    WMT::Reference<WMT::IndirectCommandBuffer> icb;
    std::vector<std::shared_ptr<StreamOutputDrawData>> draws;
    const IndirectRenderCommandData *command = nullptr;
    const SOIndirectCaptureBinding *bindings = nullptr;
    uint64_t root_offset = 0;
    uint64_t vertex_offset = 0;
    uint64_t command_offset = 0;
    uint32_t count = 0;
  };

  struct MSAAExecution {
    WMT::Reference<WMT::Buffer> vertices;
    WMT::Reference<WMT::Buffer> indices;
    WMT::Reference<WMT::Buffer> draws;
    WMT::Reference<WMT::Buffer> capture_dispatch;
    WMT::Reference<WMT::Buffer> bounds;
    WMT::Reference<WMT::Buffer> parameters;
    const MSAAPrimitiveTileBounds *mapped_bounds = nullptr;
    uint32_t max_primitives = 0;
  };

  // The hidden ROG texture orders accesses through nested descriptor buffers.
  // Reuse a submission's scratch across serial passes, but never let two
  // simultaneous native samples share one texel. The in-flight pool retains
  // every version until GPU completion, including textures replaced by growth.
  bool PrepareROVMarkers(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                         uint32_t width, uint32_t height, uint32_t layers, uint32_t samples,
                         std::array<WMT::Texture, 8> &markers) {
    if (!samples || samples > markers.size()) return false;
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    layers = std::max(layers, 1u);
    std::array<size_t, 8> selected{};
    for (uint32_t sample = 0; sample < samples; ++sample) {
      size_t slot = inflight.rov_markers.size();
      for (size_t i = 0; i < inflight.rov_markers.size(); ++i) {
        const auto &scratch = inflight.rov_markers[i];
        if (scratch.width >= width && scratch.height >= height && scratch.layers >= layers &&
            std::find(selected.begin(), selected.begin() + sample, i) == selected.begin() + sample) {
          slot = i;
          break;
        }
      }
      const bool new_texture = slot == inflight.rov_markers.size();
      if (new_texture) {
        WMTTextureInfo info{};
        info.type = WMTTextureType2DArray;
        info.width = width;
        info.height = height;
        info.depth = 1;
        info.array_length = layers;
        info.mipmap_level_count = 1;
        info.sample_count = 1;
        info.pixel_format = WMTPixelFormatR8Uint;
        info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
        info.usage = WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
        auto texture = device_->GetMTLDevice().newTexture(info);
        if (!texture) return false;
        inflight.rov_markers.push_back({texture, width, height, layers});
      }
      selected[sample] = slot;
      const auto &scratch = inflight.rov_markers[slot];
      markers[sample] = scratch.texture;
      // The marker is an ordering token, not a counter visible to the app.
      // Any already-defined value (including wraparound) is sufficient when
      // this texture is reused. Initialize only newly allocated private memory.
      if (!new_texture) continue;
      WMTRenderPassInfo clear{};
      WMT::InitializeRenderPassInfo(clear);
      clear.colors[0].texture = scratch.texture;
      clear.colors[0].load_action = WMTLoadActionClear;
      clear.colors[0].store_action = WMTStoreActionStore;
      clear.render_target_array_length = scratch.layers;
      clear.render_target_width = scratch.width;
      clear.render_target_height = scratch.height;
      auto encoder = cmdbuf.renderCommandEncoder(clear);
      encoder.waitForFence(fence_, WMTRenderStageFragment);
      encoder.updateFence(fence_, WMTRenderStageFragment);
      encoder.endEncoding();
    }
    return true;
  }

  bool EncodeMSAAProducer(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                          RenderEncoderData &render, const MSAADrawData &draw,
                          MSAAExecution &execution) {
    auto metal = device_->GetMTLDevice();
    const uint64_t record_count = uint64_t(draw.assembly.vertex_count) * draw.assembly.instance_count;
    const uint64_t max_primitives = draw.assembly.max_primitives;
    const uint64_t max_buffer = metal.maxBufferLength();
    const uint64_t vertex_stride = uint64_t(draw.vertex_layout.record_dwords) * 4;
    if (record_count > UINT32_MAX || max_primitives > UINT32_MAX ||
        !vertex_stride || record_count > max_buffer / vertex_stride ||
        max_primitives > max_buffer / ((draw.line_mode == SM50_MSAA_LINE_NONE ? 3 : 20) * sizeof(uint32_t)) ||
        max_primitives > max_buffer / sizeof(MSAAPrimitiveIndirectArguments) ||
        max_primitives > max_buffer / sizeof(MSAAPrimitiveTileBounds))
      return false;
    auto allocate = [&](uint64_t length, bool cpu_read, const void **mapped = nullptr)
        -> WMT::Reference<WMT::Buffer> {
      if (length > max_buffer) return {};
      WMTBufferInfo info{};
      info.length = std::max<uint64_t>(length, 16);
      info.options = WMTResourceHazardTrackingModeUntracked |
                     (cpu_read ? WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE
                               : WMTResourceStorageModePrivate);
      auto buffer = metal.newBuffer(info);
      if (mapped) *mapped = info.memory.get_accessible_or_null();
      if (buffer) inflight.msaa_buffers.push_back(buffer);
      return buffer;
    };
    execution.vertices = allocate(record_count * uint64_t(draw.vertex_layout.record_dwords) * 4, false);
    execution.indices = allocate(max_primitives *
        (draw.line_mode == SM50_MSAA_LINE_NONE ? 3 : 20) * sizeof(uint32_t), false);
    execution.draws = allocate(max_primitives * sizeof(MSAAPrimitiveIndirectArguments), false);
    execution.capture_dispatch = allocate(16, false);
    const void *mapped = nullptr;
    execution.bounds = allocate(max_primitives * sizeof(MSAAPrimitiveTileBounds), true, &mapped);
    execution.mapped_bounds = static_cast<const MSAAPrimitiveTileBounds *>(mapped);
    execution.max_primitives = uint32_t(max_primitives);
    const void *parameters_memory = nullptr;
    auto parameters = allocate(sizeof(MSAAPrimitiveAssemblyParams) + sizeof(MSAAPrimitiveBoundsParams) +
                               sizeof(SM50_MSAA_VERTEX_CAPTURE_CONTROL),
                               true, &parameters_memory);
    execution.parameters = parameters;
    if (!execution.vertices || !execution.indices || !execution.draws ||
        !execution.capture_dispatch || !execution.bounds ||
        !execution.mapped_bounds || !parameters || !parameters_memory)
      return false;
    auto *parameters_bytes = static_cast<uint8_t *>(const_cast<void *>(parameters_memory));
    memcpy(parameters_bytes, &draw.assembly, sizeof(draw.assembly));
    constexpr uint64_t bounds_offset = sizeof(MSAAPrimitiveAssemblyParams);
    memcpy(parameters_bytes + bounds_offset, &draw.bounds, sizeof(draw.bounds));
    constexpr uint64_t control_offset = bounds_offset + sizeof(MSAAPrimitiveBoundsParams);
    auto *capture_control = reinterpret_cast<SM50_MSAA_VERTEX_CAPTURE_CONTROL *>(
        parameters_bytes + control_offset);
    auto *control = &capture_control->capture;
    *control = {};
    control->positions_per_instance = draw.assembly.vertex_count;
    control->record_count = uint32_t(record_count);
    control->index_size = draw.indexed ? draw.assembly.index_size : 0;
    control->index_first = draw.indexed ? draw.assembly.index_skip / 2 : 0;
    control->base_vertex = draw.indexed ? draw.base_vertex : int32_t(draw.start_vertex);
    control->padding[0] = draw.start_instance;
    control->padding[1] = draw.start_vertex;
    capture_control->strip_cut_enabled = draw.assembly.cut_enabled;
    const uint32_t vpp = draw.assembly.topology == 0 ? 1 : draw.assembly.topology <= 2 ? 2 : 3;
    capture_control->active_positions_per_instance =
        draw.assembly.topology == 2 || draw.assembly.topology == 4
                   ? draw.assembly.vertex_count >= vpp ? draw.assembly.vertex_count : 0
                   : draw.assembly.vertex_count / vpp * vpp;

    auto encode_kernel = [&](WMT::ComputePipelineState pipeline, WMTSize threads, WMTSize group,
                             const std::array<std::pair<WMT::Buffer, uint64_t>, 6> &bindings) {
      auto encoder = cmdbuf.computeCommandEncoder(false);
      encoder.waitForFence(fence_);
      wmtcmd_compute_setpso pso{};
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = pipeline;
      pso.threadgroup_size = group;
      for (uint8_t i = 0; i < 6; i++) {
        wmtcmd_compute_setbuffer binding{};
        binding.type = WMTComputeCommandSetBuffer;
        binding.buffer = bindings[i].first;
        binding.offset = bindings[i].second;
        binding.index = i;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
      }
      wmtcmd_compute_dispatch dispatch{};
      dispatch.type = WMTComputeCommandDispatchThreads;
      dispatch.size = threads;
      // The Winemetal bridge tracks threadsPerThreadgroup within one encoded
      // command chain, not across separate encodeCommands calls.
      pso.next.set(&dispatch);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
      encoder.updateFence(fence_);
      encoder.endEncoding();
    };
    const auto fallback = WMT::Buffer(parameters);
    encode_kernel(draw.assembly_pipeline, {1, 1, 1}, {1, 1, 1}, {{
        {parameters, 0},
        {draw.indexed ? WMT::Buffer(draw.index_buffer) : fallback,
         draw.indexed ? draw.index_offset & ~uint64_t(3) : 0},
        {draw.predicate_buffer ? WMT::Buffer(draw.predicate_buffer) : fallback,
         draw.predicate_buffer ? draw.predicate_offset : 0},
        {execution.indices, 0}, {execution.draws, 0}, {execution.capture_dispatch, 0}}});

    auto capture = cmdbuf.computeCommandEncoder(false);
    capture.waitForFence(fence_);
    wmtcmd_compute_setpso capture_pso{};
    capture_pso.type = WMTComputeCommandSetPSO;
    capture_pso.pso = draw.capture_pipeline;
    capture_pso.threadgroup_size = {64, 1, 1};
    for (auto *node = static_cast<wmtcmd_base *>(render.cmd_head.next.get()); node;
         node = static_cast<wmtcmd_base *>(node->next.get())) {
      if (node->type != WMTRenderCommandSetVertexBuffer) continue;
      const auto *source = reinterpret_cast<const wmtcmd_render_setbuffer *>(node);
      if (source->index == SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS ||
          source->index == SM50_BINDING_INDEX_VERTEX_INDICES ||
          source->index == SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL ||
          source->index == SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH) continue;
      wmtcmd_compute_setbuffer binding{};
      binding.type = WMTComputeCommandSetBuffer;
      binding.buffer = source->buffer;
      binding.offset = source->offset;
      binding.index = source->index;
      capture.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
    }
    auto bind_capture = [&](WMT::Buffer buffer, uint64_t offset, uint8_t index) {
      wmtcmd_compute_setbuffer binding{};
      binding.type = WMTComputeCommandSetBuffer;
      binding.buffer = buffer;
      binding.offset = offset;
      binding.index = index;
      capture.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
    };
    bind_capture(execution.vertices, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
    if (draw.root_buffer)
      bind_capture(draw.root_buffer, draw.root_offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
    if (draw.vertex_buffer_table)
      bind_capture(draw.vertex_buffer_table, draw.vertex_buffer_offset, SM50_BINDING_INDEX_VERTEX_BUFFER);
    if (draw.static_sampler_buffer)
      bind_capture(draw.static_sampler_buffer, draw.static_sampler_offset, SM50_BINDING_INDEX_STATIC_SAMPLERS);
    bind_capture(parameters, control_offset, SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL);
    bind_capture(draw.indexed ? WMT::Buffer(draw.index_buffer) : WMT::Buffer(parameters),
                 draw.indexed ? draw.index_offset & ~uint64_t(3) : 0,
                 SM50_BINDING_INDEX_VERTEX_INDICES);
    bind_capture(draw.stream_output ? WMT::Buffer(draw.stream_output->scratch) : WMT::Buffer(parameters),
                 0, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
    if (draw.constants[0])
      bind_capture(draw.constants[0], draw.constant_offsets[0], SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
    wmtcmd_compute_dispatch_indirect dispatch{};
    dispatch.type = WMTComputeCommandDispatchIndirect;
    dispatch.indirect_args_buffer = execution.capture_dispatch;
    dispatch.indirect_args_offset = 0;
    capture_pso.next.set(&dispatch);
    capture.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&capture_pso));
    capture.updateFence(fence_);
    capture.endEncoding();

    if (max_primitives)
      encode_kernel(draw.bounds_pipeline, {max_primitives, 1, 1}, {64, 1, 1}, {{
          {parameters, bounds_offset}, {execution.vertices, 0}, {execution.indices, 0},
          {execution.draws, 0}, {execution.bounds, 0}, {fallback, 0}}});
    return true;
  }

  bool EncodeMSAARender(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                        RenderEncoderData &render, const MSAADrawData &draw,
                        const MSAAExecution &execution) {
    auto metal = device_->GetMTLDevice();
    WMTTextureInfo coverage_info{};
    const uint32_t layer_count = std::max(1u, uint32_t(render.render_target_array_length));
    uint32_t render_width = render.render_target_width;
    uint32_t render_height = render.render_target_height;
    bool has_color_attachment = false;
    for (uint32_t i = 0; i < render.render_target_count; ++i)
      has_color_attachment |= bool(render.colors[i].attachment || render.colors[i].buffer_attachment);
    if (!has_color_attachment && !render.depth.attachment && !render.stencil.attachment) {
      // Attachmentless draws have a 16K default extent. Size the software
      // coverage target to the GPU-produced primitive bounds instead.
      render_width = render_height = 1;
      for (uint32_t primitive = 0; primitive < execution.max_primitives; ++primitive) {
        render_width = std::max(render_width, execution.mapped_bounds[primitive].right);
        render_height = std::max(render_height, execution.mapped_bounds[primitive].bottom);
      }
    }
    std::array<WMT::Texture, 8> rov_markers{};
    if (draw.rov_marker_sample_count && !PrepareROVMarkers(
            cmdbuf, inflight, render_width, render_height,
            draw.sample_pixel_pipeline ? layer_count : 1u,
            draw.rov_marker_sample_count, rov_markers))
      return false;
    const bool multisampled = draw.sample_count > 1;
    const auto coverage_view_type = multisampled ? WMTTextureType2DMultisample : WMTTextureType2D;
    coverage_info.type = layer_count > 1
        ? multisampled ? WMTTextureType2DMultisampleArray : WMTTextureType2DArray
        : coverage_view_type;
    coverage_info.width = render_width;
    coverage_info.height = render_height;
    coverage_info.depth = 1;
    coverage_info.array_length = layer_count;
    coverage_info.mipmap_level_count = 1;
    coverage_info.sample_count = draw.sample_count;
    coverage_info.pixel_format = WMTPixelFormatR32Uint;
    coverage_info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
    coverage_info.usage = WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead;
    auto coverage = metal.newTexture(coverage_info);
    if (!coverage) return false;
    inflight.msaa_textures.push_back(coverage);
    std::vector<WMT::Reference<WMT::Texture>> layer_views(layer_count);
    WMT::Texture sampled_coverage = coverage;
    WMT::Reference<WMT::Texture> post_coverage;
    std::vector<WMT::Reference<WMT::Texture>> post_views;
    if (draw.early_depth_pipeline && (draw.pixel_layout.flags & SM50_MSAA_PIXEL_FORCE_EARLY)) {
      post_coverage = metal.newTexture(coverage_info);
      if (!post_coverage) return false;
      inflight.msaa_textures.push_back(post_coverage);
      post_views.resize(layer_count);
    }
    const uint64_t input_bytes = uint64_t(draw.pixel_layout.input_record_count) *
                                 draw.pixel_layout.input_record_dwords * 4;
    const bool pixel_frequency = bool(draw.pixel_pipeline);
    const uint64_t output_bytes = uint64_t(64) * draw.pixel_layout.output_record_dwords * 4;
    if (!input_bytes || input_bytes > metal.maxBufferLength() ||
        (pixel_frequency && (!output_bytes || output_bytes > metal.maxBufferLength()))) return false;
    auto make_scratch = [&](uint64_t size) {
      WMTBufferInfo info{};
      info.length = size;
      info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
      auto buffer = metal.newBuffer(info);
      if (buffer) inflight.msaa_buffers.push_back(buffer);
      return buffer;
    };
    auto inputs = make_scratch(input_bytes);
    auto outputs = pixel_frequency ? make_scratch(output_bytes) : WMT::Reference<WMT::Buffer>{};
    if (!inputs || (pixel_frequency && !outputs)) return false;
    const bool early = draw.pixel_layout.flags & SM50_MSAA_PIXEL_FORCE_EARLY;
    auto bind_constants = [&](WMT::RenderCommandEncoder &encoder, uint32_t vertex, uint32_t fragment) {
      if (draw.constants[vertex])
        encoder.setVertexBuffer(draw.constants[vertex], draw.constant_offsets[vertex],
                                SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
      if (draw.constants[fragment])
        encoder.setFragmentBuffer(draw.constants[fragment], draw.constant_offsets[fragment],
                                  SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
    };
    auto bind_rov_markers = [&](WMT::RenderCommandEncoder &encoder, uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
        encoder.setFragmentTexture(rov_markers[i], 31 + i);
        encoder.useResource(rov_markers[i], WMTResourceUsageRead | WMTResourceUsageWrite,
                            WMTRenderStageFragment);
      }
    };
    auto bind_tile = [&](WMT::RenderCommandEncoder &encoder, const SM50_MSAA_TILE_CONTROL &tile,
                         uint32_t vertex, uint32_t fragment, bool gather = false) {
      // The recorded state chain contains the application's PSO. Binding it to
      // our integer coverage target fails Metal validation even if immediately
      // replaced. Replay every other state command as an isolated node.
      for (auto *node = static_cast<wmtcmd_base *>(render.cmd_head.next.get()); node;
           node = static_cast<wmtcmd_base *>(node->next.get())) {
        auto bind_record = [&]<typename T>() {
          auto state = *reinterpret_cast<const T *>(node);
          state.next.set(nullptr);
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&state));
        };
        switch (node->type) {
        case WMTRenderCommandNop:
        case WMTRenderCommandSetPSO:
          break;
        case WMTRenderCommandUseResource: bind_record.operator()<wmtcmd_render_useresource>(); break;
        case WMTRenderCommandSetVertexBuffer:
        case WMTRenderCommandSetFragmentBuffer:
        // Object/mesh (GS/HS/DS) bindings are replayed so a mesh pipeline bound later in the
        // pass sees the same argument state; vertex/fragment replay functions ignore them.
        case WMTRenderCommandSetObjectBuffer:
        case WMTRenderCommandSetMeshBuffer: bind_record.operator()<wmtcmd_render_setbuffer>(); break;
        case WMTRenderCommandSetVertexBufferOffset:
        case WMTRenderCommandSetFragmentBufferOffset:
        case WMTRenderCommandSetObjectBufferOffset:
        case WMTRenderCommandSetMeshBufferOffset: bind_record.operator()<wmtcmd_render_setbufferoffset>(); break;
        case WMTRenderCommandSetFragmentTexture: bind_record.operator()<wmtcmd_render_settexture>(); break;
        case WMTRenderCommandSetFragmentBytes: bind_record.operator()<wmtcmd_render_setbytes>(); break;
        case WMTRenderCommandSetRasterizerState: {
          auto raster = *reinterpret_cast<const wmtcmd_render_setrasterizerstate *>(node);
          raster.next.set(nullptr);
          if (draw.line_mode != SM50_MSAA_LINE_NONE)
            raster.cull_mode = WMTCullModeNone;
          if (!gather) {
            raster.depth_bias = 0;
            raster.scole_scale = 0;
            raster.depth_bias_clamp = 0;
          }
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&raster));
          break;
        }
        case WMTRenderCommandSetViewports: bind_record.operator()<wmtcmd_render_setviewports>(); break;
        case WMTRenderCommandSetViewport: bind_record.operator()<wmtcmd_render_setviewport>(); break;
        case WMTRenderCommandSetScissorRects: bind_record.operator()<wmtcmd_render_setscissorrects>(); break;
        case WMTRenderCommandSetScissorRect: bind_record.operator()<wmtcmd_render_setscissorrect>(); break;
        case WMTRenderCommandSetDSSO: bind_record.operator()<wmtcmd_render_setdsso>(); break;
        case WMTRenderCommandSetDepthStencilState: bind_record.operator()<wmtcmd_render_setdepthstencilstate>(); break;
        case WMTRenderCommandSetBlendFactorAndStencilRef:
        case WMTRenderCommandSetBlendFactor: bind_record.operator()<wmtcmd_render_setblendcolor>(); break;
        case WMTRenderCommandSetStencilRef: bind_record.operator()<wmtcmd_render_setstencilref>(); break;
        case WMTRenderCommandSetStencilRefFrontBack:
          bind_record.operator()<wmtcmd_render_setstencilref_front_back>();
          break;
        case WMTRenderCommandSetVisibilityMode: bind_record.operator()<wmtcmd_render_setvisibilitymode>(); break;
        case WMTRenderCommandMemoryBarrier: bind_record.operator()<wmtcmd_render_memory_barrier>(); break;
        default:
          ERR("Unsupported recorded MSAA render state command ", node->type);
          return false;
        }
      }
      if (draw.root_buffer) {
        encoder.setVertexBuffer(draw.root_buffer, draw.root_offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
        encoder.setFragmentBuffer(draw.root_buffer, draw.root_offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
      }
      if (draw.vertex_buffer_table)
        encoder.setVertexBuffer(draw.vertex_buffer_table, draw.vertex_buffer_offset,
                                SM50_BINDING_INDEX_VERTEX_BUFFER);
      if (draw.static_sampler_buffer) {
        encoder.setVertexBuffer(draw.static_sampler_buffer, draw.static_sampler_offset,
                                SM50_BINDING_INDEX_STATIC_SAMPLERS);
        encoder.setFragmentBuffer(draw.static_sampler_buffer, draw.static_sampler_offset,
                                  SM50_BINDING_INDEX_STATIC_SAMPLERS);
      }
      bind_constants(encoder, vertex, fragment);
      encoder.setVertexBuffer(execution.vertices, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
      if (draw.line_mode != SM50_MSAA_LINE_NONE) {
        encoder.setVertexBuffer(execution.parameters, sizeof(MSAAPrimitiveAssemblyParams),
                                SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
        encoder.setVertexBuffer(execution.indices, 0, SM50_BINDING_INDEX_MSAA_COVERAGE);
      }
      if (draw.line_mode != SM50_MSAA_LINE_NONE ||
          (draw.pixel_layout.flags & SM50_MSAA_PIXEL_DISTANCE_INPUTS)) {
        encoder.setFragmentBuffer(execution.vertices, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
        encoder.setFragmentBuffer(execution.indices, 0, SM50_BINDING_INDEX_MSAA_COVERAGE);
      }
      encoder.setFragmentBuffer(inputs, 0, SM50_BINDING_INDEX_MSAA_PIXEL_INPUTS);
      if (outputs)
        encoder.setFragmentBuffer(outputs, 0, SM50_BINDING_INDEX_MSAA_PIXEL_OUTPUTS);
      encoder.setFragmentBytes(&tile, sizeof(tile), SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
      encoder.setFragmentTexture(sampled_coverage, SM50_BINDING_INDEX_MSAA_COVERAGE);
      encoder.setFragmentBytes(&draw.pixel_layout, sizeof(draw.pixel_layout),
                               SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
      return true;
    };
    auto scissor = [&](WMT::RenderCommandEncoder &encoder, const MSAAPrimitiveTileBounds &bounds,
                       uint32_t x, uint32_t y, bool fullscreen) {
      std::array<WMTScissorRect, 16> rects{};
      const uint32_t count = fullscreen ? 1 : std::max(1u, draw.bounds.viewport_count);
      for (uint32_t i = 0; i < count && i < rects.size(); i++) {
        const auto &original = draw.bounds.scissors[fullscreen ? bounds.viewport_index : i];
        const uint32_t left = std::max(x, original[0]);
        const uint32_t top = std::max(y, original[1]);
        const uint32_t right = std::min(x + 8, original[2]);
        const uint32_t bottom = std::min(y + 8, original[3]);
        rects[i] = {left, top, right > left ? right - left : 0,
                    bottom > top ? bottom - top : 0};
      }
      wmtcmd_render_setscissorrects setting{};
      setting.type = WMTRenderCommandSetScissorRects;
      setting.scissor_rects.set(rects.data());
      setting.rect_count = count;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&setting));
    };
    auto disable_visibility = [&](WMT::RenderCommandEncoder &encoder) {
      wmtcmd_render_setvisibilitymode mode{};
      mode.type = WMTRenderCommandSetVisibilityMode;
      mode.mode = WMTVisibilityResultModeDisabled;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&mode));
    };
    auto no_cull = [&](WMT::RenderCommandEncoder &encoder) {
      wmtcmd_render_setrasterizerstate raster{};
      raster.type = WMTRenderCommandSetRasterizerState;
      raster.fill_mode = WMTTriangleFillModeFill;
      raster.cull_mode = WMTCullModeNone;
      raster.depth_clip_mode = draw.depth_clip_enabled ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
      raster.winding = WMTWindingCounterClockwise;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&raster));
    };
    auto draw_primitive = [&](WMT::RenderCommandEncoder &encoder, uint32_t primitive) {
      wmtcmd_render_draw_indexed_indirect command{};
      command.type = WMTRenderCommandDrawIndexedIndirect;
      command.primitive_type = draw.assembly.topology == 0 ? WMTPrimitiveTypePoint
          : draw.line_mode == SM50_MSAA_LINE_NONE && draw.assembly.topology <= 2
              ? WMTPrimitiveTypeLine : WMTPrimitiveTypeTriangle;
      command.index_type = WMTIndexTypeUInt32;
      command.index_buffer = execution.indices;
      command.index_buffer_offset = 0;
      command.indirect_args_buffer = execution.draws;
      command.indirect_args_offset = uint64_t(primitive) * sizeof(MSAAPrimitiveIndirectArguments);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&command));
    };
    auto fanout_visibility = [&]() {
      for (const auto &target : render.visibility_targets) {
        auto encoder = cmdbuf.computeCommandEncoder(false);
        encoder.waitForFence(fence_);
        wmtcmd_compute_nop head{};
        head.type = WMTComputeCommandNop;
        wmtcmd_compute_setpso setpso{};
        setpso.type = WMTComputeCommandSetPSO;
        setpso.pso = target.accumulate_pipeline;
        setpso.threadgroup_size = {1, 1, 1};
        wmtcmd_compute_setbuffer src{};
        src.type = WMTComputeCommandSetBuffer;
        src.buffer = render.visibility_scratch;
        src.index = 0;
        wmtcmd_compute_setbuffer dst{};
        dst.type = WMTComputeCommandSetBuffer;
        dst.buffer = target.destination;
        dst.offset = uint64_t(target.index) * sizeof(uint64_t);
        dst.index = 1;
        wmtcmd_compute_dispatch dispatch{};
        dispatch.type = WMTComputeCommandDispatchThreads;
        dispatch.size = {1, 1, 1};
        head.next.set(&setpso); setpso.next.set(&src); src.next.set(&dst);
        dst.next.set(&dispatch); dispatch.next.set(nullptr);
        encoder.encodeCommands(&head);
        encoder.updateFence(fence_);
        encoder.endEncoding();
      }
    };
    for (uint32_t primitive = 0; primitive < execution.max_primitives; primitive++) {
      const auto bounds = execution.mapped_bounds[primitive];
      if (bounds.left >= bounds.right || bounds.top >= bounds.bottom ||
          bounds.viewport_index >= 16 || bounds.target_layer >= layer_count) continue;
      if (layer_count > 1) {
        auto &view = layer_views[bounds.target_layer];
        if (!view) {
          uint64_t resource_id = 0;
          view = coverage.newTextureView(
              WMTPixelFormatR32Uint, coverage_view_type, 0, 1,
              bounds.target_layer, 1,
              {WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
               WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, resource_id);
          if (!view) return false;
          inflight.msaa_textures.push_back(view);
        }
        sampled_coverage = view;
        if (post_coverage) {
          auto &post_view = post_views[bounds.target_layer];
          if (!post_view) {
            uint64_t resource_id = 0;
            post_view = post_coverage.newTextureView(
                WMTPixelFormatR32Uint, coverage_view_type, 0, 1,
                bounds.target_layer, 1,
                {WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
                 WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, resource_id);
            if (!post_view) return false;
            inflight.msaa_textures.push_back(post_view);
          }
        }
      }
      for (uint32_t y = bounds.top; y < bounds.bottom; y += 8) {
        for (uint32_t x = bounds.left; x < bounds.right; x += 8) {
          sampled_coverage = layer_count > 1 ? WMT::Texture(layer_views[bounds.target_layer])
                                             : WMT::Texture(coverage);
          auto clear_scratch = cmdbuf.blitCommandEncoder();
          clear_scratch.waitForFence(fence_);
          wmtcmd_blit_fillbuffer fill{};
          fill.type = WMTBlitCommandFillBuffer;
          fill.buffer = inputs;
          fill.length = input_bytes;
          fill.value = 0;
          clear_scratch.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
          if (outputs) {
            fill.buffer = outputs;
            fill.length = output_bytes;
            clear_scratch.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
          }
          clear_scratch.updateFence(fence_);
          clear_scratch.endEncoding();
          // With a forced raster count and a 1x RTV, SampleMask selects the
          // single OUTPUT sample, not virtual raster sample bits.
          const uint32_t raster_mask = draw.single_sample_replay
              ? (draw.sample_mask & 1u ? UINT32_MAX : 0u) : draw.sample_mask;
          const auto &viewport = draw.bounds.viewports[bounds.viewport_index];
          const SM50_MSAA_TILE_CONTROL tile{x, y, draw.sample_count, raster_mask,
              draw.viewport_depths[bounds.viewport_index][0],
              draw.viewport_depths[bounds.viewport_index][1], bounds.primitive_id, primitive,
              viewport[0], viewport[1], viewport[2], viewport[3]};
          WMTRenderPassInfo clear_info{};
          WMT::InitializeRenderPassInfo(clear_info);
          clear_info.colors[0].texture = sampled_coverage;
          clear_info.colors[0].load_action = WMTLoadActionLoad;
          clear_info.colors[0].store_action = WMTStoreActionStore;
          clear_info.default_raster_sample_count = draw.sample_count;
          clear_info.center_sample_pattern = draw.center_sample_pattern;
          clear_info.render_target_width = render_width;
          clear_info.render_target_height = render_height;
          clear_info.visibility_buffer = render.visibility_scratch;
          {
            auto encoder = cmdbuf.renderCommandEncoder(clear_info);
            encoder.waitForFence(fence_, WMTRenderStageFragment);
            if (!bind_tile(encoder, tile, 1, 2)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, true);
            no_cull(encoder);
            disable_visibility(encoder);
            encoder.setDepthStencilState(draw.no_depth_stencil);
            encoder.setRenderPipelineState(draw.clear_coverage_pipeline);
            encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          auto geometry_info = clear_info;
          geometry_info.colors[0].texture = coverage;
          geometry_info.render_target_array_length = layer_count;
          if (render.depth.attachment) {
            geometry_info.depth.texture = render.depth.attachment.texture();
            geometry_info.depth.load_action = WMTLoadActionLoad;
            geometry_info.depth.store_action = WMTStoreActionStore;
            geometry_info.depth.depth_plane = render.depth.depth_plane;
            geometry_info.depth.level = render.depth.level;
            geometry_info.depth.slice = render.depth.slice;
          }
          if (render.stencil.attachment) {
            geometry_info.stencil.texture = render.stencil.attachment.texture();
            geometry_info.stencil.load_action = WMTLoadActionLoad;
            geometry_info.stencil.store_action = WMTStoreActionStore;
            geometry_info.stencil.depth_plane = render.stencil.depth_plane;
            geometry_info.stencil.level = render.stencil.level;
            geometry_info.stencil.slice = render.stencil.slice;
          }
          {
            auto encoder = cmdbuf.renderCommandEncoder(geometry_info);
            encoder.waitForFence(fence_, WMTRenderStageVertex);
            if (!bind_tile(encoder, tile, 1, 2, true)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, false);
            if (draw.pixel_pipeline || draw.sample_pixel_pipeline || draw.early_depth_pipeline ||
                draw.late_depth_pipeline)
              disable_visibility(encoder);
            encoder.setDepthStencilState(draw.capture_depth_state);
            encoder.setRenderPipelineState(draw.gather_depth_pipeline);
            draw_primitive(encoder, primitive);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          if (!draw.pixel_pipeline && !draw.sample_pixel_pipeline &&
              !draw.early_depth_pipeline && !draw.late_depth_pipeline)
            fanout_visibility();
          if (draw.pixel_pipeline || draw.sample_pixel_pipeline) {
            auto encoder = cmdbuf.renderCommandEncoder(geometry_info);
            encoder.waitForFence(fence_, WMTRenderStageVertex);
            if (!bind_tile(encoder, tile, 1, 2)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, false);
            disable_visibility(encoder);
            encoder.setDepthStencilState(draw.capture_depth_state);
            encoder.setRenderPipelineState((render.depth.attachment || render.stencil.attachment)
                                               ? WMT::RenderPipelineState(draw.coverage_with_depth_pipeline)
                                               : WMT::RenderPipelineState(draw.coverage_pipeline));
            draw_primitive(encoder, primitive);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          const bool early_depth_executed = early && draw.early_depth_pipeline;
          if (early_depth_executed) {
            auto clear_post_info = clear_info;
            clear_post_info.colors[0].texture = layer_count > 1
                ? WMT::Texture(post_views[bounds.target_layer]) : WMT::Texture(post_coverage);
            {
              auto encoder = cmdbuf.renderCommandEncoder(clear_post_info);
              encoder.waitForFence(fence_, WMTRenderStageFragment);
              if (!bind_tile(encoder, tile, 1, 2)) { encoder.endEncoding(); return false; }
              scissor(encoder, bounds, x, y, true);
              no_cull(encoder);
              disable_visibility(encoder);
              encoder.setDepthStencilState(draw.no_depth_stencil);
              encoder.setRenderPipelineState(draw.clear_coverage_pipeline);
              encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
              encoder.updateFence(fence_, WMTRenderStageFragment);
              encoder.endEncoding();
            }
            auto post_info = geometry_info;
            post_info.colors[0].texture = post_coverage;
            post_info.colors[0].load_action = WMTLoadActionLoad;
            auto encoder = cmdbuf.renderCommandEncoder(post_info);
            encoder.waitForFence(fence_, WMTRenderStageVertex);
            if (!bind_tile(encoder, tile, 1, 2)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, false);
            encoder.setDepthStencilState(draw.original_depth_stencil);
            encoder.setRenderPipelineState(draw.early_depth_pipeline);
            draw_primitive(encoder, primitive);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
            fanout_visibility();
            sampled_coverage = layer_count > 1 ? WMT::Texture(post_views[bounds.target_layer])
                                               : WMT::Texture(post_coverage);
          }
          WMTRenderPassInfo pixel_info{};
          WMT::InitializeRenderPassInfo(pixel_info);
          pixel_info.default_raster_sample_count = 1;
          pixel_info.render_target_width = render_width;
          pixel_info.render_target_height = render_height;
          pixel_info.visibility_buffer = render.visibility_scratch;
          if (draw.pixel_pipeline) {
            auto encoder = cmdbuf.renderCommandEncoder(pixel_info);
            encoder.waitForFence(fence_, WMTRenderStageFragment);
            if (!bind_tile(encoder, tile, 1, 3)) { encoder.endEncoding(); return false; }
            if (draw.rov_marker_sample_count) bind_rov_markers(encoder, 1);
            scissor(encoder, bounds, x, y, true);
            no_cull(encoder);
            disable_visibility(encoder);
            encoder.setDepthStencilState(draw.no_depth_stencil);
            encoder.setRenderPipelineState(draw.pixel_pipeline);
            encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
            encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          if (draw.single_sample_replay && draw.query_coverage_pipeline &&
              !render.visibility_targets.empty()) {
            // Count the saved post-PS visibility at native raster frequency.
            // The 1x color replay below never contributes to the query.
            WMTRenderPassInfo query_info{};
            WMT::InitializeRenderPassInfo(query_info);
            query_info.default_raster_sample_count = draw.sample_count;
            query_info.center_sample_pattern = draw.center_sample_pattern;
            query_info.render_target_width = render_width;
            query_info.render_target_height = render_height;
            query_info.visibility_buffer = render.visibility_scratch;
            auto encoder = cmdbuf.renderCommandEncoder(query_info);
            encoder.waitForFence(fence_, WMTRenderStageFragment);
            if (!bind_tile(encoder, tile, 1, 4)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, true);
            no_cull(encoder);
            encoder.setDepthStencilState(draw.no_depth_stencil);
            encoder.setRenderPipelineState(draw.query_coverage_pipeline);
            encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
            encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
            // The following replay binds the original query mode before disabling it.
            // Metal may reset the visibility result even when that pass draws nothing.
            fanout_visibility();
          }
          WMTRenderPassInfo replay_info{};
          WMT::InitializeRenderPassInfo(replay_info);
          replay_info.render_target_width = render_width;
          replay_info.render_target_height = render_height;
          replay_info.render_target_array_length = render.render_target_array_length;
          replay_info.default_raster_sample_count = draw.single_sample_replay
              ? 1 : render.default_raster_sample_count;
          replay_info.center_sample_pattern = draw.center_sample_pattern && !draw.single_sample_replay;
          for (uint32_t i = 0; i < render.render_target_count; i++) {
            const auto &source = render.colors[i];
            if (!source.attachment && !source.buffer_attachment) continue;
            auto &destination = replay_info.colors[i];
            destination.texture = source.buffer_attachment ? WMT::Texture(source.buffer_attachment)
                                                          : source.attachment.texture();
            destination.load_action = WMTLoadActionLoad;
            destination.store_action = WMTStoreActionStore;
            destination.level = source.level;
            destination.slice = source.slice;
            destination.depth_plane = source.depth_plane;
          }
          {
            if (render.depth.attachment) {
              replay_info.depth.texture = render.depth.attachment.texture();
              replay_info.depth.load_action = WMTLoadActionLoad;
              replay_info.depth.store_action = WMTStoreActionStore;
              replay_info.depth.depth_plane = render.depth.depth_plane;
              replay_info.depth.level = render.depth.level;
              replay_info.depth.slice = render.depth.slice;
            }
            if (render.stencil.attachment) {
              replay_info.stencil.texture = render.stencil.attachment.texture();
              replay_info.stencil.load_action = WMTLoadActionLoad;
              replay_info.stencil.store_action = WMTStoreActionStore;
              replay_info.stencil.depth_plane = render.stencil.depth_plane;
              replay_info.stencil.level = render.stencil.level;
              replay_info.stencil.slice = render.stencil.slice;
            }
            replay_info.visibility_buffer = render.visibility_scratch;
          }
          if (draw.replay_pipeline || draw.sample_pixel_pipeline ||
              (!early && draw.late_depth_pipeline)) {
            if (!draw.replay_pipeline && !draw.sample_pixel_pipeline)
              for (auto &color : replay_info.colors) color = {};
            auto encoder = cmdbuf.renderCommandEncoder(replay_info);
            encoder.waitForFence(fence_, WMTRenderStageVertex);
            const uint32_t fragment = draw.sample_pixel_pipeline ? 5 : 4;
            if (!bind_tile(encoder, tile, 1, fragment)) { encoder.endEncoding(); return false; }
            if (draw.sample_pixel_pipeline && draw.rov_marker_sample_count)
              bind_rov_markers(encoder, draw.rov_marker_sample_count);
            scissor(encoder, bounds, x, y, draw.single_sample_replay);
            if (early_depth_executed || draw.single_sample_replay) disable_visibility(encoder);
            encoder.setDepthStencilState(early_depth_executed ? WMT::DepthStencilState(draw.no_depth_stencil)
                                               : WMT::DepthStencilState(draw.original_depth_stencil));
            encoder.setRenderPipelineState(draw.sample_pixel_pipeline ? WMT::RenderPipelineState(draw.sample_pixel_pipeline)
                                          : draw.single_sample_replay && layer_count > 1
                                              ? WMT::RenderPipelineState(draw.replay_layer_pipeline)
                                          : draw.replay_pipeline ? WMT::RenderPipelineState(draw.replay_pipeline)
                                          : WMT::RenderPipelineState(draw.late_depth_pipeline));
            if (draw.single_sample_replay) {
              no_cull(encoder);
              encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
              if (layer_count > 1)
                encoder.setVertexBuffer(execution.bounds,
                    uint64_t(primitive) * sizeof(MSAAPrimitiveTileBounds) +
                        offsetof(MSAAPrimitiveTileBounds, target_layer),
                    SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
              encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
            } else {
              draw_primitive(encoder, primitive);
            }
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          if (!early_depth_executed && !draw.single_sample_replay &&
              (draw.replay_pipeline || draw.sample_pixel_pipeline || draw.late_depth_pipeline))
            fanout_visibility();
        }
      }
    }
    return true;
  }

  void
  EncodeMSAAIndirectArguments(WMT::CommandBuffer cmdbuf, const SOIndirectEncoderData &indirect,
                              const SOIndirectExecution &execution) {
    if (!execution.count || !indirect.argument_resolver)
      return;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_useresource use_storage{};
    use_storage.type = WMTComputeCommandUseResource;
    use_storage.resource = execution.storage;
    use_storage.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&use_storage));
    wmtcmd_compute_nop head{};
    head.type = WMTComputeCommandNop;
    wmtcmd_compute_setpso pipeline{};
    pipeline.type = WMTComputeCommandSetPSO;
    pipeline.pso = indirect.argument_resolver;
    pipeline.threadgroup_size = {32, 1, 1};
    wmtcmd_compute_setbuffer arguments{};
    arguments.type = WMTComputeCommandSetBuffer;
    arguments.buffer = execution.storage;
    arguments.offset = execution.command_offset;
    arguments.index = 30;
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {execution.count, 1, 1};
    head.next.set(&pipeline);
    pipeline.next.set(&arguments);
    arguments.next.set(&dispatch);
    dispatch.next.set(nullptr);
    encoder.encodeCommands(&head);
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

  bool
  EncodeSOIndirectPreflight(WMT::CommandBuffer cmdbuf, const SOIndirectEncoderData &indirect,
                            SOIndirectPreflightParams params_data, uint32_t record_capacity,
                            WMT::Reference<WMT::Buffer> &storage, const SOIndirectPreflightResult *&result) {
    auto metal = device_->GetMTLDevice();
    const uint64_t bytes = sizeof(SOIndirectPreflightParams) + sizeof(SOIndirectPreflightResult) +
                           uint64_t(record_capacity) * sizeof(SOIndirectPreflightRecord);
    if (bytes > metal.maxBufferLength())
      return false;
    WMTBufferInfo info{};
    info.length = bytes;
    info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    storage = metal.newBuffer(info);
    auto *mapped = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
    if (!storage || !mapped)
      return false;
    memcpy(mapped, &params_data, sizeof(params_data));
    result = reinterpret_cast<const SOIndirectPreflightResult *>(mapped + sizeof(SOIndirectPreflightParams));
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_nop head{};
    head.type = WMTComputeCommandNop;
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = indirect.preflight_pipeline;
    pso.threadgroup_size = {1, 1, 1};
    wmtcmd_compute_setbuffer params{};
    params.type = WMTComputeCommandSetBuffer;
    params.buffer = storage;
    params.offset = 0;
    params.index = 0;
    wmtcmd_compute_setbuffer output{};
    output.type = WMTComputeCommandSetBuffer;
    output.buffer = storage;
    output.offset = sizeof(SOIndirectPreflightParams);
    output.index = 1;
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {1, 1, 1};
    head.next.set(&pso);
    pso.next.set(&params);
    params.next.set(&output);
    output.next.set(&dispatch);
    dispatch.next.set(nullptr);
    encoder.encodeCommands(&head);
    encoder.updateFence(fence_);
    encoder.endEncoding();
    return true;
  }

  bool
  PrepareSOIndirectExecution(const SOIndirectEncoderData &indirect, const SOIndirectPreflightResult &result,
                             InflightCommandBuffer &inflight, SOIndirectExecution &execution) {
    const uint32_t count = result.effective_count;
    if (count > indirect.preflight.max_count || !indirect.draw_template || !indirect.assembly_template)
      return false;
    execution.count = count;
    if (!count)
      return true;
    auto metal = device_->GetMTLDevice();
    const auto *records = reinterpret_cast<const SOIndirectPreflightRecord *>(&result + 1);
    const uint64_t root_stride = uint64_t(indirect.command.rootsig_qwords_stride) * sizeof(uint64_t);
    const uint64_t vb_stride = indirect.command.vertex_argbuf_stride;
    uint64_t cursor = 0;
    auto reserve = [&](uint64_t length) -> uint64_t {
      cursor = (cursor + 15) & ~uint64_t(15);
      const uint64_t at = cursor;
      cursor += length;
      return at;
    };
    // Per-execution storage is never written into the allocator's recorded GPU heap.
    const uint64_t command_at = reserve(sizeof(IndirectRenderCommandData));
    const uint64_t bindings_at = reserve(uint64_t(count) * sizeof(SOIndirectCaptureBinding));
    const uint64_t roots_at = reserve(uint64_t(count) * root_stride);
    const uint64_t vb_at = reserve(uint64_t(count) * vb_stride);
    const uint64_t draws_at = reserve(uint64_t(count) *
        (sizeof(StreamOutputAssemblyParams) + sizeof(SM50_SO_CAPTURE_CONTROL) + 16));
    if (cursor > metal.maxBufferLength() || cursor < draws_at)
      return false;
    WMTBufferInfo info{};
    info.length = cursor;
    info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    execution.storage = metal.newBuffer(info);
    auto *memory = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
    if (!execution.storage || !memory)
      return false;
    memset(memory, 0, cursor);
    execution.command_offset = command_at;
    auto *command = reinterpret_cast<IndirectRenderCommandData *>(memory + command_at);
    *command = indirect.command;
    auto *bindings = reinterpret_cast<SOIndirectCaptureBinding *>(memory + bindings_at);
    execution.command = command;
    execution.bindings = bindings;
    execution.root_offset = roots_at;
    execution.vertex_offset = vb_at;
    const uint64_t gpu_base = info.gpu_address;
    command->so_draws = gpu_base + bindings_at;
    command->max_count = count;
    command->max_count_buffer = 0;
    command->predicate = 0; // the preflight has already snapshotted the predicate
    if (root_stride) {
      if (!indirect.root_arguments)
        return false;
      for (uint32_t i = 0; i < count; i++)
        memcpy(memory + roots_at + uint64_t(i) * root_stride, indirect.root_arguments, root_stride);
      command->rootsig_qwords = gpu_base + roots_at;
    }
    if (vb_stride) {
      if (!indirect.vertex_buffers)
        return false;
      for (uint32_t i = 0; i < count; i++)
        memcpy(memory + vb_at + uint64_t(i) * vb_stride, indirect.vertex_buffers, vb_stride);
      command->vertex_buffer = gpu_base + vb_at;
    }
    if (!indirect.msaa) {
      WMTIndirectCommandBufferInfo icb_info{};
      icb_info.inherit_buffers = 0;
      icb_info.inherit_pso = 1;
      icb_info.inherit_cull_mode = 1;
      icb_info.inherit_fill_mode = 1;
      icb_info.inherit_front_facing = 1;
      icb_info.inherit_depth_bias = 1;
      icb_info.inherit_depth_clip_mode = 1;
      icb_info.inherit_depth_stencil_state = 1;
      icb_info.type = indirect.preflight.indexed ? WMTIndirectCommandTypeDrawIndexed : WMTIndirectCommandTypeDraw;
      icb_info.max_vertex_buffer_binding = 31;
      icb_info.max_fragment_buffer_binding = 31;
      execution.icb = metal.newIndirectCommandBuffer(icb_info, count, WMTResourceStorageModePrivate);
      if (!execution.icb)
        return false;
      command->cmd_buf = icb_info.gpu_resource_id;
    }
    const auto &layout = *indirect.assembly_template;
    const auto &original = *indirect.draw_template;
    const bool needs_stream_assembly = layout.stream_output_enabled || layout.statistics_enabled;
    for (uint32_t i = 0; i < count; i++) {
      const auto &record = records[i];
      const uint64_t vertices = uint64_t(record.vertex_count) * record.instance_count;
      const uint64_t scratch_bytes = vertices * (uint64_t(layout.record_dwords) + 1) * sizeof(uint32_t);
      const uint64_t positions_bytes = uint64_t(record.vertex_count) * sizeof(uint32_t);
      if (vertices > UINT32_MAX || scratch_bytes > metal.maxBufferLength() ||
          (indirect.preflight.indexed && vertices && positions_bytes > metal.maxBufferLength()))
        return false;
      auto draw = std::make_shared<StreamOutputDrawData>(original);
      draw->indexed = indirect.preflight.indexed;
      draw->vertex_count = vertices ? record.vertex_count : 0;
      WMTBufferInfo scratch_info{};
      uint64_t scratch_address = gpu_base;
      if (needs_stream_assembly && vertices) {
        scratch_info.length = scratch_bytes;
        scratch_info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
        draw->scratch = metal.newBuffer(scratch_info);
        if (!draw->scratch)
          return false;
        scratch_address = scratch_info.gpu_address;
      } else {
        draw->scratch = execution.storage;
      }
      uint64_t positions_address = gpu_base;
      if (needs_stream_assembly && draw->indexed && vertices) {
        WMTBufferInfo positions_info{};
        positions_info.length = positions_bytes;
        positions_info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
        draw->positions = metal.newBuffer(positions_info);
        if (!draw->positions)
          return false;
        positions_address = positions_info.gpu_address;
      }
      const uint64_t at = draws_at + uint64_t(i) *
          (sizeof(StreamOutputAssemblyParams) + sizeof(SM50_SO_CAPTURE_CONTROL) + 16);
      draw->heap = execution.storage;
      draw->params_offset = at;
      draw->control_offset = at + sizeof(StreamOutputAssemblyParams);
      draw->status_offset = draw->control_offset + sizeof(SM50_SO_CAPTURE_CONTROL);
      draw->status_cpu = reinterpret_cast<const uint32_t *>(memory + draw->status_offset);
      draw->claims_offset = vertices * layout.record_dwords * sizeof(uint32_t);
      draw->claims_bytes = vertices * sizeof(uint32_t);
      auto *params = reinterpret_cast<StreamOutputAssemblyParams *>(memory + draw->params_offset);
      *params = layout;
      params->vertex_count = record.vertex_count;
      params->instance_count = record.instance_count;
      auto *control = reinterpret_cast<SM50_SO_CAPTURE_CONTROL *>(memory + draw->control_offset);
      control->positions_per_instance = record.vertex_count;
      control->record_count = uint32_t(vertices);
      uint64_t index_address = gpu_base;
      if (draw->indexed) {
        if (vertices && record.index_format != DXGI_FORMAT_R16_UINT && record.index_format != DXGI_FORMAT_R32_UINT)
          return false;
        const uint32_t index_size = record.index_format == DXGI_FORMAT_R32_UINT ? 4 : 2;
        uint64_t first = 0;
        if (vertices) {
          first = record.index_address + uint64_t(record.start_location) * index_size;
          if (first < record.index_address)
            return false;
          uint64_t index_offset = 0;
          auto *index_owner = device_->LookupBufferByVA(first, &index_offset);
          if (!index_owner || uint64_t(record.vertex_count) * index_size > index_owner->byteLength() - index_offset)
            return false;
          draw->index_owner = index_owner;
          draw->index_buffer = index_owner->buffer();
          draw->index_offset = index_offset & ~uint64_t(3);
          index_address = first & ~uint64_t(3);
        } else {
          draw->index_buffer = execution.storage;
          draw->index_offset = 0;
        }
        params->index_size = index_size;
        params->index_skip = uint32_t(first & 3);
        params->cut_enabled = (params->topology == 2 || params->topology == 4) &&
            indirect.strip_cut == (index_size == 4 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                 : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
        params->cut_value = index_size == 4 ? 0xffffffffu : 0xffffu;
        control->index_size = index_size;
        control->index_first = params->index_skip / 2;
        control->base_vertex = record.base_vertex;
      }
      bindings[i] = {index_address, gpu_base + draw->control_offset, scratch_address,
                     positions_address, record};
      execution.draws.push_back(std::move(draw));
    }
    // Reserve before any setup commands are encoded; the caller catches allocation
    // failure and removes the device while no completion status points at freed storage.
    inflight.so_draws.reserve(inflight.so_draws.size() + execution.draws.size());
    inflight.so_indirect_buffers.push_back(execution.storage);
    if (execution.icb)
      inflight.so_indirect_icbs.push_back(execution.icb);
    return true;
  }

  struct SwizzlePassResource {
    Com<MTLD3D12Resource, false> resource;
    bool writes;
    WMT::Reference<WMT::Buffer> baseline;
  };

  void AppendStandardResource(std::vector<SwizzlePassResource> &uses,
                              Com<MTLD3D12Resource, false> resource, bool writes) {
    if (!resource)
      return;
    for (auto &use : uses) {
      if (use.resource.ptr() == resource.ptr()) {
        use.writes |= writes;
        return;
      }
    }
    uses.push_back({std::move(resource), writes});
  }

  void AppendStandardTexture(std::vector<SwizzlePassResource> &uses, Texture *texture, bool writes) {
    if (texture)
      AppendStandardResource(uses, device_->LookupStandardSwizzleTexture(texture), writes);
  }

  std::vector<SwizzlePassResource>
  CollectStandardSwizzleTables(const std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables) {
    std::vector<SwizzlePassResource> uses;
    if (!tables || !device_->HasStandardSwizzleTextures())
      return uses;
    for (const auto &table : *tables) {
      D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
      D3D12_GPU_DESCRIPTOR_HANDLE heap_start{};
      table.heap->GetDesc(&heap_desc);
      table.heap->GetGPUDescriptorHandleForHeapStart(&heap_start);
      if (!heap_start.ptr || table.base < heap_start.ptr ||
          (table.base - heap_start.ptr) % 32)
        continue;
      const uint64_t base_index = (table.base - heap_start.ptr) / 32;
      if (base_index >= heap_desc.NumDescriptors)
        continue;
      for (const auto &range : table.rootsig->GetTableRanges(table.parameter)) {
        if (range.type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
            range.type != D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
          continue;
        if (range.offset >= heap_desc.NumDescriptors - base_index)
          continue;
        const uint64_t first = base_index + range.offset;
        const uint64_t count = std::min<uint64_t>(range.count, heap_desc.NumDescriptors - first);
        for (uint64_t i = 0; i < count; ++i) {
          const auto &descriptor = table.heap->GetDescriptor(first + i);
          if (range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
              descriptor.type == ShaderVisibleDescriptorType::SRVTexture)
            AppendStandardTexture(uses, descriptor.SRVTexture.texture, false);
          if (range.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
              descriptor.type == ShaderVisibleDescriptorType::UAVTexture)
            AppendStandardTexture(uses, descriptor.UAVTexture.texture, true);
        }
      }
    }
    return uses;
  }

  bool EncodeStandardSwizzle(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                             MTLD3D12Resource *resource, bool to_canonical,
                             WMT::Buffer dirty_flags = {}, uint64_t dirty_offset = 0, bool compare = false,
                             WMT::Buffer baseline = {}) {
    if (!resource || !resource->swizzle_layout || !resource->texture || !resource->buffer ||
        !resource->texture->current() || !resource->buffer->current())
      return false;
    const auto &layout = *resource->swizzle_layout;
    const auto desc = resource->GetDesc();
    const bool packed422 = layout.block_width == 2;
    auto metal = device_->GetMTLDevice();
    auto &import_pipeline = packed422 ? swizzle_packed422_import_pipeline_ : swizzle_import_pipeline_;
    auto &export_pipeline = packed422 ? swizzle_packed422_export_pipeline_ : swizzle_export_pipeline_;
    auto &dirty_pipeline = packed422 ? swizzle_packed422_dirty_pipeline_ : swizzle_dirty_pipeline_;
    auto &conditional_pipeline = packed422 ? swizzle_packed422_export_if_dirty_pipeline_
                                        : swizzle_export_if_dirty_pipeline_;
    if (!import_pipeline || !export_pipeline || (compare && !dirty_pipeline) ||
        (dirty_flags && !compare && !conditional_pipeline)) {
      auto library = device_->GetLib().getLibrary();
      WMT::Reference<WMT::Error> error;
      if (!import_pipeline)
        import_pipeline = metal.newComputePipelineState(
            library.newFunction(packed422 ? "d3d12_swizzle_packed422_import" : "d3d12_swizzle_import"), error);
      if (!export_pipeline)
        export_pipeline = metal.newComputePipelineState(
            library.newFunction(packed422 ? "d3d12_swizzle_packed422_export" : "d3d12_swizzle_export"), error);
      if (compare && !dirty_pipeline)
        dirty_pipeline = metal.newComputePipelineState(
            library.newFunction(packed422 ? "d3d12_swizzle_packed422_dirty" : "d3d12_swizzle_dirty"), error);
      if (dirty_flags && !compare && !conditional_pipeline)
        conditional_pipeline = metal.newComputePipelineState(
            library.newFunction(packed422 ? "d3d12_swizzle_packed422_export_if_dirty"
                                        : "d3d12_swizzle_export_if_dirty"), error);
      if (!import_pipeline || !export_pipeline || (compare && !dirty_pipeline) ||
          (dirty_flags && !compare && !conditional_pipeline))
        return false;
    }
    auto pipeline = compare ? WMT::ComputePipelineState(dirty_pipeline)
        : dirty_flags ? WMT::ComputePipelineState(conditional_pipeline)
        : to_canonical ? WMT::ComputePipelineState(export_pipeline)
                       : WMT::ComputePipelineState(import_pipeline);
    const auto &shape = layout.interleave;
    const uint32_t physical_unit_bytes = packed422 ? 8 : layout.bytes_per_unit;
    const uint32_t row_pitch = (uint32_t(shape.tile_width) * physical_unit_bytes + 255) & ~255u;
    const uint32_t slice_pitch = row_pitch * shape.tile_height;
    const uint64_t scratch_size = uint64_t(slice_pitch) * shape.tile_depth;
    if (!scratch_size || scratch_size > metal.maxBufferLength())
      return false;
    WMT::Reference<WMT::Buffer> scratch;
    if (inflight.swizzle_scratch_length >= scratch_size) {
      scratch = inflight.swizzle_buffers.back();
    } else {
      WMTBufferInfo info{};
      info.length = scratch_size;
      info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      info.memory.set(nullptr);
      scratch = metal.newBuffer(info);
      if (!scratch)
        return false;
      inflight.swizzle_buffers.push_back(scratch);
      inflight.swizzle_scratch_length = scratch_size;
    }
    auto canonical = resource->buffer->current()->buffer();
    auto shadow = resource->texture->current()->texture();
    for (uint32_t array = 0; array < layout.array_count; ++array) {
      for (uint32_t mip = 0; mip < layout.mip_count; ++mip) {
        const auto &m = layout.mips[mip];
        const uint64_t mip_width = std::max<uint64_t>(1, desc.Width >> mip);
        const uint64_t mip_height = std::max<uint64_t>(1, UINT64(desc.Height) >> mip);
        D3D12StandardSwizzleParams p{};
        p.first_tile = array * layout.tiles_per_slice + m.first_tile_in_slice;
        p.tiles_x = m.tiles_x;
        p.tiles_y = m.tiles_y;
        p.units_x = m.units_x;
        p.units_y = m.units_y;
        p.units_z = m.units_z;
        p.tile_width = shape.tile_width;
        p.tile_height = shape.tile_height;
        p.tile_depth = shape.tile_depth;
        p.x_mask = shape.x_mask;
        p.y_mask = shape.y_mask;
        p.z_mask = shape.z_mask;
        p.bytes_per_unit = layout.bytes_per_unit;
        p.row_pitch = row_pitch;
        p.slice_pitch = slice_pitch;
        p.physical_width = mip_width;
        p.packed422_variant = desc.Format == DXGI_FORMAT_G8R8_G8B8_UNORM;
        for (uint64_t tz = 0; tz < m.tiles_z; ++tz) {
          for (uint64_t ty = 0; ty < m.tiles_y; ++ty) {
            for (uint64_t tx = 0; tx < m.tiles_x; ++tx) {
              p.origin_x = tx * shape.tile_width;
              p.origin_y = ty * shape.tile_height;
              p.origin_z = tz * shape.tile_depth;
              p.extent_x = std::min<uint64_t>(shape.tile_width, m.units_x - p.origin_x);
              p.extent_y = std::min<uint64_t>(shape.tile_height, m.units_y - p.origin_y);
              p.extent_z = std::min<uint64_t>(shape.tile_depth, m.units_z - p.origin_z);
              const uint64_t pixel_x = uint64_t(p.origin_x) * layout.block_width;
              const uint64_t pixel_y = uint64_t(p.origin_y) * layout.block_height;
              const WMTOrigin origin{pixel_x, pixel_y, p.origin_z};
              const WMTSize size{
                  std::min<uint64_t>(uint64_t(p.extent_x) * layout.block_width, mip_width - pixel_x),
                  std::min<uint64_t>(uint64_t(p.extent_y) * layout.block_height, mip_height - pixel_y),
                  p.extent_z};
              if (to_canonical) {
                wmtcmd_blit_copy_from_texture_to_buffer_withblitoption copy{};
                copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
                copy.src = shadow;
                copy.level = mip;
                copy.slice = array;
                copy.origin = origin;
                copy.size = size;
                copy.dst = scratch;
                copy.offset = 0;
                copy.bytes_per_row = row_pitch;
                copy.bytes_per_image = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? slice_pitch : 0;
                copy.options = WMTBlitOptionNone;
                auto blit = cmdbuf.blitCommandEncoder();
                blit.waitForFence(fence_);
                blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
                blit.updateFence(fence_);
                blit.endEncoding();
              }
              wmtcmd_compute_setpso set_pso{};
              wmtcmd_compute_setbuffer set_source{}, set_destination{};
              wmtcmd_compute_setbuffer set_dirty{};
              wmtcmd_compute_setbytes set_params{};
              wmtcmd_compute_dispatch dispatch{};
              set_pso.type = WMTComputeCommandSetPSO;
              set_pso.pso = pipeline;
              set_pso.threadgroup_size = {8, 8, 1};
              set_pso.next.set(&set_source);
              set_source.type = WMTComputeCommandSetBuffer;
              set_source.buffer = to_canonical ? WMT::Buffer(scratch) : canonical;
              set_source.offset = 0;
              set_source.index = 0;
              set_source.next.set(&set_destination);
              set_destination.type = WMTComputeCommandSetBuffer;
              set_destination.buffer = compare && baseline ? baseline
                  : to_canonical ? canonical : WMT::Buffer(scratch);
              set_destination.offset = 0;
              set_destination.index = 1;
              set_destination.next.set(&set_params);
              set_params.type = WMTComputeCommandSetBytes;
              set_params.bytes.set(&p);
              set_params.length = sizeof(p);
              set_params.index = 2;
              if (dirty_flags) {
                set_params.next.set(&set_dirty);
                set_dirty.type = WMTComputeCommandSetBuffer;
                set_dirty.buffer = dirty_flags;
                set_dirty.offset = dirty_offset;
                set_dirty.index = 3;
                set_dirty.next.set(&dispatch);
              } else
                set_params.next.set(&dispatch);
              dispatch.type = WMTComputeCommandDispatchThreads;
              dispatch.size = {p.extent_x, p.extent_y, p.extent_z};
              auto compute = cmdbuf.computeCommandEncoder(false);
              compute.waitForFence(fence_);
              compute.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&set_pso));
              compute.updateFence(fence_);
              compute.endEncoding();
              if (!to_canonical) {
                wmtcmd_blit_copy_from_buffer_to_texture_withblitoption copy{};
                copy.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
                copy.src = scratch;
                copy.src_offset = 0;
                copy.bytes_per_row = row_pitch;
                copy.bytes_per_image = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? slice_pitch : 0;
                copy.size = size;
                copy.dst = shadow;
                copy.level = mip;
                copy.slice = array;
                copy.origin = origin;
                copy.options = WMTBlitOptionNone;
                auto blit = cmdbuf.blitCommandEncoder();
                blit.waitForFence(fence_);
                blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
                blit.updateFence(fence_);
                blit.endEncoding();
              }
            }
          }
        }
      }
    }
    return true;
  }

  bool BeginStandardSwizzlePass(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                                std::vector<SwizzlePassResource> &uses) {
    for (auto &use : uses) {
      // Encoding may fail after earlier transfers have already been queued.
      // Retain all backing owners through that partial command buffer as well.
      if (std::none_of(inflight.swizzle_resources.begin(), inflight.swizzle_resources.end(),
                       [&](const auto &held) { return held.ptr() == use.resource.ptr(); }))
        inflight.swizzle_resources.push_back(use.resource);
      if (use.writes && use.resource->standard_swizzle_may_alias) {
        // An inactive placed texture UAV may alias an active buffer UAV. Save
        // the canonical bytes before either shader executes: comparing against
        // the post-shader canonical buffer would mistake buffer writes for
        // texture writes and publish the stale texture shadow over them.
        WMTBufferInfo info{};
        info.length = use.resource->swizzle_layout->size_bytes;
        info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
        info.memory.set(nullptr);
        use.baseline = device_->GetMTLDevice().newBuffer(info);
        if (!use.baseline)
          return false;
        inflight.swizzle_baselines.push_back(use.baseline);
        wmtcmd_blit_copy_from_buffer_to_buffer copy{};
        copy.type = WMTBlitCommandCopyFromBufferToBuffer;
        copy.src = use.resource->buffer->current()->buffer();
        copy.src_offset = 0;
        copy.dst = use.baseline;
        copy.dst_offset = 0;
        copy.copy_length = info.length;
        auto blit = cmdbuf.blitCommandEncoder();
        blit.waitForFence(fence_);
        blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        blit.updateFence(fence_);
        blit.endEncoding();
      }
      if (!EncodeStandardSwizzle(cmdbuf, inflight, use.resource.ptr(), false))
        return false;
    }
    return true;
  }

  bool EndStandardSwizzlePass(WMT::CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                              const std::vector<SwizzlePassResource> &uses) {
    // Descriptor tables may include inactive placed UAVs aliasing a texture
    // or a buffer actively written in this pass. Compare each placed shadow
    // against its pre-pass canonical baseline, before publishing any shadow.
    std::vector<uint32_t> compared_indices;
    for (uint32_t i = 0; i < uses.size(); ++i) {
      if (uses[i].writes && uses[i].baseline)
        compared_indices.push_back(i);
    }
    WMT::Reference<WMT::Buffer> dirty_flags;
    if (!compared_indices.empty()) {
      WMTBufferInfo info{};
      info.length = uint64_t(uses.size()) * sizeof(uint32_t);
      info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      info.memory.set(nullptr);
      dirty_flags = device_->GetMTLDevice().newBuffer(info);
      if (!dirty_flags)
        return false;
      inflight.swizzle_dirty_flags.push_back(dirty_flags);
      wmtcmd_blit_fillbuffer fill{};
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = dirty_flags;
      fill.length = info.length;
      fill.value = 0;
      auto blit = cmdbuf.blitCommandEncoder();
      blit.waitForFence(fence_);
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
      blit.updateFence(fence_);
      blit.endEncoding();
      for (auto index : compared_indices)
        if (!EncodeStandardSwizzle(cmdbuf, inflight, uses[index].resource.ptr(), true,
                                   dirty_flags, uint64_t(index) * sizeof(uint32_t), true,
                                   uses[index].baseline))
          return false;
    }
    for (uint32_t i = 0; i < uses.size(); ++i) {
      const auto &use = uses[i];
      if (!use.writes)
        continue;
      if (!EncodeStandardSwizzle(cmdbuf, inflight, use.resource.ptr(), true,
                                 use.baseline ? WMT::Buffer(dirty_flags) : WMT::Buffer{},
                                 uint64_t(i) * sizeof(uint32_t)))
        return false;
    }
    return true;
  }

  void
  ProcessExecute(ExecuteTask &task) {
    std::optional<CommittingScope> scope;
    auto begin_segment = [&] {
      scope.emplace(this);
      scope->inflight.protections.reserve(task.lists.size());
      for (auto &list : task.lists)
        scope->inflight.protections.push_back({list.allocator.Retain(), list.recording_lease.Retain()});
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
    auto start_segment = [&] {
      try {
        begin_segment();
        return true;
      } catch (const std::bad_alloc &) {
        // Preallocation failed before this segment encoded a GPU command.
        ERR("ExecuteCommandLists: out of memory preparing submission segment");
        scope.reset();
        device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
        return false;
      }
    };
    if (!start_segment())
      return;
    bool has_transitions = false;
    for (const auto &list : task.lists)
      has_transitions |= !static_cast<RecordingHead *>(list.entry)->state_events.empty();
    if (has_transitions) {
      // Queue Waits run before this FIFO task. Validate every referenced
      // resource against the state left by all preceding submissions, even
      // when the producer was on another queue. Copy first so a rejected
      // batch cannot publish a partial state or encode any of its commands.
      bool valid = true;
      bool out_of_memory = false;
      try {
        std::lock_guard<std::mutex> lock(device_->owned_barrier_state_mutex);
        std::unordered_map<BarrierTracker *, BarrierResourceState> pending;
        for (const auto &list : task.lists) {
          for (const auto &transition : static_cast<RecordingHead *>(list.entry)->state_events) {
            // COPY_SOURCE/DEST on a copy engine belong to a different state
            // class from those on 3D/compute. A cross-engine handoff must
            // reach COMMON (and be fence-ordered) before any copy-list use.
            if (desc_.Type == D3D12_COMMAND_LIST_TYPE_COPY &&
                !CopyQueueStartsCommon(transition.resource->state, transition)) {
              valid = false;
              break;
            }
            auto it = pending.try_emplace(transition.resource.get(), transition.resource->state).first;
            if (!ApplyRecordedStateEvent(it->second, transition, false, desc_.Type)) {
              valid = false;
              break;
            }
          }
          if (!valid)
            break;
        }
        if (valid) {
          for (auto &[tracker, state] : pending) {
            if (!tracker->fixed_state &&
                (desc_.Type == D3D12_COMMAND_LIST_TYPE_COPY || tracker->broad_promotion)) {
              // Decay is at ExecuteCommandLists boundaries, never between
              // lists in the same call. Preserve pending split subresources.
              if (!state.all.split && !state.all.layout_split) {
                state.all.current = D3D12_RESOURCE_STATE_COMMON;
                state.all.layout.reset();
                state.all.implicitly_promoted = false;
                state.all.creation_state = false;
                std::erase_if(state.subresources, [](const auto &entry) {
                  return !entry.second.split && !entry.second.layout_split;
                });
              }
            } else if (!tracker->fixed_state) {
              // Ordinary textures also decay if their state was implicitly
              // promoted to read-only. Explicit transitions into a read state
              // do not decay, even if the resulting bitmask is identical.
              constexpr D3D12_RESOURCE_STATES reads = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
              auto promoted_read = [](const BarrierSubresourceState &slot) {
                return slot.implicitly_promoted && slot.current && !(*slot.current & ~reads) && !slot.split;
              };
              if (promoted_read(state.all)) {
                state.all.current = D3D12_RESOURCE_STATE_COMMON;
                state.all.implicitly_promoted = false;
              }
              for (auto it = state.subresources.begin(); it != state.subresources.end();) {
                auto &slot = it->second;
                if (promoted_read(slot)) {
                  if (state.all.current == D3D12_RESOURCE_STATE_COMMON) {
                    it = state.subresources.erase(it);
                    continue;
                  }
                  slot.current = D3D12_RESOURCE_STATE_COMMON;
                  slot.implicitly_promoted = false;
                }
                ++it;
              }
            }
          }
          for (auto &[tracker, state] : pending)
            tracker->state = std::move(state);
        }
      } catch (const std::bad_alloc &) {
        out_of_memory = true;
      }
      if (!valid || out_of_memory) {
        ERR(out_of_memory ? "ExecuteCommandLists: out of memory validating transitions" :
                            "ExecuteCommandLists: cross-submission transition mismatch");
        scope.reset();
        device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
        return;
      }
    }
    for (auto &list : task.lists) {
      if (device_->RemovedReason() != S_OK)
        return;
      owned_graphics::Trace("execute_list", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                            ",\"list\":", list.list_id,
                            ",\"recording\":", list.recording);
      EncoderData *current = list.entry;
      if (ReservedResource::AnyLive())
        /* Command lists see each other's writes; for sparse tiles sharing a heap page that needs a
           shared event signal/wait, not only the pass boundary (ReservedResource::AnyLive). */
        EncodeAliasFlush(scope->inflight.cmdbuf);
      while (current) {
        if (device_->RemovedReason() != S_OK)
          return;
        if (current->type == EncoderType::Query &&
            static_cast<QueryEncoderData *>(current)->op == QueryEncoderOp::ResolveTimestamp) {
          auto query = static_cast<QueryEncoderData *>(current);
          const uint64_t producer_submission = scope->seq;
          // This is the FIFO worker, never the application's Execute caller.
          // Release both queue and temporal locks before any completion wait.
          scope.reset();
          submission_ring_.waitUntilRetired(producer_submission);
          if (device_->RemovedReason() != S_OK)
            return;
          const auto producers = query->timestamp_producers->snapshot(query->start_index, query->query_count);
          for (auto producer : producers) {
            if (!producer)
              continue;
            if (producer.status() < WMTCommandBufferStatusCommitted) {
              ERR("ResolveQueryData: timestamp producer has not been submitted");
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            if (producer.status() <= WMTCommandBufferStatusScheduled)
              producer.waitUntilCompleted();
            if (producer.status() == WMTCommandBufferStatusError) {
              ERR("ResolveQueryData: timestamp producer failed");
              device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
              return;
            }
          }
          owned_graphics::Trace("query_resolve_ready", ",\"queue\":", owned_id_,
                                ",\"completed_submission\":", producer_submission,
                                ",\"start\":", query->start_index, ",\"count\":", query->query_count);
          if (!start_segment())
            return;
        }

        bool execute_predicated_texture_copy = true;
        if (current->type == EncoderType::PredicatedTextureCopy) {
          auto copy = static_cast<PredicatedTextureCopyEncoderData *>(current);
          const uint64_t predicate_submission = scope->seq;
          auto predicate_producer = scope->inflight.cmdbuf;
          // The submission worker, never the application thread, commits the
          // commands which produced the retained snapshot and waits for their
          // actual completion before consulting CPU-coherent allocator memory.
          scope.reset();
          submission_ring_.waitUntilRetired(predicate_submission);
          if (device_->RemovedReason() != S_OK)
            return;
          if (predicate_producer.status() == WMTCommandBufferStatusError) {
            ERR("PredicatedTextureCopy: predicate producer failed");
            device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
            return;
          }
          if (!copy->predicate_value) {
            ERR("PredicatedTextureCopy: missing predicate snapshot");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          const uint64_t value = *copy->predicate_value;
          execute_predicated_texture_copy = copy->predicate_not_equal ? value == 0 : value != 0;
          owned_graphics::Trace("predicated_texture_copy_ready", ",\"queue\":", owned_id_,
                                ",\"completed_submission\":", predicate_submission,
                                ",\"predicate\":", value,
                                ",\"execute\":", uint32_t(execute_predicated_texture_copy));
          if (!start_segment())
            return;
        }

        std::optional<SOIndirectExecution> indirect_execution;
        if (current->type == EncoderType::Render) {
          auto *render = static_cast<RenderEncoderData *>(current);
          if (render->so_indirect) {
            WMT::Reference<WMT::Buffer> preflight_storage;
            const SOIndirectPreflightResult *preflight_result = nullptr;
            auto params = render->so_indirect->preflight;
            uint32_t actual_count = params.max_count;
            if (params.count_buffer || params.predicate) {
              params.count_only = 1;
              if (!EncodeSOIndirectPreflight(scope->inflight.cmdbuf, *render->so_indirect,
                                             params, 0, preflight_storage, preflight_result)) {
                ERR("Draw capture: indirect count preflight allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              const uint64_t producer = scope->seq;
              scope.reset();
              submission_ring_.waitUntilRetired(producer);
              if (device_->RemovedReason() != S_OK)
                return;
              actual_count = preflight_result->effective_count;
              preflight_storage = {};
              if (!start_segment())
                return;
            }
            if (actual_count > params.max_count) {
              ERR("Draw capture: invalid indirect count preflight result");
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            if (actual_count) {
              params.max_count = actual_count;
              params.count_buffer = 0;
              params.predicate = 0;
              params.count_only = 0;
              if (!EncodeSOIndirectPreflight(scope->inflight.cmdbuf, *render->so_indirect,
                                             params, actual_count, preflight_storage, preflight_result)) {
                ERR("Draw capture: indirect draw preflight allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              const uint64_t producer = scope->seq;
              scope.reset();
              submission_ring_.waitUntilRetired(producer);
              if (device_->RemovedReason() != S_OK)
                return;
              if (!start_segment())
                return;
            } else {
              static const SOIndirectPreflightResult empty{};
              preflight_result = &empty;
            }
            indirect_execution.emplace();
            bool prepared = false;
            try {
              prepared = PrepareSOIndirectExecution(*render->so_indirect, *preflight_result,
                                                    scope->inflight, *indirect_execution);
            } catch (const std::bad_alloc &) {
              prepared = false;
            }
            if (!prepared) {
              ERR("Draw capture: indirect capture cannot be allocated for GPU draw arguments");
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            if (render->so_indirect->msaa)
              EncodeMSAAIndirectArguments(scope->inflight.cmdbuf, *render->so_indirect, *indirect_execution);
          }
        }

        std::vector<MSAAExecution> msaa_executions;
        std::vector<MSAADrawData> msaa_draws;
        if (current->type == EncoderType::Render) {
          auto *render = static_cast<RenderEncoderData *>(current);
          if (render->msaa_draw) {
            // Vertex capture is a shader pass submitted before the raster pass.
            // Import its descriptor-table textures into this producer segment,
            // not only into the later render segment.
            auto producer_swizzle_uses = CollectStandardSwizzleTables(render->swizzle_tables);
            if (!BeginStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, producer_swizzle_uses)) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            const auto &so_draws = indirect_execution ? indirect_execution->draws : render->so_draws;
            if (render->msaa_draw->needs_stream_assembly && !so_draws.empty())
              EncodeStreamOutputSetup(scope->inflight.cmdbuf, scope->inflight, so_draws);
            const uint32_t count = indirect_execution ? indirect_execution->count : 1;
            msaa_executions.reserve(count);
            msaa_draws.reserve(count);
            for (uint32_t i = 0; i < count; i++) {
              MSAADrawData draw = *render->msaa_draw;
              if (indirect_execution) {
                const auto &record = indirect_execution->bindings[i].arguments;
                const auto &prepared = indirect_execution->draws[i];
                draw.stream_output = prepared;
                draw.assembly.vertex_count = record.vertex_count;
                draw.assembly.instance_count = record.instance_count;
                draw.assembly.positions_per_instance = record.vertex_count;
                const uint32_t vpp = draw.assembly.topology == 0 ? 1 : draw.assembly.topology <= 2 ? 2 : 3;
                const bool strip = draw.assembly.topology == 2 || draw.assembly.topology == 4;
                const uint64_t per_instance = strip ? record.vertex_count >= vpp
                    ? record.vertex_count - vpp + 1 : 0 : record.vertex_count / vpp;
                const uint64_t primitives = per_instance * record.instance_count;
                const uint32_t index_stride = draw.line_mode == SM50_MSAA_LINE_NONE ? 3 : 20;
                if (primitives > UINT32_MAX / index_stride ||
                    primitives * index_stride * sizeof(uint32_t) >
                        device_->GetMTLDevice().maxBufferLength()) {
                  ERR("MSAA indirect primitive count exceeds GPU addressability");
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                  return;
                }
                draw.assembly.max_primitives = uint32_t(primitives);
                draw.assembly.index_size = record.index_format == DXGI_FORMAT_R32_UINT ? 4 : 2;
                const auto cut = render->so_indirect->strip_cut;
                draw.assembly.cut_enabled = draw.indexed && strip &&
                    ((draw.assembly.index_size == 2 && cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
                     (draw.assembly.index_size == 4 && cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF));
                draw.assembly.cut_value = draw.assembly.index_size == 4 ? 0xffffffffu : 0xffffu;
                draw.indexed = render->so_indirect->preflight.indexed;
                draw.index_buffer = prepared->index_buffer;
                const uint64_t first = record.index_address + uint64_t(record.start_location) *
                    draw.assembly.index_size;
                draw.assembly.index_skip = first & 3;
                draw.index_offset = prepared->index_offset + draw.assembly.index_skip;
                draw.base_vertex = record.base_vertex;
                draw.start_instance = record.start_instance;
                draw.start_vertex = record.start_location;
                draw.predicate_buffer = {};
                draw.assembly.predicate_enabled = 0;
                draw.root_buffer = indirect_execution->storage;
                draw.root_offset = indirect_execution->root_offset +
                    uint64_t(i) * indirect_execution->command->rootsig_qwords_stride * 8;
                draw.vertex_buffer_table = indirect_execution->storage;
                draw.vertex_buffer_offset = indirect_execution->vertex_offset +
                    uint64_t(i) * indirect_execution->command->vertex_argbuf_stride;
                draw.static_sampler_buffer = render->so_indirect->draw_template->heap;
                draw.static_sampler_offset = render->so_indirect->static_sampler_offset;
              }
              draw.bounds.max_primitives = draw.assembly.max_primitives;
              msaa_draws.push_back(draw);
              msaa_executions.emplace_back();
              if (draw.assembly.vertex_count && draw.assembly.instance_count &&
                  !EncodeMSAAProducer(scope->inflight.cmdbuf, scope->inflight, *render, draw,
                                      msaa_executions.back())) {
                ERR("MSAA vertex capture/assembly resource allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
            }
            if (!EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, producer_swizzle_uses)) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            const uint64_t producer = scope->seq;
            scope.reset();
            submission_ring_.waitUntilRetired(producer);
            if (device_->RemovedReason() != S_OK)
              return;
            if (!start_segment())
              return;
          }
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
          std::vector<SwizzlePassResource> swizzle_uses;
          if (device_->HasStandardSwizzleTextures() && data->attachment)
            AppendStandardTexture(swizzle_uses, data->attachment.ptr()->allocation->descriptor, true);
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
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
          if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::Render: {
          auto data = static_cast<RenderEncoderData *>(current);
          auto swizzle_uses = CollectStandardSwizzleTables(data->swizzle_tables);
          if (device_->HasStandardSwizzleTextures()) {
            for (const auto &color : data->colors) {
              if (color.attachment)
                AppendStandardTexture(swizzle_uses, color.attachment.ptr()->allocation->descriptor, true);
              if (color.resolve_attachment)
                AppendStandardTexture(swizzle_uses, color.resolve_attachment.ptr()->allocation->descriptor, true);
            }
          }
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          const auto &so_draws = indirect_execution ? indirect_execution->draws : data->so_draws;
          if (data->msaa_draw) {
            for (uint32_t i = 0; i < msaa_executions.size(); i++) {
              if (!msaa_executions[i].max_primitives) continue;
              if (!EncodeMSAARender(cmdbuf, scope->inflight, *data,
                                    msaa_draws[i], msaa_executions[i])) {
                ERR("MSAA tile execution resource allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
            }
            if (data->msaa_draw->needs_stream_assembly)
              for (const auto &captured : so_draws)
                EncodeStreamOutputAssembly(cmdbuf, *captured);
            if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            break;
          }
          if (!so_draws.empty()) {
            if (!indirect_execution) {
              try {
                scope->inflight.so_draws.reserve(scope->inflight.so_draws.size() + so_draws.size());
              } catch (const std::bad_alloc &) {
                ERR("Draw capture: failed to retain capture until GPU completion");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
            }
            EncodeStreamOutputSetup(cmdbuf, scope->inflight, so_draws);
          }
          if (std::any_of(data->colors.begin(), data->colors.end(), [](const auto &color) {
                return bool(color.buffer_attachment);
              })) {
            auto blit = cmdbuf.blitCommandEncoder();
            blit.waitForFence(fence_);
            for (const auto &color : data->colors) {
              if (!color.buffer_attachment)
                continue;
              wmtcmd_blit_copy_from_buffer_to_texture copy{};
              copy.type = WMTBlitCommandCopyFromBufferToTexture;
              copy.next.set(nullptr);
              copy.src = color.buffer_backing;
              copy.src_offset = color.buffer_offset;
              copy.bytes_per_row = color.buffer_bytes_per_row;
              copy.bytes_per_image = color.buffer_bytes_per_row;
              copy.size = {color.buffer_width, 1, 1};
              copy.dst = color.buffer_attachment;
              copy.origin = {0, 0, 0};
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            }
            blit.updateFence(fence_);
            blit.endEncoding();
          }
          WMTRenderPassInfo render_pass_info;
          WMT::InitializeRenderPassInfo(render_pass_info);
          {
            for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
              auto &color_data = data->colors[i];
              if (!color_data.attachment && !color_data.buffer_attachment)
                continue;
              auto &color_info = render_pass_info.colors[i];
              color_info.texture = color_data.buffer_attachment
                  ? WMT::Texture(color_data.buffer_attachment)
                  : color_data.attachment.texture();
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
            render_pass_info.center_sample_pattern = data->center_sample_pattern;
            render_pass_info.render_target_array_length = data->render_target_array_length;
            render_pass_info.render_target_width = data->render_target_width;
            render_pass_info.render_target_height = data->render_target_height;
            render_pass_info.visibility_buffer = data->visibility_scratch;
          }
          std::array<WMT::Texture, 8> rov_markers{};
          if (data->rov_marker_sample_count && !PrepareROVMarkers(
                  cmdbuf, scope->inflight,
                  data->rov_marker_attachmentless ? data->rov_marker_width : data->render_target_width,
                  data->rov_marker_attachmentless ? data->rov_marker_height : data->render_target_height,
                  std::max(1u, uint32_t(data->render_target_array_length)),
                  data->rov_marker_sample_count, rov_markers)) {
            ERR("Raster-ordered buffer marker resource allocation failed");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          // Object/mesh (GS/HS/DS) functions read resources earlier writers produced.
          const bool mesh_pass = data->use_geometry || data->use_tessellation;
          encoder.waitForFence(fence_, mesh_pass ? WMTRenderStagePreRaster : WMTRenderStageVertex);
          for (uint32_t i = 0; i < data->rov_marker_sample_count; ++i) {
            encoder.setFragmentTexture(rov_markers[i], 31 + i);
            encoder.useResource(rov_markers[i], WMTResourceUsageRead | WMTResourceUsageWrite,
                                WMTRenderStageFragment);
          }
          encoder.encodeCommands(&data->cmd_head);
          if (indirect_execution && indirect_execution->count) {
            auto &execution = *indirect_execution;
            wmtcmd_render_nop head{};
            head.type = WMTRenderCommandNop;
            wmtcmd_render_useresource use_icb{};
            use_icb.type = WMTRenderCommandUseResource;
            use_icb.resource = execution.icb;
            use_icb.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
            use_icb.stages = WMTRenderStageVertex;
            wmtcmd_render_setpso resolver{};
            resolver.type = WMTRenderCommandSetPSO;
            resolver.pso = data->so_indirect->resolver_pipeline;
            wmtcmd_render_setbuffer argument{};
            argument.type = WMTRenderCommandSetVertexBuffer;
            argument.buffer = execution.storage;
            argument.offset = execution.command_offset;
            argument.index = 30;
            wmtcmd_render_draw dispatch{};
            dispatch.type = WMTRenderCommandDraw;
            dispatch.primitive_type = WMTPrimitiveTypePoint;
            dispatch.vertex_count = 1;
            dispatch.instance_count = 1;
            wmtcmd_render_setpso drawing{};
            drawing.type = WMTRenderCommandSetPSO;
            drawing.pso = data->so_indirect->draw_pipeline;
            wmtcmd_render_executecommands execute{};
            execute.type = WMTRenderCommandExecuteCommandsInBuffer;
            execute.indirect_command_buffer = execution.icb;
            execute.location = 0;
            execute.length = execution.count;
            head.next.set(&use_icb);
            use_icb.next.set(&resolver);
            resolver.next.set(&argument);
            argument.next.set(&dispatch);
            dispatch.next.set(&drawing);
            drawing.next.set(&execute);
            execute.next.set(nullptr);
            encoder.useResource(execution.storage, WMTResourceUsageRead | WMTResourceUsageWrite,
                                WMTRenderStageVertex);
            if (execution.draws.front()->statistics_heap)
              encoder.useResource(execution.draws.front()->statistics_heap,
                                  WMTResourceUsageRead | WMTResourceUsageWrite,
                                  WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment));
            for (const auto &draw : execution.draws) {
              encoder.useResource(draw->scratch, WMTResourceUsageWrite, WMTRenderStageVertex);
              if (draw->positions)
                encoder.useResource(draw->positions, WMTResourceUsageRead, WMTRenderStageVertex);
              if (draw->indexed && draw->index_buffer)
                encoder.useResource(draw->index_buffer, WMTResourceUsageRead, WMTRenderStageVertex);
            }
            encoder.encodeCommands(&head);
          }
          // Capture writes happen in the vertex stage; a rasterization-disabled pass has no
          // fragment work, so the assembly after it must wait for the vertex stage too.
          encoder.updateFence(fence_, WMTRenderStages(
                                          (so_draws.empty() ? 0 : WMTRenderStageVertex) | WMTRenderStageFragment |
                                          (mesh_pass ? WMTRenderStageObject | WMTRenderStageMesh : 0)));
          encoder.endEncoding();
          if (std::any_of(data->colors.begin(), data->colors.end(), [](const auto &color) {
                return bool(color.buffer_attachment);
              })) {
            auto blit = cmdbuf.blitCommandEncoder();
            blit.waitForFence(fence_);
            for (const auto &color : data->colors) {
              if (!color.buffer_attachment)
                continue;
              wmtcmd_blit_copy_from_texture_to_buffer copy{};
              copy.type = WMTBlitCommandCopyFromTextureToBuffer;
              copy.next.set(nullptr);
              copy.src = color.buffer_attachment;
              copy.origin = {0, 0, 0};
              copy.size = {color.buffer_width, 1, 1};
              copy.dst = color.buffer_backing;
              copy.offset = color.buffer_offset;
              copy.bytes_per_row = color.buffer_bytes_per_row;
              copy.bytes_per_image = color.buffer_bytes_per_row;
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            }
            blit.updateFence(fence_);
            blit.endEncoding();
          }
          for (const auto &draw : so_draws)
            EncodeStreamOutputAssembly(cmdbuf, *draw);
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
          if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
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
        case EncoderType::StandardSwizzle: {
          auto *data = static_cast<StandardSwizzleEncoderData *>(current);
          if (!EncodeStandardSwizzle(cmdbuf, scope->inflight, data->resource, data->to_canonical)) {
            ERR("Standard-swizzle GPU transfer failed");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::PredicatedTextureCopy: {
          if (!execute_predicated_texture_copy)
            break;
          auto data = static_cast<PredicatedTextureCopyEncoderData *>(current);
          wmtcmd_blit_copy_from_texture_to_texture copy{};
          copy.type = WMTBlitCommandCopyFromTextureToTexture;
          copy.next.set(nullptr);
          copy.src = data->source;
          copy.src_slice = data->source_slice;
          copy.src_level = data->source_level;
          copy.src_origin = data->source_origin;
          copy.src_size = data->size;
          copy.dst = data->destination;
          copy.dst_slice = data->destination_slice;
          copy.dst_level = data->destination_level;
          copy.dst_origin = data->destination_origin;
          auto encoder = cmdbuf.blitCommandEncoder();
          encoder.waitForFence(fence_);
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          encoder.updateFence(fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Compute: {
          auto data = static_cast<ComputeEncoderData *>(current);
          auto swizzle_uses = CollectStandardSwizzleTables(data->swizzle_tables);
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(fence_);
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
          encoder.endEncoding();
          if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::Temporal: {
          auto data = static_cast<TemporalEncoderData *>(current);
          scope->inflight.temporal_frames.push_back(data->frame);
          std::vector<SwizzlePassResource> swizzle_uses;
          if (device_->HasStandardSwizzleTextures()) {
            const auto resources = GetOwnedTemporalFrameResources(data->frame);
            for (size_t i = 0; i < resources.size(); ++i)
              if (resources[i] && resources[i]->swizzle_layout)
                AppendStandardResource(swizzle_uses, Com<MTLD3D12Resource, false>(resources[i]), i == 3);
          }
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          EncodeOwnedTemporalFrame(data->frame, cmdbuf, fence_, owned_id_, scope->seq, current->id);
          if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::Resolve: {
          auto data = static_cast<ResolveEncoderData *>(current);
          std::vector<SwizzlePassResource> swizzle_uses;
          if (device_->HasStandardSwizzleTextures()) {
            if (data->src)
              AppendStandardTexture(swizzle_uses, data->src.ptr()->allocation->descriptor, false);
            if (data->dst)
              AppendStandardTexture(swizzle_uses, data->dst.ptr()->allocation->descriptor, true);
          }
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }

          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          if (data->pipeline) {
            if (data->dsv_flags & 1) {
              info.depth.texture = data->dst.texture();
              info.depth.load_action = WMTLoadActionLoad;
              info.depth.store_action = WMTStoreActionStore;
            } else if (data->dsv_flags & 2) {
              info.stencil.texture = data->dst.texture();
              info.stencil.load_action = WMTLoadActionLoad;
              info.stencil.store_action = WMTStoreActionStore;
            } else {
              info.colors[0].texture = data->dst.texture();
              info.colors[0].load_action = WMTLoadActionLoad;
              info.colors[0].store_action = WMTStoreActionStore;
            }
          } else {
            info.colors[0].texture = data->src.texture();
            info.colors[0].load_action = WMTLoadActionLoad;
            info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
            info.colors[0].resolve_texture = data->dst.texture();
          }

          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(fence_, WMTRenderStageFragment);
          encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
          if (data->pipeline) {
            encoder.setRenderPipelineState(data->pipeline);
            if (data->depth_stencil_state)
              encoder.setDepthStencilState(data->depth_stencil_state);
            encoder.setFragmentTexture(data->src.texture(), 0);
            encoder.setFragmentBytes(&data->parameters, sizeof(data->parameters), 0);
            encoder.setFragmentBuffer(data->predicate, data->predicate_offset, 1);
            encoder.setViewport({double(data->destination_x), double(data->destination_y),
                                 double(data->width), double(data->height), 0.0, 1.0});
            encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
          }
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();
          if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }

          break;
        }
        case EncoderType::Query: {
          auto data = static_cast<QueryEncoderData *>(current);
          switch (data->op) {
          case QueryEncoderOp::AliasFlush:
            EncodeAliasFlush(cmdbuf);
            break;
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
          case QueryEncoderOp::ResetPipelineStatistics: {
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            wmtcmd_blit_fillbuffer fill{};
            fill.type = WMTBlitCommandFillBuffer;
            fill.next.set(nullptr);
            fill.buffer = data->counters;
            fill.offset = data->counters_offset;
            fill.length = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
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
          case QueryEncoderOp::BeginSOStatistics:
          case QueryEncoderOp::ResolveSOStatistics: {
            const bool begin = data->op == QueryEncoderOp::BeginSOStatistics;
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            wmtcmd_blit_copy_from_buffer_to_buffer copy{};
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.next.set(nullptr);
            copy.src = begin ? data->counters : data->source;
            copy.src_offset = begin ? data->counters_offset
                                    : uint64_t(data->start_index) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
            copy.dst = begin ? data->source : data->destination;
            copy.dst_offset = begin ? data->snapshot_offset : data->destination_offset;
            copy.copy_length = uint64_t(data->query_count) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::EndSOStatistics: {
            auto encoder = cmdbuf.computeCommandEncoder(false);
            encoder.waitForFence(fence_);
            wmtcmd_compute_nop head{};
            head.type = WMTComputeCommandNop;
            wmtcmd_compute_setpso set_pso{};
            set_pso.type = WMTComputeCommandSetPSO;
            set_pso.pso = data->compute_pipeline;
            set_pso.threadgroup_size = {1, 1, 1};
            wmtcmd_compute_setbuffer set_counter{};
            set_counter.type = WMTComputeCommandSetBuffer;
            set_counter.buffer = data->counters;
            set_counter.offset = data->counters_offset;
            set_counter.index = 0;
            wmtcmd_compute_setbuffer set_begin{};
            set_begin.type = WMTComputeCommandSetBuffer;
            set_begin.buffer = data->source;
            set_begin.offset = data->snapshot_offset;
            set_begin.index = 1;
            wmtcmd_compute_setbuffer set_result{};
            set_result.type = WMTComputeCommandSetBuffer;
            set_result.buffer = data->source;
            set_result.offset = uint64_t(data->start_index) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
            set_result.index = 2;
            wmtcmd_compute_dispatch dispatch{};
            dispatch.type = WMTComputeCommandDispatchThreads;
            dispatch.size = {1, 1, 1};
            head.next.set(&set_pso);
            set_pso.next.set(&set_counter);
            set_counter.next.set(&set_begin);
            set_begin.next.set(&set_result);
            set_result.next.set(&dispatch);
            dispatch.next.set(nullptr);
            encoder.encodeCommands(&head);
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::BeginPipelineStatistics:
          case QueryEncoderOp::ResolvePipelineStatistics: {
            const bool begin = data->op == QueryEncoderOp::BeginPipelineStatistics;
            auto encoder = cmdbuf.blitCommandEncoder();
            encoder.waitForFence(fence_);
            wmtcmd_blit_copy_from_buffer_to_buffer copy{};
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.next.set(nullptr);
            copy.src = begin ? data->counters : data->source;
            copy.src_offset = begin ? data->counters_offset
                                    : uint64_t(data->start_index) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
            copy.dst = begin ? data->source : data->destination;
            copy.dst_offset = begin ? data->snapshot_offset : data->destination_offset;
            copy.copy_length = uint64_t(data->query_count) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::EndPipelineStatistics: {
            auto encoder = cmdbuf.computeCommandEncoder(false);
            encoder.waitForFence(fence_);
            wmtcmd_compute_nop head{};
            head.type = WMTComputeCommandNop;
            wmtcmd_compute_setpso set_pso{};
            set_pso.type = WMTComputeCommandSetPSO;
            set_pso.pso = data->compute_pipeline;
            set_pso.threadgroup_size = {1, 1, 1};
            wmtcmd_compute_setbuffer set_counter{};
            set_counter.type = WMTComputeCommandSetBuffer;
            set_counter.buffer = data->counters;
            set_counter.offset = data->counters_offset;
            set_counter.index = 0;
            wmtcmd_compute_setbuffer set_begin{};
            set_begin.type = WMTComputeCommandSetBuffer;
            set_begin.buffer = data->source;
            set_begin.offset = data->snapshot_offset;
            set_begin.index = 1;
            wmtcmd_compute_setbuffer set_result{};
            set_result.type = WMTComputeCommandSetBuffer;
            set_result.buffer = data->source;
            set_result.offset = uint64_t(data->start_index) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
            set_result.index = 2;
            wmtcmd_compute_dispatch dispatch{};
            dispatch.type = WMTComputeCommandDispatchThreads;
            dispatch.size = {1, 1, 1};
            head.next.set(&set_pso);
            set_pso.next.set(&set_counter);
            set_counter.next.set(&set_begin);
            set_begin.next.set(&set_result);
            set_result.next.set(&dispatch);
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
    if (device_->RemovedReason() != S_OK)
      return;
    if (Count && !ppCommandLists) {
      ERR("ExecuteCommandLists: null command list array");
      return;
    }
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      if (device_->RemovedReason() != S_OK)
        return;
      ExecuteTask task;
      task.lists.reserve(Count);
      for (unsigned i = 0; i < Count; i++) {
        if (!ppCommandLists[i]) {
          ERR("ExecuteCommandLists: null command list");
          return;
        }
        Com<ID3D12Device> owner;
        auto child = static_cast<ID3D12DeviceChild *>(ppCommandLists[i]);
        if (FAILED(child->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
            owner.ptr() != static_cast<ID3D12Device *>(device_)) {
          ERR("ExecuteCommandLists: command list belongs to another device");
          return;
        }
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
        if (!list->owned_allocator || !list->owned_closed_valid ||
            list->encoder_count == std::numeric_limits<size_t>::max() || list->GetType() != desc_.Type) {
          ERR("ExecuteCommandLists: invalid command list or allocator");
          return;
        }
        for (const auto &bundle : list->owned_bundle_references) {
          if (!bundle.valid()) {
            ERR("ExecuteCommandLists: bundle allocator was reset after recording");
            device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
            return;
          }
        }
        SubmissionLease lease(static_cast<MTLD3D12CommandAllocatorImpl *>(list->owned_allocator),
                              list->owned_closed_generation);
        if (!lease) {
          ERR("ExecuteCommandLists: command allocator was reset after Close");
          device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
          return;
        }
        RecordingLease recording(static_cast<RecordingHead *>(list->entry));
        if (!recording) {
          ERR("ExecuteCommandLists: previous execution of this recording has not retired");
          device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
          return;
        }
        task.lists.push_back(ListSubmission{
            list->entry, list->owned_id, list->owned_recording, std::move(lease), std::move(recording)});
        if (!task.contains_temporal) {
          for (auto *encoder = list->entry; encoder; encoder = encoder->next) {
            if (encoder->type == EncoderType::Temporal) {
              task.contains_temporal = true;
              break;
            }
          }
        }
      }
      // Only contradictions within this submission are definitive at API
      // capture; another queue may produce the preceding state behind a Wait.
      std::unordered_map<BarrierTracker *, BarrierResourceState> states;
      for (const auto &list : task.lists) {
        for (const auto &transition : static_cast<RecordingHead *>(list.entry)->state_events) {
          auto &state = states[transition.resource.get()];
          if (!state.all.current && transition.resource->fixed_state)
            state.all.current = *transition.resource->fixed_state;
          if (!ApplyRecordedStateEvent(state, transition, true, desc_.Type)) {
            ERR("ExecuteCommandLists: transition before-state or split barrier mismatch");
            device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
            return;
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
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
    } catch (...) {
      ERR("ExecuteCommandLists: failed to capture submission");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
    }
  }

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  void
  ProcessSignal(SignalTask &task) {
    if (device_->RemovedReason() != S_OK)
      return;
    uint64_t submission = 0;
    try {
      {
        auto scope = StartCommitting();
        submission = scope.seq;
        // Shared fences: the GPU signals the authoritative MTLSharedEvent
        // after all prior work of this queue, so other devices and processes
        // (including their GPU waits) order against it without a CPU hop.
        if (auto event = task.fence->sharedEvent()) {
          // The native token is reserved at encode time, in queue order; its logical
          // value is published to every process before the GPU can reach it.
          task.ticket.native_token = task.fence->reserveSharedToken(task.ticket.value);
          scope.inflight.cmdbuf.encodeSignalEvent(event, task.ticket.native_token);
        }
        scope.inflight.fence_completion.emplace(task);
        owned_graphics::Trace("signal", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                              ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                              ",\"generation\":", task.ticket.generation);
      }
    } catch (const std::bad_alloc &) {
      ERR("CommandQueue::Signal: out of memory preparing native submission");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return;
    }
    owned_graphics::Trace("signal_committed", ",\"queue\":", owned_id_, ",\"submission\":", submission,
                          ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                          ",\"generation\":", task.ticket.generation);
  }

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pFence)
      return E_INVALIDARG;
    Com<ID3D12Device> owner;
    if (FAILED(pFence->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
        owner.ptr() != static_cast<ID3D12Device *>(device_))
      return E_INVALIDARG;
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      if (device_->RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      auto native = static_cast<MTLD3D12Fence *>(pFence);
      SignalTask task;
      task.fence = native->fence;
      task.fence_id = native->owned_id;
      task.ticket = task.fence->prepareQueueSignal(Value);
      return task_fifo_.push(QueueTask{std::move(task)}) ? S_OK : E_FAIL;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  };

  void
  ProcessWait(WaitTask &task) {
    if (device_->RemovedReason() != S_OK)
      return;
    if (!task.fence->queueWaitReady(task.ticket))
      owned_graphics::Trace("wait_defer", ",\"queue\":", owned_id_, ",\"fence\":", task.fence_id,
                            ",\"value\":", task.ticket.value, ",\"generation\":", task.ticket.generation);
    {
      std::lock_guard<std::mutex> lock(wait_mutex_);
      blocked_wait_ = task.ticket.latch;
    }
    const bool satisfied = task.fence->waitForQueueProducer(task.ticket, stopping_);
    {
      std::lock_guard<std::mutex> lock(wait_mutex_);
      blocked_wait_.reset();
    }

    if (!satisfied) {
      // A queue being destroyed cannot await a future producer forever.
      // Subsequent FIFO tasks were never submitted to the GPU; do not publish
      // their signals or consume their Present latency credit twice.
      task.fence->cancelQueueWait(task.ticket);
      cancel_unsubmitted_ = true;
      return;
    }
    if (device_->RemovedReason() != S_OK)
      return;

    try {
      auto scope = StartCommitting();
      // Shared fences: the CPU latch above was released by a published logical value
      // >= the wait value; the GPU additionally waits on the native token that
      // published it, ordering later work behind the remote producer's GPU work.
      if (auto event = task.fence->sharedEvent())
        scope.inflight.cmdbuf.encodeWaitForEvent(event, task.fence->observedSharedToken());
      owned_graphics::Trace("wait", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                            ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                            ",\"generation\":", task.ticket.generation);
    } catch (const std::bad_alloc &) {
      ERR("CommandQueue::Wait: out of memory preparing native submission");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
    }
  }

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!pFence)
      return E_INVALIDARG;
    Com<ID3D12Device> owner;
    if (FAILED(pFence->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
        owner.ptr() != static_cast<ID3D12Device *>(device_))
      return E_INVALIDARG;
    std::shared_ptr<owned_graphics::FenceOrderEpoch> registration_order;
    uint64_t registration_id = 0;
    try {
      std::lock_guard<dxmt::mutex> enqueue_lock(mutex_enqueue_);
      if (device_->RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      auto native = static_cast<MTLD3D12Fence *>(pFence);
      WaitTask task;
      task.fence = native->fence;
      task.fence_id = native->owned_id;
      task.ticket = task.fence->prepareQueueWait(Value);
      registration_order = task.ticket.order;
      registration_id = task.ticket.waiter_id;
      if (!task_fifo_.push(QueueTask{std::move(task)})) {
        registration_order->removeCompletionWaiter(registration_id);
        return E_FAIL;
      }
      return S_OK;
    } catch (const std::bad_alloc &) {
      if (registration_order)
        registration_order->removeCompletionWaiter(registration_id);
      return E_OUTOFMEMORY;
    } catch (...) {
      if (registration_order)
        registration_order->removeCompletionWaiter(registration_id);
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
    if (!gpu_timestamp || !cpu_timestamp)
      return E_INVALIDARG;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_FAIL;

    LARGE_INTEGER qpc_before{}, qpc_after{};
    uint64_t metal_cpu_timestamp = 0;
    uint64_t metal_gpu_timestamp = 0;
    if (!QueryPerformanceCounter(&qpc_before))
      return E_FAIL;
    device_->GetMTLDevice().sampleTimestamps(&metal_cpu_timestamp, &metal_gpu_timestamp);
    if (!QueryPerformanceCounter(&qpc_after) || !metal_cpu_timestamp || !metal_gpu_timestamp)
      return E_FAIL;

    // Metal returns the GPU timestamp paired with a CPU sample taken in the
    // native host clock domain. Wine's QPC has a different frequency and may
    // use a different epoch, so bracket the paired Metal sample in QPC space
    // instead of exposing the host CPU value directly. The midpoint bounds the
    // conversion error to half of this synchronous bridge call.
    const uint64_t before = static_cast<uint64_t>(qpc_before.QuadPart);
    const uint64_t after = static_cast<uint64_t>(qpc_after.QuadPart);
    *gpu_timestamp = metal_gpu_timestamp;
    *cpu_timestamp = before + (after - before) / 2;
    return S_OK;
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
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain, nullptr);
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainWithOutput(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput,
      IDXGISwapChain1 **ppSwapChain
  ) override {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain,
                                 pRestrictToOutput);
  }

  void
  ProcessPresent(PresentTask &task) {
    if (device_->RemovedReason() != S_OK) {
      CancelPresent(task);
      return;
    }
    if (!task.presenter->beginNativePresent(task.restart_epoch, task.state->frame_id)) {
      if (task.observer)
        task.observer.cancel(task.present_id);
      task.presenter->skipQueuedPresent(task.state->frame_id);
      if (task.completion_handle.handle && task.credit->exchange(false, std::memory_order_acq_rel))
        task.latency->complete(task.completion_handle.handle);
      return;
    }
    try {
      EncodePresent(task);
    } catch (const std::bad_alloc &) {
      ERR("D3D12: Present command allocation failed");
      device_->ReportRemoval(E_OUTOFMEMORY);
      CancelPresent(task);
    } catch (...) {
      ERR("D3D12: Present command encoding failed");
      device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      CancelPresent(task);
    }
  }

  void
  EncodePresent(PresentTask &task) {
    auto scope = StartCommitting();
    if (device_->RemovedReason() != S_OK) {
      CancelPresent(task);
      return;
    }
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.semaphore = task.completion_handle.release();
    scope.inflight.latency = std::move(task.latency);

    auto g = task.backbuffer.ptr();
    auto &view = g->texture->view(g->texture->fullView);

    // ResizeBuffers1 may rotate presents over several queues. Order them on
    // the GPU in Present call order without blocking this queue's worker, so a
    // later frame on an idle queue cannot overtake an earlier blocked one.
    auto order_event = task.presenter->presentOrderEvent();
    const uint64_t frame_id = task.state->frame_id;
    cmdbuf.encodeWaitForEvent(order_event, frame_id - 1);
    WMT::Texture present_source = view.texture;
    if (task.composition.history) {
      auto &composition = task.composition;
      auto &current = composition.history->textures[composition.write_index];
      auto &previous = composition.history->textures[composition.write_index ^ 1];
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.waitForFence(fence_);
      auto copy = [&](WMT::Texture source, int64_t source_x, int64_t source_y,
                      int64_t width, int64_t height, int64_t destination_x, int64_t destination_y) {
        wmtcmd_blit_copy_from_texture_to_texture command{};
        command.type = WMTBlitCommandCopyFromTextureToTexture;
        command.next.set(nullptr);
        command.src = source;
        command.src_origin = {uint32_t(source_x), uint32_t(source_y), 0};
        command.src_size = {uint32_t(width), uint32_t(height), 1};
        command.dst = current;
        command.dst_origin = {uint32_t(destination_x), uint32_t(destination_y), 0};
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&command));
      };
      if (composition.full || !composition.has_previous) {
        copy(view.texture, 0, 0, composition.width, composition.height, 0, 0);
      } else {
        copy(previous, 0, 0, composition.width, composition.height, 0, 0);
        if (composition.scroll) {
          const auto &rect = composition.scroll_rect;
          const int64_t offset_x = composition.scroll_offset.x;
          const int64_t offset_y = composition.scroll_offset.y;
          const int64_t left = std::max<int64_t>(rect.left, -offset_x);
          const int64_t top = std::max<int64_t>(rect.top, -offset_y);
          const int64_t right = std::min<int64_t>(rect.right, int64_t(composition.width) - offset_x);
          const int64_t bottom = std::min<int64_t>(rect.bottom, int64_t(composition.height) - offset_y);
          if (right > left && bottom > top)
            copy(previous, left, top, right - left, bottom - top, left + offset_x, top + offset_y);
        }
        for (const RECT &rect : composition.dirty_rects)
          copy(view.texture, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
               rect.left, rect.top);
      }
      encoder.updateFence(fence_);
      encoder.endEncoding();
      present_source = current;
      scope.inflight.present_history = composition.history;
    }
    auto drawable = task.presenter->encodeCommands(
        cmdbuf, present_source, task.state->metadata,
        [&](auto encoder) { encoder.waitForFence(fence_, WMTRenderStageFragment); },
        [&](auto encoder) { encoder.updateFence(fence_, WMTRenderStageFragment); }
    );
    if (task.composition.history && !task.composition.full && task.composition.has_previous) {
      // DXGI flip-sequential updates the current backbuffer before swapping,
      // not just the visible drawable. Keep application readback and later
      // incremental updates consistent with the composed presentation.
      wmtcmd_blit_copy_from_texture_to_texture copy{};
      copy.type = WMTBlitCommandCopyFromTextureToTexture;
      copy.next.set(nullptr);
      copy.src = present_source;
      copy.src_size = {task.composition.width, task.composition.height, 1};
      copy.dst = view.texture;
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.waitForFence(fence_);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      encoder.updateFence(fence_);
      encoder.endEncoding();
    }
    cmdbuf.encodeSignalEvent(order_event, frame_id);
    if (task.observer && !drawable.observePresentation(task.observer, task.present_id)) {
      task.observer.cancel(task.present_id);
      task.observer_fault->store(true, std::memory_order_release);
    }

    // Core Animation receives the drawable only after this command buffer completes, and only if
    // no DXGI_PRESENT_RESTART advanced the epoch since this frame left the queue. A discarded
    // frame withdraws its observation there, like a queue-discarded frame.
    cmdbuf.presentDrawableIfCurrent(drawable, task.presenter->restartToken(), task.restart_epoch, task.after,
                                    task.observer, task.present_id);
    owned_graphics::Trace("present_encode", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                          ",\"resource\":", g->owned_id);
    // Encoding completion releases PSO readers. Layer transitions additionally
    // wait for the completion thread to retire this command buffer.
    scope.inflight.present_completion = std::move(task.presenter);
    task.state.reset();
  }

  HRESULT
  Present(Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after,
          const DXMTPresentGeometry &geometry, WMT::Reference<WMT::PresentationObserver> observer,
          uint64_t present_id, std::shared_ptr<std::atomic<bool>> observer_fault,
          DXMTPresentComposition composition, bool do_not_wait,
          std::shared_ptr<DXMTPresentLatency> latency, bool restart, bool borrowed) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!presenter || !backbuffer || !latency)
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

      std::unique_lock<dxmt::mutex> enqueue_lock(mutex_enqueue_, std::defer_lock);
      if (do_not_wait) {
        if (!enqueue_lock.try_lock())
          return DXGI_ERROR_WAS_STILL_DRAWING;
      } else {
        enqueue_lock.lock();
      }
      if (device_->RemovedReason() != S_OK)
        return DXGI_ERROR_DEVICE_REMOVED;
      // D3D11 captures PresentState on the Present caller before deferring the
      // Metal encode. Keep D3D12's presenter/layer state on the same side of
      // the thread boundary; only the GPU encoding belongs to the queue worker.
      // Do this under the queue capture lock so concurrent Present calls cannot
      // snapshot layer/frame state in an order different from their FIFO tasks.
      PresentTask task;
      if (do_not_wait) {
        auto ready = presenter->trySynchronizeLayerProperties();
        if (!ready)
          return DXGI_ERROR_WAS_STILL_DRAWING;
        task.state.emplace(std::move(*ready));
      } else {
        task.state.emplace(presenter->synchronizeLayerProperties(true));
      }
      task.state->metadata.geometry = geometry;
      task.presenter = Rc<Presenter>(presenter);
      task.observer = std::move(observer);
      task.observer_fault = std::move(observer_fault);
      task.latency = std::move(latency);
      task.composition = std::move(composition);
      task.present_id = present_id;
      task.backbuffer = static_cast<MTLD3D12Resource *>(backbuffer);
      task.completion_handle = std::move(retained_completion_handle);
      task.after = after;
      task.credit = std::make_shared<std::atomic<bool>>(true);
      presenter->prepareQueuedPresent();
      // Commit the discard only after all nonblocking/encoding admission has
      // succeeded. Older queue-owned frames observe this epoch before encoding;
      // encoded frames observe it in their completion handler before display.
      if (restart)
        task.latency->discardQueuedForRestart(presenter->restartPresentQueue(), borrowed, hLantecyWaitable);
      task.restart_epoch = presenter->presentRestartEpoch();
      const uint64_t frame_id = task.state->frame_id;
      presenter->registerQueuedPresent(frame_id, task.credit);
      if (task_fifo_.push(QueueTask{std::move(task)}))
        return S_OK;
      presenter->unregisterQueuedPresent(frame_id);
      // The Presenter has already issued this frame ID. If the FIFO is closed,
      // later frames must not wait forever for an ID that was never encoded.
      presenter->cancelPresentOrder();
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return DXGI_ERROR_DEVICE_REMOVED;
    } catch (const std::bad_alloc &) {
      presenter->cancelPresentOrder();
      device_->ReportRemoval(E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
    } catch (...) {
      presenter->cancelPresentOrder();
      device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      return E_FAIL;
    }
  }
};

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  if (!ppCommandQueue)
    return E_POINTER;
  *ppCommandQueue = nullptr;
  if (!pDesc)
    return E_INVALIDARG;
  auto command_queue = Com(new MTLD3D12CommandQueueImpl(pDevice));
  HRESULT hr = command_queue->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  return command_queue->QueryInterface(riid, ppCommandQueue);
};

} // namespace dxmt
