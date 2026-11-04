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
#include "d3d12.h"
#include "d3dalloy_buffer.hpp"
#include "d3dalloy_texture.hpp"
#include <cstdint>
#include <utility>

#if UINTPTR_MAX == 0xffffffffffffffffULL
#define D3DALLOY_USE_EMBEDDED_HEAP_POINTER
#endif

namespace d3dalloy {

struct EMBEDDED_DESCRIPTOR_HANDLE {
#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
  SIZE_T Tag        : 5;
  SIZE_T Descriptor : 20;
  SIZE_T Heap       : 7;
#else
  SIZE_T Tag        : 5;
  SIZE_T Descriptor : 20;
  SIZE_T Heap       : 39;

  // assume pointer is 8-byte aligned, providing 3 free bits
  template <typename T>
  T *
  extract() {
    return reinterpret_cast<T *>((Heap << 8) | (Tag << 3));
  }

  EMBEDDED_DESCRIPTOR_HANDLE(const void *heap, SIZE_T index) {
    Heap = (SIZE_T)heap >> 8;
    Tag = (SIZE_T)heap >> 3;
    Descriptor = index;
  }
#endif

  EMBEDDED_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
    union {
      D3D12_CPU_DESCRIPTOR_HANDLE d3d12;
      EMBEDDED_DESCRIPTOR_HANDLE impl;
    } _;
    _.d3d12 = Handle;
    *this = _.impl;
  }

  operator D3D12_CPU_DESCRIPTOR_HANDLE() const {
    union {
      D3D12_CPU_DESCRIPTOR_HANDLE d3d12;
      EMBEDDED_DESCRIPTOR_HANDLE impl;
    } _;
    _.impl = *this;
    return _.d3d12;
  };

  EMBEDDED_DESCRIPTOR_HANDLE() = default;
};

