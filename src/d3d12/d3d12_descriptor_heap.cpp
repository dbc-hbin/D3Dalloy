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

#include "d3d12_device.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_sampler_feedback.hpp"
#include "com/com_pointer.hpp"
#include "d3dalloy_sampler.hpp"
#include "log/log.hpp"
#include <atomic>
#include <bit>
#include <cstring>
#include <type_traits>
#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
#include <map>
#include <shared_mutex>
#endif

namespace d3dalloy {

template <typename T>
static void CopyDescriptorRecords(T *destination, const T *source, UINT count) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memmove(destination, source, sizeof(T) * count);
}

#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
namespace {

struct DescriptorHandleRange {
  SIZE_T end; /* one past the last descriptor handle */
  void *heap;
  DescriptorHandleKind kind;
};

/*
 * Ranges keyed by base handle. New ranges are bump-allocated from `next`, and a released range
 * goes back on `free_ranges` for reuse by an exact-size heap only, so live ranges never overlap and
 * bases never shift. Handle values therefore stay valid for a heap's whole lifetime.
 */
struct DescriptorHandleRegistry {
  std::shared_mutex lock;
  std::map<SIZE_T, DescriptorHandleRange> ranges;
  std::map<const void *, SIZE_T> bases; /* heap -> base, for handles a heap reports itself */
  std::multimap<uint64_t, SIZE_T> free_ranges; /* span in bytes -> base */
  uint64_t next = kDescriptorHandleStride;
};

DescriptorHandleRegistry &
descriptor_handle_registry() {
  static DescriptorHandleRegistry registry;
  return registry;
}

} // namespace

SIZE_T
RegisterDescriptorHandleRange(void *heap, DescriptorHandleKind kind, uint32_t count) {
  auto &registry = descriptor_handle_registry();
  /* An empty heap still owns one slot so its start handle is distinct and resolvable to no descriptor. */
  const uint64_t span = uint64_t(count ? count : 1) *
      (kind == DescriptorHandleKind::Sampler ? kSamplerDescriptorStride : kDescriptorHandleStride);
  std::unique_lock lock(registry.lock);
  SIZE_T base = 0;
  if (auto reuse = registry.free_ranges.find(span); reuse != registry.free_ranges.end()) {
    base = reuse->second;
    registry.free_ranges.erase(reuse);
  } else {
    if (span > UINT32_MAX || registry.next > uint64_t(UINT32_MAX) + 1 - span)
      return 0;
    base = SIZE_T(registry.next);
    registry.next += span;
  }
  registry.ranges.emplace(base, DescriptorHandleRange{SIZE_T(base + span), heap, kind});
  registry.bases.emplace(heap, base);
  return base;
}

void
UnregisterDescriptorHandleRange(SIZE_T base) {
  if (!base)
    return;
  auto &registry = descriptor_handle_registry();
  std::unique_lock lock(registry.lock);
  auto range = registry.ranges.find(base);
  if (range == registry.ranges.end())
    return;
  registry.free_ranges.emplace(uint64_t(range->second.end - base), base);
  registry.bases.erase(range->second.heap);
  registry.ranges.erase(range);
}

std::pair<void *, uint32_t>
ResolveDescriptorHandle(SIZE_T handle, DescriptorHandleKind kind) {
  auto &registry = descriptor_handle_registry();
  std::shared_lock lock(registry.lock);
  auto range = registry.ranges.upper_bound(handle);
  if (range == registry.ranges.begin())
    return {nullptr, 0};
  --range;
  const SIZE_T offset = handle - range->first;
  const uint32_t stride = kind == DescriptorHandleKind::Sampler ? kSamplerDescriptorStride : kDescriptorHandleStride;
  if (handle >= range->second.end || range->second.kind != kind || offset % stride)
    return {nullptr, 0};
  return {range->second.heap, uint32_t(offset / stride)};
}

SIZE_T
DescriptorHandleOf(const void *heap, uint32_t index) {
  auto &registry = descriptor_handle_registry();
  std::shared_lock lock(registry.lock);
  auto base = registry.bases.find(heap);
  if (base == registry.bases.end())
    return 0;
  const auto &range = registry.ranges.at(base->second);
  return base->second + SIZE_T(index) *
      (range.kind == DescriptorHandleKind::Sampler ? kSamplerDescriptorStride : kDescriptorHandleStride);
}

/** Owns one heap's handle range; registration happens only once the heap is fully initialized. */
class DescriptorHandleRangeOwner {
  SIZE_T base_ = 0;

public:
  DescriptorHandleRangeOwner() = default;
  DescriptorHandleRangeOwner(const DescriptorHandleRangeOwner &) = delete;
  DescriptorHandleRangeOwner &operator=(const DescriptorHandleRangeOwner &) = delete;
  ~DescriptorHandleRangeOwner() { UnregisterDescriptorHandleRange(base_); }

  HRESULT
  Register(void *heap, DescriptorHandleKind kind, uint32_t count) {
    base_ = RegisterDescriptorHandleRange(heap, kind, count);
    return base_ ? S_OK : E_OUTOFMEMORY;
  }
};
#endif

struct SRVTextureGPUStorage {
  uint64_t resource_id;
  uint64_t metadata;
  uint64_t padding[2];
};

using UAVTextureGPUStorage = SRVTextureGPUStorage;

struct UAVTexelBufferGPUStorage {
  uint64_t resource_id;
  uint64_t metadata;
  uint64_t padding[2];
};

using SRVTexelBufferGPUStorage = UAVTexelBufferGPUStorage;

