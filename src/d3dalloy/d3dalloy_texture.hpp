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
#include "Metal.hpp"
#include "d3dalloy_deptrack.hpp"
#include "d3dalloy_residency.hpp"
#include "d3dalloy_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "thread.hpp"
#include "util_flags.hpp"
#include "util_svector.hpp"
#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace d3dalloy {

class ReservedResource;

enum class TextureAllocationFlag : uint32_t {
  GpuReadonly = 0,
  NoTracking = 0,
  GpuPrivate = 1,
  CpuInvisible = 1,
  CpuWriteCombined = 2,
  OwnedByCommandList = 3,
  GpuManaged = 4,
  Shared = 5,
  ShaderReadonly = 6,
  AllocatedOnHeap = 7,
  /* A Metal placement sparse texture: its tiles are mapped by explicit tile mapping updates. */
  PlacementSparse = 8,
};

struct TextureViewDescriptor {
  WMTPixelFormat format    : 24;
  WMTTextureType type      : 8;
  uint32_t firstMiplevel   : 4 = 0;
  uint32_t miplevelCount   : 4 = 1;
  uint32_t firstArraySlice : 12 = 0;
  uint32_t arraySize       : 12 = 1;
  // Metal cannot reinterpret BC blocks or depth texels as color pixels.
  // GPU-only shadows retain the source bytes across these format families.
  uint8_t castBlockSize = 0;
  uint8_t castSourceBlockWidth = 4;
  bool castCompressed = false;
  bool msaaShadow = false;
  bool reservedVolumeRender = false;
  uint16_t volumeFirstW = 0;
  uint8_t volumeCastMip = 0;

  bool operator==(const TextureViewDescriptor &other) const;
  struct Hash {
    size_t operator()(const TextureViewDescriptor &descriptor) const;
  };
};

struct TextureViewKey {
  union {
    struct {
      uint64_t index       : 28;
      uint64_t mip_count   : 4;
      uint64_t mip_start   : 4;
      uint64_t array_start : 12;
      uint64_t mip_end     : 4;
      uint64_t array_end   : 12;
    };
    uint64_t impl_;
  };

  TextureViewKey() {
    impl_ = 0;
  }
  TextureViewKey(const TextureViewDescriptor &descriptor, unsigned index, unsigned total_mip_count) {
    mip_start = descriptor.firstMiplevel;
    array_start = descriptor.firstArraySlice;
    mip_end = descriptor.firstMiplevel + descriptor.miplevelCount;
    array_end = descriptor.firstArraySlice + descriptor.arraySize;
    mip_count = total_mip_count;
    this->index = index;
  }
  TextureViewKey(uint64_t impl) {
    impl_ = impl;
  }
  operator uint64_t() const {
    return impl_;
  }
};

class Texture;
class TextureAllocation;

struct AuxiliarySparsePages {
  uint64_t mapping_generation = ~uint64_t(0);
  std::vector<uint32_t> mapped_tiles;
  std::unordered_set<uint32_t> mapped_pages;
  struct Backing {
    WMT::Reference<WMT::Heap> heap;
    std::vector<std::optional<uint32_t>> slots;
  };
  std::vector<Backing> backings;
  WMT::Reference<WMT::SharedEvent> ready;
  uint64_t ready_value = 0, joined_queue = 0;
};

struct ReservedVolumeRenderView : AuxiliarySparsePages {
  uint32_t mip, first_w, w_size, page_w, page_h, pages_x, pages_y;
};

struct ReservedCastStorage : AuxiliarySparsePages {
  uint32_t page_w, page_h, pages_x, pages_y;
  bool packed = false;
};

class TextureView {
public:
  virtual ~TextureView() {};

  void incRef();
  void decRef();

  WMT::Reference<WMT::Texture> texture;
  uint64_t gpuResourceID;
  D3DALLOY_RESOURCE_RESIDENCY_STATE residency{};
  TextureAllocation *allocation; // `TextureAllocation` holds strong reference to `TextureView`
  TextureViewKey key;
  uint8_t castBlockSize = 0;
  uint8_t castSourceBlockWidth = 4;
  bool castCompressed = false;
  WMT::Reference<WMT::Texture> castStorage;
  std::shared_ptr<ReservedCastStorage> reservedCast;
  std::array<WMT::Reference<WMT::Texture>, 8> msaaViews;
  WMT::Reference<WMT::Buffer> msaaSampleIds;
  uint64_t msaaSampleAddress = 0;
  uint64_t reservedVolumeCastLayoutAddress = 0;
  bool reservedVolumeAtlasCast = false;
  std::unique_ptr<ReservedVolumeRenderView> reservedVolumeRender;

