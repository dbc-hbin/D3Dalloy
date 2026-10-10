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

#include "dxmt_texture.hpp"
#include "dxmt_format.hpp"
#include "dxmt_residency.hpp"
#include "util_hash.hpp"
#include "wsi_platform.hpp"
#include <atomic>
#include <cassert>

namespace dxmt {

std::atomic_uint64_t global_texture_seq = {0};

bool
TextureViewDescriptor::operator==(const TextureViewDescriptor &other) const {
  return format == other.format && type == other.type && firstMiplevel == other.firstMiplevel &&
         miplevelCount == other.miplevelCount && firstArraySlice == other.firstArraySlice &&
         arraySize == other.arraySize && msaaShadow == other.msaaShadow &&
         reservedVolumeRender == other.reservedVolumeRender && volumeFirstW == other.volumeFirstW &&
         volumeCastMip == other.volumeCastMip && castBlockSize == other.castBlockSize &&
         castSourceBlockWidth == other.castSourceBlockWidth && castCompressed == other.castCompressed;
}

size_t
TextureViewDescriptor::Hash::operator()(const TextureViewDescriptor &descriptor) const {
  HashState hash;
  hash.add(descriptor.format);
  hash.add(descriptor.type);
  hash.add(descriptor.firstMiplevel);
  hash.add(descriptor.miplevelCount);
  hash.add(descriptor.firstArraySlice);
  hash.add(descriptor.arraySize);
  hash.add(descriptor.msaaShadow);
  hash.add(descriptor.reservedVolumeRender);
  hash.add(descriptor.volumeFirstW);
  hash.add(descriptor.volumeCastMip);
  hash.add(descriptor.castBlockSize);
  hash.add(descriptor.castSourceBlockWidth);
  hash.add(descriptor.castCompressed);
  return hash;
}

void
TextureView::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
TextureView::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

TextureView::TextureView(TextureAllocation *allocation) :
    texture(allocation->texture()),
    gpuResourceID(allocation->gpuResourceID),
    allocation(allocation),
    key(allocation->descriptor->fullView) {}

TextureView::TextureView(TextureAllocation *allocation, unsigned index, TextureViewDescriptor descriptor) :
    gpuResourceID(0),
    allocation(allocation),
    key(descriptor, index, allocation->descriptor->miplevelCount()) {
  auto parent = allocation->texture();
  if (descriptor.reservedVolumeRender) {
    auto owner = allocation->descriptor;
    const auto *layout = owner->reservedVolumeLayoutWords;
    if (!layout) return;
    const auto page = owner->device_.sparseTileSize(WMTTextureType2DArray,
        ORIGINAL_FORMAT(descriptor.format), 1, WMTSparsePageSize64);
    if (!page.width || !page.height || page.depth != 1) return;
    uint32_t width = std::max(layout[0] >> descriptor.firstMiplevel, 1u);
    uint32_t height = std::max(layout[1] >> descriptor.firstMiplevel, 1u);
    if (descriptor.castBlockSize && !descriptor.castCompressed) {
      width = (width + 3) / 4; height = (height + 3) / 4;
    }
    reservedVolumeRender = std::make_unique<ReservedVolumeRenderView>();
    castBlockSize = descriptor.castBlockSize;
    auto &volume = *reservedVolumeRender;
    if (castBlockSize) {
      auto binding=owner->reservedVolumeCastBinding(owner->reservedVolumeCastContext,descriptor.firstMiplevel,
          MTLGetUnsignedIntegerFormat(ORIGINAL_FORMAT(descriptor.format)),false);
      if (!binding.atlas) return;
      castStorage=std::move(binding.atlas);
    }
    volume.mip = descriptor.firstMiplevel;
    volume.first_w = descriptor.volumeFirstW;
    volume.w_size = descriptor.arraySize;
    volume.page_w = page.width; volume.page_h = page.height;
    volume.pages_x = (width + page.width - 1) / page.width;
    volume.pages_y = (height + page.height - 1) / page.height;
    WMTTextureInfo info{};
    info.type = WMTTextureType2DArray;
    info.pixel_format = ORIGINAL_FORMAT(descriptor.format);
    info.width = volume.pages_x * page.width; info.height = volume.pages_y * page.height;
    info.depth = 1; info.array_length = descriptor.arraySize;
    info.mipmap_level_count = info.sample_count = 1;
    info.usage = WMTTextureUsageRenderTarget | WMTTextureUsagePixelFormatView;
    info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
    texture = owner->device_.newPlacementSparseTexture(info, WMTSparsePageSize64);
    gpuResourceID = info.gpu_resource_id;
    return;
  }
  if (allocation->descriptor->reservedVolumeLayoutAddress && descriptor.castBlockSize) {
    auto owner = allocation->descriptor;
    if (!owner->reservedVolumeCastBinding) return;
    auto binding = owner->reservedVolumeCastBinding(owner->reservedVolumeCastContext,
        descriptor.volumeCastMip, descriptor.castCompressed ? ORIGINAL_FORMAT(descriptor.format) :
        MTLGetUnsignedIntegerFormat(ORIGINAL_FORMAT(descriptor.format)), descriptor.castCompressed);
    if (!binding.atlas) return;
    reservedVolumeCastLayoutAddress = binding.layout_address;
    reservedVolumeAtlasCast = true;
    castBlockSize = descriptor.castBlockSize;
    castCompressed = descriptor.castCompressed;
    castStorage = binding.atlas;
    texture = binding.atlas.newTextureView(descriptor.format, WMTTextureType2DArray, 0, 1, 0,
        owner->arrayLength(), {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha},
        gpuResourceID);
    return;
  }
  if (descriptor.msaaShadow) {
    auto owner = allocation->descriptor;
    WMTBufferInfo ids{};
    ids.length = sizeof(uint64_t) * 8;
    ids.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
    msaaSampleIds = owner->device_.newBuffer(ids);
    if (!msaaSampleIds || !ids.memory.get_accessible_or_null()) return;
    auto entries = static_cast<uint64_t *>(ids.memory.get_accessible_or_null());
    const bool depth_cast = descriptor.castBlockSize && descriptor.castSourceBlockWidth == 1;
    if (depth_cast) {
      castBlockSize = descriptor.castBlockSize;
      castSourceBlockWidth = 1;
      std::fill_n(entries, 8, uint64_t(0));
    }
    for (unsigned sample = 0; sample < owner->sampleCount(); ++sample) {
      if (depth_cast) {
        WMTTextureInfo info{};
        info.pixel_format = ORIGINAL_FORMAT(descriptor.format);
        info.type = WMTTextureType2DArray;
        info.width = owner->width(); info.height = owner->height();
        info.depth = info.sample_count = info.mipmap_level_count = 1;
        info.array_length = descriptor.arraySize;
        info.usage = WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView;
        info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
        auto shadow = owner->device_.newTexture(info);
        if (!shadow) return;
        msaaViews[sample] = shadow.newTextureView(
            descriptor.format, WMTTextureType2DArray, 0, 1, 0, descriptor.arraySize,
            {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, entries[sample]);
      } else {
        msaaViews[sample] = allocation->msaaShadow[sample].newTextureView(
            descriptor.format, WMTTextureType2DArray, 0, 1, descriptor.firstArraySlice, descriptor.arraySize,
            {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, entries[sample]);
      }
      if (!msaaViews[sample]) return;
    }
    if (depth_cast) castStorage = msaaViews[0];
    msaaSampleAddress = ids.gpu_address;
    if (descriptor.type == WMTTextureType2DArray) {
      texture = msaaViews[0];
      gpuResourceID = entries[0];
      return;
    }
  }
  castBlockSize = descriptor.castBlockSize;
  castCompressed = descriptor.castCompressed;
  if (castBlockSize) {
    castSourceBlockWidth = descriptor.castSourceBlockWidth;
    auto owner = allocation->descriptor;
    WMTTextureInfo info{};
    info.pixel_format = ORIGINAL_FORMAT(descriptor.format);
    info.type = owner->textureType();
    info.width = (std::max(owner->width() >> key.mip_start, 1u) + castSourceBlockWidth - 1) / castSourceBlockWidth;
    info.height = (std::max(owner->height() >> key.mip_start, 1u) + castSourceBlockWidth - 1) / castSourceBlockWidth;
    info.depth = owner->depth(key);
    info.array_length = descriptor.arraySize;
    if (info.type == WMTTextureTypeCube || info.type == WMTTextureTypeCubeArray) info.array_length /= 6;
    info.mipmap_level_count = castSourceBlockWidth == 1 ? descriptor.miplevelCount : 1;
    info.sample_count = 1;
    info.usage = WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite |
                 WMTTextureUsageRenderTarget | WMTTextureUsagePixelFormatView;
    info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
    if (castCompressed) {
      info.type = owner->textureType();
      info.width = owner->width();
      info.height = owner->height();
      info.depth = owner->depth();
      info.array_length = owner->arrayLength();
      if (info.type == WMTTextureTypeCube || info.type == WMTTextureTypeCubeArray) info.array_length /= 6;
      info.mipmap_level_count = owner->miplevelCount();
      info.usage = WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView;
    }
    if (!castCompressed) {
      for (const auto &cached : allocation->cached_view_) {
        if (cached->castBlockSize == castBlockSize && !cached->castCompressed &&
            cached->castSourceBlockWidth == castSourceBlockWidth &&
            (castSourceBlockWidth != 1 ||
             (cached->key.mip_end == key.mip_end &&
              ORIGINAL_FORMAT(owner->pixelFormat(cached->key)) == info.pixel_format)) &&
            cached->key.mip_start == key.mip_start && cached->key.array_start == key.array_start &&
            cached->key.array_end == key.array_end) {
          castStorage = cached->castStorage;
          reservedCast = cached->reservedCast;
          break;
        }
      }
    }
    if (!castStorage && allocation->flags().test(TextureAllocationFlag::PlacementSparse)) {
      const auto page = owner->device_.sparseTileSize(info.type, info.pixel_format, 1, WMTSparsePageSize64);
      if (!page.width || !page.height || page.depth != 1) return;
      reservedCast = std::make_shared<ReservedCastStorage>();
      reservedCast->page_w = page.width; reservedCast->page_h = page.height;
      reservedCast->pages_x = (info.width + page.width - 1) / page.width;
      reservedCast->pages_y = (info.height + page.height - 1) / page.height;
      castStorage = owner->device_.newPlacementSparseTexture(info, WMTSparsePageSize64);
      if (!castStorage) return;
      const auto tail = castStorage.sparseTail();
      if (castCompressed) {
        const auto source_tail=allocation->texture().sparseTail();
        const auto source_page=owner->device_.sparseTileSize(info.type,owner->info_.pixel_format,1,WMTSparsePageSize64);
        if (source_page.width != page.width || source_page.height != page.height ||
            source_tail.first_mipmap_in_tail != tail.first_mipmap_in_tail ||
            source_tail.tail_size_in_bytes != tail.tail_size_in_bytes) return;
      } else if (tail.first_mipmap_in_tail == 0) {
        if (!tail.tail_size_in_bytes || tail.tail_size_in_bytes % 65536) return;
        reservedCast->packed = true;
        reservedCast->pages_x = uint32_t(tail.tail_size_in_bytes / 65536);
        reservedCast->pages_y = 1;
      }
    }
    if (!castStorage) castStorage = owner->device_.newTexture(info);
    if (!castStorage) return;
    parent = castStorage;
    if (!castCompressed) {
      descriptor.firstMiplevel = 0;
      descriptor.firstArraySlice = 0;
    }
  }
  texture = parent.newTextureView(
      descriptor.format, descriptor.type, descriptor.firstMiplevel, descriptor.miplevelCount,
      descriptor.firstArraySlice, descriptor.arraySize,
      {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, gpuResourceID
  );
}

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
    unsigned bytes_per_row, Flags<TextureAllocationFlag> flags
) :
    descriptor(descriptor),
    mappedMemory(mapped_buffer),
    buffer_(std::move(buffer)),
    flags_(flags) {
  auto info_copy = info;
  obj_ = buffer_.newTexture(info_copy, 0, bytes_per_row);

  gpuResourceID = info_copy.gpu_resource_id;
  machPort = 0;
  fenceTrackers.resize(
      flags.test(TextureAllocationFlag::ShaderReadonly) ? 1 : descriptor->arrayLength() * descriptor->miplevelCount()
  );
};

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
    Flags<TextureAllocationFlag> flags
) :
    descriptor(descriptor),
    obj_(std::move(texture)),
    flags_(flags) {
  mappedMemory = nullptr;
  gpuResourceID = textureDescriptor.gpu_resource_id;
  machPort = textureDescriptor.mach_port;
  if (obj_ && descriptor->sampleCount() > 1 && (descriptor->usage() & WMTTextureUsageShaderWrite)) {
    auto info = textureDescriptor;
    msaaFormat = MTLGetUnsignedIntegerFormat(info.pixel_format);
    if (info.pixel_format == WMTPixelFormatRG11B10Float || info.pixel_format == WMTPixelFormatRGB9E5Float)
      msaaFormat = WMTPixelFormatR32Uint;
    if (msaaFormat == WMTPixelFormatInvalid) msaaFormat = info.pixel_format;
    if (msaaFormat == info.pixel_format) {
      msaaCanonical = obj_;
    } else {
      uint64_t id;
      msaaCanonical = obj_.newTextureView(msaaFormat, info.type, 0, 1, 0, info.array_length,
          {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha}, id);
      if (!msaaCanonical) { obj_ = nullptr; return; }
    }
    // Synchronize raw integer views: typeless half/float aliases must retain every stored bit.
    info.pixel_format = msaaFormat;
    info.type = WMTTextureType2DArray;
    info.sample_count = 1;
    info.usage = WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite | WMTTextureUsagePixelFormatView;
    info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
    for (unsigned sample = 0; sample < descriptor->sampleCount(); ++sample) {
      msaaShadow[sample] = descriptor->device_.newTexture(info);
      msaaBaseline[sample] = descriptor->device_.newTexture(info);
      if (!msaaShadow[sample] || !msaaBaseline[sample]) { obj_ = nullptr; break; }
    }
  }
  fenceTrackers.resize(
      flags.test(TextureAllocationFlag::ShaderReadonly) ? 1 : descriptor->arrayLength() * descriptor->miplevelCount()
  );
};

void
TextureAllocation::free(){
#ifdef __i386__
  if (!placed_memory_)
    wsi::aligned_free(mappedMemory);
#endif
  delete this;
};

void
Texture::prepareAllocationViews(TextureAllocation *allocation) {
  for (;;) {
    unsigned version;
    TextureViewDescriptor descriptor;
    {
      std::shared_lock<dxmt::shared_mutex> lock(mutex_);
      version = allocation->version_;
      if (version == version_) break;
      descriptor = viewDescriptors_[version];
    }
    // A reserved-volume view may admit native mapping work. Never wait for
    // device execution admission while retaining the descriptor cache mutex.
    TextureViewRef prepared(version ? new TextureView(allocation, version, descriptor) : new TextureView(allocation));
    std::unique_lock<dxmt::shared_mutex> lock(mutex_);
    // Another admitted encoder may have prepared this view while a CPU
    // descriptor call waited for execution admission.
    if (allocation->version_ != version) continue;
    allocation->cached_view_.push_back(std::move(prepared));
    allocation->version_ = version + 1;
  }
}

TextureViewKey
Texture::createView(TextureViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::shared_mutex> lock(mutex_);
  if (auto entry = viewIndices_.find(descriptor); entry != viewIndices_.end())
    return TextureViewKey(descriptor, entry->second, info_.mipmap_level_count);
  const unsigned i = version_;
  viewDescriptors_.push_back(descriptor);
  try {
    viewIndices_.emplace(descriptor, i);
  } catch (...) {
    viewDescriptors_.pop_back();
    throw;
  }
  version_ = version_ + 1;
  return TextureViewKey(descriptor, i, info_.mipmap_level_count);
}

Texture::Texture(const WMTTextureInfo &descriptor, WMT::Device device) :
    info_(descriptor),
    device_(device) {

  viewDescriptors_.push_back({
      .format = ORIGINAL_FORMAT(info_.pixel_format),
      .type = info_.type,
      .firstMiplevel = 0,
      .miplevelCount = info_.mipmap_level_count,
      .firstArraySlice = 0,
      .arraySize = arrayLength(),
  });
  viewIndices_.emplace(viewDescriptors_[0], 0);
  version_ = 1;
  fullView = TextureViewKey(viewDescriptors_[0], 0, info_.mipmap_level_count);
}

Texture::Texture(
    unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &descriptor, WMT::Device device
) :
    info_(descriptor),
    bytes_per_image_(bytes_per_image),
    bytes_per_row_(bytes_per_row),
    device_(device) {

  assert(info_.type == WMTTextureType2D);
  assert(info_.mipmap_level_count == 1);
  assert(info_.array_length == 1);

  viewDescriptors_.push_back({
      .format = ORIGINAL_FORMAT(info_.pixel_format),
      .type = info_.type,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
  });
  viewIndices_.emplace(viewDescriptors_[0], 0);
  version_ = 1;
  fullView = TextureViewKey(viewDescriptors_[0], 0, info_.mipmap_level_count);
}

Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag> flags) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  WMTTextureInfo info = info_; // copy
  info.mach_port = 0;
  if (info.sample_count > 1) info.usage = static_cast<WMTTextureUsage>(info.usage & ~WMTTextureUsageShaderWrite);
  if (flags.test(TextureAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(TextureAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(TextureAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  info.options = options;
  if (bytes_per_image_) {
    WMTBufferInfo buffer_info;
    buffer_info.length = bytes_per_image_;
    buffer_info.options = options;
    buffer_info.memory.set(nullptr);
#ifdef __i386__
    buffer_info.memory.set(wsi::aligned_malloc(bytes_per_image_, DXMT_PAGE_SIZE));
#endif
    auto buffer = device_.newBuffer(buffer_info);
    return new TextureAllocation(this, std::move(buffer), buffer_info.memory.get(), info, bytes_per_row_, flags);
  }
  auto texture = flags.test(TextureAllocationFlag::Shared) ? device_.newSharedTexture(info) : device_.newTexture(info);
  return new TextureAllocation(this, std::move(texture), info, flags);
}

Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag> flags, void *placed_memory) {
  if (!bytes_per_image_ || !placed_memory)
    return nullptr;
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(TextureAllocationFlag::CpuWriteCombined))
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  WMTTextureInfo info = info_; // copy
  info.mach_port = 0;
  info.options = options;
  WMTBufferInfo buffer_info;
  buffer_info.length = bytes_per_image_;
  buffer_info.options = options;
  buffer_info.memory.set(placed_memory);
  auto buffer = device_.newBuffer(buffer_info);
  if (!buffer)
    return nullptr;
  auto *allocation = new TextureAllocation(this, std::move(buffer), placed_memory, info, bytes_per_row_, flags);
  allocation->placed_memory_ = true;
  return allocation;
}

Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag> flags, WMT::Heap heap, uint64_t heap_offset) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  WMTTextureInfo info = info_; // copy
  info.mach_port = 0;
  if (info.sample_count > 1) info.usage = static_cast<WMTTextureUsage>(info.usage & ~WMTTextureUsageShaderWrite);
  if (flags.test(TextureAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(TextureAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(TextureAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  info.options = options;
  flags.set(TextureAllocationFlag::AllocatedOnHeap);
  Rc<TextureAllocation> allocation = new TextureAllocation(this, heap.newTexture(info, heap_offset), info, flags);
  allocation->canonicalHeap = heap;
  allocation->canonicalHeapOffset = heap_offset;
  return allocation;
}

Rc<TextureAllocation>
Texture::allocateSparse(WMTSparsePageSize page_size) {
  WMTTextureInfo info = info_; // copy
  info.mach_port = 0;
  info.options = WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate;
  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::GpuPrivate);
  flags.set(TextureAllocationFlag::PlacementSparse);
  auto texture = device_.newPlacementSparseTexture(info, page_size);
  return new TextureAllocation(this, std::move(texture), info, flags);
}

Rc<TextureAllocation>
Texture::import(mach_port_t mach_port) {
  Flags<TextureAllocationFlag> flags;
  WMTTextureInfo info;
  info.mach_port = mach_port;
  auto texture = device_.newSharedTexture(info);
  // now allocation's info is populated
  // and we may check if it is consistent with texture's info (it should be)
  if (texture) {
    // doing some unnecessary checks for the sake of completeness
    if (info.options & WMTResourceStorageModeManaged) // should be always false
      flags.set(TextureAllocationFlag::GpuManaged);
    if (info.options & WMTResourceStorageModePrivate) // should be always true
      flags.set(TextureAllocationFlag::GpuPrivate);
    if (info.options & WMTResourceHazardTrackingModeUntracked)
      flags.set(TextureAllocationFlag::NoTracking);
    if ((info.usage & (WMTTextureUsageShaderWrite | WMTTextureUsageRenderTarget)) == 0)
      flags.set(TextureAllocationFlag::ShaderReadonly);
    flags.set(TextureAllocationFlag::Shared);
    return new TextureAllocation(this, std::move(texture), info, flags);
  }
  // A failed import (dead or foreign port) is reported to the caller, never aborts.
  return nullptr;
}

TextureView &
Texture::view(TextureViewKey key) {
  return view(key, current_.ptr());
}

TextureView &
Texture::view(TextureViewKey key, TextureAllocation* allocation) {
  if (descriptorIndexedViews) {
    TextureViewDescriptor descriptor;
    {
      std::unique_lock<dxmt::shared_mutex> lock(mutex_);
      allocation->cached_view_.resize(version_);
      if (allocation->cached_view_[key.index]) return *allocation->cached_view_[key.index];
      descriptor = viewDescriptors_[key.index];
    }
    TextureViewRef prepared(key.index ? new TextureView(allocation, key.index, descriptor) : new TextureView(allocation));
    std::unique_lock<dxmt::shared_mutex> lock(mutex_);
    if (!allocation->cached_view_[key.index]) allocation->cached_view_[key.index] = std::move(prepared);
    return *allocation->cached_view_[key.index];
  }
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key.index];
}

bool Texture::supportsDescriptorViewPool(TextureViewKey key) {
  const auto descriptor = viewDescriptor(key);
  auto allocation = current();
  return allocation && !bytes_per_image_ && !reservedResource && !reservedVolumeLayoutAddress &&
      !allocation->flags().test(TextureAllocationFlag::PlacementSparse) &&
      !allocation->flags().test(TextureAllocationFlag::GpuManaged) && sampleCount() == 1 &&
      !descriptor.castBlockSize && !descriptor.castCompressed && !descriptor.msaaShadow &&
      !descriptor.reservedVolumeRender && descriptor.format == pixelFormat() &&
      (descriptor.type == WMTTextureType2D || descriptor.type == WMTTextureType2DArray) &&
      (textureType() == WMTTextureType2D || textureType() == WMTTextureType2DArray);
}

TextureViewKey Texture::checkViewUseArray(TextureViewKey key, bool isArray) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key.index];
  shared_lock = {};
  static constexpr uint32_t ARRAY_TYPE_MASK = 0b0101001010;
  if (unlikely(bool((1 << uint32_t(view.type)) & ARRAY_TYPE_MASK) != isArray)) {
    // TODO: this process can be cached
    auto new_view_desc = view;
    switch (view.type) {
    case WMTTextureType1D:
      new_view_desc.type = WMTTextureType1DArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType1DArray:
      new_view_desc.type = WMTTextureType1D;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2D:
      new_view_desc.type = WMTTextureType2DArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DArray:
      new_view_desc.type = WMTTextureType2D;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DMultisample:
      new_view_desc.type = WMTTextureType2DMultisampleArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DMultisampleArray:
      new_view_desc.type = WMTTextureType2DMultisample;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureTypeCube:
      new_view_desc.type = WMTTextureTypeCubeArray;
      new_view_desc.arraySize = 6;
      break;
    case WMTTextureTypeCubeArray:
      new_view_desc.type = WMTTextureTypeCube;
      new_view_desc.arraySize = 6;
      break;
    default:
      return key; // should be unreachable
    }
    return createView(new_view_desc);
  }
  return key;
}

