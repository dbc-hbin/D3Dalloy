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
#include "dxmt_format.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "owned_submission_ring.hpp"
#include "owned_queue_task_fifo.hpp"
#include "d3d12_temporal.hpp"
#include "d3d12_frame_generation.hpp"
#include "d3d12_work_graph.hpp"
#include "d3d12_state_object.hpp"
#include "dxil_raytracing.h"
#include "Metal4FX.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <new>
#include <stdexcept>
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
        if (required == D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE) {
          // Capture knows only this Execute batch: an unknown state may have been
          // established by a preceding submission. The FIFO worker must require
          // the explicit state; COMMON can never promote to SHADING_RATE_SOURCE.
          if (!allow_prior_split || slot.current) return false;
          slot.current = required;
          slot.promoted_from_unknown = true;
          slot.implicitly_promoted = false;
          return true;
        }
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

  WMT::Reference<WMT::MTL4CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  WMT::Reference<WMT::Event> timestamp_order_event_;
  uint64_t timestamp_order_value_ = 0; // submission thread only
  WMT::Reference<WMT::Texture> null_shading_rate_image_; // submission thread only; never sampled
  WMT::Reference<WMT::SharedEvent> tile_mapping_event_;
  uint64_t tile_mapping_value_ = 0; // submission thread only
  /* Signal/wait pairs of this shared event flush sparse page aliases (a plain MTLEvent does not). */
  WMT::Reference<WMT::SharedEvent> alias_flush_event_;
  uint64_t alias_flush_value_ = 0; // submission thread only
  bool reserved_volume_publish_pending_ = false; // submission thread only

  void
  EncodeAliasFlush() {
    const uint64_t value = ++alias_flush_value_;
    queue_.signalEvent(alias_flush_event_, value);
    queue_.waitForEvent(alias_flush_event_, value);
  }
  WMT::Reference<WMT::ComputePipelineState> swizzle_import_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> cast_merge_pipeline_;
  WMT::Reference<WMT::ComputePipelineState> volume_render_publish_pipeline_;
  std::array<WMT::Reference<WMT::ComputePipelineState>, 4> writable_msaa_import_;
  std::array<WMT::Reference<WMT::ComputePipelineState>, 2> depth_msaa_import_;
  std::unordered_map<uint64_t, WMT::Reference<WMT::RenderPipelineState>> writable_msaa_export_;
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
    struct FenceOperation {
      Rc<Fence> fence;
      Fence::QueueTicket ticket;
      uint64_t fence_id;
      bool signal;
    };
    EncoderData *entry = nullptr;
    uint64_t list_id = 0;
    uint64_t recording = 0;
    SubmissionLease allocator;
    RecordingLease recording_lease;
    std::vector<FenceOperation> fence_operations;
    std::shared_ptr<DiagnosticBreadcrumb> breadcrumb;
    ListSubmission() = default;
    ListSubmission(ListSubmission &&) = default;
    ListSubmission &operator=(ListSubmission &&) = default;
    ~ListSubmission() {
      for (auto &operation : fence_operations)
        if (!operation.signal && operation.ticket.order)
          operation.ticket.order->removeCompletionWaiter(operation.ticket.waiter_id);
    }
  };

  struct ExecuteTask {
    std::shared_ptr<ReflexState::Work> reflex_work;
    std::vector<ListSubmission> lists;
    bool contains_temporal = false;
  };
  struct MarkerTask { DiagnosticAnnotation annotation; std::shared_ptr<DiagnosticBreadcrumb> breadcrumb; };
  std::vector<std::string> diagnostic_groups_; // FIFO worker only.

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
    std::shared_ptr<ReflexState::Work> reflex_work;
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

  using QueueTask = std::variant<ExecuteTask, SignalTask, WaitTask, PresentTask, TileMappingTask, MarkerTask>;
  owned_graphics::QueueTaskFifo<QueueTask> task_fifo_;
  dxmt::mutex mutex_enqueue_;
  std::atomic<bool> stopping_ = false;
  bool cancel_unsubmitted_ = false; // Submission thread only, after an unresolved shutdown wait.
  std::mutex wait_mutex_;
  std::shared_ptr<owned_graphics::FenceCompletionLatch> blocked_wait_;

  struct InflightCommandBuffer {
    std::vector<std::shared_ptr<DiagnosticBreadcrumb>> breadcrumbs;
    std::shared_ptr<ReflexState::Work> reflex_work;
    uint64_t reflex_driver_start = 0, reflex_driver_end = 0, reflex_queue_start = 0;
    GPUExecutionGate::Ticket execution_ticket;
    WMT::Reference<WMT::MTL4CommandBuffer> cmdbuf{};
    std::shared_ptr<Atomic64Phase> atomic64_phase;
    WMT::Reference<WMT::MTL4Submission> submission{};
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
    std::vector<std::shared_ptr<OwnedFrameGenerationFrame>> frame_generation_frames;
    std::vector<std::shared_ptr<OwnedFrameGenerationFrame>> frame_generation_completions;
    std::optional<SignalTask> fence_completion;
    // Retain the presenter until GPU completion, not merely task encoding.
    Rc<Presenter> present_completion;
    std::shared_ptr<DXMTPresentHistory> present_history;
    struct NativePresent {
      WMT::Reference<WMT::MetalDrawable> drawable;
      WMT::Reference<WMT::SharedEvent> restart_token;
      WMT::Reference<WMT::PresentationObserver> observer;
      Com<MTLD3D12Resource, false> backbuffer;
      WMT::Reference<WMT::Object> surface;
      WMTCompositionFrame frame{};
      uint64_t frame_id = 0;
      uint64_t restart_epoch = 0;
      uint64_t present_id = 0;
      double after = 0;
      uint32_t sync_interval = 0;
      bool allow_tearing = false;
    };
    std::optional<NativePresent> native_present;
    // Stream-output and pipeline-statistics draws of this submission: their capture buffers
    // stay alive until completion, which checks each assembly's invariant status word.
    std::vector<std::shared_ptr<StreamOutputDrawData>> so_draws;
    std::vector<WMT::Reference<WMT::Buffer>> so_indirect_buffers;
    std::vector<Rc<BufferAllocation>> mesh_indirect_owners;
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
    std::vector<TextureViewRef> cast_views;
    std::vector<Rc<TextureAllocation>> cast_allocations;
    std::vector<WMT::Reference<WMT::Heap>> volume_heaps;
    std::vector<WMT::Reference<WMT::Texture>> volume_textures;
  };

  struct NativeEncodingSlot {
    WMT::Reference<WMT::MTL4CommandAllocator> allocator;
    WMT::Reference<WMT::MTL4CommandBuffer> cmdbuf;
  };
  std::array<NativeEncodingSlot, kCommandQueueSize> native_encoding_slots_;
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

      WMTMetal4SubmissionInfo completion{};
      if (inflight.submission) {
        inflight.submission.waitUntilCompleted();
        inflight.submission.getInfo(completion);
      }
      const bool metal_error = !inflight.submission || !completion.completed || completion.error_handle;
      if (metal_error)
        for (const auto &frame : inflight.frame_generation_frames)
          CompleteOwnedFrameGenerationFrame(frame, DXGI_ERROR_DEVICE_REMOVED);
      for (const auto &frame : inflight.frame_generation_completions)
        CompleteOwnedFrameGenerationFrame(frame, metal_error ? DXGI_ERROR_DEVICE_REMOVED : S_OK);
      const bool reflex_present_current = !inflight.native_present ||
          inflight.native_present->restart_token.signaledValue() == inflight.native_present->restart_epoch;
      device_->reflex.Complete(inflight.reflex_work, inflight.reflex_driver_start, inflight.reflex_driver_end,
                              inflight.reflex_queue_start, completion.gpu_start_ns, completion.gpu_end_ns,
                              !metal_error && reflex_present_current);
      if (metal_error) {
        if (completion.error_handle)
          ERR("Metal 4 submission failed: ", inflight.submission.error().description().getUTF8String());
        else
          ERR("Metal 4 submission did not complete");
        device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      } else {
        for (const auto &draw : inflight.so_draws)
          if (draw->status_cpu && draw->status_cpu[0]) {
            ERR("Draw capture: ", draw->status_cpu[0], " primitives lost an unclaimed capture record");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
          }
      }

      owned_graphics::Trace("complete", ",\"queue\":", owned_id_, ",\"submission\":", internal_seq,
                            ",\"status\":", uint32_t(metal_error ? WMTCommandBufferStatusError : WMTCommandBufferStatusCompleted));
      if (inflight.native_present) {
        auto presenter = inflight.present_completion;
        const bool composition = bool(inflight.native_present->surface);
        const uint64_t frame_id = inflight.native_present->frame_id;
        auto observer = composition ? inflight.native_present->observer : WMT::Reference<WMT::PresentationObserver>{};
        const uint64_t present_id = inflight.native_present->present_id;
        auto retire = [present = std::move(*inflight.native_present),
                       presenter = std::move(inflight.present_completion),
                       queue_id = owned_id_, metal_error]() mutable {
          const bool terminal = present.surface &&
              presenter->presentOrderEvent().signaledValue() == UINT64_MAX;
          const bool current = !terminal && present.restart_token.signaledValue() == present.restart_epoch;
          if (!metal_error && current)
            WMT4::PresentMetal4Drawable(present.drawable, present.after,
                                       present.surface ? &present.frame : nullptr, present.sync_interval,
                                       present.surface ? present.allow_tearing : false);
          else if (present.observer)
            present.observer.cancel(present.present_id);
          owned_graphics::Trace("present_native_decision", ",\"queue\":", queue_id,
                                ",\"present_id\":", present.present_id,
                                ",\"presented\":", unsigned(!metal_error && current));
          if (owned_graphics::TraceEnabled())
            Logger::info(str::format("OWNED_GRAPHICS_NATIVE_PRESENT_DECISION present_id=", present.present_id,
                                     " presented=", unsigned(!metal_error && current),
                                     " reason=", !current ? "epoch" : metal_error ? "status" : "current"));
          if (metal_error || terminal)
            presenter->cancelPresentOrder();
          else
            presenter->notifyPresentCompleted();
        };
        if (composition) {
          try {
            presenter->retireNativePresent(frame_id, std::move(retire));
          } catch (const std::bad_alloc &) {
            device_->ReportRemoval(E_OUTOFMEMORY);
            presenter->cancelPresentOrder();
            if (observer) observer.cancel(present_id);
            presenter->abortNativeRetirements();
          }
        } else
          retire();
      }
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
      if (!metal_error)
        for (const auto &breadcrumb : inflight.breadcrumbs) device_->diagnostics.Complete(breadcrumb);
      inflight = {};
      submission_ring_.retire(internal_seq);
      if (fence_completion && device_->RemovedReason() == S_OK)
        fence_completion->fence->markQueueSignalCompleted(fence_completion->ticket);

      internal_seq++;
    }
  };

  struct CommittingScope {
    MTLD3D12CommandQueueImpl *queue;
    GPUExecutionGate::Ticket execution_ticket;
    std::lock_guard<dxmt::mutex> lock;
    GPUExecutionGate::EncodingAdmission encoding_admission;
    uint64_t seq;
    InflightCommandBuffer &inflight;
    WMT::Reference<WMT::Object> pool;
    WMT::Reference<WMT::Event> signal_event;
    uint64_t signal_value = 0;
    bool timestamp_predecessors_ordered = false;
    WMT::Reference<WMT::MetalDrawable> signal_drawable;
    // This stays locked through the destructor's commit(), then releases. No
    // GPU completion or ring-capacity wait occurs while it is held.
    std::unique_lock<std::mutex> temporal_submission_lock;

    static GPUExecutionGate::Ticket Admit(MTLD3D12CommandQueueImpl *queue, bool exclusive) {
      auto ticket = exclusive ? GPUExecutionGate::Ticket{} : queue->device_->gpu_execution_gate.AcquireShared();
      if (!exclusive && !ticket) throw HRESULT(DXGI_ERROR_DEVICE_REMOVED);
      return ticket;
    }
    // Exclusive phase callers already own the device ticket and retain it through retirement.
    CommittingScope(MTLD3D12CommandQueueImpl *queue, bool exclusive = false) :
        queue(queue),
        execution_ticket(Admit(queue, exclusive)),
        lock(queue->mutex_commit_),
        encoding_admission(queue->device_->gpu_execution_gate),
        seq(queue->submission_ring_.reserve()),
        inflight(queue->inflight_cmdbuf_pool_[seq % kCommandQueueSize]),
        pool(WMT::MakeAutoreleasePool()) {
      inflight.execution_ticket = std::move(execution_ticket);
      auto &slot = queue->native_encoding_slots_[seq % kCommandQueueSize];
      // reserve() does not return this slot until its previous submission retired.
      slot.allocator.reset();
      slot.cmdbuf.begin(slot.allocator);
      inflight.cmdbuf = slot.cmdbuf;
      auto label = str::format("D3D12 queue ", queue->owned_id_, " submission ", seq);
      if (inflight.cmdbuf.diagnostic(3, label.data(), label.size())) queue->device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      for (const auto &group : queue->diagnostic_groups_)
        if (inflight.cmdbuf.diagnostic(1, group.data(), group.size())) queue->device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    };

    ~CommittingScope() {
      inflight.cmdbuf.end();
      WMT::MTL4CommandBuffer buffer = inflight.cmdbuf;
      if (inflight.reflex_work) {
        inflight.reflex_driver_end = ReflexState::NowUs();
        inflight.reflex_queue_start = inflight.reflex_driver_end;
      }
      inflight.submission = queue->device_->CommitMetal4CommandBuffers(queue->queue_, &buffer, 1);
      if (inflight.submission)
        for (const auto &frame : inflight.frame_generation_frames)
          CommitOwnedFrameGenerationFrame(frame, queue->owned_id_, seq);
      if (signal_event)
        queue->queue_.signalEvent(signal_event, signal_value);
      if (signal_drawable)
        queue->queue_.signalDrawable(signal_drawable);
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
      } else if (device_->RemovedReason() == S_OK) {
        try {
          std::visit([this](auto &typed) { this->Process(typed); }, task);
        } catch (HRESULT reason) {
          device_->ReportRemoval(reason);
          if (auto *present = std::get_if<PresentTask>(&task)) CancelPresent(*present);
        }
      }
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
  void ApplyAnnotation(WMT::MTL4CommandBuffer cmdbuf, const DiagnosticAnnotation &a) {
    if (a.action == 2 && diagnostic_groups_.empty()) {
      device_->diagnostics.Validation(D3D12_MESSAGE_ID_UNKNOWN, "EndEvent without a preceding BeginEvent");
      return;
    }
    if (cmdbuf.diagnostic(a.action, a.label.data(), a.label.size())) throw HRESULT(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    if (a.action == 1) diagnostic_groups_.push_back(a.label);
    if (a.action == 2) diagnostic_groups_.pop_back();
  }
  void Process(MarkerTask &task) {
    auto scope = StartCommitting();
    scope.inflight.breadcrumbs.push_back(task.breadcrumb);
    ApplyAnnotation(scope.inflight.cmdbuf, task.annotation);
    owned_graphics::Trace("queue_annotation", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
        ",\"action\":", task.annotation.action, ",\"metadata\":", task.annotation.metadata, ",\"bytes\":", task.annotation.payload.size());
  }

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
    auto execution_ticket = device_->gpu_execution_gate.AcquireShared();
    if (!execution_ticket) return;
    std::lock_guard<std::mutex> lock(ReservedResource::MappingQueueMutex);
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
    } else {
      const HRESULT hr = device_->CompleteSparseVolumeResidency();
      if (FAILED(hr)) device_->ReportRemoval(hr);
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
    return native->reserved && (native->reserved->isBuffer() || native->owned_is_reserved) ? native : nullptr;
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
    if (task.state) {
      task.presenter->unregisterQueuedPresent(task.state->frame_id);
      if (task.composition.surface)
        task.presenter->abortNativeRetirements();
    }
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
        (pDesc->Flags & ~(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT | kCommandQueueFlagAllowDynamicPriority)) ||
        pDesc->NodeMask > 1)
      return E_INVALIDARG;
    desc_ = *pDesc;
    desc_.NodeMask = 1; // one physical node

    auto metal_device = device_->GetMTLDevice();
    queue_ = metal_device.newMTL4CommandQueue();
    if (!queue_)
      return E_FAIL;
    queue_.addResidencySet(device_->GetGlobalResidencySet());
    // Native allocation failure must precede reserve(): an unpublished ring
    // reservation would otherwise permanently block retirement and shutdown.
    for (auto &slot : native_encoding_slots_) {
      slot.allocator = metal_device.newMTL4CommandAllocator();
      slot.cmdbuf = metal_device.newMTL4CommandBuffer();
      if (!slot.allocator || !slot.cmdbuf)
        return E_OUTOFMEMORY;
    }

    fence_ = metal_device.newFence();
    if (!fence_)
      return E_FAIL;
    timestamp_order_event_ = metal_device.newEvent();
    if (!timestamp_order_event_)
      return E_FAIL;
    tile_mapping_event_ = metal_device.newSharedEvent();
    if (!tile_mapping_event_)
      return E_FAIL;
    alias_flush_event_ = metal_device.newSharedEvent();
    if (!alias_flush_event_)
      return E_FAIL;
    owned_graphics::Trace("queue_create", ",\"queue\":", owned_id_, ",\"type\":", uint32_t(desc_.Type),
                          ",\"metal_api\":4");

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue) ||
        riid == __uuidof(ID3D12CommandQueue1)) {
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

  HRESULT STDMETHODCALLTYPE SetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY priority) final {
    if (priority != D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_NORMAL &&
        priority != D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH)
      return E_INVALIDARG;
    if (!(desc_.Flags & kCommandQueueFlagAllowDynamicPriority))
      return DXGI_ERROR_UNSUPPORTED;
    // Metal 4 has no GPU scheduling-priority setter. S_FALSE means ignored, not applied:
    // never substitute the submission thread QoS or change the value reported by Get*.
    return S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE GetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY *priority) final {
    if (!priority)
      return E_INVALIDARG;
    *priority = desc_.Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL
        ? D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_NORMAL : D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY priority) final {
    if (priority != D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_IDLE &&
        priority != D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_DEFAULT &&
        priority != D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_NORMAL_0 &&
        (priority < D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_SOFT_REALTIME_0 ||
         priority > D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_HARD_REALTIME))
      return E_INVALIDARG;
    if (!(desc_.Flags & kCommandQueueFlagAllowDynamicPriority))
      return DXGI_ERROR_UNSUPPORTED;
    return S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE GetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY *priority) final {
    if (!priority)
      return E_INVALIDARG;
    *priority = desc_.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME
        ? D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_HARD_REALTIME : D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_DEFAULT;
    return S_OK;
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
  EncodeStreamOutputSetup(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
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
  EncodeStreamOutputAssembly(WMT::MTL4CommandBuffer cmdbuf, const StreamOutputDrawData &draw) {
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
    bind(2, draw.scratch, draw.scratch_offset);
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
  bool PrepareROVMarkers(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
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
      WMT::SetRenderTargetArrayLength(clear, scratch.layers);
      clear.render_target_width = scratch.width;
      clear.render_target_height = scratch.height;
      auto encoder = cmdbuf.renderCommandEncoder(clear);
      encoder.waitForFence(fence_, WMTRenderStageFragment);
      encoder.updateFence(fence_, WMTRenderStageFragment);
      encoder.endEncoding();
    }
    return true;
  }

  bool EncodeObjectCapture(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                           RenderEncoderData &render, const MSAADrawData &draw,
                           WMT::Reference<WMT::Buffer> &records, const uint32_t *&mapped) {
    const uint64_t bytes = uint64_t(draw.mesh_control.total_slots) * draw.object_layout.record_dwords * 4;
    auto metal = device_->GetMTLDevice();
    if (!bytes || bytes > metal.maxBufferLength() - sizeof(SM50_MSAA_MESH_CONTROL)) return false;
    WMTBufferInfo buffer{}; buffer.length = bytes + sizeof(SM50_MSAA_MESH_CONTROL);
    buffer.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE | WMTResourceHazardTrackingModeUntracked;
    records = metal.newBuffer(buffer);
    mapped = static_cast<const uint32_t *>(buffer.memory.get_accessible_or_null());
    if (!records || !mapped) return false;
    memcpy(reinterpret_cast<uint8_t *>(const_cast<uint32_t *>(mapped)) + bytes, &draw.mesh_control, sizeof(draw.mesh_control));
    inflight.msaa_buffers.push_back(records);
    auto blit = cmdbuf.blitCommandEncoder(); blit.waitForFence(fence_);
    wmtcmd_blit_fillbuffer fill{}; fill.type = WMTBlitCommandFillBuffer; fill.buffer = records; fill.length = bytes;
    blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill)); blit.updateFence(fence_); blit.endEncoding();
    WMTRenderPassInfo info{}; WMT::InitializeRenderPassInfo(info);
    info.render_target_width = std::max(1u, draw.bounds.target_width);
    info.render_target_height = std::max(1u, draw.bounds.target_height);
    WMT::SetRenderTargetArrayLength(info, std::max(1u, draw.bounds.target_array_length));
    info.default_raster_sample_count = 1;
    // The full recorded state includes active visibility mode even though this producer emits no raster.
    // Preserve its retained query scratch; only the later raster passes fan out visible samples.
    info.visibility_buffer = render.visibility_scratch;
    auto encoder = cmdbuf.renderCommandEncoder(info); encoder.waitForFence(fence_, WMTRenderStageObject | WMTRenderStageMesh);
    encoder.setObjectBuffer(records, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
    encoder.setObjectBuffer(records, bytes, SM50_BINDING_INDEX_MSAA_MESH_CONTROL);
    encoder.setMeshBuffer(records, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
    encoder.setMeshBuffer(records, bytes, SM50_BINDING_INDEX_MSAA_MESH_CONTROL);
    encoder.encodeCommands(&render.cmd_head);
    encoder.updateFence(fence_, WMTRenderStageObject | WMTRenderStageMesh); encoder.endEncoding();
    return true;
  }

  void EncodeObjectReplay(WMT::MTL4RenderCommandEncoder &encoder, RenderEncoderData &render, const MSAADrawData &draw) {
    WMTSize object_threads{1, 1, 1}, mesh_threads{1, 1, 1};
    for (auto *node = static_cast<wmtcmd_base *>(render.cmd_head.next.get()); node;
         node = static_cast<wmtcmd_base *>(node->next.get())) {
      auto bind = [&]<typename T>() {
        auto command = *reinterpret_cast<const T *>(node); command.next.set(nullptr);
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&command));
      };
      switch (node->type) {
      case WMTRenderCommandUseResource: bind.operator()<wmtcmd_render_useresource>(); break;
      case WMTRenderCommandSetObjectBuffer:
      case WMTRenderCommandSetMeshBuffer: bind.operator()<wmtcmd_render_setbuffer>(); break;
      case WMTRenderCommandSetObjectBufferOffset:
      case WMTRenderCommandSetMeshBufferOffset: bind.operator()<wmtcmd_render_setbufferoffset>(); break;
      case WMTRenderCommandDrawMeshThreadgroups: {
        const auto *command = reinterpret_cast<const wmtcmd_render_draw_meshthreadgroups *>(node);
        object_threads = command->object_threadgroup_size; mesh_threads = command->mesh_threadgroup_size;
        break;
      }
      case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
        const auto *command = reinterpret_cast<const wmtcmd_render_draw_meshthreadgroups_indirect *>(node);
        object_threads = command->object_threadgroup_size; mesh_threads = command->mesh_threadgroup_size;
        break;
      }
      default: break; // Raster-only state cannot affect an attachmentless object/mesh capture.
      }
    }
    encoder.setObjectBuffer(draw.object_records, draw.object_record_offset, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
    wmtcmd_render_setpso pipeline{}; pipeline.type = WMTRenderCommandSetPSO; pipeline.pso = draw.object_replay_pipeline;
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&pipeline));
    wmtcmd_render_draw_meshthreadgroups dispatch{}; dispatch.type = WMTRenderCommandDrawMeshThreadgroups;
    dispatch.threadgroup_per_grid = {1, 1, 1}; dispatch.object_threadgroup_size = object_threads; dispatch.mesh_threadgroup_size = mesh_threads;
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&dispatch));
  }

  struct MeshCaptureExecution;
  bool EncodeMSAAProducer(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                          RenderEncoderData &render, const MSAADrawData &draw,
                          MSAAExecution &execution, const MeshCaptureExecution *capture_indirect = nullptr,
                          uint32_t capture_command = UINT32_MAX) {
    auto metal = device_->GetMTLDevice();
    const uint64_t record_count = uint64_t(draw.assembly.vertex_count) * draw.assembly.instance_count;
    const uint64_t max_primitives = draw.assembly.max_primitives;
    const uint64_t max_buffer = metal.maxBufferLength();
    const uint64_t vertex_stride = uint64_t(draw.vertex_layout.record_dwords) * 4;
    if (record_count > UINT32_MAX || max_primitives > UINT32_MAX ||
        !vertex_stride || record_count > max_buffer / vertex_stride ||
        max_primitives > max_buffer / (MSAAIndexStride(draw.line_mode, draw.conservative) * sizeof(uint32_t)) ||
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
    const uint64_t capture_bytes = draw.mesh_capture
        ? uint64_t(draw.mesh_control.total_slots) * draw.mesh_layout.capture_dwords_per_group * 4
        : record_count * uint64_t(draw.vertex_layout.record_dwords) * 4;

    execution.vertices = allocate(capture_bytes, false);
    execution.indices = allocate(max_primitives *
        MSAAIndexStride(draw.line_mode, draw.conservative) * sizeof(uint32_t), false);
    execution.draws = allocate(max_primitives * sizeof(MSAAPrimitiveIndirectArguments), false);
    execution.capture_dispatch = allocate(16, false);
    const void *mapped = nullptr;
    execution.bounds = allocate(max_primitives * sizeof(MSAAPrimitiveTileBounds), true, &mapped);
    execution.mapped_bounds = static_cast<const MSAAPrimitiveTileBounds *>(mapped);
    execution.max_primitives = uint32_t(max_primitives);
    const void *parameters_memory = nullptr;
    auto parameters = allocate(sizeof(MSAAPrimitiveAssemblyParams) + sizeof(MSAAPrimitiveBoundsParams) +
                               sizeof(SM50_MSAA_VERTEX_CAPTURE_CONTROL) + sizeof(SM50_MSAA_MESH_CONTROL),
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
        draw.assembly.topology == 2 || draw.assembly.topology == 4 || draw.assembly.topology == 5
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

    if (draw.mesh_capture) {
      constexpr uint64_t mesh_control_offset = control_offset + sizeof(SM50_MSAA_VERTEX_CAPTURE_CONTROL);
      memcpy(parameters_bytes + mesh_control_offset, &draw.mesh_control, sizeof(draw.mesh_control));
      auto blit = cmdbuf.blitCommandEncoder();
      blit.waitForFence(fence_);
      wmtcmd_blit_fillbuffer fill{};
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = execution.vertices;
      fill.length = capture_bytes;
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
      blit.updateFence(fence_);
      blit.endEncoding();
      WMTRenderPassInfo info{};
      WMT::InitializeRenderPassInfo(info);
      // This producer replays the application viewport/scissor state even though
      // it does not rasterize. Metal still validates that state against the pass.
      info.render_target_width = std::max(1u, draw.bounds.target_width);
      info.render_target_height = std::max(1u, draw.bounds.target_height);
      WMT::SetRenderTargetArrayLength(info, draw.bounds.target_array_length);
      info.default_raster_sample_count = 1;
      // GS/HS/DS producers replay the same query state chain as the draw they capture.
      info.visibility_buffer = render.visibility_scratch;
      auto encoder = cmdbuf.renderCommandEncoder(info);
      encoder.waitForFence(fence_, WMTRenderStageObject | WMTRenderStageMesh);
      wmtcmd_render_setbuffer records{}, control_binding{};
      records.type = control_binding.type = WMTRenderCommandSetMeshBuffer;
      records.buffer = execution.vertices;
      records.index = SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS;
      control_binding.buffer = parameters;
      control_binding.offset = mesh_control_offset;
      control_binding.index = SM50_BINDING_INDEX_MSAA_MESH_CONTROL;
      records.next.set(&control_binding);
      control_binding.next.set(draw.object_records ? nullptr : &render.cmd_head);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&records));
      if (draw.object_records) EncodeObjectReplay(encoder, render, draw);
      if (capture_indirect)
        EncodeMeshCaptureDraws(encoder, *capture_indirect, render.capture_indirect_pass, capture_command);
      encoder.updateFence(fence_, WMTRenderStageObject | WMTRenderStageMesh);
      encoder.endEncoding();
    } else {
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
    }

    if (max_primitives)
      encode_kernel(draw.bounds_pipeline, {max_primitives, 1, 1}, {64, 1, 1}, {{
          {parameters, bounds_offset}, {execution.vertices, 0}, {execution.indices, 0},
          {execution.draws, 0}, {execution.bounds, 0}, {fallback, 0}}});
    return true;
  }

  bool EncodeMSAARender(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                        RenderEncoderData &render, const MSAADrawData &draw,
                        const MSAAExecution &execution) {
    auto metal = device_->GetMTLDevice();
    WMTTextureInfo coverage_info{};
    if ((draw.pixel_layout.flags & SM50_MSAA_PIXEL_COARSE) && !draw.shading_rate_image &&
        !null_shading_rate_image_) {
      WMTTextureInfo info{};
      info.type = WMTTextureType2D;
      info.width = info.height = info.depth = info.array_length = info.mipmap_level_count = info.sample_count = 1;
      info.pixel_format = WMTPixelFormatR8Uint;
      info.options = WMTResourceStorageModePrivate;
      info.usage = WMTTextureUsageShaderRead;
      null_shading_rate_image_ = metal.newTexture(info);
      if (!null_shading_rate_image_) return false;
    }
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
            draw.atomic64_phase ? 1u : draw.rov_marker_sample_count, rov_markers))
      return false;
    // ForcedSampleCount 16 has no native raster: the geometry, coverage, query and (target-rate) replay passes run
    // as two 8x halves with the D3D 16x pattern halves programmed; tile.sample_base maps native to logical samples.
    const bool split16 = draw.sample_count > 8;
    const uint32_t native_samples = split16 ? 8u : draw.sample_count;
    const uint32_t halves = split16 ? 2u : 1u;
    const bool multisampled = native_samples > 1;
    const auto coverage_view_type = multisampled ? WMTTextureType2DMultisample : WMTTextureType2D;
    coverage_info.type = layer_count > 1
        ? multisampled ? WMTTextureType2DMultisampleArray : WMTTextureType2DArray
        : coverage_view_type;
    coverage_info.width = render_width;
    coverage_info.height = render_height;
    coverage_info.depth = 1;
    coverage_info.array_length = layer_count;
    coverage_info.mipmap_level_count = 1;
    coverage_info.sample_count = native_samples;
    const auto coverage_format = draw.depth_bounds_probe_pipeline ? WMTPixelFormatR8Uint : WMTPixelFormatR32Uint;
    coverage_info.pixel_format = coverage_format;
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
    const bool promoted = draw.atomic64_phase;
    const bool saved_outputs = bool(draw.pixel_pipeline) || (promoted && draw.sample_pixel_pipeline);
    const uint64_t output_bytes = uint64_t(64) * draw.pixel_layout.output_record_dwords * 4 *
        (promoted && draw.sample_pixel_pipeline ? draw.sample_count : 1u);
    if (!input_bytes || input_bytes > metal.maxBufferLength() ||
        (saved_outputs && (!output_bytes || output_bytes > metal.maxBufferLength()))) return false;
    auto make_scratch = [&](uint64_t size) {
      WMTBufferInfo info{};
      info.length = size;
      info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
      auto buffer = metal.newBuffer(info);
      if (buffer) inflight.msaa_buffers.push_back(buffer);
      return buffer;
    };
    auto inputs = make_scratch(input_bytes);
    auto outputs = saved_outputs ? make_scratch(output_bytes) : WMT::Reference<WMT::Buffer>{};
    if (!inputs || (saved_outputs && !outputs)) return false;
    const bool emulated_bounds = draw.depth_bounds_probe_pipeline && draw.depth_bounds_enabled;
    std::vector<WMT::Reference<WMT::Texture>> depth_views(emulated_bounds ? layer_count : 0);
    const bool early = draw.pixel_layout.flags & SM50_MSAA_PIXEL_FORCE_EARLY;
    auto bind_constants = [&](WMT::MTL4RenderCommandEncoder &encoder, uint32_t vertex, uint32_t fragment) {
      if (draw.constants[vertex])
        encoder.setVertexBuffer(draw.constants[vertex], draw.constant_offsets[vertex],
                                SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
      if (draw.constants[fragment])
        encoder.setFragmentBuffer(draw.constants[fragment], draw.constant_offsets[fragment],
                                  SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
    };
    auto bind_rov_markers = [&](WMT::MTL4RenderCommandEncoder &encoder, uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
        encoder.setFragmentTexture(rov_markers[i], 31 + i);
        encoder.useResource(rov_markers[i], WMTResourceUsageRead | WMTResourceUsageWrite,
                            WMTRenderStageFragment);
      }
    };
    auto bind_tile = [&](WMT::MTL4RenderCommandEncoder &encoder, const SM50_MSAA_TILE_CONTROL &tile,
                         uint32_t vertex, uint32_t fragment, bool count_visibility, bool gather = false) {
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
        case WMTRenderCommandSetDepthBounds:
          break;
        // The mesh producer has already executed once. Replay its state, never
        // its application stages (which would duplicate UAV/statistics/SO writes).
        case WMTRenderCommandDrawMeshThreadgroups:
        case WMTRenderCommandDrawMeshThreadgroupsIndirect:
        case WMTRenderCommandDXMTGeometryDraw:
        case WMTRenderCommandDXMTGeometryDrawIndexed:
        case WMTRenderCommandDXMTGeometryDrawIndirect:
        case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
        case WMTRenderCommandDXMTTessellationMeshDraw:
        case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
        case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
        case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
          if (!draw.mesh_capture) return false;
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
          // Conservative replay geometry extrapolates the primitive's plane past its clipped extent; depth
          // clipping is part of the exact coverage polygon, and extrapolated depth is clamped to the viewport.
          if (draw.conservative)
            raster.depth_clip_mode = WMTDepthClipModeClamp;
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
        // Every MSAA pass sets its own tile scissor right after bind_tile. Replaying the application scissor
        // first can exceed an attachmentless pass sized to the primitive bounds (Metal validation).
        case WMTRenderCommandSetScissorRects:
        case WMTRenderCommandSetScissorRect: break;
        case WMTRenderCommandSetDSSO: bind_record.operator()<wmtcmd_render_setdsso>(); break;
        case WMTRenderCommandSetDepthStencilState: bind_record.operator()<wmtcmd_render_setdepthstencilstate>(); break;
        case WMTRenderCommandSetBlendFactorAndStencilRef:
        case WMTRenderCommandSetBlendFactor: bind_record.operator()<wmtcmd_render_setblendcolor>(); break;
        case WMTRenderCommandSetStencilRef: bind_record.operator()<wmtcmd_render_setstencilref>(); break;
        case WMTRenderCommandSetStencilRefFrontBack:
          bind_record.operator()<wmtcmd_render_setstencilref_front_back>();
          break;
        case WMTRenderCommandSetVisibilityMode:
          // Enabling counting registers a Reset write even if disabled before
          // the draw. Only passes that contribute to the query may replay it.
          if (count_visibility)
            bind_record.operator()<wmtcmd_render_setvisibilitymode>();
          break;
        case WMTRenderCommandMemoryBarrier: bind_record.operator()<wmtcmd_render_memory_barrier>(); break;
        // A view location recorded for an earlier native draw; this draw's own location is set below.
        case WMTRenderCommandSetVertexAmplification: break;
        default:
          ERR("Unsupported recorded MSAA render state command ", node->type);
          return false;
        }
      }
      if (draw.bounds.view_offsets_enabled) {
        // Only the captured-vertex replay pipelines carry maxVertexAmplificationCount 2 and apply this mapping;
        // fullscreen pipelines (maximum 1) ignore it. msaa_primitive_bounds applies the same location on the CPU
        // side (coverage layer, tile viewport).
        wmtcmd_render_set_vertex_amplification amplification{};
        amplification.type = WMTRenderCommandSetVertexAmplification;
        amplification.viewport_offset = draw.bounds.view_viewport_offset;
        amplification.render_target_array_offset = draw.bounds.view_layer_offset;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&amplification));
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
      if (draw.statistics_claim) {
        encoder.useResource(draw.statistics_claim, WMTResourceUsageRead | WMTResourceUsageWrite, WMTRenderStageFragment);
        encoder.setFragmentBuffer(draw.statistics_claim, draw.statistics_claim_offset,
                                  SM50_BINDING_INDEX_STATISTICS_CLAIM);
      }
      encoder.setVertexBuffer(execution.vertices, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
      if (draw.line_mode != SM50_MSAA_LINE_NONE || draw.conservative) {
        encoder.setVertexBuffer(execution.parameters, sizeof(MSAAPrimitiveAssemblyParams),
                                SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
        encoder.setVertexBuffer(execution.indices, 0, SM50_BINDING_INDEX_MSAA_COVERAGE);
      }
      if (draw.line_mode != SM50_MSAA_LINE_NONE ||
          (draw.pixel_layout.flags & (SM50_MSAA_PIXEL_DISTANCE_INPUTS | SM50_MSAA_PIXEL_COARSE))) {
        encoder.setFragmentBuffer(execution.vertices, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
        encoder.setFragmentBuffer(execution.indices, 0, SM50_BINDING_INDEX_MSAA_COVERAGE);
      }
      encoder.setFragmentBuffer(inputs, 0, SM50_BINDING_INDEX_MSAA_PIXEL_INPUTS);
      if (outputs)
        encoder.setFragmentBuffer(outputs, 0, SM50_BINDING_INDEX_MSAA_PIXEL_OUTPUTS);
      encoder.setFragmentBytes(&tile, sizeof(tile), SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
      encoder.setFragmentTexture(sampled_coverage, SM50_BINDING_INDEX_MSAA_COVERAGE);
      if (draw.pixel_layout.flags & SM50_MSAA_PIXEL_COARSE) {
        const auto image = draw.shading_rate_image ? WMT::Texture(draw.shading_rate_image)
                                                  : WMT::Texture(null_shading_rate_image_);
        encoder.setFragmentTexture(image, 13);
        encoder.useResource(image, WMTResourceUsageRead, WMTRenderStageFragment);
      }
      encoder.setFragmentBytes(&draw.pixel_layout, sizeof(draw.pixel_layout),
                               SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
      return true;
    };
    auto scissor = [&](WMT::MTL4RenderCommandEncoder &encoder, const MSAAPrimitiveTileBounds &bounds,
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
    auto no_cull = [&](WMT::MTL4RenderCommandEncoder &encoder) {
      wmtcmd_render_setrasterizerstate raster{};
      raster.type = WMTRenderCommandSetRasterizerState;
      raster.fill_mode = WMTTriangleFillModeFill;
      raster.cull_mode = WMTCullModeNone;
      // Conservative coverage clips the original polygon before expansion. Clipping the expanded
      // rectangle to its extrapolated depth plane would discard covered pixels outside a thin triangle.
      raster.depth_clip_mode = draw.depth_clip_enabled && !draw.conservative
          ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
      raster.winding = WMTWindingCounterClockwise;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&raster));
    };
    auto set_depth_bounds = [&](WMT::MTL4RenderCommandEncoder &encoder) {
      if (!draw.depth_bounds_enabled || draw.depth_bounds_probe_pipeline) return;
      wmtcmd_render_setdepthbounds bounds{};
      bounds.type = WMTRenderCommandSetDepthBounds;
      bounds.min_depth = std::clamp(draw.depth_bounds_min, 0.0f, 1.0f);
      bounds.max_depth = std::clamp(draw.depth_bounds_max, 0.0f, 1.0f);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&bounds));
    };
    auto draw_primitive = [&](WMT::MTL4RenderCommandEncoder &encoder, uint32_t primitive) {
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
      if (emulated_bounds && !depth_views[bounds.target_layer]) {
        uint64_t resource_id = 0;
        const auto &attachment = render.depth.attachment;
        depth_views[bounds.target_layer] = attachment.texture().newTextureView(
            attachment->allocation->descriptor->pixelFormat(attachment->key), coverage_view_type,
            render.depth.level, 1, render.depth.slice + bounds.target_layer, 1,
            {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, resource_id);
        if (!depth_views[bounds.target_layer]) return false;
        inflight.msaa_textures.push_back(depth_views[bounds.target_layer]);
      }
      if (layer_count > 1) {
        auto &view = layer_views[bounds.target_layer];
        if (!view) {
          uint64_t resource_id = 0;
          view = coverage.newTextureView(
              coverage_format, coverage_view_type, 0, 1,
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
                coverage_format, coverage_view_type, 0, 1,
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
          // With a forced raster pattern, SampleMask selects OUTPUT (RTV) samples, not virtual raster sample bits:
          // the raster mask is full when any output sample is enabled, else empty.
          const uint32_t output_mask = draw.output_sample_count >= 32
              ? draw.sample_mask : draw.sample_mask & ((1u << draw.output_sample_count) - 1u);
          const uint32_t raster_mask = draw.single_sample_replay
              ? (output_mask ? UINT32_MAX : 0u) : draw.sample_mask;
          const auto &viewport = draw.bounds.viewports[bounds.viewport_index];
          SM50_MSAA_TILE_CONTROL tile{x, y, draw.sample_count, raster_mask,
              draw.viewport_depths[bounds.viewport_index][0],
              draw.viewport_depths[bounds.viewport_index][1], bounds.primitive_id, primitive,
              viewport[0], viewport[1], viewport[2], viewport[3], output_mask, 0, draw.shading_rate,
              draw.shading_rate_image_width, draw.shading_rate_image_height};
          tile.depth_bounds_enabled = emulated_bounds;
          if (emulated_bounds) {
            static_assert(sizeof(SM50_MSAA_DEPTH_BOUNDS_PROBE) == 6 * sizeof(uint32_t));
            SM50_MSAA_DEPTH_BOUNDS_PROBE probe{x, y, draw.pixel_layout.input_record_dwords,
                draw.pixel_layout.depth_bounds_mask_dword, draw.depth_bounds_min, draw.depth_bounds_max};
            wmtcmd_compute_setpso pipeline{};
            wmtcmd_compute_settexture source{};
            wmtcmd_compute_setbuffer records{};
            wmtcmd_compute_setbytes constants{};
            wmtcmd_compute_dispatch dispatch{};
            pipeline.type = WMTComputeCommandSetPSO; pipeline.pso = draw.depth_bounds_probe_pipeline;
            pipeline.threadgroup_size = {8, 8, 1}; pipeline.next.set(&source);
            source.type = WMTComputeCommandSetTexture; source.texture = depth_views[bounds.target_layer];
            source.index = 0; source.next.set(&records);
            records.type = WMTComputeCommandSetBuffer; records.buffer = inputs;
            records.index = 0; records.next.set(&constants);
            constants.type = WMTComputeCommandSetBytes; constants.bytes.set(&probe);
            constants.length = sizeof(probe); constants.index = 1; constants.next.set(&dispatch);
            dispatch.type = WMTComputeCommandDispatchThreads; dispatch.size = {8, 8, 1};
            // The shared fence includes the previous primitive replay/depth writes and scratch clear.
            // No render encoder binds the DSV while this live per-sample read executes.
            auto encoder = cmdbuf.computeCommandEncoder(false);
            encoder.waitForFence(fence_);
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pipeline));
            encoder.updateFence(fence_); encoder.endEncoding();
          }
          tile.programmable_sample_count = draw.sample_positions.count;
          for (uint32_t sample = 0; sample < draw.sample_positions.count; ++sample)
            for (uint32_t axis = 0; axis < 2; ++axis)
              tile.programmable_sample_positions[sample][axis] = draw.sample_positions.positions[sample][axis] / 16.f;
          WMTRenderPassInfo clear_info{};
          WMT::InitializeRenderPassInfo(clear_info);
          clear_info.colors[0].texture = sampled_coverage;
          clear_info.colors[0].load_action = WMTLoadActionLoad;
          clear_info.colors[0].store_action = WMTStoreActionStore;
          clear_info.default_raster_sample_count = native_samples;
          clear_info.center_sample_pattern = draw.center_sample_pattern;
          clear_info.sample_positions = draw.sample_positions;
          clear_info.render_target_width = render_width;
          clear_info.render_target_height = render_height;
          clear_info.visibility_buffer = render.visibility_scratch;
          {
            auto encoder = cmdbuf.renderCommandEncoder(clear_info);
            encoder.waitForFence(fence_, WMTRenderStageFragment);
            if (!bind_tile(encoder, tile, 1, 2, false)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, true);
            no_cull(encoder);
            encoder.setDepthStencilState(draw.no_depth_stencil);
            encoder.setRenderPipelineState(draw.clear_coverage_pipeline);
            encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
            encoder.updateFence(fence_, WMTRenderStageFragment);
            encoder.endEncoding();
          }
          auto geometry_info = clear_info;
          geometry_info.colors[0].texture = coverage;
          WMT::SetRenderTargetArrayLength(geometry_info, layer_count);
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
          for (uint32_t half = 0; half < halves; ++half) {
            tile.sample_base = half * 8;
            geometry_info.forced16_half = split16 ? half + 1 : 0;
            const bool gather_visibility = !draw.pixel_pipeline && !draw.sample_pixel_pipeline &&
                                           !draw.early_depth_pipeline && !draw.late_depth_pipeline;
            {
              auto encoder = cmdbuf.renderCommandEncoder(geometry_info);
              encoder.waitForFence(fence_, WMTRenderStageVertex);
              if (!bind_tile(encoder, tile, 1, 2, gather_visibility, true)) { encoder.endEncoding(); return false; }
              scissor(encoder, bounds, x, y, false);
              encoder.setDepthStencilState(draw.capture_depth_state);
              encoder.setRenderPipelineState(draw.gather_depth_pipeline);
              draw_primitive(encoder, primitive);
              encoder.updateFence(fence_, WMTRenderStageFragment);
              encoder.endEncoding();
            }
            if (gather_visibility)
              fanout_visibility();
            if (draw.pixel_pipeline || draw.sample_pixel_pipeline) {
              auto encoder = cmdbuf.renderCommandEncoder(geometry_info);
              encoder.waitForFence(fence_, WMTRenderStageVertex);
              if (!bind_tile(encoder, tile, 1, 2, false)) { encoder.endEncoding(); return false; }
              scissor(encoder, bounds, x, y, false);
              encoder.setDepthStencilState(draw.capture_depth_state);
              encoder.setRenderPipelineState((render.depth.attachment || render.stencil.attachment)
                                                 ? WMT::RenderPipelineState(draw.coverage_with_depth_pipeline)
                                                 : WMT::RenderPipelineState(draw.coverage_pipeline));
              draw_primitive(encoder, primitive);
              encoder.updateFence(fence_, WMTRenderStageFragment);
              encoder.endEncoding();
            }
          }
          tile.sample_base = 0;
          geometry_info.forced16_half = 0;
          const bool early_depth_executed = early && draw.early_depth_pipeline;
          if (early_depth_executed) {
            auto clear_post_info = clear_info;
            clear_post_info.colors[0].texture = layer_count > 1
                ? WMT::Texture(post_views[bounds.target_layer]) : WMT::Texture(post_coverage);
            {
              auto encoder = cmdbuf.renderCommandEncoder(clear_post_info);
              encoder.waitForFence(fence_, WMTRenderStageFragment);
              if (!bind_tile(encoder, tile, 1, 2, false)) { encoder.endEncoding(); return false; }
              scissor(encoder, bounds, x, y, true);
              no_cull(encoder);
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
            if (!bind_tile(encoder, tile, 1, 2, true)) { encoder.endEncoding(); return false; }
            scissor(encoder, bounds, x, y, false);
            encoder.setDepthStencilState(draw.original_depth_stencil);
            set_depth_bounds(encoder);
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
          pixel_info.render_target_width = promoted ? 32 : render_width;
          pixel_info.render_target_height = promoted ? 32 : render_height;
          pixel_info.visibility_buffer = render.visibility_scratch;
          if (draw.pixel_pipeline || (promoted && draw.sample_pixel_pipeline)) {
            // One real native1 app per selected logical sample. Original sample
            // locations/coverage/interpolation remain in the captured records.
            const uint32_t application_samples = promoted && draw.sample_pixel_pipeline ? draw.sample_count : 1;
            for (uint32_t sample = 0; sample < application_samples; ++sample) {
              tile.sample_base = sample;
              auto encoder = cmdbuf.renderCommandEncoder(pixel_info);
              encoder.waitForFence(fence_, WMTRenderStageFragment);
              if (!bind_tile(encoder, tile, 1, draw.sample_pixel_pipeline ? 5 : 3, false)) { encoder.endEncoding(); return false; }
              if (draw.rov_marker_sample_count) bind_rov_markers(encoder, 1);
              if (promoted) {
                // The unclipped triangle keeps the virtual tile at (4,4) interior
                // to all raster edges. Do not introduce another native clip edge.
                wmtcmd_render_setscissorrect rect{};
                rect.type = WMTRenderCommandSetScissorRect;
                rect.scissor_rect = {0, 0, 32, 32};
                encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&rect));
              } else if (draw.pixel_layout.flags & SM50_MSAA_PIXEL_COARSE) {
                // The virtual coarse grid occupies the tile's top-left corner; the coverage texture already
                // holds the application scissor per fine pixel.
                wmtcmd_render_setscissorrect rect{};
                rect.type = WMTRenderCommandSetScissorRect;
                rect.scissor_rect = {x, y, std::min(8u, render_width - x), std::min(8u, render_height - y)};
                encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&rect));
              } else {
                scissor(encoder, bounds, x, y, true);
              }
              no_cull(encoder);
              encoder.setDepthStencilState(draw.no_depth_stencil);
              encoder.setRenderPipelineState(draw.sample_pixel_pipeline ? WMT::RenderPipelineState(draw.sample_pixel_pipeline)
                                                                        : WMT::RenderPipelineState(draw.pixel_pipeline));
              encoder.setViewport({0, 0, double(pixel_info.render_target_width), double(pixel_info.render_target_height), 0, 1});
              encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
              encoder.updateFence(fence_, WMTRenderStageFragment);
              encoder.endEncoding();
            }
            tile.sample_base = 0;
          }
          if (draw.single_sample_replay && draw.query_coverage_pipeline &&
              !render.visibility_targets.empty()) {
            // Count the saved post-PS visibility at native raster frequency.
            // The 1x color replay below never contributes to the query.
            WMTRenderPassInfo query_info{};
            WMT::InitializeRenderPassInfo(query_info);
            query_info.default_raster_sample_count = native_samples;
            query_info.center_sample_pattern = draw.center_sample_pattern;
            query_info.sample_positions = draw.sample_positions;
            query_info.render_target_width = render_width;
            query_info.render_target_height = render_height;
            query_info.visibility_buffer = render.visibility_scratch;
            for (uint32_t half = 0; half < halves; ++half) {
              tile.sample_base = half * 8;
              query_info.forced16_half = split16 ? half + 1 : 0;
              auto encoder = cmdbuf.renderCommandEncoder(query_info);
              encoder.waitForFence(fence_, WMTRenderStageFragment);
              if (!bind_tile(encoder, tile, 1, 4, true)) { encoder.endEncoding(); return false; }
              scissor(encoder, bounds, x, y, true);
              no_cull(encoder);
              encoder.setDepthStencilState(draw.no_depth_stencil);
              encoder.setRenderPipelineState(draw.query_coverage_pipeline);
              encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
              encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
              encoder.updateFence(fence_, WMTRenderStageFragment);
              encoder.endEncoding();
              // Read this pass's count before the next query contributor resets it.
              fanout_visibility();
            }
            tile.sample_base = 0;
          }
          WMTRenderPassInfo replay_info{};
          WMT::InitializeRenderPassInfo(replay_info);
          replay_info.render_target_width = render_width;
          replay_info.render_target_height = render_height;
          WMT::SetRenderTargetArrayLength(replay_info, render.render_target_array_length);
          // Forced-pattern replay renders into the RTVs at their own sample count (taken from the attachments).
          replay_info.default_raster_sample_count = draw.single_sample_replay
              ? 0 : render.default_raster_sample_count;
          replay_info.center_sample_pattern = draw.center_sample_pattern && !draw.single_sample_replay;
          if (!draw.single_sample_replay) replay_info.sample_positions = draw.sample_positions;
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
            // Replay at the forced raster rate (UAV-only 16x) runs per 8x half; a fullscreen RTV-rate replay
            // already covers every output sample once.
            const uint32_t replay_halves = draw.single_sample_replay ? 1u : halves;
            for (uint32_t half = 0; half < replay_halves; ++half) {
            tile.sample_base = half * 8;
            replay_info.forced16_half = split16 && !draw.single_sample_replay ? half + 1 : 0;
            if (replay_info.forced16_half) replay_info.default_raster_sample_count = 8;
            auto encoder = cmdbuf.renderCommandEncoder(replay_info);
            encoder.waitForFence(fence_, WMTRenderStageVertex);
            const uint32_t fragment = draw.sample_pixel_pipeline && !promoted ? 5 : 4;
            if (!bind_tile(encoder, tile, 1, fragment, !early_depth_executed && !draw.single_sample_replay)) {
              encoder.endEncoding();
              return false;
            }
            if (draw.sample_pixel_pipeline && !promoted && draw.rov_marker_sample_count)
              bind_rov_markers(encoder, draw.rov_marker_sample_count);
            scissor(encoder, bounds, x, y, draw.single_sample_replay);
            encoder.setDepthStencilState(early_depth_executed ? WMT::DepthStencilState(draw.no_depth_stencil)
                                               : WMT::DepthStencilState(draw.original_depth_stencil));
            if (!early_depth_executed) set_depth_bounds(encoder);
            encoder.setRenderPipelineState(draw.sample_pixel_pipeline && !promoted ? WMT::RenderPipelineState(draw.sample_pixel_pipeline)
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
            // Each pass writes its own visibility count; fan out every split half before the next overwrites it.
            if (split16 && half + 1 < replay_halves && !early_depth_executed && !draw.single_sample_replay)
              fanout_visibility();
            }
            tile.sample_base = 0;
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
  EncodeMSAAIndirectArguments(WMT::MTL4CommandBuffer cmdbuf, const SOIndirectEncoderData &indirect,
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
  EncodeSOIndirectPreflight(WMT::MTL4CommandBuffer cmdbuf, WMT::ComputePipelineState preflight_pipeline,
                            SOIndirectPreflightParams params_data, uint32_t record_capacity,
                            WMT::Buffer predicate,
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
    wmtcmd_compute_useresource predicate_use{};
    predicate_use.type = WMTComputeCommandUseResource;
    predicate_use.resource = predicate;
    predicate_use.usage = WMTResourceUsageRead;
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = preflight_pipeline;
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
    head.next.set(predicate ? reinterpret_cast<wmtcmd_base *>(&predicate_use) : reinterpret_cast<wmtcmd_base *>(&pso));
    predicate_use.next.set(&pso);
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
        params->cut_enabled = (params->topology == 2 || params->topology == 4 || params->topology == 5) &&
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

  /* One execution of a capture-sized ExecuteIndirect (MeshCaptureIndirectData): per active command, its captures,
   * sized from its GPU-written arguments exactly as a direct draw of the same arguments. */
  struct MeshCaptureCommand {
    WMT::Reference<WMT::Buffer> scratch, list; // TS+GS DS capture and flat primitive list
    uint64_t slots = 0;
    uint64_t args_offset = 0, capture_offset = 0, compact_offset = 0, dispatch_offset = 0;
    uint32_t patches = 0, instances = 0, vertex_count = 0;
    uint32_t slot = 0; // the command's index among the signature's commands
    Rc<BufferAllocation> index_owner;
    WMT::Buffer index_buffer;
    uint64_t index_offset = 0;
    uint32_t index_format = 0;
    // Strip expansion (MeshCaptureIndirectData::strip_expand): the command's list and its control/arguments
    // (control at strip_control_offset of the execution storage, then 64 bytes of draw + object-grid arguments).
    WMT::Reference<WMT::Buffer> strip_list;
    uint64_t strip_control_offset = 0;
    uint64_t strip_first_byte = 0; // byte offset of the command's first index in the index buffer
    std::shared_ptr<StreamOutputDrawData> so;
  };
  struct MeshCaptureExecution {
    std::shared_ptr<const MeshCaptureIndirectData> data;
    WMT::Reference<WMT::Buffer> storage;
    std::vector<MeshCaptureCommand> commands;
    std::vector<std::shared_ptr<StreamOutputDrawData>> so_draws;
  };

  /* IA primitives in `count` vertices of a D3D_PRIMITIVE_TOPOLOGY (list topologies, non-cut strips). */
  static uint64_t MeshCaptureInputPrimitives(uint32_t topology, uint32_t count) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return count;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return count / 2;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return count > 1 ? count - 1 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return count / 3;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return count > 2 ? count - 2 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return count / 4;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return count > 3 ? count - 3 : 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return count / 6;
    default: return count > 5 ? (count - 4) / 2 : 0;
    }
  }

  bool
  PrepareMeshCaptureExecution(const std::shared_ptr<const MeshCaptureIndirectData> &data,
                              const SOIndirectPreflightResult &result, InflightCommandBuffer &inflight,
                              MeshCaptureExecution &execution) {
    const uint32_t count = result.effective_count;
    if (count > data->preflight.max_count)
      return false;
    execution.data = data;
    if (!count)
      return true;
    auto metal = device_->GetMTLDevice();
    const uint64_t max_length = metal.maxBufferLength();
    const auto *records = reinterpret_cast<const SOIndirectPreflightRecord *>(&result + 1);
    const auto &layout = data->layout;
    const uint64_t so_stream_bytes = sizeof(StreamOutputAssemblyParams) + sizeof(SM50_SO_CAPTURE_CONTROL) + 16;
    const uint64_t strip_bytes =
        data->strip_expand ? sizeof(MeshCaptureIndirectData::StripCutControl) + 64 : 0;
    const uint64_t command_bytes = (32 + sizeof(SM50_TESS_CAPTURE_CONTROL) + 16 + 16 +
                                    uint64_t(data->so_params.size()) * so_stream_bytes + strip_bytes + 15) & ~uint64_t(15);
    WMTBufferInfo info{};
    info.length = uint64_t(count) * command_bytes;
    info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    if (info.length > max_length)
      return false;
    execution.storage = metal.newBuffer(info);
    auto *memory = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
    if (!execution.storage || !memory)
      return false;
    memset(memory, 0, info.length);
    inflight.so_indirect_buffers.push_back(execution.storage);
    execution.commands.reserve(count);
    auto private_buffer = [&](uint64_t length) {
      WMTBufferInfo buffer{};
      buffer.length = std::max<uint64_t>(length, 16);
      buffer.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      return metal.newBuffer(buffer);
    };
    for (uint32_t i = 0; i < count; i++) {
      const auto &record = records[i];
      MeshCaptureCommand command;
      command.slot = i;
      command.instances = record.instance_count;
      command.vertex_count = record.vertex_count;
      // Primitive ids per draw instance that bound the GS invocation keys (the direct draw's bound).
      uint64_t primitives;
      if (data->geometry) {
        // A strip expansion bounds its primitive ids by the vertex count, as RecordMeshStripCut does.
        primitives = data->strip_expand ? record.vertex_count
                                        : MeshCaptureInputPrimitives(data->topology, record.vertex_count);
        if (!record.vertex_count || !record.instance_count)
          continue; // a direct draw of these arguments draws, captures and counts nothing
      } else {
        command.patches = record.vertex_count / data->control_points;
        if (!command.patches || !record.instance_count)
          continue;
        command.slots = uint64_t(record.instance_count) * command.patches *
            (data->domain_only && !data->so_template ? data->msaa_layout.max_workloads : layout.max_workloads);
        primitives = data->domain_only ? command.slots : command.slots * layout.max_primitives;
      }
      const uint64_t instances = data->geometry ? record.instance_count : 1;
      const uint64_t keys = primitives * instances * data->gs_instances;
      if (primitives > UINT32_MAX || (data->so_template && keys > UINT32_MAX)) {
        ERR("ExecuteIndirect: command of ", record.vertex_count, " vertices x ", record.instance_count,
            " instances exceeds the capture limit");
        return false;
      }
      const uint64_t base = uint64_t(i) * command_bytes;
      // The preflight command buffer has retired: the GPU-resolved binding
      // tables are now readable. Keep their allocations resident through the
      // final consumer command buffer, not only while preparing its draws.
      auto retain_binding = [&](uint64_t address) {
        uint64_t offset = 0;
        if (auto *owner = device_->LookupBufferByVA(address, &offset)) {
          inflight.mesh_indirect_owners.emplace_back(owner);
          inflight.so_indirect_buffers.push_back(owner->buffer());
        }
      };
      if (data->root_stride) {
        const auto *roots = reinterpret_cast<const uint64_t *>(
            data->root_memory + uint64_t(i) * data->root_stride);
        for (uint32_t offset : data->root_view_offsets)
          retain_binding(roots[offset]);
      }
      if (data->vertex_stride) {
        const auto *vertices = reinterpret_cast<const uint64_t *>(
            data->vertex_memory + uint64_t(i) * data->vertex_stride);
        for (uint32_t slots = data->vertex_mask; slots; slots &= slots - 1)
          retain_binding(vertices[2 * __builtin_ctz(slots)]);
      }
      if (data->indexed) {
        const uint64_t index_size = record.index_format == DXGI_FORMAT_R32_UINT ? 4 : 2;
        uint64_t offset = 0;
        auto *owner = device_->LookupBufferByVA(record.index_address, &offset);
        const uint64_t bytes = (uint64_t(record.start_location) + record.vertex_count) * index_size;
        if (!owner || offset > owner->byteLength() || bytes > owner->byteLength() - offset)
          return false;
        command.index_owner = owner;
        inflight.mesh_indirect_owners.emplace_back(owner);
        command.index_buffer = owner->buffer();
        command.index_offset = offset;
        command.index_format = record.index_format;
        inflight.so_indirect_buffers.push_back(command.index_buffer);
      }
      command.args_offset = base;
      command.capture_offset = base + 32;
      command.compact_offset = command.capture_offset + sizeof(SM50_TESS_CAPTURE_CONTROL);
      command.dispatch_offset = command.compact_offset + 16;
      auto *arguments = reinterpret_cast<uint32_t *>(memory + command.args_offset);
      arguments[0] = record.vertex_count;
      arguments[1] = record.instance_count;
      arguments[2] = record.start_location;
      if (data->indexed) {
        arguments[3] = uint32_t(record.base_vertex);
        arguments[4] = record.start_instance;
      } else {
        arguments[3] = record.start_instance;
      }
      if (data->strip_expand) {
        /* RecordMeshStripCut for these arguments: the list (vpp 32-bit values per primitive, at most one primitive
         * per strip vertex) and the expansion's control; the GS then draws the list as an indexed list draw. */
        const uint64_t list_values = uint64_t(record.vertex_count) * data->strip_vertices_per_primitive;
        if (list_values > UINT32_MAX || list_values * 4 > max_length)
          return false;
        command.strip_list = private_buffer(list_values * 4);
        if (!command.strip_list)
          return false;
        inflight.so_indirect_buffers.push_back(command.strip_list);
        command.strip_control_offset = base + command_bytes - strip_bytes;
        auto *control = reinterpret_cast<MeshCaptureIndirectData::StripCutControl *>(memory + command.strip_control_offset);
        const uint32_t index_size = !data->indexed ? 0 : record.index_format == DXGI_FORMAT_R32_UINT ? 4 : 2;
        control->max_threadgroups = data->max_threadgroups;
        control->topology = data->topology;
        control->index_size = index_size;
        control->cut_value = index_size == 4 ? 0xffffffffu : 0xffffu;
        control->cut_enabled = data->indexed &&
            ((index_size == 2 && data->strip_cut_value == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
             (index_size == 4 && data->strip_cut_value == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF));
        control->count = record.vertex_count;
        control->instances = record.instance_count;
        // Non-indexed: the list holds IA positions and StartVertexLocation becomes the base vertex.
        control->base_vertex = data->indexed ? record.base_vertex : int32_t(record.start_location);
        control->start_instance = record.start_instance;
        command.strip_first_byte = data->indexed ? command.index_offset + uint64_t(record.start_location) * index_size : 0;
        control->index_skip = uint32_t(command.strip_first_byte & 3);
        control->statistics_enabled = data->statistics_buffer ? 1 : 0;
        control->per_group = data->warp_increment;
      }
      if (!data->geometry && !data->domain_only) {
        const uint64_t slots = command.slots;
        const uint64_t vertex_base = (slots + 3) & ~uint64_t(3);
        const uint64_t index_base = vertex_base + slots * layout.max_vertices * layout.vertex_registers * 4;
        const uint64_t scratch_bytes = 4 * (index_base + slots * layout.max_primitives * layout.primitive_vertices);
        const uint64_t list_bytes = 4 * (4 + slots * layout.max_primitives);
        // The same limits a direct draw of these arguments is recorded under (RecordTessellationGeometryDraw).
        if (scratch_bytes > max_length || list_bytes > max_length) {
          ERR("GS after tessellation: indirect command of ", command.patches, " patches x ", record.instance_count,
              " instances exceeds the capture limit");
          return false;
        }
        auto *capture = reinterpret_cast<SM50_TESS_CAPTURE_CONTROL *>(memory + command.capture_offset);
        capture->slot_count = uint32_t(slots);
        capture->patch_count = command.patches;
        auto *compact = reinterpret_cast<uint32_t *>(memory + command.compact_offset);
        compact[0] = uint32_t(slots);
        compact[1] = layout.max_primitives;
        compact[2] = data->per_group;
        command.scratch = private_buffer(scratch_bytes);
        command.list = private_buffer(list_bytes);
        if (!command.scratch || !command.list)
          return false;
        inflight.so_indirect_buffers.push_back(command.scratch);
        inflight.so_indirect_buffers.push_back(command.list);
      }
      if (data->so_template) {
        // One capture scratch shared by every chained stream, each at keys * scratch_prefix_dwords dwords.
        uint64_t capture_dwords = 0;
        for (const auto &params : data->so_params)
          capture_dwords += uint64_t(params.vertex_count) * (uint64_t(params.record_dwords) + 1) + 1;
        if (keys * capture_dwords * 4 > max_length)
          return false;
        auto so_scratch = private_buffer(keys * capture_dwords * 4);
        if (!so_scratch)
          return false;
        std::shared_ptr<StreamOutputDrawData> head, *tail = &head;
        uint64_t at = command.dispatch_offset + 16;
        size_t s = 0;
        for (auto *original = data->so_template.get(); original; original = original->next_stream.get(), s++) {
          const auto &params = data->so_params[s];
          auto draw = std::make_shared<StreamOutputDrawData>(*original);
          draw->next_stream.reset();
          draw->scratch = so_scratch;
          draw->heap = execution.storage;
          draw->params_offset = at;
          draw->control_offset = at + sizeof(StreamOutputAssemblyParams);
          draw->status_offset = draw->control_offset + sizeof(SM50_SO_CAPTURE_CONTROL);
          draw->status_cpu = reinterpret_cast<const uint32_t *>(memory + draw->status_offset);
          draw->scratch_offset = keys * original->scratch_prefix_dwords * 4;
          draw->claims_offset =
              draw->scratch_offset + keys * params.vertex_count * (uint64_t(params.record_dwords) + 1) * 4;
          draw->claims_bytes = keys * 4;
          *reinterpret_cast<StreamOutputAssemblyParams *>(memory + draw->params_offset) = params;
          auto *control = reinterpret_cast<SM50_SO_CAPTURE_CONTROL *>(memory + draw->control_offset);
          control->positions_per_instance = uint32_t(data->domain_only
              ? uint64_t(command.patches) * layout.max_workloads : primitives);
          control->record_count = uint32_t(keys);
          at += so_stream_bytes;
          *tail = std::move(draw);
          tail = &(*tail)->next_stream;
        }
        for (auto *draw = head.get(); draw; draw = draw->next_stream.get())
          execution.so_draws.push_back(std::shared_ptr<StreamOutputDrawData>(head, draw));
        command.so = std::move(head);
      }
      execution.commands.push_back(std::move(command));
    }
    // Reserve before setup is encoded (EncodeStreamOutputSetup retains every draw until completion).
    inflight.so_draws.reserve(inflight.so_draws.size() + execution.so_draws.size());
    return true;
  }

  /* GS strip expansion (MeshCaptureIndirectData::strip_expand): per command, dxmt_mesh_strip_cut writes the list,
   * its draw arguments and object grid, exactly as RecordMeshStripCut does for a direct draw of the arguments. */
  void
  EncodeStripExpansion(WMT::MTL4CommandBuffer cmdbuf, const MeshCaptureExecution &execution) {
    const auto &data = *execution.data;
    if (execution.commands.empty() || !data.strip_expand)
      return;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    for (const auto &command : execution.commands) {
      wmtcmd_compute_setpso pso{};
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = data.strip_cut_pipeline;
      pso.threadgroup_size = {1, 1, 1};
      std::array<wmtcmd_compute_setbuffer, 6> buffers{};
      // The kernel reads indices from `index_skip` bytes past this 4-aligned address; predicate: never enabled.
      const std::pair<WMT::Buffer, uint64_t> bound[] = {
          {execution.storage, command.strip_control_offset},
          {data.indexed ? command.index_buffer : WMT::Buffer(execution.storage), command.strip_first_byte & ~uint64_t(3)},
          {command.strip_list, 0},
          {execution.storage, command.strip_control_offset + sizeof(MeshCaptureIndirectData::StripCutControl)},
          {data.statistics_buffer ? data.statistics_buffer : WMT::Buffer(execution.storage),
           data.statistics_buffer ? data.statistics_offset : 0},
          {execution.storage, command.strip_control_offset}};
      for (uint8_t i = 0; i < 6; i++) {
        buffers[i].type = WMTComputeCommandSetBuffer;
        buffers[i].buffer = bound[i].first;
        buffers[i].offset = bound[i].second;
        buffers[i].index = i;
      }
      wmtcmd_compute_dispatch dispatch{};
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {1, 1, 1};
      pso.next.set(&buffers[0]);
      for (size_t i = 0; i + 1 < buffers.size(); i++)
        buffers[i].next.set(&buffers[i + 1]);
      buffers.back().next.set(&dispatch);
      dispatch.next.set(nullptr);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
    }
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

  /* Pass 1 setup: zero every command's per-slot primitive counts (slots without a workload keep 0). */
  void
  EncodeTessGeometryClear(WMT::MTL4CommandBuffer cmdbuf, const MeshCaptureExecution &execution) {
    if (execution.commands.empty() || execution.data->geometry || execution.data->domain_only)
      return;
    auto blit = cmdbuf.blitCommandEncoder();
    blit.waitForFence(fence_);
    for (const auto &command : execution.commands) {
      wmtcmd_blit_fillbuffer fill{};
      fill.type = WMTBlitCommandFillBuffer;
      fill.next.set(nullptr);
      fill.buffer = command.scratch;
      fill.offset = 0;
      fill.length = command.slots * 4;
      fill.value = 0;
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
    }
    blit.updateFence(fence_);
    blit.endEncoding();
  }

  /* Appends the per-command draws after the recorded pipeline state of the pass, in API order: GS (one pass),
   * or TS+GS pass 1 (VS+HS / DS capture) / pass 2 (capture fetch / GS). */
  void
  EncodeMeshCaptureDraws(WMT::MTL4RenderCommandEncoder encoder, const MeshCaptureExecution &execution, uint8_t pass,
                        uint32_t selected = UINT32_MAX) {
    const auto &data = *execution.data;
    const WMTRenderStages object_mesh = WMTRenderStages(WMTRenderStageObject | WMTRenderStageMesh);
    if (!execution.commands.empty())
      encoder.useResource(execution.storage, WMTResourceUsageRead, object_mesh);
    const uint32_t patch_per_group = data.geometry ? 0 : 32 / data.threads_per_patch;
    if (data.statistics_claim && !execution.commands.empty())
      encoder.useResource(data.statistics_claim, WMTResourceUsageRead | WMTResourceUsageWrite, WMTRenderStageFragment);
    for (const auto &command : execution.commands) {
      if (selected != UINT32_MAX && command.slot != selected) continue;
      if (data.root_stride) {
        const uint64_t offset = data.root_offset + uint64_t(command.slot) * data.root_stride;
        encoder.useResource(data.root_heap, WMTResourceUsageRead, object_mesh | WMTRenderStageFragment);
        encoder.setObjectBuffer(data.root_heap, offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
        encoder.setMeshBuffer(data.root_heap, offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
        encoder.setFragmentBuffer(data.root_heap, offset, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
      }
      if (data.vertex_stride) {
        encoder.useResource(data.vertex_heap, WMTResourceUsageRead, WMTRenderStageObject);
        encoder.setObjectBuffer(data.vertex_heap, data.vertex_offset + uint64_t(command.slot) * data.vertex_stride,
                                SM50_BINDING_INDEX_VERTEX_BUFFER);
      }
      if (data.update_index && !data.strip_expand && pass == 1) {
        const auto &variant = data.index_variants[command.index_format == DXGI_FORMAT_R32_UINT];
        wmtcmd_render_setpso pipeline{};
        pipeline.type = WMTRenderCommandSetPSO;
        pipeline.pso = variant.pipeline;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&pipeline));
        if (variant.object_constants) {
          encoder.useResource(variant.object_constants, WMTResourceUsageRead, WMTRenderStageObject);
          encoder.setObjectBuffer(variant.object_constants, variant.object_offset, SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
        }
        if (variant.mesh_constants) {
          encoder.useResource(variant.mesh_constants, WMTResourceUsageRead, WMTRenderStageMesh);
          encoder.setMeshBuffer(variant.mesh_constants, variant.mesh_offset, SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
        }
      }
      if (data.statistics_claim && (data.geometry || pass == 2))
        encoder.setFragmentBuffer(data.statistics_claim,
                                  data.statistics_claim_offset + uint64_t(command.slot) * sizeof(SM50_STATISTICS_CLAIM_HEADER),
                                  SM50_BINDING_INDEX_STATISTICS_CLAIM);
      if (data.geometry) {
        if (command.so) {
          encoder.useResource(command.so->scratch, WMTResourceUsageWrite, WMTRenderStageMesh);
          encoder.setMeshBuffer(execution.storage, command.so->control_offset, SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL);
          encoder.setMeshBuffer(command.so->scratch, 0, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
        }
        encoder.setObjectBuffer(execution.storage, command.args_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
        if (data.strip_expand) {
          // The expanded list: an indexed list draw over 32-bit values with the strip's base vertex.
          const uint64_t arguments = command.strip_control_offset + sizeof(MeshCaptureIndirectData::StripCutControl);
          encoder.useResource(command.strip_list, WMTResourceUsageRead, WMTRenderStageObject);
          wmtcmd_render_dxmt_geometry_draw_indexed_indirect draw{};
          draw.type = WMTRenderCommandDXMTGeometryDrawIndexedIndirect;
          draw.next.set(nullptr);
          draw.index_buffer = command.strip_list;
          draw.index_buffer_offset = 0;
          draw.imm_draw_arguments = execution.storage;
          draw.indirect_args_buffer = execution.storage;
          draw.indirect_args_offset = arguments;
          draw.dispatch_args_buffer = execution.storage;
          draw.dispatch_args_offset = arguments + 8 * sizeof(uint32_t);
          draw.vertex_per_warp = data.vertex_per_warp;
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
          continue;
        }
        const uint32_t warps = (command.vertex_count - 1) / data.warp_increment + 1;
        if (data.indexed) {
          wmtcmd_render_dxmt_geometry_draw_indexed draw{};
          draw.type = WMTRenderCommandDXMTGeometryDrawIndexed;
          draw.next.set(nullptr);
          draw.draw_arguments_offset = command.args_offset;
          draw.index_buffer = command.index_buffer;
          draw.index_buffer_offset = command.index_offset;
          draw.warp_count = warps;
          draw.instance_count = command.instances;
          draw.vertex_per_warp = data.vertex_per_warp;
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
        } else {
          wmtcmd_render_dxmt_geometry_draw draw{};
          draw.type = WMTRenderCommandDXMTGeometryDraw;
          draw.next.set(nullptr);
          draw.draw_arguments_offset = command.args_offset;
          draw.warp_count = warps;
          draw.instance_count = command.instances;
          draw.vertex_per_warp = data.vertex_per_warp;
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
        }
        continue;
      }
      if (pass == 1) {
        if (data.domain_only && command.so) {
          encoder.useResource(command.so->scratch, WMTResourceUsageRead | WMTResourceUsageWrite, WMTRenderStageMesh);
          encoder.setMeshBuffer(execution.storage, command.so->control_offset, SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL);
          encoder.setMeshBuffer(command.so->scratch, 0, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
        }
        if (!data.domain_only) {
        encoder.useResource(command.scratch, WMTResourceUsageWrite, WMTRenderStageMesh);
        encoder.setMeshBuffer(command.scratch, 0, SM50_BINDING_INDEX_TESS_CAPTURE);
        encoder.setMeshBuffer(execution.storage, command.capture_offset, SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL);
        }
        encoder.setObjectBuffer(execution.storage, command.args_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
        const uint32_t groups = (command.patches - 1) / patch_per_group + 1;
        if (data.indexed) {
          wmtcmd_render_dxmt_tessellation_mesh_draw_indexed draw{};
          draw.type = WMTRenderCommandDXMTTessellationMeshDrawIndexed;
          draw.next.set(nullptr);
          draw.draw_arguments_offset = command.args_offset;
          draw.index_buffer = command.index_buffer;
          draw.index_buffer_offset = command.index_offset;
          draw.instance_count = command.instances;
          draw.threads_per_patch = data.threads_per_patch;
          draw.patch_per_group = patch_per_group;
          draw.patch_per_mesh_instance = groups;
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
        } else {
          wmtcmd_render_dxmt_tessellation_mesh_draw draw{};
          draw.type = WMTRenderCommandDXMTTessellationMeshDraw;
          draw.next.set(nullptr);
          draw.draw_arguments_offset = command.args_offset;
          draw.instance_count = command.instances;
          draw.threads_per_patch = data.threads_per_patch;
          draw.patch_per_group = patch_per_group;
          draw.patch_per_mesh_instance = groups;
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
        }
        continue;
      }
      encoder.useResource(command.scratch, WMTResourceUsageRead, WMTRenderStageObject);
      encoder.useResource(command.list, WMTResourceUsageRead, WMTRenderStageObject);
      encoder.setObjectBuffer(command.scratch, 0, SM50_BINDING_INDEX_TESS_CAPTURE);
      encoder.setObjectBuffer(command.list, 0, SM50_BINDING_INDEX_TESS_CAPTURE_LIST);
      encoder.setObjectBuffer(execution.storage, command.capture_offset, SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL);
      if (command.so) {
        encoder.useResource(command.so->scratch, WMTResourceUsageWrite, WMTRenderStageMesh);
        encoder.setMeshBuffer(execution.storage, command.so->control_offset, SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL);
        encoder.setMeshBuffer(command.so->scratch, 0, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
      }
      wmtcmd_render_draw_meshthreadgroups_indirect draw{};
      draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
      draw.next.set(nullptr);
      draw.indirect_args_buffer = execution.storage;
      draw.indirect_args_offset = command.dispatch_offset;
      draw.object_threadgroup_size = {32, 1, 1};
      draw.mesh_threadgroup_size = {1, 1, 1};
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
    }
  }

  /* Between the passes: per command, the flat primitive list in D3D order and its GS object grid. */
  void
  EncodeTessGeometryCompaction(WMT::MTL4CommandBuffer cmdbuf, const MeshCaptureExecution &execution) {
    if (execution.commands.empty() || execution.data->geometry)
      return;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    for (const auto &command : execution.commands) {
      wmtcmd_compute_setpso pso{};
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = execution.data->compact_pipeline;
      pso.threadgroup_size = {1024, 1, 1};
      std::array<wmtcmd_compute_setbuffer, 4> buffers{};
      const std::pair<WMT::Buffer, uint64_t> bound[] = {{execution.storage, command.compact_offset},
                                                        {command.scratch, 0},
                                                        {command.list, 0},
                                                        {execution.storage, command.dispatch_offset}};
      for (uint8_t i = 0; i < 4; i++) {
        buffers[i].type = WMTComputeCommandSetBuffer;
        buffers[i].buffer = bound[i].first;
        buffers[i].offset = bound[i].second;
        buffers[i].index = i;
      }
      wmtcmd_compute_dispatch dispatch{};
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {1, 1, 1};
      pso.next.set(&buffers[0]);
      for (size_t i = 0; i + 1 < buffers.size(); i++)
        buffers[i].next.set(&buffers[i + 1]);
      buffers.back().next.set(&dispatch);
      dispatch.next.set(nullptr);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
    }
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

  bool EncodeWritableMSAA(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                          Texture *texture, bool to_native, TextureAllocation *allocation = nullptr) {
    if (!allocation) allocation = texture->current();
    if (!allocation || !allocation->msaaShadow[0]) return false;
    inflight.cast_allocations.emplace_back(allocation);
    const auto format = allocation->msaaFormat;
    const bool integer = IsIntegerFormat(format);
    const uint32_t scalar = integer ? 1 : 0;
    const char *suffix = integer ? "uint" : "float";
    auto library = WMT4::Library(device_->GetLib().getLibrary());
    WMT::Error error;
    WMT::RenderPipelineState export_pipeline{};
    WMT::ComputePipelineState import_pipeline{};
    if (to_native) {
      const uint64_t key = uint64_t(format) | (uint64_t(texture->sampleCount()) << 32);
      auto &cached = writable_msaa_export_[key];
      if (!cached) {
        WMTRenderPipelineInfo info;
        WMT::InitializeRenderPipelineInfo(info);
        auto vertex = library.newFunction("dxmt_resolve_vertex");
        auto fragment = library.newFunction((std::string("dxmt_msaa_export_") + suffix).c_str());
        info.vertex_function = vertex.handle;
        info.fragment_function = fragment.handle;
        info.colors[0].pixel_format = format;
        info.raster_sample_count = texture->sampleCount();
        info.rasterization_enabled = true;
        info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
        cached = device_->GetMetal4Compiler().newRenderPipelineState(info, error);
        if (!cached) return false;
      }
      export_pipeline = cached;
    } else {
      const bool array = texture->arrayLength() > 1;
      auto &cached = writable_msaa_import_[scalar + (array ? 2 : 0)];
      if (!cached) {
        std::string name = array ? "dxmt_msaa_import_array_" : "dxmt_msaa_import_";
        name += suffix;
        cached = device_->GetMetal4Compiler().newComputePipelineState(library.newFunction(name.c_str()), error);
        if (!cached) return false;
      }
      import_pipeline = cached;
    }
    for (uint32_t array = 0; array < texture->arrayLength(); ++array) {
      for (uint32_t sample = 0; sample < texture->sampleCount(); ++sample) {
        const uint32_t indices[2] = {array, sample};
        if (to_native) {
          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          info.colors[0].texture = allocation->msaaCanonical;
          info.colors[0].slice = array;
          info.colors[0].load_action = WMTLoadActionLoad;
          info.colors[0].store_action = WMTStoreActionStore;
          info.default_raster_sample_count = texture->sampleCount();
          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(fence_, WMTRenderStageFragment);
          encoder.setRenderPipelineState(export_pipeline);
          encoder.setFragmentTexture(allocation->msaaShadow[sample], 0);
          encoder.setFragmentTexture(allocation->msaaBaseline[sample], 1);
          encoder.setFragmentBytes(indices, sizeof(indices), 0);
          encoder.setViewport({0, 0, double(texture->width()), double(texture->height()), 0, 1});
          encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
          encoder.updateFence(fence_, WMTRenderStageFragment);
          encoder.endEncoding();
        } else {
          wmtcmd_compute_setpso pso{};
          std::array<wmtcmd_compute_settexture, 3> bindings{};
          wmtcmd_compute_setbytes params{};
          wmtcmd_compute_dispatch dispatch{};
          pso.type = WMTComputeCommandSetPSO;
          pso.pso = import_pipeline;
          pso.threadgroup_size = {8, 8, 1};
          pso.next.set(&bindings[0]);
          const WMT::Texture handles[] = {allocation->msaaCanonical, allocation->msaaShadow[sample], allocation->msaaBaseline[sample]};
          for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].type = WMTComputeCommandSetTexture;
            bindings[i].texture = handles[i];
            bindings[i].index = i;
            bindings[i].next.set(i == 2 ? static_cast<void *>(&params) : static_cast<void *>(&bindings[i + 1]));
          }
          params.type = WMTComputeCommandSetBytes;
          params.bytes.set(const_cast<uint32_t *>(indices));
          params.length = sizeof(indices);
          params.index = 0;
          params.next.set(&dispatch);
          dispatch.type = WMTComputeCommandDispatchThreads;
          dispatch.size = {texture->width(), texture->height(), 1};
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(fence_);
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
          encoder.updateFence(fence_);
          encoder.endEncoding();
        }
      }
    }
    return true;
  }

  struct SwizzlePassResource {
    Com<MTLD3D12Resource, false> resource;
    bool writes;
    WMT::Reference<WMT::Buffer> baseline;
    TextureViewRef cast_view;
    Rc<TextureAllocation> cast_allocation;
    Texture *msaa_texture = nullptr;
    bool msaa_sync = false;
    bool cast_sync = false;
    std::array<WMT::Reference<WMT::Buffer>, 3> volume_buffers;
    WMT::Reference<WMT::Texture> volume_source;
    std::vector<uint32_t> volume_cast_tiles;
    std::vector<uint32_t> volume_cast_groups;
    struct VolumeBaseline { uint32_t tile; WMT::Reference<WMT::Buffer> bytes; };
    std::vector<VolumeBaseline> volume_baselines;
    struct CastRegion {
      uint32_t x, y, width, height, array;
      WMT::Reference<WMT::Buffer> baseline;
    };
    std::vector<CastRegion> cast_regions;
    Com<MTLD3D12Resource, false> descriptor_owner;
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

  void AppendStandardTexture(std::vector<SwizzlePassResource> &uses, Texture *texture, bool writes,
                             TextureViewKey key = {}, TextureAllocation *allocation = nullptr,
                             Com<MTLD3D12Resource, false> descriptor_owner = nullptr) {
    if (!texture) return;
    AppendStandardResource(uses, device_->LookupStandardSwizzleTexture(texture), writes);
    if (!allocation) allocation = texture->current();
    if (!allocation) return;
    auto &view = texture->view(key, allocation);
    const bool msaa = texture->isWritableMSAAView(key) && !view.castBlockSize;
    if (!view.castBlockSize && !msaa && !view.reservedVolumeRender && !texture->reservedVolumeLayoutAddress) return;
    bool msaa_sync = msaa;
    bool cast_sync = view.castBlockSize != 0;
    if (writes && view.castBlockSize && !view.castCompressed &&
        texture->reservedResource && texture->reservedResource->volumeLayout()) {
      for (auto &use:uses)
        if (use.resource && use.resource->texture.ptr()==texture) use.volume_source=view.castStorage;
    }
    for (auto &use : uses) {
      if (cast_sync && use.cast_sync && use.cast_view->castStorage == view.castStorage &&
          use.cast_view->key.array_start == key.array_start && use.cast_view->key.array_end == key.array_end) {
        use.writes |= writes;
        cast_sync = false;
      }
      if (msaa && use.msaa_sync && use.cast_allocation.ptr() == allocation) {
        use.writes |= writes;
        msaa_sync = false;
      }
      if (use.cast_view.ptr() == &view) {
        use.writes |= writes;
        if (descriptor_owner) use.descriptor_owner = std::move(descriptor_owner);
        return;
      }
    }
    SwizzlePassResource use{};
    use.writes = writes;
    use.cast_view = view;
    use.cast_allocation = allocation;
    use.descriptor_owner = std::move(descriptor_owner);
    if (texture->reservedVolumeLayoutAddress) {
      auto &reserved = *texture->reservedResource;
      std::lock_guard<dxmt::mutex> lock(reserved.mutex);
      use.volume_buffers[0] = reserved.volume_layout_buffer;
      use.volume_buffers[2] = reserved.status_buffer;
      if (view.reservedVolumeCastLayoutAddress)
        for (const auto &[mip, layout] : reserved.volume_cast_layouts)
          if (layout.address == view.reservedVolumeCastLayoutAddress) {
            use.volume_buffers[1] = layout.buffer;
            break;
          }
    }
    if (msaa) use.msaa_texture = texture;
    use.msaa_sync = msaa_sync;
    use.cast_sync = cast_sync;
    uses.push_back(std::move(use));
  }

  std::vector<SwizzlePassResource>
  CollectStandardSwizzleTables(const std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables) {
    std::vector<SwizzlePassResource> uses;
    if (tables) AppendStandardSwizzleTables(*tables, uses);
    return uses;
  }

  void AppendStandardSwizzleTables(std::span<const StandardSwizzleTableUse> tables,
                                  std::vector<SwizzlePassResource> &uses) {
    auto append_descriptor = [&](MTLD3D12DescriptorHeap *heap, uint64_t index) {
      const auto &descriptor = heap->GetDescriptor(index);
      const bool writable = descriptor.type == ShaderVisibleDescriptorType::UAVTexture;
      if (!writable && descriptor.type != ShaderVisibleDescriptorType::SRVTexture) return;
      auto *texture = writable ? descriptor.UAVTexture.texture : descriptor.SRVTexture.texture;
      if (!texture || !descriptor.owner_id) return;
      auto owner = device_->LookupDescriptorResource(texture, descriptor.owner_id);
      if (!owner) return;
      const auto view = writable ? descriptor.UAVTexture.view : descriptor.SRVTexture.view;
      AppendStandardTexture(uses, texture, writable, view, nullptr, std::move(owner));
    };
    for (const auto &table : tables) {
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
      if (table.parameter == ~0u) {
        // Only direct heaps need deduplication; ordinary table lists can be large.
        if (std::any_of(tables.data(), &table, [&](const auto &previous) {
              return previous.heap == table.heap && previous.parameter == UINT32_MAX &&
                  previous.base >= heap_start.ptr && (previous.base - heap_start.ptr) % 32 == 0 &&
                  (previous.base - heap_start.ptr) / 32 < heap_desc.NumDescriptors;
            }))
          continue;
        for (uint32_t i = 0; i < heap_desc.NumDescriptors; ++i) append_descriptor(table.heap, i);
        continue;
      }
      if (table.restrict_to_shader) {
        table.rootsig->ForEachUsedDescriptorRange(table.rootsig->GetTableRanges(table.parameter), table.shader_bindings,
            [&](uint64_t offset, uint64_t count, D3D12_DESCRIPTOR_RANGE_TYPE type) {
          if (type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV && type != D3D12_DESCRIPTOR_RANGE_TYPE_UAV) return;
          if (offset >= heap_desc.NumDescriptors - base_index) return;
          const uint64_t first = base_index + offset;
          count = std::min<uint64_t>(count, heap_desc.NumDescriptors - first);
          for (uint64_t i = 0; i < count; ++i) append_descriptor(table.heap, first + i);
        });
        continue;
      }
      for (const auto &range : table.rootsig->GetTableRanges(table.parameter)) {
        if (range.type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
            range.type != D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
          continue;
        if (range.offset >= heap_desc.NumDescriptors - base_index)
          continue;
        const uint64_t first = base_index + range.offset;
        const uint64_t count = std::min<uint64_t>(range.count, heap_desc.NumDescriptors - first);
        for (uint64_t i = 0; i < count; ++i) append_descriptor(table.heap, first + i);
      }
    }
  }

  bool EncodeStandardSwizzle(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                             MTLD3D12Resource *resource, bool to_canonical,
                             WMT::Buffer dirty_flags = {}, uint64_t dirty_offset = 0, bool compare = false,
                             WMT::Buffer baseline = {}, WMT::Texture volume_source = {},
                             bool volume_compressed = false) {
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
        import_pipeline = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction(packed422 ? "d3d12_swizzle_packed422_import" : "d3d12_swizzle_import"), error);
      if (!export_pipeline)
        export_pipeline = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction(packed422 ? "d3d12_swizzle_packed422_export" : "d3d12_swizzle_export"), error);
      if (compare && !dirty_pipeline)
        dirty_pipeline = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction(packed422 ? "d3d12_swizzle_packed422_dirty" : "d3d12_swizzle_dirty"), error);
      if (dirty_flags && !compare && !conditional_pipeline)
        conditional_pipeline = device_->GetMetal4Compiler().newComputePipelineState(WMT4::Library(library).newFunction(packed422 ? "d3d12_swizzle_packed422_export_if_dirty"
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
    auto shadow = volume_source ? volume_source : resource->texture->current()->texture();
    const auto *volume_layout=resource->reserved ? resource->reserved->volumeLayout() : nullptr;
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
              WMTOrigin origin{pixel_x, pixel_y, p.origin_z};
              WMTSize size{
                  std::min<uint64_t>(uint64_t(p.extent_x) * layout.block_width, mip_width - pixel_x),
                  std::min<uint64_t>(uint64_t(p.extent_y) * layout.block_height, mip_height - pixel_y),
                  p.extent_z};
              uint32_t native_level=mip,native_slice=array,native_pitch=row_pitch;
              uint32_t native_image_pitch=desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D ? slice_pitch : 0;
              if (volume_layout) {
                // Each logical 64 KiB 3D tile is one native 2D atlas page. Its
                // linear block bytes use the existing Morton GPU staging kernel.
                // Export the actual written UInt alias: a Metal fence alone does
                // not publish sparse heap writes through the BC texture alias.
                const uint32_t tile=p.first_tile+(tz*m.tiles_y+ty)*m.tiles_x+tx;
                const uint32_t per_slice=volume_layout->atlas_columns*volume_layout->atlas_rows;
                const uint32_t local=tile%per_slice;
                const uint32_t scale=volume_source && !volume_compressed ? layout.block_width : 1;
                origin={local%volume_layout->atlas_columns*volume_layout->page_w/scale,
                        local/volume_layout->atlas_columns*volume_layout->page_h/scale,0};
                size={volume_layout->page_w/scale,volume_layout->page_h/scale,1};
                native_level=0; native_slice=tile/per_slice;
                native_pitch=volume_layout->page_w/layout.block_width*layout.bytes_per_unit;
                native_image_pitch=0;
              }
              if (to_canonical) {
                wmtcmd_blit_copy_from_texture_to_buffer_withblitoption copy{};
                copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
                copy.src = shadow;
                copy.level = native_level;
                copy.slice = native_slice;
                copy.origin = origin;
                copy.size = size;
                copy.dst = scratch;
                copy.offset = 0;
                copy.bytes_per_row = native_pitch;
                copy.bytes_per_image = native_image_pitch;
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
                copy.bytes_per_row = native_pitch;
                copy.bytes_per_image = native_image_pitch;
                copy.size = size;
                copy.dst = shadow;
                copy.level = native_level;
                copy.slice = native_slice;
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

  bool EncodeBlockCast(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                       SwizzlePassResource &use, bool to_native,
                       SwizzlePassResource::CastRegion *region = nullptr) {
    auto &view = *use.cast_view;
    auto owner = view.allocation->descriptor;
    if (view.castSourceBlockWidth == 1 && owner->sampleCount() > 1) {
      if (to_native || !view.msaaSampleAddress) return false;
      const uint32_t width = owner->width(), height = owner->height();
      const uint32_t arrays = view.key.array_end - view.key.array_start;
      const uint32_t samples = owner->sampleCount();
      const uint32_t pitch = (width * view.castBlockSize + 255) & ~255u;
      const uint64_t image_pitch = uint64_t(pitch) * height;
      WMTBufferInfo info{};
      info.length = image_pitch * arrays * samples;
      info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      info.memory.set(nullptr);
      WMT::Reference<WMT::Buffer> scratch;
      if (inflight.swizzle_scratch_length >= info.length) scratch = inflight.swizzle_buffers.back();
      else {
        scratch = device_->GetMTLDevice().newBuffer(info);
        if (!scratch) return false;
        inflight.swizzle_buffers.push_back(scratch);
        inflight.swizzle_scratch_length = info.length;
      }
      const bool array = owner->arrayLength() > 1;
      auto &pipeline = depth_msaa_import_[array ? 1 : 0];
      if (!pipeline) {
        WMT::Error error;
        auto library = WMT4::Library(device_->GetLib().getLibrary());
        pipeline = device_->GetMetal4Compiler().newComputePipelineState(
            library.newFunction(array ? "dxmt_depth_msaa_import_array" : "dxmt_depth_msaa_import"), error);
        if (!pipeline) return false;
      }
      uint32_t layout[8] = {width, height, arrays, samples, pitch, uint32_t(image_pitch),
                            view.castBlockSize, uint32_t(view.key.array_start)};
      wmtcmd_compute_setpso pso{};
      wmtcmd_compute_settexture source{};
      wmtcmd_compute_setbuffer destination{};
      wmtcmd_compute_setbytes params{};
      wmtcmd_compute_dispatch dispatch{};
      pso.type = WMTComputeCommandSetPSO; pso.pso = pipeline;
      pso.threadgroup_size = {8, 8, 1}; pso.next.set(&source);
      source.type = WMTComputeCommandSetTexture; source.texture = view.allocation->texture();
      source.index = 0; source.next.set(&destination);
      destination.type = WMTComputeCommandSetBuffer; destination.buffer = scratch;
      destination.index = 0; destination.next.set(&params);
      params.type = WMTComputeCommandSetBytes; params.bytes.set(layout);
      params.length = sizeof(layout); params.index = 1; params.next.set(&dispatch);
      dispatch.type = WMTComputeCommandDispatchThreads; dispatch.size = {width, height, arrays * samples};
      auto compute = cmdbuf.computeCommandEncoder(false);
      compute.waitForFence(fence_);
      compute.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
      compute.updateFence(fence_); compute.endEncoding();
      auto blit = cmdbuf.blitCommandEncoder();
      blit.waitForFence(fence_);
      for (uint32_t sample = 0; sample < samples; ++sample) for (uint32_t slice = 0; slice < arrays; ++slice) {
        wmtcmd_blit_copy_from_buffer_to_texture_withblitoption copy{};
        copy.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        copy.src = scratch; copy.src_offset = image_pitch * (uint64_t(sample) * arrays + slice);
        copy.bytes_per_row = pitch; copy.size = {width, height, 1};
        copy.dst = view.msaaViews[sample]; copy.slice = slice;
        copy.options = WMTBlitOptionNone;
        blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      }
      blit.updateFence(fence_); blit.endEncoding();
      return true;
    }
    if (view.reservedVolumeAtlasCast) {
      auto resource = device_->LookupStandardSwizzleTexture(owner);
      if (!resource) {
        // Real reserved volumes have no dense canonical buffer. Bridge their
        // sparse parent and cast pages directly, just like other block casts.
        // Capture every source page before uploading any destination: logical
        // tiles (including those in other mips) may alias the same heap page.
        auto *reserved = owner->reservedResource;
        if (!reserved || !reserved->volumeLayout() || (to_native && view.castCompressed)) return false;
        if (!to_native) {
          reserved->CollectMappedTiles(~uint64_t(0), use.volume_cast_tiles);
          if (use.writes) {
            // Snapshot physical identity at queue encoding, never recording.
            std::map<std::pair<obj_handle_t, uint32_t>, uint32_t> groups;
            use.volume_cast_groups.resize(use.volume_cast_tiles.size());
            for (uint32_t i = 0; i < use.volume_cast_tiles.size(); ++i) {
              const auto target = reserved->CurrentTarget(use.volume_cast_tiles[i]);
              const auto entry = groups.try_emplace(std::pair{target.heap.handle, target.heap_tile}, i).first;
              use.volume_cast_groups[i] = entry->second;
            }
          }
        }
        if (use.volume_cast_tiles.empty()) return true;
        const auto &layout = *reserved->volumeLayout();
        const uint64_t length = uint64_t(use.volume_cast_tiles.size()) * kReservedTileBytes;
        auto metal = device_->GetMTLDevice();
        if (length > metal.maxBufferLength()) return false;
        WMT::Reference<WMT::Buffer> scratch;
        if ((!use.writes || to_native) && inflight.swizzle_scratch_length >= length) scratch = inflight.swizzle_buffers.back();
        else {
          WMTBufferInfo info{}; info.length = length;
          info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
          info.memory.set(nullptr);
          scratch = metal.newBuffer(info);
          if (!scratch) return false;
          if (use.writes && !to_native) inflight.swizzle_baselines.push_back(scratch);
          else { inflight.swizzle_buffers.push_back(scratch); inflight.swizzle_scratch_length = length; }
        }
        if (use.writes && !to_native) use.baseline = scratch;
        WMT::Reference<WMT::Buffer> merged;
        if (to_native) {
          if (!use.baseline || use.volume_cast_groups.size() != use.volume_cast_tiles.size()) return false;
          WMTBufferInfo info{}; info.length = length;
          info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
          info.memory.set(nullptr); merged = metal.newBuffer(info);
          if (!merged) return false;
          inflight.swizzle_baselines.push_back(merged);
          if (!cast_merge_pipeline_) {
            WMT::Error error; auto library = device_->GetLib().getLibrary();
            cast_merge_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(
                WMT4::Library(library).newFunction("d3d12_cast_merge"), error);
            if (!cast_merge_pipeline_) return false;
          }
        }
        const uint32_t scale = view.castCompressed ? 1 : layout.block_w;
        const uint32_t pitch = layout.page_w / layout.block_w * view.castBlockSize;
        const auto parent = view.allocation->texture();
        for (uint32_t phase = 0; phase < (to_native ? 3u : 2u); ++phase) {
          const bool upload = phase == (to_native ? 2u : 1u);
          auto blit = cmdbuf.blitCommandEncoder();
          blit.waitForFence(fence_);
          for (size_t i = 0; i < use.volume_cast_tiles.size(); ++i) {
            if (to_native && phase == 1) {
              if (use.volume_cast_groups[i] != i) continue;
              // The UInt atlas shares these heap pages. Its physical layout
              // has already replaced the BC layout, even for untouched bytes.
              // Restore the pre-import BC bytes, then overlay writer deltas.
              wmtcmd_blit_copy_from_buffer_to_buffer copy{};
              copy.type = WMTBlitCommandCopyFromBufferToBuffer;
              copy.src = use.baseline; copy.dst = merged;
              copy.src_offset = copy.dst_offset = uint64_t(i) * kReservedTileBytes;
              copy.copy_length = kReservedTileBytes;
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
              continue;
            }
            const uint32_t tile = use.volume_cast_tiles[i];
            const uint32_t slice = tile / (layout.atlas_columns * layout.atlas_rows);
            WMTOrigin origin{(tile % layout.atlas_columns) * layout.page_w,
                ((tile / layout.atlas_columns) % layout.atlas_rows) * layout.page_h, 0};
            WMTSize size{layout.page_w, layout.page_h, 1};
            const bool cast_side = upload ? !to_native : to_native && phase == 0;
            if (cast_side) { origin.x /= scale; origin.y /= scale; size.width /= scale; size.height /= scale; }
            if (upload) {
              wmtcmd_blit_copy_from_buffer_to_texture copy{};
              copy.type = WMTBlitCommandCopyFromBufferToTexture;
              copy.src = to_native ? WMT::Buffer(merged) : WMT::Buffer(scratch);
              copy.src_offset = uint64_t(to_native ? use.volume_cast_groups[i] : i) * kReservedTileBytes;
              copy.dst = cast_side ? WMT::Texture(view.castStorage) : parent;
              copy.origin = origin; copy.size = size; copy.slice = slice; copy.level = 0;
              copy.bytes_per_row = pitch; copy.bytes_per_image = 0;
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            } else {
              wmtcmd_blit_copy_from_texture_to_buffer copy{};
              copy.type = WMTBlitCommandCopyFromTextureToBuffer;
              copy.src = cast_side ? WMT::Texture(view.castStorage) : parent;
              copy.origin = origin; copy.size = size; copy.slice = slice; copy.level = 0;
              copy.dst = scratch;
              copy.offset = uint64_t(i) * kReservedTileBytes;
              copy.bytes_per_row = pitch; copy.bytes_per_image = 0;
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            }
          }
          blit.updateFence(fence_); blit.endEncoding();
          if (to_native && phase == 1) {
            // An unchanged sibling alias must not overwrite the actual writer.
            // Merge changed bytes into one pre-import physical page before
            // uploading the same result to every logical alias of that page.
            uint32_t merge_layout[4] = {kReservedTileBytes, kReservedTileBytes, 1, 1};
            for (size_t i = 0; i < use.volume_cast_tiles.size(); ++i) {
              wmtcmd_compute_setpso pso{};
              wmtcmd_compute_setbuffer source{}, baseline{}, destination{};
              wmtcmd_compute_setbytes params{};
              wmtcmd_compute_dispatch dispatch{};
              pso.type = WMTComputeCommandSetPSO; pso.pso = cast_merge_pipeline_;
              pso.threadgroup_size = {64, 1, 1}; pso.next.set(&source);
              source.type = baseline.type = destination.type = WMTComputeCommandSetBuffer;
              source.buffer = scratch; source.offset = uint64_t(i) * kReservedTileBytes;
              source.index = 0; source.next.set(&baseline);
              baseline.buffer = use.baseline; baseline.offset = source.offset;
              baseline.index = 1; baseline.next.set(&destination);
              destination.buffer = merged; destination.offset = uint64_t(use.volume_cast_groups[i]) * kReservedTileBytes;
              destination.index = 2; destination.next.set(&params);
              params.type = WMTComputeCommandSetBytes; params.bytes.set(merge_layout);
              params.length = sizeof(merge_layout); params.index = 3; params.next.set(&dispatch);
              dispatch.type = WMTComputeCommandDispatchThreads; dispatch.size = {kReservedTileBytes, 1, 1};
              auto compute = cmdbuf.computeCommandEncoder(false);
              compute.waitForFence(fence_);
              compute.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
              compute.updateFence(fence_); compute.endEncoding();
            }
          }
        }
        return true;
      }
      // Ordinary BC volumes retain their dense canonical transport. Their
      // shared UInt writer is exported by EndStandardSwizzlePass instead.
      if (to_native) return true;
      if (!EncodeStandardSwizzle(cmdbuf, inflight, resource.ptr(), false,
          {}, 0, false, {}, view.castStorage, view.castCompressed)) return false;
      return true;
    }
    if (view.reservedCast && view.castCompressed) {
      if (to_native) return false;
      auto &storage=*view.reservedCast;
      auto *reserved=owner->reservedResource;
      if (!reserved) return false;
      std::lock_guard<std::mutex> mapping_lock(ReservedResource::MappingQueueMutex);
      const auto generation=reserved->CollectMappedTiles(storage.mapping_generation,storage.mapped_tiles);
      if (generation != storage.mapping_generation) {
        std::unordered_set<uint32_t> wanted;
        D3D12_PACKED_MIP_INFO packed{};
        reserved->GetResourceTiling(nullptr,&packed,nullptr,nullptr,0,nullptr);
        for (uint32_t slice=view.key.array_start;slice<view.key.array_end;++slice) {
          for (uint32_t mip=view.key.mip_start;mip<view.key.mip_end;++mip) {
            const auto &sub=reserved->subresources()[slice*owner->miplevelCount()+mip];
            const uint32_t count=sub.packed ? packed.NumTilesForPackedMips : sub.tiles_x*sub.tiles_y;
            uint32_t mapped=0;
            for (uint32_t tile : storage.mapped_tiles)
              mapped += tile>=sub.first_tile && tile<sub.first_tile+count;
            if (sub.packed && mapped!=count) continue;
            for (uint32_t tile : storage.mapped_tiles)
              if (tile>=sub.first_tile && tile<sub.first_tile+count) wanted.insert(tile);
          }
        }
        if (wanted!=storage.mapped_pages) {
          auto queue=device_->GetSparseMappingQueue();
          if (!queue || !tile_mapping_event_) return false;
          WMT::Reference<WMT::Heap> heap;
          if (!wanted.empty()) {
            WMTHeapInfo info{}; info.type=WMTHeapTypePlacement; info.options=WMTResourceStorageModePrivate;
            info.size=uint64_t(wanted.size())*kReservedTileBytes;
            heap=device_->GetMTLDevice().newPlacementHeap(info,WMTSparsePageSize64);
            if (!heap) return false;
          }
          reserved->MapCastTiles(queue,view.castStorage,{},storage.mapped_pages);
          reserved->MapCastTiles(queue,view.castStorage,heap,wanted);
          queue.signalEvent(tile_mapping_event_,++tile_mapping_value_);
          if (!tile_mapping_event_.waitUntilSignaledValue(tile_mapping_value_,10000)) return false;
          if (storage.heap) inflight.volume_heaps.push_back(storage.heap);
          storage.heap=std::move(heap); storage.mapped_pages=std::move(wanted);
        }
        storage.mapping_generation=generation;
      }
      if (storage.heap) {
        cmdbuf.useAllocation(storage.heap);
        inflight.volume_heaps.push_back(storage.heap);
      }
    }
    if (view.reservedCast && !view.castCompressed && !region) {
      auto &storage = *view.reservedCast;
      auto *reserved = owner->reservedResource;
      if (!reserved) return false;
      const uint32_t width = std::max(owner->width() >> view.key.mip_start, 1u);
      const uint32_t height = std::max(owner->height() >> view.key.mip_start, 1u);
      if (!to_native) {
        std::lock_guard<std::mutex> mapping_lock(ReservedResource::MappingQueueMutex);
        const auto generation = reserved->CollectMappedTiles(storage.mapping_generation, storage.mapped_tiles);
        if (generation != storage.mapping_generation) {
          std::unordered_set<uint32_t> wanted;
          for (uint32_t slice = view.key.array_start; slice < view.key.array_end; ++slice) {
            const auto &sub = reserved->subresources()[slice * owner->miplevelCount() + view.key.mip_start];
            if (sub.packed) {
              D3D12_PACKED_MIP_INFO packed{};
              reserved->GetResourceTiling(nullptr,&packed,nullptr,nullptr,0,nullptr);
              uint32_t mapped=0;
              for (uint32_t tile : storage.mapped_tiles)
                mapped += tile >= sub.first_tile && tile < sub.first_tile + packed.NumTilesForPackedMips;
              if (mapped != packed.NumTilesForPackedMips) continue;
              for (uint32_t y=0;y<storage.pages_y;++y) for (uint32_t x=0;x<storage.pages_x;++x)
                wanted.insert(((slice-view.key.array_start)*storage.pages_y+y)*storage.pages_x+x);
            } else {
              const uint64_t end=uint64_t(sub.first_tile)+uint64_t(sub.tiles_x)*sub.tiles_y;
              for (uint32_t tile : storage.mapped_tiles) {
                if (tile < sub.first_tile || tile >= end) continue;
                const uint32_t local=tile-sub.first_tile;
                wanted.insert(((slice-view.key.array_start)*storage.pages_y+local/sub.tiles_x)*storage.pages_x+local%sub.tiles_x);
              }
            }
          }
          if (wanted != storage.mapped_pages) {
            auto queue=device_->GetSparseMappingQueue();
            if (!queue || !tile_mapping_event_) return false;
            WMT::Reference<WMT::Heap> heap;
            if (!wanted.empty()) {
              WMTHeapInfo info{}; info.type=WMTHeapTypePlacement; info.options=WMTResourceStorageModePrivate;
              info.size=uint64_t(wanted.size())*kReservedTileBytes;
              heap=device_->GetMTLDevice().newPlacementHeap(info,WMTSparsePageSize64);
              if (!heap) return false;
            }
            std::vector<WMTSparseTextureMapping> mappings;
            auto append=[&](uint32_t page, WMTSparseMappingMode mode, uint64_t offset) {
              WMTSparseTextureMapping mapping{}; mapping.mode=mode; mapping.heap_offset=offset; mapping.size={1,1,1};
              mapping.slice=page/(storage.pages_x*storage.pages_y);
              mapping.origin={page%storage.pages_x,(page/storage.pages_x)%storage.pages_y,0};
              mappings.push_back(mapping);
            };
            for (uint32_t page : storage.mapped_pages) append(page,WMTSparseMappingModeUnmap,0);
            if (!mappings.empty()) queue.updateTextureMappings(view.castStorage,{},mappings.data(),mappings.size());
            mappings.clear(); uint64_t offset=0;
            for (uint32_t page : wanted) append(page,WMTSparseMappingModeMap,offset++);
            if (!mappings.empty()) queue.updateTextureMappings(view.castStorage,heap,mappings.data(),mappings.size());
            queue.signalEvent(tile_mapping_event_,++tile_mapping_value_);
            if (!tile_mapping_event_.waitUntilSignaledValue(tile_mapping_value_,10000)) return false;
            if (storage.heap) inflight.volume_heaps.push_back(storage.heap);
            storage.heap=std::move(heap); storage.mapped_pages=std::move(wanted);
          }
          storage.mapping_generation=generation;
        }
        use.cast_regions.clear();
        for (uint32_t page : storage.mapped_pages) {
          const uint32_t slice=page/(storage.pages_x*storage.pages_y);
          if (storage.packed) {
            if (page % storage.pages_x == 0)
              use.cast_regions.push_back({0,0,width,height,slice,{}});
            continue;
          }
          const uint32_t x=page%storage.pages_x*storage.page_w*4;
          const uint32_t y=(page/storage.pages_x)%storage.pages_y*storage.page_h*4;
          if (x>=width || y>=height) continue;
          use.cast_regions.push_back({x,y,std::min(storage.page_w*4,width-x),
              std::min(storage.page_h*4,height-y),slice,{}});
        }
      }
      if (storage.heap) {
        cmdbuf.useAllocation(storage.heap);
        inflight.volume_heaps.push_back(storage.heap);
      }
      for (auto &piece : use.cast_regions) {
        if (to_native) use.baseline=piece.baseline;
        if (!EncodeBlockCast(cmdbuf,inflight,use,to_native,&piece)) return false;
        if (!to_native) piece.baseline=use.baseline;
      }
      return true;
    }
    if (view.castCompressed) {
      // Signed BC4/5/6 and other compressed native classes also require a
      // byte-preserving bridge, but retain the resource's texel geometry.
      if (to_native) return false; // Compressed formats are never UAVs or RTVs.
      const bool volume = owner->textureType() == WMTTextureType3D;
      for (uint32_t array=view.key.array_start;array<view.key.array_end;++array) {
        for (uint32_t mip=view.key.mip_start;mip<view.key.mip_end;++mip) {
          const uint32_t mip_width = std::max(owner->width() >> mip, 1u);
          const uint32_t mip_height = std::max(owner->height() >> mip, 1u);
          const uint32_t depth = std::max(owner->depth() >> mip, 1u);
          const auto transfer = [&](WMTOrigin origin, uint32_t width, uint32_t height) {
          const uint64_t pitch = (((uint64_t(width) + 3) / 4) * view.castBlockSize + 255) & ~255ull;
          const uint64_t image_pitch = pitch * ((height + 3) / 4);
          const uint64_t length = image_pitch * depth;
          WMT::Reference<WMT::Buffer> scratch;
          if (inflight.swizzle_scratch_length >= length) scratch = inflight.swizzle_buffers.back();
          else {
            WMTBufferInfo info{};
            info.length = length;
            info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
            info.memory.set(nullptr);
            scratch = device_->GetMTLDevice().newBuffer(info);
            if (!scratch) return false;
            inflight.swizzle_buffers.push_back(scratch);
            inflight.swizzle_scratch_length = length;
          }
          wmtcmd_blit_copy_from_texture_to_buffer_withblitoption download{};
          download.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          download.src = view.allocation->texture(); download.level = mip; download.slice = array;
          download.origin=origin;
          download.size = {width, height, depth}; download.dst = scratch;
          download.bytes_per_row = pitch; download.bytes_per_image = volume ? image_pitch : 0;
          download.options = WMTBlitOptionNone;
          wmtcmd_blit_copy_from_buffer_to_texture_withblitoption upload{};
          upload.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          upload.src = scratch; upload.level = mip; upload.slice = array;
          upload.origin=origin;
          upload.size = {width, height, depth}; upload.dst = view.castStorage;
          upload.bytes_per_row = pitch; upload.bytes_per_image = volume ? image_pitch : 0;
          upload.options = WMTBlitOptionNone;
          download.next.set(&upload);
          auto blit = cmdbuf.blitCommandEncoder();
          blit.waitForFence(fence_);
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&download));
          blit.updateFence(fence_);
          blit.endEncoding();
          return true;
          };
          if (!view.reservedCast) {
            if (!transfer({},mip_width,mip_height)) return false;
            continue;
          }
          auto *reserved=owner->reservedResource;
          const auto &sub=reserved->subresources()[array*owner->miplevelCount()+mip];
          const auto &mapped=view.reservedCast->mapped_pages;
          if (sub.packed) {
            if (mapped.contains(sub.first_tile) && !transfer({},mip_width,mip_height)) return false;
          } else {
            const auto shape=reserved->tileShape();
            const uint64_t end=uint64_t(sub.first_tile)+uint64_t(sub.tiles_x)*sub.tiles_y;
            for (uint32_t tile : mapped) {
              if (tile<sub.first_tile || tile>=end) continue;
              const uint32_t local=tile-sub.first_tile;
              const uint32_t x=local%sub.tiles_x*shape.WidthInTexels;
              const uint32_t y=local/sub.tiles_x*shape.HeightInTexels;
              if (!transfer({x,y,0},std::min(shape.WidthInTexels,mip_width-x),
                            std::min(shape.HeightInTexels,mip_height-y))) return false;
            }
          }
        }
      }
      return true;
    }
    const uint32_t mip_count = view.castSourceBlockWidth == 1 && !region
        ? view.key.mip_end - view.key.mip_start : 1;
    for (uint32_t relative_mip = 0; relative_mip < mip_count; ++relative_mip) {
      const uint32_t mip = view.key.mip_start + relative_mip;
      const uint32_t width = region ? region->width : std::max(owner->width() >> mip, 1u);
      const uint32_t height = region ? region->height : std::max(owner->height() >> mip, 1u);
      const uint32_t block_width = view.castSourceBlockWidth;
      const uint32_t rows = (height + block_width - 1) / block_width;
      const uint32_t columns = (width + block_width - 1) / block_width;
      const uint32_t depth = std::max(owner->depth() >> mip, 1u);
      const uint32_t arrays = region ? 1 : view.key.array_end - view.key.array_start;
      const uint32_t row_bytes = columns * view.castBlockSize;
      const uint32_t pitch = (row_bytes + 255) & ~255u;
      const uint64_t image_pitch = uint64_t(pitch) * rows;
      WMTBufferInfo info{};
      info.length = image_pitch * depth * arrays;
      info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      info.memory.set(nullptr);
      WMT::Reference<WMT::Buffer> scratch;
      if (!use.writes && !to_native && inflight.swizzle_scratch_length >= info.length) {
        scratch = inflight.swizzle_buffers.back();
      } else {
        scratch = device_->GetMTLDevice().newBuffer(info);
        if (!scratch) return false;
        if (!use.writes && !to_native) {
          inflight.swizzle_buffers.push_back(scratch);
          inflight.swizzle_scratch_length = info.length;
        } else inflight.swizzle_baselines.push_back(scratch);
      }
      const bool volume = owner->textureType() == WMTTextureType3D;
      auto download = [&](WMT::Texture texture, WMT::Buffer buffer, bool native) {
        auto blit = cmdbuf.blitCommandEncoder();
        blit.waitForFence(fence_);
        for (uint32_t array = 0; array < arrays; ++array) {
          wmtcmd_blit_copy_from_texture_to_buffer_withblitoption copy{};
          copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          copy.src = texture;
          copy.level = native ? mip : relative_mip;
          copy.slice = native ? view.key.array_start + (region ? region->array : array) : region ? region->array : array;
          if (region) copy.origin={region->x/(native ? 1u : block_width),region->y/(native ? 1u : block_width),0};
          copy.size = native ? WMTSize{width, height, depth} : WMTSize{columns, rows, depth};
          copy.dst = buffer;
          copy.offset = image_pitch * depth * array;
          copy.bytes_per_row = pitch;
          copy.bytes_per_image = volume ? image_pitch : 0;
          copy.options = WMTBlitOptionNone;
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        }
        blit.updateFence(fence_);
        blit.endEncoding();
      };
      auto upload = [&](WMT::Buffer buffer, WMT::Texture texture, bool native) {
        auto blit = cmdbuf.blitCommandEncoder();
        blit.waitForFence(fence_);
        for (uint32_t array = 0; array < arrays; ++array) {
          wmtcmd_blit_copy_from_buffer_to_texture_withblitoption copy{};
          copy.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          copy.src = buffer;
          copy.src_offset = image_pitch * depth * array;
          copy.bytes_per_row = pitch;
          copy.bytes_per_image = volume ? image_pitch : 0;
          copy.size = native ? WMTSize{width, height, depth} : WMTSize{columns, rows, depth};
          copy.dst = texture;
          copy.level = native ? mip : relative_mip;
          copy.slice = native ? view.key.array_start + (region ? region->array : array) : region ? region->array : array;
          if (region) copy.origin={region->x/(native ? 1u : block_width),region->y/(native ? 1u : block_width),0};
          copy.options = WMTBlitOptionNone;
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        }
        blit.updateFence(fence_);
        blit.endEncoding();
      };
      if (!to_native) {
        download(view.allocation->texture(), scratch, true);
        upload(scratch,view.texture,false);
        if (use.writes) use.baseline = scratch;
        continue;
      }
      if (!use.baseline) return false;
      download(view.texture,scratch,false);
      auto native = device_->GetMTLDevice().newBuffer(info);
      if (!native) return false;
      inflight.swizzle_baselines.push_back(native);
      download(view.allocation->texture(), native, true);
      if (!cast_merge_pipeline_) {
        WMT::Error error;
        auto library = device_->GetLib().getLibrary();
        cast_merge_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(
            WMT4::Library(library).newFunction("d3d12_cast_merge"), error);
        if (!cast_merge_pipeline_) return false;
      }
      uint32_t layout[4] = {row_bytes, pitch, rows, depth * arrays};
      wmtcmd_compute_setpso pso{};
      wmtcmd_compute_setbuffer source{}, baseline{}, destination{};
      wmtcmd_compute_setbytes params{};
      wmtcmd_compute_dispatch dispatch{};
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = cast_merge_pipeline_;
      pso.threadgroup_size = {64, 1, 1};
      pso.next.set(&source);
      source.type = baseline.type = destination.type = WMTComputeCommandSetBuffer;
      source.buffer = scratch; source.index = 0; source.next.set(&baseline);
      baseline.buffer = use.baseline; baseline.index = 1; baseline.next.set(&destination);
      destination.buffer = native; destination.index = 2; destination.next.set(&params);
      params.type = WMTComputeCommandSetBytes;
      params.bytes.set(layout); params.length = sizeof(layout); params.index = 3;
      params.next.set(&dispatch);
      dispatch.type = WMTComputeCommandDispatchThreads;
      dispatch.size = {row_bytes, rows, depth * arrays};
      auto compute = cmdbuf.computeCommandEncoder(false);
      compute.waitForFence(fence_);
      compute.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
      compute.updateFence(fence_);
      compute.endEncoding();
      upload(native, view.allocation->texture(), true);
    }
    return true;
  }

  bool EncodeReservedVolumeRender(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                                  SwizzlePassResource &use, bool publish) {
    auto &view = *use.cast_view;
    auto &volume = *view.reservedVolumeRender;
    auto *texture = view.allocation->descriptor;
    auto *reserved = texture->reservedResource;
    if (!reserved || !view.texture) return false;
    auto layout = *reserved->volumeLayout();
    const bool cast_blocks = reserved->blockWidth() == 4 && view.castBlockSize;
    const uint32_t width = cast_blocks ? (std::max(layout.width >> volume.mip, 1u) + 3) / 4 :
        std::max(layout.width >> volume.mip, 1u);
    const uint32_t height = cast_blocks ? (std::max(layout.height >> volume.mip, 1u) + 3) / 4 :
        std::max(layout.height >> volume.mip, 1u);
    if (cast_blocks) {
      layout.shape_w /= 4; layout.shape_h /= 4;
      layout.page_w /= 4; layout.page_h /= 4;
      layout.block_w = layout.block_h = 1;
    }
    const uint32_t depth = std::max(layout.depth >> volume.mip, 1u);
    const auto &sub = reserved->subresources()[volume.mip];
    TextureViewDescriptor atlas_descriptor{};
    atlas_descriptor.format = texture->pixelFormat(view.key);
    atlas_descriptor.type = WMTTextureType2DArray;
    atlas_descriptor.arraySize = texture->arrayLength();
    if (cast_blocks) {
      atlas_descriptor.castBlockSize = view.castBlockSize;
      atlas_descriptor.volumeCastMip = volume.mip;
    }
    auto &atlas_view = texture->view(texture->createView(atlas_descriptor), use.cast_allocation.ptr());
    if (!atlas_view.texture) return false;
    inflight.cast_views.emplace_back(&atlas_view);
    const WMT::Texture atlas = atlas_view.texture;
    const uint32_t unit_bytes = reserved->bytesPerUnit();
    const uint32_t tile_pitch = layout.page_w * unit_bytes;
    auto volume_address = [&](uint32_t x, uint32_t y, uint32_t z) {
      auto address = reserved->VolumeAddress(volume.mip, cast_blocks ? x * 4 : x, cast_blocks ? y * 4 : y, z);
      if (address && cast_blocks) { address->x /= 4; address->y /= 4; }
      return address;
    };
    auto encode_blit = [&](const auto &command) {
      auto blit = cmdbuf.blitCommandEncoder();
      blit.waitForFence(fence_);
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&command));
      blit.updateFence(fence_);
      blit.endEncoding();
    };
    auto read_tile = [&](uint32_t tile, WMT::Buffer buffer) {
      wmtcmd_blit_copy_from_texture_to_buffer copy{};
      copy.type = WMTBlitCommandCopyFromTextureToBuffer;
      copy.src = atlas; copy.dst = buffer;
      copy.origin = {(tile % layout.atlas_columns) * layout.page_w,
          ((tile / layout.atlas_columns) % layout.atlas_rows) * layout.page_h, 0};
      copy.slice = tile / (layout.atlas_columns * layout.atlas_rows);
      copy.size = {layout.page_w, layout.page_h, 1};
      copy.bytes_per_row = tile_pitch;
      encode_blit(copy);
    };
    if (!publish) {
      std::vector<uint32_t> active_tiles, active_pages;
      {
        std::lock_guard<std::mutex> mapping_lock(ReservedResource::MappingQueueMutex);
        const auto generation = reserved->CollectMappedTiles(volume.mapping_generation, volume.mapped_tiles, volume.mip);
        if (generation != volume.mapping_generation) {
          std::unordered_set<uint32_t> wanted;
          for (uint32_t tile : volume.mapped_tiles) {
            const uint32_t local = tile - sub.first_tile;
            const uint32_t tx = local % sub.tiles_x, ty = (local / sub.tiles_x) % sub.tiles_y;
            const uint32_t tz = local / (sub.tiles_x * sub.tiles_y);
            const uint32_t first_z = std::max(tz * layout.shape_d, volume.first_w);
            const uint32_t last_z = std::min({(tz + 1) * layout.shape_d, depth, volume.first_w + volume.w_size});
            const uint32_t last_x = std::min((tx + 1) * layout.shape_w, width);
            const uint32_t last_y = std::min((ty + 1) * layout.shape_h, height);
            for (uint32_t z = first_z; z < last_z; ++z)
              for (uint32_t y = ty * layout.shape_h / volume.page_h; y <= (last_y - 1) / volume.page_h; ++y)
                for (uint32_t x = tx * layout.shape_w / volume.page_w; x <= (last_x - 1) / volume.page_w; ++x)
                  wanted.insert(((z - volume.first_w) * volume.pages_y + y) * volume.pages_x + x);
          }
          if (wanted != volume.mapped_pages) {
            auto queue = device_->GetSparseMappingQueue();
            if (!queue || !tile_mapping_event_) return false;
            WMT::Reference<WMT::Heap> heap;
            if (!wanted.empty()) {
              WMTHeapInfo info{};
              info.type = WMTHeapTypePlacement;
              info.options = WMTResourceStorageModePrivate;
              info.size = uint64_t(wanted.size()) * kReservedTileBytes;
              heap = device_->GetMTLDevice().newPlacementHeap(info, WMTSparsePageSize64);
              if (!heap) return false;
            }
            std::vector<WMTSparseTextureMapping> operations;
            operations.reserve(std::max(wanted.size(), volume.mapped_pages.size()));
            auto append_page = [&](uint32_t page, WMTSparseMappingMode mode, uint64_t offset) {
              WMTSparseTextureMapping op{};
              op.mode = mode; op.heap_offset = offset; op.size = {1, 1, 1};
              op.origin = {page % volume.pages_x, (page / volume.pages_x) % volume.pages_y, 0};
              op.slice = page / (volume.pages_x * volume.pages_y);
              operations.push_back(op);
            };
            for (uint32_t page : volume.mapped_pages) append_page(page, WMTSparseMappingModeUnmap, 0);
            if (!operations.empty()) queue.updateTextureMappings(view.texture, {}, operations.data(), operations.size());
            operations.clear();
            uint64_t offset = 0;
            for (uint32_t page : wanted) append_page(page, WMTSparseMappingModeMap, offset++);
            if (!operations.empty()) queue.updateTextureMappings(view.texture, heap, operations.data(), operations.size());
            queue.signalEvent(tile_mapping_event_, ++tile_mapping_value_);
            if (!tile_mapping_event_.waitUntilSignaledValue(tile_mapping_value_, 10000)) return false;
            for (const auto &old : volume.heaps) inflight.volume_heaps.push_back(old);
            volume.heaps.clear();
            if (heap) volume.heaps.push_back(std::move(heap));
            volume.mapped_pages = std::move(wanted);
          }
          volume.mapping_generation = generation;
        }
        active_tiles = volume.mapped_tiles;
        active_pages.assign(volume.mapped_pages.begin(), volume.mapped_pages.end());
        // Residency of a sparse texture does not cover its private mapped heaps.
        // Register each backing allocation with this native recording as well.
        for (const auto &heap : volume.heaps) {
          cmdbuf.useAllocation(heap);
          inflight.volume_heaps.push_back(heap);
        }
      }
      // Import NULL logical tiles sharing a mapped native page as well: blending
      // must see zeros, never stale contents. No unmapped native pages allocate.
      for (uint32_t page : active_pages) {
        const uint32_t first_x = page % volume.pages_x * volume.page_w;
        const uint32_t first_y = (page / volume.pages_x) % volume.pages_y * volume.page_h;
        const uint32_t slice = page / (volume.pages_x * volume.pages_y);
        const uint32_t z = volume.first_w + slice;
        for (uint32_t y = first_y; y < std::min(first_y + volume.page_h, height); ++y)
          for (uint32_t x = first_x; x < std::min(first_x + volume.page_w, width);) {
            const auto address = volume_address(x, y, z);
            if (!address) return false;
            const uint32_t count = std::min(address->units, std::min(first_x + volume.page_w, width) - x);
            wmtcmd_blit_copy_from_texture_to_texture copy{};
            copy.type = WMTBlitCommandCopyFromTextureToTexture;
            copy.src = atlas; copy.src_slice = address->slice; copy.src_origin = {address->x, address->y, 0};
            copy.src_size = {count, 1, 1}; copy.dst = view.texture; copy.dst_slice = slice;
            copy.dst_origin = {x, y, 0};
            encode_blit(copy);
            x += count;
          }
      }
      if (use.writes) {
        WMTBufferInfo info{};
        info.length = kReservedTileBytes;
        info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
        for (uint32_t tile : active_tiles) {
          const uint32_t tz = (tile - sub.first_tile) / (sub.tiles_x * sub.tiles_y);
          if (tz * layout.shape_d >= volume.first_w + volume.w_size ||
              (tz + 1) * layout.shape_d <= volume.first_w) continue;
          auto baseline = device_->GetMTLDevice().newBuffer(info);
          if (!baseline) return false;
          read_tile(tile, baseline);
          use.volume_baselines.push_back({tile, baseline});
          inflight.swizzle_baselines.push_back(std::move(baseline));
        }
      }
      return true;
    }
    if (use.volume_baselines.empty()) return true;
    for (const auto &heap : volume.heaps) cmdbuf.useAllocation(heap);
    WMTPixelFormat publish_format = MTLGetUnsignedIntegerFormat(ORIGINAL_FORMAT(texture->pixelFormat(view.key)));
    if (publish_format == WMTPixelFormatInvalid) {
      publish_format = unit_bytes == 1 ? WMTPixelFormatR8Uint :
          unit_bytes == 2 ? WMTPixelFormatR16Uint : WMTPixelFormatR32Uint;
    }
    uint32_t component_widths = 0;
    switch (publish_format) {
    case WMTPixelFormatR8Uint: component_widths = 8; break;
    case WMTPixelFormatRG8Uint: component_widths = 0x0808; break;
    case WMTPixelFormatRGBA8Uint: component_widths = 0x08080808; break;
    case WMTPixelFormatR16Uint: component_widths = 16; break;
    case WMTPixelFormatRG16Uint: component_widths = 0x1010; break;
    case WMTPixelFormatRGBA16Uint: component_widths = 0x10101010; break;
    case WMTPixelFormatR32Uint: component_widths = 32; break;
    case WMTPixelFormatRG32Uint: component_widths = 0x2020; break;
    case WMTPixelFormatRGBA32Uint: component_widths = 0x20202020; break;
    case WMTPixelFormatRGB10A2Uint: component_widths = 0x020a0a0a; break;
    default: return false;
    }
    auto publish_descriptor = atlas_descriptor;
    publish_descriptor.format = publish_format;
    auto &publish_view = texture->view(texture->createView(publish_descriptor), use.cast_allocation.ptr());
    WMT::Texture publish_atlas = publish_view.texture;
    if (publish_atlas) {
      inflight.cast_views.emplace_back(&publish_view);
    } else {
      auto companion = reserved->GetVolumeCastAtlas(device_, publish_format, false);
      if (!companion) return false;
      publish_atlas = companion;
      // BC companions share a single raw uint2/uint4 parent, independent of
      // each typed view's channel widths. This fallback binds that parent.
      if (reserved->blockWidth() == 4 && reserved->blockHeight() == 4)
        component_widths = unit_bytes == 8 ? 0x2020 : 0x20202020;
      inflight.volume_textures.push_back(std::move(companion));
    }
    if (!volume_render_publish_pipeline_) {
      WMT::Error error;
      volume_render_publish_pipeline_ = device_->GetMetal4Compiler().newComputePipelineState(
          WMT4::Library(device_->GetLib().getLibrary()).newFunction("d3d12_volume_render_publish"), error);
      if (!volume_render_publish_pipeline_) return false;
    }
    WMTBufferInfo scratch_info{};
    scratch_info.length = kReservedTileBytes;
    scratch_info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
    auto changed = device_->GetMTLDevice().newBuffer(scratch_info);
    if (!changed) return false;
    inflight.swizzle_buffers.push_back(changed);
    for (const auto &baseline : use.volume_baselines) {
      // Preserve bytes outside this RTV's W range and edge texels verbatim.
      wmtcmd_blit_copy_from_buffer_to_buffer initialize{};
      initialize.type = WMTBlitCommandCopyFromBufferToBuffer;
      initialize.src = baseline.bytes; initialize.dst = changed; initialize.copy_length = kReservedTileBytes;
      encode_blit(initialize);
      const uint32_t local = baseline.tile - sub.first_tile;
      const uint32_t tx = local % sub.tiles_x, ty = (local / sub.tiles_x) % sub.tiles_y;
      const uint32_t tz = local / (sub.tiles_x * sub.tiles_y);
      const uint32_t last_x = std::min((tx + 1) * layout.shape_w, width);
      const uint32_t last_y = std::min((ty + 1) * layout.shape_h, height);
      const uint32_t first_z = std::max(tz * layout.shape_d, volume.first_w);
      const uint32_t last_z = std::min({(tz + 1) * layout.shape_d, depth, volume.first_w + volume.w_size});
      for (uint32_t z = first_z; z < last_z; ++z)
        for (uint32_t y = ty * layout.shape_h; y < last_y; ++y)
          for (uint32_t x = tx * layout.shape_w; x < last_x;) {
            const auto address = volume_address(x, y, z);
            if (!address) return false;
            const uint32_t count = std::min(address->units, last_x - x);
            wmtcmd_blit_copy_from_texture_to_buffer copy{};
            copy.type = WMTBlitCommandCopyFromTextureToBuffer;
            copy.src = view.texture; copy.slice = z - volume.first_w;
            copy.origin = {x, y, 0}; copy.size = {count, 1, 1}; copy.dst = changed;
            copy.offset = (uint64_t(address->y % layout.page_h) * layout.page_w + address->x % layout.page_w) * unit_bytes;
            copy.bytes_per_row = tile_pitch;
            encode_blit(copy);
            x += count;
          }
      // Native sparse textures keep their own physical page swizzling. Never
      // address a heap buffer with a row-major blit byte offset.
      uint32_t merge_layout[7] = {(baseline.tile % layout.atlas_columns) * layout.page_w,
          ((baseline.tile / layout.atlas_columns) % layout.atlas_rows) * layout.page_h,
          baseline.tile / (layout.atlas_columns * layout.atlas_rows),
          layout.page_w, layout.page_h, unit_bytes, component_widths};
      wmtcmd_compute_setpso pso{};
      wmtcmd_compute_setbuffer source{}, original{};
      wmtcmd_compute_settexture destination{};
      wmtcmd_compute_setbytes params{};
      wmtcmd_compute_dispatch dispatch{};
      pso.type = WMTComputeCommandSetPSO; pso.pso = volume_render_publish_pipeline_; pso.threadgroup_size = {32, 1, 1};
      pso.next.set(&source);
      source.type = original.type = WMTComputeCommandSetBuffer;
      source.buffer = changed; source.index = 0; source.next.set(&original);
      original.buffer = baseline.bytes; original.index = 1; original.next.set(&destination);
      destination.type = WMTComputeCommandSetTexture;
      destination.texture = publish_atlas; destination.index = 0; destination.next.set(&params);
      params.type = WMTComputeCommandSetBytes; params.bytes.set(merge_layout); params.length = sizeof(merge_layout);
      params.index = 3; params.next.set(&dispatch);
      dispatch.type = WMTComputeCommandDispatchThreads; dispatch.size = {layout.page_w, layout.page_h, 1};
      auto compute = cmdbuf.computeCommandEncoder(false);
      compute.waitForFence(fence_);
      compute.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
      compute.updateFence(fence_); compute.endEncoding();
    }
    reserved_volume_publish_pending_ = true;
    return true;
  }


  bool BeginStandardSwizzlePass(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                                std::vector<SwizzlePassResource> &uses) {
    // Addresses embedded in descriptors must have an allocation lease too. The native
    // recording retains these metadata/table buffers through submission retirement.
    WMT::MTL4ComputeCommandEncoder volume_encoder{};
    for (const auto &use : uses)
      for (const auto &buffer : use.volume_buffers) {
        if (!buffer) continue;
        if (!volume_encoder) {
          volume_encoder = cmdbuf.computeCommandEncoder(false);
          volume_encoder.waitForFence(fence_);
        }
        wmtcmd_compute_useresource resource{};
        resource.type = WMTComputeCommandUseResource;
        resource.resource = buffer;
        resource.usage = WMTResourceUsageRead;
        volume_encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&resource));
      }
    if (volume_encoder) {
      volume_encoder.updateFence(fence_);
      volume_encoder.endEncoding();
    }
    for (auto &use : uses) {
      if (!use.resource) continue;
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
    for (auto &use : uses) {
      if (!use.cast_view || !use.cast_view->reservedVolumeRender) continue;
      inflight.cast_views.push_back(use.cast_view);
      inflight.cast_allocations.push_back(use.cast_allocation);
      if (!EncodeReservedVolumeRender(cmdbuf, inflight, use, false)) return false;
    }
    for (auto &use : uses) {
      if (!use.cast_view || use.cast_view->reservedVolumeRender) continue;
      inflight.cast_views.push_back(use.cast_view);
      inflight.cast_allocations.push_back(use.cast_allocation);
      if (use.msaa_texture) {
        if (use.msaa_sync && !EncodeWritableMSAA(cmdbuf, inflight, use.msaa_texture, false, use.cast_allocation.ptr())) return false;
      } else if (use.cast_sync && !EncodeBlockCast(cmdbuf,inflight,use,false)) return false;
    }
    return true;
  }

  bool EndStandardSwizzlePass(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                              std::vector<SwizzlePassResource> &uses) {
    for (auto &use : uses) {
      if (!use.cast_view || !use.writes || use.cast_view->reservedVolumeRender) continue;
      if (use.msaa_texture) {
        if (use.msaa_sync && !EncodeWritableMSAA(cmdbuf, inflight, use.msaa_texture, true, use.cast_allocation.ptr())) return false;
      } else if (use.cast_sync && !EncodeBlockCast(cmdbuf,inflight,use,true)) return false;
    }
    for (auto &use : uses) {
      if (use.cast_view && use.cast_view->reservedVolumeRender && use.writes &&
          !EncodeReservedVolumeRender(cmdbuf, inflight, use, true)) return false;
    }
    // Descriptor tables may include inactive placed UAVs aliasing a texture
    // or a buffer actively written in this pass. Compare each placed shadow
    // against its pre-pass canonical baseline, before publishing any shadow.
    std::vector<uint32_t> compared_indices;
    for (uint32_t i = 0; i < uses.size(); ++i) {
      if (uses[i].resource && uses[i].writes && uses[i].baseline)
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
                                   uses[index].baseline,uses[index].volume_source))
          return false;
    }
    for (uint32_t i = 0; i < uses.size(); ++i) {
      const auto &use = uses[i];
      if (!use.resource || !use.writes)
        continue;
      if (!EncodeStandardSwizzle(cmdbuf, inflight, use.resource.ptr(), true,
                                 use.baseline ? WMT::Buffer(dirty_flags) : WMT::Buffer{},
                                 uint64_t(i) * sizeof(uint32_t),false,{},use.volume_source))
        return false;
    }
    return true;
  }

  // Internal AS helper kernels use GPU addresses in their POD parameters. Buffers used for
  // variable-length metadata are retained by the same submission as the dispatch.
  bool EncodeASHelper(WMT::MTL4CommandBuffer cmdbuf, WMT::ComputePipelineState pipeline,
                      const void *parameters, uint32_t bytes, WMT::Buffer geometries = {}, WMT::Buffer jobs = {},
                      uint64_t jobsOffset = 0) {
    if (!pipeline) return false;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_nop head{};
    head.type = WMTComputeCommandNop;
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = pipeline;
    pso.threadgroup_size = {raytracing::kHelperThreadgroup, 1, 1};
    wmtcmd_compute_setbytes params{};
    params.type = WMTComputeCommandSetBytes;
    params.index = 0;
    params.length = bytes;
    params.bytes.set(const_cast<void *>(parameters));
    wmtcmd_compute_setbuffer buffers[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      buffers[i].type = WMTComputeCommandSetBuffer;
      buffers[i].index = i + 1;
      buffers[i].buffer = i ? jobs : geometries;
      buffers[i].offset = i ? jobsOffset : 0;
    }
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {raytracing::kHelperThreads, 1, 1};
    head.next.set(&pso);
    pso.next.set(&params);
    if (geometries) {
      params.next.set(&buffers[0]);
      buffers[0].next.set(&buffers[1]);
      buffers[1].next.set(&dispatch);
    } else params.next.set(&dispatch);
    dispatch.next.set(nullptr);
    encoder.encodeCommands(&head);
    encoder.updateFence(fence_);
    encoder.endEncoding();
    return true;
  }

  void
  ProcessExecute(ExecuteTask &task) {
    std::optional<CommittingScope> scope;
    struct FrameGenerationRecordingScope {
      MTLD3D12Device *device;
      std::shared_ptr<OwnedFrameGenerationFrame> active;
      ~FrameGenerationRecordingScope() {
        if (!active) return;
        const auto reason = device->RemovedReason();
        CompleteOwnedFrameGenerationFrame(active,
            FAILED(reason) ? reason : DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      }
    } frame_generation{device_, {}};
    struct AtomicPhasePermit {
      MTLD3D12CommandQueueImpl *queue;
      std::optional<CommittingScope> &scope;
      GPUExecutionGate::Ticket ticket;
      std::optional<GPUExecutionGate::EncodingAdmission> admission;
      std::shared_ptr<Atomic64Phase> phase;
      void Retire() {
        if (!ticket) return;
        scope.reset();
        const auto last = queue->submission_ring_.lastPublished();
        if (last) queue->submission_ring_.waitUntilRetired(last);
        phase.reset();
        admission.reset();
        ticket.Reset();
      }
      ~AtomicPhasePermit() { Retire(); }
    } atomic{this, scope};
    auto begin_segment = [&] {
      scope.emplace(this, bool(atomic.ticket));
      scope->inflight.reflex_work = task.reflex_work;
      if (task.reflex_work) scope->inflight.reflex_driver_start = ReflexState::NowUs();
      scope->inflight.protections.reserve(task.lists.size());
      if (frame_generation.active)
        scope->inflight.frame_generation_frames.push_back(frame_generation.active);
      for (auto &list : task.lists)
        scope->inflight.protections.push_back({list.allocator.Retain(), list.recording_lease.Retain()});
      /*
       * Mark this submission active before anything is encoded. Every acceleration-structure
       * version superseded while it is in flight is held until it retires, because this
       * submission may already have encoded a dispatch that reads the old structure.
       *
       * Each segment owns its own in-flight version snapshot, including segments
       * split for feature history, aliasing, and GPU-produced metadata readback.
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
    auto begin_atomic = [&](uint64_t participants,
                            const std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables,
                            const std::vector<Atomic64RootBinding> &roots,
                            std::vector<SwizzlePassResource> *swizzle = nullptr) -> bool {
      const auto preceding = scope->seq;
      scope.reset();
      submission_ring_.waitUntilRetired(preceding);
      atomic.ticket = device_->gpu_execution_gate.AcquireExclusive();
      if (!atomic.ticket) return false;
      atomic.admission.emplace(device_->gpu_execution_gate);
      Atomic64PhaseBindings bindings;
      HRESULT hr = CollectAtomic64PhaseBindings(device_, tables ? std::span<const StandardSwizzleTableUse>(*tables)
                                                                : std::span<const StandardSwizzleTableUse>{},
                                                roots, bindings);
      if (SUCCEEDED(hr)) hr = PrepareAtomic64Phase(device_, std::max<uint64_t>(participants, 1), bindings, atomic.phase);
      if (FAILED(hr)) { device_->ReportRemoval(hr); return false; }
      if (!start_segment()) return false;
      scope->inflight.atomic64_phase = atomic.phase;
      if (swizzle && !BeginStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, *swizzle)) {
        device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return false;
      }
      hr = atomic.phase->EncodeImport(scope->inflight.cmdbuf, fence_);
      if (SUCCEEDED(hr)) hr = atomic.phase->PublishContext(scope->inflight.cmdbuf, fence_);
      if (FAILED(hr)) { device_->ReportRemoval(hr); return false; }
      return true;
    };
    auto end_atomic = [&](std::vector<SwizzlePassResource> *swizzle = nullptr) -> bool {
      HRESULT hr = atomic.phase->EncodeExport(scope->inflight.cmdbuf, fence_);
      if (SUCCEEDED(hr) && swizzle && !EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, *swizzle))
        hr = DXGI_ERROR_DRIVER_INTERNAL_ERROR;
      if (SUCCEEDED(hr)) hr = atomic.phase->ClearContext(scope->inflight.cmdbuf, fence_);
      if (FAILED(hr)) { device_->ReportRemoval(hr); return false; }
      atomic.Retire();
      return device_->RemovedReason() == S_OK && start_segment();
    };
    struct AtomicReadback { WMT::Reference<WMT::Buffer> owner; const uint8_t *bytes = nullptr; };
    auto read_atomic_buffer = [&](WMT::Buffer source, uint64_t offset, uint64_t length, AtomicReadback &result) -> bool {
      WMTBufferInfo info{};
      info.length = std::max<uint64_t>(length, 16);
      info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
      result.owner = device_->GetMTLDevice().newBuffer(info);
      result.bytes = static_cast<const uint8_t *>(info.memory.get_accessible_or_null());
      if (!result.owner || !result.bytes) { device_->ReportRemoval(E_OUTOFMEMORY); return false; }
      auto blit = scope->inflight.cmdbuf.blitCommandEncoder();
      blit.waitForFence(fence_);
      wmtcmd_blit_copy_from_buffer_to_buffer copy{}; copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = source; copy.src_offset = offset; copy.dst = result.owner; copy.copy_length = length;
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      blit.updateFence(fence_); blit.endEncoding();
      const auto producer = scope->seq; scope.reset(); submission_ring_.waitUntilRetired(producer);
      return device_->RemovedReason() == S_OK && start_segment();
    };
    auto read_atomic_address = [&](uint64_t address, uint64_t length, AtomicReadback &result) -> bool {
      uint64_t offset = 0; auto *allocation = device_->LookupBufferByVA(address, &offset);
      if (!allocation || offset > allocation->byteLength() || length > allocation->byteLength() - offset) {
        device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return false;
      }
      return read_atomic_buffer(allocation->buffer(), offset, length, result);
    };
    auto capture_atomic_arguments = [](MTLD3D12RootSignature *signature, MTLD3D12DescriptorHeap *heap,
                                       const uint8_t *bytes, std::vector<Atomic64RootBinding> &roots,
                                       std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables,
                                       const MTLD3D12PipelineState *pipeline = nullptr) {
      if (!signature) return;
      if (heap && (signature->Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)) {
        D3D12_GPU_DESCRIPTOR_HANDLE start{}; heap->GetGPUDescriptorHandleForHeapStart(&start);
        if (!tables) tables = std::make_unique<std::vector<StandardSwizzleTableUse>>();
        tables->push_back({signature, heap, start.ptr, ~0u});
      }
      for (uint32_t parameter = 0; parameter < signature->ParameterSlots; ++parameter) {
        const auto type = signature->ParameterTypes[parameter];
        if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) continue;
        uint64_t address; memcpy(&address, bytes + uint64_t(signature->SlotQwordOffsets[parameter]) * 8, sizeof(address));
        if (!address) continue;
        if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && heap) {
          if (!tables) tables = std::make_unique<std::vector<StandardSwizzleTableUse>>();
          tables->push_back({signature, heap, address, parameter});
          if (pipeline) {
            tables->back().shader_bindings = pipeline->resource_bindings;
            tables->back().restrict_to_shader = true;
          }
        } else if (type == D3D12_ROOT_PARAMETER_TYPE_CBV || type == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                   type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
          if (pipeline) {
            const auto *range = signature->GetRootDescriptorRange(parameter);
            bool used = false;
            if (range) signature->ForEachUsedDescriptorRange({range, 1}, pipeline->resource_bindings,
                [&](uint64_t, uint64_t, D3D12_DESCRIPTOR_RANGE_TYPE) { used = true; });
            if (!used) continue;
          }
          roots.push_back({address, UINT64_MAX, type == D3D12_ROOT_PARAMETER_TYPE_UAV});
        }
      }
    };
    auto read_render_atomic_arguments = [&](const RenderEncoderData &render, WMT::Buffer buffer, uint64_t offset,
                                           std::vector<Atomic64RootBinding> &roots,
                                           std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables) -> bool {
      auto *signature = static_cast<MTLD3D12RootSignature *>(render.atomic64_signature.ptr());
      auto *heap = static_cast<MTLD3D12DescriptorHeap *>(render.atomic64_heap.ptr());
      AtomicReadback arguments;
      if (signature && signature->ParameterQwords) {
        // Direct draws inherit recorded bindings; indirect draws provide an override.
        if (!buffer) {
          for (auto *cmd = static_cast<const wmtcmd_base *>(render.cmd_head.next.get_accessible_or_null()); cmd;
               cmd = static_cast<const wmtcmd_base *>(cmd->next.get_accessible_or_null())) {
            if (cmd->type != WMTRenderCommandSetVertexBuffer && cmd->type != WMTRenderCommandSetFragmentBuffer &&
                cmd->type != WMTRenderCommandSetObjectBuffer && cmd->type != WMTRenderCommandSetMeshBuffer) continue;
            const auto *binding = reinterpret_cast<const wmtcmd_render_setbuffer *>(cmd);
            if (binding->index != SM50_BINDING_INDEX_ROOT_ARGUMENTS) continue;
            buffer = WMT::Buffer{binding->buffer}; offset = binding->offset;
            break;
          }
        }
        if (!buffer) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return false; }
        if (!read_atomic_buffer(buffer, offset, uint64_t(signature->ParameterQwords) * 8, arguments)) return false;
      }
      capture_atomic_arguments(signature, heap, arguments.bytes, roots, tables,
          static_cast<MTLD3D12PipelineState *>(render.atomic64_pipeline.ptr()));
      return true;
    };
    auto producer_participants = [&](const RenderEncoderData &render, const MSAADrawData *draw,
                                     const MeshCaptureExecution *capture = nullptr, uint32_t selected = UINT32_MAX) -> uint64_t {
      if (draw && !draw->mesh_capture) {
        const uint64_t vertices = uint64_t(draw->assembly.vertex_count) * draw->assembly.instance_count;
        return ((vertices + 63) / 64) * 64;
      }
      uint64_t participants = 0;
      auto add = [&](std::initializer_list<uint64_t> dimensions) {
        uint64_t count = 1;
        for (auto dimension : dimensions) {
          if (dimension && count > UINT64_MAX / dimension) return false;
          count *= dimension;
        }
        if (participants > UINT64_MAX - count) return false;
        participants += count; return true;
      };
      for (auto *node = static_cast<const wmtcmd_base *>(render.cmd_head.next.get()); node;
           node = static_cast<const wmtcmd_base *>(node->next.get())) {
        switch (node->type) {
        case WMTRenderCommandDraw: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_draw *>(node);
          if (!add({cmd->vertex_count, cmd->instance_count})) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandDrawIndexed: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_draw_indexed *>(node);
          if (!add({cmd->index_count, cmd->instance_count})) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandDXMTGeometryDraw: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_dxmt_geometry_draw *>(node);
          if (!add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp}) ||
              (!draw && !add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp, render.atomic64_gs_instances}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandDXMTGeometryDrawIndexed: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_dxmt_geometry_draw_indexed *>(node);
          if (!add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp}) ||
              (!draw && !add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp, render.atomic64_gs_instances}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandDXMTTessellationMeshDraw: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_dxmt_tessellation_mesh_draw *>(node);
          if (!add({cmd->patch_per_mesh_instance, cmd->instance_count, cmd->threads_per_patch, cmd->patch_per_group}) ||
              (!draw && !add({cmd->patch_per_mesh_instance, cmd->instance_count, cmd->patch_per_group, render.atomic64_tess_workloads, 32}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *>(node);
          if (!add({cmd->patch_per_mesh_instance, cmd->instance_count, cmd->threads_per_patch, cmd->patch_per_group}) ||
              (!draw && !add({cmd->patch_per_mesh_instance, cmd->instance_count, cmd->patch_per_group, render.atomic64_tess_workloads, 32}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandDrawMeshThreadgroups:
        case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
          WMTSize groups{}, threads{}, mesh_threads{};
          if (node->type == WMTRenderCommandDrawMeshThreadgroups) {
            const auto *cmd = reinterpret_cast<const wmtcmd_render_draw_meshthreadgroups *>(node);
            groups = cmd->threadgroup_per_grid; threads = cmd->object_threadgroup_size; mesh_threads = cmd->mesh_threadgroup_size;
          } else {
            const auto *cmd = reinterpret_cast<const wmtcmd_render_draw_meshthreadgroups_indirect *>(node);
            threads = cmd->object_threadgroup_size; mesh_threads = cmd->mesh_threadgroup_size;
            if (!draw) {
              AtomicReadback arguments;
              if (!read_atomic_buffer(WMT::Buffer{cmd->indirect_args_buffer}, cmd->indirect_args_offset, sizeof(D3D12_DISPATCH_ARGUMENTS), arguments)) return UINT64_MAX;
              D3D12_DISPATCH_ARGUMENTS actual; memcpy(&actual, arguments.bytes, sizeof(actual));
              groups = {actual.ThreadGroupCountX, actual.ThreadGroupCountY, actual.ThreadGroupCountZ};
            }
          }
          if (draw) {
            if (!add({draw->mesh_control.total_slots, threads.width, threads.height, threads.depth})) return UINT64_MAX;
          } else if (!add({groups.width, groups.height, groups.depth, threads.width, threads.height, threads.depth}) ||
                     !add({groups.width, groups.height, groups.depth, threads.width, threads.height, threads.depth,
                           render.atomic64_gs_instances, mesh_threads.width, mesh_threads.height, mesh_threads.depth})) return UINT64_MAX;
          break;
        }
        default: break;
        }
      }
      if (capture) {
        const auto &data = *capture->data;
        for (const auto &command : capture->commands) {
          if (selected != UINT32_MAX && command.slot != selected) continue;
          if (data.geometry) {
            const uint64_t vertices = uint64_t(command.vertex_count) * (data.strip_expand ? data.strip_vertices_per_primitive : 1);
            const uint64_t warps = (vertices + data.warp_increment - 1) / data.warp_increment;
            if (!add({warps, command.instances, data.vertex_per_warp}) ||
                (!draw && !add({warps, command.instances, data.vertex_per_warp, render.atomic64_gs_instances}))) return UINT64_MAX;
          } else if (render.capture_indirect_pass == 1) {
            const uint32_t per_group = 32 / data.threads_per_patch;
            const uint64_t groups = (uint64_t(command.patches) + per_group - 1) / per_group;
            if (!add({groups, command.instances, data.threads_per_patch, per_group}) ||
                (!draw && !add({groups, command.instances, per_group, data.layout.max_workloads, 32}))) return UINT64_MAX;
          } else {
            const uint64_t primitives = uint64_t(command.slots) * data.layout.max_primitives;
            const uint64_t groups = (primitives + data.per_group - 1) / data.per_group;
            if (!add({groups, 32}) || (!draw && !add({groups, 32, render.atomic64_gs_instances}))) return UINT64_MAX;
          }
        }
      }
      // These are compiler-proven workload bounds, never assumed GPU residency.
      if (draw && !add({draw->mesh_control.total_slots, draw->mesh_layout.threads_per_group})) return UINT64_MAX;
      return participants;
    };
    if (!start_segment())
      return;
    const bool fenced = std::any_of(task.lists.begin(), task.lists.end(), [](const auto &list) { return !list.fence_operations.empty(); });
    // Capture replay before any admission. Fenced tasks validate and decay in
    // several segments, so a first execution must not turn into a replay when
    // its Begin sets started. The recording lease excludes another execution
    // of these encoder payloads until this task and its submissions retire.
    for (const auto &list : task.lists)
      for (auto *encoder = list.entry; encoder; encoder = encoder->next) {
        if (encoder->type != EncoderType::Temporal) continue;
        auto *data = static_cast<TemporalEncoderData *>(encoder);
        if (!data->fg_frame || data->fg_stage != TemporalEncoderData::FrameGenerationStage::Begin) continue;
        if (!data->fg_end) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return; }
        data->fg_replay = HasOwnedFrameGenerationFrameStarted(data->fg_frame);
        encoder = data->fg_end;
      }
    auto next_state_event = [](EncoderData *&cursor, size_t index, size_t limit) {
      while (cursor) {
        if (cursor->type != EncoderType::Temporal) { cursor = cursor->next; continue; }
        auto *data = static_cast<TemporalEncoderData *>(cursor);
        if (!data->fg_frame || data->fg_stage != TemporalEncoderData::FrameGenerationStage::Begin) {
          cursor = cursor->next; continue;
        }
        if (data->fg_replay) {
          if (index < data->fg_state_event_index) return index;
          if (index < data->fg_end->fg_state_event_index) index = std::min(limit, data->fg_end->fg_state_event_index);
        }
        cursor = data->fg_end->next;
      }
      return index;
    };
    auto validate_transitions = [&](const ListSubmission *segment, size_t first, size_t last, bool decay) {
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
          if (segment && &list != segment) continue;
          const auto &events = static_cast<RecordingHead *>(list.entry)->state_events;
          const size_t limit = segment ? last : events.size();
          auto *cursor = list.entry;
          for (size_t index = next_state_event(cursor, segment ? first : 0, limit); index < limit;
               index = next_state_event(cursor, index + 1, limit)) {
            const auto &transition = events[index];
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
          if (decay && segment)
            for (const auto &list : task.lists) {
              const auto &events = static_cast<RecordingHead *>(list.entry)->state_events;
              auto *cursor = list.entry;
              for (size_t index = next_state_event(cursor, 0, events.size()); index < events.size();
                   index = next_state_event(cursor, index + 1, events.size()))
                pending.try_emplace(events[index].resource.get(), events[index].resource->state);
            }
          if (decay) for (auto &[tracker, state] : pending) {
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
        return false;
      }
      return true;
    };
    if (!fenced && !validate_transitions(nullptr, 0, 0, true)) return;
    for (auto &list : task.lists) {
      if (device_->RemovedReason() != S_OK)
        return;
      owned_graphics::Trace("execute_list", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                            ",\"list\":", list.list_id,
                            ",\"recording\":", list.recording);
      EncoderData *current = list.entry;
      size_t fence_operation = 0;
      size_t state_event = 0;
      auto validate_segment = [&](EncoderData *start) {
        size_t end = static_cast<RecordingHead *>(list.entry)->state_events.size();
        for (auto *next = start; next; next = next->next)
          if (next->type == EncoderType::FenceBarrier) {
            end = static_cast<FenceBarrierEncoderData *>(next)->state_event_index; break;
          }
        if (!validate_transitions(&list, state_event, end, false)) return false;
        state_event = end; return true;
      };
      if (fenced && !validate_segment(current)) return;
      // Capture-sized ExecuteIndirect: prepared at its pass-1 render pass, consumed by its last render pass.
      std::optional<MeshCaptureExecution> capture_execution;
      if (ReservedResource::AnyLive()) {
        /* Command lists see each other's writes; for sparse tiles sharing a heap page that needs a
           shared event signal/wait, not only the pass boundary (ReservedResource::AnyLive). */
        scope.reset();
        EncodeAliasFlush();
        reserved_volume_publish_pending_ = false;
        if (!start_segment())
          return;
      }
      while (current) {
        if (device_->RemovedReason() != S_OK)
          return;
        if (reserved_volume_publish_pending_ &&
            !(current->type == EncoderType::Query &&
              static_cast<QueryEncoderData *>(current)->op == QueryEncoderOp::AliasFlush)) {
          // A virtual volume RTV publishes through its physical heap page.
          // Flush sparse aliases before the next native-texture consumer;
          // this is queue-local GPU ordering, not a CPU retirement wait.
          scope.reset();
          EncodeAliasFlush();
          reserved_volume_publish_pending_ = false;
          if (!start_segment()) return;
        }
        if (current->type == EncoderType::FenceBarrier) {
          auto *barrier = static_cast<FenceBarrierEncoderData *>(current);
          // Ordinary FIFO handlers publish logical signals only on validated
          // retirement. A wait blocks this worker, never the application caller.
          scope.reset();
          for (size_t i = 0; i < barrier->operands.size(); ++i) {
            auto &operation = list.fence_operations[fence_operation++];
            if (operation.signal) {
              SignalTask signal{operation.fence, operation.ticket, operation.fence_id};
              ProcessSignal(signal);
            } else {
              WaitTask wait{operation.fence, operation.ticket, operation.fence_id};
              ProcessWait(wait);
              if (cancel_unsubmitted_) return;
            }
            if (device_->RemovedReason() != S_OK) return;
          }
          if (!start_segment()) return;
          current = current->next;
          if (fenced && !validate_segment(current)) return;
          continue;
        }
        if (current->type == EncoderType::WorkGraph) {
          auto *graph = static_cast<WorkGraphEncoderData *>(current);
          const uint64_t producer = scope->seq;
          scope.reset();
          submission_ring_.waitUntilRetired(producer);
          if (device_->RemovedReason() != S_OK) return;
          struct {
            MTLD3D12CommandQueueImpl *owner;
            InflightCommandBuffer retained;
            std::vector<SwizzlePassResource> uses;
          } publication{this};
          const WorkGraphTexturePublisher textures{
            [&publication](WMT::MTL4CommandBuffer buffer, WMT::Fence graph_fence,
                           std::span<const StandardSwizzleTableUse> global,
                           std::span<const StandardSwizzleTableUse> local) -> HRESULT {
              // The previous group was actually retired before the executor
              // calls begin again. No ordinary ticket belongs to this scratch.
              publication.retained = {};
              publication.uses.clear();
              auto *owner = publication.owner;
              owner->AppendStandardSwizzleTables(global, publication.uses);
              owner->AppendStandardSwizzleTables(local, publication.uses);
              if (publication.uses.empty()) return S_FALSE;
              if (!owner->BeginStandardSwizzlePass(buffer, publication.retained, publication.uses))
                return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
              // Transfer kernels use the queue fence; original node kernels
              // and atomic import/export use the executor fence. Bridge them
              // explicitly rather than relying on encoder issue order.
              auto bridge = buffer.computeCommandEncoder(false);
              bridge.waitForFence(owner->fence_);
              bridge.updateFence(graph_fence);
              bridge.endEncoding();
              return S_OK;
            },
            [&publication](WMT::MTL4CommandBuffer buffer, WMT::Fence graph_fence) -> HRESULT {
              auto *owner = publication.owner;
              auto bridge = buffer.computeCommandEncoder(false);
              bridge.waitForFence(graph_fence);
              bridge.updateFence(owner->fence_);
              bridge.endEncoding();
              if (!owner->EndStandardSwizzlePass(buffer, publication.retained, publication.uses))
                return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
              bridge = buffer.computeCommandEncoder(false);
              bridge.waitForFence(owner->fence_);
              bridge.updateFence(graph_fence);
              bridge.endEncoding();
              return S_OK;
            }};
          HRESULT result = ExecuteWorkGraph(device_, queue_, *graph->dispatch, textures);
          if (FAILED(result)) {
            ERR("DispatchGraph execution failed: ", str::format(result));
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          if (!start_segment()) return;
          current = current->next;
          continue;
        }
        bool execute_predicated_texture_copy = true;
        if (current->type == EncoderType::PredicatedTextureCopy) {
          auto copy = static_cast<PredicatedTextureCopyEncoderData *>(current);
          const uint64_t predicate_submission = scope->seq;
          // The submission worker, never the application thread, commits the
          // commands which produced the retained snapshot and waits for their
          // actual completion before consulting CPU-coherent allocator memory.
          scope.reset();
          submission_ring_.waitUntilRetired(predicate_submission);
          if (device_->RemovedReason() != S_OK)
            return;
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
              if (!EncodeSOIndirectPreflight(scope->inflight.cmdbuf, render->so_indirect->preflight_pipeline,
                                             params, 0, render->so_indirect->draw_template->predicate_heap,
                                             preflight_storage, preflight_result)) {
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
              if (!EncodeSOIndirectPreflight(scope->inflight.cmdbuf, render->so_indirect->preflight_pipeline,
                                             params, actual_count, render->so_indirect->draw_template->predicate_heap,
                                             preflight_storage, preflight_result)) {
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
          if (render->capture_indirect_pass == 1) {
            // Capture-sized ExecuteIndirect: read the GPU-written arguments (after every earlier producer), then
            // size each command's captures exactly before any of it runs.
            const auto &indirect = render->capture_indirect;
            WMT::Reference<WMT::Buffer> preflight_storage;
            const SOIndirectPreflightResult *preflight_result = nullptr;
            if (!EncodeSOIndirectPreflight(scope->inflight.cmdbuf, indirect->preflight_pipeline, indirect->preflight,
                                           indirect->preflight.max_count, indirect->predicate_heap,
                                           preflight_storage, preflight_result)) {
              ERR("ExecuteIndirect: capture argument preflight allocation failed");
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
            capture_execution.emplace();
            bool prepared = false;
            try {
              prepared = PrepareMeshCaptureExecution(indirect, *preflight_result, scope->inflight, *capture_execution);
            } catch (const std::bad_alloc &) {
              prepared = false;
            }
            if (!prepared) {
              ERR("ExecuteIndirect: capture cannot be allocated for GPU draw arguments");
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            EncodeTessGeometryClear(scope->inflight.cmdbuf, *capture_execution);
            EncodeStripExpansion(scope->inflight.cmdbuf, *capture_execution);
          }
          if (render->capture_indirect_pass &&
              (!capture_execution || capture_execution->data != render->capture_indirect)) {
            ERR("ExecuteIndirect: capture pass without its preflight");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          if (render->capture_indirect_pass == 2)
            EncodeTessGeometryCompaction(scope->inflight.cmdbuf, *capture_execution);
        }

        std::vector<MSAAExecution> msaa_executions;
        std::vector<MSAADrawData> msaa_draws;
        std::vector<std::vector<Atomic64RootBinding>> msaa_atomic_roots;
        std::vector<std::unique_ptr<std::vector<StandardSwizzleTableUse>>> msaa_atomic_tables;
        if (current->type == EncoderType::Render) {
          auto *render = static_cast<RenderEncoderData *>(current);
          if (render->msaa_draw) {
            const auto *mesh_capture_execution = render->capture_indirect_pass ? &*capture_execution : nullptr;
            // Vertex capture is a shader pass submitted before the raster pass.
            // Import its descriptor-table textures into this producer segment,
            // not only into the later render segment.
            auto producer_swizzle_uses = CollectStandardSwizzleTables(render->swizzle_tables);
            if (!render->msaa_draw->atomic64_phase &&
                !BeginStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, producer_swizzle_uses)) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            std::optional<MSAADrawData> mesh_template;
            if (render->msaa_draw->mesh_grid_buffer || render->msaa_draw->object_replay_pipeline)
              mesh_template = *render->msaa_draw;
            const MSAADrawData &mesh_draw = mesh_template ? *mesh_template : *render->msaa_draw;
            auto finish_mesh_preflight = [&]() -> bool {
              if (!mesh_draw.atomic64_phase &&
                  !EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, producer_swizzle_uses)) return false;
              const auto producer = scope->seq; scope.reset(); submission_ring_.waitUntilRetired(producer);
              if (device_->RemovedReason() != S_OK || !start_segment()) return false;
              return mesh_draw.atomic64_phase ||
                  BeginStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, producer_swizzle_uses);
            };
            auto set_mesh_grid = [&](MSAADrawData &draw, const uint32_t *grid) -> bool {
              const uint64_t slots = uint64_t(grid[0]) * grid[1] * grid[2];
              const uint64_t records = slots * draw.mesh_layout.max_primitives * draw.mesh_layout.vertices_per_primitive;
              if (slots > (1u << 22) || records > UINT32_MAX) return false;
              draw.mesh_control = {grid[0], grid[1], 0, uint32_t(slots)};
              draw.assembly.vertex_count = draw.assembly.positions_per_instance = uint32_t(records);
              draw.assembly.instance_count = 1;
              draw.assembly.max_primitives = uint32_t(slots * draw.mesh_layout.max_primitives);
              return true;
            };
            if (mesh_draw.mesh_grid_buffer) {
              WMTBufferInfo info{}; info.length = 16; info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
              auto readback = device_->GetMTLDevice().newBuffer(info);
              const auto *grid = static_cast<const uint32_t *>(info.memory.get_accessible_or_null());
              if (!readback || !grid) { device_->ReportRemoval(E_OUTOFMEMORY); return; }
              scope->inflight.msaa_buffers.push_back(readback);
              auto blit = scope->inflight.cmdbuf.blitCommandEncoder(); blit.waitForFence(fence_);
              wmtcmd_blit_copy_from_buffer_to_buffer copy{}; copy.type = WMTBlitCommandCopyFromBufferToBuffer;
              copy.src = mesh_draw.mesh_grid_buffer; copy.src_offset = mesh_draw.mesh_grid_offset;
              copy.dst = readback; copy.copy_length = 16;
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy)); blit.updateFence(fence_); blit.endEncoding();
              if (!finish_mesh_preflight() || !set_mesh_grid(*mesh_template, grid)) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
            }
            WMT::Reference<WMT::Buffer> object_records;
            const uint32_t *object_records_mapped = nullptr;
            if (mesh_draw.object_replay_pipeline && mesh_draw.mesh_control.total_slots) {
              std::vector<Atomic64RootBinding> roots;
              std::unique_ptr<std::vector<StandardSwizzleTableUse>> tables;
              if (mesh_draw.atomic64_phase) {
                if (!read_render_atomic_arguments(*render, mesh_draw.root_buffer, mesh_draw.root_offset, roots, tables)) return;
                producer_swizzle_uses = CollectStandardSwizzleTables(tables);
                if (!begin_atomic(producer_participants(*render, &mesh_draw), tables, roots, &producer_swizzle_uses)) return;
              }
              if (!EncodeObjectCapture(scope->inflight.cmdbuf, scope->inflight, *render, mesh_draw,
                                       object_records, object_records_mapped)) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              if (mesh_draw.atomic64_phase && !end_atomic(&producer_swizzle_uses)) return;
              if (!finish_mesh_preflight()) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return; }
              scope->inflight.msaa_buffers.push_back(object_records);
            }
            const auto &so_draws = indirect_execution ? indirect_execution->draws :
                mesh_capture_execution ? mesh_capture_execution->so_draws : render->so_draws;
            if (render->msaa_draw->needs_stream_assembly && !so_draws.empty())
              EncodeStreamOutputSetup(scope->inflight.cmdbuf, scope->inflight, so_draws);
            const uint32_t count = indirect_execution ? indirect_execution->count :
                mesh_capture_execution ? uint32_t(mesh_capture_execution->commands.size()) :
                object_records ? mesh_draw.mesh_control.total_slots : 1;
            msaa_executions.reserve(count);
            msaa_draws.reserve(count);
            for (uint32_t i = 0; i < count; i++) {
              MSAADrawData draw = mesh_draw;
              if (object_records) {
                const uint64_t word = uint64_t(i) * draw.object_layout.record_dwords;
                if (!set_mesh_grid(draw, object_records_mapped + word)) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
                draw.object_records = object_records;
                draw.object_record_offset = word * 4;
              }
              if (mesh_capture_execution) {
                const auto &capture = *mesh_capture_execution;
                const auto &command = capture.commands[i];
                const auto &data = *capture.data;
                if (data.root_stride) {
                  draw.root_buffer = data.root_heap;
                  draw.root_offset = data.root_offset + uint64_t(command.slot) * data.root_stride;
                }
                if (data.vertex_stride) {
                  draw.vertex_buffer_table = data.vertex_heap;
                  draw.vertex_buffer_offset = data.vertex_offset + uint64_t(command.slot) * data.vertex_stride;
                }
                const uint64_t primitives = data.geometry
                    ? data.strip_expand ? command.vertex_count : MeshCaptureInputPrimitives(data.topology, command.vertex_count)
                    : data.domain_only ? command.patches : command.slots * data.layout.max_primitives;
                const uint32_t instances = data.geometry || data.domain_only ? command.instances : 1;
                const uint64_t slots = primitives * instances * draw.mesh_layout.max_workloads;
                const uint64_t records = slots * draw.mesh_layout.max_primitives * draw.mesh_layout.vertices_per_primitive;
                if (slots > UINT32_MAX || records > UINT32_MAX) {
                  device_->ReportRemoval(E_OUTOFMEMORY); return;
                }
                draw.mesh_control = {uint32_t(primitives), instances, 0, uint32_t(slots)};
                draw.assembly.vertex_count = uint32_t(records);
                draw.assembly.instance_count = 1;
                draw.assembly.max_primitives = uint32_t(records / draw.mesh_layout.vertices_per_primitive);
                draw.stream_output = command.so;
                draw.statistics_claim = data.statistics_claim;
                draw.statistics_claim_offset = data.statistics_claim_offset +
                    uint64_t(command.slot) * sizeof(SM50_STATISTICS_CLAIM_HEADER);
              }
              if (indirect_execution) {
                const auto &record = indirect_execution->bindings[i].arguments;
                const auto &prepared = indirect_execution->draws[i];
                draw.stream_output = prepared;
                draw.assembly.vertex_count = record.vertex_count;
                draw.assembly.instance_count = record.instance_count;
                draw.assembly.positions_per_instance = record.vertex_count;
                const uint32_t vpp = draw.assembly.topology == 0 ? 1 : draw.assembly.topology <= 2 ? 2 : 3;
                const bool strip = draw.assembly.topology == 2 || draw.assembly.topology == 4 || draw.assembly.topology == 5;
                const uint64_t per_instance = strip ? record.vertex_count >= vpp
                    ? record.vertex_count - vpp + 1 : 0 : record.vertex_count / vpp;
                const uint64_t primitives = per_instance * record.instance_count;
                const uint32_t index_stride = MSAAIndexStride(draw.line_mode, draw.conservative);
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
                draw.static_sampler_buffer = render->so_indirect->static_sampler_buffer;
                draw.static_sampler_offset = render->so_indirect->static_sampler_offset;
              }
              draw.bounds.max_primitives = draw.assembly.max_primitives;
              if (draw.atomic64_phase) {
                msaa_atomic_roots.emplace_back(); msaa_atomic_tables.emplace_back();
                if (!read_render_atomic_arguments(*render, draw.root_buffer, draw.root_offset,
                    msaa_atomic_roots.back(), msaa_atomic_tables.back())) return;
                producer_swizzle_uses = CollectStandardSwizzleTables(msaa_atomic_tables.back());
              }
              msaa_draws.push_back(draw);
              msaa_executions.emplace_back();
              const bool atomic_producer = draw.atomic64_phase && draw.assembly.vertex_count && draw.assembly.instance_count;
              if (atomic_producer && !begin_atomic(producer_participants(*render, &draw, mesh_capture_execution,
                  mesh_capture_execution ? mesh_capture_execution->commands[i].slot : UINT32_MAX),
                  msaa_atomic_tables[i], msaa_atomic_roots[i], &producer_swizzle_uses)) return;
              if (draw.assembly.vertex_count && draw.assembly.instance_count &&
                  !EncodeMSAAProducer(scope->inflight.cmdbuf, scope->inflight, *render, draw,
                                      msaa_executions.back(), mesh_capture_execution,
                                      mesh_capture_execution ? mesh_capture_execution->commands[i].slot : UINT32_MAX)) {
                ERR("MSAA vertex capture/assembly resource allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              if (atomic_producer && !end_atomic(&producer_swizzle_uses)) return;
            }
            if (!mesh_draw.atomic64_phase &&
                !EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, producer_swizzle_uses)) {
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

        raytracing::DeserializedStructure as_deserialized;
        raytracing::AccelerationStructureHeader as_source_header{};
        raytracing::NativeDescriptor as_native;
        WMT::Reference<WMT::Buffer> as_scratch;
        bool as_rebuild_update = false;
        if (current->type == EncoderType::Raytracing) {
          auto *as = static_cast<RTASEncoderData *>(current);
          const bool ommBuild = as->operation == RTASOperation::Build && as->metadata.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
          if (ommBuild || as->operation == RTASOperation::Deserialize || as->operation == RTASOperation::Clone ||
              as->operation == RTASOperation::Compact || as->operation == RTASOperation::Serialize ||
              as->operation == RTASOperation::Visualize || as->deferredUpdate) {
            // Only the FIFO worker blocks. App-owned source data may have been written by any
            // earlier GPU command, including serialization in this same ExecuteCommandLists.
            // Read only descriptor metadata. Vertex/index/instance payloads remain GPU-owned;
            // a serialized AS may occupy a small range of a much larger application buffer.
            const auto &readLocation = ommBuild ? as->deferredInputs.ommDescs : as->sourceLocation;
            const uint64_t bytes = ommBuild ? (as->deferredInputs.ommCount ?
                uint64_t(as->deferredInputs.ommCount - 1) * as->deferredInputs.ommDescStride + 8 : 0)
                : as->operation == RTASOperation::Deserialize
                ? raytracing::kSerializedHeaderSize : as->deferredUpdate
                ? std::min(as->sourceLocation.available, raytracing::kHeaderSize +
                    uint64_t(as->shadowPlan.geometries.size()) * sizeof(raytracing::ShadowGeometry))
                : raytracing::kHeaderSize;
            WMT::Reference<WMT::Buffer> readback;
            const uint8_t *mapped = nullptr;
            uint64_t readbackCapacity = 0;
            auto read_source = [&](uint64_t offset, uint64_t length) {
              if (offset > readLocation.available || length > readLocation.available - offset) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return false;
              }
              if (length > readbackCapacity) {
                WMTBufferInfo info{};
                info.length = std::max<uint64_t>(length, raytracing::kHeaderSize);
                info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
                readback = device_->GetMTLDevice().newBuffer(info);
                mapped = static_cast<const uint8_t *>(info.memory.get_accessible_or_null());
                readbackCapacity = info.length;
                if (!readback || !mapped) { device_->ReportRemoval(E_OUTOFMEMORY); return false; }
              }
              auto blit = scope->inflight.cmdbuf.blitCommandEncoder();
              blit.waitForFence(fence_);
              wmtcmd_blit_copy_from_buffer_to_buffer copy{};
              copy.type = WMTBlitCommandCopyFromBufferToBuffer;
              copy.src = readLocation.buffer;
              copy.src_offset = readLocation.offset + offset;
              copy.dst = readback;
              copy.copy_length = length;
              blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
              blit.updateFence(fence_);
              blit.endEncoding();
              const auto producer = scope->seq;
              scope.reset();
              submission_ring_.waitUntilRetired(producer);
              if (device_->RemovedReason() != S_OK) return false;
              return start_segment();
            };
            if (bytes && !read_source(0, bytes)) return;
            if (ommBuild) {
              if (!raytracing::PlanOmmShadow(as->deferredInputs, mapped, bytes, as->shadowPlan)) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
            } else if (as->operation == RTASOperation::Deserialize) {
              D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER serialized;
              memcpy(&serialized, mapped, sizeof(serialized));
              const uint64_t minimumSize = raytracing::kSerializedHeaderSize + raytracing::kHeaderSize;
              const bool blocks = uint32_t(serialized.NumBottomLevelAccelerationStructurePointersAfterHeader >> 32) == UINT32_MAX;
              if (serialized.SerializedSizeInBytesIncludingHeader < minimumSize ||
                  serialized.SerializedSizeInBytesIncludingHeader > as->sourceLocation.available ||
                  (!blocks && serialized.NumBottomLevelAccelerationStructurePointersAfterHeader >
                      (serialized.SerializedSizeInBytesIncludingHeader - minimumSize) / 8)) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              uint64_t headerOffset = raytracing::kSerializedHeaderSize +
                  serialized.NumBottomLevelAccelerationStructurePointersAfterHeader * 8;
              if (blocks) {
                if (serialized.NumBottomLevelAccelerationStructurePointersAfterHeader != 0xffffffff00000001ull ||
                    !read_source(56, 16)) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return; }
                D3D12_RAYTRACING_SERIALIZED_BLOCK block;
                memcpy(&block, mapped, 16);
                if (block.Type != 0 || block.NumBlockPointersAfterHeader >
                    (serialized.SerializedSizeInBytesIncludingHeader - minimumSize - 16) / 8) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
                headerOffset = 72 + block.NumBlockPointersAfterHeader * 8;
              }
              if (!read_source(headerOffset, raytracing::kHeaderSize)) return;
              memcpy(&as_source_header, mapped, sizeof(as_source_header));
              const uint64_t geometryBytes = as_source_header.type != 0
                  ? uint64_t(as_source_header.descCount) * sizeof(raytracing::ShadowGeometry) : 0;
              if (geometryBytes > serialized.SerializedSizeInBytesIncludingHeader - headerOffset - raytracing::kHeaderSize) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              if (geometryBytes && !read_source(headerOffset + raytracing::kHeaderSize, geometryBytes)) return;
              if (FAILED(raytracing::ParseSerialized(serialized, as_source_header, mapped, geometryBytes,
                      serialized.SerializedSizeInBytesIncludingHeader, device_->GetMTLDevice().registryID(),
                      as->destinationLocation, as_deserialized, headerOffset))) {
                ERR("Invalid serialized acceleration structure");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              raytracing::NativeSizes sizes;
              if (FAILED(device_->QueryRTASSizes(as_deserialized.inputs, sizes)) ||
                  (as_source_header.type != 2 && !device_->AllocateRTAS(as->destination, sizes.accelerationStructureSize, as_deserialized.inputs.instanceCount)) ||
                  FAILED(raytracing::CreateNativeDescriptor(device_->GetMTLDevice(), as_deserialized.inputs,
                      as->destination.instanceBuffer(), as_native))) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              WMTBufferInfo scratch{};
              scratch.length = std::max<uint64_t>(sizes.buildScratchSize, 256);
              scratch.options = WMTResourceStorageModePrivate;
              as_scratch = device_->GetMTLDevice().newBuffer(scratch);
              if (!as_scratch) { device_->ReportRemoval(E_OUTOFMEMORY); return; }
              scope->inflight.msaa_buffers.push_back(as_scratch);
            } else {
              memcpy(&as_source_header, mapped, sizeof(as_source_header));
              if (as_source_header.magic != raytracing::kHeaderMagic || as_source_header.type > 2 ||
                  as_source_header.shadowSize > as->sourceLocation.available - raytracing::kHeaderSize) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              const uint64_t overhead = raytracing::kHeaderSize + as_source_header.shadowSize;
              if (as->deferredUpdate) {
                raytracing::StructureInfo sourceInfo{D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE(as_source_header.type),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(as_source_header.flags), as_source_header.descCount,
                    as_source_header.shadowSize, {}};
                if (as_source_header.type == 1) {
                  if (as_source_header.descCount > (bytes - raytracing::kHeaderSize) / sizeof(raytracing::ShadowGeometry) ||
                      as_source_header.descCount > as_source_header.shadowSize / sizeof(raytracing::ShadowGeometry)) {
                    device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                  }
                  sourceInfo.geometries.resize(as_source_header.descCount);
                  memcpy(sourceInfo.geometries.data(), mapped + raytracing::kHeaderSize,
                      sourceInfo.geometries.size() * sizeof(raytracing::ShadowGeometry));
                }
                const uint64_t nativeSize = std::max(as->source.size(), as->nativeCapacity);
                as_rebuild_update = as->source == as->destination && nativeSize > as->source.size();
                const bool overlaps = as->sourceLocation.buffer == as->destinationLocation.buffer &&
                    as->sourceLocation.offset != as->destinationLocation.offset &&
                    as->sourceLocation.offset < as->destinationLocation.offset + as_source_header.currentSize &&
                    as->destinationLocation.offset < as->sourceLocation.offset + as_source_header.currentSize;
                if (overlaps || as_source_header.currentSize > as->destinationLocation.available ||
                    !raytracing::IsCompatibleUpdate(sourceInfo, as->deferredInputs, as->shadowPlan) ||
                    !device_->AllocateRTAS(as->destination, nativeSize, as->instanceCount) ||
                    FAILED(raytracing::CreateNativeDescriptor(device_->GetMTLDevice(), as->deferredInputs,
                        as->destination.instanceBuffer(), as_native))) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
                if (as_rebuild_update || !as->scratch) {
                  WMTBufferInfo scratch{};
                  scratch.length = std::max<uint64_t>(as_rebuild_update ? as_native.sizes.buildScratchSize : as_native.sizes.refitScratchSize, 256);
                  scratch.options = WMTResourceStorageModePrivate;
                  as_scratch = device_->GetMTLDevice().newBuffer(scratch);
                  if (!as_scratch) { device_->ReportRemoval(E_OUTOFMEMORY); return; }
                  scope->inflight.msaa_buffers.push_back(as_scratch);
                }
              } else if (as->operation == RTASOperation::Serialize || as->operation == RTASOperation::Visualize) {
                const uint64_t ommPointers = as_source_header.type == 1 && as_source_header.reserved0
                    ? as_source_header.reserved0 - 1 : 0;
                const uint64_t required = as->operation == RTASOperation::Serialize
                    ? 56 + (as_source_header.type == 0 ? uint64_t(as_source_header.descCount) * 8
                      : ommPointers ? 16 + ommPointers * 8 : 0) + overhead
                    : 8 + as_source_header.shadowSize + (as_source_header.type == 2 ? 352 + uint64_t(as_source_header.descCount) * 8
                      : as_source_header.type == 1 && as_source_header.reserved0 ? uint64_t(as_source_header.descCount) * 80 : 0);
                if (required > as->destinationLocation.available) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
              } else {
                const uint64_t nativeSize = as->operation == RTASOperation::Compact
                    ? as_source_header.nativeCompactedSize : as->source.size();
                if ((as->operation == RTASOperation::Compact && !(as_source_header.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION)) ||
                    overhead > as->destinationLocation.available || nativeSize > as->destinationLocation.available - overhead ||
                    (as_source_header.type != 2 && !device_->AllocateRTAS(as->destination, nativeSize,
                        as_source_header.type == 0 ? as_source_header.descCount : 0))) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
              }
            }
          }
        }
        WMT::MTL4CommandBuffer cmdbuf = scope->inflight.cmdbuf;
        if (current->type != EncoderType::Null)
          owned_graphics::Trace("encode", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                                ",\"list\":", list.list_id,
                                ",\"recording\":", list.recording,
                                ",\"encoder\":", current->id, ",\"type\":", uint32_t(current->type));
        switch (current->type) {
        case EncoderType::Null:
          break;
        case EncoderType::Diagnostic: {
          const auto &annotation = static_cast<DiagnosticEncoderData *>(current)->annotation;
          ApplyAnnotation(cmdbuf, annotation);
          break;
        }
        case EncoderType::Clear: {
          auto data = static_cast<ClearEncoderData *>(current);
          std::vector<SwizzlePassResource> swizzle_uses;
          if (data->attachment)
            AppendStandardTexture(swizzle_uses, data->attachment.ptr()->allocation->descriptor, true,
                                  data->attachment.ptr()->key, data->attachment.ptr()->allocation);
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          {
            WMTRenderPassInfo info;
            WMT::InitializeRenderPassInfo(info);
            if (data->clear_dsv) {
              info.sample_positions = data->sample_positions;
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
            WMT::SetRenderTargetArrayLength(info, data->array_length);
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
          const bool atomic_render = data->atomic64_phase && !data->msaa_draw;
          std::vector<Atomic64RootBinding> atomic_render_roots;
          std::unique_ptr<std::vector<StandardSwizzleTableUse>> atomic_render_tables;
          if (atomic_render) {
            if (data->capture_indirect_pass && data->capture_indirect->root_stride) {
              const auto &indirect = *data->capture_indirect;
              for (const auto &command : capture_execution->commands)
                if (!read_render_atomic_arguments(*data, indirect.root_heap,
                    indirect.root_offset + uint64_t(command.slot) * indirect.root_stride, atomic_render_roots, atomic_render_tables)) return;
            } else if (!read_render_atomic_arguments(*data, {}, 0, atomic_render_roots, atomic_render_tables)) return;
            swizzle_uses = CollectStandardSwizzleTables(atomic_render_tables);
          }
          auto append_attachments = [&]() {
            for (const auto &color : data->colors) {
              if (color.attachment)
                AppendStandardTexture(swizzle_uses, color.attachment.ptr()->allocation->descriptor, true,
                                      color.attachment.ptr()->key, color.attachment.ptr()->allocation);
              if (color.resolve_attachment)
                AppendStandardTexture(swizzle_uses, color.resolve_attachment.ptr()->allocation->descriptor, true,
                                      color.resolve_attachment.ptr()->key, color.resolve_attachment.ptr()->allocation);
            }
          };
          append_attachments();
          if (atomic_render) {
            const uint64_t participants = producer_participants(*data, nullptr,
                data->capture_indirect_pass ? &*capture_execution : nullptr);
            if (!begin_atomic(participants, atomic_render_tables, atomic_render_roots, &swizzle_uses)) return;
            cmdbuf = scope->inflight.cmdbuf;
          }
          if (!data->atomic64_phase && !BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          const auto &so_draws = indirect_execution ? indirect_execution->draws
                                 : data->capture_indirect_pass &&
                                           (data->capture_indirect->geometry || data->capture_indirect->domain_only ||
                                            data->capture_indirect_pass == 2)
                                       ? capture_execution->so_draws
                                                                 : data->so_draws;
          if (data->msaa_draw) {
            for (uint32_t i = 0; i < msaa_executions.size(); i++) {
              if (!msaa_executions[i].max_primitives) continue;
              if (msaa_draws[i].atomic64_phase) {
                swizzle_uses = CollectStandardSwizzleTables(msaa_atomic_tables[i]);
                append_attachments();
                if (!begin_atomic(16384, msaa_atomic_tables[i], msaa_atomic_roots[i], &swizzle_uses)) return;
                cmdbuf = scope->inflight.cmdbuf;
              }
              if (!EncodeMSAARender(cmdbuf, scope->inflight, *data,
                                    msaa_draws[i], msaa_executions[i])) {
                ERR("MSAA tile execution resource allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              if (msaa_draws[i].atomic64_phase) {
                if (!end_atomic(&swizzle_uses)) return;
                cmdbuf = scope->inflight.cmdbuf;
              }
            }
            if (data->msaa_draw->needs_stream_assembly)
              for (const auto &captured : so_draws)
                EncodeStreamOutputAssembly(cmdbuf, *captured);
            if (!data->msaa_draw->atomic64_phase && !EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
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
            render_pass_info.sample_positions = data->sample_positions;
            WMT::SetRenderTargetArrayLength(render_pass_info, data->render_target_array_length);
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
          for (const auto &use : swizzle_uses) {
            if (!use.cast_view) continue;
            const auto usage = use.writes ? WMTResourceUsageRead | WMTResourceUsageWrite : WMTResourceUsageRead;
            if (use.cast_view->castBlockSize)
              encoder.useResource(use.cast_view->texture, usage, WMTRenderStageVertex | WMTRenderStageFragment | WMTRenderStageObject | WMTRenderStageMesh);
            if (use.cast_view->msaaSampleAddress) {
              encoder.useResource(use.cast_view->msaaSampleIds, WMTResourceUsageRead,
                  WMTRenderStageVertex | WMTRenderStageFragment | WMTRenderStageObject | WMTRenderStageMesh);
              for (uint32_t sample = 0; sample < use.cast_view->allocation->descriptor->sampleCount(); ++sample)
                encoder.useResource(use.cast_view->msaaViews[sample], usage,
                    WMTRenderStageVertex | WMTRenderStageFragment | WMTRenderStageObject | WMTRenderStageMesh);
            }
          }
          // Object/mesh (GS/HS/DS) functions read resources earlier writers produced.
          const bool mesh_pass = data->use_geometry || data->use_tessellation;
          encoder.waitForFence(fence_, mesh_pass ? WMTRenderStagePreRaster : WMTRenderStageVertex);
          for (uint32_t i = 0; i < data->rov_marker_sample_count; ++i) {
            encoder.setFragmentTexture(rov_markers[i], 31 + i);
            encoder.useResource(rov_markers[i], WMTResourceUsageRead | WMTResourceUsageWrite,
                                WMTRenderStageFragment);
          }
          encoder.encodeCommands(&data->cmd_head);
          if (data->capture_indirect_pass)
            EncodeMeshCaptureDraws(encoder, *capture_execution, data->capture_indirect_pass);
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
            dispatch.vertex_count = execution.count;
            dispatch.instance_count = 1;
            // The resolver writes the ICB and its root/VB arguments in the vertex
            // stage. Metal 4 requires device visibility before the ICB consumes them.
            wmtcmd_render_memory_barrier resolved{};
            resolved.type = WMTRenderCommandMemoryBarrier;
            resolved.scope = WMTBarrierScopeBuffers;
            resolved.stages_after = WMTRenderStageVertex;
            resolved.stages_before = WMTRenderStageVertex;
            wmtcmd_render_setpso drawing{};
            drawing.type = WMTRenderCommandSetPSO;
            drawing.pso = data->so_indirect->draw_pipeline;
            wmtcmd_render_set_vertex_amplification mapping{};
            mapping.type = WMTRenderCommandSetVertexAmplification;
            // An ICB view variant owns both offsets in its shader epilogue.
            mapping.viewport_offset = 0;
            mapping.render_target_array_offset = 0;
            wmtcmd_render_executecommands execute{};
            execute.type = WMTRenderCommandExecuteCommandsInBuffer;
            execute.indirect_command_buffer = execution.icb;
            execute.location = 0;
            execute.length = execution.count;
            head.next.set(&use_icb);
            use_icb.next.set(&resolver);
            resolver.next.set(&argument);
            argument.next.set(&dispatch);
            dispatch.next.set(&resolved);
            resolved.next.set(&drawing);
            drawing.next.set(data->so_indirect->shader_view_locations ? static_cast<void *>(&mapping)
                                                                    : static_cast<void *>(&execute));
            mapping.next.set(&execute);
            execute.next.set(nullptr);
            encoder.useResource(execution.storage, WMTResourceUsageRead | WMTResourceUsageWrite,
                                WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment));
            if (data->so_indirect->static_sampler_buffer)
              encoder.useResource(data->so_indirect->static_sampler_buffer, WMTResourceUsageRead,
                                  WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment));
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
          if (atomic_render) {
            if (!end_atomic(&swizzle_uses)) return;
            cmdbuf = scope->inflight.cmdbuf;
          } else if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::Raytracing: {
                  auto *data = static_cast<RTASEncoderData *>(current);
                  const auto operation = data->operation;
                  const bool build = operation == RTASOperation::Build || operation == RTASOperation::Refit;
                  const bool copy = operation == RTASOperation::Clone || operation == RTASOperation::Compact;
                  const bool deserialize = operation == RTASOperation::Deserialize;
                  auto fail = [&] { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); };
                  if (operation == RTASOperation::Serialize) {
                    raytracing::SerializeParams params{data->sourceLocation.address, data->destinationLocation.address,
                        raytracing::DriverMatchingIdentifier(device_->GetMTLDevice().registryID())};
                    if (!EncodeASHelper(cmdbuf, data->pipelines.serialize, &params, sizeof(params))) fail();
                    break;
                  }
                  if (operation == RTASOperation::Visualize) {
                    const uint64_t params[] = {data->sourceLocation.address, data->destinationLocation.address};
                    if (!EncodeASHelper(cmdbuf, data->pipelines.visualize, params, sizeof(params)) ||
                        !EncodeASHelper(cmdbuf, data->pipelines.visualizeDescriptors, params, sizeof(params))) fail();
                    break;
                  }
                  if (deserialize) {
                    raytracing::DeserializeParams params{data->sourceLocation.address, data->destinationLocation.address,
                        as_deserialized.deserializedSize};
                    if (!EncodeASHelper(cmdbuf, data->pipelines.deserialize, &params, sizeof(params))) { fail(); break; }
                  }
                  if (build || copy) {
                    raytracing::ShadowParams params{data->destinationLocation.address, data->sourceLocation.address,
                        uint32_t(data->shadowPlan.jobs.size()), copy ? raytracing::kShadowMetadataCopy : raytracing::kShadowMetadataParams,
                        data->metadata};
            if (operation == RTASOperation::Refit) {
              params.metadata.currentSize = as_source_header.currentSize;
              params.metadata.deserializedSize = as_source_header.deserializedSize;
            }
                    if (copy) {
                      // Copy mode does not consume geometry/job tables; bind an existing buffer
                      // of the required reflected size instead of allocating empty tables.
                      if (!EncodeASHelper(cmdbuf, data->pipelines.shadow, &params, sizeof(params),
                              data->sourceLocation.buffer, data->sourceLocation.buffer)) { fail(); break; }
                    } else {
                      const uint64_t geometryBytes = data->shadowPlan.geometries.size() * sizeof(raytracing::ShadowGeometry);
                      const uint64_t jobsBytes = data->shadowPlan.jobs.size() * sizeof(raytracing::ShadowJob);
                      const uint64_t jobsOffset = std::max<uint64_t>(geometryBytes, sizeof(raytracing::ShadowGeometry));
                      WMTBufferInfo info{};
                      info.length = jobsOffset + std::max<uint64_t>(jobsBytes, sizeof(raytracing::ShadowJob));
                      info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
                      auto tables = device_->GetMTLDevice().newBuffer(info);
                      auto *mapped = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
                      if (!tables || !mapped) { fail(); break; }
                      if (geometryBytes) memcpy(mapped, data->shadowPlan.geometries.data(), geometryBytes);
                      if (jobsBytes) memcpy(mapped + jobsOffset, data->shadowPlan.jobs.data(), jobsBytes);
                      scope->inflight.msaa_buffers.push_back(tables);
                      if (!EncodeASHelper(cmdbuf, data->pipelines.shadow, &params, sizeof(params), tables, tables, jobsOffset)) { fail(); break; }
                    }
                  }
                  if (build || copy || deserialize) {
                    const uint32_t instanceCount = build ? data->instanceCount : uint32_t(data->destination.instanceCount());
                    const auto instances = build ? WMT::Buffer(data->sourceInstances)
                        : deserialize ? data->destinationLocation.buffer : data->sourceLocation.buffer;
                    const uint64_t instanceOffset = build ? data->sourceInstancesOffset
                        : (deserialize ? data->destinationLocation.offset : data->sourceLocation.offset) + raytracing::kHeaderSize;
                    auto compute = cmdbuf.computeCommandEncoder(false);
                    compute.waitForFence(fence_);
                    WMTASPrepassInfo info{data->destination.resourceID(), data->destination.contributionGPUAddress(), 0,
                        raytracing::kD3DInstanceDescriptorSize, raytracing::kMetalIndirectInstanceDescriptorSize,
                        instanceCount, instanceCount ? (build ? data->instanceLayout : 1u) : 0u};
                    info.version_token = data->versionToken;
                    info.live_mirror_slot = uint32_t(data->liveMirrorOffset / sizeof(uint32_t));
                    const bool prepass = compute.encodeASPrepass(data->pipelines.prepass, instances, instanceOffset,
                        data->destination.instanceBuffer(), 0, data->header, data->destinationOffset,
                        data->destination.contributionBuffer(), 0, data->liveMirror, 0, info);
                    compute.updateFence(fence_);
                    compute.endEncoding();
                    if (!prepass) { fail(); break; }
                    scope->inflight.rtas_observations.push_back({data->destinationKey, data->versionToken});
                    device_->HoldRTASLease(data->destinationKey, data->versionToken);
                    auto encoder = cmdbuf.accelerationStructureCommandEncoder();
                    encoder.waitForFence(fence_);
                    const bool ommArray = build ? data->metadata.type == 2 : as_source_header.type == 2;
                    const bool encoded = ommArray ? true : deserialize || as_rebuild_update ? encoder.build(as_native.descriptor, data->destination, as_scratch, 0)
                        : operation == RTASOperation::Build ? encoder.build(data->descriptor, data->destination, data->scratch, data->scratchOffset)
                        : operation == RTASOperation::Refit ? encoder.refit(data->deferredUpdate ? WMT::MTL4AccelerationStructureDescriptor(as_native.descriptor) : WMT::MTL4AccelerationStructureDescriptor(data->descriptor),
                          data->source, data->destination, as_scratch ? WMT::Buffer(as_scratch) : WMT::Buffer(data->scratch), as_scratch ? 0 : data->scratchOffset)
                        : encoder.copy(data->source, data->destination, operation == RTASOperation::Compact ? 1 : 0);
                    // Every header carries the native compacted size, so standalone postbuild and compact
                    // copies read exactly the same GPU-produced value as inline postbuild queries.
                    if (!ommArray) encoder.compactedSize(data->destination, data->header, data->destinationOffset + 48);
                    encoder.updateFence(fence_);
                    encoder.endEncoding();
                    if (!encoded) { fail(); break; }
                    // Metal writes an eight-byte compacted size at offset48,
                    // clearing the adjacent OMM marker. Restore it on the same
                    // GPU timeline before copies, serialization or postbuild.
                    if (!ommArray && (build ? data->metadata.reserved0 : as_source_header.reserved0)) {
                      raytracing::PostbuildParams params{data->destinationLocation.address, 0, raytracing::kPostbuildRestoreOmmHeader, 0};
                      if (!EncodeASHelper(cmdbuf, data->pipelines.postbuild, &params, sizeof(params))) { fail(); break; }
                    }
                    if (operation == RTASOperation::Compact) {
                      raytracing::PostbuildParams params{data->destinationLocation.address, 0, raytracing::kPostbuildFinalizeCompactedHeader, 0};
                      if (!EncodeASHelper(cmdbuf, data->pipelines.postbuild, &params, sizeof(params))) { fail(); break; }
                    }
                  }
                  for (const auto &entry : data->postbuild) {
                    raytracing::PostbuildParams params{operation == RTASOperation::Emit ? data->sourceLocation.address : data->destinationLocation.address,
                        entry.address, uint32_t(entry.type), 0};
                    if (!EncodeASHelper(cmdbuf, data->pipelines.postbuild, &params, sizeof(params))) { fail(); break; }
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
          const bool msaa = data->resource->texture && data->resource->texture->sampleCount() > 1;
          const bool encoded = msaa
              ? EncodeWritableMSAA(cmdbuf, scope->inflight, data->resource->texture.ptr(), data->to_canonical)
              : EncodeStandardSwizzle(cmdbuf, scope->inflight, data->resource, data->to_canonical);
          if (!encoded) {
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
          for (const auto &use : data->cast_textures)
            AppendStandardTexture(swizzle_uses, use.texture.ptr(), use.writes, use.view, use.allocation.ptr());
          if (data->atomic64_phase) {
            uint64_t participants = 0;
            WMTSize threads{1, 1, 1};
            auto add_grid = [&](WMTSize groups, WMTSize group_threads) -> bool {
              uint64_t count = 1;
              for (const auto dimension : {groups.width, groups.height, groups.depth,
                                           group_threads.width, group_threads.height, group_threads.depth}) {
                if (dimension && count > UINT64_MAX / dimension) { device_->ReportRemoval(E_OUTOFMEMORY); return false; }
                count *= dimension;
              }
              if (participants > UINT64_MAX - count) { device_->ReportRemoval(E_OUTOFMEMORY); return false; }
              participants += count; return true;
            };
            for (auto *cmd = reinterpret_cast<const wmtcmd_base *>(&data->cmd_head); cmd;
                 cmd = static_cast<const wmtcmd_base *>(cmd->next.get_accessible_or_null())) {
              if (cmd->type == WMTComputeCommandSetPSO)
                threads = reinterpret_cast<const wmtcmd_compute_setpso *>(cmd)->threadgroup_size;
              else if (cmd->type == WMTComputeCommandDispatch) {
                if (!add_grid(reinterpret_cast<const wmtcmd_compute_dispatch *>(cmd)->size, threads)) return;
              } else if (cmd->type == WMTComputeCommandDispatchThreads) {
                auto size = reinterpret_cast<const wmtcmd_compute_dispatch *>(cmd)->size;
                WMTSize groups{(size.width + threads.width - 1) / threads.width,
                               (size.height + threads.height - 1) / threads.height,
                               (size.depth + threads.depth - 1) / threads.depth};
                if (!add_grid(groups, threads)) return;
              } else if (cmd->type == WMTComputeCommandDispatchIndirect) {
                const auto *indirect = reinterpret_cast<const wmtcmd_compute_dispatch_indirect *>(cmd);
                AtomicReadback arguments;
                if (!read_atomic_buffer(WMT::Buffer{indirect->indirect_args_buffer}, indirect->indirect_args_offset,
                                        sizeof(D3D12_DISPATCH_ARGUMENTS), arguments)) return;
                D3D12_DISPATCH_ARGUMENTS groups; memcpy(&groups, arguments.bytes, sizeof(groups));
                if (!add_grid({groups.ThreadGroupCountX, groups.ThreadGroupCountY, groups.ThreadGroupCountZ}, threads)) return;
              }
            }
            std::vector<Atomic64RootBinding> resolved_roots;
            if (!data->atomic64_indirect.empty() || data->atomic64_ray) resolved_roots = data->atomic64_roots;
            for (const auto &use : data->atomic64_indirect) {
              const auto &command = *use.command;
              uint32_t count = command.max_count;
              if (command.max_count_buffer) {
                AtomicReadback count_data; if (!read_atomic_address(command.max_count_buffer, sizeof(count), count_data)) return;
                memcpy(&count, count_data.bytes, sizeof(count)); count = std::min<uint64_t>(count, command.max_count);
              }
              if (use.predicate_buffer) {
                AtomicReadback predicate;
                if (!read_atomic_buffer(use.predicate_buffer, use.predicate_offset, sizeof(uint64_t), predicate)) return;
                uint64_t value; memcpy(&value, predicate.bytes, sizeof(value));
                if (command.predicate_op ? value != 0 : value == 0) count = 0;
              }
              if (!count) continue;
              AtomicReadback records;
              if (!read_atomic_address(command.argument_buffer, uint64_t(count) * use.stride, records)) return;
              for (uint32_t slot = 0; slot < count; ++slot) {
                const auto *record = records.bytes + uint64_t(slot) * use.stride;
                D3D12_DISPATCH_ARGUMENTS groups; memcpy(&groups, record + use.dispatch_offset, sizeof(groups));
                if (!add_grid({groups.ThreadGroupCountX, groups.ThreadGroupCountY, groups.ThreadGroupCountZ},
                              {command.tgsize_x, command.tgsize_y, command.tgsize_z})) return;
                for (const auto &field : use.root_fields) {
                  uint64_t address; memcpy(&address, record + field.argument_offset, sizeof(address));
                  if (address) resolved_roots.push_back({address, UINT64_MAX, field.writable});
                }
              }
            }
            std::unique_ptr<std::vector<StandardSwizzleTableUse>> resolved_tables;
            if (data->atomic64_ray) {
              const auto &use = *data->atomic64_ray;
              auto *state = static_cast<MTLD3D12StateObject *>(use.state_object.ptr());
              auto *heap = static_cast<MTLD3D12DescriptorHeap *>(use.heap.ptr());
              auto *global = state->GetDispatchRootSignature();
              if (global && global->ParameterQwords) {
                AtomicReadback arguments;
                if (!read_atomic_buffer(use.roots, use.roots_offset, uint64_t(global->ParameterQwords) * 8, arguments)) return;
                capture_atomic_arguments(global, heap, arguments.bytes, resolved_roots, resolved_tables);
              } else capture_atomic_arguments(global, heap, nullptr, resolved_roots, resolved_tables);
              AtomicReadback dispatch_data;
              if (!read_atomic_buffer(use.arguments, use.arguments_offset, sizeof(SM50_RT_DISPATCH_ARGUMENTS), dispatch_data)) return;
              SM50_RT_DISPATCH_ARGUMENTS arguments; memcpy(&arguments, dispatch_data.bytes, sizeof(arguments));
              auto capture_records = [&](uint64_t base, uint32_t stride, uint32_t count) -> bool {
                if (!base || !count) return true;
                const uint64_t record_bytes = stride ? stride : sizeof(OwnedShaderIdentifier);
                if (record_bytes < sizeof(OwnedShaderIdentifier)) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return false; }
                AtomicReadback records;
                if (!read_atomic_address(base, uint64_t(count) * record_bytes, records)) return false;
                for (uint32_t slot = 0; slot < count; ++slot) {
                  const auto *record = records.bytes + uint64_t(slot) * record_bytes;
                  OwnedShaderIdentifier identifier; memcpy(&identifier, record, sizeof(identifier));
                  if (identifier.magic != kOwnedShaderIdentifierMagic || identifier.version != kOwnedShaderIdentifierVersion ||
                      identifier.kind != kOwnedShaderIdentifierPipeline || identifier.pipeline_token != arguments.PipelineToken) continue;
                  const auto *exported = state->FindExport(identifier.export_index);
                  if (!exported || !exported->local_root_signature) continue;
                  auto *local = exported->local_root_signature;
                  const uint64_t local_bytes = uint64_t(local->ParameterQwords) * 8;
                  if (local_bytes <= record_bytes - sizeof(identifier))
                    capture_atomic_arguments(local, heap, record + sizeof(identifier), resolved_roots, resolved_tables);
                  else {
                    AtomicReadback local_arguments;
                    if (!read_atomic_address(base + uint64_t(slot) * stride + sizeof(identifier), local_bytes, local_arguments)) return false;
                    capture_atomic_arguments(local, heap, local_arguments.bytes, resolved_roots, resolved_tables);
                  }
                }
                return true;
              };
              // The prefix has already executed: tables and root copies are the
              // actual GPU-produced values, not the recording-time arguments.
              if (!capture_records(arguments.RayGenerationRecord, 0, 1) ||
                  !capture_records(arguments.MissRecordBase, arguments.MissRecordStride, arguments.MissRecordCount) ||
                  !capture_records(arguments.HitGroupRecordBase, arguments.HitGroupRecordStride, arguments.HitGroupRecordCount) ||
                  !capture_records(arguments.CallableRecordBase, arguments.CallableRecordStride, arguments.CallableRecordCount)) return;
              swizzle_uses = CollectStandardSwizzleTables(resolved_tables);
            }
            const auto &roots = data->atomic64_indirect.empty() && !data->atomic64_ray ? data->atomic64_roots : resolved_roots;
            const auto &tables = data->atomic64_ray ? resolved_tables : data->swizzle_tables;
            if (!begin_atomic(participants, tables, roots, &swizzle_uses)) return;
            cmdbuf = scope->inflight.cmdbuf;
          }
          if (!data->atomic64_phase && !BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(fence_);
          for (const auto &use : swizzle_uses) {
            if (!use.cast_view) continue;
            wmtcmd_compute_useresource declaration{};
            declaration.type = WMTComputeCommandUseResource;
            declaration.usage = use.writes ? WMTResourceUsageRead | WMTResourceUsageWrite : WMTResourceUsageRead;
            if (use.cast_view->castBlockSize) {
              declaration.resource = use.cast_view->texture;
              encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&declaration));
            }
            if (use.cast_view->msaaSampleAddress) {
              declaration.resource = use.cast_view->msaaSampleIds;
              declaration.usage = WMTResourceUsageRead;
              encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&declaration));
              declaration.usage = use.writes ? WMTResourceUsageRead | WMTResourceUsageWrite : WMTResourceUsageRead;
              for (uint32_t sample = 0; sample < use.cast_view->allocation->descriptor->sampleCount(); ++sample) {
                declaration.resource = use.cast_view->msaaViews[sample];
                encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&declaration));
              }
            }
          }
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(fence_);
          encoder.endEncoding();
          if (data->atomic64_phase) {
            if (!end_atomic(&swizzle_uses)) return;
          } else if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::WorkGraph:
        case EncoderType::FenceBarrier:
          break; // Executed at the submission segment boundary above.
        case EncoderType::Temporal: {
          auto data = static_cast<TemporalEncoderData *>(current);
          if (data->fg_frame && data->fg_stage == TemporalEncoderData::FrameGenerationStage::Begin) {
            // Release every submission/device lock before the backend waits
            // for a committed producer's validated GPU completion.
            const auto preceding_submission = scope->seq;
            scope.reset();
            const bool execute = BeginOwnedFrameGenerationFrame(data->fg_frame, owned_id_, preceding_submission);
            frame_generation.active = data->fg_frame;
            if (!execute) {
              // Replay/stale packets must skip copies as well as kernels.
              // A native split callback is not a D3D12 nested-list scope.
              auto *end = data->fg_end;
              if (!end) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              current = end;
            }
            if (!start_segment()) return;
            if (!execute) {
              scope->inflight.frame_generation_completions.push_back(data->fg_frame);
              scope.reset();
              frame_generation.active.reset();
              if (!start_segment()) return;
            }
            break;
          }
          if (data->fg_frame && data->fg_stage == TemporalEncoderData::FrameGenerationStage::End) {
            // This segment follows every SDK pass (and its GPU counter
            // readback); only the completion worker may settle the result.
            scope->inflight.frame_generation_completions.push_back(data->fg_frame);
            scope.reset();
            frame_generation.active.reset();
            if (!start_segment()) return;
            break;
          }
          // History waits precede this feature commit, not application work
          // already encoded into the prior segment.
          scope.reset();
          if (!start_segment())
            return;
          auto temporal_cmdbuf = scope->inflight.cmdbuf;
          scope->inflight.temporal_frames.push_back(data->frame);
          std::vector<SwizzlePassResource> swizzle_uses;
          if (device_->HasStandardSwizzleTextures()) {
            const auto resources = GetOwnedTemporalFrameResources(data->frame);
            for (size_t i = 0; i < resources.size(); ++i)
              if (resources[i] && resources[i]->swizzle_layout)
                AppendStandardResource(swizzle_uses, Com<MTLD3D12Resource, false>(resources[i]), i == 3);
          }
          if (!BeginStandardSwizzlePass(temporal_cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          const auto wait = [&](WMT::Event event, uint64_t value) { queue_.waitForEvent(event, value); };
          const auto signal = [&](WMT::Event event, uint64_t value) {
            scope->signal_event = event;
            scope->signal_value = value;
            scope.reset();
          };
          EncodeOwnedTemporalFrame(data->frame, temporal_cmdbuf, fence_, owned_id_, scope->seq, current->id, wait, signal);
          // Reacquire the device submission lock only after the feature mutex
          // has been released, preserving device-lock then feature-lock order.
          if (!start_segment())
            return;
          if (!EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::Resolve: {
          auto data = static_cast<ResolveEncoderData *>(current);
          std::vector<SwizzlePassResource> swizzle_uses;
          {
            if (data->src)
              AppendStandardTexture(swizzle_uses, data->src.ptr()->allocation->descriptor, false,
                                    data->src.ptr()->key, data->src.ptr()->allocation);
            if (data->dst)
              AppendStandardTexture(swizzle_uses, data->dst.ptr()->allocation->descriptor, true,
                                    data->dst.ptr()->key, data->dst.ptr()->allocation);
          }
          if (!BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }

          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          if (data->pipeline) {
            if (data->dsv_flags & 1) {
              info.sample_positions = data->sample_positions;
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
            wmtcmd_render_setscissorrect scissor{};
            scissor.type = WMTRenderCommandSetScissorRect;
            scissor.scissor_rect = {data->destination_x, data->destination_y, data->width, data->height};
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&scissor));
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
            scope.reset();
            EncodeAliasFlush();
            reserved_volume_publish_pending_ = false;
            if (!start_segment())
              return;
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
            fill.length = data->record_bytes;
            fill.value = 0;
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
            encoder.updateFence(fence_);
            encoder.endEncoding();
            break;
          }
          case QueryEncoderOp::SampleTimestamp:
            // writeTimestamp waits only for earlier work in this command buffer.
            // Order all already-committed queue work before this segment too;
            // the current buffer is committed by scope after this GPU-only pair.
            // One pair covers every sample in the segment, including an initial
            // timestamp with no encoder/fence dependency of its own.
            if (!scope->timestamp_predecessors_ordered) {
              const uint64_t value = ++timestamp_order_value_;
              queue_.signalEvent(timestamp_order_event_, value);
              queue_.waitForEvent(timestamp_order_event_, value);
              scope->timestamp_predecessors_ordered = true;
            }
            WMT::writeTimestamp(cmdbuf, data->timestamp_samples, data->start_index);
            break;
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
                                    : uint64_t(data->start_index) * data->record_bytes;
            copy.dst = begin ? data->source : data->destination;
            copy.dst_offset = begin ? data->snapshot_offset : data->destination_offset;
            copy.copy_length = uint64_t(data->query_count) * data->record_bytes;
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
            set_result.offset = uint64_t(data->start_index) * data->record_bytes;
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
            auto barrier = cmdbuf.blitCommandEncoder();
            barrier.barrierAfterQueueStages(WMTMetal4StageAll, WMTMetal4StageBlit, WMTMetal4VisibilityDevice);
            barrier.updateFence(fence_);
            barrier.endEncoding();
            WMT::resolveTimestampCounters(cmdbuf, data->timestamp_samples, data->start_index, data->query_count,
                                          data->destination, data->destination_offset, fence_, fence_);
            break;
          }
          }
          break;
        }
        }
        current = current->next;
      }
      scope->inflight.breadcrumbs.push_back(list.breadcrumb);
    }
    if (fenced && !task.lists.empty()) {
      const auto &last = task.lists.back();
      const auto count = static_cast<RecordingHead *>(last.entry)->state_events.size();
      if (!validate_transitions(&last, count, count, true)) return;
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
      task.reflex_work = device_->reflex.Capture(false);
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
        auto &submission = task.lists.emplace_back();
        submission.entry = list->entry; submission.list_id = list->owned_id; submission.recording = list->owned_recording;
        submission.allocator = std::move(lease); submission.recording_lease = std::move(recording);
        std::vector<D3D12_AUTO_BREADCRUMB_OP> history{D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION};
        std::vector<std::pair<UINT, std::string>> contexts;
        for (auto *encoder = list->entry; encoder; encoder = encoder->next) {
          if (encoder->type == EncoderType::Diagnostic) {
            const auto &annotation = static_cast<DiagnosticEncoderData *>(encoder)->annotation;
            if (annotation.action != 2) contexts.emplace_back(history.size(), annotation.label);
            history.push_back(static_cast<D3D12_AUTO_BREADCRUMB_OP>(annotation.action));
          } else {
            const wmtcmd_base *command = nullptr;
            if (encoder->type == EncoderType::Render)
              command = reinterpret_cast<const wmtcmd_base *>(&static_cast<RenderEncoderData *>(encoder)->cmd_head);
            if (encoder->type == EncoderType::Compute)
              command = reinterpret_cast<const wmtcmd_base *>(&static_cast<ComputeEncoderData *>(encoder)->cmd_head);
            if (encoder->type == EncoderType::Blit)
              command = reinterpret_cast<const wmtcmd_base *>(&static_cast<BlitEncoderData *>(encoder)->cmd_head);
            for (; command; command = static_cast<const wmtcmd_base *>(command->next.get())) {
              std::optional<D3D12_AUTO_BREADCRUMB_OP> op;
              if (encoder->type == EncoderType::Render) {
                switch (command->type) {
                case WMTRenderCommandDraw: op = D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED; break;
                case WMTRenderCommandDrawIndexed: op = D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED; break;
                case WMTRenderCommandDrawIndirect:
                case WMTRenderCommandDrawIndexedIndirect: op = D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT; break;
                case WMTRenderCommandDrawMeshThreadgroups:
                case WMTRenderCommandDrawMeshThreadgroupsIndirect: op = D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH; break;
                default: break;
                }
              } else if (encoder->type == EncoderType::Compute) {
                switch (command->type) {
                case WMTComputeCommandDispatch:
                case WMTComputeCommandDispatchThreads: op = D3D12_AUTO_BREADCRUMB_OP_DISPATCH; break;
                case WMTComputeCommandDispatchIndirect: op = D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT; break;
                default: break;
                }
              } else {
                switch (command->type) {
                case WMTBlitCommandCopyFromBufferToBuffer: op = D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION; break;
                case WMTBlitCommandCopyFromBufferToTexture:
                case WMTBlitCommandCopyFromBufferToTextureWithBlitOption:
                case WMTBlitCommandCopyFromTextureToBuffer:
                case WMTBlitCommandCopyFromTextureToBufferWithBlitOption:
                case WMTBlitCommandCopyFromTextureToTexture: op = D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION; break;
                case WMTBlitCommandCopyTexture: op = D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE; break;
                default: break;
                }
              }
              if (op) history.push_back(*op);
            }
            if (encoder->type == EncoderType::Clear) {
              const auto *clear = static_cast<ClearEncoderData *>(encoder);
              history.push_back(clear->clear_dsv ? D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW : D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW);
            }
            if (encoder->type == EncoderType::Resolve) history.push_back(D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE);
            if (encoder->type == EncoderType::FenceBarrier) history.push_back(D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER);
          }
        }
        history.push_back(D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION);
        submission.breadcrumb = device_->diagnostics.BeginSubmission(list, this, std::move(history), contexts);
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
      // Capture generations at API enqueue time, before a worker can block.
      // Every replay owns fresh tickets; canceled tasks unregister wait latches.
      for (auto &list : task.lists) {
        size_t count = 0;
        for (auto *encoder = list.entry; encoder; encoder = encoder->next)
          if (encoder->type == EncoderType::FenceBarrier) count += static_cast<FenceBarrierEncoderData *>(encoder)->operands.size();
        list.fence_operations.reserve(count);
        for (auto *encoder = list.entry; encoder; encoder = encoder->next) {
          if (encoder->type != EncoderType::FenceBarrier) continue;
          const auto *barrier = static_cast<FenceBarrierEncoderData *>(encoder);
          for (const auto &operand : barrier->operands) {
            auto *native = static_cast<MTLD3D12Fence *>(operand.fence.ptr());
            auto &operation = list.fence_operations.emplace_back();
            operation.fence = native->fence; operation.fence_id = native->owned_id; operation.signal = barrier->signal;
            operation.ticket = barrier->signal ? native->fence->prepareQueueSignal(operand.value) : native->fence->prepareQueueWait(operand.value);
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

  void EnqueueAnnotation(UINT action, UINT metadata, const void *data, UINT size) {
    try {
      auto annotation = CaptureAnnotation(action, metadata, data, size);
      std::lock_guard<dxmt::mutex> lock(mutex_enqueue_);
      if (device_->RemovedReason() != S_OK) return;
      auto breadcrumb = device_->diagnostics.BeginSubmission(nullptr, this, {D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION,
          static_cast<D3D12_AUTO_BREADCRUMB_OP>(action), D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION}, {{1, annotation.label}});
      if (!task_fifo_.push(QueueTask{MarkerTask{std::move(annotation), std::move(breadcrumb)}}))
        device_->diagnostics.Validation(D3D12_MESSAGE_ID_UNKNOWN, "Queue annotation submission worker is closed");
    } catch (const std::invalid_argument &) {
      device_->diagnostics.Validation(D3D12_MESSAGE_ID_CORRUPTED_PARAMETER2, "Invalid queue annotation payload");
    } catch (const std::bad_alloc &) { device_->ReportRemoval(E_OUTOFMEMORY); }
  }
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) { EnqueueAnnotation(0, metadata, data, size); }
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) { EnqueueAnnotation(1, metadata, data, size); }
  void STDMETHODCALLTYPE EndEvent() { EnqueueAnnotation(2, 0, nullptr, 0); }

  void
  ProcessSignal(SignalTask &task) {
    if (device_->RemovedReason() != S_OK)
      return;
    uint64_t submission = 0;
    try {
      {
        auto scope = StartCommitting();
        submission = scope.seq;
        // Publish logical signals only on validated retirement. Reserving a
        // shared native token here would confuse enqueue and completion order.
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

    // The latch requires actual validated completion, including remote producers.
    // This FIFO worker gate has no GPU work and needs no empty native submission.
    owned_graphics::Trace("wait", ",\"queue\":", owned_id_, ",\"fence\":", task.fence_id,
                          ",\"value\":", task.ticket.value, ",\"generation\":", task.ticket.generation);
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
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        desc_.Type != D3D12_COMMAND_LIST_TYPE_COPY)
      return E_FAIL;
    *pFrequency = WMT::queryTimestampFrequency(device_->GetMTLDevice());
    return *pFrequency ? S_OK : E_FAIL;
  };

  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    if (!gpu_timestamp || !cpu_timestamp)
      return E_INVALIDARG;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        desc_.Type != D3D12_COMMAND_LIST_TYPE_COPY)
      return E_FAIL;

    LARGE_INTEGER qpc_before{}, qpc_after{};
    uint64_t metal_cpu_timestamp = 0;
    uint64_t metal_gpu_timestamp = 0;
    if (!QueryPerformanceCounter(&qpc_before))
      return E_FAIL;
    if (!WMT::sampleTimestampCalibration(device_->GetMTLDevice(), metal_cpu_timestamp, metal_gpu_timestamp))
      return E_FAIL;
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
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain, nullptr, nullptr,
                                DXMTSwapChainKind::Hwnd);
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainWithOutput(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput,
      IDXGISwapChain1 **ppSwapChain, IUnknown *core_window, DXMTSwapChainKind kind
  ) override {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain,
                                 pRestrictToOutput, core_window, kind);
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
      if (task.composition.surface) {
        try {
          task.presenter->retireNativePresent(task.state->frame_id,
              [presenter = task.presenter, frame_id = task.state->frame_id] {
                if (presenter->presentOrderEvent().signaledValue() != UINT64_MAX)
                  presenter->skipQueuedPresent(frame_id);
              });
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
          CancelPresent(task);
          return;
        }
      } else
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
    // No submission ticket may span a wait on an unsubmitted producer. A
    // predecessor present can itself follow an exclusive atomic phase on another queue.
    auto order_event = task.presenter->presentOrderEvent();
    const uint64_t frame_id = task.state->frame_id;
    while (!order_event.waitUntilSignaledValue(frame_id - 1, 100)) {
      if (stopping_.load(std::memory_order_acquire) || device_->RemovedReason() != S_OK) {
        CancelPresent(task);
        return;
      }
    }
    auto scope = StartCommitting();
    if (device_->RemovedReason() != S_OK) {
      CancelPresent(task);
      return;
    }
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.semaphore = task.completion_handle.release();
    scope.inflight.reflex_work = task.reflex_work;
    if (task.reflex_work) scope.inflight.reflex_driver_start = ReflexState::NowUs();
    scope.inflight.latency = std::move(task.latency);

    auto g = task.backbuffer.ptr();
    auto &view = g->texture->view(g->texture->fullView);

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
    auto drawable = task.presenter->encodeCommandsMetal4(
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
    queue_.waitForDrawable(drawable);
    scope.signal_event = order_event;
    scope.signal_value = frame_id;
    scope.signal_drawable = drawable;
    if (task.observer && !drawable.observePresentation(task.observer, task.present_id)) {
      task.observer.cancel(task.present_id);
      task.observer_fault->store(true, std::memory_order_release);
    }

    // Core Animation receives the drawable only after this command buffer completes, and only if
    // no DXGI_PRESENT_RESTART advanced the epoch since this frame left the queue. A discarded
    // frame withdraws its observation there, like a queue-discarded frame.
    scope.inflight.native_present.emplace(InflightCommandBuffer::NativePresent{
        drawable, task.presenter->restartToken(), task.observer, task.backbuffer,
        std::move(task.composition.surface), task.composition.frame,
        frame_id, task.restart_epoch, task.present_id, task.after, task.composition.sync_interval,
        task.composition.allow_tearing});
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
    if (HRESULT hr = device_->reflex.Sleep(true, do_not_wait); FAILED(hr)) return hr;
    const bool native_composition = bool(composition.surface);
    uint64_t captured_frame_id = 0;
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
      task.reflex_work = device_->reflex.Capture(true);
      if (do_not_wait) {
        auto ready = presenter->trySynchronizeLayerProperties();
        if (!ready)
          return DXGI_ERROR_WAS_STILL_DRAWING;
        task.state.emplace(std::move(*ready));
      } else {
        task.state.emplace(presenter->synchronizeLayerProperties(true));
      }
      captured_frame_id = task.state->frame_id;
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
      if (native_composition)
        presenter->abortNativeRetirements();
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return DXGI_ERROR_DEVICE_REMOVED;
    } catch (const std::bad_alloc &) {
      presenter->cancelPresentOrder();
      if (native_composition && captured_frame_id)
        presenter->abortNativeRetirements();
      device_->ReportRemoval(E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
    } catch (...) {
      presenter->cancelPresentOrder();
      if (native_composition && captured_frame_id)
        presenter->abortNativeRetirements();
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