struct UAVBufferGPUStorage {
  uint64_t pointer;
  uint64_t metadata;
  uint64_t counter_pointer;
  uint64_t padding;
};

using SRVBufferGPUStorage = UAVBufferGPUStorage;

struct ShaderVisibleDescriptorGPUStorage {
  union {
    SRVTextureGPUStorage SRVTexture;
    CBVCommonStorage ConstantBuffer;
    UAVTextureGPUStorage UAVTexture;
    UAVTexelBufferGPUStorage UAVTexelBuffer;
    UAVBufferGPUStorage UAVBuffer;
    SRVTexelBufferGPUStorage SRVTexelBuffer;
    SRVBufferGPUStorage SRVBuffer;
    std::array<uint64_t, 4> ZeroFilled;
  };

  ShaderVisibleDescriptorGPUStorage();
};

static_assert(sizeof(ShaderVisibleDescriptorGPUStorage) == 32);

/*
 * Residency status of a typed view of a reserved buffer, in the view's two spare qwords: [0] the
 * buffer's biased residency base (BufferAllocation::residencyStatusBase), [1] the view's first byte
 * address << 8 | log2(texel size) << 1. Bit 0 of [1] stays clear: set, it marks an RGB32 view.
 * Both are zero for every other buffer, which is always fully mapped.
 */
inline void
TypedResidencyStatus(Buffer *buffer, BufferViewKey view, uint64_t (&slots)[2]) {
  slots[0] = slots[1] = 0;
  if (!buffer || !buffer->current()->residencyStatusBase)
    return;
  const auto descriptor = buffer->viewDescriptor(view);
  const uint32_t texel = MTLGetTexelSize(descriptor.format);
  if (!texel || (texel & (texel - 1)))
    return;
  slots[0] = buffer->current()->residencyStatusBase;
  slots[1] = ((buffer->current()->gpuAddress() + descriptor.byteOffset) << 8) | (uint64_t(__builtin_ctz(texel)) << 1);
}

inline uint64_t
TextureMetadata(uint32_t array_length, float min_lod) {
  return ((uint64_t)array_length << 32) | (uint64_t)std::bit_cast<uint32_t>(min_lod);
}

class MTLD3D12DescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12DescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<ShaderVisibleDescriptorCPUStorage> descriptors_;
  struct DescriptorBlock {
    std::atomic<uint64_t> version{0};
    std::atomic<uint64_t> pooled{0};
    std::atomic<uint64_t> copy_token{0};

    std::atomic<uint64_t> dirty_slots{UINT64_MAX};
    std::atomic_flag publishing{};
    uint64_t last_source_token = 0;

    // Multiple CPU readers may copy the same block to different heaps.
    // Only the publisher advances its baseline; a concurrent reader can
    // always copy the complete block without waiting or claiming a token.
    uint64_t PublishSource(uint64_t target, uint64_t &changed) {
      if (publishing.test_and_set(std::memory_order_acquire)) return 0;
      auto token = copy_token.load(std::memory_order_relaxed);
      if (!token) {
        const auto dirty = dirty_slots.exchange(0, std::memory_order_relaxed);
        if (target && target == last_source_token) changed = dirty;
        token = last_source_token = owned_graphics::NewObjectId();
        copy_token.store(token, std::memory_order_relaxed);
      }
      publishing.clear(std::memory_order_release);
      return token;
    }
  };
  static_assert(kDescriptorVersionGranularity == 64);
  std::unique_ptr<DescriptorBlock[]> blocks_;
  Rc<Buffer> buffer_;
  ShaderVisibleDescriptorGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;
  struct PoolSlot {
    WMT::Reference<WMT::Texture> texture;
    WMTTextureViewPoolInfo info{};
  };
  std::vector<PoolSlot> pool_slots_;
  WMT::Reference<WMT::TextureViewPool> view_pool_;
  bool pool_supported_ = false;

  void InvalidateDescriptors(UINT first, UINT count) {
    if (!count) return;
    const uint64_t last = (uint64_t(first) + count - 1) / kDescriptorVersionGranularity;
    for (uint64_t block = first / kDescriptorVersionGranularity; block <= last; ++block) {
      blocks_[block].copy_token.store(0, std::memory_order_relaxed);
      if (buffer_) blocks_[block].version.fetch_add(1, std::memory_order_release);
      else {
        const unsigned lo = block == first / 64 ? first % 64 : 0;
        const unsigned hi = block == last ? (uint64_t(first) + count - 1) % 64 : 63;
        blocks_[block].dirty_slots.fetch_or((UINT64_MAX << lo) & (UINT64_MAX >> (63 - hi)), std::memory_order_relaxed);
      }
    }
  }

  bool HasPooledSlots(UINT first, UINT count) const {
    while (count) {
      const unsigned bit = first % 64, span = std::min<UINT>(count, 64 - bit);
      const uint64_t mask = (UINT64_MAX >> (64 - span)) << bit;
      if (blocks_[first / 64].pooled.load(std::memory_order_relaxed) & mask) return true;
      first += span;
      count -= span;
    }
    return false;
  }

  void MarkPooled(UINT index) {
    if (!pool_slots_[index].texture)
      blocks_[index / 64].pooled.fetch_or(uint64_t(1) << (index % 64), std::memory_order_relaxed);
  }

  void MarkPooledRange(UINT first, UINT count) {
    while (count) {
      const unsigned bit = first % 64, span = std::min<UINT>(count, 64 - bit);
      blocks_[first / 64].pooled.fetch_or((UINT64_MAX >> (64 - span)) << bit, std::memory_order_relaxed);
      first += span;
      count -= span;
    }
  }

  ShaderVisibleDescriptorCPUStorage &WritableDescriptor(UINT index, uint64_t owner_id = 0, uint64_t counter_owner_id = 0) {
    // Also invalidate partial/failed replacements. As with GetDescriptor,
    // callers must not race writes to a slot with a submitted use of that slot.
    InvalidateDescriptors(index, 1);
    descriptors_[index].owner_id = owner_id;
    descriptors_[index].counter_owner_id = counter_owner_id;
    return descriptors_[index];
  }

  void ResetPoolSlot(UINT index) {
    if (pool_slots_[index].texture) {
      view_pool_.clear(index);
      pool_slots_[index] = {};
      blocks_[index / 64].pooled.fetch_and(~(uint64_t(1) << (index % 64)), std::memory_order_relaxed);
    }
  }

  bool SetPoolSlot(UINT index, Texture *texture, TextureViewKey key) {
    if (!pool_supported_ || !texture->supportsDescriptorViewPool(key)) {
      ResetPoolSlot(index);
      return false;
    }
    const auto descriptor = texture->viewDescriptor(key);
    auto &slot = pool_slots_[index];
    MarkPooled(index);
    slot.texture = texture->current()->texture();
    slot.info = {uint32_t(descriptor.format), uint32_t(descriptor.type), descriptor.firstMiplevel,
                 descriptor.miplevelCount, descriptor.firstArraySlice, descriptor.arraySize};
    mapped_argument_buffer_[index].SRVTexture.resource_id = view_pool_.setTextureView(slot.texture, slot.info, index);
    // Native set replaces the old slot and its residency owner atomically. A
    // rejected set must still leave a null slot, as the former clear+set did.
    if (!mapped_argument_buffer_[index].SRVTexture.resource_id) ResetPoolSlot(index);
    return true;
  }
