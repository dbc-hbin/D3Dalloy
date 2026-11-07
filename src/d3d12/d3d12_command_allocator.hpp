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

#pragma once

#include "d3d12_pageable.hpp"
#include "d3dalloy_command_clear.hpp"
#include "d3dalloy_ring_bump_allocator.hpp"
#include "owned_submission_storage.hpp"
#include <memory>
#include <new>
#include <source_location>
#include <unordered_set>

namespace d3dalloy {

constexpr auto kCPUHeapSize = 0x400000u;
constexpr auto kGPUHeapSize = 0x400000u;

inline std::size_t
align_forward_adjustment(const void *const ptr, const std::size_t &alignment) noexcept {
  const auto iptr = reinterpret_cast<std::uintptr_t>(ptr);
  const auto aligned = (iptr - 1u + alignment) & -alignment;
  return aligned - iptr;
}

inline void *
ptr_add(const void *const p, const std::uintptr_t &amount) noexcept {
  return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(p) + amount);
}

struct IndirectComputeCommandData {
  uint64_t cmd_buf;
  uint64_t max_count;
  uint64_t max_count_buffer;
  uint64_t argument_buffer;
  uint64_t predicate;
  uint32_t predicate_op;
  uint32_t first_command;
  uint64_t static_samplers;
  uint64_t rootsig_qwords;
  uint64_t immutable_constants;
  uint32_t rootsig_qwords_stride;
  uint32_t tgsize_x;
  uint32_t tgsize_y;
  uint32_t tgsize_z;
  uint64_t pipeline_statistics;
};
static_assert(sizeof(IndirectComputeCommandData) == 96);
static_assert(offsetof(IndirectComputeCommandData, tgsize_x) == 76);
static_assert(offsetof(IndirectComputeCommandData, pipeline_statistics) == 88);

// One stable entry head per recording, owned by its allocator arena. The
// frontend may Reset the same command-list object onto another allocator
// while an older recording is executing; each head tracks its own submissions.
struct RecordedStateEvent {
  std::shared_ptr<BarrierTracker> resource;
  UINT subresource;
  D3D12_RESOURCE_STATES before;
  D3D12_RESOURCE_STATES after;
  D3D12_RESOURCE_BARRIER_FLAGS flags;
  bool implicit_use = false;
  // ID3D12GraphicsCommandList7::Barrier. Texture barriers carry layouts; buffer barriers (and
  // texture barriers on layout-less resources) only assert that no legacy state is in effect.
  bool enhanced = false;
  bool split_begin = false;
  bool split_end = false;
  D3D12_BARRIER_LAYOUT layout_before = D3D12_BARRIER_LAYOUT_UNDEFINED;
  D3D12_BARRIER_LAYOUT layout_after = D3D12_BARRIER_LAYOUT_UNDEFINED;
  D3D12_BARRIER_ACCESS access_before = D3D12_BARRIER_ACCESS_NO_ACCESS;
  D3D12_BARRIER_ACCESS access_after = D3D12_BARRIER_ACCESS_NO_ACCESS;
};

struct ClosedDescriptorSpan {
  MTLD3D12DescriptorHeap *heap;
  uint32_t first, count;
  uint64_t stages;
  bool writable;
  bool operator==(const ClosedDescriptorSpan &) const = default;
};
struct SubmissionDescriptorCache;
struct ClosedDescriptorShape {
  std::vector<ClosedDescriptorSpan> descriptors;
  bool descriptors_complete = true;
  mutable std::shared_ptr<const SubmissionDescriptorCache> descriptor_cache;
};
struct ClosedDispatchShape : ClosedDescriptorShape {
  bool uses_pass_descriptors = false;
  uint64_t buffers = 0;
};
struct ClosedPassShape : ClosedDescriptorShape {
  explicit ClosedPassShape(const EncoderData *record) : record(record) {}
  const EncoderData *record;
  bool compute_commands_complete = true, dispatch_access_complete = false;
  std::vector<ClosedDispatchShape> dispatches;
  std::vector<size_t> dispatch_memberships;
};
struct RecordingHead : EncoderData {
  ~RecordingHead();
  std::atomic<uint32_t> in_flight{0};
  std::vector<RecordedStateEvent> state_events;
  const EncoderData *last_implicit_encoder = nullptr;
  std::vector<RecordedBoundary> boundaries;
  struct RetainedDescriptorHeap {
    Com<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    UINT count;
    uint64_t base = UINT64_MAX; // First needed by a directly indexed root.
    size_t previous; // One-based link in the command list's lookup bucket.

