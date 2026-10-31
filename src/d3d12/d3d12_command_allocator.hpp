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
#include "dxmt_command_clear.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "owned_submission_storage.hpp"
#include <memory>
#include <new>
#include <source_location>
#include <unordered_set>

namespace dxmt {

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
  uint32_t predicate_padding;
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

struct RecordingHead : EncoderData {
  std::atomic<uint32_t> in_flight{0};
  std::vector<RecordedStateEvent> state_events;
  std::vector<RecordedBoundary> boundaries;
  std::vector<Com<ID3D12DescriptorHeap>> descriptor_heaps;
  std::vector<Rc<BufferAllocation>> root_allocations;
  std::unordered_set<BufferAllocation *> root_allocation_set;
};

class MTLD3D12CommandAllocatorImpl : public MTLD3D12Pageable<MTLD3D12CommandAllocator> {
  friend class MTLD3D12GraphicsCommandListImpl;
  friend struct SimpleCommandContext<MTLD3D12CommandAllocatorImpl>;

  D3D12_COMMAND_LIST_TYPE type_;
  std::shared_ptr<std::atomic<uint64_t>> recording_generation_ = std::make_shared<std::atomic<uint64_t>>(0);
  dxmt::mutex submission_mutex_;
  uint32_t outstanding_submissions_ = 0;

  std::vector<void *> spilled_cpu_heap_;
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

  // Closed command-list entry pointers may outlive the list object while a
  // queue worker defers CPU encoding behind a future fence signal. Appending a
  // new recording after CommandList::Reset must not move earlier entry heads.
  owned_graphics::StableSubmissionStorage<RecordingHead> encoder_lists_;

  small_vector<WMT::Reference<WMT::IndirectCommandBuffer>, 4> icb_;

  ClearUAV<MTLD3D12CommandAllocatorImpl> clear_uav_;
  ClearRTV<MTLD3D12CommandAllocatorImpl, WMT4::Compiler, WMT4::Library, WMT4::Function> clear_rtv_;

  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator_;
  uint64_t copy_temp_version_;

public:
  uint64_t GetTraceId() const { return trace_id_; }
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
    std::lock_guard<dxmt::mutex> lock(submission_mutex_);
    if (recording_generation_->load(std::memory_order_acquire) != generation)
      return false;
    ++outstanding_submissions_;
    return true;
  }

  void RetainSubmission() {
    std::lock_guard<dxmt::mutex> lock(submission_mutex_);
    ++outstanding_submissions_;
  }

  void ReleaseSubmission() {
    std::lock_guard<dxmt::mutex> lock(submission_mutex_);
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
    encoder_last = nullptr;
    recording_head_ = nullptr;
    *pEncoderCount = encoder_count_;
    encoder_count_ = 0;
    return S_OK;
  }

  void *
  AllocateCPUHeap(size_t Length, size_t Alignment) {
    assert(Length < kCPUHeapSize);
    const auto adjustment = align_forward_adjustment((void *)cpu_heap_offset_, Alignment);
    auto aligned = cpu_heap_offset_ + adjustment;
    if (unlikely(aligned >= kCPUHeapSize || Length >= kCPUHeapSize - aligned)) {
      // Neither the current cursor nor ownership changes until both allocations
      // succeed. Existing command pointers must survive either failure.
      std::unique_ptr<void, decltype(&free)> next(malloc(kCPUHeapSize), &free);
      if (!next)
        throw std::bad_alloc();
      spilled_cpu_heap_.push_back(cpu_heap_);
      cpu_heap_ = next.release();
      aligned = 0;
    }
    cpu_heap_offset_ = aligned + Length;
    return ptr_add(cpu_heap_, aligned);
  }

  template <typename T>
  T *
  AllocatePass() {
    auto p = (new (AllocateCPUHeap(sizeof(T), alignof(T))) T());
    p->id = owned_graphics::NewObjectId();
    encoder_current = p;
    return p;
  };

  template <typename T>
  T *
  AllocateCommandData(size_t Count) {
    return (T *)AllocateCPUHeap(sizeof(T) * Count, alignof(T));
  };

  template <typename cmd_struct>
  cmd_struct &
  EncodeRenderCommand() {
    assert(encoder_current->type == EncoderType::Render);
    auto encoder = static_cast<RenderEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)AllocateCPUHeap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
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
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
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

  std::tuple<WMT::Buffer, uint64_t>
  AllocateTempBuffer(size_t Size, size_t Alignment) {
    assert(copy_temp_version_);
    auto [block, offset] = copy_temp_allocator_.allocate(copy_temp_version_, copy_temp_version_ - 1, Size, Alignment);
    return {block.buffer, offset};
  }

  IndirectComputeCommandData *EncodeIndirectComputeCommand(
      MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount, bool statistics);

  IndirectRenderCommandData *EncodeIndirectRenderCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, size_t MaxCount, bool index_pull);
};

} // namespace dxmt
