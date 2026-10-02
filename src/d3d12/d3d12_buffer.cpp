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
#include "d3d12_pageable.hpp"
#include "d3d12_resource_contract.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_format.hpp"
#include <cstring>
#include <new>

namespace dxmt {

namespace {

HRESULT
BuildBufferSlice(
    UINT64 buffer_size, UINT64 first_element, UINT num_elements, UINT stride, bool texel_view, BufferSlice &slice
) {
  if (!stride || !num_elements)
    return E_INVALIDARG;
  if (first_element > UINT64_MAX / stride || static_cast<UINT64>(num_elements) > UINT64_MAX / stride)
    return E_INVALIDARG;

  const UINT64 byte_offset = first_element * stride;
  const UINT64 byte_length = static_cast<UINT64>(num_elements) * stride;
  if (byte_offset > buffer_size || byte_length > buffer_size - byte_offset)
    return E_INVALIDARG;
  slice.firstElement = texel_view ? 0 : static_cast<uint32_t>(first_element);
  slice.elementCount = num_elements;
  slice.byteOffset = byte_offset;
  slice.byteLength = byte_length;
  return S_OK;
}

} // namespace

class MTLD3D12Buffer : public MTLD3D12Pageable<MTLD3D12Resource> {
  D3D12_RESOURCE_DESC desc_;
  D3D12_HEAP_PROPERTIES heap_props_;
  D3D12_HEAP_FLAGS heap_flags_;
  Com<MTLD3D12Heap> placed_heap_;
  bool owns_registration_ = false;

public:
  MTLD3D12Buffer(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Resource>(pDevice) {}

  HRESULT
  Initialize(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, MTLD3D12Heap *pHeap, UINT64 Offset
  ) {
    if (OptimizedClearValue)
      return E_INVALIDARG;

    desc_ = *pDesc;
    heap_props_ = *pHeapProps;
    heap_flags_ = HeapFlags;

    if (desc_.Alignment) {
      if (desc_.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        return E_INVALIDARG;
    } else {
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }

    if (pHeap) {
      auto size_and_align = device_->GetMTLDevice().heapBufferSizeAndAlign(desc_.Width, {});
      const UINT64 heap_size = pHeap->GetDesc().SizeInBytes;
      if (!resource_contract::PlacementFits(
              Offset, size_and_align.size, heap_size, desc_.Alignment, size_and_align.align
          ))
        return E_INVALIDARG;
    }

    try {
      buffer = new Buffer(desc_.Width, device_->GetMTLDevice());
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }

    Flags<BufferAllocationFlag> flags;
    // GPU upload heaps: CPU writes go through write-combined, uncached pages (the GPU reads them in place).
    if (heap_props_.Type == kHeapTypeGpuUpload)
      flags.set(BufferAllocationFlag::CpuWriteCombined);
    if (pHeap && pHeap->external_memory) {
      // Shared or host-memory heap: alias its memory at the placement offset.
      buffer->rename(buffer->allocate(flags, pHeap->external_memory + Offset));
    } else if (!pHeap && (HeapFlags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER))) {
      heap_flags_ |= D3D12_HEAP_FLAG_SHARED;
      if (!share) {
        HRESULT hr = sharing::Create(sharing::Kind::Buffer, desc_.Width, share);
        if (FAILED(hr))
          return hr;
        share->header().heap_props = heap_props_;
        share->header().heap_flags = heap_flags_;
        share->header().resource_desc = desc_;
      }
      buffer->rename(buffer->allocate(flags, share->payload()));
    } else {
#ifdef __i386__
      /* Map() hands the CPU mapping to a 32-bit caller; back CPU-visible buffers with caller-addressable
       * storage: private memory for committed ones, the heap's host storage for placed ones. */
      if (!pHeap && IsCpuVisibleHeap(&heap_props_))
        flags.set(BufferAllocationFlag::CpuPlaced);
      if (pHeap && pHeap->host_memory)
        buffer->rename(buffer->allocate(flags, pHeap->host_memory + Offset));
      else
#endif
      if (pHeap)
        buffer->rename(buffer->allocate(flags, pHeap->heap, Offset));
      else
        buffer->rename(buffer->allocate(flags));
    }
    if (!buffer->current() || !buffer->current()->buffer())
      return E_OUTOFMEMORY;
    owns_registration_ = !pHeap;
    HRESULT hr = device_->RegisterResidencyAndVA(
        buffer->current(), owns_registration_ ? this : nullptr,
        pHeap || !(HeapFlags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT)
    );
    if (SUCCEEDED(hr) && pHeap)
      placed_heap_ = pHeap;
    return hr;
  };

  ~MTLD3D12Buffer() {
    if (buffer && buffer->current())
      device_->UnregisterResidencyAndVA(buffer->current(), owns_registration_ ? this : nullptr);
    if (reserved && reserved->status_buffer)
      device_->UnregisterResidency(reserved->status_buffer);
  }

  /*
   * A reserved buffer: a Metal placement sparse buffer whose 64KB pages start unmapped. Its GPU virtual
   * address is permanent, so root descriptors and views work before any tile is mapped. A CPU-visible
   * byte per tile mirrors the mapping state in queue order for shader residency status.
   */
  HRESULT
  InitializeReserved(const D3D12_RESOURCE_DESC *pDesc) {
    desc_ = *pDesc;
    heap_props_ = {};
    heap_flags_ = D3D12_HEAP_FLAG_NONE;
    if (desc_.Alignment && desc_.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
      return E_INVALIDARG;
    desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    HRESULT hr = ReservedResource::CreateBufferTiling(desc_, reserved);
    if (FAILED(hr))
      return hr;
    auto metal = device_->GetMTLDevice();
    const uint64_t length = uint64_t(reserved->totalTiles()) * kReservedTileBytes;
    if (length > metal.maxBufferLength())
      return E_OUTOFMEMORY;

    WMTBufferInfo status_info{};
    status_info.length = std::max<uint64_t>(reserved->totalTiles(), 16);
    status_info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
    reserved->status_buffer = metal.newBuffer(status_info);
    reserved->status_bytes = static_cast<uint8_t *>(status_info.memory.get_accessible_or_null());
    if (!reserved->status_buffer || !reserved->status_bytes)
      return E_OUTOFMEMORY;
    memset(reserved->status_bytes, 0, status_info.length);
    hr = device_->RegisterResidency(reserved->status_buffer);
    if (FAILED(hr)) {
      reserved->status_buffer = {};
      return hr;
    }

    try {
      buffer = new Buffer(length, metal);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    buffer->rename(buffer->allocateSparse(WMTSparsePageSize64));
    if (!buffer->current() || !buffer->current()->buffer())
      return E_OUTOFMEMORY;
    buffer->current()->residencyStatusBase =
        status_info.gpu_address - (buffer->current()->gpuAddress() >> 16);
    owns_registration_ = true;
    return device_->RegisterResidencyAndVA(buffer->current(), this, true);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource) ||
        riid == __uuidof(ID3D12Resource1) || riid == __uuidof(ID3D12Resource2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Buffer: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData) {
    if (device_->RemovedReason() != S_OK) {
      if (ppData)
        *ppData = nullptr;
      return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (Subresource)
      return E_INVALIDARG;
    if (!IsCpuVisibleHeap(&heap_props_))
      return E_INVALIDARG;
    if (ppData)
      *ppData = buffer->current()->mappedMemory(0);
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange) {
    // no-op
  };

  virtual D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  virtual D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() {
    return buffer->current()->gpuAddress();
  };

  virtual HRESULT STDMETHODCALLTYPE
  WriteToSubresource(
      UINT DstSubresource, const D3D12_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcSlicePitch
  ) {
    return E_INVALIDARG;
  };

  virtual HRESULT STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstSlicePitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox
  ) {
    return E_INVALIDARG;
  };

  virtual HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS *pFlags) {
    if (reserved)
      return E_INVALIDARG; // a reserved resource has no heap
    if (pHeapProps)
      *pHeapProps = heap_props_;
    if (pFlags)
      *pFlags = heap_flags_;
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_SHADER_RESOURCE_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
    }

    if ((desc_.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        ViewDesc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
      return E_INVALIDARG;

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    BufferSlice Slice;

    if (ViewDesc.Buffer.Flags & ~D3D12_BUFFER_SRV_FLAG_RAW)
      return E_INVALIDARG;
    const bool raw = ViewDesc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW;
    const bool structured = ViewDesc.Buffer.StructureByteStride != 0;
    if (raw) {
      if (ViewDesc.Format != DXGI_FORMAT_R32_TYPELESS || structured)
        return E_INVALIDARG;
    } else if (structured) {
      if (ViewDesc.Format != DXGI_FORMAT_UNKNOWN)
        return E_INVALIDARG;
    } else if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN) {
      return E_INVALIDARG;
    }

    if (raw || structured) {
      const UINT Stride = raw ? 4 : ViewDesc.Buffer.StructureByteStride;
      hr = BuildBufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Stride, false, Slice);
      if (FAILED(hr))
        return hr;
      return Heap->AddShaderResourceView(Index, buffer.ptr(), Slice);
    }

    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, Format))) {
      ERR("D3D12Buffer::CreateShaderResourceView: not an ordinary or packed format: ", ViewDesc.Format);
      return E_FAIL;
    }
    if (!Format.BytesPerTexel)
      return E_INVALIDARG;
    if (resource_contract::GetEmulatedTextureStorage(ViewDesc.Format) ==
        resource_contract::EmulatedTextureStorage::RGB32 &&
        !(Format.Flag & MTL_DXGI_FORMAT_TYPELESS)) {
      hr = BuildBufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements,
                            12, true, Slice);
      if (FAILED(hr))
        return hr;
      return Heap->AddRGB32ShaderResourceView(Index, buffer.ptr(), Slice, ViewDesc.Shader4ComponentMapping);
    }
    const auto read_capability = device_->GetMTLPixelFormatCapability(Format.PixelFormat);
    if ((Format.Flag & (MTL_DXGI_FORMAT_TYPELESS | MTL_DXGI_FORMAT_BC |
                        MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)) ||
        MTLGetTexelSize(Format.PixelFormat) != Format.BytesPerTexel ||
        !any_bit_set(read_capability &
                     (FormatCapability::TextureBufferRead | FormatCapability::TextureBufferReadWrite)))
      return E_INVALIDARG;
    BufferViewDescriptor view_descriptor{Format.PixelFormat};
    hr = BuildBufferSlice(
        desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Format.BytesPerTexel, true, Slice
    );
    if (FAILED(hr))
      return hr;
    view_descriptor.byteOffset = Slice.byteOffset & ~UINT64(255);
    const UINT64 prefix_bytes = Slice.byteOffset - view_descriptor.byteOffset;
    view_descriptor.byteLength = prefix_bytes + Slice.byteLength;
    Slice.firstElement = static_cast<UINT>(prefix_bytes / Format.BytesPerTexel);

    auto view = buffer->createView(view_descriptor);
    if (!buffer->view(view))
      return E_OUTOFMEMORY;
    return Heap->AddShaderResourceView(Index, buffer.ptr(), view, Slice);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    HRESULT hr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
    }

    if (!(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        ViewDesc.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
      return E_INVALIDARG;

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    BufferSlice Slice;

    if (ViewDesc.Buffer.Flags & ~D3D12_BUFFER_UAV_FLAG_RAW)
      return E_INVALIDARG;
    const bool raw = ViewDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW;
    const bool structured = ViewDesc.Buffer.StructureByteStride != 0;
    if (raw) {
      if (ViewDesc.Format != DXGI_FORMAT_R32_TYPELESS || structured || pCounter)
        return E_INVALIDARG;
    } else if (structured) {
      if (ViewDesc.Format != DXGI_FORMAT_UNKNOWN)
        return E_INVALIDARG;
    } else {
      if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN || pCounter)
        return E_INVALIDARG;
    }
    if (!pCounter && ViewDesc.Buffer.CounterOffsetInBytes)
      return E_INVALIDARG;

    if (raw || structured) {
      const UINT Stride = raw ? 4 : ViewDesc.Buffer.StructureByteStride;
      hr = BuildBufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Stride, false, Slice);
      if (FAILED(hr))
        return hr;
      if (!pCounter)
        return Heap->AddUnorderedAccessView(Index, buffer.ptr(), Slice, nullptr, 0);

      auto Counter = static_cast<MTLD3D12Resource *>(pCounter);
      if (!Counter->buffer || (ViewDesc.Buffer.CounterOffsetInBytes % D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT))
        return E_INVALIDARG;
      const auto counter_desc = pCounter->GetDesc();
      if (ViewDesc.Buffer.CounterOffsetInBytes > counter_desc.Width ||
          sizeof(UINT) > counter_desc.Width - ViewDesc.Buffer.CounterOffsetInBytes)
        return E_INVALIDARG;
      return Heap->AddUnorderedAccessView(
          Index, buffer.ptr(), Slice, Counter->buffer.ptr(), ViewDesc.Buffer.CounterOffsetInBytes
      );
    }

    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, Format))) {
      ERR("D3D12Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", ViewDesc.Format);
      return E_FAIL;
    }
    if (!Format.BytesPerTexel)
      return E_INVALIDARG;
    const auto write_capability = device_->GetMTLPixelFormatCapability(Format.PixelFormat);
    if ((Format.Flag & (MTL_DXGI_FORMAT_TYPELESS | MTL_DXGI_FORMAT_BC |
                        MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)) ||
        MTLGetTexelSize(Format.PixelFormat) != Format.BytesPerTexel ||
        Is_sRGBVariant(Format.PixelFormat) ||
        !any_bit_set(write_capability &
                     (FormatCapability::TextureBufferWrite | FormatCapability::TextureBufferReadWrite)))
      return E_INVALIDARG;
    BufferViewDescriptor view_descriptor{Format.PixelFormat};
    view_descriptor.writable = true;
    hr = BuildBufferSlice(
        desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Format.BytesPerTexel, true, Slice
    );
    if (FAILED(hr))
      return hr;
    view_descriptor.byteOffset = Slice.byteOffset & ~UINT64(255);
    const UINT64 prefix_bytes = Slice.byteOffset - view_descriptor.byteOffset;
    view_descriptor.byteLength = prefix_bytes + Slice.byteLength;
    Slice.firstElement = static_cast<UINT>(prefix_bytes / Format.BytesPerTexel);

    auto view = buffer->createView(view_descriptor);
    if (!buffer->view(view))
      return E_OUTOFMEMORY;
    return Heap->AddUnorderedAccessView(Index, buffer.ptr(), view, Slice);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    if (!pDesc || !(desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        pDesc->ViewDimension != D3D12_RTV_DIMENSION_BUFFER || pDesc->Format == DXGI_FORMAT_UNKNOWN)
      return E_INVALIDARG;

    MTL_DXGI_FORMAT_DESC format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pDesc->Format, format)) || !format.BytesPerTexel ||
        format.PixelFormat == WMTPixelFormatInvalid || (format.Flag & MTL_DXGI_FORMAT_TYPELESS) ||
        !any_bit_set(device_->GetMTLPixelFormatCapability(format.PixelFormat) & FormatCapability::Color))
      return E_INVALIDARG;

    BufferSlice slice;
    HRESULT hr = BuildBufferSlice(
        desc_.Width, pDesc->Buffer.FirstElement, pDesc->Buffer.NumElements, format.BytesPerTexel, true, slice
    );
    if (FAILED(hr))
      return hr;

    // A Metal render attachment must be a 2D texture. Texture-buffer views are
    // deliberately not marked RenderTarget: Metal does not permit using them as
    // attachments. Recording creates a private single-row texture, copies this
    // exact range in, renders, and copies it back.
    BufferViewDescriptor view_descriptor{format.PixelFormat};
    view_descriptor.writable = true;
    view_descriptor.byteOffset = slice.byteOffset & ~UINT64(255);
    const UINT64 prefix_bytes = slice.byteOffset - view_descriptor.byteOffset;
    view_descriptor.byteLength = prefix_bytes + slice.byteLength;
    slice.firstElement = static_cast<UINT>(prefix_bytes / format.BytesPerTexel);

    const auto view = buffer->createView(view_descriptor);
    if (!buffer->view(view))
      return E_OUTOFMEMORY;

    auto [heap, index] = GetRenderTargetHeap(device_, Descriptor);
    const MTL_RENDER_TARGET_DESC render_target{
        .BufferResource = buffer.ptr(),
        .BufferView = view,
        .BufferRange = slice,
        .Format = format.PixelFormat,
        .Width = pDesc->Buffer.NumElements,
        .Height = 1,
        .RenderTargetArrayLength = 1,
    };
    return heap->AddRenderTarget(index, &render_target);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    return E_INVALIDARG;
  };

  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    if (reserved) {
      reserved->GetResourceTiling(TotalTileCount, PackedMipInfo, StandardTileShape, SubresourceTilingCount,
                                  FirstSubresourceTiling, SubresourceTilings);
      return;
    }
    /* A committed or placed buffer is not tiled. */
    if (TotalTileCount)
      *TotalTileCount = 0;
    if (PackedMipInfo)
      *PackedMipInfo = {};
    if (StandardTileShape)
      *StandardTileShape = {};
    if (SubresourceTilingCount)
      *SubresourceTilingCount = 0;
  };
};