    RetainedDescriptorHeap(ID3D12DescriptorHeap *object, D3D12_DESCRIPTOR_HEAP_TYPE kind, UINT descriptors, size_t link)
        : heap(object), type(kind), count(descriptors), previous(link) {}
    RetainedDescriptorHeap(RetainedDescriptorHeap &&) noexcept = default;
  };
  std::vector<RetainedDescriptorHeap> descriptor_heaps;
  // Heap COM references survive replay even when ordinary shader roots cannot address a sampler heap.
  bool may_read_sampler_heap = false;
  std::vector<Rc<BufferAllocation>> root_allocations;
  std::unordered_set<BufferAllocation *> root_allocation_set;
  // GPU-selected indirect root/VB addresses cannot be resolved while recording.
  // The Execute task pins the current VA registry once, through GPU retirement.
  bool gpu_indirect_buffer_addresses = false;
  // Close seals layout only. Execution may reuse resource-access summaries after
  // checking descriptor versions and current backing; GPU values are never cached.
  std::vector<ClosedPassShape> pass_shapes;
  std::unordered_map<const EncoderData *, size_t> pass_indices;
  std::vector<const FenceBarrierEncoderData *> fence_barriers;
  size_t fence_operand_count = 0;
  size_t raytracing_passes = 0;
  // Internal render templates belong to the recording even if a later
  // allocation fails before its logical frontier node can be published.
  std::vector<RenderEncoderData *> detached_render_templates;
};

class MTLD3D12CommandAllocatorImpl : public MTLD3D12Pageable<MTLD3D12CommandAllocator> {
  friend class MTLD3D12GraphicsCommandListImpl;
  friend struct SimpleCommandContext<MTLD3D12CommandAllocatorImpl>;
  HRESULT SealRecordingPlan();
  void RemoveOverwrittenClear(EncoderData *before, RenderEncoderData *clear);

  D3D12_COMMAND_LIST_TYPE type_;
  std::shared_ptr<std::atomic<uint64_t>> recording_generation_ = std::make_shared<std::atomic<uint64_t>>(0);
  d3dalloy::mutex submission_mutex_;
  uint32_t outstanding_submissions_ = 0;

  std::vector<void *> spilled_cpu_heap_;
  // Retired 4MiB slabs only; no recording pointer is moved or recycled early.
  std::array<void *, 3> retired_cpu_heaps_{};
  size_t retired_cpu_heap_count_ = 0;
  void *cpu_heap_ = nullptr;
  size_t cpu_heap_offset_ = 0;

  struct GPUHeapChunk {
    WMT::Reference<WMT::Buffer> buffer;
    void *memory;
    uint64_t begin;
    uint64_t address;
    size_t capacity;
    size_t used = 0;
    uint64_t id = owned_graphics::NewObjectId();
    uint64_t created_epoch = 0;
    uint64_t last_used_epoch = UINT64_MAX;
  };
  std::vector<GPUHeapChunk> gpu_heap_chunks_;
  size_t gpu_heap_active_ = 0;
  uint64_t gpu_heap_epoch_ = 0;
  // Immutable device limit; ReserveGPUHeap runs per root upload, so avoid a native call each time.
  const uint64_t gpu_heap_max_length_ = std::min<uint64_t>(device_->GetMTLDevice().maxBufferLength(), SIZE_MAX);
  const uint64_t trace_id_ = owned_graphics::NewObjectId();

  EncoderData *encoder_last = nullptr;
  EncoderData *encoder_current = nullptr;
  RecordingHead *recording_head_ = nullptr;
  size_t encoder_count_ = 0;
  // Recording progress only; queue replay never changes this serial.
  uint64_t hud_work_serial_ = 0;