#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  DescriptorHandleRangeOwner handles_;
#endif

public:
  MTLD3D12DescriptorHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12DescriptorHeap>(pDevice) {}

  WMT::Buffer GetArgumentBuffer() const override {
    return buffer_ ? buffer_->current()->buffer() : WMT::Buffer{};
  }

  uint64_t GetDescriptorVersion(UINT index) const override {
    return blocks_[index / kDescriptorVersionGranularity].version.load(std::memory_order_acquire);
  }

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
      break;
    }
    default:
      return E_INVALIDARG;
    }
    descriptors_.resize(pDesc->NumDescriptors);
    blocks_ = std::make_unique<DescriptorBlock[]>(
        (uint64_t(pDesc->NumDescriptors) + kDescriptorVersionGranularity - 1) / kDescriptorVersionGranularity);
    pool_slots_.resize(pDesc->NumDescriptors);
    pool_supported_ = device_->GetMTLDevice().supportsMetal4();

    if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      buffer_ = new Buffer(descriptors_.size() * sizeof(ShaderVisibleDescriptorGPUStorage), device_->GetMTLDevice());

      Flags<BufferAllocationFlag> flags;
#ifdef __i386__
      /* The CPU writes descriptors into this argument buffer; keep it in 32-bit-addressable storage. */
      flags.set(BufferAllocationFlag::CpuPlaced);
#endif
      buffer_->rename(buffer_->allocate(flags));
      if (!buffer_->current())
        return E_OUTOFMEMORY;
      mapped_argument_buffer_ =
          reinterpret_cast<ShaderVisibleDescriptorGPUStorage *>(buffer_->current()->mappedMemory(0));
      if (!mapped_argument_buffer_)
        return E_OUTOFMEMORY;
      argument_buffer_gpu_address_ = buffer_->current()->gpuAddress();
      HRESULT hr = device_->RegisterResidencyAndVA(buffer_->current(), this);
      if (FAILED(hr))
        return hr;
    } else {
      mapped_argument_buffer_ = reinterpret_cast<ShaderVisibleDescriptorGPUStorage *>(
          calloc(descriptors_.size(), sizeof(ShaderVisibleDescriptorGPUStorage))
      );
      if (!descriptors_.empty() && !mapped_argument_buffer_)
        return E_OUTOFMEMORY;
      InitializeResidency(true);
    }
    // CPU heaps are legal CopyDescriptors sources: their native pool avoids recreating
    // a view when copying into a shader-visible destination pool.
    if (pDesc->NumDescriptors && pool_supported_) {
      view_pool_ = device_->GetMTLDevice().newTextureViewPool(pDesc->NumDescriptors, GetArgumentBuffer());
      if (!view_pool_) return E_OUTOFMEMORY;
    }

#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
    return handles_.Register(static_cast<MTLD3D12DescriptorHeap *>(this), DescriptorHandleKind::ShaderVisible,
                             pDesc->NumDescriptors);
#else
    return S_OK;
