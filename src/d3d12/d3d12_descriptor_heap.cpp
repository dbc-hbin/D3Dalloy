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
#include "dxmt_sampler.hpp"
#include "log/log.hpp"
#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
#include <map>
#include <shared_mutex>
#endif

namespace dxmt {

#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
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
  Rc<Buffer> buffer_;
  ShaderVisibleDescriptorGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;
#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
  DescriptorHandleRangeOwner handles_;
#endif

public:
  MTLD3D12DescriptorHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12DescriptorHeap>(pDevice) {}

  void SetDescriptorOwner(UINT index, uint64_t owner_id, uint64_t counter_owner_id) override {
    if (index >= descriptors_.size()) return;
    descriptors_[index].owner_id = owner_id;
    descriptors_[index].counter_owner_id = counter_owner_id;
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

#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
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
  AddShaderResourceView(UINT Index, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp,
                        bool CubeView, bool CenterSamplePattern, UINT64 FormatDefaults) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexture;
    cpu_storage.SRVTexture.texture = Texture;
    cpu_storage.SRVTexture.view = View;
    auto &texture_view = Texture->view(View);
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.SRVTexture.resource_id = texture_view.gpuResourceID;
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
      const auto cast_address = texture_view.reservedVolumeCastLayoutAddress;
      gpu_storage.SRVTexture.padding[0] = (1ull << 62) | (cast_address ? cast_address : Texture->reservedVolumeLayoutAddress);
      gpu_storage.SRVTexture.padding[1] |= (cast_address ? uint64_t(1) << 8 :
          uint64_t(View.mip_start) | uint64_t(View.mip_end - View.mip_start) << 8) << 32;
    }
    if (texture_view.msaaSampleAddress) {
      gpu_storage.SRVTexture.padding[0] = texture_view.msaaSampleAddress;
      gpu_storage.SRVTexture.metadata |= 2u | (uint64_t(Texture->sampleCount()) << 8);
    }
    return S_OK;
  }
    virtual HRESULT
  AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
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
  AddUnorderedAccessView(UINT Index, Texture *Texture, TextureViewKey View, uint16_t FirstWSlice, uint16_t WSize, UINT FormatDefaults) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexture;
    cpu_storage.UAVTexture.texture = Texture;
    cpu_storage.UAVTexture.view = View;
    cpu_storage.UAVTexture.first_w_slice = FirstWSlice;
    cpu_storage.UAVTexture.w_size = WSize;
    auto &texture_view = Texture->view(View);
    auto &gpu_storage = mapped_argument_buffer_[Index];
    gpu_storage.UAVTexture.resource_id = texture_view.gpuResourceID;
    // The D3D12 texture3D UAV's high metadata word packs its logical W range.
    // Other textures retain their existing array-length interpretation.
    const uint32_t length = WSize ? (uint32_t(WSize) << 16) | FirstWSlice : Texture->arrayLength(View);
    gpu_storage.UAVTexture.metadata = TextureMetadata(length, 0);
    gpu_storage.UAVTexture.padding[0] = 0;
    gpu_storage.UAVTexture.padding[1] = FormatDefaults;
    if (Texture->reservedVolumeLayoutAddress) {
      const auto cast_address = texture_view.reservedVolumeCastLayoutAddress;
      gpu_storage.UAVTexture.padding[0] = (1ull << 62) | (cast_address ? cast_address : Texture->reservedVolumeLayoutAddress);
      gpu_storage.UAVTexture.padding[1] |= (cast_address ? uint64_t(1) << 8 :
          uint64_t(View.mip_start) | uint64_t(View.mip_end - View.mip_start) << 8) << 32;
      gpu_storage.UAVTexture.padding[1] |= uint64_t(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING & 0xfff) << 48;
    }
    if (texture_view.msaaSampleAddress) {
      gpu_storage.UAVTexture.padding[0] = texture_view.msaaSampleAddress;
      gpu_storage.UAVTexture.metadata |= 2u | (uint64_t(Texture->sampleCount()) << 8);
    }
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferViewKey View, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
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
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT64 CounterOffsetInBytes) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
    cpu_storage.type = ShaderVisibleDescriptorType::UAVBuffer;
    cpu_storage.UAVBuffer.buffer = UAVBuffer;
    cpu_storage.UAVBuffer.slice = Slice;
    cpu_storage.UAVBuffer.counter = Counter;
    cpu_storage.UAVBuffer.counter_offset = CounterOffsetInBytes;
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

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
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

  virtual HRESULT AddRGB32ShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice,
                                             UINT Shader4ComponentMapping) {
    if (Index >= descriptors_.size() || !Buffer)
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
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

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
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
    auto &cpu_storage = descriptors_[Index];
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
    auto &cpu_storage = descriptors_[Index];
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
  AddSamplerFeedbackView(UINT Index, Buffer *Cells, uint64_t CellBytes, bool Paired, uint64_t LayoutWord) {
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index];
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
    auto &cpu_storage = descriptors_[Index];
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
    for (unsigned i = 0; i < CopyCount; i++) {
      destination->descriptors_[DescriptorTo + i] = descriptors_[From + i];
      destination->mapped_argument_buffer_[DescriptorTo + i] = mapped_argument_buffer_[From + i];
    }
  }
};

class MTLD3D12RenderTargetDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<MTL_RENDER_TARGET_DESC> render_targets_;
#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
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
#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
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
    for (unsigned i = 0; i < CopyCount; i++) {
      static_cast<MTLD3D12RenderTargetDescriptorHeapImpl *>(pHeapTo)->render_targets_[DescriptorTo + i] =
          render_targets_[From + i];
    }
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
#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
  DescriptorHandleRangeOwner handles_;
#endif

public:
  MTLD3D12SamplerDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap>(pDevice) {}

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

#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
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
    PopulateWMTSamplerInfo(device_->GetMTLDevice(), info, *pDesc);
    const bool non_normalized = Flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
    if (non_normalized) {
      if (!sampler_feedback::ValidNonNormalizedSampler(*pDesc))
        return E_INVALIDARG;
      // Shader lowering divides pixel coordinates by the view dimensions, allowing BORDER filtering.
      info.normalized_coords = true;
      info.mip_filter = WMTSamplerMipFilterNearest;
      info.max_anisotroy = 1;
    }
    auto sampler = Sampler::createSampler(device_->GetMTLDevice(), info, pDesc->MipLODBias,
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
    for (unsigned i = 0; i < CopyCount; i++) {
      static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(pHeapTo)->samplers_[DescriptorTo + i] = samplers_[From + i];
      static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(pHeapTo)->mapped_argument_buffer_[DescriptorTo + i] =
          mapped_argument_buffer_[From + i];
    }
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

} // namespace dxmt
