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

#include "dxmt_buffer.hpp"
#include "dxmt_format.hpp"
#include "thread.hpp"
#include "util_likely.hpp"
#include "util_math.hpp"
#include "wsi_platform.hpp"
#include <cassert>
#include <mutex>

namespace dxmt {

std::atomic_uint64_t global_buffer_seq = {0};

BufferAllocation::BufferAllocation(WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags) :
    info_(info),
    flags_(flags) {
  // (sub)allocate a minimum of 256B buffer so that texture can be created
  info_.length = std::max(info_.length, 256ull);
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage) && suballocation_size_ <= DXMT_PAGE_SIZE) {
    suballocation_size_ = align(info_.length, 16);
    suballocation_count_ = DXMT_PAGE_SIZE / suballocation_size_;
    info_.length = DXMT_PAGE_SIZE;
  }
  fenceTrackers.resize(suballocation_count_);
  if (flags_.test(BufferAllocationFlag::CpuPlaced) && !info_.memory.get_accessible_or_null()) {
    placed_buffer = wsi::aligned_malloc(info_.length, DXMT_PAGE_SIZE);
    info_.memory.set(placed_buffer);
  }
  obj_ = device.newBuffer(info_);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
};

BufferAllocation::BufferAllocation(
    WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, WMT::Heap heap,
    uint64_t heap_offset
) :
    info_(info),
    flags_(flags) {
  info_.length = info_.length;
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage)) {
    assert(0 && "heap-backed buffer doesn't support suballocation");
  }
  fenceTrackers.resize(suballocation_count_);
  if (flags_.test(BufferAllocationFlag::CpuPlaced)) {
    assert(0 && "heap doesn't support placed buffer");
  }
  obj_ = heap.newBuffer(info_, heap_offset);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = info_.memory.get_accessible_or_null();
}

BufferAllocation::BufferAllocation(
    WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags, WMTSparsePageSize page_size
) :
    info_(info),
    flags_(flags),
    sparse_page_size_(page_size) {
  suballocation_size_ = info_.length;
  fenceTrackers.resize(suballocation_count_);
  obj_ = device.newPlacementSparseBuffer(info_, page_size);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = nullptr;
}

void
BufferAllocation::free() {
  if (placed_buffer) {
    wsi::aligned_free(placed_buffer);
    placed_buffer = nullptr;
  }
  delete this;
}

WMT::Texture
Buffer::view(BufferViewKey key) {
  return view(key, current_.ptr());
};

WMT::Texture
Buffer::view(BufferViewKey key, BufferAllocation *allocation) {
  return view_(key, allocation).texture;
};

BufferView const &
Buffer::view_(BufferViewKey key) {
  return view_(key, current_.ptr());
};

BufferView const &
Buffer::view_(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key.index];
};

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key) {
  return residency(key, current_.ptr());
}

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return allocation->cached_view_[key.index]->residency;
}

void
Buffer::prepareAllocationViews(BufferAllocation *allocation) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  for (unsigned version = allocation->version_; version < version_; version++) {
    const auto &descriptor = viewDescriptors_[version];
    auto format = descriptor.format;
    auto texel_size = MTLGetTexelSize(format);
    assert(texel_size);
    assert(!(allocation->suballocation_size_ & (texel_size - 1)));
    auto total_length = allocation->suballocation_size_ * allocation->suballocation_count_;
    const uint64_t view_offset = descriptor.byteOffset;
    const uint64_t view_length = descriptor.byteLength ? descriptor.byteLength : total_length;
    assert(view_offset <= total_length && view_length <= total_length - view_offset);
    WMTTextureInfo info;
    info.type = WMTTextureTypeTextureBuffer;
    info.width = view_length / (uint64_t)texel_size;
    info.height = 1;
    info.depth = 1;
    info.array_length = 1;
    info.mipmap_level_count = 1;
    info.sample_count = 1;
    info.pixel_format = format;
    info.options = allocation->info_.options;
    auto usage = WMTTextureUsageShaderRead;
    if (descriptor.writable ||
        (!allocation->flags().test(BufferAllocationFlag::GpuReadonly) &&
         (allocation->flags().test(BufferAllocationFlag::GpuManaged) ||
          allocation->flags().test(BufferAllocationFlag::GpuPrivate)))) {
      usage |= WMTTextureUsageShaderWrite;
      if (format == WMTPixelFormatR32Uint || format == WMTPixelFormatR32Sint ||
          (format == WMTPixelFormatRG32Uint && device_.supportsFamily(WMTGPUFamilyApple8))) {
        usage |= WMTTextureUsageShaderAtomic;
      }
    }
    info.usage = usage;

    /* A texel view of a placement sparse buffer must carry the buffer's page size, or it reads zero. */
    auto view = allocation->flags().test(BufferAllocationFlag::PlacementSparse)
                    ? allocation->obj_.newPlacementSparseTextureView(info, view_offset, view_length,
                                                                     allocation->sparse_page_size_)
                    : allocation->obj_.newTexture(info, view_offset, view_length);

    allocation->cached_view_.push_back(std::make_unique<BufferView>(
        std::move(view), info.gpu_resource_id, view_length / texel_size
    ));
  }
  allocation->version_ = version_;
};

BufferViewDescriptor
Buffer::viewDescriptor(BufferViewKey key) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  return viewDescriptors_[key.index];
}

BufferViewKey
Buffer::createView(BufferViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < version_; i++) {
    if (viewDescriptors_[i].format == descriptor.format &&
        viewDescriptors_[i].byteOffset == descriptor.byteOffset &&
        viewDescriptors_[i].byteLength == descriptor.byteLength &&
        viewDescriptors_[i].writable == descriptor.writable) {
      return BufferViewKey(i, std::nullopt);
    }
  }
  viewDescriptors_.push_back(descriptor);
  version_ = version_ + 1;
  return BufferViewKey(i, std::nullopt);;
}

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(BufferAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(BufferAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = options;
  return new BufferAllocation(device_, info, flags);
};

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, WMT::Heap heap, uint64_t heap_offset) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(BufferAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(BufferAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = options;
  flags.set(BufferAllocationFlag::AllocatedOnHeap);
  return new BufferAllocation(device_, info, flags, heap, heap_offset);
};

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags, void *placed_memory) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(BufferAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  WMTBufferInfo info;
  info.memory.set(placed_memory);
  info.length = length_;
  info.options = options;
  flags.set(BufferAllocationFlag::CpuPlaced);
  return new BufferAllocation(device_, info, flags);
};

Rc<BufferAllocation>
Buffer::allocateSparse(WMTSparsePageSize page_size) {
  WMTBufferInfo info;
  info.memory.set(0);
  info.length = length_;
  info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
  Flags<BufferAllocationFlag> flags;
  flags.set(BufferAllocationFlag::GpuPrivate);
  flags.set(BufferAllocationFlag::PlacementSparse);
  return new BufferAllocation(device_, info, flags, page_size);
};

Rc<BufferAllocation>
Buffer::rename(Rc<BufferAllocation> &&newAllocation) {
  Rc<BufferAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void
Buffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Buffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

} // namespace dxmt