#endif
  };

  ~MTLD3D12DescriptorHeapImpl() {
    if (buffer_) {
      device_->UnregisterResidencyAndVA(buffer_->current(), this);
    } else {
      free(mapped_argument_buffer_);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) {
    *__ret = GetShaderVisibleDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) {
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, uint64_t owner_id, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp,
                        bool CubeView, bool CenterSamplePattern, UINT64 FormatDefaults) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    const auto *texture_view = SetPoolSlot(Index, Texture, View) ? nullptr : &Texture->view(View);
    if (texture_view ? !texture_view->texture : !mapped_argument_buffer_[Index].SRVTexture.resource_id) return E_OUTOFMEMORY;
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexture;
    cpu_storage.SRVTexture.texture = Texture;
    cpu_storage.SRVTexture.view = View;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    if (texture_view) gpu_storage.SRVTexture.resource_id = texture_view->gpuResourceID;
    // The physical view's array range counts faces; a shader cube-array
    // coordinate indexes cubes, not individual faces.
    // Multisampled SRVs do not have an LOD clamp: their low metadata word
    // carries the resource's fixed sample pattern for GetSamplePosition.
    gpu_storage.SRVTexture.metadata = TextureMetadata(
        Texture->arrayLength(View) / (CubeView ? 6 : 1),
        CenterSamplePattern ? std::bit_cast<float>(uint32_t(1)) : ResourceMinLODClamp);
    // q2: sampler-feedback view range, replaced below by reserved-volume or MSAA sample tables.
    gpu_storage.SRVTexture.padding[0] = (1ull << 63) | (uint64_t(View.array_start) << 16) |
                                        (uint64_t(View.mip_end) << 8) | uint64_t(View.mip_start);
    // q3: empty-view defaults and integer channel widths; see EmptyViewDefaults.
    gpu_storage.SRVTexture.padding[1] = FormatDefaults;
    if (Texture->reservedVolumeLayoutAddress) {
      const auto cast_address = texture_view->reservedVolumeCastLayoutAddress;
      gpu_storage.SRVTexture.padding[0] = (1ull << 62) | (cast_address ? cast_address : Texture->reservedVolumeLayoutAddress);
      gpu_storage.SRVTexture.padding[1] |= (cast_address ? uint64_t(1) << 8 :
          uint64_t(View.mip_start) | uint64_t(View.mip_end - View.mip_start) << 8) << 32;
    }
    if (texture_view && texture_view->msaaSampleAddress) {
      gpu_storage.SRVTexture.padding[0] = texture_view->msaaSampleAddress;
      gpu_storage.SRVTexture.metadata |= 2u | (uint64_t(Texture->sampleCount()) << 8);
    }
    return S_OK;
  }
  uint64_t GetTextureResourceId(UINT index) const override {
    if (index >= descriptors_.size()) return 0;
    const auto type = descriptors_[index].type;
    return type == ShaderVisibleDescriptorType::SRVTexture || type == ShaderVisibleDescriptorType::UAVTexture
        ? mapped_argument_buffer_[index].SRVTexture.resource_id : 0;
  }
    virtual HRESULT
  AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index);
    cpu_storage.type = ShaderVisibleDescriptorType::ConstantBuffer;
    cpu_storage.ConstantBuffer.address = VA;
    cpu_storage.ConstantBuffer.size = SizeInBytes;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.ConstantBuffer.address = VA;
    gpu_storage.ConstantBuffer.size = SizeInBytes;
    gpu_storage.ZeroFilled[2] = gpu_storage.ZeroFilled[3] = 0;
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, uint64_t owner_id, Texture *Texture, TextureViewKey View, uint16_t FirstWSlice, uint16_t WSize, UINT FormatDefaults) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    const auto *texture_view = SetPoolSlot(Index, Texture, View) ? nullptr : &Texture->view(View);
    if (texture_view ? !texture_view->texture : !mapped_argument_buffer_[Index].SRVTexture.resource_id) return E_OUTOFMEMORY;
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexture;
    cpu_storage.UAVTexture.texture = Texture;
    cpu_storage.UAVTexture.view = View;
    cpu_storage.UAVTexture.first_w_slice = FirstWSlice;
    cpu_storage.UAVTexture.w_size = WSize;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    if (texture_view) gpu_storage.UAVTexture.resource_id = texture_view->gpuResourceID;
    // The D3D12 texture3D UAV's high metadata word packs its logical W range.
    // Other textures retain their existing array-length interpretation.
    const uint32_t length = WSize ? (uint32_t(WSize) << 16) | FirstWSlice : Texture->arrayLength(View);
    gpu_storage.UAVTexture.metadata = TextureMetadata(length, 0);
    gpu_storage.UAVTexture.padding[0] = 0;
    gpu_storage.UAVTexture.padding[1] = FormatDefaults;
    if (Texture->reservedVolumeLayoutAddress) {
      const auto cast_address = texture_view->reservedVolumeCastLayoutAddress;
      gpu_storage.UAVTexture.padding[0] = (1ull << 62) | (cast_address ? cast_address : Texture->reservedVolumeLayoutAddress);
      gpu_storage.UAVTexture.padding[1] |= (cast_address ? uint64_t(1) << 8 :
          uint64_t(View.mip_start) | uint64_t(View.mip_end - View.mip_start) << 8) << 32;
      gpu_storage.UAVTexture.padding[1] |= uint64_t(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING & 0xfff) << 48;
    }
    if (texture_view && texture_view->msaaSampleAddress) {
      gpu_storage.UAVTexture.padding[0] = texture_view->msaaSampleAddress;
      gpu_storage.UAVTexture.metadata |= 2u | (uint64_t(Texture->sampleCount()) << 8);
    }
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, uint64_t owner_id, Buffer *UAVBuffer, BufferViewKey View, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexelBuffer;
    cpu_storage.UAVTexelBuffer.buffer = UAVBuffer;
    cpu_storage.UAVTexelBuffer.slice = Slice;
    cpu_storage.UAVTexelBuffer.view = View;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    if (UAVBuffer) {
      auto &buffer_view = UAVBuffer->view_(View);
      gpu_storage.UAVTexelBuffer.resource_id = buffer_view.gpu_resource_id;
      gpu_storage.UAVTexelBuffer.metadata = ((uint64_t)Slice.elementCount << 32) | (uint64_t)(Slice.firstElement);
    } else {
      gpu_storage.UAVTexelBuffer.resource_id = 0;
      gpu_storage.UAVTexelBuffer.metadata = 0;
    }
    TypedResidencyStatus(UAVBuffer, View, gpu_storage.UAVTexelBuffer.padding);
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, uint64_t owner_id, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT64 CounterOffsetInBytes, uint64_t counter_owner_id) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index, owner_id, counter_owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::UAVBuffer;
    cpu_storage.UAVBuffer.buffer = UAVBuffer;
    cpu_storage.UAVBuffer.slice = Slice;
    cpu_storage.UAVBuffer.counter = Counter;
    cpu_storage.counter_page = CounterOffsetInBytes / D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    if (UAVBuffer) {
      gpu_storage.UAVBuffer.pointer = UAVBuffer->current()->gpuAddress() + Slice.byteOffset;
      gpu_storage.UAVBuffer.metadata = Slice.byteLength;
      gpu_storage.UAVBuffer.counter_pointer = Counter ? Counter->current()->gpuAddress() + CounterOffsetInBytes : 0;
    } else {
      gpu_storage.UAVBuffer.pointer = 0;
      gpu_storage.UAVBuffer.metadata = 0;
      gpu_storage.UAVBuffer.counter_pointer = 0;
    }
    gpu_storage.UAVBuffer.padding = UAVBuffer ? UAVBuffer->current()->residencyStatusBase : 0;
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, uint64_t owner_id, Buffer *Buffer, BufferViewKey View, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexelBuffer;
    cpu_storage.SRVTexelBuffer.buffer = Buffer;
    cpu_storage.SRVTexelBuffer.slice = Slice;
    cpu_storage.SRVTexelBuffer.view = View;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    if (Buffer) {
      auto &buffer_view = Buffer->view_(View);
      gpu_storage.SRVTexelBuffer.resource_id = buffer_view.gpu_resource_id;
      gpu_storage.SRVTexelBuffer.metadata = ((uint64_t)Slice.elementCount << 32) | (uint64_t)(Slice.firstElement);
    } else {
      gpu_storage.SRVTexelBuffer.resource_id = 0;
      gpu_storage.SRVTexelBuffer.metadata = 0;
    }
    TypedResidencyStatus(Buffer, View, gpu_storage.SRVTexelBuffer.padding);
    return S_OK;
  }

  virtual HRESULT AddRGB32ShaderResourceView(UINT Index, uint64_t owner_id, Buffer *Buffer, BufferSlice Slice,
                                             UINT Shader4ComponentMapping) {
    if (Index >= descriptors_.size() || !Buffer)
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVRGB32Buffer;
    cpu_storage.SRVRGB32Buffer.buffer = Buffer;
    cpu_storage.SRVRGB32Buffer.slice = Slice;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    // The RGB32 branch never uses a native texture handle; q0 carries the residency status base of a
    // reserved buffer (BufferAllocation::residencyStatusBase, 0 otherwise) instead.
    gpu_storage.ZeroFilled[0] = Buffer->current()->residencyStatusBase;
    gpu_storage.ZeroFilled[1] = uint64_t(Slice.elementCount) << 32;
    gpu_storage.ZeroFilled[2] = Buffer->current()->gpuAddress() + Slice.byteOffset;
    gpu_storage.ZeroFilled[3] = (uint64_t(Shader4ComponentMapping) << 32) | 1u;
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, uint64_t owner_id, Buffer *Buffer, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVBuffer;
    cpu_storage.SRVBuffer.buffer = Buffer;
    cpu_storage.SRVBuffer.slice = Slice;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    if (Buffer) {
      gpu_storage.SRVBuffer.pointer = Buffer->current()->gpuAddress() + Slice.byteOffset;
      gpu_storage.SRVBuffer.metadata = Slice.byteLength;
    } else {
      gpu_storage.SRVBuffer.pointer = 0;
      gpu_storage.SRVBuffer.metadata = 0;
    }
    gpu_storage.SRVBuffer.counter_pointer = 0;
    gpu_storage.SRVBuffer.padding = Buffer ? Buffer->current()->residencyStatusBase : 0;
    return S_OK;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, D3D12_SHADER_RESOURCE_VIEW_DESC const *pDesc) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index);
    cpu_storage.type = ShaderVisibleDescriptorType::Null;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.ZeroFilled = {{}};
    return pDesc ? S_OK : E_INVALIDARG;
  }

  /**
   * A raytracing acceleration structure view.
   *
   * The application names the structure by D3D12 GPU virtual address, and that
   * address is the whole descriptor: a shader that reads the slot loads the
   * structure's 64 byte header from it (d3d12_acceleration_structure.hpp) and takes
   * the native structure from the header's resource id, so the runtime keeps no
   * descriptor-to-structure bookkeeping at all. A zero address is D3D12's null
   * acceleration structure: a null descriptor with a zeroed slot, not an error.
   */
  virtual HRESULT AddAccelerationStructure(UINT Index, UINT64 Address) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index);
    cpu_storage.AccelerationStructure.address = Address;
    cpu_storage.type =
        Address ? ShaderVisibleDescriptorType::AccelerationStructure : ShaderVisibleDescriptorType::Null;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.SRVBuffer.pointer = Address;
    gpu_storage.SRVBuffer.metadata = 0;
    gpu_storage.SRVBuffer.counter_pointer = 0;
    gpu_storage.SRVBuffer.padding = 0;
    return S_OK;
  }

  virtual HRESULT
  AddSamplerFeedbackView(UINT Index, uint64_t owner_id, Buffer *Cells, uint64_t CellBytes, bool Paired, uint64_t LayoutWord) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index, owner_id);
    cpu_storage.type = ShaderVisibleDescriptorType::SamplerFeedback;
    cpu_storage.SamplerFeedback.buffer = Cells;
    cpu_storage.SamplerFeedback.slice = {0, CellBytes, 0, uint32_t(CellBytes / 4)};
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.UAVBuffer.pointer = Paired ? Cells->current()->gpuAddress() : 0;
    gpu_storage.UAVBuffer.metadata = LayoutWord;
    gpu_storage.UAVBuffer.counter_pointer = 0;
    gpu_storage.UAVBuffer.padding = 0;
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, D3D12_UNORDERED_ACCESS_VIEW_DESC const *pDesc) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    ResetPoolSlot(Index);
    auto &cpu_storage = WritableDescriptor(Index);
    cpu_storage.type = ShaderVisibleDescriptorType::Null;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.ZeroFilled = {{}};
    return pDesc ? S_OK : E_INVALIDARG;
  }

  virtual ShaderVisibleDescriptorCPUStorage const &
  GetDescriptor(UINT Index) {
    return descriptors_[Index];
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12DescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) {
    auto *destination = static_cast<MTLD3D12DescriptorHeapImpl *>(pHeapTo);
    if (!CopyCount || (destination == this && From == DescriptorTo)) return;
    // A token identifies all CPU and GPU bytes of a complete unpooled block.
    // D3D12 excludes writing the same slots concurrently. Independent writes
    // in a shared block may race only on its atomic invalidation, never on data.
    const UINT head = (-From) & (kDescriptorVersionGranularity - 1);
    if (!buffer_ && destination->buffer_ &&
        (From % kDescriptorVersionGranularity) == (DescriptorTo % kDescriptorVersionGranularity) &&
        CopyCount >= head + kDescriptorVersionGranularity) {
      if (head) {
        CopyDescriptors(From, destination, DescriptorTo, head);
        From += head;
        DescriptorTo += head;
        CopyCount -= head;
      }
      const UINT complete = CopyCount - CopyCount % kDescriptorVersionGranularity;
      unsigned dirty_begin = 0;
      auto copy = [&](unsigned begin, unsigned end) {
        if (begin == end) return;
        CopyDescriptorRecords(destination->descriptors_.data() + DescriptorTo + begin,
            descriptors_.data() + From + begin, end - begin);
        CopyDescriptorRecords(destination->mapped_argument_buffer_ + DescriptorTo + begin,
            mapped_argument_buffer_ + From + begin, end - begin);
      };
      unsigned i = 0;
      for (; i < complete; i += kDescriptorVersionGranularity) {
        auto &source_block = blocks_[(From + i) / kDescriptorVersionGranularity];
        auto &target_block = destination->blocks_[(DescriptorTo + i) / kDescriptorVersionGranularity];
        const auto known = source_block.copy_token.load(std::memory_order_relaxed);
        const auto target = target_block.copy_token.load(std::memory_order_relaxed);
        if (known && target == known) {
          copy(dirty_begin, i);
          dirty_begin = i + kDescriptorVersionGranularity;
          continue;
        }
        if (source_block.pooled.load(std::memory_order_relaxed) ||
            target_block.pooled.load(std::memory_order_relaxed)) break;
        target_block.version.fetch_add(1, std::memory_order_release);
        uint64_t changed = UINT64_MAX;
        const auto token = known ? known : source_block.PublishSource(target, changed);
        target_block.copy_token.store(token, std::memory_order_relaxed);
        if (changed != UINT64_MAX) {
          copy(dirty_begin, i);
          if (changed) {
            const unsigned first = std::countr_zero(changed), end = 64 - std::countl_zero(changed);
            const unsigned first_end = first + std::countr_one(changed >> first);
            const uint64_t rest = first_end < end ? changed >> first_end : 0;
            // Split only two separated runs; dense or fragmented masks retain a single bulk copy.
            const unsigned gap = std::countr_zero(rest);
            if (rest && gap >= 4 && std::has_single_bit((rest >> gap) + 1)) {
              copy(i + first, i + first_end);
              copy(i + first_end + gap, i + end);
            } else {
              copy(i + first, i + end);
            }
          }
          dirty_begin = i + kDescriptorVersionGranularity;
        }
      }
      copy(dirty_begin, i);
      if (i == CopyCount) return;
      From += i;
      DescriptorTo += i;
      CopyCount -= i;
    }
    destination->InvalidateDescriptors(DescriptorTo, CopyCount);
    // Non-pooled slots have no native view or retained texture to replace, including on heaps without a pool.
    if (CopyCount == 1 && !pool_slots_[From].texture && !destination->pool_slots_[DescriptorTo].texture) {
      destination->descriptors_[DescriptorTo] = descriptors_[From];
      destination->mapped_argument_buffer_[DescriptorTo] = mapped_argument_buffer_[From];
      return;
    }
    if ((!view_pool_ && !destination->view_pool_) ||
        (CopyCount > 1 && !HasPooledSlots(From, CopyCount) && !destination->HasPooledSlots(DescriptorTo, CopyCount))) {
      CopyDescriptorRecords(destination->descriptors_.data() + DescriptorTo, descriptors_.data() + From, CopyCount);
      CopyDescriptorRecords(destination->mapped_argument_buffer_ + DescriptorTo, mapped_argument_buffer_ + From, CopyCount);
      return;
    }
    const bool overlap = destination == this && DescriptorTo < From + CopyCount && From < DescriptorTo + CopyCount;
    const bool reverse = overlap && DescriptorTo > From;
    unsigned single_copy_until = 0;
    for (unsigned n = 0; n < CopyCount;) {
      if (!overlap && n >= single_copy_until && view_pool_ && destination->view_pool_) {
        unsigned range = 0;
        while (n + range < CopyCount && pool_slots_[From + n + range].texture)
          ++range;
        if (range > 1) {
          const uint64_t source_id = mapped_argument_buffer_[From + n].SRVTexture.resource_id;
          // The native range copy replaces every destination texture itself; a failed copy changes
          // nothing and the slots fall through to the per-descriptor path below.
          const uint64_t destination_id = destination->view_pool_.copyTextureViews(
              view_pool_, From + n, DescriptorTo + n, range);
          if (destination_id) {
            destination->MarkPooledRange(DescriptorTo + n, range);
            CopyDescriptorRecords(destination->descriptors_.data() + DescriptorTo + n, descriptors_.data() + From + n,
                                  range);
            for (unsigned i = 0; i < range; ++i) {
              auto gpu = mapped_argument_buffer_[From + n + i];
              destination->pool_slots_[DescriptorTo + n + i] = pool_slots_[From + n + i];
              gpu.SRVTexture.resource_id = destination_id +
                  (gpu.SRVTexture.resource_id - source_id);
              destination->mapped_argument_buffer_[DescriptorTo + n + i] = gpu;
            }
            n += range;
            continue;
          }
          // The ordinary-view fallback copies one slot at a time. Do not retry every suffix of this run.
          single_copy_until = n + range;
        }
      }
      const unsigned i = reverse ? CopyCount - n - 1 : n;
      const auto &slot = pool_slots_[From + i];
      auto gpu = mapped_argument_buffer_[From + i];
      // The native slot copy replaces the destination texture, so only a non-pool source needs the clear.
      destination->descriptors_[DescriptorTo + i] = descriptors_[From + i];
      if (slot.texture) {
        destination->MarkPooled(DescriptorTo + i);
        destination->pool_slots_[DescriptorTo + i] = slot;
        gpu.SRVTexture.resource_id = destination->view_pool_.copyTextureView(
            view_pool_, From + i, DescriptorTo + i, slot.texture, slot.info);
        // A rejected copy leaves the old native slot; clear it as the reset would have.
        if (!gpu.SRVTexture.resource_id)
          destination->ResetPoolSlot(DescriptorTo + i);
      } else {
        destination->ResetPoolSlot(DescriptorTo + i);
      }
      destination->mapped_argument_buffer_[DescriptorTo + i] = gpu;
      ++n;
    }
  }
};

