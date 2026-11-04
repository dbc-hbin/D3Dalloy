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
#include "d3d12_resource_contract.hpp"
#include "d3d12_capture_frontier.hpp"
#include "d3d12_msaa_tile_tasks.hpp"
#include "d3d12_standard_swizzle_gpu.hpp"
#include "d3dalloy_format.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "owned_submission_ring.hpp"
#include "owned_queue_task_fifo.hpp"
#include "owned_temporal_submission.hpp"
#include "owned_submission_access.hpp"
#include "d3d12_temporal.hpp"
#include "d3d12_frame_generation.hpp"
#include "d3d12_ray_regeneration.hpp"
#include "d3d12_work_graph.hpp"
#include "d3d12_state_object.hpp"
#include "dxil_raytracing.h"
#include "Metal4FX.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <list>
#include <deque>
#include <new>
#include <stdexcept>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace d3dalloy {

constexpr auto kCommandQueueSize = 32u;

// One immutable descriptor-only summary per sealed pass. The recording and each
// submission using it retain this object; replacement cannot retire GPU owners.
// The last summary keeps its owners until replacement or recording destruction.
struct SubmissionDescriptorCache {
  struct Version { MTLD3D12DescriptorHeap *heap; uint32_t index; uint64_t value; };
  struct Resource {
    Com<MTLD3D12Resource, false> owner;
    Rc<BufferAllocation> buffer;
    Rc<TextureAllocation> texture;
  };
  struct Buffer { Rc<BufferAllocation> allocation; uint64_t suballocation_offset; };
  std::vector<Version> versions;
  std::vector<Resource> resources;
  std::vector<Buffer> buffers;
  std::vector<SubmissionAccess> accesses;
  uint64_t va_generation = 0;
  size_t descriptor_count = 0;
  bool helper_passes = false;

  bool Current(MTLD3D12Device *device) const {
    if (va_generation != device->buffer_va_generation.load(std::memory_order_acquire)) return false;
    for (const auto &version : versions)
      if (version.heap->GetDescriptorVersion(version.index) != version.value) return false;
    for (const auto &resource : resources) {
      if ((resource.owner->buffer ? resource.owner->buffer->current() : nullptr) != resource.buffer.ptr() ||
          (resource.owner->texture ? resource.owner->texture->current() : nullptr) != resource.texture.ptr()) return false;
    }
    for (const auto &buffer : buffers)
      if (buffer.allocation->currentSuballocationOffset() != buffer.suballocation_offset) return false;
    return true;
  }
};

namespace {
constexpr size_t kSubmissionDescriptorBudget = 64;
constexpr size_t kSubmissionRangeBudget = 32;
constexpr size_t kSubmissionCandidateBudget = 8;
constexpr size_t kSubmissionGroupBudget = 64;
struct SubmissionDependency { size_t source; uint64_t before, after; bool attachment_only; uint8_t scope; };
struct SubmissionNode {
  const EncoderData *record;
  size_t list_ordinal, node_ordinal;
  size_t group = SIZE_MAX;
  const char *rejection = nullptr;
  // Access completeness answers whether resource dependencies can be trusted.
  // Merge eligibility is separate: an encoder may be fully described yet still
  // require its own native render encoder (for example, fragment completion).
  bool access_complete = false, merge_eligible = false;
  bool concurrent_dispatches = false;
  const char *dispatch_serial_reason = "unresolved";
  size_t dispatch_proof_checks = 0;
  bool boundary_before = false, boundary_after = false, force_fence_before = false, skip_clear = false;
  // The boundary only bounds planner frontier work: it orders no memory the
  // allocation summaries miss, so replay keeps in-flight tracking across it.
  bool partition_before = false;
  // Writable MSAA import/export helpers run around this node and wait on the
  // fence themselves: the node keeps its own wait and produces render stages.
  bool helper_passes = false;
  bool sparse_resource_used = false;
  bool placed_attachments = false;
  // A whole-surface AVERAGE ResolveSubresource immediately after this native
  // MSAA pass, folded into the pass's color store. Value-owned: the target
  // view is retained by the plan, the const resolve record stays unmodified.
  struct FoldedResolve { unsigned slot; TextureViewRef target; };
  std::optional<FoldedResolve> folded_resolve;
  bool resolve_folded_into_previous = false;
  std::vector<SubmissionAccess> accesses;
  std::unordered_map<PhysicalIdentity, std::vector<size_t>, PhysicalIdentityHash> access_slots;
  std::vector<Rc<BufferAllocation>> allocation_pins;
  size_t descriptor_resolutions = 0;
  std::vector<SubmissionAccess> scopes_before, scopes_after;
  std::vector<SubmissionDependency> dependencies;
  std::vector<size_t> clears_before;
};

// Metal4 cannot complete a fragment producer inside the current render
// encoder. Keep the only proven inline family explicit: attachment-only edges
// are represented by the render-pass mapping; non-attachment edges must be
// vertex-to-vertex to use an inline buffer/texture barrier.
bool InlineRenderDependencySafe(const SubmissionDependency &dependency) {
  return dependency.attachment_only ||
      (dependency.before == WMTMetal4StageVertex && dependency.after == WMTMetal4StageVertex);
}
struct RenderReplayGroup {
  std::vector<size_t> members;
  bool force_fence_before = false;
  WMTRenderPassInfo descriptor{};
  std::vector<std::array<uint8_t, 8>> color_maps;
  std::array<TextureViewRef, 8> colors;
  TextureViewRef depth, stencil;
  uint32_t color_count = 0;
  bool needs_unbound_slot = false;
  bool placed_attachments = false;
  uint64_t stages = 0;
};
struct SubmissionClearBatch {
  WMTRenderPassInfo descriptor{};
  std::vector<std::pair<size_t, uint8_t>> members;
};
struct SubmissionResourcePins {
  std::vector<Com<MTLD3D12Resource, false>> descriptor_owners;
  std::vector<Rc<BufferAllocation>> buffer_owners;
  std::vector<std::shared_ptr<const SubmissionDescriptorCache>> descriptor_caches;
};
// The gate retains this control block through actual GPU retirement. Native
// identities cannot be recycled while an admitted physical access is live.
struct PhysicalAdmissionProof {
  std::vector<SubmissionAccess> accesses;
  std::vector<Rc<BufferAllocation>> buffers;
  std::vector<Rc<TextureAllocation>> textures;
  std::vector<Com<MTLD3D12Resource, false>> resources;
};
bool PlainAdmissionBuffer(const BufferAllocation *allocation) {
  return allocation && !allocation->canonicalHeap && !allocation->reservedResource &&
      !allocation->flags().test(BufferAllocationFlag::CpuPlaced) &&
      !allocation->flags().test(BufferAllocationFlag::PlacementSparse) &&
      !allocation->flags().test(BufferAllocationFlag::SuballocateFromOnePage);
}
bool PlainAdmissionTexture(const TextureAllocation *allocation) {
  return allocation && !allocation->canonicalHeap && !allocation->mappedMemory &&
      !allocation->flags().test(TextureAllocationFlag::Shared) &&
      !allocation->flags().test(TextureAllocationFlag::PlacementSparse) &&
      !allocation->descriptor->reservedResource && !allocation->descriptor->reservedVolumeLayoutAddress;
}
using AdmissionBufferOwners = std::unordered_map<const void *, const Rc<BufferAllocation> *>;
bool AppendAdmissionAccess(PhysicalAdmissionProof &proof, SubmissionAccess access,
    std::span<const Rc<BufferAllocation>> buffers, bool whole = false,
    const AdmissionBufferOwners *cached_buffers = nullptr) {
  if (proof.accesses.size() == 256) return false;
  if (access.Key().domain == PhysicalDomain::Buffer) {
    const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto &allocation) {
      return allocation.ptr() == access.allocation;
    });
    const Rc<BufferAllocation> *owner = found != buffers.end() ? &*found : nullptr;
    if (!owner && cached_buffers) {
      const auto cached = cached_buffers->find(access.allocation);
      if (cached != cached_buffers->end()) owner = cached->second;
    }
    if (!owner || !PlainAdmissionBuffer(owner->ptr()) || access.begin >= access.end ||
        access.end > (*owner)->byteLength() || access.end == UINT64_MAX ||
        access.Key().owner != (*owner)->buffer().handle) return false;
    proof.buffers.push_back(*owner);
    if (whole) { access.begin = 0; access.end = (*owner)->byteLength(); }
  } else if (access.Key().domain == PhysicalDomain::Texture) {
    auto *allocation = const_cast<TextureAllocation *>(static_cast<const TextureAllocation *>(access.allocation));
    if (!PlainAdmissionTexture(allocation) || !access.aspects ||
        access.mip_begin >= access.mip_end || access.slice_begin >= access.slice_end ||
        access.mip_end > allocation->descriptor->miplevelCount() ||
        access.slice_end > allocation->descriptor->arrayLength() ||
        access.Key().owner != allocation->texture().handle) return false;
    proof.textures.emplace_back(allocation);
    if (whole) {
      access.mip_begin = access.slice_begin = 0;
      access.mip_end = allocation->descriptor->miplevelCount();
      access.slice_end = allocation->descriptor->arrayLength();
      access.aspects = 7;
    }
  } else return false;
  if (whole) access.writes = true;
  proof.accesses.push_back(access);
  return true;
}
GPUExecutionGate::Accesses ShareAdmissionProof(std::shared_ptr<PhysicalAdmissionProof> proof) {
  return {proof, &proof->accesses};
}
bool AdmissionCovers(const GPUExecutionGate::Accesses &proof, const SubmissionAccess &access) {
  if (!proof) return false;
  return std::any_of(proof->begin(), proof->end(), [&](const auto &prior) {
    if (prior.Key() != access.Key() || (access.writes && !prior.writes)) return false;
    if (!prior.aspects && !access.aspects) return prior.begin <= access.begin && prior.end >= access.end;
    return prior.aspects && access.aspects && (access.aspects & ~prior.aspects) == 0 &&
        prior.mip_begin <= access.mip_begin && prior.mip_end >= access.mip_end &&
        prior.slice_begin <= access.slice_begin && prior.slice_end >= access.slice_end;
  });
}
struct SparsePublicationProof {
  bool required = false;
  size_t heap_ranges = 0;
  OwnedSparseHeapIndex::Proof heap_proof{};
};
struct SubmissionPlan {
  std::vector<SubmissionNode> nodes;
  std::vector<RenderReplayGroup> groups;
  std::vector<SubmissionClearBatch> clear_batches;
  std::vector<size_t> tail_clears;
  std::vector<Com<MTLD3D12Resource, false>> descriptor_owners;
  std::unordered_set<uint64_t> descriptor_owner_ids;
  std::vector<std::shared_ptr<const SubmissionDescriptorCache>> descriptor_caches;
  std::vector<SparsePublicationProof> sparse_publication;
  size_t history_checks = 0, candidate_checks = 0, metadata_copies = 0, scope_checks = 0, feedback_checks = 0;
  size_t descriptor_resolutions = 0, descriptor_cache_hits = 0, descriptor_cache_misses = 0, descriptor_cache_builds = 0;
};
bool AppendSubmissionAccess(SubmissionNode &node, SubmissionAccess access) {
  auto &slots = node.access_slots[access.Key()];
  for (const auto slot : slots) {
    auto &prior = node.accesses[slot];
    if (prior.begin == access.begin && prior.end == access.end && prior.mip_begin == access.mip_begin &&
        prior.mip_end == access.mip_end && prior.slice_begin == access.slice_begin && prior.slice_end == access.slice_end &&
        prior.aspects == access.aspects && prior.writes == access.writes && prior.attachment == access.attachment &&
        prior.allocation == access.allocation) {
      prior.stages |= access.stages; return true;
    }
  }
  if (slots.size() == kSubmissionRangeBudget) { node.rejection = "access_range_budget"; return false; }
  slots.push_back(node.accesses.size()); node.accesses.push_back(access);
  return true;
}
bool AppendSubmissionNativeBuffer(SubmissionNode &node, WMT::Buffer buffer, bool writes, uint64_t stages) {
  if (!buffer) return true;
  return AppendSubmissionAccess(node, {reinterpret_cast<const void *>(buffer.handle), 0, UINT64_MAX,
      0, 1, 0, 1, 0, writes, false, stages, {PhysicalDomain::Buffer,uintptr_t(buffer.handle)}});
}
SubmissionAccess BufferAccess(BufferAllocation *allocation,uint64_t offset,uint64_t bytes,bool writes,uint64_t stages) {
  const bool heap=bool(allocation->canonicalHeap);
  const uint64_t base=heap?allocation->canonicalHeapOffset:allocation->currentSuballocationOffset();
  return {allocation,base+offset,base+offset+bytes,0,1,0,1,0,writes,false,stages,
      {heap?PhysicalDomain::Heap:PhysicalDomain::Buffer,
       uintptr_t(heap?allocation->canonicalHeap.handle:allocation->buffer().handle)}};
}
SubmissionAccess TextureAccess(TextureAllocation *allocation,TextureViewKey key,uint8_t aspects,
    bool writes,bool attachment,uint64_t stages) {
  // Use the immutable native allocation extent for heap aliases. Ordinary
  // non-heap textures retain their precise mip/slice/aspect lanes.
  const bool heap=bool(allocation->canonicalHeap);
  SubmissionAccess access{allocation,0,0,uint32_t(key.mip_start),uint32_t(key.mip_end),
      uint32_t(key.array_start),uint32_t(key.array_end),aspects,writes,attachment,stages,
      {heap?PhysicalDomain::Heap:PhysicalDomain::Texture,
       uintptr_t(heap?allocation->canonicalHeap.handle:allocation->texture().handle)}};
  if (heap) SetHeapAccessExtent(access, allocation->canonicalHeapOffset, allocation->canonicalHeapSize);
  return access;
}
bool HeapAttachmentViewsOverlap(const TextureViewRef &a, const TextureViewRef &b) {
  // One retained view may provide both depth and stencil, or recur in a group.
  // Distinct views must not alias physical heap storage inside that native pass.
  if (!a || !b || a.ptr() == b.ptr() || !a->allocation->canonicalHeap || !b->allocation->canonicalHeap)
    return false;
  return AccessOverlaps(TextureAccess(a->allocation, a->key, 7, true, true, 0),
      TextureAccess(b->allocation, b->key, 7, true, true, 0));
}
bool AppendSubmissionAllocation(SubmissionNode &node, BufferAllocation *allocation,
    uint64_t offset, uint64_t bytes, bool writes, uint64_t stages) {
  if (!allocation || allocation->reservedResource ||
      allocation->flags().test(BufferAllocationFlag::PlacementSparse) || offset >= allocation->byteLength())
    return false;
  if (bytes > allocation->byteLength() - offset) return false;
  node.allocation_pins.emplace_back(allocation);
  return AppendSubmissionAccess(node, BufferAccess(allocation,offset,bytes,writes,stages));
}
bool ResolveSubmissionDescriptorBudget(SubmissionNode &node, uint64_t count) {
  if (count > kSubmissionDescriptorBudget - node.descriptor_resolutions) {
    node.rejection = "descriptor_resolution_budget"; return false;
  }
  node.descriptor_resolutions += size_t(count);
  return true;
}

// With access_only, only restrictions that leave resource accesses unproven
// reject; descriptor-only merge restrictions (read-only DSV, layered or
// offset attachments, sample pattern, depth bounds, attachmentless passes,
// opaque recording endpoints)
// keep exact access summaries and are rejected for merging alone.
const char *OrdinaryRenderRejection(const RenderEncoderData &render, MTLD3D12Device *device, bool access_only = false) {
  if (render.submission_root_srv_opaque) return "unresolved_root_srv";
  if (render.submission_snapshot_budget) return "recorded_buffer_budget";
  if (!render.submission_group_safe) return "shader_or_state_unresolved";
  // The pass's own accesses do not depend on why recording closed it. The
  // opaque endpoint still forces boundary_after, so the next node resets
  // in-flight tracking and keeps its fence wait (draining a skipped one).
  if (!access_only && render.endpoint_reason == RecordedEndpointReason::Opaque) return "opaque_endpoint";
  // Native MSAA passes always carry their explicit or center sample pattern;
  // that is pass-descriptor state, not an untracked resource access.
  const bool native_msaa = render.native_msaa_sample_count > 1;
  if (render.atomic64_phase || render.msaa_draw || render.capture_indirect || render.so_indirect ||
      !render.so_draws.empty() || render.use_geometry || render.use_tessellation ||
      render.rov_marker_sample_count) return "advanced_render";
  if (!access_only &&
      ((!render.render_target_count && !render.depth.attachment && !render.stencil.attachment) ||
       render.dsv_readonly_flags || render.default_raster_sample_count ||
       (!native_msaa && (render.center_sample_pattern || render.sample_positions.count)) ||
       render.render_target_array_length > 1 || render.depth_bounds_active)) return "advanced_render";
  // Native MSAA passes keep their multisample attachments in Metal; a folded
  // ending-access resolve writes its single-sample target at the attachment
  // store. Both are fragment-stage attachment writes the planner can track.
  const uint32_t samples = render.native_msaa_sample_count ? render.native_msaa_sample_count : 1;
  // Access tracking needs only allocation identity; the 2D type proof is for merged attachment unions.
  auto plain_texture = [&](const TextureAllocation *allocation, uint32_t count) {
    return (access_only || !allocation->canonicalHeap ||
            HasFiniteHeapExtent(allocation->canonicalHeapOffset, allocation->canonicalHeapSize)) &&
        !allocation->flags().test(TextureAllocationFlag::Shared) &&
        !allocation->flags().test(TextureAllocationFlag::PlacementSparse) && allocation->descriptor->sampleCount() == count &&
        (access_only ||
         allocation->descriptor->textureType() == (count == 1 ? WMTTextureType2D : WMTTextureType2DMultisample)) &&
        !device->LookupStandardSwizzleTexture(allocation->descriptor);
  };
  // A placed DSV joins only the same plain, finite-extent family as color.
  // Preserve the existing non-heap depth-view eligibility unchanged.
  auto plain_placed_depth = [&](const auto &attachment) {
    return !attachment.attachment || !attachment.attachment->allocation->canonicalHeap ||
        (!attachment.level && !attachment.slice && !attachment.depth_plane &&
         plain_texture(attachment.attachment->allocation, samples));
  };
  if (!access_only && (!plain_placed_depth(render.depth) || !plain_placed_depth(render.stencil)))
    return "attachment_alias_or_swizzle";
  if (!access_only && HeapAttachmentViewsOverlap(render.depth.attachment, render.stencil.attachment))
    return "attachment_heap_overlap";
  for (unsigned i = 0; i < render.render_target_count; ++i) {
    const auto &color = render.colors[i];
    if (color.buffer_attachment) return "attachment_buffer";
    if (!color.attachment) continue;
    const bool folded_resolve = color.resolve_attachment && samples > 1 &&
        (color.store_action == WMTStoreActionStoreAndMultisampleResolve ||
         color.store_action == WMTStoreActionMultisampleResolve) && !color.resolve_level &&
        !color.resolve_slice && !color.resolve_depth_plane;
    if (color.buffer_attachment || (color.resolve_attachment && !folded_resolve) ||
        (!access_only && (color.level || color.slice || color.depth_plane)) ||
        (!folded_resolve && color.store_action != WMTStoreActionStore &&
                              color.store_action != WMTStoreActionDontCare) ||
        (color.load_action != WMTLoadActionLoad && color.load_action != WMTLoadActionClear))
      return "attachment_actions";
    const auto *view = color.attachment.ptr();
    const auto *allocation = view->allocation;
    if (view->castBlockSize || view->msaaSampleAddress || view->reservedCast || view->reservedVolumeRender ||
        view->reservedVolumeAtlasCast) return "attachment_cast_or_sparse";
    if (!access_only && (HeapAttachmentViewsOverlap(color.attachment, render.depth.attachment) ||
                        HeapAttachmentViewsOverlap(color.attachment, render.stencil.attachment)))
      return "attachment_heap_overlap";
    for (unsigned previous = 0; previous < i; ++previous) {
      if (!access_only && HeapAttachmentViewsOverlap(color.attachment, render.colors[previous].attachment))
        return "attachment_heap_overlap";
      if (const auto &other = render.colors[previous].attachment; other && other->allocation == allocation &&
          other->key.mip_start < view->key.mip_end && view->key.mip_start < other->key.mip_end &&
          other->key.array_start < view->key.array_end && view->key.array_start < other->key.array_end)
        return "attachment_overlap";
    }
    if (!plain_texture(allocation, samples)) return "attachment_alias_or_swizzle";
    if (folded_resolve) {
      const auto *target = color.resolve_attachment.ptr();
      if (target->castBlockSize || target->msaaSampleAddress || target->reservedCast || target->reservedVolumeRender ||
          target->reservedVolumeAtlasCast) return "attachment_cast_or_sparse";
      if (!plain_texture(target->allocation, 1)) return "attachment_alias_or_swizzle";
    }
  }
  return nullptr;
}
// No state_events inference: these summaries describe native allocation effects.
bool AppendSubmissionBuffer(SubmissionNode &node, MTLD3D12Device *device,
                            uint64_t address, uint64_t bytes, bool writes, uint64_t stages) {
  uint64_t offset = 0;
  auto allocation_owner = device->LookupBufferByVA(address, &offset);
  auto *allocation = allocation_owner.ptr();
  if (!allocation || offset >= allocation->byteLength()) return false;
  if (allocation->reservedResource || allocation->flags().test(BufferAllocationFlag::PlacementSparse)) {
    node.sparse_resource_used = true;
    return false;
  }
  const uint64_t length = bytes == UINT64_MAX ? allocation->byteLength() - offset : bytes;
  if (length > allocation->byteLength() - offset) return false;
  if (!length) return true;
  node.allocation_pins.push_back(std::move(allocation_owner));
  return AppendSubmissionAccess(node, BufferAccess(allocation,bytes == UINT64_MAX ? 0 : offset,
      bytes == UINT64_MAX ? allocation->byteLength() : length,writes,stages));
}
bool AppendSubmissionTexture(SubmissionNode &node, MTLD3D12Device *device, Texture *texture,
    TextureAllocation *allocation, TextureViewKey key, uint8_t aspects, bool writes, bool attachment, uint64_t stages,
    uint32_t samples = 1) {
  if (!texture || !allocation ||
      allocation->flags().test(TextureAllocationFlag::Shared) ||
      allocation->descriptor->sampleCount() != samples || device->LookupStandardSwizzleTexture(texture)) return false;
  if (texture->reservedResource || texture->reservedVolumeLayoutAddress ||
      allocation->flags().test(TextureAllocationFlag::PlacementSparse)) {
    node.sparse_resource_used = true;
    return false;
  }
  if (!key.mip_end || !key.array_end) return false;
  return AppendSubmissionAccess(node, TextureAccess(allocation,key,aspects,writes,attachment,stages));
}
bool ResolveSubmissionTablesUncached(SubmissionPlan &plan, SubmissionNode &node, MTLD3D12Device *device,
    const ClosedDescriptorShape &shape) {
  if (!shape.descriptors_complete) return false;
  bool complete = true;
  for (const auto &table : shape.descriptors) {
      const uint64_t count = table.count, binding_stages = table.stages;
      if (!ResolveSubmissionDescriptorBudget(node, count)) return false;
      for (uint64_t i = 0; complete && i < count; ++i) {
        ++plan.descriptor_resolutions;
        const auto &descriptor = table.heap->GetDescriptor(table.first + i);
        switch (descriptor.type) {
        case ShaderVisibleDescriptorType::Null: break;
        case ShaderVisibleDescriptorType::ConstantBuffer:
          complete &= AppendSubmissionBuffer(node, device, descriptor.ConstantBuffer.address,
              descriptor.ConstantBuffer.size, false, binding_stages); break;
        case ShaderVisibleDescriptorType::SRVTexture:
        case ShaderVisibleDescriptorType::UAVTexture: {
          const bool uav = descriptor.type == ShaderVisibleDescriptorType::UAVTexture;
          const bool writes = uav && table.writable;
          auto *texture = uav ? descriptor.UAVTexture.texture : descriptor.SRVTexture.texture;
          auto owner = device->LookupDescriptorResource(texture, descriptor.owner_id);
          if (!owner || !texture) { complete = false; break; }
          const auto key = uav ? descriptor.UAVTexture.view : descriptor.SRVTexture.view;
          // Plane reinterpretation, casts and shadow work are deliberately opaque.
          // A writable MSAA view is the exception: its import/export helper
          // passes touch only this allocation and are ordered by the node's
          // own fence wait (SubmissionNode::helper_passes).
          uint32_t samples = 1;
          if (texture->directViewOfAllocation(key)) {
            samples = texture->sampleCount();
          } else {
            if (!uav || !texture->isWritableMSAAView(key)) { complete = false; break; }
            samples = texture->sampleCount();
            node.helper_passes = true;
          }
          // A depth SRV/UAV reads the same planes depth clears and copies
          // write; record every plane of the format, never the color aspect.
          const auto planes = DepthStencilPlanarFlags(texture->pixelFormat());
          complete &= AppendSubmissionTexture(node, device, texture, texture->current(), key,
              planes ? uint8_t(planes) : uint8_t(4), writes, false, binding_stages, samples);
          if (plan.descriptor_owner_ids.insert(owner->owned_id).second) plan.descriptor_owners.push_back(std::move(owner)); break;
        }
        case ShaderVisibleDescriptorType::SRVBuffer:
        case ShaderVisibleDescriptorType::SRVRGB32Buffer:
        case ShaderVisibleDescriptorType::UAVBuffer:
        case ShaderVisibleDescriptorType::SRVTexelBuffer:
        case ShaderVisibleDescriptorType::UAVTexelBuffer: {
          const bool texel = descriptor.type == ShaderVisibleDescriptorType::SRVTexelBuffer ||
                             descriptor.type == ShaderVisibleDescriptorType::UAVTexelBuffer;
          const bool uav = descriptor.type == ShaderVisibleDescriptorType::UAVBuffer ||
                           descriptor.type == ShaderVisibleDescriptorType::UAVTexelBuffer;
          const bool writes = uav && table.writable;
          auto *buffer = texel ? descriptor.UAVTexelBuffer.buffer : descriptor.UAVBuffer.buffer;
          auto owner = device->LookupDescriptorResource(buffer, descriptor.owner_id);
          if (!owner || !buffer || (!texel && uav && descriptor.UAVBuffer.counter)) { complete = false; break; }
          const auto slice = texel ? descriptor.UAVTexelBuffer.slice : descriptor.UAVBuffer.slice;
          complete &= AppendSubmissionBuffer(node, device, buffer->current()->gpuAddress() + slice.byteOffset,
              slice.byteLength, writes, binding_stages);
          if (plan.descriptor_owner_ids.insert(owner->owned_id).second) plan.descriptor_owners.push_back(std::move(owner)); break;
        }
        default: complete = false; break;
        }
      }
      if (!complete) return false;
  }
  return complete;
}
GPUExecutionGate::Accesses AtomicAdmissionProof(MTLD3D12Device *device, const Atomic64PhaseBindings &bindings) {
  auto proof = std::make_shared<PhysicalAdmissionProof>();
  proof->resources = bindings.resources;
  for (const auto &binding : bindings.buffers) {
    auto *allocation = binding.allocation.ptr();
    if (!PlainAdmissionBuffer(allocation) || binding.byteOffset > allocation->byteLength() ||
        binding.byteLength > allocation->byteLength() - binding.byteOffset) return {};
    if (!binding.byteLength) continue;
    // Import/ordinary 8/16/32-bit accesses can share a canonical 64-bit cell.
    // Keep the whole cell inside the physical admission, including view edges.
    const uint64_t begin = binding.byteOffset & ~uint64_t(7);
    const uint64_t end = binding.byteOffset + binding.byteLength;
    const uint64_t rounded_end = end > UINT64_MAX - 7 ? allocation->byteLength() :
        std::min(allocation->byteLength(), (end + 7) & ~uint64_t(7));
    auto access = BufferAccess(allocation, begin, rounded_end - begin, true, WMTMetal4StageAll);
    if (!AppendAdmissionAccess(*proof, access, std::span(&binding.allocation, 1))) return {};
  }
  for (const auto &binding : bindings.textures) {
    auto *texture = binding.texture.ptr();
    if (!texture || !PlainAdmissionTexture(binding.allocation.ptr()) || texture->sampleCount() != 1 ||
        !texture->directViewOfAllocation(binding.view) || device->LookupStandardSwizzleTexture(texture)) return {};
    const auto planes = DepthStencilPlanarFlags(texture->pixelFormat());
    auto access = TextureAccess(binding.allocation.ptr(), binding.view, planes ? uint8_t(planes) : uint8_t(4),
        true, false, WMTMetal4StageAll);
    if (!AppendAdmissionAccess(*proof, access, {})) return {};
  }
  return ShareAdmissionProof(std::move(proof));
}

// A private SC domain must cover both the helper's actual imported/exported
// 64-bit cells and the shader's *ordinary* accesses. In particular, a valid
// phase context alone says nothing about an indirect address, a query, or
// two native objects backed by the same host pages.
bool IndependentAtomicDomain(MTLD3D12Device *device, const ClosedPassShape &shape,
    const Atomic64PhaseBindings &bindings, const GPUExecutionGate::Accesses &physical) {
  if (!physical || physical->empty() || !shape.descriptors_complete || !shape.compute_commands_complete ||
      !shape.record || shape.record->type != EncoderType::Compute) return false;
  const auto &c = *static_cast<const ComputeEncoderData *>(shape.record);
  if (!c.atomic64_phase || !c.atomic64_constants || c.atomic64_constants->atomic64_relocations.empty() ||
      !c.submission_access_complete || c.submission_snapshot_budget || c.submission_root_srv_opaque ||
      !c.atomic64_indirect.empty() || c.atomic64_ray || c.internal_pipeline || !c.cast_textures.empty() ||
      bindings.resources.size() > 256) return false;
  if (std::any_of(c.atomic64_constants->atomic64_relocations.begin(), c.atomic64_constants->atomic64_relocations.end(),
      [](const auto &r) { return r.TargetOffset == SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE; })) return false;
  // A native buffer can be Shared on UMA even for a private public resource.
  // Require its actual registered D3D12 owner, not just a native handle. A
  // root-only buffer with no such ownership proof stays in the serial domain.
  std::unordered_set<const void *> allocations;
  for (const auto &resource : bindings.resources) {
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    if (!resource || resource->share || resource->reserved || FAILED(resource->GetHeapProperties(&heap, &flags)) ||
        IsCpuVisibleHeap(&heap) || (flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)))
      return false;
    if (resource->buffer) allocations.insert(resource->buffer->current());
    if (resource->texture) allocations.insert(resource->texture->current());
  }
  for (const auto &access : *physical) if (!allocations.contains(access.allocation)) return false;
  SubmissionPlan snapshot;
  SubmissionNode shader{};
  for (const auto &buffer : c.submission_buffers)
    if (!AppendSubmissionBuffer(shader, device, buffer.address, buffer.length, buffer.writes,
                                WMTMetal4StageDispatch)) return false;
  if (!ResolveSubmissionTablesUncached(snapshot, shader, device, shape) ||
      shader.sparse_resource_used || shader.helper_passes || shader.accesses.empty()) return false;
  return std::all_of(shader.accesses.begin(), shader.accesses.end(),
      [&](const auto &access) { return AdmissionCovers(physical, access); });
}

bool ResolveSubmissionTables(SubmissionPlan &plan, SubmissionNode &node, MTLD3D12Device *device,
    const ClosedDescriptorShape &shape) {
  if (!shape.descriptors_complete) return false;
  uint64_t count = 0;
  for (const auto &table : shape.descriptors) {
    if (table.count > kSubmissionDescriptorBudget - count)
      return ResolveSubmissionTablesUncached(plan, node, device, shape);
    count += table.count;
  }
  if (!count) return true;
  auto append_cached = [&](const std::shared_ptr<const SubmissionDescriptorCache> &cache) {
    if (!ResolveSubmissionDescriptorBudget(node, cache->descriptor_count)) return false;
    plan.descriptor_caches.push_back(cache);
    node.helper_passes |= cache->helper_passes;
    for (const auto &access : cache->accesses)
      if (!AppendSubmissionAccess(node, access)) return false;
    return true;
  };
  auto cached = std::atomic_load_explicit(&shape.descriptor_cache, std::memory_order_acquire);
  if (cached && cached->Current(device)) {
    ++plan.descriptor_cache_hits;
    return append_cached(cached);
  }
  ++plan.descriptor_cache_misses;
  // Release stale cache-only owners before capturing the VA registry epoch.
  // In-flight submissions hold their own shared references until retirement.
  if (cached) {
    std::atomic_store_explicit(&shape.descriptor_cache,
        std::shared_ptr<const SubmissionDescriptorCache>{}, std::memory_order_release);
    cached.reset();
  }
  const auto va_generation = device->buffer_va_generation.load(std::memory_order_acquire);
  std::vector<SubmissionDescriptorCache::Version> versions;
  try {
    for (const auto &table : shape.descriptors) {
      const uint64_t end = uint64_t(table.first) + table.count;
      for (uint64_t index = table.first; index < end;
           index += kDescriptorVersionGranularity - index % kDescriptorVersionGranularity)
        versions.push_back({table.heap, uint32_t(index), table.heap->GetDescriptorVersion(UINT(index))});
    }
  } catch (const std::bad_alloc &) {
    return ResolveSubmissionTablesUncached(plan, node, device, shape);
  }

  // Resolve independently of root/attachment accesses, whose stage unions and
  // physical backing must still be rebuilt for each execution.
  SubmissionPlan resources;
  SubmissionNode resolved{};
  const bool complete = ResolveSubmissionTablesUncached(resources, resolved, device, shape);
  plan.descriptor_resolutions += resources.descriptor_resolutions;
  if (complete) {
    try {
      auto candidate = std::make_shared<SubmissionDescriptorCache>();
      candidate->va_generation = va_generation;
      candidate->descriptor_count = resolved.descriptor_resolutions;
      candidate->helper_passes = resolved.helper_passes;
      candidate->accesses = resolved.accesses;
      // Published D3D12 backing is stable during encoding, as required by the
      // uncached resolver. Current() also guards reuse between executions.
      for (const auto &owner : resources.descriptor_owners)
        candidate->resources.push_back({owner, Rc<BufferAllocation>(owner->buffer ? owner->buffer->current() : nullptr),
            Rc<TextureAllocation>(owner->texture ? owner->texture->current() : nullptr)});
      for (const auto &allocation : resolved.allocation_pins)
        candidate->buffers.push_back({allocation, allocation->currentSuballocationOffset()});
      candidate->versions = std::move(versions);
      if (candidate->Current(device)) {
        cached = std::move(candidate);
        std::atomic_store_explicit(&shape.descriptor_cache, cached, std::memory_order_release);
        ++plan.descriptor_cache_builds;
      }
    } catch (const std::bad_alloc &) {
      // Optional cache storage must not turn a valid uncached plan into OOM.
    }
  }
  if (cached) return append_cached(cached);

  // Incomplete/sparse/cast summaries keep the original conservative path and
  // retain any owners already resolved, without resolving the slots twice.
  node.helper_passes |= resolved.helper_passes;
  node.sparse_resource_used |= resolved.sparse_resource_used;
  if (resolved.rejection) node.rejection = resolved.rejection;
  for (auto &owner : resources.descriptor_owners)
    if (plan.descriptor_owner_ids.insert(owner->owned_id).second) plan.descriptor_owners.push_back(std::move(owner));
  for (auto &owner : resolved.allocation_pins) node.allocation_pins.push_back(std::move(owner));
  bool appended = ResolveSubmissionDescriptorBudget(node, resolved.descriptor_resolutions);
  if (appended) for (const auto &access : resolved.accesses)
    if (!AppendSubmissionAccess(node, access)) { appended = false; break; }
  return complete && appended;
}

// The encoder-wide union cannot prove dispatch independence: a later dispatch
// may read an earlier dispatch's UAV. Resolve each sealed membership separately
// using current descriptor versions and retained physical backing. Any unknown
// or conflict keeps the original serial command stream, without rewriting it.
bool IndependentComputeDispatches(SubmissionPlan &plan, SubmissionNode &node, MTLD3D12Device *device,
    const ComputeEncoderData &compute, const ClosedPassShape &shape) {
  if (!shape.dispatch_access_complete || shape.dispatches.size() < 2) {
    node.dispatch_serial_reason = "dispatch_capture"; return false;
  }
  constexpr size_t access_budget = 256, comparison_budget = 4096;
  size_t access_count = 0;
  std::unordered_map<PhysicalIdentity, std::vector<SubmissionAccess>, PhysicalIdentityHash> prior;
  for (const auto &dispatch : shape.dispatches) {
    SubmissionNode resolved{};
    bool complete = true;
    for (const auto index : dispatch.buffers) {
      if (index >= compute.submission_buffers.size()) { complete = false; break; }
      const auto &use = compute.submission_buffers[index];
      if (!AppendSubmissionBuffer(resolved, device, use.address, use.length, use.writes,
                                  use.stages ? use.stages : WMTMetal4StageDispatch)) { complete = false; break; }
    }
    if (complete) complete = ResolveSubmissionTables(plan, resolved, device, dispatch);
    for (auto &pin : resolved.allocation_pins) node.allocation_pins.push_back(std::move(pin));
    if (!complete || resolved.sparse_resource_used || resolved.helper_passes) {
      node.dispatch_serial_reason = "dispatch_access"; return false;
    }
    if (resolved.accesses.size() > access_budget - access_count) {
      node.dispatch_serial_reason = "dispatch_budget"; return false;
    }
    access_count += resolved.accesses.size();
    for (const auto &access : resolved.accesses) {
      const auto found = prior.find(access.Key());
      if (found == prior.end()) continue;
      for (const auto &earlier : found->second) {
        if (!access.writes && !earlier.writes) continue;
        if (++node.dispatch_proof_checks > comparison_budget) {
          node.dispatch_serial_reason = "dispatch_budget"; return false;
        }
        if (AccessOverlaps(access, earlier)) {
          node.dispatch_serial_reason = "dispatch_dependency"; return false;
        }
      }
    }
    // Publish only after checking the entire dispatch: its own read/write
    // aliases are intra-dispatch semantics and do not imply a preceding edge.
    for (const auto &access : resolved.accesses) prior[access.Key()].push_back(access);
  }
  node.dispatch_serial_reason = "independent";
  return true;
}
const char *AppendRenderMember(RenderReplayGroup &group, const RenderEncoderData &render, bool first,
                              std::array<uint8_t, 8> &map) {
  uint8_t bound_outputs = 0;
  for (unsigned i = 0; i < render.render_target_count; ++i)
    if (render.colors[i].attachment) bound_outputs |= 1u << i;
  if (__builtin_popcount(unsigned(render.submission_color_output_mask & ~bound_outputs)) > 1)
    return "multiple_emitted_disabled_outputs_unproved";
  if (first) {
    WMT::InitializeRenderPassInfo(group.descriptor);
    group.descriptor.render_target_width = render.render_target_width;
    group.descriptor.render_target_height = render.render_target_height;
    WMT::SetRenderTargetArrayLength(group.descriptor, render.render_target_array_length);
  } else if (group.descriptor.render_target_width != render.render_target_width ||
      group.descriptor.render_target_height != render.render_target_height ||
      group.descriptor.render_target_array_length != render.render_target_array_length) return "attachment_dimensions";
  map.fill(0xff);
  for (unsigned logical = 0; logical < render.render_target_count; ++logical) {
    const auto &src = render.colors[logical];
    if (!src.attachment) continue;
    group.placed_attachments |= bool(src.attachment->allocation->canonicalHeap);
    unsigned physical = 0;
    while (physical < group.color_count && group.colors[physical].ptr() != src.attachment.ptr()) ++physical;
    if (physical == group.color_count) {
      if (physical == 8) return "attachment_union_overflow";
      for (unsigned existing = 0; existing < group.color_count; ++existing) {
        const auto &other = group.colors[existing]; const auto &view = src.attachment;
        if (HeapAttachmentViewsOverlap(other, view)) return "attachment_union_heap_alias";
        if (other->allocation == view->allocation && other->key.mip_start < view->key.mip_end &&
            view->key.mip_start < other->key.mip_end && other->key.array_start < view->key.array_end &&
            view->key.array_start < other->key.array_end) return "attachment_union_alias";
      }
      if (HeapAttachmentViewsOverlap(src.attachment, group.depth) ||
          HeapAttachmentViewsOverlap(src.attachment, group.stencil)) return "attachment_union_heap_alias";
      if (!first && src.load_action != WMTLoadActionLoad) return "incoming_union_clear_discard";
      group.colors[physical] = src.attachment;
      ++group.color_count;
      auto &dst = group.descriptor.colors[physical];
      dst.texture = src.attachment.texture(); dst.level = src.level; dst.slice = src.slice;
      dst.depth_plane = src.depth_plane; dst.load_action = src.load_action; dst.clear_color = src.clear_color;
    } else if (!first && src.load_action != WMTLoadActionLoad) return "incoming_clear_discard";
    map[logical] = uint8_t(physical);
    group.descriptor.colors[physical].store_action = src.store_action;
  }
  group.needs_unbound_slot |= std::find(map.begin(), map.end(), 0xff) != map.end();
  if (group.color_count == 8 && group.needs_unbound_slot) return "attachment_union_no_disabled_slot";
  if (first) {
    group.depth = render.depth.attachment; group.stencil = render.stencil.attachment;
    group.placed_attachments |= (group.depth && bool(group.depth->allocation->canonicalHeap)) ||
        (group.stencil && bool(group.stencil->allocation->canonicalHeap));
    if (group.depth) {
      const auto &src = render.depth; auto &dst = group.descriptor.depth;
      dst.texture = src.attachment.texture(); dst.level = src.level; dst.slice = src.slice;
      dst.depth_plane = src.depth_plane; dst.load_action = src.load_action; dst.store_action = src.store_action;
      dst.clear_depth = src.clear_depth;
    }
    if (group.stencil) {
      const auto &src = render.stencil; auto &dst = group.descriptor.stencil;
      dst.texture = src.attachment.texture(); dst.level = src.level; dst.slice = src.slice;
      dst.depth_plane = src.depth_plane; dst.load_action = src.load_action; dst.store_action = src.store_action;
      dst.clear_stencil = src.clear_stencil;
    }
  } else if (group.depth.ptr() != render.depth.attachment.ptr() || group.stencil.ptr() != render.stencil.attachment.ptr() ||
      (group.depth && render.depth.load_action != WMTLoadActionLoad) ||
      (group.stencil && render.stencil.load_action != WMTLoadActionLoad)) return "depth_stencil_actions";
  group.descriptor.support_color_attachment_mapping = 1;
  return nullptr;
}
} // namespace

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
  // Submission labels only serve debugging tools: the D3D12 debug layer,
  // structured tracing, or a Metal capture. Each costs a string and a unix call.
  // Declared before the worker threads, so it is set before they start.
  const bool label_submissions_ = owned_graphics::TraceEnabled() || device_->runtime_configuration.debug ||
                                  env::getEnvVar("MTL_CAPTURE_ENABLED") == "1";

  WMT::Reference<WMT::MTL4CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  // Retained once: per-pass labels otherwise cost an NSString unix call each.
  WMT::Reference<WMT::String> clear_batch_label_, clear_pass_label_, resolve_pass_label_;
  WMT::Reference<WMT::Event> timestamp_order_event_;
  uint64_t timestamp_order_value_ = 0; // submission thread only
  WMT::Reference<WMT::Texture> null_shading_rate_image_; // submission thread only; never sampled
  WMT::Reference<WMT::SharedEvent> tile_mapping_event_;
  WMT::Reference<WMT::SharedEvent> tile_mapping_input_event_;
  uint64_t tile_mapping_value_ = 0; // submission thread only
  uint64_t tile_mapping_request_count_ = 0; // public FIFO order, tracing only
  /* Signal/wait pairs of this shared event flush sparse page aliases (a plain MTLEvent does not). */
  WMT::Reference<WMT::SharedEvent> alias_flush_event_;
  uint64_t alias_flush_value_ = 0; // submission thread only
  bool reserved_volume_publish_pending_ = false; // submission thread only

  void
  EncodeAliasFlush() {
    const uint64_t value = ++alias_flush_value_;
    queue_.signalEvent(alias_flush_event_, value);
    queue_.waitForEvent(alias_flush_event_, value);
    owned_graphics::Trace("sparse_alias_flush", ",\"queue\":", owned_id_, ",\"value\":", value);
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

  // Submission-thread state. Accesses of tracked encoders whose submission may
  // still run on the GPU, and the last submission holding queue work outside
  // that tracking. An encoder may skip the queue fence only when neither can
  // overlap it: Metal 4 reorders encoders across command buffers otherwise.
  // Accesses are keyed by allocation identity. Entries with an identical
  // range, subresource span, aspect set and access kind coalesce: their stages
  // union and the entry retires with the newest contributing submission, so
  // every WAR reader and every partial range stays visible until it retires.
  struct InflightAccess { uint64_t producer_stages; SubmissionAccess access; };
  struct InflightEpoch {
    uint64_t seq = UINT64_MAX;
    std::vector<InflightAccess> entries;
    std::unordered_map<PhysicalIdentity,std::vector<size_t>,PhysicalIdentityHash> slots;
    OwnedAccessFrontier<size_t> index;
  };
  // Retirement removes whole epochs, not a sweep of every allocation/range.
  std::deque<InflightEpoch> inflight_epochs_;
  size_t inflight_access_count_ = 0;
  uint64_t inflight_swept_retired_ = 0;
  bool inflight_unsealed_ = false;
  uint64_t untracked_seq_ = 0;

  // Fallback stages of an encoder type whose access summary lacks stages.
  // Metal 4 blits run on compute encoders. Unknown types are conservative.
  static uint64_t ProducerStages(EncoderType type) {
    constexpr uint64_t compute = WMTMetal4StageDispatch | WMTMetal4StageBlit | WMTMetal4StageAccelerationStructure;
    if (type == EncoderType::Render || type == EncoderType::Clear)
      return WMTMetal4StageVertex | WMTMetal4StageFragment | WMTMetal4StageTile | WMTMetal4StageObject |
          WMTMetal4StageMesh | compute;
    if (type == EncoderType::Compute || type == EncoderType::Blit) return compute;
    return WMTMetal4StageAll;
  }
  // Stages of helper work replay encodes around an access-complete node, in
  // addition to the stages its proven accesses record. Each helper touches
  // only memory the node's summary already records, so its stages attach to
  // every access of the node. Types without a proof stay conservative.
  static uint64_t HelperStages(const SubmissionNode &node) {
    const auto *record = node.record;
    if (record->type == EncoderType::Render) {
      const auto &render = *static_cast<const RenderEncoderData *>(record);
      uint64_t stages = 0;
      // Interval zero-fill and visibility reset are blits; accumulation is a dispatch.
      if (render.visibility_scratch || !render.visibility_targets.empty())
        stages |= WMTMetal4StageBlit | WMTMetal4StageDispatch;
      // Writable-MSAA import is a dispatch; export is a fragment pass.
      if (node.helper_passes) stages |= WMTMetal4StageDispatch | WMTMetal4StageFragment;
      return stages;
    }
    if (record->type == EncoderType::Compute)
      return node.helper_passes ? WMTMetal4StageDispatch | WMTMetal4StageFragment : 0;
    if (record->type == EncoderType::Blit || record->type == EncoderType::Clear) return 0;
    return WMTMetal4StageAll;
  }
  void SweepInflightAccesses(uint64_t retired) {
    if (retired == inflight_swept_retired_) return;
    inflight_swept_retired_ = retired;
    while (!inflight_epochs_.empty() && inflight_epochs_.front().seq < retired) {
      inflight_access_count_ -= inflight_epochs_.front().entries.size();
      inflight_epochs_.pop_front();
    }
  }
  // Producer stages of every in-flight access that conflicts with these
  // accesses: 0 when nothing conflicts, WMTMetal4StageAll when untracked
  // queue work may still run.
  uint64_t InflightHazard(std::span<const SubmissionAccess> accesses) {
    const uint64_t retired = submission_ring_.firstUnretired();
    if (untracked_seq_ >= retired) return WMTMetal4StageAll;
    SweepInflightAccesses(retired);
    uint64_t stages = 0;
    for (const auto &epoch : inflight_epochs_)
      for (const auto &access : accesses) epoch.index.Visit(access,[&](size_t index) {
        stages |= epoch.entries[index].producer_stages;
      });
    return stages;
  }
  void MarkUntracked(uint64_t seq) {
    untracked_seq_ = seq;
    inflight_epochs_.clear();
    inflight_access_count_ = 0;
    inflight_unsealed_ = false;
  }
  static bool SameAccessSpan(const SubmissionAccess &a, const SubmissionAccess &b) {
    return a.begin == b.begin && a.end == b.end && a.mip_begin == b.mip_begin && a.mip_end == b.mip_end &&
        a.slice_begin == b.slice_begin && a.slice_end == b.slice_end && a.aspects == b.aspects &&
        a.writes == b.writes && a.attachment == b.attachment && a.Key() == b.Key();
  }
  // Producer stages are per access: its recorded stages plus the stages of
  // helper work around it, or the encoder type's fallback when it has none.
  void RecordInflightAccesses(std::span<const SubmissionAccess> accesses, uint64_t helper_stages,
                              uint64_t fallback_stages) {
    for (const auto &access : accesses) {
      if (inflight_epochs_.empty() || inflight_epochs_.back().seq != UINT64_MAX) inflight_epochs_.emplace_back();
      auto &epoch = inflight_epochs_.back();
      auto &slots = epoch.slots[access.Key()];
      const uint64_t producer = !access.stages ? fallback_stages
          : access.stages | helper_stages | (access.attachment ? uint64_t(WMTMetal4StageTile) : 0);
      // A node can be encoded into a later segment than the one current at
      // planning time, so its sequence is assigned when the Execute finishes.
      auto same = std::find_if(slots.begin(), slots.end(),
          [&](size_t index) { return SameAccessSpan(epoch.entries[index].access, access); });
      if (same != slots.end()) {
        auto &entry = epoch.entries[*same]; entry.producer_stages |= producer; entry.access.stages |= access.stages;
      } else {
        // Same bound as one plan's group span; beyond it tracking degrades to fences.
        if (inflight_access_count_ == kSubmissionGroupBudget * kSubmissionRangeBudget) {
          MarkUntracked(UINT64_MAX);
          owned_graphics::Trace("native_inflight_fallback", ",\"entries\":", kSubmissionGroupBudget * kSubmissionRangeBudget,
                                ",\"fallback_count\":1");
          return;
        }
        const size_t index = epoch.entries.size();
        epoch.entries.push_back({producer, access}); slots.push_back(index);
        epoch.index.Publish(access,index,false);
        ++inflight_access_count_;
      }
      inflight_unsealed_ = true;
    }
  }
  void SealInflightAccesses(uint64_t seq) {
    if (inflight_unsealed_ && !inflight_epochs_.empty()) inflight_epochs_.back().seq = seq;
    inflight_unsealed_ = false;
    if (untracked_seq_ == UINT64_MAX) untracked_seq_ = seq;
    if (fence_chain_broken_seq_ == UINT64_MAX) fence_chain_broken_seq_ = seq;
  }
  // A fence wait orders only after the most recent fence update. An encoder
  // that skipped its wait may still run when a later one updates the fence,
  // so before the next fence waiter one empty encoder waits for all earlier
  // queue work and updates the fence, restoring the chain for every waiter.
  // The broken chain carries across submissions: every fence waiter drains
  // first (replay nodes that keep their wait, Present), and every other queue
  // consumer orders by CPU retirement or queue-level events, which cover
  // skipped encoders of earlier command buffers.
  bool fence_chain_broken_ = false;
  // Newest submission holding a skipped wait. A skipped node may be encoded
  // into a later segment than the one current at its decision, so the value
  // stays unknown until the Execute seals its final sequence.
  uint64_t fence_chain_broken_seq_ = 0;
  void BreakFenceChain() {
    fence_chain_broken_ = true;
    fence_chain_broken_seq_ = UINT64_MAX;
  }
  void DrainSkippedWork(WMT::MTL4CommandBuffer cmdbuf, uint64_t seq) {
    if (!fence_chain_broken_) return;
    fence_chain_broken_ = false;
    // Retired skipped work already precedes every later fence waiter.
    if (fence_chain_broken_seq_ < submission_ring_.firstUnretired()) {
      owned_graphics::Trace("native_skipped_drain_retired", ",\"submission\":", seq, ",\"elided_count\":1");
      return;
    }
    auto drain = cmdbuf.blitCommandEncoder();
    drain.barrierAfterQueueStages(WMTMetal4StageAll, WMTMetal4StageBlit, WMTMetal4VisibilityDevice);
    drain.updateFence(fence_);
    drain.endEncoding();
    owned_graphics::Trace("native_skipped_drain", ",\"submission\":", seq, ",\"barrier_count\":1");
  }
  using OwnedHandle = std::unique_ptr<void, decltype([](HANDLE handle) { CloseHandle(handle); })>;

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
    std::shared_ptr<D3DalloyPresentLatency> latency;
    D3DalloyPresentComposition composition;
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
    uint32_t request_count = 1;
  };

  using QueueTask = std::variant<ExecuteTask, SignalTask, WaitTask, PresentTask, TileMappingTask, MarkerTask>;
  owned_graphics::QueueTaskFifo<QueueTask> task_fifo_;
  d3dalloy::mutex mutex_enqueue_;
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
    std::shared_ptr<D3DalloyPresentLatency> latency;
    // Allocator GPU argument storage uses newBufferWithBytesNoCopy. Retaining
    // its MTLBuffer alone does not retain the allocator's malloc backing.
    // Keep that owner until the submitted command buffer has completed.
    std::vector<SubmissionProtection> protections;
    std::shared_ptr<const SubmissionResourcePins> resolved_resources;
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
    // Execution-time AS table snapshots cannot use a recording allocator: the
    // same closed list may be submitted again before its previous execution retires.
    // Native buffers never move; each helper receives a distinct aligned slice.
    static constexpr uint64_t kASMetadataBlockSize = 64 * 1024;
    static constexpr uint64_t kASMetadataRetainedBytes = 256 * 1024;
    struct ASMetadataBlock {
      WMT::Reference<WMT::Buffer> buffer;
      uint8_t *memory;
      uint64_t capacity, used;
      uint64_t id = owned_graphics::NewObjectId();
    };
    struct ASMetadataSlice {
      WMT::Buffer buffer{};
      uint8_t *memory = nullptr;
      uint64_t offset = 0;
      uint64_t block_id = 0;
    };
    std::vector<ASMetadataBlock> as_metadata_blocks;
    size_t as_metadata_active = 0;

    ASMetadataSlice AllocateASMetadata(WMT::Device device, uint64_t bytes) {
      if (!bytes || bytes > SIZE_MAX) return {};
      for (; as_metadata_active < as_metadata_blocks.size(); ++as_metadata_active) {
        auto &block = as_metadata_blocks[as_metadata_active];
        const uint64_t padding = (-block.used) & 15u;
        if (padding > block.capacity - block.used || bytes > block.capacity - block.used - padding) continue;
        const uint64_t offset = block.used + padding;
        block.used = offset + bytes;
        return {block.buffer, block.memory + offset, offset, block.id};
      }
      // Reserve metadata before allocating the native buffer. Allocation failure
      // leaves all earlier slices and their owners unchanged.
      if (as_metadata_blocks.size() == as_metadata_blocks.capacity())
        as_metadata_blocks.reserve(as_metadata_blocks.size() + std::max<size_t>(as_metadata_blocks.size(), 1));
      WMTBufferInfo info{};
      info.length = std::max(bytes, kASMetadataBlockSize);
      info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
      auto buffer = device.newBuffer(info);
      auto *memory = static_cast<uint8_t *>(info.memory.get_accessible_or_null());
      if (!buffer || !memory) return {};
      as_metadata_blocks.push_back({std::move(buffer), memory, info.length, bytes});
      as_metadata_active = as_metadata_blocks.size() - 1;
      return {as_metadata_blocks.back().buffer, memory, 0, as_metadata_blocks.back().id};
    }

    // Prepared commands retain feature history, native views, and private
    // scratch until Metal completion, independently of frontend ReleaseFeature.
    std::vector<std::shared_ptr<OwnedTemporalFrame>> temporal_frames;
    std::vector<std::shared_ptr<OwnedFrameGenerationFrame>> frame_generation_frames;
    std::vector<std::shared_ptr<OwnedRayRegenerationFrame>> ray_regeneration_frames;
    uint64_t cpu_submit_ns = 0;
    std::vector<SignalTask> fence_completions;
    // Retain the presenter until GPU completion, not merely task encoding.
    Rc<Presenter> present_completion;
    std::shared_ptr<D3DalloyPresentHistory> present_history;
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
      bool native_present_attempted = false;
      bool native_present_reserved = false;
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
    // Only the batched swizzle staging allocator appends here. Other swizzle
    // helpers also retain non-staging buffers in swizzle_buffers.
    std::vector<WMT::Reference<WMT::Buffer>> swizzle_batch_buffers;
    uint64_t swizzle_batch_scratch_length = 0;
    std::vector<WMT::Reference<WMT::Buffer>> swizzle_dirty_flags;
    std::vector<WMT::Reference<WMT::Buffer>> swizzle_baselines;
    std::vector<Com<MTLD3D12Resource, false>> swizzle_resources;
    // This recording owns both allocations, even for equality inherited from
    // an earlier public Execute. No owner transfers to the next recording.
    struct SwizzleRepresentation {
      uint64_t resource_id;
      Rc<BufferAllocation> canonical;
      Rc<TextureAllocation> shadow;
      obj_handle_t canonical_handle, shadow_handle;
      const standard_swizzle::Layout *layout;
      std::array<uint64_t, 4> valid{};
      std::array<uint64_t, 4> inherited{};
      bool allow_carry = false;
      uint64_t source_queue = 0, source_submission = 0;
      uint64_t content_version = 0, synchronized_version = UINT64_MAX, writer_serial = 0, writer_encoder = 0;

      bool Contains(uint32_t subresource) const {
        return synchronized_version == content_version &&
               (valid[subresource / 64] & (uint64_t(1) << (subresource % 64)));
      }
      void Invalidate(uint64_t serial, uint64_t encoder) {
        valid.fill(0);
        inherited.fill(0);
        ++content_version;
        writer_serial = serial;
        writer_encoder = encoder;
      }
    };
    std::vector<SwizzleRepresentation> swizzle_representations;
    uint64_t swizzle_encoding_serial = 0, swizzle_encoder_id = 0, swizzle_submission = 0;
    uint64_t swizzle_predecessor = 0;
    bool swizzle_validity_enabled = false;
    void InvalidateSwizzleRepresentations() {
      swizzle_predecessor = 0; // Also invalidate previous entries not looked up in this recording yet.
      // Keep owners until retirement even when their proof is invalidated.
      for (auto &entry : swizzle_representations) entry.Invalidate(swizzle_encoding_serial, swizzle_encoder_id);
    }
    std::vector<TextureViewRef> cast_views;
    std::vector<Rc<TextureAllocation>> cast_allocations;
    std::vector<WMT::Reference<WMT::Heap>> volume_heaps;
    std::vector<WMT::Reference<WMT::Texture>> volume_textures;
    struct MappingResource {
      Com<MTLD3D12Resource, false> owner;
      uint64_t generation;
    };
    std::vector<MappingResource> mapping_resources;
    std::vector<WMT::Reference<WMT::Heap>> mapping_heaps;
    WMT::Reference<WMT::MTL4Submission> mapping_pre_submission;
    std::vector<WMT::Reference<WMT::SharedEvent>> mapping_join_events;
    OwnedSparseHeapIndex::Pending mapping_pending;
    uint64_t mapping_ready_value = 0;
    void ResetRetired() {
      InflightCommandBuffer fresh;
      // Only this completed submission owned these slices. Keep at most four
      // ordinary blocks per ring slot; oversized and excess blocks retire now.
      uint64_t retained_metadata_bytes = 0;
      std::erase_if(as_metadata_blocks, [&](const auto &block) {
        if (block.capacity != kASMetadataBlockSize ||
            block.capacity > kASMetadataRetainedBytes - retained_metadata_bytes) return true;
        retained_metadata_bytes += block.capacity;
        return false;
      });
      for (auto &block : as_metadata_blocks) block.used = 0;
      if (as_metadata_blocks.capacity() <= (256 * 1024) / sizeof(ASMetadataBlock))
        as_metadata_blocks.swap(fresh.as_metadata_blocks);
      auto keep = [](auto &from, auto &to) {
        from.clear(); // All referenced owners must be gone before ring retirement.
        using Element = typename std::remove_reference_t<decltype(from)>::value_type;
        if (from.capacity() <= (256 * 1024) / sizeof(Element)) from.swap(to);
      };
      keep(breadcrumbs, fresh.breadcrumbs); keep(protections, fresh.protections);
      keep(rtas_observations, fresh.rtas_observations); keep(temporal_frames, fresh.temporal_frames);
      keep(frame_generation_frames, fresh.frame_generation_frames); keep(ray_regeneration_frames, fresh.ray_regeneration_frames);
      keep(so_draws, fresh.so_draws); keep(so_indirect_buffers, fresh.so_indirect_buffers);
      keep(mesh_indirect_owners, fresh.mesh_indirect_owners); keep(so_indirect_icbs, fresh.so_indirect_icbs);
      keep(msaa_buffers, fresh.msaa_buffers); keep(msaa_textures, fresh.msaa_textures); keep(rov_markers, fresh.rov_markers);
      keep(swizzle_buffers, fresh.swizzle_buffers); keep(swizzle_dirty_flags, fresh.swizzle_dirty_flags);
      keep(swizzle_batch_buffers, fresh.swizzle_batch_buffers);
      keep(swizzle_baselines, fresh.swizzle_baselines); keep(swizzle_resources, fresh.swizzle_resources);
      keep(swizzle_representations, fresh.swizzle_representations);
      keep(cast_views, fresh.cast_views); keep(cast_allocations, fresh.cast_allocations);
      keep(volume_heaps, fresh.volume_heaps); keep(volume_textures, fresh.volume_textures);
      keep(mapping_heaps, fresh.mapping_heaps);
      *this = std::move(fresh);
    }
  };

  struct NativeEncodingSlot {
    WMT::Reference<WMT::MTL4CommandAllocator> allocator;
    WMT::Reference<WMT::MTL4CommandBuffer> cmdbuf;
    // Lazily allocated, bounded by the same 32 retirement slots as ordinary
    // command storage. Neither allocator is reset while its mapping is pending.
    WMT::Reference<WMT::MTL4CommandAllocator> mapping_allocator;
    WMT::Reference<WMT::MTL4CommandBuffer> mapping_cmdbuf;
    WMT::Reference<WMT::Buffer> mapping_marker;
  };
  std::array<NativeEncodingSlot, kCommandQueueSize> native_encoding_slots_;
  std::array<InflightCommandBuffer, kCommandQueueSize> inflight_cmdbuf_pool_;
  // Submission thread only. Keep values, not resource or Metal allocation
  // references: retaining a fixed number of arbitrary-size textures is not a
  // memory bound. An eligible committed texture's canonical/shadow backings
  // and layout are initialized once for its unique public resource lifetime.
  std::array<MTLD3D12Device::SwizzleCarry, 64> swizzle_carry_;
  size_t swizzle_carry_count_ = 0;

  void LoadSwizzleCarry(InflightCommandBuffer &inflight) {
    const auto serial = inflight.execution_ticket.OrderedAdmissionSerial();
    swizzle_carry_count_ = 0;
    if (!serial) return;
    auto &published = device_->swizzle_proofs;
    std::lock_guard lock(published.mutex);
    if (published.serial != serial - 1 || !published.count) return;
    swizzle_carry_count_ = published.count;
    std::copy_n(published.entries.begin(), published.count, swizzle_carry_.begin());
    inflight.swizzle_predecessor = published.serial;
  }

  void PublishSwizzleCarry(const InflightCommandBuffer &inflight, uint64_t submission) {
    const auto serial = inflight.execution_ticket.OrderedAdmissionSerial();
    if (!serial) return;
    auto &published = device_->swizzle_proofs;
    std::lock_guard lock(published.mutex);
    if (published.serial >= serial) return;
    published.serial = serial;
    published.count = 0;
    for (const auto &entry : inflight.swizzle_representations) {
      if (!entry.allow_carry || entry.content_version != entry.synchronized_version ||
          std::none_of(entry.valid.begin(), entry.valid.end(), [](uint64_t bits) { return bits != 0; }))
        continue;
      published.entries[published.count++] = {entry.resource_id, entry.canonical_handle, entry.shadow_handle,
          uintptr_t(entry.layout), entry.valid, owned_id_, submission};
    }
    // FindSwizzleRepresentation removes touched resources from the snapshot.
    if (inflight.swizzle_predecessor) {
      const auto count = std::min(swizzle_carry_count_, published.entries.size() - published.count);
      std::copy_n(swizzle_carry_.begin(), count, published.entries.begin() + published.count);
      published.count += count;
    }
  }
  // Queue ownership avoids a device -> cached resource -> device cycle. The
  // destructor joins both threads and retires submissions before members die.
  std::shared_ptr<Atomic64PhaseCache> atomic64_phase_cache_;
  d3dalloy::thread inflight_cmdbuf_wait_thread_;
  d3dalloy::thread submission_thread_;

  d3dalloy::mutex mutex_commit_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("d3dalloy-cmdbuf-waiting-thread");
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
      WMTMetal4SubmissionInfo mapping_pre{};
      if (inflight.mapping_pre_submission) {
        inflight.mapping_pre_submission.waitUntilCompleted();
        inflight.mapping_pre_submission.getInfo(mapping_pre);
      }
      const bool mapping_error = inflight.mapping_pending &&
          (!inflight.mapping_pre_submission || !mapping_pre.completed || mapping_pre.error_handle);
      const bool metal_error = !inflight.submission || !completion.completed || completion.error_handle || mapping_error;
      if (inflight.mapping_pending) {
        inflight.mapping_pending.Retire(!metal_error);
        for (const auto &entry : inflight.mapping_resources)
          owned_graphics::Trace("tile_mapping_retire", ",\"queue\":", owned_id_, ",\"submission\":", internal_seq,
                                ",\"resource\":", entry.owner->owned_id,
                                ",\"generation\":", entry.generation, ",\"success\":", !metal_error);
      }
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
        // A failed mapping may not execute its native ready signal. Release
        // internal GPU waiters only after device removal; public fence values
        // still cannot complete, and their native owners retire normally.
        if (inflight.mapping_ready_value && tile_mapping_event_.signaledValue() < inflight.mapping_ready_value)
          tile_mapping_event_.signalValue(inflight.mapping_ready_value);
      } else {
        for (const auto &draw : inflight.so_draws)
          if (draw->status_cpu && draw->status_cpu[0]) {
            ERR("Draw capture: ", draw->status_cpu[0], " primitives lost an unclaimed capture record");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
          }
      }

      owned_graphics::Trace("complete", ",\"queue\":", owned_id_, ",\"submission\":", internal_seq,
                            ",\"status\":", uint32_t(metal_error ? WMTCommandBufferStatusError : WMTCommandBufferStatusCompleted),
                            ",\"cpu_submit_ns\":", inflight.cpu_submit_ns,
                            ",\"cpu_complete_ns\":", owned_graphics::NowNs(),
                            ",\"gpu_start_ns\":", completion.gpu_start_ns,
                            ",\"gpu_end_ns\":", completion.gpu_end_ns,
                            ",\"present_frame_id\":", inflight.native_present ? inflight.native_present->frame_id : 0);
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
          // A reserved present reached Core Animation at commit (D3DMetal 4.0b3
          // DoPresent); only an unreserved one is presented or withdrawn here.
          const bool presented = present.native_present_reserved ||
                                 (!present.native_present_attempted && !metal_error && current);
          if (!present.native_present_attempted && !metal_error && current)
            WMT4::PresentMetal4Drawable(present.drawable, present.after,
                                       present.surface ? &present.frame : nullptr, present.sync_interval,
                                       present.surface ? present.allow_tearing : false);
          else if (!presented && present.observer)
            present.observer.cancel(present.present_id);
          owned_graphics::Trace("present_native_decision", ",\"queue\":", queue_id,
                                ",\"present_id\":", present.present_id,
                                ",\"reserved\":", unsigned(present.native_present_reserved),
                                ",\"presented\":", unsigned(presented));
          if (owned_graphics::TraceEnabled())
            Logger::info(str::format("OWNED_GRAPHICS_NATIVE_PRESENT_DECISION present_id=", present.present_id,
                                     " reserved=", unsigned(present.native_present_reserved),
                                     ", presented=", unsigned(presented),
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
      device_->CompleteRTASSubmission(inflight.rtas_submission, inflight.rtas_observations, !metal_error);

      // A public fence completion is an allocator-retirement watermark too:
      // after the app observes it, Reset of every preceding allocator must be
      // immediately legal. Release all native and allocator leases first.
      auto fence_completions = std::move(inflight.fence_completions);
      if (!metal_error)
        for (const auto &breadcrumb : inflight.breadcrumbs) device_->diagnostics.Complete(breadcrumb);
      inflight.ResetRetired();
      submission_ring_.retire(internal_seq);
      if (device_->RemovedReason() == S_OK)
        for (auto &completion : fence_completions)
          completion.fence->markQueueSignalCompleted(completion.ticket);

      internal_seq++;
    }
  };

  struct CommittingScope {
    MTLD3D12CommandQueueImpl *queue;
    GPUExecutionGate::Ticket execution_ticket;
    std::lock_guard<d3dalloy::mutex> lock;
    GPUExecutionGate::EncodingAdmission encoding_admission;
    uint64_t seq;
    InflightCommandBuffer &inflight;
    WMT::Reference<WMT::Object> pool;
    // Queue-level event signals issued right after this command buffer commits.
    std::vector<std::pair<WMT::Reference<WMT::Event>, uint64_t>> signals;
    // Sparse publications use the device mapping queue but the same bounded
    // owner/retirement ring. Keep its global enqueue lock through commit/signal.
    WMT::MTL4CommandQueue native_queue{};
    std::unique_lock<std::mutex> mapping_enqueue_lock;
    bool timestamp_predecessors_ordered = false;
    bool publish_swizzle_carry = false;
    // Per-feature reservations survive commit and event publication. Ring
    // capacity waits and blocking acquisition never hold another reservation.
    OwnedTemporalSubmissionLocks temporal_submission_locks;

    static GPUExecutionGate::Ticket Admit(MTLD3D12CommandQueueImpl *queue, bool exclusive,
                                          GPUExecutionGate::Accesses known, bool track_queue) {
      auto ticket = exclusive ? GPUExecutionGate::Ticket{} :
          queue->device_->gpu_execution_gate.AcquireKnownShared(std::move(known), track_queue ? queue->owned_id_ : 0);
      if (!exclusive && !ticket) throw HRESULT(DXGI_ERROR_DEVICE_REMOVED);
      return ticket;
    }
    // Exclusive phase callers already own the device ticket and retain it through retirement.
    CommittingScope(MTLD3D12CommandQueueImpl *queue, bool exclusive = false, GPUExecutionGate::Accesses known = {},
                    bool track_queue = false) :
        queue(queue),
        execution_ticket(Admit(queue, exclusive, std::move(known), track_queue)),
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
      if (queue->label_submissions_) {
        auto label = str::format("D3D12 queue ", queue->owned_id_, " submission ", seq);
        if (inflight.cmdbuf.diagnostic(3, label.data(), label.size()))
          queue->device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      }
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
      inflight.cpu_submit_ns = owned_graphics::NowNs();
      queue->device_->reflex.Submitted(inflight.reflex_work, inflight.cpu_submit_ns);
      auto target = native_queue ? native_queue : WMT::MTL4CommandQueue(queue->queue_);
      inflight.submission = queue->device_->CommitMetal4CommandBuffers(target, &buffer, 1);
      if (publish_swizzle_carry && inflight.submission)
        queue->PublishSwizzleCarry(inflight, seq);
      for (const auto &[event, value] : signals)
        target.signalEvent(event, value);
      owned_graphics::Trace("submit", ",\"queue\":", queue->owned_id_, ",\"submission\":", seq,
                            ",\"cpu_submit_ns\":", inflight.cpu_submit_ns);
      queue->submission_ring_.publish(seq);
    }
  };

  CommittingScope
  StartCommitting() {
    return CommittingScope(this);
  }

  void ProcessTask(QueueTask &task) {
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
    } else if (auto *present = std::get_if<PresentTask>(&task)) {
      CancelPresent(*present);
    }
  }

  static constexpr size_t kMappingRunLimit = 16;
  void
  ProcessTileMappingRun(TileMappingTask &first, std::optional<QueueTask> &pending_task) {
    constexpr size_t max_tiles = 4096;
    struct Interval { MTLD3D12Resource *resource; uint64_t begin, end; };
    auto interval = [](const TileMappingTask &item, Interval &range) {
      if (item.copy_source || item.no_hazard || item.assignments.empty() || item.assignments.size() > max_tiles)
        return false;
      range = {item.resource.ptr(), item.assignments.front().first,
               uint64_t(item.assignments.front().first) + item.assignments.size()};
      if (item.assignments.back().first != range.end - 1) return false;
      for (size_t i = 1; i + 1 < item.assignments.size(); ++i)
        if (item.assignments[i].first != range.begin + i) return false;
      return true;
    };
    std::array<Interval, kMappingRunLimit> ranges;
    if (!interval(first, ranges[0])) return Process(first);
    std::array<TileMappingTask, kMappingRunLimit> groups;
    size_t tiles = first.assignments.size(), count = 1, requests = 1;
    groups[0] = std::move(first);
    QueueTask next;
    while (requests < kMappingRunLimit && tiles < max_tiles && task_fifo_.tryPop(next)) {
      auto *update = std::get_if<TileMappingTask>(&next);
      auto &range = ranges[requests];
      if (!update || !interval(*update, range) || update->assignments.size() > max_tiles - tiles ||
          std::any_of(ranges.begin(), ranges.begin() + requests, [&](const Interval &prior) {
            return prior.resource == range.resource && prior.begin < range.end && range.begin < prior.end;
          })) {
        pending_task = std::move(next);
        break;
      }
      tiles += update->assignments.size();
      ++requests;
      auto &last = groups[count - 1];
      if (last.resource.ptr() == range.resource) {
        last.heaps.insert(last.heaps.end(), std::make_move_iterator(update->heaps.begin()),
                          std::make_move_iterator(update->heaps.end()));
        last.assignments.insert(last.assignments.end(), update->assignments.begin(), update->assignments.end());
        ++last.request_count;
      } else {
        groups[count++] = std::move(*update);
      }
    }
    ProcessTileMapping({groups.data(), count});
  }

  void
  SubmissionThread() {
    env::setThreadName("d3dalloy-d3d12-submit-thread");
    QueueTask task;
    std::optional<QueueTask> pending_task;
    while (pending_task || task_fifo_.waitPop(task)) {
      if (pending_task) {
        task = std::move(*pending_task);
        pending_task.reset();
      }
      if (auto *mapping = std::get_if<TileMappingTask>(&task);
          mapping && !cancel_unsubmitted_ && device_->RemovedReason() == S_OK) {
        try {
          ProcessTileMappingRun(*mapping, pending_task);
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
        } catch (HRESULT reason) {
          device_->ReportRemoval(reason);
        }
      } else if (auto *signal = std::get_if<SignalTask>(&task);
          signal && !cancel_unsubmitted_ && device_->RemovedReason() == S_OK) {
        try {
          std::vector<SignalTask> signals;
          signals.push_back(std::move(*signal));
          QueueTask next;
          while (task_fifo_.tryPop(next)) {
            if (auto *next_signal = std::get_if<SignalTask>(&next))
              signals.push_back(std::move(*next_signal));
            else {
              pending_task = std::move(next);
              break;
            }
          }
          ProcessSignalBatch(signals);
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
        } catch (HRESULT reason) {
          device_->ReportRemoval(reason);
        }
      } else if (auto *execute = std::get_if<ExecuteTask>(&task);
                 execute && !cancel_unsubmitted_ && device_->RemovedReason() == S_OK) {
        // beta3 FoldedSignal: signals already queued behind an Execute ride its
        // final commit. Any other task ends the run and is processed next.
        std::vector<SignalTask> signals;
        try {
          QueueTask next;
          while (task_fifo_.tryPop(next)) {
            if (auto *next_signal = std::get_if<SignalTask>(&next))
              signals.push_back(std::move(*next_signal));
            else {
              pending_task = std::move(next);
              break;
            }
          }
          ProcessExecute(*execute, &signals, &pending_task);
          if (!signals.empty()) ProcessSignalBatch(signals);
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
        } catch (HRESULT reason) {
          device_->ReportRemoval(reason);
        }
      } else if (auto *present = std::get_if<PresentTask>(&task);
                 present && !cancel_unsubmitted_ && device_->RemovedReason() == S_OK) {
        // Signals queued directly behind a Present retire with its command
        // buffer instead of an empty submission. A Present that encodes nothing
        // (restart skip, cancellation) leaves them for an ordinary batch after it.
        std::vector<SignalTask> signals;
        try {
          QueueTask next;
          while (task_fifo_.tryPop(next)) {
            if (auto *next_signal = std::get_if<SignalTask>(&next))
              signals.push_back(std::move(*next_signal));
            else {
              pending_task = std::move(next);
              break;
            }
          }
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
        }
        try {
          ProcessPresent(*present, &signals);
        } catch (HRESULT reason) {
          device_->ReportRemoval(reason);
          CancelPresent(*present);
        }
        try {
          if (!signals.empty()) ProcessSignalBatch(signals);
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
        }
      } else {
        ProcessTask(task);
      }
      // Release allocator/fence/presenter/resource references as soon as the
      // task has been encoded instead of retaining the last task while idle.
      task = ExecuteTask{};
    }
  }

  void Process(ExecuteTask &task) { ProcessExecute(task); }
  void Process(SignalTask &task) { ProcessSignal(task); }
  void Process(WaitTask &task) { ProcessWait(task); }
  void Process(PresentTask &task) { ProcessPresent(task); }
  void Process(TileMappingTask &task) { ProcessTileMapping({&task, 1}); }
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

  // A bare MTL4 queue event wait does not order placement sparse updates. An
  // All -> ResourceState producer barrier bridges that wait to the mappings;
  // ResourceState -> Blit orders status publication and actual GPU retirement.
  void
  ProcessTileMapping(std::span<TileMappingTask> tasks) {
    if (device_->RemovedReason() != S_OK) return;
    try {
      const uint64_t first_request = tile_mapping_request_count_ + 1;
      for (const auto &task : tasks) tile_mapping_request_count_ += task.request_count;
      auto resolve_copy = [&] {
        auto &task = tasks.front(); // A copy always forms its own run.
        if (task.copy_source)
          for (size_t i = 0; i < task.copy_source_tiles.size(); ++i)
            task.assignments[i].second = task.copy_source->reserved->CurrentTarget(task.copy_source_tiles[i]);
      };
      auto trace = [&](const TileMappingTask &task, uint64_t request, bool changed, uint64_t seq, bool joined = false) {
        owned_graphics::Trace("tile_mapping", ",\"queue\":", owned_id_, ",\"resource\":", task.resource->owned_id,
                              ",\"tiles\":", task.assignments.size(), ",\"requests\":", task.request_count,
                              ",\"first_request\":", request, ",\"groups\":", tasks.size(),
                              ",\"changed\":", changed,
                              ",\"native_completion_waits\":0,\"prior_submission_waits\":0,\"submission\":", seq,
                              ",\"gpu_dependencies\":", changed ? (task.no_hazard ? 1 : 2) : unsigned(joined),
                              ",\"pending_join\":", joined,
                              ",\"resource_state_barriers\":", changed ? 2 : 0,
                              ",\"generation\":", task.resource->reserved->MappingGeneration());
      };
      {
        // Do not reserve/submit a native command for a repeated target. A later
        // concurrent writer is checked again after admission and ring reserve.
        std::lock_guard lock(ReservedResource::MappingQueueMutex);
        resolve_copy();
        const bool ready = std::all_of(tasks.begin(), tasks.end(), [&](const TileMappingTask &task) {
          auto &reserved = *task.resource->reserved;
          if (reserved.HasMappingChanges(task.assignments)) return false;
          auto pending = reserved.NativeMappingReady();
          return !pending.event || pending.event.handle == tile_mapping_event_.handle ||
                 pending.event.signaledValue() >= pending.value;
        });
        if (ready) {
          auto request = first_request;
          for (const auto &task : tasks) {
            trace(task, request, false, 0);
            request += task.request_count;
          }
          return;
        }
      }
      // Admission and ring backpressure must never wait with MappingQueueMutex
      // held. The shared ticket remains with this publication until retirement.
      auto scope = StartCommitting();
      auto &inflight = scope.inflight;
      auto &slot = native_encoding_slots_[scope.seq % kCommandQueueSize];
      if (!slot.mapping_allocator) slot.mapping_allocator = device_->GetMTLDevice().newMTL4CommandAllocator();
      if (!slot.mapping_cmdbuf) slot.mapping_cmdbuf = device_->GetMTLDevice().newMTL4CommandBuffer();
      if (!slot.mapping_marker) {
        WMTBufferInfo info{};
        info.length = sizeof(uint32_t);
        info.options = WMTResourceStorageModePrivate;
        slot.mapping_marker = device_->GetMTLDevice().newBuffer(info);
      }
      auto mapping_queue = device_->GetSparseMappingQueue();
      if (!slot.mapping_allocator || !slot.mapping_cmdbuf || !slot.mapping_marker || !mapping_queue)
        throw HRESULT(E_OUTOFMEMORY);
      scope.signals.reserve(1);
      scope.mapping_enqueue_lock = std::unique_lock(ReservedResource::MappingQueueMutex);
      resolve_copy();
      std::array<bool, kMappingRunLimit> changed{}, joined{};
      bool any_changed = false;
      for (size_t i = 0; i < tasks.size(); ++i) {
        auto &reserved = *tasks[i].resource->reserved;
        changed[i] = reserved.HasMappingChanges(tasks[i].assignments);
        any_changed |= changed[i];
        if (changed[i]) continue;
        auto ready = reserved.NativeMappingReady();
        if (ready.event && ready.event.handle != tile_mapping_event_.handle &&
            ready.event.signaledValue() < ready.value) {
          inflight.mapping_join_events.push_back(ready.event);
          queue_.waitForEvent(ready.event, ready.value);
          joined[i] = true;
        }
      }
      auto trace_run = [&](uint64_t seq) {
        auto request = first_request;
        for (size_t i = 0; i < tasks.size(); ++i) {
          trace(tasks[i], request, changed[i], seq, joined[i]);
          request += tasks[i].request_count;
        }
      };
      if (!any_changed && inflight.mapping_join_events.empty()) { trace_run(scope.seq); return; }
      wmtcmd_blit_fillbuffer marker{};
      marker.type = WMTBlitCommandFillBuffer;
      marker.buffer = slot.mapping_marker;
      marker.length = sizeof(uint32_t);
      inflight.cmdbuf.useAllocation(slot.mapping_marker);
      if (!any_changed) {
        auto encoder = inflight.cmdbuf.blitCommandEncoder();
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&marker));
        encoder.endEncoding();
        MarkUntracked(scope.seq);
        trace_run(scope.seq);
        return;
      }
      if (tile_mapping_value_ == UINT64_MAX) throw HRESULT(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      const uint64_t value = ++tile_mapping_value_;
      scope.native_queue = mapping_queue;
      scope.signals.emplace_back(tile_mapping_event_, value);
      inflight.mapping_resources.reserve(tasks.size());
      inflight.mapping_pending = ReservedResource::BeginNativeMapping();
      inflight.mapping_ready_value = value;
      inflight.cmdbuf.useResidencySet(device_->GetGlobalResidencySet());
      if (!tasks.front().no_hazard) {
        queue_.signalEvent(tile_mapping_input_event_, value);
        mapping_queue.waitForEvent(tile_mapping_input_event_, value);
      }
      slot.mapping_allocator.reset();
      slot.mapping_cmdbuf.begin(slot.mapping_allocator);
      auto before = slot.mapping_cmdbuf.blitCommandEncoder();
      before.barrierAfterStages(WMTMetal4StageAll, WMTMetal4StageResourceState,
                               WMTMetal4VisibilityDevice | WMTMetal4VisibilityResourceAlias);
      before.endEncoding();
      slot.mapping_cmdbuf.end();
      const WMT::MTL4CommandBuffer pre = slot.mapping_cmdbuf;
      inflight.mapping_pre_submission = device_->CommitMetal4CommandBuffers(mapping_queue, &pre, 1);
      if (!inflight.mapping_pre_submission) throw HRESULT(DXGI_ERROR_DRIVER_INTERNAL_ERROR);

      auto publication = inflight.cmdbuf.blitCommandEncoder();
      publication.barrierAfterQueueStages(WMTMetal4StageResourceState, WMTMetal4StageBlit,
                                         WMTMetal4VisibilityDevice | WMTMetal4VisibilityResourceAlias);
      // A real blit anchors completion even for a texture with no status table.
      // Thus post-command feedback cannot retire its heaps before ResourceState.
      publication.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&marker));
      try {
        for (size_t i = 0; i < tasks.size(); ++i) {
          if (!changed[i]) continue;
          auto &resource = tasks[i].resource;
          auto &reserved = *resource->reserved;
          WMT::Buffer buffer{};
          WMT::Texture texture{};
          if (resource->buffer) buffer = resource->buffer->current()->buffer();
          else texture = resource->texture->current()->texture();
          inflight.cmdbuf.useAllocation(WMT::Allocation{buffer ? buffer.handle : texture.handle});
          if (reserved.status_buffer) inflight.cmdbuf.useAllocation(reserved.status_buffer);
          // Apply can fail after encoding a native update. Keep its resource alive on that path too.
          inflight.mapping_resources.push_back({resource, 0});
          reserved.Apply(mapping_queue, buffer, texture, tasks[i].assignments, publication, &inflight.mapping_heaps);
          inflight.mapping_resources.back().generation = reserved.MappingGeneration();
          reserved.PublishMappingReady(tile_mapping_event_, value);
        }
      } catch (const std::bad_alloc &) {
        device_->ReportRemoval(E_OUTOFMEMORY);
        publication.endEncoding();
        throw;
      } catch (...) {
        device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
        publication.endEncoding();
        throw HRESULT(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
      }
      for (const auto &heap : inflight.mapping_heaps) inflight.cmdbuf.useAllocation(heap);
      publication.barrierAfterStages(WMTMetal4StageAll, WMTMetal4StageAll,
                                    WMTMetal4VisibilityDevice | WMTMetal4VisibilityResourceAlias);
      publication.endEncoding();
      // All changed resources share this marker and its retained heap owners.
      // This wait orders subsequent public submissions, including Signal. The
      // ready signal and post-command commit occur before releasing the enqueue
      // lock, so another queue cannot insert a remap ahead of this publication.
      queue_.waitForEvent(tile_mapping_event_, value);
      MarkUntracked(scope.seq);
      trace_run(scope.seq);
    } catch (const std::bad_alloc &) {
      device_->ReportRemoval(E_OUTOFMEMORY);
    }
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
      std::lock_guard<d3dalloy::mutex> enqueue_lock(mutex_enqueue_);
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
    if (task.completion_handle && task.credit &&
        task.credit->exchange(false, std::memory_order_acq_rel))
      task.latency->complete(task.completion_handle.get());
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
    std::lock_guard<d3dalloy::mutex> lock(mutex_commit_);
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
    clear_batch_label_ = WMT::MakeString("SubmissionClearBatch", WMTUTF8StringEncoding);
    clear_pass_label_ = WMT::MakeString("ClearPass", WMTUTF8StringEncoding);
    resolve_pass_label_ = WMT::MakeString("ResolvePass", WMTUTF8StringEncoding);
    if (!clear_batch_label_ || !clear_pass_label_ || !resolve_pass_label_)
      return E_OUTOFMEMORY;
    timestamp_order_event_ = metal_device.newEvent();
    if (!timestamp_order_event_)
      return E_FAIL;
    tile_mapping_event_ = metal_device.newSharedEvent();
    tile_mapping_input_event_ = metal_device.newSharedEvent();
    if (!tile_mapping_event_ || !tile_mapping_input_event_)
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

  /* Assemble each captured draw after its render pass in API order. */
  void
  EncodeStreamOutputAssembly(WMT::MTL4CommandBuffer cmdbuf, const StreamOutputDrawData &draw) {
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_nop head{};
    head.type = WMTComputeCommandNop;
    wmtcmd_compute_setpso set_pso{};
    set_pso.type = WMTComputeCommandSetPSO;
    set_pso.pso = draw.assemble_pipeline;
    uint32_t lanes = draw.parallel_list_assembly ? 32 : 1;
    set_pso.threadgroup_size = {lanes, 1, 1};
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
    dispatch.size = {lanes, 1, 1};
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

  struct CaptureFrontierExecution {
    CaptureFrontierParams params{};
    WMT::Reference<WMT::Buffer> state, parameters, completion;
    WMT::Reference<WMT::Buffer> roots, vertices, scratch, records, replay_indices;
    WMT::Reference<WMT::Buffer> carry_scratch, carry_records, tess_scratch, tess_list, expanded, claim;
    std::vector<WMT::Reference<WMT::Buffer>> buffers;
    CaptureFrontierCompletion *mailbox = nullptr;
    uint64_t layouts_offset = 0, point_offset = 0;
    uint64_t participant_bound = 0;
    std::optional<MSAADrawData> msaa_draw;
    MSAAExecution msaa;
  };

  // All capacities depend on a PSO's proven output bound and a fixed amount of
  // resident work. No application count or draw dimension is read by the host.
  bool PrepareCaptureFrontier(const CaptureFrontierEncoderData &node, CaptureFrontierExecution &execution) {
    using resource_contract::CheckedAdd;
    using resource_contract::CheckedMul;
    static_assert(sizeof(CaptureFrontierShaderControl) == sizeof(SM50_CAPTURE_FRONTIER_CONTROL));
    static_assert(offsetof(CaptureFrontierShaderControl, first_patch) ==
                  offsetof(SM50_CAPTURE_FRONTIER_CONTROL, first_patch));
    static_assert(sizeof(CaptureFrontierVertexControl) == sizeof(SM50_MSAA_VERTEX_CAPTURE_CONTROL));
    if (!node.producer || !node.consumer) return false;
    const auto &render = *node.producer;
    const auto *vertex = render.so_indirect.get();
    const auto *mesh = render.capture_indirect.get();
    if (bool(vertex) == bool(mesh)) return false;
    auto metal = device_->GetMTLDevice();
    const uint64_t maximum = metal.maxBufferLength();
    auto &p = execution.params;
    const auto &source = vertex ? vertex->preflight : mesh->preflight;
    p.Arguments = source.argument_buffer;
    p.Count = source.count_buffer;
    p.Predicate = source.predicate;
    p.FixedIndexAddress = source.index_buffer;
    p.FixedIndexFormat = source.index_format;
    p.FixedIndexSize = vertex ? vertex->index_view_size : mesh->index_view_size;
    p.MaxCommands = source.max_count;
    p.ArgumentStride = source.stride;
    p.DrawOffset = source.draw_offset;
    p.IndexViewOffset = source.ib_offset;
    p.PredicateNotEqual = source.predicate_op;
    p.Flags = source.indexed ? kCaptureFrontierIndexed : 0;
    if (vertex && vertex->frontier_replay_pipeline) p.Flags |= kCaptureFrontierRaster;
    if (render.atomic64_phase || node.consumer->atomic64_phase) p.Flags |= kCaptureFrontierAtomic;
    p.Kind = vertex ? kCaptureFrontierVertex : mesh->geometry ? kCaptureFrontierGeometry :
        mesh->domain_only ? kCaptureFrontierDomain : kCaptureFrontierTessellationGeometry;
    p.StagedIndexViews = vertex ? vertex->index_view_address : mesh->index_view_address;
    p.IndexViewStride = vertex ? vertex->index_view_stride : mesh->index_view_stride;
    p.RootStride = vertex ? vertex->root_stride : mesh->root_stride;
    p.VertexStride = vertex ? vertex->vertex_stride : mesh->vertex_stride;
    p.StagedRoots = vertex ? vertex->root_address : mesh->root_address;
    p.StagedVertices = vertex ? vertex->vertex_address : mesh->vertex_address;
    if (p.RootStride > UINT32_MAX || p.VertexStride > UINT32_MAX) return false;
    p.RootBytes = uint32_t(p.RootStride);
    p.VertexTableBytes = uint32_t(p.VertexStride);
    if ((p.RootBytes && !p.StagedRoots) || (p.VertexTableBytes && !p.StagedVertices)) return false;
    p.StripCut = vertex ? vertex->strip_cut : uint32_t(mesh->strip_cut_value);
    p.PipelineStatistics = vertex ? vertex->command.pipeline_statistics : mesh->statistics_address;
    p.GSInstances = mesh ? std::max(mesh->gs_instances, 1u) : 1;
    p.Topology = vertex ? vertex->assembly_template->topology : mesh->topology;
    p.KeyCapacity = kCaptureFrontierVertexCapacity + kCaptureFrontierCarryCapacity;
    p.PrimitiveCapacity = kCaptureFrontierVertexCapacity;
    auto retain_buffer = [&](WMT::Buffer buffer) {
      if (buffer && std::none_of(execution.buffers.begin(), execution.buffers.end(),
          [&](const auto &held) { return held.handle == buffer.handle; })) execution.buffers.push_back(buffer);
    };
    if (vertex) {
      retain_buffer(vertex->root_heap);
      retain_buffer(vertex->vertex_heap);
      retain_buffer(vertex->index_view_heap);
      retain_buffer(vertex->static_sampler_buffer);
    } else {
      retain_buffer(mesh->root_heap);
      retain_buffer(mesh->vertex_heap);
      retain_buffer(mesh->index_view_heap);
      retain_buffer(mesh->index_buffer);
      retain_buffer(mesh->predicate_heap);
      retain_buffer(mesh->statistics_buffer);
    }
    if (mesh) {
      if (mesh->strip_expand) p.Flags |= kCaptureFrontierExpandStrip;
      p.Flags |= kCaptureFrontierRaster;
      p.VertexPerWarp = mesh->vertex_per_warp;
      p.WarpIncrement = mesh->warp_increment;
      p.PrimitivesPerGroup = mesh->geometry ? mesh->strip_expand
          ? mesh->vertex_per_warp / mesh->strip_vertices_per_primitive
          : uint32_t(MeshCaptureInputPrimitives(mesh->topology, mesh->vertex_per_warp)) : mesh->per_group;
      if (!p.PrimitivesPerGroup) return false;
      uint64_t keys = uint64_t(p.PrimitivesPerGroup) * p.GSInstances;
      if (!mesh->geometry) {
        if (!mesh->threads_per_patch || 32 % mesh->threads_per_patch || !mesh->control_points) return false;
        p.ControlPoints = mesh->control_points;
        p.PatchesPerGroup = 32 / mesh->threads_per_patch;
        p.TessWorkloads = mesh->domain_only && !mesh->so_template ? mesh->msaa_layout.max_workloads :
            mesh->layout.max_workloads;
        p.TessVertices = mesh->layout.max_vertices;
        p.TessPrimitives = mesh->layout.max_primitives;
        p.TessRegisters = mesh->layout.vertex_registers;
        p.TessPrimitiveVertices = mesh->layout.primitive_vertices;
        const uint64_t slots = uint64_t(p.PatchesPerGroup) * p.TessWorkloads;
        if (!slots || slots > UINT32_MAX) return false;
        p.ParentSlotCapacity = uint32_t(slots);
        if (mesh->domain_only) keys = slots;
      }
      if (!keys || keys > UINT32_MAX) return false;
      p.KeyCapacity = uint32_t(keys);
      p.PrimitiveCapacity = 1; // Native mesh rasterization consumes its own bounded output.
    }
    std::array<StreamOutputAssemblyParams, 4> layouts{};
    auto draw = vertex ? vertex->draw_template : mesh->so_template;
    uint64_t key_words = 0;
    for (; draw; draw = draw->next_stream) {
      if (p.StreamCount == layouts.size() || !draw->params_cpu) return false;
      const uint32_t stream = p.StreamCount++;
      retain_buffer(draw->counters_heap);
      retain_buffer(draw->statistics_heap);
      retain_buffer(draw->predicate_heap);
      retain_buffer(draw->index_buffer);
      layouts[stream] = *draw->params_cpu;
      const auto &layout = layouts[stream];
      auto &binding = p.Streams[stream];
      binding.Counters = draw->counters_address;
      binding.Statistics = draw->statistics_address;
      binding.PrefixDwords = draw->scratch_prefix_dwords;
      if ((layout.stream_output_enabled && !binding.Counters) ||
          (layout.statistics_enabled && !binding.Statistics)) return false;
      for (unsigned slot = 0; slot < 4; ++slot) {
        if (!draw->targets[slot]) continue;
        binding.Targets[slot] = draw->targets[slot]->gpuAddress() + draw->target_offsets[slot];
        binding.FilledSizes[slot] = draw->filled_sizes[slot]->gpuAddress() + draw->filled_size_offsets[slot];
        retain_buffer(draw->targets[slot]->buffer());
        retain_buffer(draw->filled_sizes[slot]->buffer());
      }
      if (vertex) {
        p.SORecordDwords = layout.record_dwords;
        key_words = uint64_t(layout.record_dwords) + 1;
        if (layout.rasterization_enabled) p.Flags |= kCaptureFrontierRaster;
      } else {
        uint64_t words = 0;
        if (!CheckedMul(layout.vertex_count, uint64_t(layout.record_dwords) + 1, words) ||
            !CheckedAdd(words, 1, words) || !CheckedAdd(key_words, words, key_words)) return false;
      }
    }
    if (!CheckedMul(p.KeyCapacity, key_words, p.ScratchBytes) ||
        !CheckedMul(p.ScratchBytes, 4, p.ScratchBytes)) return false;
    const auto *msaa = node.consumer->msaa_draw.get();
    if (msaa) {
      execution.msaa_draw = *msaa;
      p.Flags |= kCaptureFrontierMSAA;
      p.LineMode = uint32_t(msaa->line_mode);
      p.Conservative = msaa->conservative;
      p.MSAAIndexStride = MSAAIndexStride(msaa->line_mode, msaa->conservative);
      p.MSAARecordDwords = msaa->vertex_layout.record_dwords;
      p.VertexRecordDwords = p.MSAARecordDwords;
      if (msaa->mesh_capture) {
        p.MeshOutputPrimitives = msaa->mesh_layout.max_primitives;
        p.MeshOutputVertices = msaa->mesh_layout.vertices_per_primitive;
        uint64_t primitives = 0;
        if (!CheckedMul(p.KeyCapacity, p.MeshOutputPrimitives, primitives) || primitives > UINT32_MAX ||
            !CheckedMul(p.KeyCapacity, msaa->mesh_layout.capture_dwords_per_group, p.VertexBytes) ||
            !CheckedMul(p.VertexBytes, 4, p.VertexBytes)) return false;
        p.PrimitiveCapacity = uint32_t(primitives);
      }
    } else if (vertex) p.VertexRecordDwords = vertex->frontier_vertex_layout.record_dwords;
    if (vertex && (!p.VertexRecordDwords || !CheckedMul(p.KeyCapacity, p.VertexRecordDwords, p.VertexBytes) ||
        !CheckedMul(p.VertexBytes, 4, p.VertexBytes))) return false;
    if (p.Kind == kCaptureFrontierTessellationGeometry) {
      uint64_t vertices = 0, indices = 0, words = 0;
      if (!CheckedMul(p.ParentSlotCapacity, p.TessVertices, vertices) ||
          !CheckedMul(vertices, p.TessRegisters, vertices) || !CheckedMul(vertices, 4, vertices) ||
          !CheckedMul(p.ParentSlotCapacity, p.TessPrimitives, indices) || indices > UINT32_MAX ||
          !CheckedMul(indices, p.TessPrimitiveVertices, indices) ||
          !CheckedAdd((uint64_t(p.ParentSlotCapacity) + 3) & ~uint64_t(3), vertices, words) ||
          !CheckedAdd(words, indices, words) || !CheckedMul(words, 4, p.TessScratchBytes) ||
          !CheckedMul(p.ParentSlotCapacity, p.TessPrimitives, words) ||
          !CheckedAdd(words, 4, words) || !CheckedMul(words, 4, p.TessListBytes)) return false;
    }
    auto allocate = [&](WMT::Reference<WMT::Buffer> &target, uint64_t bytes, uint64_t &address,
                        const void *initial = nullptr, void **mapped = nullptr) {
      if (bytes > maximum || bytes > SIZE_MAX) return false;
      WMTBufferInfo info{};
      info.length = std::max<uint64_t>(bytes, 16);
      info.options = WMTResourceHazardTrackingModeUntracked |
          ((initial || mapped) ? WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE : WMTResourceStorageModePrivate);
      target = metal.newBuffer(info);
      if (!target) return false;
      address = info.gpu_address;
      auto *memory = info.memory.get_accessible_or_null();
      if ((initial || mapped) && !memory) return false;
      if (initial && bytes) memcpy(memory, initial, size_t(bytes));
      if (mapped) *mapped = memory;
      execution.buffers.push_back(target);
      return true;
    };
    uint64_t unused = 0;
    CaptureFrontierState initial{};
    CaptureFrontierCompletion completion{};
    void *mailbox = nullptr;
    if (!allocate(execution.state, sizeof(initial), unused, &initial) ||
        !allocate(execution.completion, sizeof(completion), p.Completion, &completion, &mailbox)) return false;
    execution.mailbox = static_cast<CaptureFrontierCompletion *>(mailbox);
    const uint64_t root_bytes = vertex ? uint64_t(vertex->command.rootsig_qwords_stride) * 8 : p.RootBytes;
    const uint64_t vertex_bytes = vertex ? vertex->command.vertex_argbuf_stride : p.VertexTableBytes;
    if (root_bytes && !allocate(execution.roots, root_bytes, p.ActiveRoots, vertex ? vertex->root_arguments : nullptr))
      return false;
    if (vertex_bytes && !allocate(execution.vertices, vertex_bytes, p.ActiveVertices, vertex ? vertex->vertex_buffers : nullptr))
      return false;
    if (!allocate(execution.scratch, p.ScratchBytes, p.Scratch) ||
        !allocate(execution.records, p.VertexBytes, p.VertexRecords)) return false;
    if (vertex) {
      if (!allocate(execution.carry_scratch, uint64_t(kCaptureFrontierCarryCapacity) * p.SORecordDwords * 4, p.CarryScratch) ||
          !allocate(execution.carry_records, uint64_t(kCaptureFrontierCarryCapacity) * p.VertexRecordDwords * 4, p.CarryVertexRecords) ||
          !allocate(execution.replay_indices, uint64_t(p.PrimitiveCapacity) * 3 * 4, p.ReplayIndices)) return false;
    }
    if (p.TessScratchBytes && (!allocate(execution.tess_scratch, p.TessScratchBytes, p.TessScratch) ||
        !allocate(execution.tess_list, p.TessListBytes, p.TessList))) return false;
    if (p.Flags & kCaptureFrontierExpandStrip) {
      const uint64_t bytes = uint64_t(p.PrimitivesPerGroup) * mesh->strip_vertices_per_primitive * 4;
      if (!allocate(execution.expanded, bytes, p.ExpandedIndices)) return false;
    }
    if (mesh && mesh->frontier_statistics_claim.samples) {
      auto header = mesh->frontier_statistics_claim;
      uint64_t bytes = 0;
      if (!CheckedMul(header.width, header.height, bytes) || !CheckedMul(bytes, header.layers, bytes) ||
          !CheckedMul(bytes, header.samples, bytes) || !CheckedMul(bytes, 16, bytes)) return false;
      p.StatisticsClaimBytes = std::max<uint64_t>(bytes, 16);
      if (!CheckedAdd(p.StatisticsClaimBytes, sizeof(header), bytes)) return false;
      void *memory = nullptr;
      if (!allocate(execution.claim, bytes, p.StatisticsClaim, nullptr, &memory)) return false;
      header.entries = sizeof(header) / 4;
      header.epoch = 0;
      header.mesh_groups = 0;
      memcpy(memory, &header, sizeof(header));
      p.StatisticsClaimEntries = p.StatisticsClaim + sizeof(header);
    }
    if (msaa) {
      auto &runtime = execution.msaa;
      auto &draw = *execution.msaa_draw;
      p.MSAAIndicesBytes = uint64_t(p.PrimitiveCapacity) * p.MSAAIndexStride * 4;
      p.MSAADrawsBytes = uint64_t(p.PrimitiveCapacity) * sizeof(MSAAPrimitiveIndirectArguments);
      p.MSAABoundsBytes = uint64_t(p.PrimitiveCapacity) * sizeof(MSAAPrimitiveTileBounds);
      void *bounds = nullptr;
      if (!allocate(runtime.indices, p.MSAAIndicesBytes, p.MSAAIndices) ||
          !allocate(runtime.draws, p.MSAADrawsBytes, p.MSAADraws) ||
          !allocate(runtime.bounds, p.MSAABoundsBytes, p.MSAABounds, nullptr,
                    draw.gpu_tile_tasks ? nullptr : &bounds)) return false;
      runtime.vertices = execution.records;
      runtime.max_primitives = p.PrimitiveCapacity;
      runtime.mapped_bounds = static_cast<const MSAAPrimitiveTileBounds *>(bounds);
      draw.bounds.max_primitives = p.PrimitiveCapacity;
      draw.assembly.max_primitives = p.PrimitiveCapacity;
      draw.root_buffer = execution.roots ? execution.roots : draw.root_buffer;
      if (execution.roots) draw.root_offset = 0;
      draw.statistics_claim = execution.claim;
      draw.statistics_claim_offset = 0;
      std::array<uint8_t, sizeof(MSAAPrimitiveAssemblyParams) + sizeof(MSAAPrimitiveBoundsParams)> parameters{};
      memcpy(parameters.data(), &draw.assembly, sizeof(draw.assembly));
      memcpy(parameters.data() + sizeof(draw.assembly), &draw.bounds, sizeof(draw.bounds));
      if (!allocate(runtime.parameters, parameters.size(), p.MSAAAssembly, parameters.data())) return false;
    }
    const uint64_t clear_bytes = std::max({p.ScratchBytes, p.VertexBytes, p.TessScratchBytes, p.TessListBytes,
        p.MSAAIndicesBytes, p.MSAADrawsBytes, p.MSAABoundsBytes, p.StatisticsClaimBytes});
    if (clear_bytes / 4 > UINT32_MAX) return false;
    p.ClearDwords = uint32_t((clear_bytes + 3) / 4);
    execution.layouts_offset = (sizeof(p) + 15) & ~uint64_t(15);
    execution.point_offset = execution.layouts_offset + sizeof(layouts);
    std::vector<uint8_t> parameters(execution.point_offset + 16, 0);
    memcpy(parameters.data(), &p, sizeof(p));
    memcpy(parameters.data() + execution.layouts_offset, layouts.data(), sizeof(layouts));
    const uint32_t one = 1;
    memcpy(parameters.data() + execution.point_offset + 4, &one, sizeof(one));
    if (!allocate(execution.parameters, parameters.size(), unused, parameters.data())) return false;
    const uint64_t producer_bound = vertex ? kCaptureFrontierVertexCapacity : mesh->geometry
        ? uint64_t(p.VertexPerWarp) + uint64_t(p.PrimitivesPerGroup) * p.GSInstances
        : 32 + uint64_t(p.ParentSlotCapacity) * 32 + uint64_t(p.PrimitivesPerGroup) * (32 + p.GSInstances);
    if (!CheckedMul(producer_bound, kCaptureFrontierQuantum, execution.participant_bound)) return false;
    return true;
  }

  void RetainCaptureFrontier(InflightCommandBuffer &inflight, const CaptureFrontierExecution &execution) {
    // Reserve before any encoder uses these allocations. Every submitted
    // quantum and every host consumer retains them to its real retirement.
    inflight.so_indirect_buffers.reserve(inflight.so_indirect_buffers.size() + execution.buffers.size());
    inflight.so_indirect_buffers.insert(inflight.so_indirect_buffers.end(), execution.buffers.begin(), execution.buffers.end());
    // Completion, shaded carry, expanded indices and staged tables can be
    // reached only through GPU addresses. Register them on every quantum,
    // including the compute-only NO_RASTER path and resumed submissions.
    for (const auto &buffer : execution.buffers)
      inflight.cmdbuf.useAllocation(WMT::Allocation{buffer.handle});
  }

  void EncodeCaptureFrontierKernel(WMT::MTL4CommandBuffer cmdbuf, const CaptureFrontierExecution &execution,
                                  WMT::ComputePipelineState pipeline, uint32_t point = 0, bool clear = false) {
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = pipeline;
    pso.threadgroup_size = {clear ? 64u : 1u, 1, 1};
    const std::array<std::pair<WMT::Buffer, uint64_t>, 4> bindings = {{
        {execution.state, 0}, {execution.parameters, 0},
        {execution.parameters, execution.layouts_offset}, {execution.parameters, execution.point_offset + point * 4}}};
    for (uint8_t slot = 0; slot < bindings.size(); ++slot) {
      wmtcmd_compute_setbuffer binding{};
      binding.type = WMTComputeCommandSetBuffer;
      binding.buffer = bindings[slot].first;
      binding.offset = bindings[slot].second;
      binding.index = slot;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
    }
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {1, 1, 1};
    wmtcmd_compute_dispatch_indirect indirect{};
    indirect.type = WMTComputeCommandDispatchIndirect;
    indirect.indirect_args_buffer = execution.state;
    indirect.indirect_args_offset = offsetof(CaptureFrontierState, Indirect) + offsetof(CaptureFrontierIndirect, Clear);
    pso.next.set(clear ? static_cast<void *>(&indirect) : static_cast<void *>(&dispatch));
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

  void EncodeCaptureFrontierVertex(WMT::MTL4CommandBuffer cmdbuf, const CaptureFrontierEncoderData &node,
                                  const CaptureFrontierExecution &execution) {
    const auto &render = *node.producer;
    const auto &data = *render.so_indirect;
    const auto *msaa = execution.msaa_draw ? &*execution.msaa_draw : nullptr;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    auto bind = [&](WMT::Buffer buffer, uint64_t offset, uint8_t index) {
      wmtcmd_compute_setbuffer binding{};
      binding.type = WMTComputeCommandSetBuffer;
      binding.buffer = buffer;
      binding.offset = offset;
      binding.index = index;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
    };
    std::array<std::pair<WMT::Buffer, uint64_t>, 32> recorded{};
    for (auto *command = static_cast<const wmtcmd_base *>(render.cmd_head.next.get()); command;
         command = static_cast<const wmtcmd_base *>(command->next.get())) {
      if (command->type == WMTRenderCommandSetVertexBuffer) {
        const auto &buffer = *reinterpret_cast<const wmtcmd_render_setbuffer *>(command);
        if (buffer.index < recorded.size()) recorded[buffer.index] = {WMT::Buffer{buffer.buffer}, buffer.offset};
      } else if (command->type == WMTRenderCommandSetVertexBufferOffset) {
        const auto &buffer = *reinterpret_cast<const wmtcmd_render_setbufferoffset *>(command);
        if (buffer.index < recorded.size()) recorded[buffer.index].second = buffer.offset;
      }
    }
    for (uint8_t index = 0; index < recorded.size(); ++index)
      if (recorded[index].first) bind(recorded[index].first, recorded[index].second, index);
    if (execution.roots) bind(execution.roots, 0, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
    if (execution.vertices) bind(execution.vertices, 0, SM50_BINDING_INDEX_VERTEX_BUFFER);
    if (data.static_sampler_buffer)
      bind(data.static_sampler_buffer, data.static_sampler_offset, SM50_BINDING_INDEX_STATIC_SAMPLERS);
    bind(execution.state, offsetof(CaptureFrontierState, Shader), SM50_BINDING_INDEX_CAPTURE_FRONTIER);
    bind(execution.state, offsetof(CaptureFrontierState, ActiveIndex), SM50_BINDING_INDEX_VERTEX_INDICES);
    bind(execution.state, offsetof(CaptureFrontierState, Vertex), SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL);
    bind(execution.scratch, 0, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
    bind(execution.records, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
    const WMT::Buffer constants = msaa ? WMT::Buffer(msaa->constants[0]) : data.frontier_capture_constants;
    const uint64_t constants_offset = msaa ? msaa->constant_offsets[0] : data.frontier_capture_constants_offset;
    if (constants) bind(constants, constants_offset, SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = msaa ? msaa->capture_pipeline : data.frontier_capture_pipeline;
    pso.threadgroup_size = {64, 1, 1};
    wmtcmd_compute_dispatch_indirect dispatch{};
    dispatch.type = WMTComputeCommandDispatchIndirect;
    dispatch.indirect_args_buffer = execution.state;
    dispatch.indirect_args_offset = offsetof(CaptureFrontierState, Indirect) + offsetof(CaptureFrontierIndirect, Vertex);
    pso.next.set(&dispatch);
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

  static WMTRenderPassInfo CaptureFrontierRenderPass(const RenderEncoderData &render, bool attachments,
                                                    bool initial = false, bool final = false) {
    WMTRenderPassInfo info{};
    WMT::InitializeRenderPassInfo(info);
    info.render_target_width = std::max(render.render_target_width, 1u);
    info.render_target_height = std::max(render.render_target_height, 1u);
    WMT::SetRenderTargetArrayLength(info, std::max(1u, uint32_t(render.render_target_array_length)));
    info.default_raster_sample_count = attachments ? render.default_raster_sample_count : 1;
    info.center_sample_pattern = attachments && render.center_sample_pattern;
    if (attachments) info.sample_positions = render.sample_positions;
    info.visibility_buffer = render.visibility_scratch;
    if (!attachments) return info;
    for (size_t i = 0; i < render.colors.size(); ++i) {
      const auto &source = render.colors[i];
      if (!source.attachment && !source.buffer_attachment) continue;
      auto &color = info.colors[i];
      color.texture = source.buffer_attachment ? WMT::Texture(source.buffer_attachment) : source.attachment.texture();
      color.level = source.level;
      color.slice = source.slice;
      color.depth_plane = source.depth_plane;
      color.load_action = initial ? source.load_action : WMTLoadActionLoad;
      color.store_action = final ? source.store_action : WMTStoreActionStore;
      color.clear_color = source.clear_color;
      if (final) {
        color.resolve_texture = source.resolve_attachment.texture();
        color.resolve_level = source.resolve_level;
        color.resolve_slice = source.resolve_slice;
        color.resolve_depth_plane = source.resolve_depth_plane;
      }
    }
    if (render.depth.attachment) {
      const auto &source = render.depth;
      info.depth.texture = source.attachment.texture();
      info.depth.level = source.level;
      info.depth.slice = source.slice;
      info.depth.depth_plane = source.depth_plane;
      info.depth.load_action = initial ? source.load_action : WMTLoadActionLoad;
      info.depth.store_action = final ? source.store_action : WMTStoreActionStore;
      info.depth.clear_depth = source.clear_depth;
    }
    if (render.stencil.attachment) {
      const auto &source = render.stencil;
      info.stencil.texture = source.attachment.texture();
      info.stencil.level = source.level;
      info.stencil.slice = source.slice;
      info.stencil.depth_plane = source.depth_plane;
      info.stencil.load_action = initial ? source.load_action : WMTLoadActionLoad;
      info.stencil.store_action = final ? source.store_action : WMTStoreActionStore;
      info.stencil.clear_stencil = source.clear_stencil;
    }
    return info;
  }

  void EncodeCaptureFrontierVisibility(WMT::MTL4CommandBuffer cmdbuf, const RenderEncoderData &render,
                                      bool reset) {
    for (const auto &target : render.visibility_targets) {
      if (target.reset != reset) continue;
      if (reset) {
        auto encoder = cmdbuf.blitCommandEncoder();
        encoder.waitForFence(fence_);
        wmtcmd_blit_fillbuffer fill{};
        fill.type = WMTBlitCommandFillBuffer;
        fill.buffer = target.destination;
        fill.offset = uint64_t(target.index) * sizeof(uint64_t);
        fill.length = sizeof(uint64_t);
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
        encoder.updateFence(fence_);
        encoder.endEncoding();
        continue;
      }
      auto encoder = cmdbuf.computeCommandEncoder(false);
      encoder.waitForFence(fence_);
      wmtcmd_compute_setpso pso{};
      pso.type = WMTComputeCommandSetPSO;
      pso.pso = target.accumulate_pipeline;
      pso.threadgroup_size = {1, 1, 1};
      wmtcmd_compute_setbuffer source{}, destination{};
      source.type = destination.type = WMTComputeCommandSetBuffer;
      source.buffer = render.visibility_scratch;
      source.offset = target.source_offset;
      destination.buffer = target.destination;
      destination.offset = uint64_t(target.index) * sizeof(uint64_t);
      destination.index = 1;
      wmtcmd_compute_dispatch dispatch{};
      dispatch.type = WMTComputeCommandDispatchThreads;
      dispatch.size = {1, 1, 1};
      pso.next.set(&source);
      source.next.set(&destination);
      destination.next.set(&dispatch);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
      encoder.updateFence(fence_);
      encoder.endEncoding();
    }
  }

  void EncodeCaptureFrontierAttachments(WMT::MTL4CommandBuffer cmdbuf, const RenderEncoderData &render, bool final) {
    if (!final) {
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.waitForFence(fence_);
      for (const auto &color : render.colors) {
        if (!color.buffer_attachment) continue;
        wmtcmd_blit_copy_from_buffer_to_texture copy{};
        copy.type = WMTBlitCommandCopyFromBufferToTexture;
        copy.src = color.buffer_backing;
        copy.src_offset = color.buffer_offset;
        copy.bytes_per_row = copy.bytes_per_image = color.buffer_bytes_per_row;
        copy.size = {color.buffer_width, 1, 1};
        copy.dst = color.buffer_attachment;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      }
      encoder.updateFence(fence_);
      encoder.endEncoding();
    }
    // Empty pass applies initial clears or final store/resolve exactly once.
    // Intermediate chunks always load and store the same attachment contents.
    auto info = CaptureFrontierRenderPass(render, true, !final, final);
    info.visibility_buffer = {};
    auto encoder = cmdbuf.renderCommandEncoder(info);
    encoder.waitForFence(fence_, WMTRenderStagePreRaster | WMTRenderStageFragment);
    encoder.updateFence(fence_, WMTRenderStagePreRaster | WMTRenderStageFragment);
    encoder.endEncoding();
    if (final) {
      auto copy_encoder = cmdbuf.blitCommandEncoder();
      copy_encoder.waitForFence(fence_);
      for (const auto &color : render.colors) {
        if (!color.buffer_attachment) continue;
        wmtcmd_blit_copy_from_texture_to_buffer copy{};
        copy.type = WMTBlitCommandCopyFromTextureToBuffer;
        copy.src = color.buffer_attachment;
        copy.size = {color.buffer_width, 1, 1};
        copy.dst = color.buffer_backing;
        copy.offset = color.buffer_offset;
        copy.bytes_per_row = copy.bytes_per_image = color.buffer_bytes_per_row;
        copy_encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
      }
      copy_encoder.updateFence(fence_);
      copy_encoder.endEncoding();
    }
  }

  bool EncodeCaptureFrontierRender(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &inflight,
                                   const CaptureFrontierEncoderData &node, const CaptureFrontierExecution &execution,
                                   bool consumer) {
    const auto &p = execution.params;
    const bool vertex = p.Kind == kCaptureFrontierVertex;
    const bool child = p.Kind == kCaptureFrontierTessellationGeometry && consumer;
    auto &render = consumer ? *node.consumer : *node.producer;
    const bool raster = !(p.Flags & kCaptureFrontierMSAA) &&
        (vertex || p.Kind != kCaptureFrontierTessellationGeometry || child);
    auto info = CaptureFrontierRenderPass(render, raster);
    std::array<WMT::Texture, 8> markers{};
    if (raster && render.rov_marker_sample_count && !PrepareROVMarkers(cmdbuf, inflight,
        render.rov_marker_attachmentless ? render.rov_marker_width : render.render_target_width,
        render.rov_marker_attachmentless ? render.rov_marker_height : render.render_target_height,
        std::max(1u, uint32_t(render.render_target_array_length)), render.rov_marker_sample_count, markers)) return false;
    auto encoder = cmdbuf.renderCommandEncoder(info);
    encoder.waitForFence(fence_, WMTRenderStagePreRaster | WMTRenderStageFragment);
    encoder.encodeCommands(&render.cmd_head);
    for (uint32_t i = 0; i < render.rov_marker_sample_count; ++i) {
      if (!markers[i]) continue;
      encoder.setFragmentTexture(markers[i], 31 + i);
      encoder.useResource(markers[i], WMTResourceUsageRead | WMTResourceUsageWrite, WMTRenderStageFragment);
    }
    const auto stages = WMTRenderStages(WMTRenderStagePreRaster | WMTRenderStageFragment);
    for (const auto &buffer : execution.buffers)
      encoder.useResource(buffer, WMTResourceUsageRead | WMTResourceUsageWrite, stages);
    auto bind_all = [&](WMT::Buffer buffer, uint64_t offset, uint8_t index) {
      encoder.setVertexBuffer(buffer, offset, index);
      encoder.setObjectBuffer(buffer, offset, index);
      encoder.setMeshBuffer(buffer, offset, index);
      encoder.setFragmentBuffer(buffer, offset, index);
    };
    if (execution.roots) bind_all(execution.roots, 0, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
    if (execution.vertices) {
      encoder.setVertexBuffer(execution.vertices, 0, SM50_BINDING_INDEX_VERTEX_BUFFER);
      encoder.setObjectBuffer(execution.vertices, 0, SM50_BINDING_INDEX_VERTEX_BUFFER);
    }
    bind_all(execution.state, offsetof(CaptureFrontierState, Shader), SM50_BINDING_INDEX_CAPTURE_FRONTIER);
    if (execution.claim)
      encoder.setFragmentBuffer(execution.claim, 0, SM50_BINDING_INDEX_STATISTICS_CLAIM);
    if (vertex) {
      const auto &data = *render.so_indirect;
      wmtcmd_render_setpso pipeline{};
      pipeline.type = WMTRenderCommandSetPSO;
      pipeline.pso = data.frontier_replay_pipeline;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&pipeline));
      encoder.setVertexBuffer(execution.records, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
      if (data.frontier_replay_vertex_constants)
        encoder.setVertexBuffer(data.frontier_replay_vertex_constants, data.frontier_replay_vertex_constants_offset,
                                SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
      if (data.frontier_replay_fragment_constants)
        encoder.setFragmentBuffer(data.frontier_replay_fragment_constants, data.frontier_replay_fragment_constants_offset,
                                  SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
      wmtcmd_render_draw_indexed_indirect draw{};
      draw.type = WMTRenderCommandDrawIndexedIndirect;
      draw.primitive_type = p.Topology == 0 ? WMTPrimitiveTypePoint :
          (p.Topology <= 2 || p.Topology == 6 || p.Topology == 7) ? WMTPrimitiveTypeLine : WMTPrimitiveTypeTriangle;
      draw.index_type = WMTIndexTypeUInt32;
      draw.index_buffer = execution.replay_indices;
      draw.indirect_args_buffer = execution.state;
      draw.indirect_args_offset = offsetof(CaptureFrontierState, Indirect) + offsetof(CaptureFrontierIndirect, Replay);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
    } else {
      encoder.setObjectBuffer(execution.state, offsetof(CaptureFrontierState, DrawArguments), SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      encoder.setObjectBuffer(execution.state, offsetof(CaptureFrontierState, ActiveIndex), SM50_BINDING_INDEX_INDEX_BUFFER);
      encoder.setMeshBuffer(execution.state, offsetof(CaptureFrontierState, SO), SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL);
      encoder.setMeshBuffer(execution.scratch, 0, SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH);
      encoder.setMeshBuffer(execution.records, 0, SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS);
      encoder.setMeshBuffer(execution.state, offsetof(CaptureFrontierState, Mesh), SM50_BINDING_INDEX_MSAA_MESH_CONTROL);
      if (p.Kind == kCaptureFrontierTessellationGeometry) {
        encoder.setMeshBuffer(execution.tess_scratch, 0, SM50_BINDING_INDEX_TESS_CAPTURE);
        encoder.setMeshBuffer(execution.state, offsetof(CaptureFrontierState, Tess), SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL);
        encoder.setObjectBuffer(execution.tess_scratch, 0, SM50_BINDING_INDEX_TESS_CAPTURE);
        encoder.setObjectBuffer(execution.tess_list, 0, SM50_BINDING_INDEX_TESS_CAPTURE_LIST);
        encoder.setObjectBuffer(execution.state, offsetof(CaptureFrontierState, Tess), SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL);
      }
      wmtcmd_render_draw_meshthreadgroups_indirect draw{};
      draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
      draw.indirect_args_buffer = execution.state;
      draw.indirect_args_offset = offsetof(CaptureFrontierState, Indirect) +
          (child ? offsetof(CaptureFrontierIndirect, Consumer) : offsetof(CaptureFrontierIndirect, Producer));
      draw.object_threadgroup_size = child ? WMTSize{32, 1, 1} : p.Kind == kCaptureFrontierGeometry
          ? WMTSize{p.VertexPerWarp, 1, 1} : WMTSize{32 / p.PatchesPerGroup, p.PatchesPerGroup, 1};
      draw.mesh_threadgroup_size = child || p.Kind == kCaptureFrontierGeometry ? WMTSize{1, 1, 1} : WMTSize{32, 1, 1};
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
    }
    encoder.updateFence(fence_, WMTRenderStagePreRaster | WMTRenderStageFragment);
    encoder.endEncoding();
    if (raster) EncodeCaptureFrontierVisibility(cmdbuf, render, false);
    return true;
  }

  void EncodeCaptureFrontierBounds(WMT::MTL4CommandBuffer cmdbuf, const CaptureFrontierExecution &execution) {
    const auto &draw = *execution.msaa_draw;
    const auto &runtime = execution.msaa;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    const std::array<std::pair<WMT::Buffer, uint64_t>, 5> bindings = {{
        {runtime.parameters, sizeof(MSAAPrimitiveAssemblyParams)}, {runtime.vertices, 0}, {runtime.indices, 0},
        {runtime.draws, 0}, {runtime.bounds, 0}}};
    for (uint8_t slot = 0; slot < bindings.size(); ++slot) {
      wmtcmd_compute_setbuffer binding{};
      binding.type = WMTComputeCommandSetBuffer;
      binding.buffer = bindings[slot].first;
      binding.offset = bindings[slot].second;
      binding.index = slot;
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&binding));
    }
    wmtcmd_compute_setpso pipeline{};
    pipeline.type = WMTComputeCommandSetPSO;
    pipeline.pso = draw.bounds_pipeline;
    pipeline.threadgroup_size = {64, 1, 1};
    wmtcmd_compute_dispatch_indirect dispatch{};
    dispatch.type = WMTComputeCommandDispatchIndirect;
    dispatch.indirect_args_buffer = execution.state;
    // Clear covers at least the draw-packet dwords (six per primitive), so
    // this guarded grid covers all bounded slots and is zero for inactive work.
    dispatch.indirect_args_offset = offsetof(CaptureFrontierState, Indirect) + offsetof(CaptureFrontierIndirect, Clear);
    pipeline.next.set(&dispatch);
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pipeline));
    encoder.updateFence(fence_);
    encoder.endEncoding();
  }

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
    uint64_t capture_bytes = record_count * vertex_stride; // bounded by max_buffer above
    if (draw.mesh_capture &&
        (!resource_contract::CheckedMul(draw.mesh_control.total_slots, draw.mesh_layout.capture_dwords_per_group,
                                        capture_bytes) ||
         !resource_contract::CheckedMul(capture_bytes, 4, capture_bytes)))
      return false;

    execution.vertices = allocate(capture_bytes, false);
    execution.indices = allocate(max_primitives *
        MSAAIndexStride(draw.line_mode, draw.conservative) * sizeof(uint32_t), false);
    execution.draws = allocate(max_primitives * sizeof(MSAAPrimitiveIndirectArguments), false);
    execution.capture_dispatch = allocate(16, false);
    const void *mapped = nullptr;
    execution.bounds = allocate(max_primitives * sizeof(MSAAPrimitiveTileBounds), !draw.gpu_tile_tasks,
                                draw.gpu_tile_tasks ? nullptr : &mapped);
    execution.mapped_bounds = static_cast<const MSAAPrimitiveTileBounds *>(mapped);
    execution.max_primitives = uint32_t(max_primitives);
    const void *parameters_memory = nullptr;
    auto parameters = allocate(sizeof(MSAAPrimitiveAssemblyParams) + sizeof(MSAAPrimitiveBoundsParams) +
                               sizeof(SM50_MSAA_VERTEX_CAPTURE_CONTROL) + sizeof(SM50_MSAA_MESH_CONTROL),
                               true, &parameters_memory);
    execution.parameters = parameters;
    if (!execution.vertices || !execution.indices || !execution.draws ||
        !execution.capture_dispatch || !execution.bounds ||
        (!draw.gpu_tile_tasks && !execution.mapped_bounds) || !parameters || !parameters_memory)
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
    control->index_first = draw.indexed
        ? draw.assembly.index_size == 8 ? draw.start_vertex : draw.assembly.index_skip / 2 : 0;
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

  bool EncodeMSAARender(WMT::MTL4CommandBuffer cmdbuf, InflightCommandBuffer &initial,
                        RenderEncoderData &render, const MSAADrawData &draw,
                        const MSAAExecution &execution,
                        const std::function<bool(WMT::MTL4CommandBuffer &, InflightCommandBuffer *&, uint64_t &)> &retire,
                        uint64_t list_id, uint64_t recording, uint64_t submission) {
    auto *inflight = &initial;
    const bool gpu_tiles = draw.gpu_tile_tasks;
    if (gpu_tiles) {
      // Producer submissions and earlier tile draws may already be retired.
      // MSAAExecution owns the inputs independently; enroll this consumer's
      // inputs again so every resumed segment retains and resides them.
      try {
        for (const auto &buffer : {execution.vertices, execution.indices, execution.draws,
                                   execution.capture_dispatch, execution.bounds, execution.parameters}) {
          if (!buffer) continue;
          if (std::none_of(inflight->msaa_buffers.begin(), inflight->msaa_buffers.end(),
              [&](const auto &retained) { return retained.handle == buffer.handle; }))
            inflight->msaa_buffers.push_back(buffer);
          cmdbuf.useAllocation(WMT::Allocation{buffer.handle});
        }
      } catch (const std::bad_alloc &) {
        return false;
      }
    }
    static_assert(sizeof(SM50_MSAA_TILE_CONTROL) == offsetof(MSAATileTaskControl, padding));
    static_assert(offsetof(SM50_MSAA_TILE_CONTROL, task_active) == offsetof(MSAATileTaskControl, task_active));
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
    WMT::Reference<WMT::Buffer> task_state, task_parameters, task_completion;
    MSAATileTaskParams *task_params = nullptr;
    MSAATileTaskCompletion *mailbox = nullptr;
    uint32_t task_slot = 0;
    auto task_slot_offset = [&] {
      return offsetof(MSAATileTaskState, Slots) + uint64_t(task_slot) * sizeof(MSAATileTaskSlot);
    };
    auto task_kernel = [&](WMT::ComputePipelineState pipeline, WMTSize group, WMTSize threads,
                           std::initializer_list<std::pair<WMT::Buffer, uint64_t>> bindings,
                           uint64_t indirect_offset = UINT64_MAX, uint32_t mode = UINT32_MAX) {
      auto encoder = cmdbuf.computeCommandEncoder(false);
      encoder.waitForFence(fence_);
      uint8_t slot = 0;
      for (const auto &binding : bindings) {
        wmtcmd_compute_setbuffer command{};
        command.type = WMTComputeCommandSetBuffer;
        command.buffer = binding.first; command.offset = binding.second; command.index = slot++;
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&command));
      }
      if (mode != UINT32_MAX) {
        wmtcmd_compute_setbytes command{};
        command.type = WMTComputeCommandSetBytes;
        command.index = 5; command.length = sizeof(mode); command.bytes.set(&mode);
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&command));
      }
      wmtcmd_compute_setpso pso{};
      pso.type = WMTComputeCommandSetPSO; pso.pso = pipeline; pso.threadgroup_size = group;
      wmtcmd_compute_dispatch direct{};
      direct.type = WMTComputeCommandDispatchThreads; direct.size = threads;
      wmtcmd_compute_dispatch_indirect indirect{};
      indirect.type = WMTComputeCommandDispatchIndirect;
      indirect.indirect_args_buffer = task_state; indirect.indirect_args_offset = indirect_offset;
      pso.next.set(indirect_offset == UINT64_MAX ? static_cast<void *>(&direct) : static_cast<void *>(&indirect));
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pso));
      encoder.updateFence(fence_); encoder.endEncoding();
    };
    if (gpu_tiles) {
      if (!draw.task_prepare_pipeline || !draw.task_clear_pipeline || !draw.task_extent_pipeline) return false;
      auto shared = [&](uint64_t size, void **memory) {
        WMTBufferInfo info{};
        info.length = size;
        info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
        auto buffer = metal.newBuffer(info);
        *memory = info.memory.get_accessible_or_null();
        if (buffer) inflight->msaa_buffers.push_back(buffer);
        return buffer;
      };
      void *parameters_memory = nullptr, *completion_memory = nullptr;
      task_parameters = shared(sizeof(MSAATileTaskParams), &parameters_memory);
      task_completion = shared(sizeof(MSAATileTaskCompletion), &completion_memory);
      WMTBufferInfo state_info{};
      state_info.length = sizeof(MSAATileTaskState);
      state_info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
      task_state = metal.newBuffer(state_info);
      if (!task_parameters || !task_completion || !task_state || !parameters_memory || !completion_memory)
        return false;
      inflight->msaa_buffers.push_back(task_state);
      task_params = static_cast<MSAATileTaskParams *>(parameters_memory);
      mailbox = static_cast<MSAATileTaskCompletion *>(completion_memory);
      *task_params = {}; *mailbox = {};
      task_params->MaxPrimitives = execution.max_primitives;
      task_params->Width = render_width; task_params->Height = render_height;
      task_params->Layers = layer_count;
      task_params->ViewportCount = draw.bounds.viewport_count;
      mailbox->Width = mailbox->Height = 1;
      auto clear = cmdbuf.blitCommandEncoder();
      clear.waitForFence(fence_);
      wmtcmd_blit_fillbuffer fill{};
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = task_state; fill.length = sizeof(MSAATileTaskState);
      clear.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
      clear.updateFence(fence_); clear.endEncoding();
      owned_graphics::Trace("msaa_tile_begin", ",\"queue\":", owned_id_, ",\"encoder\":", render.id,
          ",\"list\":", list_id, ",\"recording\":", recording, ",\"submission\":", submission,
          ",\"source\":\"gpu\",\"capacity\":", execution.max_primitives,
          ",\"first_budget\":", kMSAATileFirstQuantum, ",\"quantum_budget\":", kMSAATileQuantum,
          ",\"host_interop\":\"native_scissor\",\"descriptor_capacity\":", kMSAATileQuantum,
          ",\"descriptor_bytes\":", sizeof(MSAATileTaskCompletion));
    }
    const bool attachmentless = !has_color_attachment && !render.depth.attachment && !render.stencil.attachment;
    if (attachmentless) {
      // A native attachmentless pass still needs a host-known allocation extent.
      // Reduce to two GPU extent words; never map or enumerate primitive bounds.
      if (gpu_tiles) {
        task_kernel(draw.task_extent_pipeline, {64, 1, 1}, {execution.max_primitives, 1, 1},
                    {{task_parameters, 0}, {execution.bounds, 0}, {task_completion, 0}});
        uint64_t producer = 0;
        if (!retire(cmdbuf, inflight, producer)) return false;
        render_width = mailbox->Width; render_height = mailbox->Height;
        if (!render_width || !render_height || render_width > task_params->Width ||
            render_height > task_params->Height) return false;
        task_params->Width = render_width; task_params->Height = render_height;
        owned_graphics::Trace("msaa_tile_extent", ",\"queue\":", owned_id_, ",\"encoder\":", render.id,
            ",\"producer\":", producer, ",\"width\":", render_width, ",\"height\":", render_height);
      } else {
        render_width = render_height = 1;
        for (uint32_t primitive = 0; primitive < execution.max_primitives; ++primitive) {
          render_width = std::max(render_width, execution.mapped_bounds[primitive].right);
          render_height = std::max(render_height, execution.mapped_bounds[primitive].bottom);
        }
      }
    }
    std::array<WMT::Texture, 8> rov_markers{};
    if (draw.rov_marker_sample_count && !PrepareROVMarkers(
            cmdbuf, *inflight, render_width, render_height,
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
    coverage_info.type = gpu_tiles || layer_count > 1
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
    inflight->msaa_textures.push_back(coverage);
    std::vector<WMT::Reference<WMT::Texture>> layer_views(layer_count);
    WMT::Texture sampled_coverage = coverage;
    WMT::Reference<WMT::Texture> post_coverage;
    std::vector<WMT::Reference<WMT::Texture>> post_views;
    if (draw.early_depth_pipeline && (draw.pixel_layout.flags & SM50_MSAA_PIXEL_FORCE_EARLY)) {
      post_coverage = metal.newTexture(coverage_info);
      if (!post_coverage) return false;
      inflight->msaa_textures.push_back(post_coverage);
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
      if (buffer) inflight->msaa_buffers.push_back(buffer);
      return buffer;
    };
    auto inputs = make_scratch(input_bytes);
    auto outputs = saved_outputs ? make_scratch(output_bytes) : WMT::Reference<WMT::Buffer>{};
    if (!inputs || (saved_outputs && !outputs)) return false;
    const bool emulated_bounds = draw.depth_bounds_probe_pipeline && draw.depth_bounds_enabled;
    const uint32_t output_mask = draw.output_sample_count >= 32
        ? draw.sample_mask : draw.sample_mask & ((1u << draw.output_sample_count) - 1u);
    const uint32_t raster_mask = draw.single_sample_replay
        ? (output_mask ? UINT32_MAX : 0u) : draw.sample_mask;
    if (gpu_tiles) {
      if (input_bytes / 4 > UINT32_MAX || output_bytes / 4 > UINT32_MAX) return false;
      task_params->InputDwords = uint32_t(input_bytes / 4);
      task_params->OutputDwords = outputs ? uint32_t(output_bytes / 4) : 0;
      auto &base = task_params->Base;
      base.sample_count = draw.sample_count; base.sample_mask = raster_mask;
      base.output_mask = output_mask; base.shading_rate = draw.shading_rate;
      base.shading_rate_image_width = draw.shading_rate_image_width;
      base.shading_rate_image_height = draw.shading_rate_image_height;
      base.depth_bounds_enabled = emulated_bounds;
      base.programmable_sample_count = draw.sample_positions.count;
      for (uint32_t sample = 0; sample < draw.sample_positions.count; ++sample)
        for (uint32_t axis = 0; axis < 2; ++axis)
          base.programmable_sample_positions[sample][axis] = draw.sample_positions.positions[sample][axis] / 16.f;
      memcpy(task_params->ViewportDepths, draw.viewport_depths, sizeof(task_params->ViewportDepths));
      memcpy(task_params->Viewports, draw.bounds.viewports, sizeof(task_params->Viewports));
      memcpy(task_params->Scissors, draw.bounds.scissors, sizeof(task_params->Scissors));
    }
    std::vector<WMT::Reference<WMT::Texture>> depth_views(emulated_bounds ? layer_count : 0);
    WMT::Reference<WMT::Texture> task_depth_view;
    if (gpu_tiles && emulated_bounds) {
      uint64_t resource_id = 0;
      const auto &attachment = render.depth.attachment;
      task_depth_view = attachment.texture().newTextureView(
          attachment->allocation->descriptor->pixelFormat(attachment->key),
          multisampled ? WMTTextureType2DMultisampleArray : WMTTextureType2DArray,
          render.depth.level, 1, render.depth.slice, layer_count,
          {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, resource_id);
      if (!task_depth_view) return false;
      inflight->msaa_textures.push_back(task_depth_view);
    }
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
    using MSAAReplayCommand = std::variant<
        wmtcmd_render_useresource, wmtcmd_render_setbuffer, wmtcmd_render_setbufferoffset,
        wmtcmd_render_settexture, wmtcmd_render_setbytes, wmtcmd_render_setrasterizerstate,
        wmtcmd_render_setviewports, wmtcmd_render_setviewport, wmtcmd_render_setdsso,
        wmtcmd_render_setdepthstencilstate, wmtcmd_render_setblendcolor, wmtcmd_render_setstencilref,
        wmtcmd_render_setstencilref_front_back, wmtcmd_render_setvisibilitymode, wmtcmd_render_memory_barrier>;
    struct MSAAStateReplay {
      std::vector<MSAAReplayCommand> commands;
      const wmtcmd_render_nop *head = nullptr;
      bool prepared = false;
    };
    // Only gather depth bias and query participation vary between passes. Build
    // each used variant once per draw, then replay it for every primitive/tile.
    std::array<MSAAStateReplay, 4> state_replays;
    auto prepare_replay = [&](MSAAStateReplay &replay, bool count_visibility, bool gather) {
      // The recorded state chain contains the application's PSO. Binding it to
      // our integer coverage target fails Metal validation even if immediately
      // replaced. Copy the permitted commands without modifying the recording.
      for (auto *node = static_cast<wmtcmd_base *>(render.cmd_head.next.get()); node;
           node = static_cast<wmtcmd_base *>(node->next.get())) {
        auto append_record = [&]<typename T>() {
          replay.commands.emplace_back(*reinterpret_cast<const T *>(node));
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
        case WMTRenderCommandD3DalloyGeometryDraw:
        case WMTRenderCommandD3DalloyGeometryDrawIndexed:
        case WMTRenderCommandD3DalloyGeometryDrawIndirect:
        case WMTRenderCommandD3DalloyGeometryDrawIndexedIndirect:
        case WMTRenderCommandD3DalloyTessellationMeshDraw:
        case WMTRenderCommandD3DalloyTessellationMeshDrawIndexed:
        case WMTRenderCommandD3DalloyTessellationMeshDrawIndirect:
        case WMTRenderCommandD3DalloyTessellationMeshDrawIndexedIndirect:
          if (!draw.mesh_capture) return false;
          break;
        case WMTRenderCommandUseResource: append_record.operator()<wmtcmd_render_useresource>(); break;
        case WMTRenderCommandSetVertexBuffer:
        case WMTRenderCommandSetFragmentBuffer:
        // Object/mesh (GS/HS/DS) bindings are replayed so a mesh pipeline bound later in the
        // pass sees the same argument state; vertex/fragment replay functions ignore them.
        case WMTRenderCommandSetObjectBuffer:
        case WMTRenderCommandSetMeshBuffer: append_record.operator()<wmtcmd_render_setbuffer>(); break;
        case WMTRenderCommandSetVertexBufferOffset:
        case WMTRenderCommandSetFragmentBufferOffset:
        case WMTRenderCommandSetObjectBufferOffset:
        case WMTRenderCommandSetMeshBufferOffset: append_record.operator()<wmtcmd_render_setbufferoffset>(); break;
        case WMTRenderCommandSetFragmentTexture: append_record.operator()<wmtcmd_render_settexture>(); break;
        case WMTRenderCommandSetFragmentBytes: append_record.operator()<wmtcmd_render_setbytes>(); break;
        case WMTRenderCommandSetRasterizerState: {
          auto raster = *reinterpret_cast<const wmtcmd_render_setrasterizerstate *>(node);
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
          replay.commands.emplace_back(raster);
          break;
        }
        case WMTRenderCommandSetViewports: append_record.operator()<wmtcmd_render_setviewports>(); break;
        case WMTRenderCommandSetViewport: append_record.operator()<wmtcmd_render_setviewport>(); break;
        // Every MSAA pass sets its own tile scissor right after bind_tile. Replaying the application scissor
        // first can exceed an attachmentless pass sized to the primitive bounds (Metal validation).
        case WMTRenderCommandSetScissorRects:
        case WMTRenderCommandSetScissorRect: break;
        case WMTRenderCommandSetDSSO: append_record.operator()<wmtcmd_render_setdsso>(); break;
        case WMTRenderCommandSetDepthStencilState: append_record.operator()<wmtcmd_render_setdepthstencilstate>(); break;
        case WMTRenderCommandSetBlendFactorAndStencilRef:
        case WMTRenderCommandSetBlendFactor: append_record.operator()<wmtcmd_render_setblendcolor>(); break;
        case WMTRenderCommandSetStencilRef: append_record.operator()<wmtcmd_render_setstencilref>(); break;
        case WMTRenderCommandSetStencilRefFrontBack:
          append_record.operator()<wmtcmd_render_setstencilref_front_back>();
          break;
        case WMTRenderCommandSetVisibilityMode:
          // Enabling counting registers a Reset write even if disabled before
          // the draw. Only passes that contribute to the query may replay it.
          if (count_visibility)
            append_record.operator()<wmtcmd_render_setvisibilitymode>();
          break;
        case WMTRenderCommandMemoryBarrier: append_record.operator()<wmtcmd_render_memory_barrier>(); break;
        // A view location recorded for an earlier native draw; this draw's own location is set below.
        case WMTRenderCommandSetVertexAmplification: break;
        default:
          ERR("Unsupported recorded MSAA render state command ", node->type);
          return false;
        }
      }
      // Vector growth has finished. Link the active records, not the variant
      // objects; these addresses remain stable until EncodeMSAARender returns.
      WMTMemoryPointer *tail = nullptr;
      for (auto &command : replay.commands) {
        std::visit([&](auto &state) {
          if (tail) tail->set(&state);
          else replay.head = reinterpret_cast<const wmtcmd_render_nop *>(&state);
          state.next.set(nullptr);
          tail = &state.next;
        }, command);
      }
      replay.prepared = true;
      return true;
    };
    auto bind_tile = [&](WMT::MTL4RenderCommandEncoder &encoder, const SM50_MSAA_TILE_CONTROL &tile,
                         uint32_t vertex, uint32_t fragment, bool count_visibility, bool gather = false) {
      auto &replay = state_replays[unsigned(count_visibility) | (unsigned(gather) << 1)];
      try {
        if (!replay.prepared && !prepare_replay(replay, count_visibility, gather)) return false;
      } catch (const std::bad_alloc &) {
        return false; // The caller must still end its active native encoder.
      } catch (const std::length_error &) {
        return false;
      }
      if (replay.head) encoder.encodeCommands(replay.head);
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
      if (gpu_tiles) {
        const uint64_t offset = task_slot_offset() + offsetof(MSAATileTaskSlot, Controls) +
            uint64_t(tile.sample_base) * sizeof(MSAATileTaskControl);
        encoder.setFragmentBuffer(task_state, offset, SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
      } else {
        encoder.setFragmentBytes(&tile, sizeof(tile), SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
      }
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
      const uint32_t tile_right = x + std::min(8u, render_width - std::min(x, render_width));
      const uint32_t tile_bottom = y + std::min(8u, render_height - std::min(y, render_height));
      for (uint32_t i = 0; i < count && i < rects.size(); i++) {
        const auto &original = draw.bounds.scissors[fullscreen ? bounds.viewport_index : i];
        // GPU-generated descriptors supply only the native scissor interop.
        // Keep the original geometry: clipping it to shader-generated planes
        // would re-snap new edge vertices and change sample coverage.
        const uint32_t left = std::min(std::max(x, original[0]), render_width);
        const uint32_t top = std::min(std::max(y, original[1]), render_height);
        const uint32_t right = std::min(tile_right, original[2]);
        const uint32_t bottom = std::min(tile_bottom, original[3]);
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
      command.indirect_args_buffer = gpu_tiles ? WMT::Buffer(task_state) : WMT::Buffer(execution.draws);
      command.indirect_args_offset = gpu_tiles ? task_slot_offset() + offsetof(MSAATileTaskSlot, Indexed)
          : uint64_t(primitive) * sizeof(MSAAPrimitiveIndirectArguments);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&command));
    };
    auto draw_fullscreen = [&](WMT::MTL4RenderCommandEncoder &encoder, uint32_t sample_base) {
      if (!gpu_tiles) { encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3); return; }
      // Fullscreen transport and captured geometry use distinct vertex slots:
      // expanded line/conservative replay still consumes its original bounds at 23.
      encoder.setVertexBuffer(task_state, task_slot_offset() + offsetof(MSAATileTaskSlot, Controls) +
          uint64_t(sample_base) * sizeof(MSAATileTaskControl), SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
      wmtcmd_render_draw_indirect command{};
      command.type = WMTRenderCommandDrawIndirect; command.primitive_type = WMTPrimitiveTypeTriangle;
      command.indirect_args_buffer = task_state;
      command.indirect_args_offset = task_slot_offset() + offsetof(MSAATileTaskSlot, Fullscreen);
      encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&command));
    };
    auto fanout_visibility = [&]() {
      if (render.visibility_targets.empty()) return;
      auto encoder = cmdbuf.computeCommandEncoder(false);
      encoder.waitForFence(fence_);
      for (const auto &target : render.visibility_targets) {
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
      }
      encoder.updateFence(fence_);
      encoder.endEncoding();
    };
    auto encode_tile = [&](uint32_t primitive, const MSAAPrimitiveTileBounds &bounds,
                           uint32_t x, uint32_t y) -> bool {
      sampled_coverage = !gpu_tiles && layer_count > 1 ? WMT::Texture(layer_views[bounds.target_layer])
                                                     : WMT::Texture(coverage);
      if (gpu_tiles) {
        task_kernel(draw.task_clear_pipeline, {64, 1, 1}, {},
            {{task_state, task_slot_offset()}, {task_parameters, 0}, {inputs, 0},
             {outputs ? WMT::Buffer(outputs) : WMT::Buffer(inputs), 0}},
             task_slot_offset() + offsetof(MSAATileTaskSlot, Clear));
      } else {
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
      }
      // With a forced raster pattern, SampleMask selects OUTPUT (RTV) samples, not virtual raster sample bits:
      // the raster mask is full when any output sample is enabled, else empty.
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
        source.type = WMTComputeCommandSetTexture; source.texture = gpu_tiles ? WMT::Texture(task_depth_view)
            : WMT::Texture(depth_views[bounds.target_layer]);
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
        wmtcmd_compute_setbuffer task_binding{};
        wmtcmd_compute_dispatch_indirect task_dispatch{};
        if (gpu_tiles) {
          task_binding.type = WMTComputeCommandSetBuffer;
          task_binding.buffer = task_state;
          task_binding.offset = task_slot_offset() + offsetof(MSAATileTaskSlot, Controls);
          task_binding.index = SM50_BINDING_INDEX_MSAA_TILE_CONTROL;
          task_dispatch.type = WMTComputeCommandDispatchIndirect;
          task_dispatch.indirect_args_buffer = task_state;
          task_dispatch.indirect_args_offset = task_slot_offset() + offsetof(MSAATileTaskSlot, Probe);
          constants.next.set(&task_binding); task_binding.next.set(&task_dispatch);
        }
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
      if (gpu_tiles) WMT::SetRenderTargetArrayLength(clear_info, layer_count);
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
        encoder.waitForFence(fence_, gpu_tiles ? WMTRenderStageVertex : WMTRenderStageFragment);
        if (!bind_tile(encoder, tile, 1, 2, false)) { encoder.endEncoding(); return false; }
        scissor(encoder, bounds, x, y, true);
        no_cull(encoder);
        encoder.setDepthStencilState(draw.no_depth_stencil);
        encoder.setRenderPipelineState(draw.clear_coverage_pipeline);
        if (gpu_tiles) encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
        draw_fullscreen(encoder, tile.sample_base);
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
        clear_post_info.colors[0].texture = !gpu_tiles && layer_count > 1
            ? WMT::Texture(post_views[bounds.target_layer]) : WMT::Texture(post_coverage);
        {
          auto encoder = cmdbuf.renderCommandEncoder(clear_post_info);
          encoder.waitForFence(fence_, gpu_tiles ? WMTRenderStageVertex : WMTRenderStageFragment);
          if (!bind_tile(encoder, tile, 1, 2, false)) { encoder.endEncoding(); return false; }
          scissor(encoder, bounds, x, y, true);
          no_cull(encoder);
          encoder.setDepthStencilState(draw.no_depth_stencil);
          encoder.setRenderPipelineState(draw.clear_coverage_pipeline);
          if (gpu_tiles) encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
          draw_fullscreen(encoder, tile.sample_base);
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
        sampled_coverage = !gpu_tiles && layer_count > 1 ? WMT::Texture(post_views[bounds.target_layer])
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
          encoder.waitForFence(fence_, gpu_tiles ? WMTRenderStageVertex : WMTRenderStageFragment);
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
          draw_fullscreen(encoder, tile.sample_base);
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
          encoder.waitForFence(fence_, gpu_tiles ? WMTRenderStageVertex : WMTRenderStageFragment);
          if (!bind_tile(encoder, tile, 1, 4, true)) { encoder.endEncoding(); return false; }
          scissor(encoder, bounds, x, y, true);
          no_cull(encoder);
          encoder.setDepthStencilState(draw.no_depth_stencil);
          encoder.setRenderPipelineState(draw.query_coverage_pipeline);
          encoder.setViewport({0, 0, double(render_width), double(render_height), 0, 1});
          draw_fullscreen(encoder, tile.sample_base);
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
          if (!gpu_tiles && layer_count > 1)
            encoder.setVertexBuffer(execution.bounds,
                uint64_t(primitive) * sizeof(MSAAPrimitiveTileBounds) +
                    offsetof(MSAAPrimitiveTileBounds, target_layer),
                SM50_BINDING_INDEX_MSAA_TILE_CONTROL);
          draw_fullscreen(encoder, tile.sample_base);
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
      return true;
    };
    if (gpu_tiles) {
      // Generate only the first task before observing the bounded native-
      // scissor descriptors. Later prepares follow all consumers of the prior
      // immutable batch in the same submission; no primitive bounds are mapped.
      task_kernel(draw.task_prepare_pipeline, {1, 1, 1}, {1, 1, 1},
          {{task_state, 0}, {task_parameters, 0}, {execution.bounds, 0},
           {execution.draws, 0}, {task_completion, 0}}, UINT64_MAX, kMSAATilePrepare);
      uint64_t ready_producer = 0;
      if (!retire(cmdbuf, inflight, ready_producer)) return false;
      auto batch = *mailbox;
      if (batch.Error || batch.Status > kMSAATileDone || batch.CompletedTasks || batch.Quantum ||
          batch.ReadyTasks > kMSAATileFirstQuantum ||
          (batch.Status == kMSAATileDone) != (batch.ReadyTasks == 0)) return false;
      owned_graphics::Trace("msaa_tile_ready", ",\"queue\":", owned_id_, ",\"encoder\":", render.id,
          ",\"producer\":", ready_producer, ",\"ready\":", batch.ReadyTasks, ",\"status\":", batch.Status,
          ",\"width\":", render_width, ",\"height\":", render_height,
          ",\"viewports\":", task_params->ViewportCount, ",\"layers\":", layer_count);
      uint64_t completed_tasks = 0, quantum = 0;
      uint32_t budget = kMSAATileFirstQuantum;
      for (;;) {
        const uint32_t scheduled = batch.ReadyTasks;
        for (task_slot = 0; task_slot < scheduled; ++task_slot) {
          const auto &descriptor = batch.Descriptors[task_slot];
          if (descriptor.X >= render_width || descriptor.Y >= render_height ||
              (descriptor.X & 7u) || (descriptor.Y & 7u) ||
              descriptor.Viewport >= std::min(task_params->ViewportCount, 16u) ||
              descriptor.Layer >= layer_count) return false;
          MSAAPrimitiveTileBounds bounds{};
          bounds.viewport_index = descriptor.Viewport;
          bounds.target_layer = descriptor.Layer;
          if (!encode_tile(0, bounds, descriptor.X, descriptor.Y)) return false;
        }
        task_kernel(draw.task_prepare_pipeline, {1, 1, 1}, {1, 1, 1},
            {{task_state, 0}, {task_parameters, 0}, {execution.bounds, 0},
             {execution.draws, 0}, {task_completion, 0}}, UINT64_MAX, kMSAATilePublish);
        uint64_t producer = 0;
        if (!retire(cmdbuf, inflight, producer)) return false;
        const auto completed = *mailbox;
        owned_graphics::Trace("msaa_tile_quantum", ",\"queue\":", owned_id_, ",\"encoder\":", render.id,
            ",\"producer\":", producer, ",\"quantum\":", completed.Quantum, ",\"budget\":", budget,
            ",\"count\":", completed.CompletedTasks - completed_tasks,
            ",\"task_done\":", completed.CompletedTasks, ",\"status\":", completed.Status,
            ",\"ready\":", completed.ReadyTasks, ",\"scissors\":", scheduled);
        if (completed.Error || completed.Status > kMSAATileDone || completed.Quantum != quantum + 1 ||
            completed.CompletedTasks < completed_tasks ||
            completed.CompletedTasks - completed_tasks != scheduled || scheduled > budget ||
            completed.ReadyTasks > kMSAATileQuantum ||
            (completed.Status == kMSAATileDone && completed.ReadyTasks) ||
            (completed.Status == kMSAATileRunning &&
             (!completed.ReadyTasks || completed.CompletedTasks == completed_tasks))) return false;
        if (completed.Status == kMSAATileDone) {
          // Every command using these submission scratch lists has retired.
          // Future MSAA executions own their captures separately and enroll
          // them on entry; SO/frontier/atomic/swizzle owners are separate.
          // Do not carry completed full-size coverage and scratch through all
          // subsequent draws in this Execute. The newly opened native segment
          // can still hold its already-registered allocations until retirement.
          const auto buffers = inflight->msaa_buffers.size();
          const auto textures = inflight->msaa_textures.size();
          const auto markers = inflight->rov_markers.size();
          inflight->msaa_buffers.clear();
          inflight->msaa_textures.clear();
          inflight->rov_markers.clear();
          owned_graphics::Trace("msaa_tile_release", ",\"queue\":", owned_id_, ",\"encoder\":", render.id,
              ",\"producer\":", producer, ",\"released_buffers\":", buffers,
              ",\"released_textures\":", textures, ",\"released_markers\":", markers,
              ",\"retained_buffers\":", inflight->msaa_buffers.size(),
              ",\"retained_textures\":", inflight->msaa_textures.size(),
              ",\"retained_markers\":", inflight->rov_markers.size());
          break;
        }
        quantum = completed.Quantum; completed_tasks = completed.CompletedTasks;
        budget = completed.ReadyTasks;
        batch = completed;
      }
    } else {
      for (uint32_t primitive = 0; primitive < execution.max_primitives; ++primitive) {
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
          inflight->msaa_textures.push_back(depth_views[bounds.target_layer]);
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
            inflight->msaa_textures.push_back(view);
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
              inflight->msaa_textures.push_back(post_view);
            }
          }
        }

        for (uint32_t y = bounds.top; y < bounds.bottom; y += 8)
          for (uint32_t x = bounds.left; x < bounds.right; x += 8)
            if (!encode_tile(primitive, bounds, x, y)) return false;
      }
    }
    return true;
  }

  /* One execution of a capture-sized ExecuteIndirect (MeshCaptureIndirectData): per active command, its captures,
   * sized from its GPU-written arguments exactly as a direct draw of the same arguments. */
  struct MeshCaptureCommand {
    WMT::Reference<WMT::Buffer> scratch, list; // TS+GS DS capture and flat primitive list
    WMT::Reference<WMT::Buffer> primitive_summaries;
    PrimitiveScanPlan scan_plan;
    uint64_t scan_control_offset = 0;
    uint64_t slots = 0;
    uint64_t args_offset = 0, capture_offset = 0, compact_offset = 0, dispatch_offset = 0;
    uint32_t patches = 0, instances = 0, vertex_count = 0;
    uint32_t slot = 0; // the command's index among the signature's commands
    Rc<BufferAllocation> index_owner;
    WMT::Buffer index_buffer;
    uint64_t index_offset = 0;
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
          const uint64_t arguments = command.strip_control_offset + sizeof(MeshStripCutControl);
          encoder.useResource(command.strip_list, WMTResourceUsageRead, WMTRenderStageObject);
          wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect draw{};
          draw.type = WMTRenderCommandD3DalloyGeometryDrawIndexedIndirect;
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
          wmtcmd_render_d3dalloy_geometry_draw_indexed draw{};
          draw.type = WMTRenderCommandD3DalloyGeometryDrawIndexed;
          draw.next.set(nullptr);
          draw.draw_arguments_offset = command.args_offset;
          draw.index_buffer = command.index_buffer;
          draw.index_buffer_offset = command.index_offset;
          draw.warp_count = warps;
          draw.instance_count = command.instances;
          draw.vertex_per_warp = data.vertex_per_warp;
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw));
        } else {
          wmtcmd_render_d3dalloy_geometry_draw draw{};
          draw.type = WMTRenderCommandD3DalloyGeometryDraw;
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
          wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed draw{};
          draw.type = WMTRenderCommandD3DalloyTessellationMeshDrawIndexed;
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
          wmtcmd_render_d3dalloy_tessellation_mesh_draw draw{};
          draw.type = WMTRenderCommandD3DalloyTessellationMeshDraw;
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
        auto vertex = library.newFunction("d3dalloy_resolve_vertex");
        auto fragment = library.newFunction((std::string("d3dalloy_msaa_export_") + suffix).c_str());
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
        std::string name = array ? "d3dalloy_msaa_import_array_" : "d3dalloy_msaa_import_";
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

  struct SwizzleSubresourceRange {
    uint32_t mip_begin, mip_end, array_begin, array_end;
  };

  struct SwizzleSubresourceSelection {
    // Keep an exact union without storage proportional to an array texture's
    // subresource count. Overflow and every unproven consumer select everything.
    std::array<SwizzleSubresourceRange, 16> ranges{};
    uint32_t count = 0;
    bool whole = false;

    void Include(const SwizzleSubresourceRange *range) {
      if (whole) return;
      if (!range) {
        whole = true;
        count = 0;
        return;
      }
      for (uint32_t i = 0; i < count; ++i) {
        const auto &held = ranges[i];
        if (held.mip_begin <= range->mip_begin && held.mip_end >= range->mip_end &&
            held.array_begin <= range->array_begin && held.array_end >= range->array_end)
          return;
      }
      if (count == ranges.size()) {
        whole = true;
        count = 0;
        return;
      }
      ranges[count++] = *range;
    }

    bool Contains(uint32_t mip, uint32_t array) const {
      if (whole) return true;
      for (uint32_t i = 0; i < count; ++i) {
        const auto &range = ranges[i];
        if (mip >= range.mip_begin && mip < range.mip_end &&
            array >= range.array_begin && array < range.array_end)
          return true;
      }
      return false;
    }

    bool HasMip(uint32_t mip) const {
      if (whole) return true;
      for (uint32_t i = 0; i < count; ++i)
        if (mip >= ranges[i].mip_begin && mip < ranges[i].mip_end) return true;
      return false;
    }
  };

  struct SwizzlePassResource {
    Com<MTLD3D12Resource, false> resource;
    bool writes;
    SwizzleSubresourceSelection imports, exports;
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

  void PrepareSwizzleValidity(InflightCommandBuffer &inflight, const EncoderData *record, uint64_t submission) {
    ++inflight.swizzle_encoding_serial;
    inflight.swizzle_encoder_id = record->id;
    inflight.swizzle_submission = submission;
    bool complete = false;
    switch (record->type) {
    case EncoderType::Null:
    case EncoderType::Diagnostic:
    case EncoderType::Clear:
    case EncoderType::StandardSwizzle:
      complete = true;
      break;
    case EncoderType::Render: {
      const auto &render = *static_cast<const RenderEncoderData *>(record);
      complete = render.submission_group_safe && !render.submission_snapshot_budget &&
                 !render.submission_root_srv_opaque && !render.atomic64_phase && !render.msaa_draw;
      break;
    }
    case EncoderType::Compute: {
      const auto &compute = *static_cast<const ComputeEncoderData *>(record);
      complete = compute.submission_access_complete && !compute.submission_snapshot_budget &&
                 !compute.submission_root_srv_opaque && !compute.atomic64_phase && compute.cast_textures.empty();
      break;
    }
    default:
      break;
    }
    // Copies and every unproved writer invalidate first; their existing
    // explicit export can establish equality again after the native write.
    // Ordinary state/UAV barriers order work without changing either image.
    inflight.swizzle_validity_enabled = complete;
    if (!complete) inflight.InvalidateSwizzleRepresentations();
  }

  InflightCommandBuffer::SwizzleRepresentation *
  FindSwizzleRepresentation(InflightCommandBuffer &inflight, MTLD3D12Resource *resource) {
    if (!inflight.swizzle_validity_enabled || !resource || !resource->swizzle_layout ||
        !resource->texture || !resource->buffer || resource->reserved || resource->share ||
        resource->standard_swizzle_may_alias)
      return nullptr;
    const auto desc = resource->GetDesc();
    const auto &layout = *resource->swizzle_layout;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        uint64_t(layout.mip_count) * layout.array_count > 256)
      return nullptr;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    // The internal canonical buffer may use Metal Shared storage even when
    // the public heap cannot be mapped. Persistent CPU mappings have no write
    // notification, so public CPU visibility is the eligibility boundary.
    if (FAILED(resource->GetHeapProperties(&heap, &flags)) || IsCpuVisibleHeap(&heap) ||
        (flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)))
      return nullptr;
    auto canonical = resource->buffer->current();
    auto shadow = resource->texture->current();
    if (!canonical || !shadow || canonical->canonicalHeap || canonical->reservedResource ||
        canonical->flags().test(BufferAllocationFlag::CpuPlaced) ||
        canonical->currentSuballocationOffset() ||
        canonical->flags().test(BufferAllocationFlag::PlacementSparse) || shadow->canonicalHeap ||
        resource->texture->reservedResource || shadow->flags().test(TextureAllocationFlag::Shared) ||
        shadow->flags().test(TextureAllocationFlag::PlacementSparse))
      return nullptr;
    for (auto &entry : inflight.swizzle_representations)
      if (entry.resource_id == resource->owned_id && entry.canonical.ptr() == canonical &&
          entry.shadow.ptr() == shadow && entry.canonical_handle == canonical->buffer().handle &&
          entry.shadow_handle == shadow->texture().handle && entry.layout == &layout)
        return &entry;
    // Never evict an owner before GPU retirement or broaden an overflowing
    // proof to the whole resource. Excess resources take the original path.
    if (inflight.swizzle_representations.size() == 64) return nullptr;
    try {
      inflight.swizzle_representations.push_back(
          {resource->owned_id, canonical, shadow, canonical->buffer().handle, shadow->texture().handle, &layout});
      auto &entry = inflight.swizzle_representations.back();
      // Simultaneous-access resources allow concurrent foreign queue writes
      // to other texels of a subresource. Never carry their equality.
      entry.allow_carry = !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
      if (entry.allow_carry && inflight.swizzle_predecessor) {
        for (size_t i = 0; i < swizzle_carry_count_; ++i) {
          const auto &previous = swizzle_carry_[i];
          if (previous.resource_id != entry.resource_id) continue;
          if (previous.canonical == entry.canonical_handle && previous.shadow == entry.shadow_handle &&
              previous.layout == uintptr_t(&layout)) {
            entry.valid = entry.inherited = previous.valid;
            entry.source_queue = previous.queue;
            entry.source_submission = previous.submission;
            entry.synchronized_version = entry.content_version;
          }
          swizzle_carry_[i] = swizzle_carry_[--swizzle_carry_count_];
          break;
        }
      }
      return &entry;
    } catch (const std::bad_alloc &) {
      return nullptr;
    }
  }

  void AppendStandardResource(std::vector<SwizzlePassResource> &uses,
                              Com<MTLD3D12Resource, false> resource, bool writes,
                              const SwizzleSubresourceRange *range = nullptr) {
    if (!resource)
      return;
    for (auto &use : uses) {
      if (use.resource.ptr() == resource.ptr()) {
        use.writes |= writes;
        use.imports.Include(range);
        if (writes) use.exports.Include(range);
        return;
      }
    }
    uses.push_back({std::move(resource), writes});
    uses.back().imports.Include(range);
    if (writes) uses.back().exports.Include(range);
  }

  void AppendStandardTexture(std::vector<SwizzlePassResource> &uses, Texture *texture, bool writes,
                             TextureViewKey key = {}, TextureAllocation *allocation = nullptr,
                             Com<MTLD3D12Resource, false> descriptor_owner = nullptr,
                             bool select_view = false) {
    if (!texture) return;
    auto resource = device_->LookupStandardSwizzleTexture(texture);
    SwizzleSubresourceRange range{};
    const SwizzleSubresourceRange *selected = nullptr;
    if (select_view && resource && resource->swizzle_layout && !resource->reserved &&
        resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        resource->GetDesc().SampleDesc.Count == 1) {
      const auto descriptor = texture->viewDescriptor(key);
      const auto &layout = *resource->swizzle_layout;
      // Cubes, sparse/volume atlases and cast/MSAA shadows keep their original
      // whole-resource synchronization. Only native 2D view coordinates qualify.
      if ((descriptor.type == WMTTextureType2D || descriptor.type == WMTTextureType2DArray) &&
          !descriptor.castBlockSize && !descriptor.castCompressed && !descriptor.msaaShadow &&
          !descriptor.reservedVolumeRender && !texture->reservedVolumeLayoutAddress &&
          key.mip_start < key.mip_end && key.mip_end <= layout.mip_count &&
          key.array_start < key.array_end && key.array_end <= layout.array_count &&
          key.mip_start == descriptor.firstMiplevel &&
          key.mip_end == descriptor.firstMiplevel + descriptor.miplevelCount &&
          key.array_start == descriptor.firstArraySlice &&
          key.array_end == descriptor.firstArraySlice + descriptor.arraySize) {
        range = {uint32_t(key.mip_start), uint32_t(key.mip_end),
                 uint32_t(key.array_start), uint32_t(key.array_end)};
        selected = &range;
      }
    }
    AppendStandardResource(uses, std::move(resource), writes, selected);
    if (!allocation) allocation = texture->current();
    if (!allocation) return;
    if (texture->supportsDescriptorViewPool(key)) return;
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
      std::lock_guard<d3dalloy::mutex> lock(reserved.mutex);
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

  /* Ordinary table walks only find swizzled, cast, MSAA-shadow or reserved-volume textures. Read at Execute,
   * after every descriptor write the submission can observe. */
  bool StandardSwizzleTablesMayMatter() const {
    return device_->HasStandardSwizzleTextures() || Texture::AnySpecialViews() || ReservedResource::AnyLive();
  }

  std::vector<SwizzlePassResource>
  CollectStandardSwizzleTables(const std::unique_ptr<std::vector<StandardSwizzleTableUse>> &tables,
                              bool select_views = false) {
    std::vector<SwizzlePassResource> uses;
    if (tables) AppendStandardSwizzleTables(*tables, uses, select_views);
    return uses;
  }

  // These recording flags prove the union across every draw/dispatch in the
  // encoder, not just its last PSO. Descriptor values are still read at Execute.
  std::vector<SwizzlePassResource> CollectStandardSwizzleTables(const RenderEncoderData &render) {
    const bool complete = render.submission_group_safe && !render.submission_snapshot_budget &&
                          !render.submission_root_srv_opaque && !render.atomic64_phase;
    return CollectStandardSwizzleTables(complete ? render.submission_tables : render.swizzle_tables, complete);
  }

  std::vector<SwizzlePassResource> CollectStandardSwizzleTables(const ComputeEncoderData &compute) {
    const bool complete = compute.submission_access_complete && !compute.submission_snapshot_budget &&
                          !compute.submission_root_srv_opaque && !compute.atomic64_phase;
    return CollectStandardSwizzleTables(complete ? compute.submission_tables : compute.swizzle_tables, complete);
  }

  void AppendStandardSwizzleTables(std::span<const StandardSwizzleTableUse> tables,
                                  std::vector<SwizzlePassResource> &uses, bool select_views = false) {
    auto append_descriptor = [&](MTLD3D12DescriptorHeap *heap, uint64_t index, bool select_view = false) {
      const auto &descriptor = heap->GetDescriptor(index);
      const bool writable = descriptor.type == ShaderVisibleDescriptorType::UAVTexture;
      if (!writable && descriptor.type != ShaderVisibleDescriptorType::SRVTexture) return;
      auto *texture = writable ? descriptor.UAVTexture.texture : descriptor.SRVTexture.texture;
      if (!texture || !descriptor.owner_id) return;
      auto owner = device_->LookupDescriptorResource(texture, descriptor.owner_id);
      if (!owner) return;
      const auto view = writable ? descriptor.UAVTexture.view : descriptor.SRVTexture.view;
      AppendStandardTexture(uses, texture, writable, view, nullptr, std::move(owner), select_view);
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
          for (uint64_t i = 0; i < count; ++i) append_descriptor(table.heap, first + i, select_views);
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
                             bool volume_compressed = false,
                             const SwizzleSubresourceSelection *selection = nullptr) {
    if (!resource || !resource->swizzle_layout || !resource->texture || !resource->buffer ||
        !resource->texture->current() || !resource->buffer->current())
      return false;
    const auto &layout = *resource->swizzle_layout;
    const auto desc = resource->GetDesc();
    auto *representation = !compare && !dirty_flags && !baseline && !volume_source
        ? FindSwizzleRepresentation(inflight, resource) : nullptr;
    if (!representation && to_canonical && inflight.swizzle_predecessor)
      inflight.InvalidateSwizzleRepresentations();
    if (representation && to_canonical)
      representation->Invalidate(inflight.swizzle_encoding_serial, inflight.swizzle_encoder_id);
    std::array<uint64_t, 4> requested{};
    uint32_t requested_subresources = 0, reused_subresources = 0;
    uint64_t reused_tiles = 0;
    if (representation) {
      for (uint32_t array = 0; array < layout.array_count; ++array)
        for (uint32_t mip = 0; mip < layout.mip_count; ++mip) {
          if (selection && !selection->Contains(mip, array)) continue;
          const uint32_t subresource = array * layout.mip_count + mip;
          requested[subresource / 64] |= uint64_t(1) << (subresource % 64);
          ++requested_subresources;
          if (!to_canonical && representation->Contains(subresource)) {
            ++reused_subresources;
            const auto &m = layout.mips[mip];
            reused_tiles += uint64_t(m.tiles_x) * m.tiles_y * m.tiles_z;
          }
        }
    }
    auto trace_reuse = [&] {
      if (!reused_subresources) return;
      uint32_t cross_subresources = 0;
      for (size_t i = 0; i < requested.size(); ++i) {
        cross_subresources += std::popcount(representation->inherited[i] & requested[i]);
        representation->inherited[i] &= ~requested[i];
      }
      owned_graphics::Trace("standard_swizzle_reuse", ",\"queue\":", owned_id_,
          ",\"submission\":", inflight.swizzle_submission, ",\"encoder\":", inflight.swizzle_encoder_id,
          ",\"resource\":", resource->owned_id, ",\"encoding_serial\":", inflight.swizzle_encoding_serial,
          ",\"writer_serial\":", representation->writer_serial,
          ",\"writer_encoder\":", representation->writer_encoder,
          ",\"content_version\":", representation->content_version,
          ",\"requested_subresources\":", requested_subresources,
          ",\"cross_submission_subresources\":", cross_subresources,
          ",\"source_queue\":", cross_subresources ? representation->source_queue : 0,
          ",\"source_submission\":", cross_subresources ? representation->source_submission : 0,
          ",\"reused_subresources\":", reused_subresources, ",\"reused_tiles\":", reused_tiles);
    };
    if (representation && !to_canonical && reused_subresources == requested_subresources) {
      trace_reuse();
      return true;
    }
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
    const uint64_t tile_scratch_size = uint64_t(slice_pitch) * shape.tile_depth;
    const uint64_t max_buffer_length = metal.maxBufferLength();
    if (!tile_scratch_size || tile_scratch_size > max_buffer_length)
      return false;
    auto canonical = resource->buffer->current()->buffer();
    auto shadow = volume_source ? volume_source : resource->texture->current()->texture();
    const auto *volume_layout = resource->reserved ? resource->reserved->volumeLayout() : nullptr;
    constexpr uint32_t kMaxBatchTiles = 8;
    constexpr uint64_t kBatchScratchLimit = 1024 * 1024;
    // The limit is for this tile window, not all generations retained by an
    // in-flight submission. Preserve the original single-tile path for sparse
    // aliases/volume atlases and any tile larger than the batching budget.
    const bool concurrent = !resource->reserved && !volume_source && !volume_layout &&
                            tile_scratch_size <= kBatchScratchLimit;
    uint64_t max_mip_tiles = 1;
    for (uint32_t mip = 0; mip < layout.mip_count; ++mip) {
      if (selection && !selection->HasMip(mip)) continue;
      const auto &m = layout.mips[mip];
      max_mip_tiles = std::max(max_mip_tiles, uint64_t(m.tiles_x) * m.tiles_y * m.tiles_z);
    }
    const uint32_t batch_capacity = concurrent ? uint32_t(std::min({
        uint64_t(kMaxBatchTiles), kBatchScratchLimit / tile_scratch_size,
        max_buffer_length / tile_scratch_size, max_mip_tiles})) : 1;
    const uint64_t scratch_size = tile_scratch_size * batch_capacity;
    WMT::Reference<WMT::Buffer> scratch;
    if (inflight.swizzle_batch_scratch_length >= scratch_size && !inflight.swizzle_batch_buffers.empty()) {
      scratch = inflight.swizzle_batch_buffers.back();
    } else {
      WMTBufferInfo info{};
      info.length = scratch_size;
      info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
      info.memory.set(nullptr);
      scratch = metal.newBuffer(info);
      if (!scratch)
        return false;
      inflight.swizzle_batch_buffers.push_back(scratch);
      inflight.swizzle_batch_scratch_length = scratch_size;
    }
    struct SwizzleTile {
      D3D12StandardSwizzleParams params;
      WMTOrigin origin;
      WMTSize size;
      uint32_t native_level, native_slice, native_pitch, native_image_pitch;
    };
    std::array<SwizzleTile, kMaxBatchTiles> tiles{};
    uint32_t tile_count = 0;
    uint64_t encoded_tiles = 0, encoded_batches = 0;
    uint32_t encoded_subresources = 0;
    auto flush_batch = [&]() {
      if (!tile_count) return;
      if (to_canonical) {
        auto blit = cmdbuf.blitCommandEncoder();
        blit.waitForFence(fence_);
        for (uint32_t i = 0; i < tile_count; ++i) {
          const auto &tile = tiles[i];
          wmtcmd_blit_copy_from_texture_to_buffer_withblitoption copy{};
          copy.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          copy.src = shadow;
          copy.level = tile.native_level;
          copy.slice = tile.native_slice;
          copy.origin = tile.origin;
          copy.size = tile.size;
          copy.dst = scratch;
          copy.offset = uint64_t(i) * tile_scratch_size;
          copy.bytes_per_row = tile.native_pitch;
          copy.bytes_per_image = tile.native_image_pitch;
          copy.options = WMTBlitOptionNone;
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        }
        blit.updateFence(fence_);
        blit.endEncoding();
      }
      // Ordinary resources have separate canonical-buffer and shadow-texture
      // allocations. Each tile owns a disjoint 64 KiB canonical interval and
      // linear scratch slice; packed-422 edge preservation stays in that tile.
      // Dirty comparisons only share an idempotent atomic_store(1). The caller
      // still completes every comparison before starting conditional exports.
      auto compute = cmdbuf.computeCommandEncoder(concurrent);
      compute.waitForFence(fence_);
      for (uint32_t i = 0; i < tile_count; ++i) {
        auto p = tiles[i].params;
        const uint64_t scratch_offset = uint64_t(i) * tile_scratch_size;
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
        set_source.offset = to_canonical ? scratch_offset : 0;
        set_source.index = 0;
        set_source.next.set(&set_destination);
        set_destination.type = WMTComputeCommandSetBuffer;
        set_destination.buffer = compare && baseline ? baseline
            : to_canonical ? canonical : WMT::Buffer(scratch);
        set_destination.offset = (compare && baseline) || to_canonical ? 0 : scratch_offset;
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
        compute.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&set_pso));
      }
      compute.updateFence(fence_);
      compute.endEncoding();
      if (!to_canonical) {
        auto blit = cmdbuf.blitCommandEncoder();
        blit.waitForFence(fence_);
        for (uint32_t i = 0; i < tile_count; ++i) {
          const auto &tile = tiles[i];
          wmtcmd_blit_copy_from_buffer_to_texture_withblitoption copy{};
          copy.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          copy.src = scratch;
          copy.src_offset = uint64_t(i) * tile_scratch_size;
          copy.bytes_per_row = tile.native_pitch;
          copy.bytes_per_image = tile.native_image_pitch;
          copy.size = tile.size;
          copy.dst = shadow;
          copy.level = tile.native_level;
          copy.slice = tile.native_slice;
          copy.origin = tile.origin;
          copy.options = WMTBlitOptionNone;
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        }
        blit.updateFence(fence_);
        blit.endEncoding();
      }
      encoded_tiles += tile_count;
      ++encoded_batches;
      tile_count = 0;
    };
    for (uint32_t array = 0; array < layout.array_count; ++array) {
      for (uint32_t mip = 0; mip < layout.mip_count; ++mip) {
        if (selection && !selection->Contains(mip, array)) continue;
        if (representation && !to_canonical && representation->Contains(array * layout.mip_count + mip))
          continue;
        ++encoded_subresources;
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
              tiles[tile_count++] = {p, origin, size, native_level, native_slice, native_pitch, native_image_pitch};
              if (tile_count == batch_capacity) flush_batch();
            }
          }
        }
        // Do not mix resource, mip or array boundaries in a staging window.
        flush_batch();
      }
    }
    owned_graphics::Trace("standard_swizzle_batch", ",\"queue\":", owned_id_, ",\"resource\":", resource->owned_id,
        ",\"mode\":\"", compare ? "compare" : dirty_flags ? "export_if_dirty" : to_canonical ? "export" : "import", "\"",
        ",\"tiles\":", encoded_tiles, ",\"batches\":", encoded_batches, ",\"capacity\":", batch_capacity,
        ",\"subresources\":", encoded_subresources,
        ",\"total_subresources\":", layout.mip_count * layout.array_count,
        ",\"total_tiles\":", layout.total_tiles,
        ",\"tile_bytes\":", tile_scratch_size, ",\"window_bytes\":", scratch_size,
        ",\"scratch_bytes\":", inflight.swizzle_batch_scratch_length, ",\"concurrent\":", concurrent,
        ",\"encoder_count\":", encoded_batches * 2, ",\"wait_count\":", encoded_batches * 2,
        ",\"update_count\":", encoded_batches * 2);
    if (representation) {
      representation->synchronized_version = representation->content_version;
      for (uint32_t i = 0; i < requested.size(); ++i) representation->valid[i] |= requested[i];
    }
    trace_reuse();
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
            library.newFunction(array ? "d3dalloy_depth_msaa_import_array" : "d3dalloy_depth_msaa_import"), error);
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
    if (std::any_of(uses.begin(), uses.end(), [](const auto &use) {
          return bool(use.cast_view) || use.msaa_sync || use.cast_sync;
        })) {
      // These paths can merge several physical representations on the GPU.
      // Their actual writer is not described by an ordinary texture use.
      inflight.swizzle_validity_enabled = false;
      inflight.InvalidateSwizzleRepresentations();
    }
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
        copy.dst = use.baseline;
        auto blit = cmdbuf.blitCommandEncoder();
        blit.waitForFence(fence_);
        uint64_t copied_bytes = 0;
        uint32_t copy_count = 0;
        auto copy_interval = [&](uint64_t offset, uint64_t bytes) {
          copy.src_offset = copy.dst_offset = offset;
          copy.copy_length = bytes;
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          copied_bytes += bytes;
          ++copy_count;
        };
        const auto &layout = *use.resource->swizzle_layout;
        if (use.exports.whole)
          copy_interval(0, info.length);
        else {
          // Preserve global canonical offsets. Only these subresources will be
          // compared, so unselected baseline bytes need neither copying nor use.
          for (uint32_t array = 0; array < layout.array_count; ++array)
            for (uint32_t mip = 0; mip < layout.mip_count; ++mip) {
              if (!use.exports.Contains(mip, array)) continue;
              const auto &m = layout.mips[mip];
              copy_interval((array * layout.tiles_per_slice + m.first_tile_in_slice) * 65536,
                            uint64_t(m.tiles_x) * m.tiles_y * m.tiles_z * 65536);
            }
        }
        blit.updateFence(fence_);
        blit.endEncoding();
        owned_graphics::Trace("standard_swizzle_baseline", ",\"queue\":", owned_id_,
            ",\"resource\":", use.resource->owned_id, ",\"allocation_bytes\":", info.length,
            ",\"copied_bytes\":", copied_bytes, ",\"copies\":", copy_count);
      }
      if (!EncodeStandardSwizzle(cmdbuf, inflight, use.resource.ptr(), false, {}, 0, false, {}, {}, false,
                                 &use.imports))
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
                                   uses[index].baseline, uses[index].volume_source, false, &uses[index].exports))
          return false;
    }
    for (uint32_t i = 0; i < uses.size(); ++i) {
      const auto &use = uses[i];
      if (!use.resource || !use.writes)
        continue;
      if (!EncodeStandardSwizzle(cmdbuf, inflight, use.resource.ptr(), true,
                                 use.baseline ? WMT::Buffer(dirty_flags) : WMT::Buffer{},
                                 uint64_t(i) * sizeof(uint32_t), false, {}, use.volume_source, false, &use.exports))
        return false;
    }
    return true;
  }

  static bool HasUnaliasedRTASStorage(const raytracing::BufferLocation &location) {
    const auto *allocation = static_cast<const BufferAllocation *>(location.allocationIdentity);
    // Placed aliases, sparse remapping and shared/CPU-backed storage can change
    // the contents through a different ArenaKey. Keep their GPU readback path.
    return allocation && !allocation->canonicalHeap && !allocation->reservedResource &&
        !allocation->flags().test(BufferAllocationFlag::PlacementSparse) &&
        !allocation->flags().test(BufferAllocationFlag::CpuPlaced);
  }

  // Internal AS helper kernels use GPU addresses in their POD parameters. Buffers used for
  // variable-length metadata are retained by the same submission as the dispatch.
  bool EncodeASHelper(WMT::MTL4CommandBuffer cmdbuf, WMT::ComputePipelineState pipeline,
                      const void *parameters, uint32_t bytes, uint32_t threads = raytracing::kHelperThreads,
                      WMT::Buffer geometries = {}, WMT::Buffer jobs = {},
                      uint64_t jobsOffset = 0, uint64_t geometriesOffset = 0, uint32_t rows = 1) {
    if (!pipeline) return false;
    auto encoder = cmdbuf.computeCommandEncoder(false);
    encoder.waitForFence(fence_);
    wmtcmd_compute_nop head{};
    head.type = WMTComputeCommandNop;
    wmtcmd_compute_setpso pso{};
    pso.type = WMTComputeCommandSetPSO;
    pso.pso = pipeline;
    pso.threadgroup_size = {std::min(threads, raytracing::kHelperThreadgroup), 1, 1};
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
      buffers[i].offset = i ? jobsOffset : geometriesOffset;
    }
    wmtcmd_compute_dispatch dispatch{};
    dispatch.type = WMTComputeCommandDispatchThreads;
    dispatch.size = {threads, rows, 1};
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
    owned_graphics::Trace("rtas_helper_dispatch", ",\"queue\":", owned_id_, ",\"threads\":", threads, ",\"rows\":", rows);
    return true;
  }

  void
  ProcessExecute(ExecuteTask &task, std::vector<SignalTask> *folded_signals = nullptr,
                 std::optional<QueueTask> *pending_task = nullptr) {
    uint64_t first_segment = 0;
    std::optional<CommittingScope> scope;
    struct AtomicPhasePermit {
      MTLD3D12CommandQueueImpl *queue;
      std::optional<CommittingScope> &scope;
      GPUExecutionGate::Ticket ticket;
      std::optional<GPUExecutionGate::EncodingAdmission> admission;
      std::shared_ptr<Atomic64Phase> phase;
      bool private_context = false;
      void Retire(bool reusable = false) {
        if (!ticket) return;
        scope.reset();
        const auto last = queue->submission_ring_.lastPublished();
        if (last) queue->submission_ring_.waitUntilRetired(last);
        if (reusable && phase && queue->device_->RemovedReason() == S_OK) phase->MarkRetired();
        phase.reset();
        admission.reset();
        ticket.Reset();
      }
      ~AtomicPhasePermit() { Retire(); }
    } atomic{this, scope};
    // Closed recording topology is immutable for the whole Execute task. Do
    // not walk it again after each temporal/readback/aliasing segment split.
    std::vector<WMT::Allocation> task_allocations;
    std::shared_ptr<SubmissionResourcePins> resolved_resources;
    GPUExecutionGate::Accesses ordinary_accesses;
    size_t rtas_passes = 0;
    bool task_metadata_ready = false;
    auto begin_segment = [&] {
      if (!resolved_resources) resolved_resources = std::make_shared<SubmissionResourcePins>();
      if (!task_metadata_ready) {
        std::unordered_set<obj_handle_t> buffers;
        auto remember = [&](WMT::Buffer buffer) {
          if (buffer && buffers.insert(buffer.handle).second)
            task_allocations.push_back(WMT::Allocation{buffer.handle});
        };
        for (const auto &list : task.lists) {
          const auto &head = *static_cast<const RecordingHead *>(list.entry);
          for (const auto &heap : head.descriptor_heaps) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{}; heap->GetDesc(&desc);
            if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
              remember(static_cast<MTLD3D12DescriptorHeap *>(heap.ptr())->GetArgumentBuffer());
            else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
              remember(static_cast<MTLD3D12SamplerDescriptorHeap *>(heap.ptr())->GetArgumentBuffer());
          }
          for (const auto &allocation : head.root_allocations) remember(allocation->buffer());
          rtas_passes += head.raytracing_passes;
        }
        if (std::any_of(task.lists.begin(), task.lists.end(), [](const auto &list) {
              return static_cast<const RecordingHead *>(list.entry)->gpu_indirect_buffer_addresses;
            })) {
          // Addresses are chosen by the GPU. Pin the live registry once for this
          // Execute task, and publish each native allocation to every segment.
          // Do not keep this conservative snapshot in the replayable recording.
          auto pins = device_->SnapshotBufferAllocations();
          for (const auto &allocation : pins) remember(allocation->buffer());
          owned_graphics::Trace("indirect_gpu_buffer_pins", ",\"queue\":", owned_id_,
                                ",\"allocations\":", pins.size());
          resolved_resources->buffer_owners.insert(resolved_resources->buffer_owners.end(),
              std::make_move_iterator(pins.begin()), std::make_move_iterator(pins.end()));
        }
        task_metadata_ready = true;
      }
      scope.emplace(this, bool(atomic.ticket), ordinary_accesses,
                    !first_segment && device_->HasStandardSwizzleTextures());
      if (!first_segment) first_segment = scope->seq;
      if (!atomic.ticket && ordinary_accesses)
        owned_graphics::Trace("atomic64_ordinary_admission", ",\"queue\":", owned_id_,
            ",\"submission\":", scope->seq, ",\"ranges\":", ordinary_accesses->size(),
            ",\"preflight\":1,\"independent_peers\":", scope->inflight.execution_ticket.IndependentPeers());
      scope->inflight.resolved_resources = resolved_resources;
      scope->inflight.reflex_work = task.reflex_work;
      if (task.reflex_work) scope->inflight.reflex_driver_start = ReflexState::NowUs();
      scope->inflight.protections.reserve(task.lists.size());
      for (auto &list : task.lists)
        scope->inflight.protections.push_back({list.allocator.Retain(), list.recording_lease.Retain()});
      for (auto allocation : task_allocations) scope->inflight.cmdbuf.useAllocation(allocation);
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
       * counted once per task, and Emit passes are simply never added.
       */
      scope->inflight.rtas_observations.reserve(rtas_passes);
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
                            std::vector<SwizzlePassResource> *swizzle = nullptr,
                            const ShaderImmutableConstants *constants = nullptr, uint64_t encoder_id = 0,
                            const ClosedPassShape *shape = nullptr) -> bool {
      const auto preceding = scope->seq;
      scope.reset();
      submission_ring_.waitUntilRetired(preceding);
      Atomic64PhaseBindings bindings;
      GPUExecutionGate::Accesses accesses;
      HRESULT hr = S_OK;
      if (constants) {
        // CPU descriptor/VA snapshots alone run before admission. A scoped
        // proof accepts only retained, nonaliasing committed native storage.
        hr = CollectAtomic64PhaseBindings(device_, tables ? std::span<const StandardSwizzleTableUse>(*tables)
                                                         : std::span<const StandardSwizzleTableUse>{}, roots, bindings);
        if (SUCCEEDED(hr)) accesses = AtomicAdmissionProof(device_, bindings);
      }
      bool private_domain = false;
      if (shape && accesses) {
        try { private_domain = IndependentAtomicDomain(device_, *shape, bindings, accesses); }
        catch (const std::bad_alloc &) { /* Optional proof; retain SC exclusion. */ }
      }
      atomic.ticket = device_->gpu_execution_gate.AcquireAtomic(accesses, private_domain);
      if (!atomic.ticket) return false;
      atomic.admission.emplace(device_->gpu_execution_gate);
      if (!accesses) {
        // Unknown/sparse topology is captured again under the original global
        // permit. A speculative preflight is never a mapping-ready epoch.
        bindings = {};
        hr = CollectAtomic64PhaseBindings(device_, tables ? std::span<const StandardSwizzleTableUse>(*tables)
                                                         : std::span<const StandardSwizzleTableUse>{}, roots, bindings);
      }
      if (SUCCEEDED(hr)) hr = PrepareAtomic64Phase(device_, std::max<uint64_t>(participants, 1), bindings,
                                                 atomic.phase, atomic64_phase_cache_);
      atomic.private_context = constants && !constants->atomic64_relocations.empty();
      if (SUCCEEDED(hr) && atomic.private_context) hr = atomic.phase->CreatePrivateConstants(*constants);
      if (FAILED(hr)) { device_->ReportRemoval(hr); return false; }
      if (!start_segment()) return false;
      scope->inflight.atomic64_phase = atomic.phase;
      if (swizzle && !BeginStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, *swizzle)) {
        device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return false;
      }
      hr = atomic.phase->EncodeImport(scope->inflight.cmdbuf, fence_, !atomic.private_context);
      if (FAILED(hr)) { device_->ReportRemoval(hr); return false; }
      owned_graphics::Trace("atomic64_admission", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
          ",\"encoder\":", encoder_id, ",\"scoped\":", bool(accesses), ",\"ranges\":", accesses ? accesses->size() : 0,
          ",\"private_context\":", atomic.private_context, ",\"constant_bytes\":", atomic.private_context ? constants->size : 0,
          ",\"publication_count\":", atomic.private_context ? 0 : 1,
          ",\"independent_peers\":", atomic.ticket.IndependentPeers(), ",\"private_domain\":", private_domain,
          ",\"atomic_peers\":", atomic.ticket.AtomicPeers());
      return true;
    };
    auto end_atomic = [&](std::vector<SwizzlePassResource> *swizzle = nullptr) -> bool {
      HRESULT hr = atomic.phase->EncodeExport(scope->inflight.cmdbuf, fence_);
      if (SUCCEEDED(hr) && swizzle && !EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, *swizzle))
        hr = DXGI_ERROR_DRIVER_INTERNAL_ERROR;
      if (SUCCEEDED(hr) && !atomic.private_context) hr = atomic.phase->ClearContext(scope->inflight.cmdbuf, fence_);
      if (FAILED(hr)) { device_->ReportRemoval(hr); return false; }
      owned_graphics::Trace("atomic64_context_end", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
          ",\"private_context\":", atomic.private_context, ",\"clear_count\":", atomic.private_context ? 0 : 1,
          ",\"retirement_wait_count\":1");
      atomic.Retire(true);
      return device_->RemovedReason() == S_OK && start_segment();
    };
    auto retire_msaa = [&](WMT::MTL4CommandBuffer &cmdbuf, InflightCommandBuffer *&inflight,
                           uint64_t &producer) -> bool {
      // A quantum boundary is a real retirement, not an encode/commit lifetime
      // boundary. Carry raw-ID view metadata, sparse backing and scratch owners
      // into the resumed segment, and republish their native residency there.
      const auto &previous = scope->inflight;
      auto buffers = previous.msaa_buffers;
      auto textures = previous.msaa_textures;
      auto markers = previous.rov_markers;
      auto indirect_buffers = previous.so_indirect_buffers;
      auto indirect_owners = previous.mesh_indirect_owners;
      auto swizzle_buffers = previous.swizzle_buffers;
      auto swizzle_batch_buffers = previous.swizzle_batch_buffers;
      auto swizzle_dirty_flags = previous.swizzle_dirty_flags;
      auto swizzle_baselines = previous.swizzle_baselines;
      auto swizzle_resources = previous.swizzle_resources;
      auto cast_views = previous.cast_views;
      auto cast_allocations = previous.cast_allocations;
      auto volume_heaps = previous.volume_heaps;
      auto volume_textures = previous.volume_textures;
      const auto swizzle_scratch_length = previous.swizzle_scratch_length;
      const auto swizzle_batch_scratch_length = previous.swizzle_batch_scratch_length;
      producer = scope->seq;
      scope.reset();
      submission_ring_.waitUntilRetired(producer);
      if (device_->RemovedReason() != S_OK || !start_segment()) return false;
      auto &next = scope->inflight;
      next.msaa_buffers = std::move(buffers);
      next.msaa_textures = std::move(textures);
      next.rov_markers = std::move(markers);
      next.so_indirect_buffers = std::move(indirect_buffers);
      next.mesh_indirect_owners = std::move(indirect_owners);
      next.swizzle_buffers = std::move(swizzle_buffers);
      next.swizzle_scratch_length = swizzle_scratch_length;
      next.swizzle_batch_buffers = std::move(swizzle_batch_buffers);
      next.swizzle_batch_scratch_length = swizzle_batch_scratch_length;
      next.swizzle_dirty_flags = std::move(swizzle_dirty_flags);
      next.swizzle_baselines = std::move(swizzle_baselines);
      next.swizzle_resources = std::move(swizzle_resources);
      next.cast_views = std::move(cast_views);
      next.cast_allocations = std::move(cast_allocations);
      next.volume_heaps = std::move(volume_heaps);
      next.volume_textures = std::move(volume_textures);
      if (atomic.phase) next.atomic64_phase = atomic.phase;
      auto reside = [&](const auto &resources) {
        for (const auto &resource : resources)
          if (resource) next.cmdbuf.useAllocation(WMT::Allocation{resource.handle});
      };
      reside(next.msaa_buffers); reside(next.msaa_textures); reside(next.so_indirect_buffers);
      reside(next.swizzle_buffers); reside(next.swizzle_dirty_flags); reside(next.swizzle_baselines);
      reside(next.swizzle_batch_buffers);
      reside(next.volume_heaps); reside(next.volume_textures);
      for (const auto &marker : next.rov_markers)
        next.cmdbuf.useAllocation(WMT::Allocation{marker.texture.handle});
      for (const auto &owner : next.mesh_indirect_owners)
        next.cmdbuf.useAllocation(WMT::Allocation{owner->buffer().handle});
      for (const auto &view : next.cast_views) {
        if (!view) continue;
        if (view->texture) next.cmdbuf.useAllocation(WMT::Allocation{view->texture.handle});
        if (view->msaaSampleAddress) {
          next.cmdbuf.useAllocation(WMT::Allocation{view->msaaSampleIds.handle});
          for (uint32_t sample = 0; sample < view->allocation->descriptor->sampleCount(); ++sample)
            next.cmdbuf.useAllocation(WMT::Allocation{view->msaaViews[sample].handle});
        }
      }
      cmdbuf = next.cmdbuf;
      inflight = &next;
      return true;
    };
    struct AtomicReadback { WMT::Reference<WMT::Buffer> owner; const uint8_t *bytes = nullptr; };
    struct AtomicReadbackRequest {
      wmtcmd_blit_copy_from_buffer_to_buffer copy{};
      Rc<BufferAllocation> source_owner;
      AtomicReadback result;
    };
    using AtomicReadbackBatch = std::vector<AtomicReadbackRequest>;
    auto append_atomic_buffer = [&](AtomicReadbackBatch &batch, WMT::Buffer source, uint64_t offset,
                                    uint64_t length) -> size_t {
      if (!source || !length || offset > UINT64_MAX - length) {
        device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return SIZE_MAX;
      }
      auto &request = batch.emplace_back();
      request.copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      request.copy.src = source; request.copy.src_offset = offset; request.copy.copy_length = length;
      return batch.size() - 1;
    };
    auto append_atomic_address = [&](AtomicReadbackBatch &batch, uint64_t address, uint64_t length) -> size_t {
      uint64_t offset = 0; auto allocation = device_->LookupBufferByVA(address, &offset);
      if (!allocation || offset > allocation->byteLength() || length > allocation->byteLength() - offset) {
        device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return SIZE_MAX;
      }
      const auto index = append_atomic_buffer(batch, allocation->buffer(), offset, length);
      if (index != SIZE_MAX) batch[index].source_owner = std::move(allocation);
      return index;
    };
    auto read_atomic_batch = [&](AtomicReadbackBatch &batch, const char *stage, uint64_t encoder_id) -> bool {
      if (batch.empty()) return true;
      const uint64_t limit = device_->GetMTLDevice().maxBufferLength();
      uint64_t copied_bytes = 0;
      size_t allocations = 0;
      // Finish all allocations and command links before encoding. Requests are
      // referred to by index while the vector grows; VA owners survive the wait.
      for (size_t first = 0; first < batch.size();) {
        uint64_t length = 0;
        size_t last = first;
        for (; last < batch.size(); ++last) {
          auto &copy = batch[last].copy;
          if (copy.copy_length > limit || copied_bytes > UINT64_MAX - copy.copy_length) {
            device_->ReportRemoval(E_OUTOFMEMORY); return false;
          }
          if (length > UINT64_MAX - 15) break;
          const uint64_t offset = (length + 15) & ~uint64_t(15);
          if (offset > limit || copy.copy_length > limit - offset) break;
          copy.dst_offset = offset;
          length = offset + copy.copy_length;
          copied_bytes += copy.copy_length;
        }
        WMTBufferInfo info{};
        info.length = std::max<uint64_t>(length, 16);
        info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
        auto owner = device_->GetMTLDevice().newBuffer(info);
        auto *bytes = static_cast<const uint8_t *>(info.memory.get_accessible_or_null());
        if (!owner || !bytes) { device_->ReportRemoval(E_OUTOFMEMORY); return false; }
        ++allocations;
        for (size_t index = first; index < last; ++index) {
          auto &request = batch[index];
          request.result.owner = owner;
          request.result.bytes = bytes + request.copy.dst_offset;
          request.copy.dst = owner;
        }
        first = last;
      }
      for (size_t index = 0; index < batch.size(); ++index) {
        auto &copy = batch[index].copy;
        copy.next.set(index + 1 < batch.size() ? &batch[index + 1].copy : nullptr);
        scope->inflight.cmdbuf.useAllocation(WMT::Allocation{copy.src});
        scope->inflight.cmdbuf.useAllocation(WMT::Allocation{copy.dst});
      }
      auto blit = scope->inflight.cmdbuf.blitCommandEncoder();
      blit.waitForFence(fence_);
      blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&batch.front().copy));
      blit.updateFence(fence_); blit.endEncoding();
      const auto producer = scope->seq;
      owned_graphics::Trace("atomic_readback_batch", ",\"queue\":", owned_id_, ",\"encoder\":", encoder_id,
          ",\"producer\":", producer, ",\"stage\":\"", stage, "\",\"requests\":", batch.size(),
          ",\"bytes\":", copied_bytes, ",\"allocations\":", allocations, ",\"blits\":1,\"waits\":1");
      scope.reset(); submission_ring_.waitUntilRetired(producer);
      return device_->RemovedReason() == S_OK && start_segment();
    };
    auto read_atomic_buffer = [&](WMT::Buffer source, uint64_t offset, uint64_t length, AtomicReadback &result) -> bool {
      AtomicReadbackBatch batch;
      if (append_atomic_buffer(batch, source, offset, length) == SIZE_MAX ||
          !read_atomic_batch(batch, "single", 0)) return false;
      result = std::move(batch.front().result);
      return true;
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
        case WMTRenderCommandD3DalloyGeometryDraw: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_d3dalloy_geometry_draw *>(node);
          if (!add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp}) ||
              (!draw && !add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp, render.atomic64_gs_instances}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandD3DalloyGeometryDrawIndexed: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_d3dalloy_geometry_draw_indexed *>(node);
          if (!add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp}) ||
              (!draw && !add({cmd->warp_count, cmd->instance_count, cmd->vertex_per_warp, render.atomic64_gs_instances}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandD3DalloyTessellationMeshDraw: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_d3dalloy_tessellation_mesh_draw *>(node);
          if (!add({cmd->patch_per_mesh_instance, cmd->instance_count, cmd->threads_per_patch, cmd->patch_per_group}) ||
              (!draw && !add({cmd->patch_per_mesh_instance, cmd->instance_count, cmd->patch_per_group, render.atomic64_tess_workloads, 32}))) return UINT64_MAX;
          break;
        }
        case WMTRenderCommandD3DalloyTessellationMeshDrawIndexed: {
          const auto *cmd = reinterpret_cast<const wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed *>(node);
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
    // The prologue and epilogue of a single direct Atomic dispatch are
    // ordinary submissions. Give them the full physical proof as well, so
    // starting this task behind another phase need not wait at the prologue.
    const ClosedPassShape *atomic_envelope = nullptr;
    if (task.lists.size() == 1 && task.lists.front().fence_operations.empty()) {
      const auto &head = *static_cast<const RecordingHead *>(task.lists.front().entry);
      if (!head.gpu_indirect_buffer_addresses && head.pass_shapes.size() == 1 &&
          head.pass_shapes.front().record->type == EncoderType::Compute &&
          static_cast<const ComputeEncoderData *>(head.pass_shapes.front().record)->atomic64_phase)
        atomic_envelope = &head.pass_shapes.front();
    }
    auto envelope_proof = [&]() -> GPUExecutionGate::Accesses {
      const auto &c = *static_cast<const ComputeEncoderData *>(atomic_envelope->record);
      if (!c.submission_access_complete || !c.atomic64_constants || !c.atomic64_indirect.empty() ||
          c.atomic64_ray || !c.cast_textures.empty() || !atomic_envelope->compute_commands_complete ||
          StandardSwizzleTablesMayMatter()) return {};
      Atomic64PhaseBindings bindings;
      if (FAILED(CollectAtomic64PhaseBindings(device_, c.swizzle_tables ? std::span<const StandardSwizzleTableUse>(*c.swizzle_tables)
          : std::span<const StandardSwizzleTableUse>{}, c.atomic64_roots, bindings))) return {};
      auto physical = AtomicAdmissionProof(device_, bindings);
      return IndependentAtomicDomain(device_, *atomic_envelope, bindings, physical) ? physical : GPUExecutionGate::Accesses{};
    };
    auto ordinary_preflight = [&]() -> GPUExecutionGate::Accesses {
      // Only a scoped SC owner makes this additional snapshot useful. Normal
      // frames obtain their proof from the existing planner below, at no extra
      // descriptor-walk cost. This preflight records no native GPU commands.
      if (!device_->gpu_execution_gate.WantsKnownAccess() || StandardSwizzleTablesMayMatter()) return {};
      if (atomic_envelope) return envelope_proof();
      auto proof = std::make_shared<PhysicalAdmissionProof>();
      SubmissionPlan snapshots;
      size_t records = 0;
      for (const auto &list : task.lists) {
        const auto *head = static_cast<const RecordingHead *>(list.entry);
        if (!list.fence_operations.empty() || head->gpu_indirect_buffer_addresses) return {};
        for (const auto &shape : head->pass_shapes) {
          if (++records > 64) return {};
          SubmissionNode node{};
          node.access_complete = true;
          auto append_buffers = [&](const auto &buffers, uint64_t stages) {
            for (const auto &use : buffers)
              if (!AppendSubmissionBuffer(node, device_, use.address, use.length, use.writes,
                                           use.stages ? use.stages : stages)) return false;
            return true;
          };
          if (shape.record->type == EncoderType::Compute) {
            const auto &compute = *static_cast<const ComputeEncoderData *>(shape.record);
            if (!compute.submission_access_complete || compute.submission_root_srv_opaque ||
                compute.submission_snapshot_budget || compute.atomic64_phase || !compute.atomic64_indirect.empty() ||
                compute.atomic64_ray || !compute.cast_textures.empty() || !shape.compute_commands_complete ||
                !append_buffers(compute.submission_buffers, WMTMetal4StageDispatch) ||
                !ResolveSubmissionTablesUncached(snapshots, node, device_, shape)) return {};
          } else if (shape.record->type == EncoderType::Render) {
            const auto &render = *static_cast<const RenderEncoderData *>(shape.record);
            if (OrdinaryRenderRejection(render, device_, true) || render.native_msaa_sample_count > 1 ||
                render.visibility_scratch || !render.visibility_targets.empty() ||
                !append_buffers(render.submission_buffers, WMTMetal4StageVertex | WMTMetal4StageFragment) ||
                !ResolveSubmissionTablesUncached(snapshots, node, device_, shape)) return {};
            auto attachment = [&](const TextureViewRef &view, uint8_t aspects) {
              return !view || (!view->castBlockSize && !view->msaaSampleAddress && !view->reservedCast &&
                  !view->reservedVolumeRender && !view->reservedVolumeAtlasCast &&
                  AppendSubmissionTexture(node, device_, view->allocation->descriptor, view->allocation, view->key,
                      aspects, true, true, WMTMetal4StageFragment));
            };
            for (const auto &color : render.colors)
              if (color.buffer_attachment || color.resolve_attachment || !attachment(color.attachment, 4)) return {};
            if (!attachment(render.depth.attachment, 1) || !attachment(render.stencil.attachment, 2)) return {};
          } else if (shape.record->type == EncoderType::Blit) {
            const auto &blit = *static_cast<const BlitEncoderData *>(shape.record);
            if (!blit.submission_access_complete || blit.submission_buffers.size() != blit.submission_copy_count * 2 ||
                !append_buffers(blit.submission_buffers, WMTMetal4StageBlit)) return {};
            for (const auto &use : blit.submission_uploads)
              if (!AppendSubmissionBuffer(node, device_, use.address, use.length, use.writes, WMTMetal4StageBlit)) return {};
            for (const auto &use : blit.submission_textures) {
              const auto aspects = DepthStencilPlanarFlags(use.texture->pixelFormat());
              if (!AppendSubmissionTexture(node, device_, use.texture.ptr(), use.texture->current(), use.key,
                  aspects ? uint8_t(aspects) : uint8_t(4), use.writes, false, WMTMetal4StageBlit)) return {};
            }
          } else return {};
          if (node.helper_passes || node.sparse_resource_used) return {};
          for (const auto &access : node.accesses)
            if (!AppendAdmissionAccess(*proof, access, node.allocation_pins, true)) return {};
        }
      }
      proof->resources = std::move(snapshots.descriptor_owners);
      return ShareAdmissionProof(std::move(proof));
    };
    try {
      ordinary_accesses = ordinary_preflight();
    } catch (const std::bad_alloc &) {
      // Optional proof allocation failure preserves the original unknown gate.
      ordinary_accesses.reset();
    }
    if (!start_segment())
      return;
    const bool fenced = std::any_of(task.lists.begin(), task.lists.end(), [](const auto &list) { return !list.fence_operations.empty(); });

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
          for (size_t index = segment ? first : 0; index < limit; ++index) {
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
              for (size_t index = 0; index < events.size(); ++index)
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
    // Value-owned scratch over const arena records. Every member remains under
    // the existing Execute leases; neither links nor payloads are rewritten.
    SubmissionPlan plan;
    try {
      for (size_t ordinal = 0; ordinal < task.lists.size(); ++ordinal) {
        const auto *head = static_cast<const RecordingHead *>(task.lists[ordinal].entry);
        const size_t list_begin = plan.nodes.size();
        size_t node_ordinal = 0;
        for (const auto &shape : head->pass_shapes) {
          const auto *record = shape.record;
          SubmissionNode node{}; node.record = record; node.list_ordinal = ordinal; node.node_ordinal = node_ordinal++;
          const uint64_t render_stages = WMTMetal4StageVertex | WMTMetal4StageFragment;
          auto append_buffers = [&](const auto &buffers, uint64_t stages) {
            for (const auto &use : buffers) {
              if (!node.access_complete) break;
              node.access_complete = AppendSubmissionBuffer(node, device_, use.address, use.length, use.writes, use.stages ? use.stages : stages);
            }
          };
          auto append_attachment = [&](const auto &attachment, uint8_t aspect, uint32_t samples, uint32_t layers) {
            if (!node.access_complete || !attachment.attachment) return;
            const auto *view = attachment.attachment.ptr();
            const TextureViewRef *resolve = nullptr;
            if constexpr (requires { attachment.resolve_attachment; })
              if (attachment.resolve_attachment) resolve = &attachment.resolve_attachment;
            const bool folded_resolve = resolve && samples > 1 &&
                (attachment.store_action == WMTStoreActionStoreAndMultisampleResolve ||
                 attachment.store_action == WMTStoreActionMultisampleResolve);
            if (view->castBlockSize || view->msaaSampleAddress || view->reservedCast || view->reservedVolumeRender ||
                view->reservedVolumeAtlasCast ||
                (resolve && !folded_resolve) ||
                (!folded_resolve && attachment.store_action != WMTStoreActionStore &&
                 attachment.store_action != WMTStoreActionDontCare) ||
                (attachment.load_action != WMTLoadActionLoad && attachment.load_action != WMTLoadActionClear)) {
              node.access_complete = false; return;
            }
            // Attachment level/slice are relative to the view. A layered pass
            // renders from that slice to the end of the view; a 3D depth plane
            // stays inside its mip, which the whole-mip span covers.
            auto key = view->key;
            if (attachment.level >= key.mip_end - key.mip_start ||
                attachment.slice >= key.array_end - key.array_start) {
              node.access_complete = false; return;
            }
            key.mip_start = key.mip_start + attachment.level;
            key.mip_end = key.mip_start + 1;
            key.array_start = key.array_start + attachment.slice;
            if (layers <= 1) key.array_end = key.array_start + 1;
            node.access_complete = AppendSubmissionTexture(node, device_, view->allocation->descriptor,
                view->allocation, key, aspect, true, true, WMTMetal4StageFragment, samples);
            if (node.access_complete && folded_resolve) {
              const auto *target = resolve->ptr();
              node.access_complete = AppendSubmissionTexture(node, device_, target->allocation->descriptor,
                  target->allocation, target->key, aspect, true, true, WMTMetal4StageFragment);
            }
          };
          if (record->type == EncoderType::Render) {
            const auto &render = *static_cast<const RenderEncoderData *>(record);
            for (const auto &color : render.colors)
              node.placed_attachments |= color.attachment && bool(color.attachment->allocation->canonicalHeap);
            node.placed_attachments |= (render.depth.attachment && bool(render.depth.attachment->allocation->canonicalHeap)) ||
                (render.stencil.attachment && bool(render.stencil.attachment->allocation->canonicalHeap));
            const uint32_t samples = render.native_msaa_sample_count ? render.native_msaa_sample_count : 1;
            node.rejection = fenced ? "fence_segment" : OrdinaryRenderRejection(render, device_, true);
            const char *merge_rejection = fenced ? nullptr : OrdinaryRenderRejection(render, device_);
            node.access_complete = !node.rejection;
            if (node.access_complete) append_buffers(render.submission_buffers, render_stages);
            if (node.access_complete) node.access_complete = ResolveSubmissionTables(plan, node, device_, shape);
            if (node.access_complete && StandardSwizzleTablesMayMatter())
              node.access_complete = CollectStandardSwizzleTables(render).empty();
            if (node.access_complete && render.visibility_scratch)
              node.access_complete = AppendSubmissionNativeBuffer(node, render.visibility_scratch, true,
                  WMTMetal4StageFragment);
            if (render.visibility_scratch) node.boundary_before = true;
            if (node.access_complete)
              for (const auto &target : render.visibility_targets)
                if (!AppendSubmissionNativeBuffer(node, target.destination, true, WMTMetal4StageDispatch)) {
                  node.access_complete = false;
                  break;
                }
            const uint32_t layers = render.render_target_array_length;
            for (const auto &color : render.colors) append_attachment(color, 4, samples, layers);
            append_attachment(render.depth, 1, samples, layers); append_attachment(render.stencil, 2, samples, layers);
            // A shader-visible attachment feedback loop needs its own native
            // side-work proof; it cannot become an ordinary load/store group.
            for (const auto &attachment : node.accesses)
              if (attachment.attachment)
                for (const auto &shader : node.accesses)
                  if (!shader.attachment && AccessOverlaps(attachment, shader)) node.access_complete = false;
            for (const auto &color : render.colors)
              // A resolve-only store leaves the multisample source undefined, like DONT_CARE.
              if (color.attachment && (color.store_action == WMTStoreActionDontCare ||
                                       color.store_action == WMTStoreActionMultisampleResolve))
                node.boundary_before = node.boundary_after = true;
            if ((render.depth.attachment && render.depth.store_action == WMTStoreActionDontCare) ||
                (render.stencil.attachment && render.stencil.store_action == WMTStoreActionDontCare))
              node.boundary_before = node.boundary_after = true;
            if (render.render_pass_requires_fence) node.force_fence_before = true;
            if (node.sparse_resource_used) node.rejection = "sparse_alias";
            else if (!node.access_complete && !node.rejection) node.rejection = "access_or_side_work_unresolved";
            // Descriptor-only restrictions keep exact accesses but never merge.
            if (node.access_complete && !node.rejection) node.rejection = merge_rejection;
            node.merge_eligible = node.access_complete && !node.rejection && samples == 1 &&
                !render.visibility_scratch && render.visibility_targets.empty();
            owned_graphics::Trace("submission_render_candidate", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                ",\"list_ordinal\":", ordinal, ",\"node_ordinal\":", node.node_ordinal, ",\"encoder\":", record->id,
                ",\"eligible\":", node.merge_eligible, ",\"reason\":\"", node.rejection ? node.rejection : "eligible", "\"");
          } else if (record->type == EncoderType::Compute) {
            const auto &compute = *static_cast<const ComputeEncoderData *>(record);
            if (compute.submission_root_srv_opaque) node.rejection = "unresolved_root_srv";
            else if (compute.submission_snapshot_budget) node.rejection = "recorded_buffer_budget";
            node.access_complete = !node.rejection && !fenced && compute.submission_access_complete && !compute.atomic64_phase &&
                compute.atomic64_indirect.empty() && !compute.atomic64_ray && compute.cast_textures.empty();
            if (node.access_complete) append_buffers(compute.submission_buffers, WMTMetal4StageDispatch);
            if (node.access_complete) node.access_complete = ResolveSubmissionTables(plan, node, device_, shape);
            if (node.access_complete) {
              // Helper collection scans bound ranges, including shader-unused
              // descriptors. Import/export touches every slice, not just the UAV view.
              auto uses = StandardSwizzleTablesMayMatter() ? CollectStandardSwizzleTables(compute)
                                                           : std::vector<SwizzlePassResource>{};
              for (auto &use : uses) {
                if (use.resource || !use.msaa_texture || use.cast_view->castBlockSize ||
                    use.cast_view->reservedVolumeRender || !use.descriptor_owner) {
                  node.access_complete = false;
                  break;
                }
                auto key = use.cast_view->key;
                key.mip_start = key.array_start = 0;
                key.mip_end = 1;
                key.array_end = use.msaa_texture->arrayLength();
                node.helper_passes = true;
                if (!AppendSubmissionTexture(node, device_, use.msaa_texture, use.cast_allocation.ptr(), key,
                    4, true, false, WMTMetal4StageDispatch | WMTMetal4StageFragment,
                    use.msaa_texture->sampleCount())) {
                  node.access_complete = false;
                  break;
                }
                if (plan.descriptor_owner_ids.insert(use.descriptor_owner->owned_id).second)
                  plan.descriptor_owners.push_back(std::move(use.descriptor_owner));
              }
            }
            node.access_complete &= shape.compute_commands_complete;
            if (node.sparse_resource_used) node.rejection = "sparse_alias";
            if (node.access_complete && !node.helper_passes)
              node.concurrent_dispatches = IndependentComputeDispatches(plan, node, device_, compute, shape);
          } else if (record->type == EncoderType::Blit) {
            const auto &blit = *static_cast<const BlitEncoderData *>(record);
            node.access_complete = !fenced && blit.submission_access_complete &&
                blit.submission_buffers.size() == blit.submission_copy_count * 2;
            append_buffers(blit.submission_buffers, WMTMetal4StageBlit);
            for (const auto &use : blit.submission_uploads) {
              if (!node.access_complete) break;
              node.access_complete = AppendSubmissionBuffer(node, device_, use.address, use.length, use.writes, WMTMetal4StageBlit);
            }
            for (const auto &use : blit.submission_textures) {
              if (!node.access_complete) break;
              const auto aspects = DepthStencilPlanarFlags(use.texture->pixelFormat());
              node.access_complete = AppendSubmissionTexture(node, device_, use.texture.ptr(), use.texture->current(),
                  use.key, aspects ? uint8_t(aspects) : uint8_t(4), use.writes, false, WMTMetal4StageBlit);
            }
            if (node.sparse_resource_used) node.rejection = "sparse_alias";
          } else if (record->type == EncoderType::CaptureFrontier) {
            node.rejection = "capture_frontier";
            node.access_complete = false;
            node.helper_passes = true;
          } else if (record->type == EncoderType::Clear) {
            const auto &clear = *static_cast<const ClearEncoderData *>(record);
            // Layered and programmable-sample clears replay natively in place
            // (no batching), but their attachment writes are still tracked.
            node.access_complete = !fenced;
            if (node.access_complete && !clear.folded_into_render && clear.attachment) {
              const auto *view = clear.attachment.ptr();
              node.access_complete &= !view->castBlockSize && !view->msaaSampleAddress && !view->reservedCast &&
                  !view->reservedVolumeRender && !view->reservedVolumeAtlasCast && !clear.depth_plane &&
                  view->key.mip_end - view->key.mip_start == 1;
              if (node.access_complete) node.access_complete = AppendSubmissionTexture(node, device_, view->allocation->descriptor,
                  view->allocation, view->key, clear.clear_dsv ? uint8_t(clear.clear_dsv) : uint8_t(4),
                  true, true, WMTMetal4StageFragment, view->allocation->descriptor->sampleCount());
            }
          } else if (record->type == EncoderType::Temporal) {
            const auto &effect = *static_cast<const TemporalEncoderData *>(record);
            node.access_complete = !fenced;
            node.helper_passes = true; // Preserve the native feature's internal fence protocol.
            auto append = [&](const auto &resources, auto output) {
              for (size_t i=0;node.access_complete && i<resources.size();++i) {
                auto *resource=resources[i]; if (!resource) continue;
                if (!resource->texture || resource->swizzle_layout) {node.access_complete=false;break;}
                auto *texture=resource->texture.ptr();
                const auto planes=DepthStencilPlanarFlags(texture->pixelFormat());
                node.access_complete=AppendSubmissionTexture(node,device_,texture,texture->current(),
                    texture->fullView,planes?uint8_t(planes):uint8_t(4),output(i),false,WMTMetal4StageAll);
              }
            };
            const void *history = nullptr;
            if (effect.fg_frame) {
              append(GetOwnedFrameGenerationResources(effect.fg_frame),[&](size_t i){return IsOwnedFrameGenerationOutput(effect.fg_frame,i);});
              history=&OwnedFrameGenerationSubmissionMutex(effect.fg_frame);
            } else if (effect.rr_frame) {
              append(GetOwnedRayRegenerationResources(effect.rr_frame),[](size_t i){return IsOwnedRayRegenerationOutput(i);});
              history=&OwnedRayRegenerationSubmissionMutex(effect.rr_frame);
            } else {
              append(GetOwnedTemporalFrameResources(effect.frame),[](size_t i){return i==3;});
              history=&OwnedTemporalSubmissionMutex(effect.frame);
            }
            if(node.access_complete)node.access_complete=AppendSubmissionAccess(node,
                {history,0,1,0,1,0,1,0,true,false,WMTMetal4StageAll,{PhysicalDomain::History,reinterpret_cast<uintptr_t>(history)}});
          } else if (record->type == EncoderType::Query) {
            const auto &query = *static_cast<const QueryEncoderData *>(record);
            node.access_complete = !fenced && query.op != QueryEncoderOp::AliasFlush;
            if (node.access_complete) {
              node.access_complete = AppendSubmissionNativeBuffer(node, query.counters, true, WMTMetal4StageDispatch);
              node.access_complete &= AppendSubmissionNativeBuffer(node, query.source, true, WMTMetal4StageDispatch);
              if (query.destination_allocation)
                node.access_complete &= AppendSubmissionAllocation(node, query.destination_allocation.ptr(),
                    query.destination_offset, uint64_t(query.query_count) * std::max<uint32_t>(query.record_bytes, 8u), true,
                    WMTMetal4StageDispatch);
              else
                node.access_complete &= AppendSubmissionNativeBuffer(node, query.destination, true,
                    WMTMetal4StageDispatch);
            }
          }
          if (record->endpoint_reason == RecordedEndpointReason::Opaque) node.boundary_after = true;
          if (fenced || node.sparse_resource_used) node.access_complete = false;
          if (!node.access_complete) { node.accesses.clear(); node.access_slots.clear(); }
          if (!node.access_complete) owned_graphics::Trace("submission_access_opaque", ",\"submission\":", scope->seq,
              ",\"list_ordinal\":", ordinal, ",\"node_ordinal\":", node.node_ordinal, ",\"encoder\":", record->id,
              ",\"reason\":\"", node.rejection ? node.rejection : "unresolved_encoder_access", "\"");
          plan.nodes.push_back(std::move(node));
        }
        SparsePublicationProof publication;
        std::vector<OwnedHeapRange> heap_ranges;
        for(size_t index=list_begin;index<plan.nodes.size();++index) {
          const auto &node=plan.nodes[index];
          const bool publication_marker=node.record->type==EncoderType::Query &&
              static_cast<const QueryEncoderData *>(node.record)->op==QueryEncoderOp::AliasFlush;
          if ((!node.access_complete && !publication_marker) || node.sparse_resource_used) publication.required=true;
          for(const auto &access:node.accesses) if (access.Key().domain==PhysicalDomain::Heap) {
            if (access.begin >= access.end || access.end == UINT64_MAX) publication.required = true;
            heap_ranges.push_back({access.Key().owner, access.begin, access.end});
          }
        }
        publication.heap_ranges = heap_ranges.size();
        if (!publication.required && !heap_ranges.empty()) {
          publication.heap_proof = ReservedResource::ProveHeapRanges(heap_ranges);
          publication.required = !publication.heap_proof.disjoint;
        }
        plan.sparse_publication.push_back(publication);
        for (const auto &boundary : head->boundaries) {
          std::optional<SubmissionAccess> observation;
          if (boundary.resource_scoped && boundary.reason != RecordedBoundaryReason::Split &&
              boundary.reason != RecordedBoundaryReason::Alias) {
            if (boundary.buffer && boundary.offset <= boundary.buffer->byteLength()) {
              const auto available = boundary.buffer->byteLength() - boundary.offset;
              const auto bytes = boundary.size == UINT64_MAX ? available : boundary.size;
              if (bytes <= available) observation = BufferAccess(boundary.buffer.ptr(), boundary.offset, bytes, true, 0);
            } else if (boundary.texture) {
              const auto &range = boundary.subresources;
              uint32_t mip = 0, mips = boundary.mip_count, slice = 0, slices = boundary.array_count;
              uint8_t aspects = boundary.texture_aspects;
              if (!range.NumMipLevels && range.IndexOrFirstMipLevel != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                const auto subresource = range.IndexOrFirstMipLevel;
                mip = subresource % boundary.mip_count; mips = 1;
                slice = (subresource / boundary.mip_count) % boundary.array_count; slices = 1;
                if (aspects != 4) aspects = uint8_t(1u << (subresource / (boundary.mip_count * boundary.array_count)));
              } else if (range.NumMipLevels) {
                mip = range.IndexOrFirstMipLevel; mips = range.NumMipLevels;
                slice = range.FirstArraySlice; slices = range.NumArraySlices;
                if (aspects != 4) aspects = uint8_t(((1u << range.NumPlanes) - 1u) << range.FirstPlane);
              }
              observation = SubmissionAccess{boundary.texture.ptr(), 0, 0, mip, mip + mips, slice, slice + slices,
                  aspects, true, false, 0};
              observation->physical = {boundary.texture->canonicalHeap ? PhysicalDomain::Heap : PhysicalDomain::Texture,
                  uintptr_t(boundary.texture->canonicalHeap ? boundary.texture->canonicalHeap.handle
                                                           : boundary.texture->texture().handle)};
              if (boundary.texture->canonicalHeap)
                SetHeapAccessExtent(*observation, boundary.texture->canonicalHeapOffset,
                    boundary.texture->canonicalHeapSize);
            }
          }
          owned_graphics::Trace("submission_boundary", ",\"submission\":", scope->seq, ",\"list_ordinal\":", ordinal,
              ",\"encoder\":", boundary.encoder ? boundary.encoder->id : 0,
              ",\"reason\":", uint32_t(boundary.reason), ",\"state_event\":", boundary.state_event_index,
              ",\"resource_scoped\":", bool(observation), ",\"offset\":", boundary.offset, ",\"size\":", boundary.size,
              ",\"sync_before\":", uint64_t(boundary.sync_before), ",\"sync_after\":", uint64_t(boundary.sync_after),
              ",\"access_before\":", uint64_t(boundary.access_before), ",\"access_after\":", uint64_t(boundary.access_after));
          const auto anchor = head->pass_indices.find(boundary.encoder);
          if (anchor == head->pass_indices.end()) {
            if (list_begin < plan.nodes.size()) {
              if (observation) plan.nodes[list_begin].scopes_before.push_back(*observation);
              else plan.nodes[list_begin].boundary_before = true;
            } else if (list_begin) {
              if (observation) plan.nodes.back().scopes_after.push_back(*observation);
              else plan.nodes.back().boundary_after = true;
            }
            if (!observation && list_begin) plan.nodes[list_begin - 1].boundary_after = true;
            continue;
          }
          const size_t anchor_index = list_begin + anchor->second;
          auto &node = plan.nodes[anchor_index];
          const wmtcmd_base *tail = nullptr;
          if (node.record->type == EncoderType::Render) tail = static_cast<const RenderEncoderData *>(node.record)->cmd_tail;
          if (node.record->type == EncoderType::Compute) tail = static_cast<const ComputeEncoderData *>(node.record)->cmd_tail;
          if (node.record->type == EncoderType::Blit) tail = static_cast<const BlitEncoderData *>(node.record)->cmd_tail;
          if (observation) {
            if (boundary.command == tail) node.scopes_after.push_back(*observation);
            else node.scopes_before.push_back(*observation);
          } else node.boundary_after = true;
          if (boundary.command != tail && (!observation || std::any_of(node.accesses.begin(), node.accesses.end(),
              [&](const auto &access) { return AccessOverlaps(access, *observation); }))) {
            node.access_complete = false;
            if (node.record->type == EncoderType::Render) node.rejection = "interior_barrier";
          }
          if (!observation && anchor_index + 1 < plan.nodes.size()) plan.nodes[anchor_index + 1].boundary_before = true;
        }
        // A whole-surface AVERAGE ResolveSubresource recorded right after a
        // native MSAA pass that stores its source folds into that pass as a
        // StoreAndMultisampleResolve color store, removing the ResolvePass
        // that reloads the samples. The source keeps its store: a standalone
        // resolve cannot prove the samples are dead. Barriers between the
        // two must be resource-scoped to the source or destination, which
        // the attachment store order satisfies; the destination write joins
        // the pass's tracked accesses so in-flight hazards still order it.
        for (size_t index = list_begin; index + 1 < plan.nodes.size(); ++index) {
          auto &producer = plan.nodes[index];
          auto &consumer = plan.nodes[index + 1];
          if (producer.record->type != EncoderType::Render || consumer.record->type != EncoderType::Resolve ||
              !producer.access_complete || producer.boundary_after || consumer.boundary_before ||
              producer.folded_resolve) continue;
          const auto &render = *static_cast<const RenderEncoderData *>(producer.record);
          const auto &resolve = *static_cast<const ResolveEncoderData *>(consumer.record);
          if (render.native_msaa_sample_count < 2 || render.render_target_array_length > 1 || resolve.pipeline ||
              resolve.dsv_flags || !resolve.src || !resolve.dst) continue;
          const auto *source = resolve.src.ptr();
          const auto *target = resolve.dst.ptr();
          auto source_texture = resolve.src.texture();
          if (target->castBlockSize || target->msaaSampleAddress || target->reservedCast || target->reservedVolumeRender ||
              target->reservedVolumeAtlasCast || target->key.mip_end - target->key.mip_start != 1 ||
              target->key.array_end - target->key.array_start != 1 ||
              source_texture.width() != render.render_target_width || source_texture.height() != render.render_target_height ||
              resolve.dst.texture().pixelFormat() != source_texture.pixelFormat()) continue;
          unsigned slot = 8;
          for (unsigned i = 0; i < render.render_target_count && slot == 8; ++i) {
            const auto &color = render.colors[i];
            if (color.attachment && !color.buffer_attachment && !color.resolve_attachment &&
                color.store_action == WMTStoreActionStore && !color.level && !color.slice && !color.depth_plane &&
                color.attachment->allocation == source->allocation && color.attachment->key.mip_start == source->key.mip_start &&
                color.attachment->key.mip_end == source->key.mip_end &&
                color.attachment->key.array_start == source->key.array_start &&
                color.attachment->key.array_end == source->key.array_end &&
                color.attachment.texture().pixelFormat() == source_texture.pixelFormat())
              slot = i;
          }
          if (slot == 8) continue;
          auto written = TextureAccess(target->allocation,target->key,4,true,true,WMTMetal4StageFragment);
          const auto source_identity = TextureAccess(source->allocation,source->key,4,false,true,0).Key();
          bool scoped = true;
          for (const auto *observations : {&producer.scopes_after, &consumer.scopes_before})
            for (const auto &observation : *observations)
              scoped &= observation.Key() == source_identity || observation.Key() == written.Key();
          // The pass must not read or write the destination itself.
          for (const auto &access : producer.accesses) scoped &= !AccessOverlaps(access, written);
          if (!scoped) continue;
          const auto rejection = producer.rejection;
          const bool sparse = producer.sparse_resource_used;
          if (!AppendSubmissionTexture(producer, device_, target->allocation->descriptor, target->allocation, target->key,
                                       4, true, true, WMTMetal4StageFragment)) {
            producer.rejection = rejection;
            producer.sparse_resource_used = sparse;
            continue;
          }
          producer.folded_resolve = SubmissionNode::FoldedResolve{slot, resolve.dst};
          // The folded record encodes nothing; its accesses moved into the pass.
          consumer.access_complete = true;
          consumer.resolve_folded_into_previous = true;
          consumer.scopes_before.clear();
          owned_graphics::Trace("resolve_folded_standalone", ",\"submission\":", scope->seq, ",\"encoder\":", render.id,
                                ",\"resolve\":", resolve.id, ",\"slot\":", slot);
        }
      }
      std::list<std::pair<size_t, uint8_t>> pending_clears;
      using PendingIterator = decltype(pending_clears)::iterator;
      struct PendingKey {
        const void *allocation;
        uint32_t mip, slice;
        bool operator==(const PendingKey &) const = default;
      };
      struct PendingKeyHash {
        size_t operator()(const PendingKey &key) const {
          return std::hash<const void *>{}(key.allocation) ^ (size_t(key.mip) << 16) ^ size_t(key.slice);
        }
      };
      // Full single-subresource clears replace earlier unobserved writes across
      // format views. Keep the latest view and typed value; never absorb an
      // older linear load clear after a newer sRGB clear of the same aspect.
      std::unordered_map<PendingKey, std::array<PendingIterator, 3>, PendingKeyHash> pending_slots;
      auto erase_pending = [&](PendingIterator it) {
        const auto &clear = *static_cast<const ClearEncoderData *>(plan.nodes[it->first].record);
        auto slot = pending_slots.find({clear.attachment->allocation, clear.attachment->key.mip_start, clear.attachment->key.array_start});
        slot->second[it->second == 1 ? 0 : it->second == 2 ? 1 : 2] = pending_clears.end();
        if (std::all_of(slot->second.begin(), slot->second.end(), [&](const auto &old) { return old == pending_clears.end(); }))
          pending_slots.erase(slot);
        return pending_clears.erase(it);
      };
      auto flush_clears = [&](std::vector<size_t> &destination, const std::vector<SubmissionAccess> *observations = nullptr) {
        auto selected = [&](const auto &pending) {
          if (!observations) return true;
          for (auto access : plan.nodes[pending.first].accesses) {
            access.aspects = pending.second;
            for (const auto &observation : *observations) if (AccessOverlaps(access,observation)) return true;
          }
          return false;
        };
        while (!pending_clears.empty()) {
          const auto chosen = std::find_if(pending_clears.begin(),pending_clears.end(),selected);
          if (chosen == pending_clears.end()) break;
          SubmissionClearBatch batch;
          WMT::InitializeRenderPassInfo(batch.descriptor);
          const auto &first = *static_cast<const ClearEncoderData *>(plan.nodes[chosen->first].record);
          batch.descriptor.render_target_width = first.width; batch.descriptor.render_target_height = first.height;
          WMT::SetRenderTargetArrayLength(batch.descriptor, first.array_length);
          unsigned colors = 0;
          for (auto it = pending_clears.begin(); it != pending_clears.end();) {
            if (!selected(*it)) {++it;continue;}
            const auto &data = *static_cast<const ClearEncoderData *>(plan.nodes[it->first].record);
            const auto aspect = it->second;
            bool compatible = data.width == first.width && data.height == first.height && data.array_length == first.array_length &&
                data.attachment->allocation->descriptor->sampleCount() == first.attachment->allocation->descriptor->sampleCount();
            if (aspect == 4) compatible &= colors < 8;
            if (aspect == 1) compatible &= !batch.descriptor.depth.texture;
            if (aspect == 2) compatible &= !batch.descriptor.stencil.texture;
            // Two distinct views of one allocation remain separate native
            // passes until attachment alias legality has an independent proof.
            for (const auto &[previous, previous_aspect] : batch.members) {
              const auto &other = *static_cast<const ClearEncoderData *>(plan.nodes[previous].record);
              if (other.attachment->allocation == data.attachment->allocation &&
                  (other.attachment.ptr() != data.attachment.ptr() || previous_aspect == aspect)) compatible = false;
            }
            if (!compatible) { ++it; continue; }
            if (aspect == 4) {
              auto &color = batch.descriptor.colors[colors++];
              color.texture = data.attachment.texture(); color.depth_plane = data.depth_plane;
              color.load_action = WMTLoadActionClear; color.store_action = WMTStoreActionStore; color.clear_color = data.color;
            } else if (aspect == 1) {
              auto &depth = batch.descriptor.depth;
              depth.texture = data.attachment.texture(); depth.depth_plane = data.depth_plane;
              depth.load_action = WMTLoadActionClear; depth.store_action = WMTStoreActionStore; depth.clear_depth = data.depth_stencil.first;
            } else {
              auto &stencil = batch.descriptor.stencil;
              stencil.texture = data.attachment.texture(); stencil.depth_plane = data.depth_plane;
              stencil.load_action = WMTLoadActionClear; stencil.store_action = WMTStoreActionStore;
              stencil.clear_stencil = data.depth_stencil.second;
            }
            batch.members.push_back(*it); it = erase_pending(it);
          }
          destination.push_back(plan.clear_batches.size()); plan.clear_batches.push_back(std::move(batch));
        }
      };
      for (size_t index = 0; index < plan.nodes.size(); ++index) {
        auto &node = plan.nodes[index];
        // ponytail: 64-source-node planning partitions bound frontier scans.
        // Establish them before clear promotion so no replay group or delayed
        // clear can cross discarded history; original native fences remain.
        if (index && index % kSubmissionGroupBudget == 0 && !node.boundary_before)
          node.boundary_before = node.partition_before = true;
        if (node.boundary_before || !node.access_complete) flush_clears(node.clears_before);
        else if (!node.scopes_before.empty()) flush_clears(node.clears_before,&node.scopes_before);
        if (node.record->type == EncoderType::Clear && node.access_complete) {
          const auto &clear = *static_cast<const ClearEncoderData *>(node.record);
          // Zero is the recording/descriptor sentinel for a nonlayered view.
          const bool batchable = !clear.sample_positions.count && clear.array_length <= 1 && clear.attachment &&
              !clear.attachment->allocation->canonicalHeap &&
              clear.attachment->key.array_end - clear.attachment->key.array_start == 1;
          // Tracking a physical heap alias does not make aliased attachments
          // legal in the same native render pass. Keep these clears in place.
          if (clear.attachment && clear.attachment->allocation->canonicalHeap)
            owned_graphics::Trace("submission_heap_clear_in_place", ",\"submission\":", scope->seq,
                ",\"encoder\":", clear.id);
          if (!batchable) flush_clears(node.clears_before);
          if (batchable && !clear.folded_into_render) {
            node.skip_clear = true;
            // Bound compatible-batch exploration; larger windows materialize
            // conservatively rather than retaining an unbounded clear scan.
            const uint8_t aspects = clear.clear_dsv ? uint8_t(clear.clear_dsv) : uint8_t(4);
            if (pending_clears.size() + std::popcount(unsigned(aspects)) > kSubmissionGroupBudget)
              flush_clears(node.clears_before);
            for (uint8_t aspect : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
              if (!(aspects & aspect)) continue;
              const unsigned aspect_slot = aspect == 1 ? 0 : aspect == 2 ? 1 : 2;
              const PendingKey key{clear.attachment->allocation, clear.attachment->key.mip_start, clear.attachment->key.array_start};
              auto slots = pending_slots.find(key);
              auto prior = slots == pending_slots.end() ? pending_clears.end() : slots->second[aspect_slot];
              if (prior != pending_clears.end()) {
                owned_graphics::Trace("submission_clear_superseded", ",\"submission\":", scope->seq,
                    ",\"before\":", prior->first, ",\"after\":", index, ",\"aspect\":", uint32_t(aspect));
                erase_pending(prior);
              }
              pending_clears.emplace_back(index, aspect);
              std::array<PendingIterator, 3> empty_slots; empty_slots.fill(pending_clears.end());
              auto [stored, inserted] = pending_slots.try_emplace(key, empty_slots);
              stored->second[aspect_slot] = std::prev(pending_clears.end());
            }
          }
        } else if (!pending_clears.empty()) {
          if (node.record->type == EncoderType::Render && !node.rejection && node.access_complete &&
              !static_cast<const RenderEncoderData *>(node.record)->native_msaa_sample_count) {
            const auto &render = *static_cast<const RenderEncoderData *>(node.record);
            RenderReplayGroup trial;
            std::array<uint8_t, 8> first_map;
            if (!AppendRenderMember(trial, render, true, first_map)) {
              trial.color_maps.push_back(first_map);
              bool absorbed = false;
              for (auto it = pending_clears.begin(); it != pending_clears.end();) {
                const auto &clear = *static_cast<const ClearEncoderData *>(plan.nodes[it->first].record);
                bool match = clear.width == render.render_target_width && clear.height == render.render_target_height &&
                    clear.array_length == render.render_target_array_length;
                unsigned physical = 0;
                if (it->second == 4) {
                  while (physical < trial.color_count && trial.colors[physical].ptr() != clear.attachment.ptr()) ++physical;
                  match &= physical < trial.color_count && trial.descriptor.colors[physical].load_action == WMTLoadActionLoad;
                  if (match) {
                    trial.descriptor.colors[physical].load_action = WMTLoadActionClear;
                    trial.descriptor.colors[physical].clear_color = clear.color;
                  }
                } else if (it->second == 1) {
                  match &= trial.depth.ptr() == clear.attachment.ptr() && trial.descriptor.depth.load_action == WMTLoadActionLoad;
                  if (match) { trial.descriptor.depth.load_action = WMTLoadActionClear; trial.descriptor.depth.clear_depth = clear.depth_stencil.first; }
                } else {
                  match &= trial.stencil.ptr() == clear.attachment.ptr() && trial.descriptor.stencil.load_action == WMTLoadActionLoad;
                  if (match) { trial.descriptor.stencil.load_action = WMTLoadActionClear; trial.descriptor.stencil.clear_stencil = clear.depth_stencil.second; }
                }
                if (!match) { ++it; continue; }
                owned_graphics::Trace("submission_clear_load", ",\"submission\":", scope->seq,
                    ",\"clear_node\":", it->first, ",\"render_node\":", index, ",\"aspect\":", uint32_t(it->second));
                absorbed = true; it = erase_pending(it);
              }
              if (absorbed) {
                trial.members.push_back(index);
                trial.force_fence_before = node.force_fence_before;
                for (const auto &access : node.accesses) trial.stages |= access.stages;
                node.group = plan.groups.size(); plan.groups.push_back(std::move(trial));
              }
            }
          }
          const size_t before = pending_clears.size();
          flush_clears(node.clears_before,&node.accesses);
          if(before != pending_clears.size() && !pending_clears.empty())
            owned_graphics::Trace("submission_clear_selective", ",\"submission\":", scope->seq,
                ",\"flushed\":", before-pending_clears.size(), ",\"still_deferred\":", pending_clears.size());
        }
        if ((node.boundary_after || !node.scopes_after.empty()) && !pending_clears.empty()) {
          auto &destination = index + 1 < plan.nodes.size() ? plan.nodes[index + 1].clears_before : plan.tail_clears;
          flush_clears(destination,node.boundary_after ? nullptr : &node.scopes_after);
        }
      }
      flush_clears(plan.tail_clears);
      // Resource frontiers retain every required reader until a covering write.
      // Reads never inspect readers; covered older writes/readers are retired.
      struct History { size_t node; SubmissionAccess access; };
      OwnedAccessFrontier<History> history;
      size_t partition = 0;
      for (size_t index = 0; index < plan.nodes.size(); ++index) {
        auto &node = plan.nodes[index];
        if (index && (!plan.nodes[index - 1].access_complete || plan.nodes[index - 1].boundary_after))
          node.boundary_before = true, node.partition_before = false;
        if (node.boundary_before || !node.access_complete) { history.Clear(); partition = index; }
        std::unordered_map<size_t, size_t> edge_slots;
        for (const auto &access : node.accesses) {
          history.Visit(access,[&](const History &prior) {
              ++plan.history_checks;
              if (prior.node == index || !AccessOverlaps(access, prior.access)) return;
              const auto [slot, inserted] = edge_slots.try_emplace(prior.node, node.dependencies.size());
              const bool heap_alias = access.Key().domain == PhysicalDomain::Heap;
              const bool attachment_only = AttachmentDependencyOrdered(access, prior.access);
              const uint8_t scope_mask = heap_alias ? WMTBarrierScopeTextures | WMTBarrierScopeBuffers :
                  access.aspects ? WMTBarrierScopeTextures : WMTBarrierScopeBuffers;
              if (inserted) node.dependencies.push_back({prior.node, attachment_only ? 0 : prior.access.stages,
                  attachment_only ? 0 : access.stages, attachment_only, uint8_t(attachment_only ? 0 : scope_mask)});
              else {
                auto &edge = node.dependencies[slot->second];
                if (!attachment_only) {
                  edge.before |= prior.access.stages; edge.after |= access.stages; edge.scope |= scope_mask;
                }
                edge.attachment_only &= attachment_only;
              }
              owned_graphics::Trace("submission_access_edge", ",\"submission\":", scope->seq,
                  ",\"before\":", prior.node, ",\"after\":", index, ",\"raw\":", prior.access.writes && !access.writes,
                  ",\"war\":", !prior.access.writes && access.writes, ",\"waw\":", prior.access.writes && access.writes,
                  ",\"stages_before\":", prior.access.stages, ",\"stages_after\":", access.stages);
          });
          // Different stage/aspect accesses in the same encoder are not ordered
          // by an external dependency. Keep them all until a later node covers them.
          history.PublishPreserving(access,History{index,access},[&](const History& prior){return prior.node==index;});
        }
        if (node.record->type == EncoderType::Render && node.merge_eligible &&
            !node.boundary_before && node.clears_before.empty() && node.group == SIZE_MAX) {
          size_t attempts = 0;
          // ponytail: bounded candidates/source span and 64-member groups; a
          // range-indexed group summary can replace this if larger groups pay.
          const size_t lower = std::max(partition, index > kSubmissionGroupBudget ? index - kSubmissionGroupBudget : 0);
          for (size_t candidate = index; candidate-- > lower;) {
            auto &prior = plan.nodes[candidate];
            if (prior.boundary_after || !prior.access_complete) break;
            if (prior.record->type != EncoderType::Render || !prior.merge_eligible) continue;
            if (attempts++ == kSubmissionCandidateBudget) break;
            ++plan.candidate_checks;
            const auto *existing = prior.group == SIZE_MAX ? nullptr : &plan.groups[prior.group];
            const size_t leader = existing ? existing->members.front() : candidate;
            if (leader < partition) continue;
            // A later list may require sparse publication (or invalidate its
            // negative proof) at replay. Never move a placed attachment before
            // that list's original publication point; non-heap groups retain
            // their existing cross-list family.
            if ((node.placed_attachments || prior.placed_attachments || (existing && existing->placed_attachments)) &&
                plan.nodes[leader].list_ordinal != node.list_ordinal) {
              owned_graphics::Trace("submission_render_rejected", ",\"submission\":", scope->seq,
                  ",\"before_list\":", prior.list_ordinal, ",\"after_list\":", node.list_ordinal,
                  ",\"reason\":\"placed_cross_list\"");
              continue;
            }
            if (index - leader > kSubmissionGroupBudget || (existing && existing->members.size() == kSubmissionGroupBudget)) continue;
            bool commute = true;
            for (const auto &dependency : node.dependencies) {
              const bool internal = dependency.source == leader ||
                  (prior.group != SIZE_MAX && plan.nodes[dependency.source].group == prior.group);
              if (dependency.source > leader && !internal) commute = false;
              // TBDR cannot complete Fragment inside the same render pass.
              // The proven ordinary inline family is Vertex -> Vertex only;
              // other nonattachment dependencies retain store/load + fences.
              if (internal && !InlineRenderDependencySafe(dependency)) commute = false;
            }
            for (size_t crossed = leader + 1; commute && crossed < index; ++crossed)
              if (plan.nodes[crossed].boundary_before || plan.nodes[crossed].boundary_after ||
                  !plan.nodes[crossed].access_complete || !plan.nodes[crossed].clears_before.empty() ||
                  plan.nodes[crossed].record->type == EncoderType::Clear) commute = false;
            bool scope_blocked = false;
            for (size_t crossed = leader; commute && crossed <= index; ++crossed) {
              auto conflicts = [&](const auto &observations) {
                for (const auto &observation : observations)
                  for (const auto &access : node.accesses) {
                    ++plan.scope_checks;
                    if (AccessOverlaps(observation, access)) scope_blocked = true;
                  }
              };
              if (crossed != leader) conflicts(plan.nodes[crossed].scopes_before);
              if (crossed != index) conflicts(plan.nodes[crossed].scopes_after);
            }
            if (scope_blocked) {
              commute = false;
              owned_graphics::Trace("submission_render_rejected", ",\"submission\":", scope->seq,
                  ",\"before_list\":", prior.list_ordinal, ",\"after_list\":", node.list_ordinal,
                  ",\"reason\":\"resource_boundary\"");
            }
            if (!commute) continue;
            RenderReplayGroup trial;
            std::array<uint8_t, 8> first_map, map;
            if (existing) {
              trial.descriptor = existing->descriptor; trial.colors = existing->colors;
              trial.depth = existing->depth; trial.stencil = existing->stencil;
              trial.color_count = existing->color_count; trial.needs_unbound_slot = existing->needs_unbound_slot;
              trial.placed_attachments = existing->placed_attachments;
              trial.stages = existing->stages; ++plan.metadata_copies;
            } else if (AppendRenderMember(trial, *static_cast<const RenderEncoderData *>(prior.record), true, first_map)) continue;
            const char *reason = AppendRenderMember(trial, *static_cast<const RenderEncoderData *>(node.record), false, map);
            auto feedback = [&](size_t member) {
              for (const auto &a : plan.nodes[member].accesses)
                for (const auto &b : node.accesses) {
                  ++plan.feedback_checks;
                  if (a.attachment != b.attachment && AccessOverlaps(a, b)) reason = "cross_member_attachment_feedback";
                }
            };
            if (existing) for (const auto member : existing->members) feedback(member);
            else feedback(candidate);
            if (reason) {
              owned_graphics::Trace("submission_render_rejected", ",\"submission\":", scope->seq,
                  ",\"before_list\":", prior.list_ordinal, ",\"after_list\":", node.list_ordinal,
                  ",\"reason\":\"", reason, "\""); continue;
            }
            for (const auto &access : node.accesses) trial.stages |= access.stages;
            for (const auto &access : prior.accesses) trial.stages |= access.stages;
            if (!existing) {
              trial.members.push_back(candidate); trial.color_maps.push_back(first_map);
              trial.force_fence_before = plan.nodes[candidate].force_fence_before;
              prior.group = plan.groups.size(); plan.groups.push_back(std::move(trial));
            } else {
              auto &group = plan.groups[prior.group];
              group.descriptor = trial.descriptor; group.colors = std::move(trial.colors);
              group.depth = std::move(trial.depth); group.stencil = std::move(trial.stencil);
              group.color_count = trial.color_count; group.needs_unbound_slot = trial.needs_unbound_slot;
              group.placed_attachments = trial.placed_attachments;
              group.stages = trial.stages;
              group.force_fence_before |= trial.force_fence_before;
            }
            auto &group = plan.groups[prior.group];
            group.members.push_back(index); group.color_maps.push_back(map);
            group.force_fence_before |= node.force_fence_before;
            node.group = prior.group;
            owned_graphics::Trace("submission_render_merged", ",\"submission\":", scope->seq, ",\"group\":", node.group,
                ",\"before_list\":", prior.list_ordinal, ",\"after_list\":", node.list_ordinal,
                ",\"members\":", group.members.size(), ",\"nonconsecutive\":", candidate + 1 != index);
            break;
          }
        }
        if (!node.access_complete || node.boundary_after) { history.Clear(); partition = index + 1; }
      }
      owned_graphics::Trace("submission_plan_work", ",\"submission\":", scope->seq,
          ",\"history_checks\":", plan.history_checks, ",\"candidate_checks\":", plan.candidate_checks,
          ",\"metadata_copies\":", plan.metadata_copies, ",\"scope_checks\":", plan.scope_checks,
          ",\"feedback_checks\":", plan.feedback_checks,
          ",\"descriptor_resolutions\":", plan.descriptor_resolutions,
          ",\"descriptor_cache_hits\":", plan.descriptor_cache_hits,
          ",\"descriptor_cache_misses\":", plan.descriptor_cache_misses,
          ",\"descriptor_cache_builds\":", plan.descriptor_cache_builds);
      // All native GPU memory work is still unencoded here. Only begin(),
      // residency declarations and CPU planning preceded this guard. A
      // preflight mismatch therefore drops its narrow ticket and reacquires
      // the original exclusion before any work can reach another queue.
      GPUExecutionGate::Accesses completed_accesses;
      try {
        auto proof = std::make_shared<PhysicalAdmissionProof>();
        bool complete = !fenced;
        // Cached descriptor summaries retain their pins in the immutable cache,
        // not in each node. Index those exact owners once; never infer ownership
        // from a native handle or rescan all caches for every access. The plan
        // retains each cache until the selected owners have entered the proof.
        AdmissionBufferOwners cached_buffers;
        size_t owner_checks = 0;
        for (const auto &cache : plan.descriptor_caches) {
          if (!complete || ++owner_checks > 4096) { complete = false; break; }
          for (const auto &buffer : cache->buffers) {
            if (++owner_checks > 4096) { complete = false; break; }
            cached_buffers.try_emplace(buffer.allocation.ptr(), &buffer.allocation);
            if (cached_buffers.size() > 256) { complete = false; break; }
          }
        }
        for (const auto &node : plan.nodes) {
          if (!complete) break;
          complete = node.access_complete && !node.helper_passes && !node.sparse_resource_used &&
              (node.record->type == EncoderType::Render || node.record->type == EncoderType::Compute ||
               node.record->type == EncoderType::Blit || node.record->type == EncoderType::Clear);
          if (!complete) break;
          for (const auto &access : node.accesses)
            if (!AppendAdmissionAccess(*proof, access, node.allocation_pins, false, &cached_buffers)) {
              complete = false; break;
            }
        }
        if (complete) {
          proof->resources = plan.descriptor_owners;
          completed_accesses = ShareAdmissionProof(std::move(proof));
        }
      } catch (const std::bad_alloc &) {
        // This optional summary changes exclusion only, never GPU results.
      }
      if (ordinary_accesses && atomic_envelope) {
        // Re-resolve the actual inputs after obtaining the prologue ticket.
        // Keep the replay planner's Atomic node opaque for fence/merge purposes.
        try { completed_accesses = envelope_proof(); }
        catch (const std::bad_alloc &) { completed_accesses.reset(); }
      }
      if (ordinary_accesses && (!completed_accesses ||
          !std::all_of(completed_accesses->begin(), completed_accesses->end(),
              [&](const auto &access) { return AdmissionCovers(ordinary_accesses, access); }))) {
        scope->inflight.execution_ticket.Reset();
        ordinary_accesses.reset();
        scope->inflight.execution_ticket = device_->gpu_execution_gate.AcquireShared();
        if (!scope->inflight.execution_ticket) return;
        owned_graphics::Trace("atomic64_admission_fallback", ",\"queue\":", owned_id_,
            ",\"submission\":", scope->seq, ",\"encoded_work_before_reacquire\":0");
      }
      if (completed_accesses) {
        ordinary_accesses = std::move(completed_accesses);
        scope->inflight.execution_ticket.NarrowShared(ordinary_accesses);
        owned_graphics::Trace("atomic64_ordinary_proof", ",\"queue\":", owned_id_,
            ",\"submission\":", scope->seq, ",\"ranges\":", ordinary_accesses->size(), ",\"retirement_owned\":1");
      }
      resolved_resources->descriptor_owners = std::move(plan.descriptor_owners);
      resolved_resources->descriptor_caches = std::move(plan.descriptor_caches);
      for(auto &node:plan.nodes)
        for(auto &owner:node.allocation_pins)resolved_resources->buffer_owners.push_back(std::move(owner));
    } catch (const std::bad_alloc &) {
      ERR("ExecuteCommandLists: out of memory planning immutable render groups");
      scope.reset();
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return;
    }
    // Check after any Atomic64 admission fallback/reacquisition. Same FIFO
    // order is sufficient within an uninterrupted queue era; no GPU/CPU
    // wait is added. Embedded fence operations can hand work to another
    // queue mid-list, so such an Execute never consumes or publishes a proof.
    if (!fenced && scope->seq == first_segment) LoadSwizzleCarry(scope->inflight);
    // A batch whose member clears are all tracked records their attachment
    // writes as in-flight accesses. It waits only for conflicting tracked
    // stages (or nothing), and consumers see its writes through the same
    // in-flight check. An untracked neighbor already reset tracking.
    auto emit_clear_batches = [&](const std::vector<size_t> &batches) {
      for (const auto batch_index : batches) {
        auto &batch = plan.clear_batches[batch_index];
        std::vector<SubmissionAccess> accesses;
        bool tracked = true;
        for (const auto &[member, aspect] : batch.members) {
          const auto &clear_node = plan.nodes[member];
          tracked &= clear_node.access_complete;
          for (auto access : clear_node.accesses) { access.aspects &= aspect; if (access.aspects) accesses.push_back(access); }
        }
        uint64_t hazard = WMTMetal4StageAll;
        if (tracked) {
          hazard = InflightHazard(accesses);
          RecordInflightAccesses(accesses, 0, ProducerStages(EncoderType::Clear));
        } else {
          MarkUntracked(UINT64_MAX);
        }
        if (hazard == WMTMetal4StageAll) DrainSkippedWork(scope->inflight.cmdbuf, scope->seq);
        else BreakFenceChain();
        auto encoder = scope->inflight.cmdbuf.renderCommandEncoder(batch.descriptor);
        owned_graphics::Trace("native_clear_begin", ",\"submission\":", scope->seq, ",\"batch\":", batch_index,
            ",\"members\":", batch.members.size(), ",\"begin_count\":1");
        encoder.setLabel(clear_batch_label_);
        if (hazard == WMTMetal4StageAll) {
          encoder.waitForFence(fence_, WMTRenderStageFragment);
          owned_graphics::Trace("native_clear_wait", ",\"submission\":", scope->seq, ",\"batch\":", batch_index,
              ",\"stages\":", uint32_t(WMTRenderStageFragment), ",\"wait_count\":1");
        } else if (hazard) {
          const bool heap_alias=std::any_of(accesses.begin(),accesses.end(),[](const auto& access){return access.Key().domain==PhysicalDomain::Heap;});
          encoder.barrierAfterQueueStages(hazard, uint64_t(WMTRenderStageFragment),
              WMTMetal4VisibilityDevice | (heap_alias ? WMTMetal4VisibilityResourceAlias : 0));
          owned_graphics::Trace("native_clear_queue_barrier", ",\"submission\":", scope->seq, ",\"batch\":", batch_index,
              ",\"after\":", hazard, ",\"barrier_count\":1");
        } else {
          owned_graphics::Trace("native_fence_wait_skipped", ",\"submission\":", scope->seq, ",\"batch\":", batch_index,
              ",\"reason\":\"no_known_hazard\",\"skip_count\":1");
        }
        for (const auto &[node, aspect] : batch.members)
          owned_graphics::Trace("native_clear_load", ",\"submission\":", scope->seq, ",\"batch\":", batch_index,
              ",\"node\":", node, ",\"aspect\":", uint32_t(aspect), ",\"clear_count\":1");
        encoder.updateFence(fence_, WMTRenderStageFragment);
        owned_graphics::Trace("native_clear_update", ",\"submission\":", scope->seq, ",\"batch\":", batch_index,
            ",\"stages\":", uint32_t(WMTRenderStageFragment), ",\"update_count\":1");
        encoder.endEncoding();
        owned_graphics::Trace("native_clear_end", ",\"submission\":", scope->seq, ",\"batch\":", batch_index, ",\"end_count\":1");
      }
    };
    size_t replay_node = 0;
    size_t replay_list = 0;
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
      const auto &publication = plan.sparse_publication[replay_list++];
      // A mapping update may have run after planning on another queue. Never
      // reuse a negative proof across epochs or while native remapping is live.
      const bool proof_current = !publication.required && publication.heap_ranges &&
          ReservedResource::HeapProofCurrent(publication.heap_proof);
      bool sparse_publication = publication.required || (publication.heap_ranges && !proof_current);
      if (publication.heap_ranges)
        owned_graphics::Trace("sparse_publication_proof", ",\"queue\":", owned_id_, ",\"list\":", list.list_id,
            ",\"mapping_generation\":", publication.heap_proof.generation,
            ",\"heap_ranges\":", publication.heap_ranges, ",\"required\":", sparse_publication,
            ",\"current\":", proof_current);
      if (ReservedResource::AnyLive() && sparse_publication) {
        /* Command lists see each other's writes; for sparse tiles sharing a heap page that needs a
           shared event signal/wait, not only the pass boundary (ReservedResource::AnyLive). */
        owned_graphics::Trace("sparse_publication_required", ",\"queue\":", owned_id_, ",\"list\":", list.list_id,
            ",\"mapping_generation\":", publication.heap_proof.generation, ",\"heap_ranges\":", publication.heap_ranges,
            ",\"reason\":\"", publication.required ? "mapped_heap_or_opaque_access" : "stale_heap_proof", "\"");
        scope.reset();
        EncodeAliasFlush();
        reserved_volume_publish_pending_ = false;
        if (!start_segment())
          return;
      } else if (ReservedResource::AnyLive()) {
        owned_graphics::Trace("sparse_publication_elided", ",\"queue\":", owned_id_,
            ",\"list\":", list.list_id, ",\"proof\":\"",
            publication.heap_ranges ? "disjoint_sparse_heap_ranges" : "no_sparse_heap_or_opaque_access", "\"",
            ",\"mapping_generation\":", publication.heap_proof.generation, ",\"heap_ranges\":", publication.heap_ranges);
      }
      while (current) {
        if (device_->RemovedReason() != S_OK)
          return;
        PrepareSwizzleValidity(scope->inflight, current, scope->seq);
        const RenderReplayGroup *replay_group = nullptr;
        const SubmissionNode *replay_current_node = nullptr;
        bool native_fence_required = true;
        uint64_t consumer_barrier_after = 0;
        uint64_t consumer_visibility = WMTMetal4VisibilityDevice;
        if (current->type != EncoderType::Null) {
          const auto index = replay_node++;
          const auto &node = plan.nodes[index];
          for(const auto &access:node.accesses)if(access.Key().domain==PhysicalDomain::Heap)
            consumer_visibility |= WMTMetal4VisibilityResourceAlias;
          assert(node.record == current);
          replay_current_node = &node;
          if (node.resolve_folded_into_previous) {
            // Encoded by the preceding pass's resolve store.
            emit_clear_batches(node.clears_before);
            current = current->next;
            continue;
          }
          if (node.group != SIZE_MAX) replay_group = &plan.groups[node.group];
          if (replay_group && replay_group->placed_attachments)
            consumer_visibility |= WMTMetal4VisibilityResourceAlias;
          // Skip the queue fence only when no tracked in-flight access (this
          // or an earlier submission) overlaps and no untracked work may still
          // run. Grouped members replay inside the leader's encoder. A boundary
          // (alias/global barrier, opaque neighbor, DISCARD, partition) orders
          // memory allocation identity cannot describe, so it resets tracking.
          // A planner partition orders nothing: earlier nodes stay recorded as
          // in-flight accesses, and the hazard check covers them instead.
          const bool ordering_boundary = node.boundary_before && !node.partition_before;
          if (!node.access_complete || ordering_boundary) MarkUntracked(UINT64_MAX);
          // Deferred clears encode first and record their writes in flight,
          // so this node's hazard check below orders it after them.
          emit_clear_batches(node.clears_before);
          uint64_t inflight_stages = WMTMetal4StageAll;
          if (node.access_complete && (node.group == SIZE_MAX || replay_group->members.front() == index)) {
            // Earlier nodes of this submission are recorded in flight too, so
            // a same-submission dependency is one of these hazards. A
            // resource-scoped barrier orders the same way as a write to its
            // resource; it is checked but never recorded as GPU work.
            const auto check = [&](std::span<const SubmissionAccess> accesses) { inflight_stages |= InflightHazard(accesses); };
            inflight_stages = 0;
            if (replay_group) {
              for (const auto member : replay_group->members) check(plan.nodes[member].accesses);
            } else {
              check(node.accesses);
            }
            check(node.scopes_before);
            const auto record = [&](const SubmissionNode &member) {
              RecordInflightAccesses(member.accesses, HelperStages(member), ProducerStages(member.record->type));
            };
            if (replay_group) {
              for (const auto member : replay_group->members) record(plan.nodes[member]);
            } else {
              record(node);
            }
          }
          native_fence_required = !node.access_complete || inflight_stages == WMTMetal4StageAll || ordering_boundary ||
              node.helper_passes ||
              node.force_fence_before || (replay_group && replay_group->force_fence_before);
          // Only render, blit, compute and in-place clear replay honor a
          // skipped wait; every other emitted encoder waits on the fence
          // itself and needs the drain.
          if (!node.skip_clear && node.record->type != EncoderType::Render && node.record->type != EncoderType::Clear &&
              node.record->type != EncoderType::Blit && node.record->type != EncoderType::Compute)
            native_fence_required = true;
          // A conflict only with tracked in-flight work of known producer
          // stages waits for those stages through a consumer queue barrier
          // instead of the queue-wide fence chain (D3DMetal ResolveBarriers).
          if (!native_fence_required) consumer_barrier_after = inflight_stages;
          if (native_fence_required) DrainSkippedWork(scope->inflight.cmdbuf, scope->seq);
          else BreakFenceChain();
          if (!native_fence_required)
            owned_graphics::Trace("native_fence_wait_skipped", ",\"submission\":", scope->seq,
                ",\"encoder\":", current->id, ",\"reason\":\"", consumer_barrier_after ? "queue_barrier" : "no_known_hazard",
                "\",\"skip_count\":1");
          if (node.skip_clear) { current = current->next; continue; }
          if (node.group != SIZE_MAX) {
            if (replay_group->members.front() != index) {
              current = current->next;
              continue;
            }
          }
        }
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
            [&publication](std::span<const StandardSwizzleTableUse> global,
                           std::span<const StandardSwizzleTableUse> local) {
              std::vector<SwizzlePassResource> uses;
              publication.owner->AppendStandardSwizzleTables(global, uses);
              publication.owner->AppendStandardSwizzleTables(local, uses);
              return !uses.empty();
            },
            [&publication](WMT::MTL4CommandBuffer buffer, WMT::Fence graph_fence,
                           std::span<const StandardSwizzleTableUse> global,
                           std::span<const StandardSwizzleTableUse> local) -> HRESULT {
              // GPU traversal yields before a publication batch, and the host
              // retires that batch before resuming. No ordinary ticket belongs
              // to this scratch; never release it at an unretired GPU step.
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
          HRESULT result = ExecuteWorkGraph(device_, queue_, *graph->dispatch, textures, atomic64_phase_cache_);
          if (FAILED(result)) {
            ERR("DispatchGraph execution failed: ", str::format(result));
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          if (!start_segment()) return;
          current = current->next;
          continue;
        }
        if (current->type == EncoderType::CaptureFrontier) {
          auto &frontier = *static_cast<CaptureFrontierEncoderData *>(current);
          try {
            CaptureFrontierExecution execution;
            if (!PrepareCaptureFrontier(frontier, execution)) {
              ERR("ExecuteIndirect: bounded capture frontier allocation or layout failed");
              device_->ReportRemoval(E_OUTOFMEMORY);
              return;
            }
            const auto &p = execution.params;
            const char *path = p.Kind == kCaptureFrontierVertex ? "vertex_so" :
                p.Kind == kCaptureFrontierGeometry ? "geometry" :
                p.Kind == kCaptureFrontierDomain ? "domain" : "tess_geometry";
            owned_graphics::Trace("encode", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                ",\"list\":", list.list_id, ",\"recording\":", list.recording,
                ",\"encoder\":", frontier.id, ",\"type\":", uint32_t(frontier.type));
            owned_graphics::Trace("capture_frontier_begin", ",\"queue\":", owned_id_, ",\"encoder\":", frontier.id,
                ",\"path\":\"", path, "\",\"vertex_capacity\":", kCaptureFrontierVertexCapacity,
                ",\"object_group_capacity\":1,\"tess_parent_slots\":", p.ParentSlotCapacity,
                ",\"gs_key_capacity\":", p.KeyCapacity, ",\"parent_bytes\":", p.TessScratchBytes + p.TessListBytes,
                ",\"child_bytes\":", p.ScratchBytes + p.VertexBytes);
            auto uses = CollectStandardSwizzleTables(frontier.producer->swizzle_tables);
            if (frontier.consumer != frontier.producer && frontier.consumer->swizzle_tables)
              AppendStandardSwizzleTables(*frontier.consumer->swizzle_tables, uses);
            for (auto *render : {frontier.producer, frontier.consumer}) {
              for (const auto &color : render->colors) {
                if (color.attachment)
                  AppendStandardTexture(uses, color.attachment->allocation->descriptor, true,
                      color.attachment->key, color.attachment->allocation);
                if (color.resolve_attachment)
                  AppendStandardTexture(uses, color.resolve_attachment->allocation->descriptor, true,
                      color.resolve_attachment->key, color.resolve_attachment->allocation);
              }
            }
            std::vector<WMT::Reference<WMT::Heap>> swizzle_heaps;
            auto retain = [&] {
              RetainCaptureFrontier(scope->inflight, execution);
              if (atomic.phase) scope->inflight.atomic64_phase = atomic.phase;
              for (const auto &use : uses) {
                if (use.resource) scope->inflight.swizzle_resources.push_back(use.resource);
                if (use.baseline) scope->inflight.swizzle_baselines.push_back(use.baseline);
                // Special views are reached by raw texture IDs and are not
                // part of the descriptor-view pool's automatic residency.
                // Keep their owners and allocations on every resumed segment.
                for (const auto &buffer : use.volume_buffers) {
                  if (!buffer) continue;
                  scope->inflight.so_indirect_buffers.push_back(buffer);
                  scope->inflight.cmdbuf.useAllocation(WMT::Allocation{buffer.handle});
                }
                if (!use.cast_view) continue;
                scope->inflight.cast_views.push_back(use.cast_view);
                scope->inflight.cast_allocations.push_back(use.cast_allocation);
                const auto &view = *use.cast_view;
                if (view.texture)
                  scope->inflight.cmdbuf.useAllocation(WMT::Allocation{view.texture.handle});
                if (view.msaaSampleAddress) {
                  scope->inflight.cmdbuf.useAllocation(WMT::Allocation{view.msaaSampleIds.handle});
                  for (uint32_t sample = 0; sample < view.allocation->descriptor->sampleCount(); ++sample)
                    scope->inflight.cmdbuf.useAllocation(WMT::Allocation{view.msaaViews[sample].handle});
                }
              }
              for (const auto &heap : swizzle_heaps) {
                scope->inflight.volume_heaps.push_back(heap);
                scope->inflight.cmdbuf.useAllocation(heap);
              }
            };
            retain();
            const auto swizzle_heap_begin = scope->inflight.volume_heaps.size();
            if (!BeginStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, uses)) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
            // Import registers the actual sparse backing heaps it selected.
            // Snapshot those references once; continuation must not remap or
            // re-import a view, or observe a later mutable heap selection.
            swizzle_heaps.assign(scope->inflight.volume_heaps.begin() + swizzle_heap_begin,
                                 scope->inflight.volume_heaps.end());
            EncodeCaptureFrontierAttachments(scope->inflight.cmdbuf, *frontier.producer, false);
            EncodeCaptureFrontierVisibility(scope->inflight.cmdbuf, *frontier.producer, true);
            if (frontier.consumer != frontier.producer)
              EncodeCaptureFrontierVisibility(scope->inflight.cmdbuf, *frontier.consumer, true);
            std::vector<Atomic64RootBinding> atomic_roots;
            std::unique_ptr<std::vector<StandardSwizzleTableUse>> atomic_tables;
            uint32_t budget = kCaptureFrontierFirstQuantum;
            for (;;) {
              retain();
              auto buffer = scope->inflight.cmdbuf;
              for (uint32_t step = 0; step < budget; ++step) {
                EncodeCaptureFrontierKernel(buffer, execution, frontier.prepare_pipeline);
                EncodeCaptureFrontierKernel(buffer, execution, frontier.clear_pipeline, 0, true);
                if (p.Kind == kCaptureFrontierVertex)
                  EncodeCaptureFrontierVertex(buffer, frontier, execution);
                else if (!EncodeCaptureFrontierRender(buffer, scope->inflight, frontier, execution, false)) {
                  device_->ReportRemoval(E_OUTOFMEMORY);
                  return;
                }
                EncodeCaptureFrontierKernel(buffer, execution, frontier.consume_pipeline);
                if (p.Kind == kCaptureFrontierTessellationGeometry ||
                    (p.Kind == kCaptureFrontierVertex && (p.Flags & kCaptureFrontierRaster) &&
                     !(p.Flags & kCaptureFrontierMSAA))) {
                  if (!EncodeCaptureFrontierRender(buffer, scope->inflight, frontier, execution, true)) {
                    device_->ReportRemoval(E_OUTOFMEMORY);
                    return;
                  }
                }
                EncodeCaptureFrontierKernel(buffer, execution, frontier.consume_pipeline, 1);
                if (execution.msaa_draw) EncodeCaptureFrontierBounds(buffer, execution);
                EncodeCaptureFrontierKernel(buffer, execution, frontier.advance_pipeline);
              }
              EncodeCaptureFrontierKernel(buffer, execution, frontier.advance_pipeline, 1);
              const uint64_t producer = scope->seq;
              scope.reset();
              submission_ring_.waitUntilRetired(producer);
              if (device_->RemovedReason() != S_OK) return;
              const CaptureFrontierCompletion completed = *execution.mailbox;
              owned_graphics::Trace("capture_frontier_quantum", ",\"queue\":", owned_id_, ",\"encoder\":", frontier.id,
                  ",\"producer\":", producer, ",\"quantum\":", completed.Quantum, ",\"budget\":", budget,
                  ",\"producer_steps\":", completed.ProducerSteps, ",\"consumer_steps\":", completed.ConsumerSteps,
                  ",\"parent_steps\":", completed.ParentSteps, ",\"child_steps\":", completed.ChildSteps,
                  ",\"commands_done\":", completed.CommandsCompleted, ",\"status\":", completed.Status,
                  ",\"error\":", completed.Error, ",\"host_reason\":", completed.HostReason,
                  ",\"done\":", completed.Status == kCaptureFrontierDone ? "true" : "false");
              if (completed.Status == kCaptureFrontierError || completed.Error ||
                  completed.Status > kCaptureFrontierHost) {
                ERR("ExecuteIndirect: GPU capture frontier invariant failed: ", completed.Error);
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              if (!start_segment()) return;
              retain();
              if (completed.Status == kCaptureFrontierDone) break;
              budget = kCaptureFrontierQuantum;
              if (completed.Status == kCaptureFrontierRunning) continue;
              owned_graphics::Trace("capture_frontier_host", ",\"queue\":", owned_id_, ",\"encoder\":", frontier.id,
                  ",\"producer\":", producer, ",\"reason\":\"",
                  completed.HostReason == kCaptureFrontierAtomicBindings ? "atomic_bindings" : "msaa_consume", "\"");
              if (completed.HostReason == kCaptureFrontierAtomicBindings) {
                if (atomic.phase && !end_atomic()) return;
                atomic_roots.clear();
                atomic_tables.reset();
                if (!read_render_atomic_arguments(*frontier.producer, execution.roots, 0, atomic_roots, atomic_tables) ||
                    !begin_atomic(execution.participant_bound, atomic_tables, atomic_roots)) return;
                retain();
              } else if (completed.HostReason == kCaptureFrontierMSAAConsume && execution.msaa_draw) {
                // Captured output capacity is fixed. Eligible consumers keep
                // primitive/tile cursors on the GPU; legacy clipping variants
                // retain the original bounds host phase.
                if (atomic.phase && !end_atomic()) return;
                const bool pixel_atomic = execution.msaa_draw->atomic64_phase;
                if (pixel_atomic && !begin_atomic(16384, atomic_tables, atomic_roots)) return;
                retain();
                if (!EncodeMSAARender(scope->inflight.cmdbuf, scope->inflight, *frontier.consumer,
                                     *execution.msaa_draw, execution.msaa, retire_msaa,
                                     list.list_id, list.recording, scope->seq)) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                  return;
                }
                if (pixel_atomic) {
                  if (!end_atomic()) return;
                } else if (!execution.msaa_draw->gpu_tile_tasks) {
                  const uint64_t consumer = scope->seq;
                  scope.reset();
                  submission_ring_.waitUntilRetired(consumer);
                  if (device_->RemovedReason() != S_OK || !start_segment()) return;
                }
                retain();
              } else {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              // CPU writes only the completion acknowledgement, after the
              // prior consumer's real retirement. GPU owns every cursor.
              execution.mailbox->Status = kCaptureFrontierRunning;
            }
            EncodeCaptureFrontierAttachments(scope->inflight.cmdbuf, *frontier.consumer, true);
            if (atomic.phase && !end_atomic()) return;
            retain();
            if (!EndStandardSwizzlePass(scope->inflight.cmdbuf, scope->inflight, uses)) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
              return;
            }
          } catch (const std::bad_alloc &) {
            device_->ReportRemoval(E_OUTOFMEMORY);
            return;
          }
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
          const auto &render = *static_cast<const RenderEncoderData *>(current);
          // Indirect capture templates are detached into CaptureFrontier at
          // recording time. Never reintroduce a CPU-sized fallback here.
          if (render.so_indirect || render.capture_indirect) {
            ERR("ExecuteIndirect: capture template escaped its logical frontier");
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
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
              uint64_t slots = 0, records = 0;
              if (!resource_contract::CheckedMul(grid[0], grid[1], slots) ||
                  !resource_contract::CheckedMul(slots, grid[2], slots) || slots > (1u << 22) ||
                  !resource_contract::CheckedMul(slots, draw.mesh_layout.max_primitives, records) ||
                  !resource_contract::CheckedMul(records, draw.mesh_layout.vertices_per_primitive, records) ||
                  records > UINT32_MAX)
                return false;
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
                uint64_t slots = 0, records = 0;
                if (!resource_contract::CheckedMul(primitives, instances, slots) ||
                    !resource_contract::CheckedMul(slots, draw.mesh_layout.max_workloads, slots) ||
                    slots > UINT32_MAX ||
                    !resource_contract::CheckedMul(slots, draw.mesh_layout.max_primitives, records) ||
                    !resource_contract::CheckedMul(records, draw.mesh_layout.vertices_per_primitive, records) ||
                    records > UINT32_MAX) {
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
                const uint32_t index_size = record.index_format == DXGI_FORMAT_R32_UINT ? 4 : 2;
                draw.indexed = render->so_indirect->preflight.indexed;
                const bool dynamic_index = draw.indexed && render->so_indirect->index_view_heap;
                draw.assembly.index_size = dynamic_index ? 8 : index_size;
                const auto cut = render->so_indirect->strip_cut;
                draw.assembly.cut_enabled = draw.indexed && strip &&
                    ((index_size == 2 && cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF) ||
                     (index_size == 4 && cut == D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF));
                draw.assembly.cut_value = index_size == 4 ? 0xffffffffu : 0xffffu;
                draw.index_buffer = prepared->index_buffer;
                const uint64_t first = dynamic_index ? 0
                    : record.index_address + uint64_t(record.start_location) * index_size;
                draw.assembly.index_skip = first & 3;
                if (dynamic_index) draw.assembly.first_index = 0; // the GPU view already holds StartIndexLocation
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
            if (std::any_of(msaa_draws.begin(), msaa_draws.end(),
                [](const auto &draw) { return !draw.gpu_tile_tasks; })) {
              const uint64_t producer = scope->seq;
              scope.reset();
              owned_graphics::Trace("msaa_producer_wait", ",\"queue\":", owned_id_,
                  ",\"encoder\":", render->id, ",\"producer\":", producer, ",\"wait_count\":1");
              submission_ring_.waitUntilRetired(producer);
              if (device_->RemovedReason() != S_OK || !start_segment()) return;
            }
          }
        }

        raytracing::DeserializedStructure as_deserialized;
        raytracing::AccelerationStructureHeader as_source_header{};
        raytracing::NativeDescriptor as_native;
        WMT::Reference<WMT::Buffer> as_scratch;
        bool as_rebuild_update = false;
        bool as_fast_refit = false;
        bool as_preserve_index_shadow = false;
        if (current->type == EncoderType::Raytracing) {
          auto *as = static_cast<RTASEncoderData *>(current);
          const char *refit_reason = "not_refit";
          if (as->deferredUpdate) {
            // ProcessWait gates this FIFO worker on validated producer retirement.
            // Thus a valid cross-queue writer is encoded before its consumer and
            // replaces this proof; mere CPU Publish never does. This is execution
            // provenance only, not a substitute for GPU mirror observations.
            auto resolve_refit = [&]() -> const char * {
              const raytracing::ArenaKey sourceKey{as->sourceLocation.allocationIdentity, as->sourceLocation.offset};
              if (sourceKey != as->destinationKey || as->source != as->destination)
                return "out_of_place";
              if (!HasUnaliasedRTASStorage(as->sourceLocation))
                return "aliased_or_sparse_storage";
              if (as->metadata.type > 1 || as->metadata.reserved0)
                return "omm";
              RTASState source;
              if (!device_->FindRTASExecution(sourceKey, owned_id_, source))
                return "unproven_execution";
              const auto &proof = source.execution;
              if (source.sidecar != as->source || as->source.resourceID() != proof.resourceID ||
                  as->source.size() != proof.nativeSize || as->source.instanceCount() != proof.instanceCount)
                return "native_generation";
              if (as->nativeCapacity > proof.nativeSize || as->instanceCount != proof.instanceCount)
                return "native_capacity";
              const auto &metadata = proof.metadata;
              if (metadata.magic != raytracing::kHeaderMagic || metadata.type > 1 || metadata.reserved0 ||
                  metadata.shadowSize > as->sourceLocation.available - raytracing::kHeaderSize ||
                  metadata.currentSize > as->destinationLocation.available ||
                  !raytracing::IsCompatibleUpdate(*source.info, as->deferredInputs, as->shadowPlan))
                return "source_shape_or_bounds";
              as_source_header.currentSize = metadata.currentSize;
              as_source_header.deserializedSize = metadata.deserializedSize;
              // D3D12 ordinary updates may change positions/transforms, but not
              // index contents. Keep the existing index shadow only when the
              // executed source owns this exact in-place byte layout as well.
              as_preserve_index_shadow = metadata.type == 1 && source.info->shadowSize == as->shadowPlan.size &&
                  std::equal(source.info->geometries.begin(), source.info->geometries.end(),
                      as->shadowPlan.geometries.begin(), [](const auto &a, const auto &b) {
                        return !memcmp(&a, &b, sizeof(raytracing::ShadowGeometry));
                      });
              return nullptr;
            };
            refit_reason = resolve_refit();
            as_fast_refit = !refit_reason;
          }
          const bool ommBuild = as->operation == RTASOperation::Build && as->metadata.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
          if (ommBuild || as->operation == RTASOperation::Deserialize || as->operation == RTASOperation::Clone ||
              as->operation == RTASOperation::Compact || as->operation == RTASOperation::Serialize ||
              as->operation == RTASOperation::Visualize || (as->deferredUpdate && !as_fast_refit)) {
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
              owned_graphics::Trace("rtas_metadata_readback", ",\"queue\":", owned_id_,
                  ",\"submission\":", producer, ",\"list\":", list.list_id, ",\"recording\":", list.recording,
                  ",\"encoder\":", as->id, ",\"operation\":", uint32_t(as->operation),
                  ",\"bytes\":", length, ",\"cpu_wait_count\":1");
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
              D3DalloyRtasSerializedLayout layout;
              if (d3dalloy_rtas_serialized_prefix(&serialized, sizeof(serialized), as->sourceLocation.available, &layout)) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              if (layout.blocks) {
                if (!read_source(D3DALLOY_RTAS_SERIALIZED_HEADER_SIZE, D3DALLOY_RTAS_SERIALIZED_BLOCK_SIZE) ||
                    d3dalloy_rtas_serialized_block(&layout, mapped, D3DALLOY_RTAS_SERIALIZED_BLOCK_SIZE, &layout)) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
              }
              if (!read_source(layout.header_offset, raytracing::kHeaderSize)) return;
              memcpy(&as_source_header, mapped, sizeof(as_source_header));
              uint64_t geometryBytes;
              if (d3dalloy_rtas_serialized_geometry_bytes(&layout, &as_source_header, sizeof(as_source_header), &geometryBytes)) {
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              if (geometryBytes && !read_source(layout.header_offset + raytracing::kHeaderSize, geometryBytes)) return;
              if (FAILED(raytracing::ParseSerialized(serialized, layout, as_source_header, mapped, geometryBytes,
                      device_->GetMTLDevice().registryID(), as->destinationLocation, as_deserialized))) {
                ERR("Invalid serialized acceleration structure");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
              }
              raytracing::NativeSizes sizes;
              if (FAILED(device_->QueryRTASSizes(as_deserialized.inputs, sizes)) ||
                  (as_source_header.type != 2 && !device_->AllocateRTAS(as->destination, sizes.accelerationStructureSize, as_deserialized.inputs.instanceCount)) ||
                  FAILED(raytracing::CreateNativeDescriptor(device_->GetMTLDevice(), as_deserialized.inputs,
                      as->destination.instanceBuffer(), as_native, &sizes))) {
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
                const bool overlaps = as->sourceLocation.buffer == as->destinationLocation.buffer &&
                    as->sourceLocation.offset != as->destinationLocation.offset &&
                    as->sourceLocation.offset < as->destinationLocation.offset + as_source_header.currentSize &&
                    as->destinationLocation.offset < as->sourceLocation.offset + as_source_header.currentSize;
                if (overlaps || as_source_header.currentSize > as->destinationLocation.available ||
                    !raytracing::IsCompatibleUpdate(sourceInfo, as->deferredInputs, as->shadowPlan)) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
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
          if (as->deferredUpdate) {
            const uint64_t nativeSize = std::max(as->source.size(), as->nativeCapacity);
            as_rebuild_update = as->source == as->destination && nativeSize > as->source.size();
            raytracing::NativeSizes update_sizes;
            if (FAILED(device_->QueryRTASSizes(as->deferredInputs, update_sizes)) ||
                (!as_fast_refit && !device_->AllocateRTAS(as->destination, nativeSize, as->instanceCount)) ||
                FAILED(raytracing::CreateNativeDescriptor(device_->GetMTLDevice(), as->deferredInputs,
                    as->destination.instanceBuffer(), as_native, &update_sizes))) {
              device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
            }
            if (as_rebuild_update || !as->scratch) {
              WMTBufferInfo scratch{};
              scratch.length = std::max<uint64_t>(
                  as_rebuild_update ? as_native.sizes.buildScratchSize : as_native.sizes.refitScratchSize, 256);
              scratch.options = WMTResourceStorageModePrivate;
              as_scratch = device_->GetMTLDevice().newBuffer(scratch);
              if (!as_scratch) { device_->ReportRemoval(E_OUTOFMEMORY); return; }
              scope->inflight.msaa_buffers.push_back(as_scratch);
            }
            owned_graphics::Trace("rtas_refit_resolution", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                ",\"list\":", list.list_id, ",\"recording\":", list.recording, ",\"encoder\":", as->id,
                ",\"fast_path\":", as_fast_refit, ",\"readback_count\":", as_fast_refit ? 0 : 1,
                ",\"cpu_wait_count\":", as_fast_refit ? 0 : 1,
                ",\"reason\":\"", as_fast_refit ? "ordered_in_place" : refit_reason, "\"");
          }
        }
        WMT::MTL4CommandBuffer cmdbuf = scope->inflight.cmdbuf;
        if (current->type != EncoderType::Null &&
            (current->type != EncoderType::Clear || !static_cast<ClearEncoderData *>(current)->folded_into_render))
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
          if (data->folded_into_render)
            break;
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
            owned_graphics::Trace("native_clear_begin", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"members\":1,\"begin_count\":1");
            encoder.setLabel(clear_pass_label_);
            if (native_fence_required) {
              encoder.waitForFence(fence_, WMTRenderStageFragment);
              owned_graphics::Trace("native_clear_wait", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"wait_count\":1");
            } else if (consumer_barrier_after) {
              encoder.barrierAfterQueueStages(consumer_barrier_after, uint64_t(WMTRenderStageFragment), consumer_visibility);
              owned_graphics::Trace("native_clear_queue_barrier", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                  ",\"after\":", consumer_barrier_after, ",\"barrier_count\":1");
            }
            for (uint8_t aspect : {uint8_t(1), uint8_t(2), uint8_t(4)})
              if ((data->clear_dsv ? data->clear_dsv : 4) & aspect)
                owned_graphics::Trace("native_clear_load", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                    ",\"aspect\":", uint32_t(aspect), ",\"clear_count\":1");
            encoder.updateFence(fence_, WMTRenderStageFragment);
            owned_graphics::Trace("native_clear_update", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"update_count\":1");
            encoder.endEncoding();
            owned_graphics::Trace("native_clear_end", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"end_count\":1");
          }
          if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::Render: {
          auto data = static_cast<RenderEncoderData *>(current);
          auto swizzle_uses = StandardSwizzleTablesMayMatter() ? CollectStandardSwizzleTables(*data)
                                                               : std::vector<SwizzlePassResource>{};
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
          owned_graphics::Trace("render_work", ",\"encoder\":", data->id,
                                ",\"width\":", data->render_target_width, ",\"height\":", data->render_target_height,
                                ",\"swizzle\":", swizzle_uses.size(), ",\"msaa\":", bool(data->msaa_draw),
                                ",\"atomic64\":", bool(data->atomic64_phase),
                                ",\"rov_samples\":", uint32_t(data->rov_marker_sample_count),
                                ",\"visibility\":", data->visibility_targets.size(),
                                ",\"stream_output\":", data->so_draws.size(),
                                ",\"capture_indirect\":", bool(data->capture_indirect),
                                ",\"depth\":", bool(data->depth.attachment));
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
                                    msaa_draws[i], msaa_executions[i], retire_msaa,
                                    list.list_id, list.recording, scope->seq)) {
                ERR("MSAA tile execution resource allocation failed");
                device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
                return;
              }
              cmdbuf = scope->inflight.cmdbuf;
              if (msaa_draws[i].atomic64_phase) {
                if (!end_atomic(&swizzle_uses)) return;
                cmdbuf = scope->inflight.cmdbuf;
              }
            }
            if (data->msaa_draw->needs_stream_assembly) {
              // Tile retirement may have consumed the producer's submission.
              // The assembly status and its backing owner belong to the segment
              // that actually executes the final SO/statistics assembly.
              try {
                scope->inflight.so_draws.reserve(scope->inflight.so_draws.size() + so_draws.size());
              } catch (const std::bad_alloc &) {
                device_->ReportRemoval(E_OUTOFMEMORY);
                return;
              }
              for (const auto &captured : so_draws) {
                scope->inflight.so_draws.push_back(captured);
                EncodeStreamOutputAssembly(cmdbuf, *captured);
              }
            }
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
          if (replay_current_node && replay_current_node->folded_resolve) {
            const auto &folded = *replay_current_node->folded_resolve;
            auto &color_info = render_pass_info.colors[folded.slot];
            color_info.store_action = WMTStoreActionStoreAndMultisampleResolve;
            color_info.resolve_texture = folded.target.texture();
            color_info.resolve_level = 0;
            color_info.resolve_slice = 0;
            color_info.resolve_depth_plane = 0;
            owned_graphics::Trace("native_resolve_folded", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                                  ",\"slot\":", folded.slot, ",\"resolve_pass_elided\":1");
          }
          if (data->native_msaa_sample_count) {
            if (data->submission_tables) {
              bool bindings_safe = true;
              for (const auto &table : *data->submission_tables) {
                D3D12_DESCRIPTOR_HEAP_DESC desc{}; D3D12_GPU_DESCRIPTOR_HANDLE start{};
                table.heap->GetDesc(&desc); table.heap->GetGPUDescriptorHandleForHeapStart(&start);
                if (!start.ptr || table.base < start.ptr || (table.base - start.ptr) % 32 || table.parameter == ~0u) {
                  bindings_safe = false; break;
                }
                const auto base = (table.base - start.ptr) / 32;
                if (base >= desc.NumDescriptors) { bindings_safe = false; break; }
                table.rootsig->ForEachUsedDescriptorRange(table.rootsig->GetTableRanges(table.parameter), table.shader_bindings,
                    [&](uint64_t offset, uint64_t count, D3D12_DESCRIPTOR_RANGE_TYPE kind) {
                  if (!bindings_safe || kind == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) return;
                  if (kind == D3D12_DESCRIPTOR_RANGE_TYPE_UAV || offset >= desc.NumDescriptors - base ||
                      count > desc.NumDescriptors - base - offset) { bindings_safe = false; return; }
                  for (uint64_t i = 0; bindings_safe && i < count; ++i)
                    bindings_safe = NativeMSAADescriptorSafe(device_, table.heap->GetDescriptor(base + offset + i));
                });
              }
              if (!bindings_safe) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return; }
            }
            const auto safe_view = [&](const TextureViewRef &view) {
              if (!view) return true;
              const auto *v = view.ptr();
              const auto *a = v->allocation;
              return !v->castBlockSize && !v->msaaSampleAddress && !v->reservedCast && !v->reservedVolumeRender &&
                  !v->reservedVolumeAtlasCast && !a->canonicalHeap &&
                  !a->flags().test(TextureAllocationFlag::Shared) &&
                  !a->flags().test(TextureAllocationFlag::PlacementSparse) &&
                  a->descriptor->sampleCount() == data->native_msaa_sample_count &&
                  a->descriptor->textureType() == WMTTextureType2DMultisample &&
                  !v->key.mip_start && !v->key.array_start && v->key.array_end == 1 &&
                  !device_->LookupStandardSwizzleTexture(a->descriptor);
            };
            bool valid = !data->msaa_draw && !data->dsv_descriptor_flags &&
                safe_view(data->depth.attachment) && safe_view(data->stencil.attachment);
            for (const auto &color : data->colors)
              valid &= safe_view(color.attachment) && !color.buffer_attachment &&
                  (!color.resolve_attachment ||
                   ((color.store_action == WMTStoreActionStoreAndMultisampleResolve ||
                     color.store_action == WMTStoreActionMultisampleResolve) &&
                    !color.resolve_attachment->allocation->canonicalHeap &&
                    !color.resolve_attachment->allocation->flags().test(TextureAllocationFlag::PlacementSparse) &&
                    color.resolve_attachment->allocation->descriptor->sampleCount() == 1));
            if (!valid) { device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return; }
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
          if (replay_group) render_pass_info = replay_group->descriptor;
          if (data->visibility_intervals && data->visibility_interval_count) {
            // GPU-driven draws can contain zero work: initialize every interval
            // even when native visibility Reset never encounters a fragment.
            auto zero = cmdbuf.blitCommandEncoder();
            zero.waitForFence(fence_);
            wmtcmd_blit_fillbuffer fill{};
            fill.type = WMTBlitCommandFillBuffer;
            fill.next.set(nullptr);
            fill.buffer = data->visibility_scratch;
            fill.length = data->visibility_interval_count * sizeof(uint64_t);
            zero.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
            zero.updateFence(fence_);
            zero.endEncoding();
            owned_graphics::Trace("query_scratch_fill", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                ",\"encoder\":", data->id, ",\"slots\":", data->visibility_interval_count,
                ",\"bytes\":", fill.length, ",\"fill_count\":1");
          }
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          owned_graphics::Trace("native_render_begin", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
              ",\"encoder\":", data->id, ",\"members\":", replay_group ? replay_group->members.size() : 1,
              ",\"native_msaa_sample_count\":", uint32_t(data->native_msaa_sample_count), ",\"begin_count\":1");
          for (unsigned i = 0; i < (replay_group ? replay_group->color_count : data->render_target_count); ++i)
            owned_graphics::Trace("native_render_attachment", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                ",\"slot\":", i, ",\"load\":", uint32_t(render_pass_info.colors[i].load_action),
                ",\"store\":", uint32_t(render_pass_info.colors[i].store_action));
          for (unsigned i = 0; i < 8; ++i)
            if (render_pass_info.colors[i].texture && render_pass_info.colors[i].load_action == WMTLoadActionClear)
              owned_graphics::Trace("native_clear_load", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                  ",\"slot\":", i, ",\"aspect\":4,\"clear_count\":1");
          if (render_pass_info.depth.texture && render_pass_info.depth.load_action == WMTLoadActionClear)
            owned_graphics::Trace("native_clear_load", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"aspect\":1,\"clear_count\":1");
          if (render_pass_info.stencil.texture && render_pass_info.stencil.load_action == WMTLoadActionClear)
            owned_graphics::Trace("native_clear_load", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"aspect\":2,\"clear_count\":1");
          // One encodeCommands chain instead of a unix call per useResource; the
          // native walker applies the nodes in this same order.
          std::vector<wmtcmd_render_useresource> resource_uses;
          auto use_resource = [&](WMT::Resource resource, WMTResourceUsage usage) {
            wmtcmd_render_useresource &command = resource_uses.emplace_back();
            command.type = WMTRenderCommandUseResource;
            command.resource = resource;
            command.usage = usage;
            command.stages = WMTRenderStageVertex | WMTRenderStageFragment | WMTRenderStageObject | WMTRenderStageMesh;
          };
          for (const auto &use : swizzle_uses) {
            if (!use.cast_view) continue;
            const auto usage = use.writes ? WMTResourceUsageRead | WMTResourceUsageWrite : WMTResourceUsageRead;
            if (use.cast_view->castBlockSize)
              use_resource(use.cast_view->texture, usage);
            if (use.cast_view->msaaSampleAddress) {
              use_resource(use.cast_view->msaaSampleIds, WMTResourceUsageRead);
              for (uint32_t sample = 0; sample < use.cast_view->allocation->descriptor->sampleCount(); ++sample)
                use_resource(use.cast_view->msaaViews[sample], usage);
            }
          }
          if (!resource_uses.empty()) {
            for (size_t i = 0; i < resource_uses.size(); ++i)
              resource_uses[i].next.set(i + 1 < resource_uses.size() ? &resource_uses[i + 1] : nullptr);
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(resource_uses.data()));
          }
          // Object/mesh (GS/HS/DS) functions read resources earlier writers produced.
          const bool mesh_pass = data->use_geometry || data->use_tessellation;
          WMTRenderStages wait_stages = mesh_pass ? WMTRenderStagePreRaster : WMTRenderStageVertex;
          if (replay_current_node && replay_current_node->access_complete) {
            uint64_t stages = replay_group ? replay_group->stages : 0;
            for (const auto &access : replay_current_node->accesses) stages |= access.stages;
            constexpr uint64_t render_stage_mask = WMTMetal4StageVertex | WMTMetal4StageFragment |
                WMTMetal4StageTile | WMTMetal4StageObject | WMTMetal4StageMesh;
            const auto selected_stages = stages & render_stage_mask;
            if (selected_stages) wait_stages = WMTRenderStages(selected_stages);
          }
          if (native_fence_required) {
            encoder.waitForFence(fence_, wait_stages);
            owned_graphics::Trace("native_render_wait", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                ",\"stages\":", uint32_t(wait_stages), ",\"wait_count\":1");
          } else if (consumer_barrier_after) {
            encoder.barrierAfterQueueStages(consumer_barrier_after, uint64_t(wait_stages), consumer_visibility);
            owned_graphics::Trace("native_render_queue_barrier", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                ",\"after\":", consumer_barrier_after, ",\"before\":", uint32_t(wait_stages),
                ",\"visibility\":", consumer_visibility, ",\"barrier_count\":1");
          }
          for (uint32_t i = 0; i < data->rov_marker_sample_count; ++i) {
            encoder.setFragmentTexture(rov_markers[i], 31 + i);
            encoder.useResource(rov_markers[i], WMTResourceUsageRead | WMTResourceUsageWrite,
                                WMTRenderStageFragment);
          }
          auto set_member_map = [&](size_t member) {
            auto map = replay_group->color_maps[member];
            // 0xff never crosses the native ABI. Inactive logical outputs
            // route into a capacity-proven, truly unbound physical slot.
            for (unsigned logical = 0; logical < 8; ++logical)
              if (map[logical] == 0xff) map[logical] = uint8_t(std::max<unsigned>(logical, replay_group->color_count));
            encoder.setColorAttachmentMap(map.data());
            owned_graphics::Trace("native_render_color_map", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                ",\"member\":", member, ",\"map_count\":1,\"physical0\":", uint32_t(map[0]),
                ",\"physical1\":", uint32_t(map[1]), ",\"physical2\":", uint32_t(map[2]), ",\"physical3\":", uint32_t(map[3]),
                ",\"physical4\":", uint32_t(map[4]), ",\"physical5\":", uint32_t(map[5]), ",\"physical6\":", uint32_t(map[6]),
                ",\"physical7\":", uint32_t(map[7]));
          };
          if (replay_group) set_member_map(0);
          else if (data->submission_group_safe) {
            const uint8_t identity[8] = {0, 1, 2, 3, 4, 5, 6, 7};
            encoder.setColorAttachmentMap(identity);
            owned_graphics::Trace("native_render_color_map_identity", ",\"submission\":", scope->seq,
                ",\"encoder\":", data->id, ",\"map_count\":1");
          }
          encoder.encodeCommands(&data->cmd_head);
          if (replay_group) {
            // PreDraw reinitializes PSO/fixed state, roots, input tables,
            // viewport/scissor, blend and stencil for every independent chain.
            for (size_t member = 1; member < replay_group->members.size(); ++member) {
              const auto &node = plan.nodes[replay_group->members[member]];
              const auto *render = static_cast<const RenderEncoderData *>(node.record);
              uint64_t before = 0, after = 0; uint8_t scope_mask = 0;
              for (const auto &dependency : node.dependencies)
                if (!dependency.attachment_only && plan.nodes[dependency.source].group == node.group) {
                  before |= dependency.before; after |= dependency.after; scope_mask |= dependency.scope;
                }
              if (scope_mask) {
                // Native replay maps stages_after to the producer and
                // stages_before to the consumer, unlike our edge names.
                encoder.memoryBarrier(WMTBarrierScope(scope_mask), WMTRenderStages(after), WMTRenderStages(before));
                owned_graphics::Trace("native_render_inline_barrier", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                    ",\"member\":", member, ",\"scope\":", uint32_t(scope_mask), ",\"before\":", before, ",\"after\":", after,
                    ",\"barrier_count\":1");
              }
              set_member_map(member);
              wmtcmd_render_set_vertex_amplification mapping{};
              mapping.type = WMTRenderCommandSetVertexAmplification;
              encoder.encodeCommands(reinterpret_cast<const wmtcmd_render_nop *>(&mapping));
              encoder.encodeCommands(&render->cmd_head);
              owned_graphics::Trace("native_render_member", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                  ",\"member_encoder\":", render->id, ",\"list_ordinal\":", node.list_ordinal,
                  ",\"node_ordinal\":", node.node_ordinal);
            }
          }
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
          const WMTRenderStages update_stages = replay_current_node && replay_current_node->access_complete ? wait_stages :
              WMTRenderStages((so_draws.empty() ? 0 : WMTRenderStageVertex) | WMTRenderStageFragment |
                              (mesh_pass ? WMTRenderStageObject | WMTRenderStageMesh : 0));
          encoder.updateFence(fence_, update_stages);
          owned_graphics::Trace("native_render_update", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
              ",\"stages\":", uint32_t(update_stages), ",\"update_count\":1");
          encoder.endEncoding();
          owned_graphics::Trace("native_render_end", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"end_count\":1");
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
          for (size_t first = 0; first < data->visibility_targets.size();) {
            const bool resets = data->visibility_targets[first].reset;
            size_t end = first + 1;
            // Only adjacent independent destinations share one encoder. Reset
            // boundaries, repeated counters and any scratch alias stay ordered.
            while (end < data->visibility_targets.size() && end - first < 64) {
              const auto &next = data->visibility_targets[end];
              if (next.reset != resets || (!resets && next.destination == data->visibility_scratch)) break;
              bool dependent = false;
              for (size_t prior = first; prior < end; ++prior) {
                const auto &old = data->visibility_targets[prior];
                dependent |= old.destination == next.destination && old.index == next.index;
                dependent |= !resets && old.destination == data->visibility_scratch;
              }
              if (dependent) break;
              ++end;
            }
            auto batch = cmdbuf.computeCommandEncoder(false);
            batch.waitForFence(fence_);
            for (size_t entry = first; entry < end; ++entry) {
              const auto &target = data->visibility_targets[entry];
            if (target.reset) {
              wmtcmd_blit_fillbuffer fill{};
              fill.type = WMTBlitCommandFillBuffer;
              fill.next.set(nullptr);
              fill.buffer = target.destination;
              fill.offset = uint64_t(target.index) * sizeof(uint64_t);
              fill.length = sizeof(uint64_t);
              batch.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
              owned_graphics::Trace("query_reset", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                  ",\"encoder\":", data->id, ",\"heap\":", target.heap_id, ",\"index\":", target.index,
                  ",\"reset_count\":1");
              continue;
            }
            wmtcmd_compute_nop head{};
            head.type = WMTComputeCommandNop;
            wmtcmd_compute_setpso set_pso{};
            set_pso.type = WMTComputeCommandSetPSO;
            set_pso.pso = target.accumulate_pipeline;
            set_pso.threadgroup_size = {1, 1, 1};
            wmtcmd_compute_setbuffer set_source{};
            set_source.type = WMTComputeCommandSetBuffer;
            set_source.buffer = data->visibility_scratch;
            set_source.offset = target.source_offset;
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
            batch.encodeCommands(&head);
            owned_graphics::Trace("query_accumulate", ",\"queue\":", owned_id_,
                                  ",\"submission\":", scope->seq, ",\"heap\":", target.heap_id,
                                  ",\"index\":", target.index, ",\"source_offset\":", target.source_offset);
            }
            batch.updateFence(fence_);
            batch.endEncoding();
            owned_graphics::Trace("query_fanout_batch", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                ",\"encoder\":", data->id, ",\"targets\":", end-first, ",\"reset\":", unsigned(resets),
                ",\"native_encoders\":1");
            first = end;
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
                  const bool fused_tlas_shadow = build && data->metadata.type == 0;
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
                  if ((build && !fused_tlas_shadow) || copy) {
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
                              raytracing::kHelperThreads, data->sourceLocation.buffer,
                              data->sourceLocation.buffer)) { fail(); break; }
                    } else {
                      const uint64_t geometryBytes = data->shadowPlan.geometries.size() * sizeof(raytracing::ShadowGeometry);
                      params.jobCount = 0;
                      uint64_t maxWork = 1;
                      uint64_t sourceBytes = 0, destinationBytes = 0, preservedIndexBytes = 0;
                      for (const auto &job : data->shadowPlan.jobs) {
                        if (as_preserve_index_shadow && (job.flags & raytracing::kShadowJobIndex)) {
                          preservedIndexBytes += uint64_t(job.count) * job.destinationStride;
                          continue;
                        }
                        ++params.jobCount;
                        uint64_t work = job.count;
                        if (job.mode == raytracing::kShadowJobAddress && job.sourceStride == job.elementSize &&
                            (job.destinationStride == job.elementSize || job.count == 1)) {
                          uint64_t alignment = job.source | (params.asAddress + job.destinationOffset);
                          uint32_t width = !(alignment & 15) ? 16 : !(alignment & 3) ? 4 : 1;
                          work = (uint64_t(job.count) * job.destinationStride + width - 1) / width;
                        }
                        maxWork = std::max(maxWork, work);
                        sourceBytes += uint64_t(job.count) * job.elementSize;
                        destinationBytes += uint64_t(job.count) * job.destinationStride;
                      }
                      const uint64_t jobsBytes = uint64_t(params.jobCount) * sizeof(raytracing::ShadowJob);
                      const uint64_t jobsOffset = std::max<uint64_t>(geometryBytes, sizeof(raytracing::ShadowGeometry));
                      const uint64_t jobsCapacity = std::max<uint64_t>(jobsBytes, sizeof(raytracing::ShadowJob));
                      if (jobsOffset > SIZE_MAX || jobsCapacity > SIZE_MAX - jobsOffset) {
                        device_->ReportRemoval(E_OUTOFMEMORY); break;
                      }
                      const auto tables = scope->inflight.AllocateASMetadata(device_->GetMTLDevice(), jobsOffset + jobsCapacity);
                      if (!tables.buffer || !tables.memory) { fail(); break; }
                      if (geometryBytes) memcpy(tables.memory, data->shadowPlan.geometries.data(), geometryBytes);
                      auto *jobs = reinterpret_cast<raytracing::ShadowJob *>(tables.memory + jobsOffset);
                      for (const auto &job : data->shadowPlan.jobs)
                        if (!as_preserve_index_shadow || !(job.flags & raytracing::kShadowJobIndex)) *jobs++ = job;
                      owned_graphics::Trace("rtas_metadata_upload", ",\"submission\":", scope->seq,
                          ",\"block\":", tables.block_id, ",\"offset\":", tables.offset,
                          ",\"geometry_bytes\":", geometryBytes, ",\"job_bytes\":", jobsBytes);
                      owned_graphics::Trace("rtas_shadow_work", ",\"submission\":", scope->seq,
                          ",\"jobs\":", params.jobCount, ",\"source_bytes\":", sourceBytes,
                          ",\"destination_bytes\":", destinationBytes, ",\"preserved_index_bytes\":", preservedIndexBytes);
                      uint32_t rows = std::clamp(params.jobCount, 1u, 64u);
                      maxWork = std::max(maxWork, (uint64_t(params.metadata.descCount) + rows - 1) / rows);
                      uint32_t threads = uint32_t(std::min<uint64_t>(maxWork, raytracing::kHelperThreads));
                      if (!EncodeASHelper(cmdbuf, data->pipelines.shadow, &params, sizeof(params),
                              threads, tables.buffer, tables.buffer, tables.offset + jobsOffset, tables.offset,
                              rows)) { fail(); break; }
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
                    if (fused_tlas_shadow) {
                      info.flags = WMTASPrepassWriteShadow;
                      auto metadata = data->metadata;
                      if (operation == RTASOperation::Refit) {
                        metadata.currentSize = as_source_header.currentSize;
                        metadata.deserializedSize = as_source_header.deserializedSize;
                      }
                      memcpy(info.shadow_metadata, &metadata, sizeof(metadata));
                      owned_graphics::Trace("rtas_tlas_fused_shadow", ",\"submission\":", scope->seq,
                          ",\"instances\":", instanceCount, ",\"source_bytes\":", uint64_t(instanceCount) * 64);
                    }
                    const bool prepass = compute.encodeASPrepass(data->pipelines.prepass, instances, instanceOffset,
                        data->destination.instanceBuffer(), 0, data->header, data->destinationOffset,
                        data->destination.contributionBuffer(), 0, data->liveMirror, 0, info);
                    compute.updateFence(fence_);
                    compute.endEncoding();
                    if (!prepass) { fail(); break; }
                    scope->inflight.rtas_observations.push_back({data->destinationKey, data->versionToken});
                    auto execution_metadata = data->metadata;
                    if (operation == RTASOperation::Refit) {
                      execution_metadata.currentSize = as_source_header.currentSize;
                      execution_metadata.deserializedSize = as_source_header.deserializedSize;
                    }
                    const bool ordinary_execution = as_fast_refit || (operation == RTASOperation::Build &&
                        data->metadata.type <= 1 && !data->metadata.reserved0 &&
                        HasUnaliasedRTASStorage(data->destinationLocation));
                    device_->RecordRTASExecution(data->destinationKey, data->versionToken, owned_id_,
                        ordinary_execution ? &execution_metadata : nullptr);
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
                      if (!EncodeASHelper(cmdbuf, data->pipelines.postbuild, &params, sizeof(params), 1)) { fail(); break; }
                    }
                    if (operation == RTASOperation::Compact) {
                      raytracing::PostbuildParams params{data->destinationLocation.address, 0, raytracing::kPostbuildFinalizeCompactedHeader, 0};
                      if (!EncodeASHelper(cmdbuf, data->pipelines.postbuild, &params, sizeof(params), 1)) { fail(); break; }
                    }
                  }
                  for (size_t first = 0; first < data->postbuild.size();) {
                    std::array<raytracing::PostbuildParams, 32> batch;
                    uint32_t count = 0;
                    auto overlaps = [](uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
                      return a <= b ? b - a < a_size : a - b < b_size;
                    };
                    auto output_size = [](uint32_t type) -> uint32_t { return type == 2 ? 16 : 8; };
                    while (count < batch.size() && count < data->postbuild.size() - first) {
                      const auto &entry = data->postbuild[first + count];
                      const auto &src = operation == RTASOperation::Emit ? entry.source : data->destinationLocation;
                      raytracing::PostbuildParams p{src.address, entry.destination.address, uint32_t(entry.type), 0};
                      // Only committed, non-aliased allocations have unique physical VA ranges.
                      bool independent = PlainAdmissionBuffer(static_cast<const BufferAllocation *>(src.allocationIdentity)) &&
                          PlainAdmissionBuffer(static_cast<const BufferAllocation *>(entry.destination.allocationIdentity));
                      for (uint32_t j = 0; independent && j < count; ++j) {
                        const auto &q = batch[j];
                        independent = !overlaps(p.destination, output_size(p.operation), q.destination, output_size(q.operation)) &&
                            !overlaps(p.destination, output_size(p.operation), q.asAddress, raytracing::kHeaderSize) &&
                            !overlaps(q.destination, output_size(q.operation), p.asAddress, raytracing::kHeaderSize);
                      }
                      if (!independent && count) break;
                      batch[count++] = p;
                      if (!independent) break;
                    }
                    batch[0].batchCount = count;
                    if (!EncodeASHelper(cmdbuf, data->pipelines.postbuild, batch.data(), count * sizeof(batch[0]), count)) {
                      fail(); break;
                    }
                    first += count;
                  }
                  break;
                }
        case EncoderType::Blit: {
          auto data = static_cast<BlitEncoderData *>(current);
          auto encoder = cmdbuf.blitCommandEncoder();
          owned_graphics::Trace("native_blit_begin", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"begin_count\":1");
          if (native_fence_required) {
            encoder.waitForFence(fence_);
            owned_graphics::Trace("native_blit_wait", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"wait_count\":1");
          } else if (consumer_barrier_after) {
            encoder.barrierAfterQueueStages(consumer_barrier_after, WMTMetal4StageBlit, consumer_visibility);
            owned_graphics::Trace("native_blit_queue_barrier", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                ",\"after\":", consumer_barrier_after, ",\"before\":", uint64_t(WMTMetal4StageBlit), ",\"barrier_count\":1");
          }
          encoder.encodeCommands(&data->cmd_head);
          owned_graphics::Trace("native_blit_encode", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"encode_count\":1");
          encoder.updateFence(fence_);
          owned_graphics::Trace("native_blit_update", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"update_count\":1");
          encoder.endEncoding();
          owned_graphics::Trace("native_blit_end", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"end_count\":1");
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
          auto swizzle_uses = StandardSwizzleTablesMayMatter() ? CollectStandardSwizzleTables(*data)
                                                               : std::vector<SwizzlePassResource>{};
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
            AtomicReadbackBatch metadata, records, local_roots;
            struct IndirectGrid { WMTSize threads; size_t request; };
            std::vector<IndirectGrid> grids;
            std::vector<std::array<size_t, 2>> indirect_metadata(data->atomic64_indirect.size(),
                std::array<size_t, 2>{SIZE_MAX, SIZE_MAX});
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
                const auto request = append_atomic_buffer(metadata, WMT::Buffer{indirect->indirect_args_buffer},
                    indirect->indirect_args_offset, sizeof(D3D12_DISPATCH_ARGUMENTS));
                if (request == SIZE_MAX) return;
                grids.push_back({threads, request});
              }
            }
            for (size_t index = 0; index < data->atomic64_indirect.size(); ++index) {
              const auto &use = data->atomic64_indirect[index];
              if (use.command->max_count_buffer) {
                auto &request = indirect_metadata[index][0];
                request = append_atomic_address(metadata, use.command->max_count_buffer, sizeof(uint32_t));
                if (request == SIZE_MAX) return;
              }
              if (use.predicate_buffer) {
                auto &request = indirect_metadata[index][1];
                request = append_atomic_buffer(metadata, use.predicate_buffer, use.predicate_offset, sizeof(uint64_t));
                if (request == SIZE_MAX) return;
              }
            }
            auto *ray_state = data->atomic64_ray
                ? static_cast<MTLD3D12StateObject *>(data->atomic64_ray->state_object.ptr()) : nullptr;
            auto *ray_heap = data->atomic64_ray
                ? static_cast<MTLD3D12DescriptorHeap *>(data->atomic64_ray->heap.ptr()) : nullptr;
            auto *global = ray_state ? ray_state->GetDispatchRootSignature() : nullptr;
            size_t global_request = SIZE_MAX, dispatch_request = SIZE_MAX;
            if (data->atomic64_ray) {
              const auto &use = *data->atomic64_ray;
              if (global && global->ParameterQwords) {
                global_request = append_atomic_buffer(metadata, use.roots, use.roots_offset,
                                                       uint64_t(global->ParameterQwords) * 8);
                if (global_request == SIZE_MAX) return;
              }
              dispatch_request = append_atomic_buffer(metadata, use.arguments, use.arguments_offset,
                                                       sizeof(SM50_RT_DISPATCH_ARGUMENTS));
              if (dispatch_request == SIZE_MAX) return;
            }
            // All fixed metadata observes the same preceding GPU work. Record
            // addresses and lengths are resolved only after this batch retires.
            if (!read_atomic_batch(metadata, "metadata", data->id)) return;
            for (const auto &grid : grids) {
              D3D12_DISPATCH_ARGUMENTS groups;
              memcpy(&groups, metadata[grid.request].result.bytes, sizeof(groups));
              if (!add_grid({groups.ThreadGroupCountX, groups.ThreadGroupCountY, groups.ThreadGroupCountZ}, grid.threads))
                return;
            }
            struct IndirectRecords { const Atomic64IndirectComputeUse *use; uint32_t count; size_t request; };
            std::vector<IndirectRecords> indirect_records;
            std::vector<Atomic64RootBinding> resolved_roots;
            if (!data->atomic64_indirect.empty() || data->atomic64_ray) resolved_roots = data->atomic64_roots;
            for (size_t index = 0; index < data->atomic64_indirect.size(); ++index) {
              const auto &use = data->atomic64_indirect[index];
              const auto &command = *use.command;
              uint32_t count = command.max_count;
              const auto [count_request, predicate_request] = indirect_metadata[index];
              if (count_request != SIZE_MAX) {
                memcpy(&count, metadata[count_request].result.bytes, sizeof(count));
                count = std::min<uint64_t>(count, command.max_count);
              }
              if (predicate_request != SIZE_MAX) {
                uint64_t value; memcpy(&value, metadata[predicate_request].result.bytes, sizeof(value));
                if (command.predicate_op ? value != 0 : value == 0) count = 0;
              }
              if (!count) continue;
              const auto request = append_atomic_address(records, command.argument_buffer, uint64_t(count) * use.stride);
              if (request == SIZE_MAX) return;
              indirect_records.push_back({&use, count, request});
            }
            SM50_RT_DISPATCH_ARGUMENTS ray_arguments{};
            struct ShaderTableRead {
              uint64_t base;
              uint32_t stride, count;
              size_t request = SIZE_MAX;
            };
            std::array<ShaderTableRead, 4> shader_tables{};
            if (data->atomic64_ray) {
              memcpy(&ray_arguments, metadata[dispatch_request].result.bytes, sizeof(ray_arguments));
              shader_tables = {{{ray_arguments.RayGenerationRecord, 0, 1},
                  {ray_arguments.MissRecordBase, ray_arguments.MissRecordStride, ray_arguments.MissRecordCount},
                  {ray_arguments.HitGroupRecordBase, ray_arguments.HitGroupRecordStride, ray_arguments.HitGroupRecordCount},
                  {ray_arguments.CallableRecordBase, ray_arguments.CallableRecordStride, ray_arguments.CallableRecordCount}}};
              for (auto &table : shader_tables) {
                if (!table.base || !table.count) continue;
                const uint64_t record_bytes = table.stride ? table.stride : sizeof(OwnedShaderIdentifier);
                if (record_bytes < sizeof(OwnedShaderIdentifier)) {
                  device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                }
                table.request = append_atomic_address(records, table.base, uint64_t(table.count) * record_bytes);
                if (table.request == SIZE_MAX) return;
              }
            }
            if (!read_atomic_batch(records, "records", data->id)) return;
            for (const auto &resolved : indirect_records) {
              const auto &use = *resolved.use;
              const auto &command = *use.command;
              for (uint32_t slot = 0; slot < resolved.count; ++slot) {
                const auto *record = records[resolved.request].result.bytes + uint64_t(slot) * use.stride;
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
              capture_atomic_arguments(global, ray_heap,
                  global_request == SIZE_MAX ? nullptr : metadata[global_request].result.bytes,
                  resolved_roots, resolved_tables);
              struct LocalArguments { MTLD3D12RootSignature *signature; const uint8_t *bytes; size_t request; };
              std::vector<LocalArguments> arguments;
              for (const auto &table : shader_tables) {
                if (table.request == SIZE_MAX) continue;
                const uint64_t record_bytes = table.stride ? table.stride : sizeof(OwnedShaderIdentifier);
                for (uint32_t slot = 0; slot < table.count; ++slot) {
                  const auto *record = records[table.request].result.bytes + uint64_t(slot) * record_bytes;
                  OwnedShaderIdentifier identifier; memcpy(&identifier, record, sizeof(identifier));
                  if (identifier.magic != kOwnedShaderIdentifierMagic || identifier.version != kOwnedShaderIdentifierVersion ||
                      identifier.kind != kOwnedShaderIdentifierPipeline || identifier.pipeline_token != ray_arguments.PipelineToken) continue;
                  const auto *exported = ray_state->FindExport(identifier.export_index);
                  if (!exported || !exported->local_root_signature) continue;
                  auto *local = exported->local_root_signature;
                  const uint64_t local_bytes = uint64_t(local->ParameterQwords) * 8;
                  if (local_bytes <= record_bytes - sizeof(identifier))
                    arguments.push_back({local, record + sizeof(identifier), SIZE_MAX});
                  else {
                    const uint64_t offset = uint64_t(slot) * table.stride + sizeof(identifier);
                    if (table.base > UINT64_MAX - offset) {
                      device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR); return;
                    }
                    const auto request = append_atomic_address(local_roots, table.base + offset, local_bytes);
                    if (request == SIZE_MAX) return;
                    arguments.push_back({local, nullptr, request});
                  }
                }
              }
              // Local layouts are selected by actual GPU-produced identifiers.
              // Read only missing tails, then retain the original record order.
              if (!read_atomic_batch(local_roots, "local_roots", data->id)) return;
              for (const auto &local : arguments)
                capture_atomic_arguments(local.signature, ray_heap,
                    local.request == SIZE_MAX ? local.bytes : local_roots[local.request].result.bytes,
                    resolved_roots, resolved_tables);
              swizzle_uses = CollectStandardSwizzleTables(resolved_tables);
            }
            const auto &roots = data->atomic64_indirect.empty() && !data->atomic64_ray ? data->atomic64_roots : resolved_roots;
            const auto &tables = data->atomic64_ray ? resolved_tables : data->swizzle_tables;
            // Parsing is complete; release readback storage before allocating
            // the atomic phase arena. No captured binding refers to these bytes.
            metadata.clear(); records.clear(); local_roots.clear();
            const ShaderImmutableConstants *private_constants = nullptr;
            if (data->atomic64_constants && !data->atomic64_constants->atomic64_relocations.empty() &&
                data->atomic64_indirect.empty() && !data->atomic64_ray && data->cast_textures.empty() &&
                swizzle_uses.empty()) {
              bool direct = true;
              for (auto *command = static_cast<const wmtcmd_base *>(data->cmd_head.next.get_accessible_or_null());
                   command; command = static_cast<const wmtcmd_base *>(command->next.get_accessible_or_null()))
                if (command->type == WMTComputeCommandDispatchIndirect) direct = false;
              if (direct) private_constants = data->atomic64_constants;
            }
            const auto &head = *static_cast<const RecordingHead *>(list.entry);
            const auto *shape = replay_current_node ? &head.pass_shapes[replay_current_node->node_ordinal] : nullptr;
            if (!begin_atomic(participants, tables, roots, &swizzle_uses, private_constants, data->id, shape)) return;
            cmdbuf = scope->inflight.cmdbuf;
          }
          if (!data->atomic64_phase && !BeginStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          const bool concurrent_dispatches = replay_current_node && replay_current_node->concurrent_dispatches &&
              swizzle_uses.empty() && !data->atomic64_phase;
          auto encoder = cmdbuf.computeCommandEncoder(concurrent_dispatches);
          owned_graphics::Trace("native_compute_begin", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
              ",\"begin_count\":1,\"concurrent\":", concurrent_dispatches,
              ",\"native_context\":", encoder.handle,
              ",\"dispatches\":", data->dispatch_accesses.size(), ",\"proof_checks\":",
              replay_current_node ? replay_current_node->dispatch_proof_checks : 0, ",\"reason\":\"",
              replay_current_node ? replay_current_node->dispatch_serial_reason : "unresolved", "\"");
          if (native_fence_required) {
            encoder.waitForFence(fence_);
            owned_graphics::Trace("native_compute_wait", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"wait_count\":1");
          } else if (consumer_barrier_after) {
            encoder.barrierAfterQueueStages(consumer_barrier_after, WMTMetal4StageDispatch, consumer_visibility);
            owned_graphics::Trace("native_compute_queue_barrier", ",\"submission\":", scope->seq, ",\"encoder\":", data->id,
                ",\"after\":", consumer_barrier_after, ",\"before\":", uint64_t(WMTMetal4StageDispatch), ",\"barrier_count\":1");
          }
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
          if (data->atomic64_constants && data->atomic64_constants->buffer) {
            wmtcmd_compute_setbuffer constants{};
            constants.type = WMTComputeCommandSetBuffer;
            constants.buffer = atomic.private_context ? atomic.phase->ConstantsBuffer()
                                                     : WMT::Buffer(data->atomic64_constants->buffer);
            constants.offset = atomic.private_context ? atomic.phase->ConstantsOffset() : data->atomic64_constants->offset;
            constants.index = SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS;
            encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&constants));
          }
          encoder.encodeCommands(&data->cmd_head);
          owned_graphics::Trace("native_compute_encode", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"encode_count\":1");
          encoder.updateFence(fence_);
          owned_graphics::Trace("native_compute_update", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"update_count\":1");
          encoder.endEncoding();
          owned_graphics::Trace("native_compute_end", ",\"submission\":", scope->seq, ",\"encoder\":", data->id, ",\"end_count\":1");
          if (data->atomic64_phase) {
            if (!end_atomic(&swizzle_uses)) return;
          } else if (!EndStandardSwizzlePass(cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          break;
        }
        case EncoderType::WorkGraph:
        case EncoderType::CaptureFrontier:
        case EncoderType::FenceBarrier:
          break; // Executed at the submission segment boundary above.
        case EncoderType::Temporal: {
          auto data = static_cast<TemporalEncoderData *>(current);
          auto &feature_submission = data->fg_frame ? OwnedFrameGenerationSubmissionMutex(data->fg_frame)
              : data->rr_frame ? OwnedRayRegenerationSubmissionMutex(data->rr_frame)
              : OwnedTemporalSubmissionMutex(data->frame);
          if (!scope->temporal_submission_locks.TryAcquire(feature_submission)) {
            // Commit all earlier work and release its feature reservations before
            // waiting. A competing queue can now publish the event we consume.
            scope.reset();
            if (!start_segment()) return;
            scope->temporal_submission_locks.AcquireFirst(feature_submission);
            owned_graphics::Trace("temporal_feature_contention_split", ",\"queue\":", owned_id_,
                ",\"submission\":", scope->seq);
          }

          // A feature last evaluated by another queue waits for its history
          // timeline at queue level. That wait must precede only this feature
          // commit, not application work already encoded into the prior segment.
          // A same-queue predecessor needs neither the wait nor its own command
          // buffer: a consumer queue barrier below orders this encode after it.
          // Only this feature is reserved; unrelated features on other queues
          // may encode concurrently. Re-read history after a prefix split since
          // that split releases all reservations before reserving ring capacity.
          const auto history = [&] {
            return data->fg_frame ? OwnedFrameGenerationFrameHistoryQueue(data->fg_frame)
                : data->rr_frame ? OwnedRayRegenerationFrameHistoryQueue(data->rr_frame)
                : OwnedTemporalFrameHistoryQueue(data->frame);
          };
          uint64_t history_queue = history();
          if (history_queue && history_queue != owned_id_) {
            scope.reset();
            if (!start_segment()) return;
            scope->temporal_submission_locks.AcquireFirst(feature_submission);
            history_queue = history();
          }
          const bool cross_queue_history = history_queue && history_queue != owned_id_;
          auto temporal_cmdbuf = scope->inflight.cmdbuf;
          if (data->fg_frame) scope->inflight.frame_generation_frames.push_back(data->fg_frame);
          else if (data->rr_frame) scope->inflight.ray_regeneration_frames.push_back(data->rr_frame);
          else scope->inflight.temporal_frames.push_back(data->frame);
          std::vector<SwizzlePassResource> swizzle_uses;
          if (device_->HasStandardSwizzleTextures()) {
            if (data->fg_frame) {
              const auto resources = GetOwnedFrameGenerationResources(data->fg_frame);
              for (size_t i = 0; i < resources.size(); ++i)
                if (resources[i] && resources[i]->swizzle_layout)
                  AppendStandardResource(swizzle_uses, Com<MTLD3D12Resource, false>(resources[i]),
                      IsOwnedFrameGenerationOutput(data->fg_frame, i));
            } else if (data->rr_frame) {
              const auto resources = GetOwnedRayRegenerationResources(data->rr_frame);
              for (size_t i = 0; i < resources.size(); ++i)
                if (resources[i] && resources[i]->swizzle_layout)
                  AppendStandardResource(swizzle_uses, Com<MTLD3D12Resource, false>(resources[i]),
                      IsOwnedRayRegenerationOutput(i));
            } else {
              const auto resources = GetOwnedTemporalFrameResources(data->frame);
              for (size_t i = 0; i < resources.size(); ++i)
                if (resources[i] && resources[i]->swizzle_layout)
                  AppendStandardResource(swizzle_uses, Com<MTLD3D12Resource, false>(resources[i]), i == 3);
            }
          }
          if (!BeginStandardSwizzlePass(temporal_cmdbuf, scope->inflight, swizzle_uses)) {
            device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
            return;
          }
          // Recording the timeline signal must not fail after the native encode.
          scope->signals.reserve(scope->signals.size() + 1);
          if (history_queue == owned_id_) {
            // The skipped timeline wait ordered this encode after all earlier queue
            // work, including internal MetalFX encoders of the previous evaluation
            // that fence_ does not cover. A consumer queue barrier keeps that order
            // on the GPU without a commit or event.
            auto order = temporal_cmdbuf.computeCommandEncoder();
            order.barrierAfterQueueStages(WMTMetal4StageAll,
                WMTMetal4StageDispatch | WMTMetal4StageBlit | WMTMetal4StageAccelerationStructure,
                WMTMetal4VisibilityDevice);
            order.endEncoding();
            owned_graphics::Trace("temporal_history_queue_barrier", ",\"queue\":", owned_id_,
                ",\"submission\":", scope->seq, ",\"encoder\":", current->id, ",\"barrier_count\":1");
          }
          const auto wait = [&](WMT::Event event, uint64_t value) { queue_.waitForEvent(event, value); };
          // The timeline is signaled after the commit of the command buffer that
          // holds this encode, partial or not. Cross-queue history commits here so
          // the next waiter is released promptly; a same-queue encode rides the
          // commit of the current segment.
          const auto signal = [&](WMT::Event event, uint64_t value) {
            scope->signals.emplace_back(event, value);
            if (cross_queue_history) scope.reset();
          };
          if (data->fg_frame)
            EncodeOwnedFrameGenerationFrame(data->fg_frame, temporal_cmdbuf, fence_, owned_id_, wait, signal);
          else if (data->rr_frame)
            EncodeOwnedRayRegenerationFrame(data->rr_frame, temporal_cmdbuf, fence_, owned_id_, wait, signal);
          else EncodeOwnedTemporalFrame(data->frame, temporal_cmdbuf, fence_, owned_id_, scope->seq, current->id, wait, signal);
          // Start the next segment only after the encode mutex is released. No
          // feature reservation is held during the ring's capacity wait.
          if (cross_queue_history && !start_segment())
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
          encoder.setLabel(resolve_pass_label_);
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
            if (!sparse_publication && publication.heap_ranges &&
                !ReservedResource::HeapProofCurrent(publication.heap_proof)) {
              sparse_publication = true;
              owned_graphics::Trace("sparse_publication_invalidated", ",\"queue\":", owned_id_,
                  ",\"list\":", list.list_id, ",\"mapping_generation\":", publication.heap_proof.generation);
            }
            if (!sparse_publication && !reserved_volume_publish_pending_) {
              owned_graphics::Trace("sparse_barrier_elided", ",\"queue\":", owned_id_,
                  ",\"list\":", list.list_id, ",\"encoder\":", current->id,
                  ",\"mapping_generation\":", publication.heap_proof.generation,
                  ",\"heap_ranges\":", publication.heap_ranges);
              break;
            }
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
    emit_clear_batches(plan.tail_clears);
    // A skipped fence wait stays outstanding across this Execute: the next
    // fence waiter (a later replay node or Present) drains it first.
    if (scope) SealInflightAccesses(scope->seq);
    if (fenced && !task.lists.empty()) {
      const auto &last = task.lists.back();
      const auto count = static_cast<RecordingHead *>(last.entry)->state_events.size();
      if (!validate_transitions(&last, count, count, true)) return;
    }
    // Signals queued directly behind this Execute retire with its last command
    // buffer instead of an empty submission; completion order is unchanged.
    // Signals enqueued while it encoded fold too when the earlier lookahead
    // found no other task in between: only immediately adjacent signals are
    // taken, and the first other task stays next in FIFO order.
    if (folded_signals && pending_task && !*pending_task && scope && !cancel_unsubmitted_ &&
        device_->RemovedReason() == S_OK) {
      QueueTask next;
      while (task_fifo_.tryPop(next)) {
        if (auto *next_signal = std::get_if<SignalTask>(&next)) {
          folded_signals->push_back(std::move(*next_signal));
        } else {
          *pending_task = std::move(next);
          break;
        }
      }
    }
    if (folded_signals && scope && device_->RemovedReason() == S_OK) {
      auto &completions = scope->inflight.fence_completions;
      completions.reserve(completions.size() + folded_signals->size());
      for (auto &signal : *folded_signals) {
        owned_graphics::Trace("signal", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                              ",\"fence\":", signal.fence_id, ",\"value\":", signal.ticket.value,
                              ",\"generation\":", signal.ticket.generation, ",\"folded\":1");
        owned_graphics::Trace("signal_committed", ",\"queue\":", owned_id_, ",\"submission\":", scope->seq,
                              ",\"fence\":", signal.fence_id, ",\"value\":", signal.ticket.value,
                              ",\"generation\":", signal.ticket.generation, ",\"folded\":1");
        completions.push_back(std::move(signal));
      }
      folded_signals->clear();
    }
    if (scope && scope->seq == first_segment && !fenced && device_->RemovedReason() == S_OK)
      scope->publish_swizzle_carry = true;
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
      std::lock_guard<d3dalloy::mutex> enqueue_lock(mutex_enqueue_);
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
        if (device_->diagnostics.CaptureEnabled()) {
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
              if (encoder->type == EncoderType::CaptureFrontier) history.push_back(D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT);
            }
          }
          history.push_back(D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION);
          submission.breadcrumb = device_->diagnostics.BeginSubmission(list, this, std::move(history), contexts);
        }
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
      std::lock_guard<d3dalloy::mutex> lock(mutex_enqueue_);
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
  ProcessSignalBatch(std::vector<SignalTask> &tasks) {
    if (device_->RemovedReason() != S_OK)
      return;
    struct SignalTrace {
      uint64_t fence, value, generation;
    };
    std::vector<SignalTrace> traces;
    uint64_t submission = 0;
    try {
      if (owned_graphics::TraceEnabled()) {
        traces.reserve(tasks.size());
        for (const auto &task : tasks)
          traces.push_back({task.fence_id, task.ticket.value, task.ticket.generation});
      }
      {
        CommittingScope scope(this, false, {}, device_->HasStandardSwizzleTextures());
        LoadSwizzleCarry(scope.inflight);
        scope.publish_swizzle_carry = scope.inflight.swizzle_predecessor != 0;
        submission = scope.seq;
        // This empty buffer changes no texture. Carry its proofs across the
        // signal, whose logical value is published only on validated retirement. Reserving a
        // shared native token here would confuse enqueue and completion order.
        for (const auto &task : tasks) {
          owned_graphics::Trace("signal", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                                ",\"fence\":", task.fence_id, ",\"value\":", task.ticket.value,
                                ",\"generation\":", task.ticket.generation);
        }
        scope.inflight.fence_completions = std::move(tasks);
      }
    } catch (const std::bad_alloc &) {
      ERR("CommandQueue::Signal: out of memory preparing native submission");
      device_->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
      return;
    }
    for (const auto &trace : traces)
      owned_graphics::Trace("signal_committed", ",\"queue\":", owned_id_, ",\"submission\":", submission,
                            ",\"fence\":", trace.fence, ",\"value\":", trace.value,
                            ",\"generation\":", trace.generation);
  }

  void
  ProcessSignal(SignalTask &task) {
    try {
      std::vector<SignalTask> tasks;
      tasks.push_back(std::move(task));
      ProcessSignalBatch(tasks);
    } catch (const std::bad_alloc &) {
      device_->ReportRemoval(E_OUTOFMEMORY);
    }
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
      std::lock_guard<d3dalloy::mutex> enqueue_lock(mutex_enqueue_);
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
    // Fixture-only observation of this FIFO worker's arrival. Read each time:
    // independent cases replace the name after draining their queues.
    wchar_t arrival_name[256];
    const auto arrival_length = GetEnvironmentVariableW(
        L"OWNED_GRAPHICS_QUEUE_WAIT_ARRIVAL", arrival_name, std::size(arrival_name));
    if (arrival_length && arrival_length < std::size(arrival_name)) {
      if (auto arrival = OpenEventW(EVENT_MODIFY_STATE, FALSE, arrival_name)) {
        SetEvent(arrival);
        CloseHandle(arrival);
      }
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
      std::lock_guard<d3dalloy::mutex> enqueue_lock(mutex_enqueue_);
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
    return d3dalloy::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain, nullptr, nullptr,
                                D3DalloySwapChainKind::Hwnd);
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainWithOutput(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput,
      IDXGISwapChain1 **ppSwapChain, IUnknown *core_window, D3DalloySwapChainKind kind
  ) override {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return d3dalloy::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain,
                                 pRestrictToOutput, core_window, kind);
  }

  void
  ProcessPresent(PresentTask &task, std::vector<SignalTask> *folded_signals = nullptr) {
    if (device_->RemovedReason() != S_OK) {
      CancelPresent(task);
      return;
    }
    // Native present now happens at commit (D3DMetal 4.0b3 DoPresent), so a
    // frame from another queue must be committed or skipped before this one;
    // the FIFO already orders frames of one queue. Waiting before the epoch
    // check keeps the frame restartable until it can actually commit.
    task.presenter->waitPresentEncoded(task.state->frame_id - 1);
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
      } else {
        try {
          auto scope = StartCommitting();
          if (device_->RemovedReason() != S_OK) {
            CancelPresent(task);
            return;
          }
          auto event = task.presenter->presentOrderEvent();
          const uint64_t frame_id = task.state->frame_id;
          scope.signals.emplace_back(event, frame_id);
          scope.inflight.present_completion = task.presenter;
          // Encoding a predecessor does not mean its drawable/history writes
          // have completed. Preserve the event chain for a skipped frame too;
          // a CPU signal here could release another queue while those writes
          // are still running. This marker acquires no drawable and adds no
          // CPU completion wait. Its Presenter owner retires with the marker.
          queue_.waitForEvent(event, frame_id - 1);
          owned_graphics::Trace("present_skip_order", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
              ",\"frame\":", frame_id, ",\"wait_value\":", frame_id - 1, ",\"signal_value\":", frame_id,
              ",\"gpu_timeline\":1");
        } catch (const std::bad_alloc &) {
          device_->ReportRemoval(E_OUTOFMEMORY);
          CancelPresent(task);
          return;
        }
      }
      task.state.reset();
      if (task.completion_handle && task.credit->exchange(false, std::memory_order_acq_rel))
        task.latency->complete(task.completion_handle.get());
      return;
    }
    try {
      EncodePresent(task, folded_signals);
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
  EncodePresent(PresentTask &task, std::vector<SignalTask> *folded_signals) {
    // Order presents from different queues on the GPU timeline, as D3DMetal
    // 4.0b3 DoPresent does: frame N's commands wait for N-1's signal without
    // blocking on GPU completion. A canceled order signals UINT64_MAX.
    auto order_event = task.presenter->presentOrderEvent();
    const uint64_t frame_id = task.state->frame_id;
    queue_.waitForEvent(order_event, frame_id - 1);
    EncodePresentCommitted(task, order_event, frame_id, folded_signals);
    // The native present ran in the scope destructor's commit; publish encoding
    // only now, so a later frame on another queue presents after this one.
    task.state.reset();
  }

  void
  EncodePresentCommitted(PresentTask &task, WMT::SharedEvent order_event, uint64_t frame_id,
                         std::vector<SignalTask> *folded_signals) {
    auto scope = StartCommitting();
    if (device_->RemovedReason() != S_OK) {
      CancelPresent(task);
      return;
    }
    if (folded_signals && !folded_signals->empty()) {
      // Published on validated retirement of this command buffer, like any
      // queue signal; device removal before then suppresses them alike.
      for (const auto &signal : *folded_signals) {
        owned_graphics::Trace("signal", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                              ",\"fence\":", signal.fence_id, ",\"value\":", signal.ticket.value,
                              ",\"generation\":", signal.ticket.generation, ",\"folded\":1");
        owned_graphics::Trace("signal_committed", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                              ",\"fence\":", signal.fence_id, ",\"value\":", signal.ticket.value,
                              ",\"generation\":", signal.ticket.generation, ",\"folded\":1");
      }
      scope.inflight.fence_completions = std::move(*folded_signals);
      folded_signals->clear();
    }
    auto &cmdbuf = scope.inflight.cmdbuf;
    // Present reads the backbuffer outside planner tracking.
    MarkUntracked(scope.seq);
    // Its encoders wait on the queue fence; skipped work of earlier Executes drains first.
    DrainSkippedWork(cmdbuf, scope.seq);
    scope.inflight.semaphore = task.completion_handle.release();
    scope.inflight.reflex_work = task.reflex_work;
    if (task.reflex_work) scope.inflight.reflex_driver_start = ReflexState::NowUs();
    scope.inflight.latency = std::move(task.latency);

    auto g = task.backbuffer.ptr();
    auto &view = g->texture->view(g->texture->fullView);

    WMT::Texture present_source = view.texture;
    const auto &composition = task.composition;
    // Dirty rects describe the backbuffer, not the cropped/scaled drawable.
    // Public validation has bounded every rect to these dimensions. A whole
    // backbuffer dirty rect overwrites every previous/scroll texel, so neither
    // history restoration nor copying that same result back is necessary.
    const bool full_image_dirty = composition.history && !composition.full &&
        std::any_of(composition.dirty_rects.begin(), composition.dirty_rects.end(), [&](const RECT &rect) {
          return rect.left == 0 && rect.top == 0 && uint64_t(rect.right) == composition.width &&
                 uint64_t(rect.bottom) == composition.height;
        });
    const bool full_history = composition.full || !composition.has_previous || full_image_dirty;
    uint64_t history_copies = 0, history_texels = 0, backbuffer_copies = 0, backbuffer_texels = 0;
    if (task.composition.history) {
      auto &current = composition.history->textures[0];
      auto &scroll_snapshot = composition.history->textures[1];
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.waitForFence(fence_);
      auto copy = [&](WMT::Texture source, WMT::Texture destination, int64_t source_x, int64_t source_y,
                      int64_t width, int64_t height, int64_t destination_x, int64_t destination_y) {
        wmtcmd_blit_copy_from_texture_to_texture command{};
        command.type = WMTBlitCommandCopyFromTextureToTexture;
        command.next.set(nullptr);
        command.src = source;
        command.src_origin = {uint32_t(source_x), uint32_t(source_y), 0};
        command.src_size = {uint32_t(width), uint32_t(height), 1};
        command.dst = destination;
        command.dst_origin = {uint32_t(destination_x), uint32_t(destination_y), 0};
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&command));
        ++history_copies;
        history_texels += uint64_t(width) * uint64_t(height);
      };
      if (full_history) {
        copy(view.texture, current, 0, 0, composition.width, composition.height, 0, 0);
      } else {
        // Pixels outside the update already reside in current. No whole-image
        // history restore is needed. A scroll reads the previous frame, so keep
        // just its clipped source before modifying an overlapping destination.
        // The native blit tracker orders these read/write hazards, including
        // overlapping dirty rectangles, within this encoder.
        if (composition.scroll && (composition.scroll_offset.x || composition.scroll_offset.y)) {
          const auto &rect = composition.scroll_rect;
          const int64_t offset_x = composition.scroll_offset.x;
          const int64_t offset_y = composition.scroll_offset.y;
          const int64_t left = std::max<int64_t>(rect.left, -offset_x);
          const int64_t top = std::max<int64_t>(rect.top, -offset_y);
          const int64_t right = std::min<int64_t>(rect.right, int64_t(composition.width) - offset_x);
          const int64_t bottom = std::min<int64_t>(rect.bottom, int64_t(composition.height) - offset_y);
          if (right > left && bottom > top) {
            copy(current, scroll_snapshot, left, top, right - left, bottom - top, 0, 0);
            copy(scroll_snapshot, current, 0, 0, right - left, bottom - top, left + offset_x, top + offset_y);
          }
        }
        for (const RECT &rect : composition.dirty_rects)
          copy(view.texture, current, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
               rect.left, rect.top);
      }
      encoder.updateFence(fence_);
      encoder.endEncoding();
      present_source = current;
      scope.inflight.present_history = composition.history;
    }
    Presenter::PresentWork present_work;
    auto drawable = task.presenter->encodeCommandsMetal4(cmdbuf, present_source, task.state->metadata, fence_,
                                                        present_work);
    owned_graphics::Trace("present_output_work", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
        ",\"frame\":", frame_id, ",\"identity_copy\":", present_work.identity_copy,
        ",\"pq_code_copy\":", present_work.pq_code_copy,
        ",\"render_passes\":", drawable && !present_work.identity_copy ? 1 : 0,
        ",\"width\":", present_work.width, ",\"height\":", present_work.height);
    if (task.composition.history && !full_history) {
      // DXGI flip-sequential updates the current backbuffer before swapping,
      // not just the visible drawable. Keep application readback and later
      // incremental updates consistent with the composed presentation.
      std::array<wmtcmd_blit_copy_from_texture_to_texture, 4> spans{};
      auto encoder = cmdbuf.blitCommandEncoder();
      encoder.waitForFence(fence_);
      auto restore = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        if (!w || !h) return;
        assert(backbuffer_copies < spans.size());
        auto &copy = spans[backbuffer_copies];
        copy.type = WMTBlitCommandCopyFromTextureToTexture;
        copy.src = present_source;
        copy.dst = view.texture;
        copy.src_origin = copy.dst_origin = {x, y, 0};
        copy.src_size = {w, h, 1};
        if (backbuffer_copies) spans[backbuffer_copies - 1].next.set(&copy);
        ++backbuffer_copies;
        backbuffer_texels += uint64_t(w) * h;
      };
      const auto w = composition.width, h = composition.height;
      const auto *rect = composition.dirty_rects.size() == 1 ? &composition.dirty_rects.front() : nullptr;
      // Dirty pixels came from this exact public buffer, *after* scroll. They
      // already equal history. Restore everything else (including pixels
      // outside SetSourceSize), bounded to at most four nonempty blits.
      if (rect && uint64_t(rect->right - rect->left) * (rect->bottom - rect->top) * 2 >= uint64_t(w) * h) {
        const uint32_t l = rect->left, t = rect->top, r = rect->right, b = rect->bottom;
        restore(0, 0, w, t);
        restore(0, b, w, h - b);
        restore(0, t, l, b - t);
        restore(r, t, w - r, b - t);
      } else {
        restore(0, 0, w, h);
      }
      if (backbuffer_copies)
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(spans.data()));
      encoder.updateFence(fence_);
      encoder.endEncoding();
    }
    if (composition.history)
      owned_graphics::Trace("present_history_work", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
          ",\"frame\":", frame_id, ",\"full_image_dirty\":", full_image_dirty,
          ",\"history_copies\":", history_copies, ",\"history_texels\":", history_texels,
          ",\"backbuffer_copies\":", backbuffer_copies,
          ",\"backbuffer_texels\":", backbuffer_texels);
    queue_.waitForDrawable(drawable);
    scope.signals.emplace_back(order_event, frame_id);
    if (task.observer && !drawable.observePresentation(task.observer, task.present_id)) {
      task.observer.cancel(task.present_id);
      task.observer_fault->store(true, std::memory_order_release);
    }

    // Core Animation receives the drawable right after this command buffer commits; the
    // signaled drawable makes the display wait for the GPU (D3DMetal 4.0b3 DoPresent). A
    // DXGI_PRESENT_RESTART or GPU error after commit no longer withdraws the frame.
    scope.inflight.native_present.emplace(InflightCommandBuffer::NativePresent{
        drawable, task.presenter->restartToken(), task.observer, task.backbuffer,
        std::move(task.composition.surface), task.composition.frame,
        frame_id, task.restart_epoch, task.present_id, task.after, task.composition.sync_interval,
        task.composition.allow_tearing});
    scope.inflight.native_present->native_present_attempted = true;
    scope.inflight.native_present->native_present_reserved = WMT4::ReserveMetal4Present(
        cmdbuf, drawable, task.after,
        scope.inflight.native_present->surface ? &scope.inflight.native_present->frame : nullptr,
        task.composition.sync_interval, scope.inflight.native_present->surface ? task.composition.allow_tearing : false);
    if (!scope.inflight.native_present->native_present_reserved)
      device_->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    owned_graphics::Trace("present_encode", ",\"queue\":", owned_id_, ",\"submission\":", scope.seq,
                          ",\"resource\":", g->owned_id);
    // Encoding completion releases PSO readers. Layer transitions additionally
    // wait for the completion thread to retire this command buffer.
    scope.inflight.present_completion = std::move(task.presenter);
  }

  HRESULT
  Present(Presenter *presenter, ID3D12Resource *backbuffer, HANDLE hLantecyWaitable, double after,
          const D3DalloyPresentGeometry &geometry, WMT::Reference<WMT::PresentationObserver> observer,
          uint64_t present_id, std::shared_ptr<std::atomic<bool>> observer_fault,
          D3DalloyPresentComposition composition, bool do_not_wait,
          std::shared_ptr<D3DalloyPresentLatency> latency, bool restart, bool borrowed, bool closes_frame,
          std::shared_ptr<owned_graphics::LowLatencyScheduler::Frame> *latency_frame) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (!presenter || !backbuffer || !latency)
      return E_INVALIDARG;
    // Only the safety bound applies before enqueue. Adaptive force pacing belongs
    // after successful Present, before the game can collect the next frame's input.
    ReflexState::PresentReservation latency_reservation;
    if (HRESULT hr = device_->reflex.ReservePresent(do_not_wait, latency_reservation); FAILED(hr)) return hr;
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

      std::unique_lock<d3dalloy::mutex> enqueue_lock(mutex_enqueue_, std::defer_lock);
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
      auto capture = device_->reflex.CapturePresent(closes_frame);
      task.reflex_work = std::move(capture.work);
      *latency_frame = std::move(capture.frame);
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

} // namespace d3dalloy