HRESULT
CreateCommittedBuffer(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
) {
  auto *impl = new (std::nothrow) MTLD3D12Buffer(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto buffer = Com(impl);
  HRESULT hr = buffer->Initialize(pHeapProps, HeapFlags, pDesc, OptimizedClearValue, nullptr, 0);
  if (FAILED(hr))
    return hr;
  buffer->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
OpenSharedBuffer(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppResource) {
  InitReturnPtr(ppResource);
  if (share->kind() != sharing::Kind::Buffer)
    return E_INVALIDARG;
  const auto props = share->header().heap_props;
  const auto flags = share->header().heap_flags;
  const auto desc = share->header().resource_desc;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width > share->payloadSize())
    return E_INVALIDARG;
  auto *impl = new (std::nothrow) MTLD3D12Buffer(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto buffer = Com(impl);
  buffer->share = std::move(share);
  HRESULT hr = buffer->Initialize(&props, flags, &desc, nullptr, nullptr, 0);
  if (FAILED(hr))
    return hr;
  buffer->owned_initial_state = D3D12_RESOURCE_STATE_COMMON;
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
CreateReservedBuffer(
    MTLD3D12Device *pDevice, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  if (OptimizedClearValue)
    return E_INVALIDARG;
  auto *impl = new (std::nothrow) MTLD3D12Buffer(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto buffer = Com(impl);
  HRESULT hr = buffer->InitializeReserved(pDesc);
  if (FAILED(hr))
    return hr;
  buffer->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
CreatePlacedBuffer(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  auto *impl = new (std::nothrow) MTLD3D12Buffer(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto buffer = Com(impl);
  D3D12_HEAP_DESC heap_desc = pHeap->GetDesc();

  if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)
    return E_INVALIDARG;

  HRESULT hr = buffer->Initialize(&heap_desc.Properties, heap_desc.Flags, pDesc, OptimizedClearValue, pHeap, Offset);
  if (FAILED(hr))
    return hr;
  buffer->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

} // namespace dxmt