class MTLD3D12RenderTargetDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<MTL_RENDER_TARGET_DESC> render_targets_;
#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  DescriptorHandleRangeOwner handles_;
#endif

public:
  MTLD3D12RenderTargetDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: {
      if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        return E_INVALIDARG;
      render_targets_.resize(pDesc->NumDescriptors);
      break;
    }
    default:
      return E_INVALIDARG;
    }
    InitializeResidency(true);
#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
    return handles_.Register(static_cast<MTLD3D12RenderTargetDescriptorHeap *>(this),
                             DescriptorHandleKind::RenderTarget, pDesc->NumDescriptors);
#else
    return S_OK;
#endif
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) {
    *__ret = GetRenderTargetDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) {
    __ret->ptr = 0;
    return __ret;
  }

  virtual HRESULT
  AddRenderTarget(UINT Index, MTL_RENDER_TARGET_DESC const *pDesc) {
    if (Index >= render_targets_.size())
      return E_INVALIDARG;
    if (pDesc)
      render_targets_[Index] = *pDesc;
    else
      render_targets_[Index] = {};
    return S_OK;
  }

  virtual MTL_RENDER_TARGET_DESC
  GetRenderTarget(UINT Index) {
    return render_targets_[Index];
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12RenderTargetDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) {
    if (CopyCount)
      CopyDescriptorRecords(static_cast<MTLD3D12RenderTargetDescriptorHeapImpl *>(pHeapTo)->render_targets_.data() +
          DescriptorTo, render_targets_.data() + From, CopyCount);
  }
};