  // Closed command-list entry pointers may outlive the list object while a
  // queue worker defers CPU encoding behind a future fence signal. Appending a
  // new recording after CommandList::Reset must not move earlier entry heads.
  owned_graphics::StableSubmissionStorage<RecordingHead> encoder_lists_;

  struct CachedICB {
    WMT::Reference<WMT::IndirectCommandBuffer> buffer;
    uint64_t resource_id, last_epoch;
    uint32_t capacity, used;
  };
  struct ICBPool {
    std::vector<CachedICB> pages;
    size_t active = SIZE_MAX;
    std::map<uint32_t, std::vector<size_t>> available; // partial tails for this recording
    struct WholePages {
      std::vector<size_t> indices;
      size_t unused = 0;
    };
    std::map<uint32_t, WholePages> whole; // stable capacity classes across resets
    size_t indexed_pages = 0;

    void IndexPage(uint32_t remaining, size_t i) {
      auto fit = available.lower_bound(remaining);
      if (fit != available.end() && fit->first == remaining) fit->second.push_back(i);
      else available.emplace_hint(fit, remaining, std::vector<size_t>{i});
    }
  };
  using ICBKey = std::pair<WMTResourceOptions, decltype(NativeIndirectCommandKey(WMTIndirectCommandBufferInfo{}))>;
  std::map<ICBKey, ICBPool> icb_;
  template<class Prepare>
  WMT::Reference<WMT::IndirectCommandBuffer> AcquirePreparedICB(WMTIndirectCommandBufferInfo &info,
      uint32_t count, WMTResourceOptions options, uint32_t &first_command, bool reuse_only, Prepare prepare);

  ClearUAV<MTLD3D12CommandAllocatorImpl> clear_uav_;
  ClearRTV<MTLD3D12CommandAllocatorImpl, WMT4::Compiler, WMT4::Library, WMT4::Function> clear_rtv_;

  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator_;
  uint64_t copy_temp_version_;