  TextureView(const TextureView &) = delete;
  TextureView(TextureView &&) = delete;
  TextureView &operator=(const TextureView &) = delete;
  TextureView &operator=(TextureView &&) = delete;
  TextureView(TextureAllocation *allocation);
  TextureView(TextureAllocation *allocation, unsigned index, TextureViewDescriptor descriptor);

private:
  std::atomic<uint32_t> refcount_ = {0u};
};

class TextureViewRef : public Rc<TextureView> {
public:
  using Rc<TextureView>::Rc;

  WMT::Texture
  texture() const {
    if (!*this)
      return {};
    return ptr()->texture;
  }

  TextureViewRef &
  operator=(TextureView &ref) {
    return (*this = &ref);
  }
};

class TextureAllocation final : public Allocation {
  friend class Texture;
  friend class TextureView;

  /**
   * notes on thread-safety:
   * all states in `TextureAllocation` is either immutable or only accessed by `d3dalloy-encode-thread`
   */

public:

  WMT::Texture texture() const {
    return obj_;
  }

  Flags<TextureAllocationFlag>
  flags() const {
    return flags_;
  }

  Texture *descriptor;
  WMT::Reference<WMT::Heap> canonicalHeap;
  uint64_t canonicalHeapOffset = 0;
  // D3D12 publishes the actual native placement extent once at creation.
  // Zero leaves unknown/shared allocation paths conservatively whole-heap.
  uint64_t canonicalHeapSize = 0;
  void *mappedMemory;
  uint64_t gpuResourceID;
  mach_port_t machPort;
  small_vector<GenericAccessTracker, 1> fenceTrackers;
  std::array<WMT::Reference<WMT::Texture>, 8> msaaShadow;
  std::array<WMT::Reference<WMT::Texture>, 8> msaaBaseline;
  WMT::Reference<WMT::Texture> msaaCanonical;
  WMTPixelFormat msaaFormat = WMTPixelFormatInvalid;

private:
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
      unsigned bytes_per_row, Flags<TextureAllocationFlag> flags
  );
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
      Flags<TextureAllocationFlag> flags
  );

  void free();

  TextureAllocation(const TextureAllocation &) = delete;
  TextureAllocation(TextureAllocation &&) = delete;

  WMT::Reference<WMT::Texture> obj_;
  WMT::Reference<WMT::Buffer> buffer_;
  bool placed_memory_ = false;
  uint32_t version_ = 0;
  Flags<TextureAllocationFlag> flags_;
  small_vector<TextureViewRef, 4> cached_view_;
};

class Texture {
  friend class TextureView;
  friend class TextureAllocation;

public:
  /* Nonzero only for a D3D12 tiled volume's physical sparse 2D-array atlas. */
  uint64_t reservedVolumeLayoutAddress = 0;

  const uint32_t *reservedVolumeLayoutWords = nullptr;
  ReservedResource *reservedResource = nullptr;
  struct ReservedVolumeCastBinding { WMT::Reference<WMT::Texture> atlas; uint64_t layout_address; };
  ReservedVolumeCastBinding (*reservedVolumeCastBinding)(void *, uint32_t, WMTPixelFormat, bool) = nullptr;
  void *reservedVolumeCastContext = nullptr;
  void incRef();
  void decRef();

  TextureViewKey createView(TextureViewDescriptor const &descriptor);
  /* True while any live texture owns a cast, MSAA-shadow or reserved-volume view (process-wide). */
  static bool AnySpecialViews() { return special_view_textures_.load(std::memory_order_acquire) != 0; }
  TextureViewDescriptor viewDescriptor(TextureViewKey key) {
    std::shared_lock<d3dalloy::shared_mutex> lock(mutex_);
    return viewDescriptors_[key.index];
  }
  bool isWritableMSAAView(TextureViewKey key) {
    std::shared_lock<d3dalloy::shared_mutex> lock(mutex_);
    return viewDescriptors_[key.index].msaaShadow;
  }

  constexpr TextureAllocation *
  current() {
    return current_.ptr();
  }

  WMTTextureType
  textureType() const {
    return info_.type;
  }

  WMTPixelFormat
  pixelFormat() const {
    return info_.pixel_format;
  }

  WMTTextureType
  textureType(TextureViewKey view) {
    std::shared_lock<d3dalloy::shared_mutex> lock(mutex_);
    return viewDescriptors_[view.index].type;
  }

  WMTPixelFormat
  pixelFormat(TextureViewKey view) {
    std::shared_lock<d3dalloy::shared_mutex> lock(mutex_);
    return viewDescriptors_[view.index].format;
  }

  WMTTextureUsage
  usage() const {
    return info_.usage;
  }