struct SamplerGPUStorage {
  uint64_t sampler;
  uint64_t cube_sampler;
  uint64_t metadata;
  uint64_t padding;
  uint32_t uint_border[4];
  float float_border[4];
};
static_assert(sizeof(SamplerGPUStorage) == kSamplerDescriptorStride);

class MTLD3D12SamplerDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<Rc<Sampler>> samplers_;

  Rc<Buffer> buffer_;
  SamplerGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;
#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  DescriptorHandleRangeOwner handles_;
#endif

public:
  MTLD3D12SamplerDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap>(pDevice) {}

  WMT::Buffer GetArgumentBuffer() const override {
    return buffer_ ? buffer_->current()->buffer() : WMT::Buffer{};
  }

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
      break;
    }
    default:
      return E_INVALIDARG;
    }
    samplers_.resize(pDesc->NumDescriptors);

    if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      buffer_ = new Buffer(samplers_.size() * sizeof(SamplerGPUStorage), device_->GetMTLDevice());

      Flags<BufferAllocationFlag> flags;
#ifdef __i386__
      /* The CPU writes samplers into this argument buffer; keep it in 32-bit-addressable storage. */
      flags.set(BufferAllocationFlag::CpuPlaced);
#endif
      buffer_->rename(buffer_->allocate(flags));
      if (!buffer_->current())
        return E_OUTOFMEMORY;
      mapped_argument_buffer_ = reinterpret_cast<SamplerGPUStorage *>(buffer_->current()->mappedMemory(0));
      if (!mapped_argument_buffer_)
        return E_OUTOFMEMORY;
      argument_buffer_gpu_address_ = buffer_->current()->gpuAddress();
      HRESULT hr = device_->RegisterResidencyAndVA(buffer_->current(), this);
      if (FAILED(hr))
        return hr;
    } else {
      mapped_argument_buffer_ =
          reinterpret_cast<SamplerGPUStorage *>(calloc(samplers_.size(), sizeof(SamplerGPUStorage)));
      if (!samplers_.empty() && !mapped_argument_buffer_)
        return E_OUTOFMEMORY;
      InitializeResidency(true);
    }