  /*
   * Per-dispatch raytracing intersection function tables, grouped by the pipeline they were made from.
   * Each table is rebound to one dispatch's arguments block, so within one epoch every dispatch takes
   * a table of its own (`used` counts them). A table is handed out again only after Reset, which is
   * accepted only once no submission of this allocator is outstanding - the same retirement rule that
   * lets the GPU heap chunks be rewritten - so no in-flight command buffer still reads its bindings.
   * The pipeline reference keeps its handle from being reused as a key while its tables live.
   */
  struct DispatchIntersectionTables {
    WMT::Reference<WMT::ComputePipelineState> pipeline;
    std::vector<WMT::Reference<WMT::IntersectionFunctionTable>> tables;
    size_t used = 0;
    uint64_t last_used_epoch = 0;
  };
  std::vector<DispatchIntersectionTables> dispatch_intersection_tables_;

public:
  uint64_t GetTraceId() const { return trace_id_; }
  WMT::Reference<WMT::IndirectCommandBuffer> AcquireICB(WMTIndirectCommandBufferInfo &info,
      uint32_t count, WMTResourceOptions options, uint32_t &first_command);
  RecordedBoundary &RecordBoundary(RecordedBoundaryReason reason) {
    auto *anchor = encoder_current ? encoder_current : encoder_last;
    const auto *command = anchor && anchor->type == EncoderType::Render
        ? static_cast<RenderEncoderData *>(anchor)->cmd_tail :
        anchor && anchor->type == EncoderType::Compute ? static_cast<ComputeEncoderData *>(anchor)->cmd_tail :
        anchor && anchor->type == EncoderType::Blit ? static_cast<BlitEncoderData *>(anchor)->cmd_tail : nullptr;
    auto &boundary = recording_head_->boundaries.emplace_back();
    boundary.encoder = anchor; boundary.command = command; boundary.reason = reason;
    boundary.state_event_index = recording_head_->state_events.size();
    return boundary;
  }
  MTLD3D12CommandAllocatorImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type);

  bool CaptureSubmission(uint64_t generation) {
    std::lock_guard<d3dalloy::mutex> lock(submission_mutex_);
    if (recording_generation_->load(std::memory_order_acquire) != generation)
      return false;
    ++outstanding_submissions_;
    return true;
  }

  void RetainSubmission() {
    std::lock_guard<d3dalloy::mutex> lock(submission_mutex_);
    ++outstanding_submissions_;
  }

  void ReleaseSubmission() {
    std::lock_guard<d3dalloy::mutex> lock(submission_mutex_);
    assert(outstanding_submissions_);
    --outstanding_submissions_;
  }

  ~MTLD3D12CommandAllocatorImpl();

  HRESULT
  Initialize();

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject);

  HRESULT STDMETHODCALLTYPE Reset();

  // Placement-new encoder payloads own retained resources and must be destroyed
  // both on Reset and final release; freeing their arena alone leaks them.
  void ClearRecordedCommands();

  HRESULT STDMETHODCALLTYPE CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
      void **ppCommandList
  );

  void
  InvalidateCurrentPass(std::source_location source = std::source_location::current()) {
    if (!encoder_current)
      return;
    owned_graphics::Trace("pass_end", ",\"encoder\":", encoder_current->id,
                          ",\"type\":", uint32_t(encoder_current->type),
                          ",\"line\":", source.line(), ",\"function\":\"", source.function_name(), "\"");
    encoder_last->next = encoder_current;
    encoder_last = encoder_current;

    encoder_current = nullptr;
    encoder_count_++;
  }

  void EndSchedulingPass() {
    if (encoder_current) encoder_current->endpoint_reason = RecordedEndpointReason::Scheduling;
    InvalidateCurrentPass();
  }

  RenderEncoderData *DetachCurrentRenderTemplate() {
    assert(recording_head_ && encoder_current && encoder_current->type == EncoderType::Render);
    auto *render = static_cast<RenderEncoderData *>(encoder_current);
    // Publish the owner first. If vector growth fails, the current pass remains
    // attached and normal failed-recording cleanup still owns it.
    recording_head_->detached_render_templates.push_back(render);
    encoder_current = nullptr;
    render->next = nullptr;
    owned_graphics::Trace("capture_template_detached", ",\"encoder\":", render->id);
    return render;
  }

  HRESULT
  StartRecord(EncoderData **pStartEncoder) {
    if (encoder_last)
      return E_INVALIDARG;
    auto &head = encoder_lists_.emplace_back();
    recording_head_ = &head;
    head.type = EncoderType::Null;
    head.next = nullptr;
    head.id = 0;
    *pStartEncoder = encoder_last = &head;
    return S_OK;
  }

  HRESULT
  EndRecord(size_t *pEncoderCount) {
    if (!encoder_last)
      return E_FAIL;
    if (encoder_current) {
      encoder_current->endpoint_reason = RecordedEndpointReason::ListEnd;
      InvalidateCurrentPass();
    }
    const HRESULT sealed = SealRecordingPlan();
    encoder_last = nullptr;
    recording_head_ = nullptr;
    *pEncoderCount = encoder_count_;
    encoder_count_ = 0;
    return sealed;
  }

  void *
  AllocateCPUHeap(size_t Length, size_t Alignment) {
    assert(Length < kCPUHeapSize);
    const auto adjustment = align_forward_adjustment((void *)cpu_heap_offset_, Alignment);
    auto aligned = cpu_heap_offset_ + adjustment;
    if (unlikely(aligned >= kCPUHeapSize || Length >= kCPUHeapSize - aligned)) {
      // Neither the current cursor nor ownership changes until both allocations
      // succeed. Existing command pointers must survive either failure.
      if (retired_cpu_heap_count_) {
        spilled_cpu_heap_.push_back(cpu_heap_); // May throw; the spare still belongs to us.
        cpu_heap_ = retired_cpu_heaps_[--retired_cpu_heap_count_];
        retired_cpu_heaps_[retired_cpu_heap_count_] = nullptr;
      } else {
        std::unique_ptr<void, decltype(&free)> next(malloc(kCPUHeapSize), &free);
        if (!next) throw std::bad_alloc();
        spilled_cpu_heap_.push_back(cpu_heap_);
        cpu_heap_ = next.release();
      }
      aligned = 0;
    }
    cpu_heap_offset_ = aligned + Length;
    return ptr_add(cpu_heap_, aligned);
  }

  template <typename T>
  T *
  AllocatePass() {
    auto p = (new (AllocateCPUHeap(sizeof(T), alignof(T))) T());
    if constexpr (std::is_same_v<T, ClearEncoderData> || std::is_same_v<T, ResolveEncoderData> ||
                  std::is_same_v<T, PredicatedTextureCopyEncoderData> ||
                  std::is_same_v<T, CaptureFrontierEncoderData>)
      ++hud_work_serial_;
    p->id = owned_graphics::NewObjectId();
    encoder_current = p;
    return p;
  };

  template <typename T>
  T *
  AllocateCommandData(size_t Count) {
    return (T *)AllocateCPUHeap(sizeof(T) * Count, alignof(T));
  };

  template <bool extend_single = false, typename Wide>
  bool EncodeDrawRun(WMTPrimitiveType primitive, const Wide &args, wmtdraw_index_binding ib = {},
                     WMTIndexType index_type = WMTIndexTypeUInt16) {
    constexpr bool indexed = std::is_same_v<Wide, wmtdraw_indexed_parameters>;
    constexpr auto type = indexed ? WMTRenderCommandDrawIndexedRun : WMTRenderCommandDrawRun;
    auto *encoder = static_cast<RenderEncoderData *>(encoder_current);
    int32_t base = 0;
    if constexpr (indexed) base = args.base_vertex;
    auto *run = reinterpret_cast<wmtcmd_render_draw_run *>(encoder->cmd_tail);
    bool small = (args.count | args.first) <= UINT16_MAX && (args.instances | args.base_instance) <= UINT8_MAX &&
                 base >= INT16_MIN && base <= INT16_MAX;
    if constexpr (extend_single)
      if (run->type == type && !(run->flags & WMTDrawRunSmall)) small = false;
    uint8_t flags = (small ? WMTDrawRunSmall : 0) | (index_type == WMTIndexTypeUInt32 ? WMTDrawRunIndex32 : 0);
    size_t stride = small ? sizeof(wmtdraw_small_parameters) : sizeof(Wide);
    const auto *bound = encoder->draw_run_binding;
    const bool same_binding = !indexed || (bound && bound->index_buffer == ib.index_buffer &&
                                           bound->index_offset == ib.index_offset);
    auto *cursor = static_cast<char *>(ptr_add(cpu_heap_, cpu_heap_offset_));
    void *slot = nullptr;
    if (same_binding && run->type == type && run->flags == flags && run->primitive_type == primitive &&
        run->count < UINT16_MAX && stride < kCPUHeapSize - cpu_heap_offset_ &&
        static_cast<const char *>(wmt_draw_run_tuples(run)) + stride * run->count == cursor) {
      const auto &boundaries = recording_head_->boundaries;
      if (boundaries.empty() || boundaries.back().command != encoder->cmd_tail) {
        slot = cursor;
        cpu_heap_offset_ += stride;
        ++run->count;
      }
    }
    // A nonzero reference must land in this slab and point strictly backwards.
    auto binding_reference = [&](uintptr_t start) -> uint16_t {
      const uintptr_t distance = start - reinterpret_cast<uintptr_t>(bound);
      if (!same_binding || !distance || distance > UINT16_MAX * 8u ||
          reinterpret_cast<uintptr_t>(bound) < reinterpret_cast<uintptr_t>(cpu_heap_)) return 0;
      return uint16_t(distance / 8);
    };
    if constexpr (extend_single) {
      if (!slot) {
        const auto *leaf = encoder->cmd_tail;
        const auto &boundaries = recording_head_->boundaries;
        if (!boundaries.empty() && boundaries.back().command == leaf) return false;
        uint32_t first_count, first_start;
        int32_t first_base = 0;
        uint16_t back8 = 0;
        size_t leaf_bytes;
        if constexpr (indexed) {
          uint32_t offset;
          auto capture = [&](const auto *first) {
            if (first->primitive_type != primitive || first->index_buffer != ib.index_buffer ||
                first->index_type != index_type || first->index_buffer_offset < ib.index_offset)
              return false;
            offset = uint32_t(first->index_buffer_offset - ib.index_offset);
            first_count = first->index_count;
            leaf_bytes = sizeof(*first);
            return true;
          };
          if (leaf->type == WMTRenderCommandDrawIndexedSmall) {
            if (!capture(reinterpret_cast<const wmtcmd_render_draw_indexed_small *>(leaf))) return false;
          } else if (leaf->type == WMTRenderCommandDrawIndexedCompact) {
            const auto *first = reinterpret_cast<const wmtcmd_render_draw_indexed_compact *>(leaf);
            if (!capture(first)) return false;
            first_base = first->base_vertex;
          } else return false;
          const unsigned shift = index_type == WMTIndexTypeUInt32 ? 2 : 1;
          if (offset & ((1u << shift) - 1)) return false;
          first_start = offset >> shift;
          back8 = binding_reference(reinterpret_cast<uintptr_t>(leaf));
        } else {
          if (leaf->type != WMTRenderCommandDrawCompact) return false;
          const auto *first = reinterpret_cast<const wmtcmd_render_draw_compact *>(leaf);
          if (first->primitive_type != primitive) return false;
          first_count = first->vertex_count;
          first_start = first->vertex_start;
          leaf_bytes = sizeof(*first);
        }
        small &= (first_count | first_start) <= UINT16_MAX && first_base >= INT16_MIN && first_base <= INT16_MAX;
        if (!small) { flags &= ~WMTDrawRunSmall; stride = sizeof(Wide); }
        const size_t extra = sizeof(*run) + 2 * stride + (indexed && !back8 ? sizeof(ib) : 0) - leaf_bytes;
        // Two cold wide indexed leaves are smaller than a run with an embedded binding.
        if ((indexed && !small && !back8) || reinterpret_cast<const char *>(leaf) + leaf_bytes != cursor ||
            extra >= kCPUHeapSize - cpu_heap_offset_) return false;
        run->type = type;
        run->primitive_type = primitive;
        run->flags = flags;
        run->count = 2;
        run->binding_back8 = back8;
        slot = run + 1;
        if constexpr (indexed) {
          if (!back8) {
            auto *binding = static_cast<wmtdraw_index_binding *>(slot);
            *binding = ib;
            encoder->draw_run_binding = binding;
            slot = binding + 1;
          }
        }
        if (small)
          *static_cast<wmtdraw_small_parameters *>(slot) = {uint16_t(first_count), uint16_t(first_start), 1, 0, int16_t(first_base)};
        else if constexpr (indexed)
          *static_cast<Wide *>(slot) = {first_count, 1, first_start, 0, first_base};
        else
          *static_cast<Wide *>(slot) = {first_count, 1, first_start, 0};
        slot = static_cast<char *>(slot) + stride;
        cpu_heap_offset_ += extra;
      }
    } else if (!slot) {
      size_t bytes = sizeof(*run) + stride;
      uint16_t back8 = 0;
      if constexpr (indexed) {
        const auto aligned = (cpu_heap_offset_ + 7) & ~size_t(7);
        if (aligned + bytes < kCPUHeapSize)
          back8 = binding_reference(reinterpret_cast<uintptr_t>(cpu_heap_) + aligned);
        if (!back8) bytes += sizeof(ib);
      }
      run = static_cast<wmtcmd_render_draw_run *>(AllocateCPUHeap(bytes, 8));
      run->type = type;
      run->primitive_type = primitive;
      run->flags = flags;
      run->count = 1;
      run->binding_back8 = back8;
      run->next.set(nullptr);
      encoder->cmd_tail->next.set(run);
      encoder->cmd_tail = reinterpret_cast<wmtcmd_base *>(run);
      slot = run + 1;
      if constexpr (indexed) {
        if (!back8) {
          auto *binding = static_cast<wmtdraw_index_binding *>(slot);
          *binding = ib;
          encoder->draw_run_binding = binding;
          slot = binding + 1;
        }
      }
    }
    if (small)
      *static_cast<wmtdraw_small_parameters *>(slot) = {uint16_t(args.count), uint16_t(args.first),
          uint8_t(args.instances), uint8_t(args.base_instance), int16_t(base)};
    else
      memcpy(slot, &args, stride);
    ++hud_work_serial_;
    return true;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeRenderCommand(size_t payload_bytes = 0) {
    assert(encoder_current->type == EncoderType::Render);
    auto encoder = static_cast<RenderEncoderData *>(encoder_current);
    constexpr bool compact24 = std::is_same_v<cmd_struct, wmtcmd_render_draw_compact> ||
                               std::is_same_v<cmd_struct, wmtcmd_render_draw_indexed_small>;
    static_assert(!compact24 || alignof(cmd_struct) <= 8);
    constexpr auto alignment = compact24 ? 8 : 16;
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct) + payload_bytes, alignment);
    if constexpr (std::is_same_v<cmd_struct, wmtcmd_render_setviewport> || std::is_same_v<cmd_struct, wmtcmd_render_setviewports>)
      encoder->fixed_viewports = nullptr;
    if constexpr (std::is_same_v<cmd_struct, wmtcmd_render_setscissorrect> || std::is_same_v<cmd_struct, wmtcmd_render_setscissorrects>)
      encoder->fixed_scissors = nullptr;
    if constexpr (
        std::is_same_v<cmd_struct, wmtcmd_render_draw_compact> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indexed_compact> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indexed_small> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indexed> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indirect_range> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indexed_indirect_range> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_indexed_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_meshthreadgroups> ||
        std::is_same_v<cmd_struct, wmtcmd_render_draw_meshthreadgroups_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_geometry_draw> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_geometry_draw_indexed> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_geometry_draw_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_tessellation_mesh_draw> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_tessellation_mesh_draw_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed_indirect> ||
        std::is_same_v<cmd_struct, wmtcmd_render_executecommands>)
      ++hud_work_serial_;
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    encoder->cmd_tail->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeBlitCommand(bool tracked = false) {
    assert(encoder_current->type == EncoderType::Blit);
    auto encoder = static_cast<BlitEncoderData *>(encoder_current);
    if constexpr (std::is_same_v<cmd_struct, wmtcmd_blit_copy_from_buffer_to_buffer>)
      ++encoder->submission_copy_count;
    else if (!tracked)
      // Tracked commands describe their accesses through the command list's
      // RecordBlitTextureAccess/RecordBlitUploadAccess; every other blit
      // command stays untracked.
      encoder->submission_access_complete = false;
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    ++hud_work_serial_;
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  EncodeComputeCommand() {
    assert(encoder_current->type == EncoderType::Compute);
    auto encoder = static_cast<ComputeEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    if constexpr (std::is_same_v<cmd_struct, wmtcmd_compute_dispatch_indirect_range> ||
                  std::is_same_v<cmd_struct, wmtcmd_compute_dispatch_compact> ||
                  std::is_same_v<cmd_struct, wmtcmd_compute_dispatch> ||
                  std::is_same_v<cmd_struct, wmtcmd_compute_dispatch_indirect> ||
                  std::is_same_v<cmd_struct, wmtcmd_compute_executecommands>)
      ++hud_work_serial_;
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    encoder->cmd_tail->next.set(nullptr);
    return *storage;
  }

  // Reserves contiguous capacity without consuming it. Composite callers reserve
  // their whole batch before recording any pointers or commands. Earlier chunks
  // remain immutable until a safely retired allocator Reset.
  bool ReserveGPUHeap(uint64_t Length, size_t Alignment);

  struct GPUHeapAllocation {
    void *memory;
    WMT::Buffer buffer;
    uint64_t offset;
    uint64_t address;
  };

  GPUHeapAllocation ResolveGPUHeap(uint64_t Offset) const;

  uint64_t GPUHeapAddress(uint64_t Offset) {
    const auto allocation = ResolveGPUHeap(Offset);
    if (encoder_current && encoder_current->type == EncoderType::Compute) {
      auto &use = EncodeComputeCommand<wmtcmd_compute_useresource>();
      use.type = WMTComputeCommandUseResource;
      use.resource = allocation.buffer;
      use.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    } else if (encoder_current && encoder_current->type == EncoderType::Render) {
      auto &use = EncodeRenderCommand<wmtcmd_render_useresource>();
      use.type = WMTRenderCommandUseResource;
      use.resource = allocation.buffer;
      use.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
      use.stages = WMTRenderStageVertex | WMTRenderStageFragment | WMTRenderStageObject | WMTRenderStageMesh;
    }
    return allocation.address;
  }

  template <typename BufferType>
  void BindGPUHeap(BufferType &Buffer, uint64_t &Offset, uint64_t VirtualOffset) const {
    const auto allocation = ResolveGPUHeap(VirtualOffset);
    Buffer = allocation.buffer;
    Offset = allocation.offset;
  }

  std::tuple<void *, size_t>
  AllocateGPUHeap(uint64_t Length, size_t Alignment) {
    if (!Length)
      return {nullptr, 0};
    if (!ReserveGPUHeap(Length, Alignment))
      return {nullptr, SIZE_MAX};
    auto &chunk = gpu_heap_chunks_[gpu_heap_active_];
    const size_t adjustment = (-chunk.used) & (Alignment - 1);
    const auto aligned = chunk.used + adjustment;
    if (!chunk.used && chunk.created_epoch < gpu_heap_epoch_)
      owned_graphics::Trace("root_upload_reuse", ",\"allocator\":", trace_id_, ",\"chunk\":", chunk.id,
                            ",\"epoch\":", gpu_heap_epoch_, ",\"buffer\":", chunk.buffer.handle,
                            ",\"va\":", chunk.address, ",\"capacity\":", chunk.capacity);
    chunk.used = aligned + Length;
    chunk.last_used_epoch = gpu_heap_epoch_;
    owned_graphics::Trace("root_upload_allocate", ",\"allocator\":", trace_id_, ",\"chunk\":", chunk.id,
                          ",\"epoch\":", gpu_heap_epoch_, ",\"buffer\":", chunk.buffer.handle,
                          ",\"va\":", chunk.address + aligned, ",\"begin\":", chunk.begin,
                          ",\"offset\":", aligned, ",\"size\":", Length, ",\"alignment\":", Alignment);
    return {ptr_add(chunk.memory, aligned), chunk.begin + aligned};
  }

  /*
   * The table slot for one more raytracing dispatch of `Pipeline` in this epoch. An empty slot has no
   * table yet; a filled one was last bound by a dispatch of an earlier, retired epoch and may be rebound.
   * The reference is valid until the next call.
   */
  WMT::Reference<WMT::IntersectionFunctionTable> &
  DispatchIntersectionTableSlot(WMT::ComputePipelineState Pipeline) {
    auto entry = std::find_if(dispatch_intersection_tables_.begin(), dispatch_intersection_tables_.end(),
                              [&](const auto &candidate) { return candidate.pipeline == Pipeline; });
    if (entry == dispatch_intersection_tables_.end()) {
      dispatch_intersection_tables_.push_back({Pipeline});
      entry = std::prev(dispatch_intersection_tables_.end());
    }
    entry->last_used_epoch = gpu_heap_epoch_;
    if (entry->used == entry->tables.size())
      entry->tables.emplace_back();
    return entry->tables[entry->used++];
  }

  std::tuple<WMT::Buffer, uint64_t>
  AllocateTempBuffer(size_t Size, size_t Alignment) {
    assert(copy_temp_version_);
    auto [block, offset] = copy_temp_allocator_.allocate(copy_temp_version_, copy_temp_version_ - 1, Size, Alignment);
    return {block.buffer, offset};
  }

  IndirectComputeCommandData *EncodeIndirectComputeCommand(
      MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount, bool statistics);

  IndirectRenderCommandData *EncodeIndirectRenderCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, size_t MaxCount, bool index_pull, bool reuse_only = false);
};

} // namespace d3dalloy