  unsigned
  sampleCount() const {
    return info_.sample_count;
  }

  unsigned
  width() const {
    return info_.width;
  }

  unsigned
  height() const {
    return info_.height;
  }

  unsigned
  depth() const {
    return info_.depth;
  }

  unsigned
  width(TextureViewKey view) {
    std::shared_lock<d3dalloy::shared_mutex> lock(mutex_);
    const auto width = std::max((reservedVolumeLayoutWords ? reservedVolumeLayoutWords[0] : info_.width) >> view.mip_start, 1u);
    const auto &descriptor = viewDescriptors_[view.index];
    return descriptor.castBlockSize && !descriptor.castCompressed
        ? (width + descriptor.castSourceBlockWidth - 1) / descriptor.castSourceBlockWidth : width;
  }

  unsigned
  height(TextureViewKey view) {
    std::shared_lock<d3dalloy::shared_mutex> lock(mutex_);
    const auto height = std::max((reservedVolumeLayoutWords ? reservedVolumeLayoutWords[1] : info_.height) >> view.mip_start, 1u);
    const auto &descriptor = viewDescriptors_[view.index];
    return descriptor.castBlockSize && !descriptor.castCompressed
        ? (height + descriptor.castSourceBlockWidth - 1) / descriptor.castSourceBlockWidth : height;
  }

  unsigned
  depth(TextureViewKey view) const {
    return std::max((reservedVolumeLayoutWords ? reservedVolumeLayoutWords[2] : info_.depth) >> view.mip_start, 1u);
  }

  /**
  \warning for cube texture, this would be multiple of 6.
  */
  unsigned
  arrayLength() const {
    switch (info_.type) {
    case WMTTextureTypeCubeArray:
    case WMTTextureTypeCube:
      return info_.array_length * 6;
    default:
      break;
    }
    return info_.array_length;
  }

  unsigned
  arrayLength(TextureViewKey view) {
    return view.array_end - view.array_start;
  }

  unsigned
  miplevelCount() const {
    return info_.mipmap_level_count;
  }

  TextureViewKey fullView;

  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags);
  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags, WMT::Heap heap, uint64_t heap_offset = 0);
  /* Linear (bytes_per_row) textures only: alias caller-owned memory, e.g. a shared section view. */
  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags, void *placed_memory);
  /** A placement sparse texture whose tiles are all unmapped. */
  Rc<TextureAllocation> allocateSparse(WMTSparsePageSize page_size);
  Rc<TextureAllocation> import(mach_port_t mach_port);

  TextureView &view(TextureViewKey key);
  TextureView &view(TextureViewKey key, TextureAllocation *allocation);
  /* Set before D3D12 resource publication; D3D11 keeps eager view preparation. */
  bool descriptorIndexedViews = false;
  bool privateAttachmentStore = false;
  bool supportsDescriptorViewPool(TextureViewKey key);
  // A shader view the submission planner can describe by allocation, mip and
  // slice: a native view of the current allocation, no cast/shadow/sparse
  // indirection. Unlike the view pool, multisample and format views qualify.
  bool directViewOfAllocation(TextureViewKey key);

  TextureViewKey checkViewUseArray(TextureViewKey key, bool isArray);
  TextureViewKey checkViewUseFormat(TextureViewKey key, WMTPixelFormat format);

  Rc<TextureAllocation> rename(Rc<TextureAllocation> &&newAllocation);

  Texture(const WMTTextureInfo &info, WMT::Device device);

  Texture(unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &info, WMT::Device device);

private:
  void prepareAllocationViews(TextureAllocation* allocation);

  WMTTextureInfo info_;
  unsigned bytes_per_image_ = 0;
  unsigned bytes_per_row_ = 0;

  Rc<TextureAllocation> current_;
  uint32_t version_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};

  small_vector<TextureViewDescriptor, 4> viewDescriptors_;
  std::unordered_map<TextureViewDescriptor, unsigned, TextureViewDescriptor::Hash> viewIndices_;
  d3dalloy::shared_mutex mutex_;
  WMT::Device device_;
  bool has_special_view_ = false;
  static inline std::atomic<uint32_t> special_view_textures_{0};
};

class RenamableTexturePool {
public:
  void incRef();
  void decRef();
  Rc<TextureAllocation> getNext(uint64_t frame_);

  RenamableTexturePool(Texture *texture, size_t capacity,Flags<TextureAllocationFlag> allocation_flags);

private:
  Rc<Texture> texture_;
  std::vector<Rc<TextureAllocation>> allocations_;
  uint64_t last_frame_ = 0;
  Flags<TextureAllocationFlag> allocation_flags_;
  unsigned capacity_;
  unsigned current_index_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

} // namespace d3dalloy