TextureViewKey Texture::checkViewUseFormat(TextureViewKey key, WMTPixelFormat format) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key.index];
  shared_lock = {};
  if (unlikely(view.format != format)) {
    auto new_view_desc = view;
    new_view_desc.format = format;
    auto result = createView(new_view_desc);
    if (reservedVolumeLayoutAddress) {
      result.mip_start = key.mip_start;
      result.mip_end = key.mip_end;
      result.mip_count = key.mip_count;
    }
    return result;
  }
  return key;
}

Rc<TextureAllocation>
Texture::rename(Rc<TextureAllocation> &&newAllocation) {
  Rc<TextureAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void Texture::incRef(){
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void Texture::decRef(){
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

RenamableTexturePool::RenamableTexturePool(
    Texture *texture, size_t capacity, Flags<TextureAllocationFlag> allocation_flags
) :
    texture_(texture),
    allocations_(capacity, nullptr),
    allocation_flags_(allocation_flags),
    capacity_(capacity) {}

void
RenamableTexturePool::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
RenamableTexturePool::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<TextureAllocation>
RenamableTexturePool::getNext(uint64_t frame) {
  if (frame > last_frame_) {
    last_frame_ = frame;
    current_index_ = 0;
  }
  auto current_index = current_index_++ % capacity_;
  if (!allocations_[current_index].ptr())
    allocations_[current_index] = texture_->allocate(allocation_flags_);
  return allocations_[current_index];
}

} // namespace dxmt