static_assert(sizeof(EMBEDDED_DESCRIPTOR_HANDLE) == sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
constexpr uint32_t kSamplerDescriptorStride = 64;
// Coarse invalidation avoids making an unrelated part of a large heap cold.
constexpr uint32_t kDescriptorVersionGranularity = 64;

#ifndef D3DALLOY_USE_EMBEDDED_HEAP_POINTER
/*
 * 32-bit CPU descriptor handles. A 32-bit SIZE_T cannot embed a heap pointer and a 20-bit index,
 * so each heap reserves a disjoint range of a process-wide 32-bit handle space instead: its handle
 * for descriptor i is Base + i * its kind's descriptor stride, the same increment
 * GetDescriptorHandleIncrementSize reports on 64-bit. Handle 0 is never allocated. A lookup
 * resolves only a handle inside a live range of the requested heap kind, on a stride boundary;
 * anything else resolves to a null heap. Ranges are registered by a heap's successful
 * initialization and released by its destruction, under a lock shared with lookups.
 */
enum class DescriptorHandleKind : uint8_t { ShaderVisible, Sampler, RenderTarget };
constexpr uint32_t kDescriptorHandleStride = 32;

/** Reserves a range for `count` descriptors; returns its base handle, or 0 when the space is exhausted. */
SIZE_T RegisterDescriptorHandleRange(void *heap, DescriptorHandleKind kind, uint32_t count);
void UnregisterDescriptorHandleRange(SIZE_T base);
/** The handle of descriptor `index` of a registered heap, or 0 for an unregistered heap. */
SIZE_T DescriptorHandleOf(const void *heap, uint32_t index);
/** The heap of `kind` owning `handle` and the descriptor index, or {nullptr, 0}. */
std::pair<void *, uint32_t> ResolveDescriptorHandle(SIZE_T handle, DescriptorHandleKind kind);
#endif

enum class ShaderVisibleDescriptorType {
  Null,
  SRVTexture,
  ConstantBuffer,
  UAVTexture,
  UAVTexelBuffer,
  UAVBuffer,
  SRVTexelBuffer,
  SRVRGB32Buffer,
  SRVBuffer,
  AccelerationStructure,
  /* A sampler feedback UAV (CreateSamplerFeedbackUnorderedAccessView): UAVBuffer storage over the map cells. */
  SamplerFeedback,
};

struct SRVTextureCPUStorage {
  Texture *texture = nullptr;
  TextureViewKey view{};
};

struct UAVTextureCPUStorage {
  Texture *texture = nullptr;
  TextureViewKey view{};
  // Only texture3D UAVs use a W-slice range; the native view spans the mip.
  uint16_t first_w_slice = 0;
  uint16_t w_size = 0;
};

struct UAVTexelBufferCPUStorage {
  Buffer *buffer = nullptr;
  BufferViewKey view{};
  BufferSlice slice{};
};

using SRVTexelBufferCPUStorage = UAVTexelBufferCPUStorage;

struct CBVCommonStorage {
  uint64_t address = 0;
  uint64_t size = 0;
};

struct UAVBufferCPUStorage {
  Buffer *buffer = nullptr;
  BufferSlice slice{};
  Buffer *counter = nullptr;
  uint64_t counter_offset = 0;
};

using SRVBufferCPUStorage = UAVBufferCPUStorage;

/**
 * A D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE view.
 *
 * Unlike every other view this one names an acceleration structure by D3D12 GPU
 * virtual address rather than by resource. The address is what the shader needs:
 * the traversal loads the structure's 64 byte header from it and takes the native
 * structure out of the header's resource id, so no descriptor-to-structure
 * resolution happens on the CPU.
 */
struct AccelerationStructureCPUStorage {
  uint64_t address = 0;
};

struct ShaderVisibleDescriptorCPUStorage {
  ShaderVisibleDescriptorType type;
  // Borrowed pointers are dereferenced only after acquiring this live owner.
  uint64_t owner_id = 0;
  uint64_t counter_owner_id = 0;
  union {
    SRVTextureCPUStorage SRVTexture;
    CBVCommonStorage ConstantBuffer;
    UAVTextureCPUStorage UAVTexture;
    UAVTexelBufferCPUStorage UAVTexelBuffer;
    UAVBufferCPUStorage UAVBuffer;
    SRVTexelBufferCPUStorage SRVTexelBuffer;
    SRVBufferCPUStorage SRVRGB32Buffer;
    SRVBufferCPUStorage SRVBuffer;
    AccelerationStructureCPUStorage AccelerationStructure;
    UAVBufferCPUStorage SamplerFeedback;
  };

  ShaderVisibleDescriptorCPUStorage() : type(ShaderVisibleDescriptorType::Null) {}
};

class MTLD3D12DescriptorHeap : public ID3D12DescriptorHeap {
public:
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  virtual void SetDescriptorOwner(UINT index, uint64_t owner_id, uint64_t counter_owner_id = 0) = 0;
  /* Native identity allows a recording to retain the descriptor pool and its backing residency. */
  virtual WMT::Buffer GetArgumentBuffer() const = 0;

  virtual HRESULT
  AddShaderResourceView(UINT Index, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp,
                        bool CubeView, bool CenterSamplePattern, UINT64 FormatDefaults) = 0;

  virtual HRESULT AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) = 0;

  virtual HRESULT AddUnorderedAccessView(
      UINT Index, Texture *Texture, TextureViewKey View, uint16_t FirstWSlice, uint16_t WSize, UINT FormatDefaults
  ) = 0;

  virtual HRESULT AddUnorderedAccessView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice) = 0;

  virtual HRESULT AddUnorderedAccessView(
      UINT Index, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT64 CounterOffsetInBytes
  ) = 0;

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice) = 0;

  virtual HRESULT AddRGB32ShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice,
                                             UINT Shader4ComponentMapping) = 0;

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice) = 0;

  virtual HRESULT AddShaderResourceView(UINT Index, D3D12_SHADER_RESOURCE_VIEW_DESC const *pDesc) = 0;

  virtual HRESULT AddUnorderedAccessView(UINT Index, D3D12_UNORDERED_ACCESS_VIEW_DESC const *pDesc) = 0;

  /**
   * A raytracing acceleration structure view.
   *
   * The application names the structure by D3D12 GPU virtual address. The shader
   * reads that address out of the descriptor slot and loads the structure's header
   * from it (d3d12_acceleration_structure.hpp), so the address is the whole payload:
   * the backend never has to keep a side list of which slots hold structures. A zero
   * address is D3D12's null acceleration structure: a null descriptor, not an error.
   */
  virtual HRESULT AddAccelerationStructure(UINT Index, UINT64 Address) = 0;

  /**
   * A sampler feedback UAV: q0 = the map's cell array address (0 when the paired texture is null,
   * so feedback writes are dropped), q1 = its layout word (d3d12_sampler_feedback.hpp).
   */
  virtual HRESULT AddSamplerFeedbackView(UINT Index, Buffer *Cells, uint64_t CellBytes, bool Paired,
                                         uint64_t LayoutWord) = 0;

  virtual ShaderVisibleDescriptorCPUStorage const &GetDescriptor(UINT Index) = 0;
  virtual uint64_t GetDescriptorVersion(UINT Index) const = 0;
  // Shader-visible view identity, which may be a pooled slot rather than an
  // ordinary native texture view. The caller retains this heap while using it.
  virtual uint64_t GetTextureResourceId(UINT Index) const = 0;

  virtual void CopyDescriptors(UINT From, MTLD3D12DescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) = 0;
};

class MTLD3D12SamplerDescriptorHeap : public ID3D12DescriptorHeap {
public:
  virtual HRESULT AddSampler(UINT Index, const D3D12_SAMPLER_DESC *Desc, UINT Flags = 0,
                             const UINT *UintBorderColor = nullptr) = 0;

  virtual void CopyDescriptors(UINT From, MTLD3D12SamplerDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) = 0;

  // Borrowed stable shader-visible storage; retain the heap through GPU retirement.
  virtual WMT::Buffer GetArgumentBuffer() const = 0;
};

struct MTL_RENDER_TARGET_DESC {
  Texture *Texture = nullptr;
  TextureViewKey View{};
  Buffer *BufferResource = nullptr;
  BufferViewKey BufferView{};
  BufferSlice BufferRange{};
  WMTPixelFormat Format = WMTPixelFormatInvalid;
  UINT Width = 0;
  UINT Height = 0;
  UINT DepthPlane = 0;
  UINT RenderTargetArrayLength = 0;
  bool CenterSamplePattern = false;
  UINT Flags = 0;
  /* Never-reused owning resource id (MTLD3D12Resource::owned_id) for texture views; 0 otherwise. */
  uint64_t Owner = 0;
};

class MTLD3D12RenderTargetDescriptorHeap : public ID3D12DescriptorHeap {
public:
  virtual HRESULT AddRenderTarget(UINT Index, MTL_RENDER_TARGET_DESC const *pDesc) = 0;
  virtual MTL_RENDER_TARGET_DESC GetRenderTarget(UINT Index) = 0;

  virtual void CopyDescriptors(UINT From, MTLD3D12RenderTargetDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) = 0;
};

} // namespace d3dalloy