#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
    return handles_.Register(static_cast<MTLD3D12SamplerDescriptorHeap *>(this), DescriptorHandleKind::Sampler,
                             pDesc->NumDescriptors);
#else
    return S_OK;
#endif
  };

  ~MTLD3D12SamplerDescriptorHeapImpl() {
     if (buffer_) {
      device_->UnregisterResidencyAndVA(buffer_->current(), this);
    } else {
      free(mapped_argument_buffer_);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) {
    *__ret = GetSamplerDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) {
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }


  virtual HRESULT
  AddSampler(UINT Index, const D3D12_SAMPLER_DESC *pDesc, UINT Flags, const UINT *UintBorderColor) {
    if (!pDesc)
      return E_INVALIDARG;
    if (Index >= samplers_.size())
      return E_INVALIDARG;
  
    WMTSamplerInfo info;
    PopulateWMTSamplerInfo(info, *pDesc);
    const bool non_normalized = Flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
    if (non_normalized) {
      if (!sampler_feedback::ValidNonNormalizedSampler(*pDesc))
        return E_INVALIDARG;
      // Shader lowering divides pixel coordinates by the view dimensions, allowing BORDER filtering.
      info.normalized_coords = true;
      info.mip_filter = WMTSamplerMipFilterNearest;
      info.max_anisotroy = 1;
    }
    auto sampler = device_->owned_native_states.SamplerState(device_->GetMTLDevice(), info, pDesc->MipLODBias,
                                          SamplerReductionMode(pDesc->Filter));
    if (!sampler)
      return E_OUTOFMEMORY;

    samplers_[Index] = sampler;
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.sampler = sampler->sampler_state_handle;
    gpu_storage.cube_sampler = sampler->sampler_state_cube_handle;
    gpu_storage.metadata = sampler_feedback::SamplerMetadata(
        sampler->lod_bias,
        sampler_feedback::SamplerFlags(pDesc->Filter, pDesc->AddressU, pDesc->AddressV, pDesc->MaxAnisotropy,
                                       non_normalized, pDesc->AddressW, UintBorderColor != nullptr, pDesc->ComparisonFunc));
    gpu_storage.padding = sampler_feedback::SamplerLodRange(pDesc->MinLOD, pDesc->MaxLOD);
    for (unsigned channel = 0; channel < 4; ++channel)
      gpu_storage.uint_border[channel] = UintBorderColor ? UintBorderColor[channel] : 0;
    std::copy_n(pDesc->BorderColor, 4, gpu_storage.float_border);

    return S_OK;
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12SamplerDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) {
    auto *destination = static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(pHeapTo);
    if (!CopyCount || (destination == this && From == DescriptorTo)) return;
    auto first = samplers_.begin() + From;
    auto target = destination->samplers_.begin() + DescriptorTo;
    // The CPU records retain their samplers. Only the shader-visible record is a byte copy.
    if (CopyCount == 1) {
      if (*first != *target) *target = *first;
      destination->mapped_argument_buffer_[DescriptorTo] = mapped_argument_buffer_[From];
      return;
    }
    if (destination == this && DescriptorTo > From && DescriptorTo - From < CopyCount)
      std::copy_backward(first, first + CopyCount, target + CopyCount);
    else if (*first == *target) {
      for (UINT i = 1; i < CopyCount; ++i)
        if (first[i] != target[i]) target[i] = first[i];
    } else
      std::copy_n(first, CopyCount, target);
    CopyDescriptorRecords(destination->mapped_argument_buffer_ + DescriptorTo, mapped_argument_buffer_ + From, CopyCount);
  }
};

HRESULT
CreateDescriptorHeap(
    MTLD3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap
) {
  InitReturnPtr(ppDescriptorHeap);
  if (!pDesc)
    return E_INVALIDARG;
  if (pDesc->NumDescriptors > 0xFFFFF ||
      ((pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
       ((pDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && pDesc->NumDescriptors > 1000000) ||
        (pDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && pDesc->NumDescriptors > 2048)))) {
    ERR("CreateDescriptorHeap: NumDescriptors is too large");
    return E_INVALIDARG;
  }
  switch (pDesc->Type) {
  case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
    auto descriptor_heap = Com(new MTLD3D12DescriptorHeapImpl(pDevice));
    HRESULT hr = descriptor_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return descriptor_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
    auto sampler_heap = Com(new MTLD3D12SamplerDescriptorHeapImpl(pDevice));
    HRESULT hr = sampler_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return sampler_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
  case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: {
    auto descriptor_heap = Com(new MTLD3D12RenderTargetDescriptorHeapImpl(pDevice));
    HRESULT hr = descriptor_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return descriptor_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  default:
    break;
  }
  return E_INVALIDARG;
}

} // namespace d3dalloy
