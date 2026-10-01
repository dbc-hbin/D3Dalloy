/*
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

/*
 * Sampler feedback maps (d3d12_sampler_feedback.hpp): an opaque TEXTURE2D resource backed by an
 * owned buffer of u32 cells. It has no texture, no CPU mapping and no SRV/UAV/RTV/DSV of its own;
 * it is bound only through CreateSamplerFeedbackUnorderedAccessView, cleared with
 * ClearUnorderedAccessViewUint, copied with CopyResource and transcoded with
 * ResolveSubresourceRegion.
 */

#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_resource_contract.hpp"
#include "com/com_pointer.hpp"
#include <new>

namespace dxmt {

namespace {

class MTLD3D12SamplerFeedbackMap : public MTLD3D12Pageable<MTLD3D12Resource> {
  D3D12_RESOURCE_DESC desc_;
  D3D12_HEAP_PROPERTIES heap_props_;
  D3D12_HEAP_FLAGS heap_flags_;
  Com<MTLD3D12Heap> placed_heap_;
  bool owns_registration_ = false;

public:
  MTLD3D12SamplerFeedbackMap(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Resource>(pDevice) {}

  HRESULT
  Initialize(const D3D12_HEAP_PROPERTIES &heap_props, D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC &desc,
             const sampler_feedback::Layout &layout, MTLD3D12Heap *heap, UINT64 offset) {
    desc_ = desc;
    desc_.MipLevels = UINT16(layout.mips);
    desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_props_ = heap_props;
    heap_flags_ = heap_flags;
    feedback = layout;
    const uint64_t bytes = layout.totalCells() * 4;
    auto metal = device_->GetMTLDevice();
    if (bytes > metal.maxBufferLength())
      return E_OUTOFMEMORY;
    if (heap) {
      const auto native = metal.heapBufferSizeAndAlign(bytes, {});
      if (!resource_contract::PlacementFits(offset, native.size, heap->GetDesc().SizeInBytes,
                                            D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, native.align))
        return E_INVALIDARG;
    }
    try {
      buffer = new Buffer(bytes, metal);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    Flags<BufferAllocationFlag> flags;
    if (heap && heap->external_memory)
      buffer->rename(buffer->allocate(flags, heap->external_memory + offset));
#ifdef __i386__
    else if (heap && heap->host_memory)
      buffer->rename(buffer->allocate(flags, heap->host_memory + offset));
#endif
    else if (heap)
      buffer->rename(buffer->allocate(flags, heap->heap, offset));
    else
      buffer->rename(buffer->allocate(flags)); // a new Metal buffer is zero filled: created cleared
    if (!buffer->current() || !buffer->current()->buffer())
      return E_OUTOFMEMORY;
    owns_registration_ = !heap;
    HRESULT hr = device_->RegisterResidencyAndVA(buffer->current(), owns_registration_ ? this : nullptr,
                                                 heap || !(heap_flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT));
    if (SUCCEEDED(hr) && heap)
      placed_heap_ = heap;
    return hr;
  }

  ~MTLD3D12SamplerFeedbackMap() {
    if (buffer && buffer->current())
      device_->UnregisterResidencyAndVA(buffer->current(), owns_registration_ ? this : nullptr);
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid))
      WARN("D3D12SamplerFeedbackMap: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  /* Opaque: no CPU access. */
  HRESULT STDMETHODCALLTYPE
  Map(UINT, const D3D12_RANGE *, void **ppData) {
    if (ppData)
      *ppData = nullptr;
    return E_INVALIDARG;
  }

  void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) {}

  D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  }

  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() {
    return 0;
  }

  HRESULT STDMETHODCALLTYPE
  WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT, UINT) {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  ReadFromSubresource(void *, UINT, UINT, UINT, const D3D12_BOX *) {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS *pFlags) {
    if (pHeapProps)
      *pHeapProps = heap_props_;
    if (pFlags)
      *pFlags = heap_flags_;
    return S_OK;
  }

  /* Feedback maps are bound only through CreateSamplerFeedbackUnorderedAccessView. */
  HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) {
    return E_INVALIDARG;
  }

  void STDMETHODCALLTYPE
  GetResourceTiling(UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTileShape,
                    UINT *SubresourceTilingCount, UINT, D3D12_SUBRESOURCE_TILING *) {
    if (TotalTileCount)
      *TotalTileCount = 0;
    if (PackedMipInfo)
      *PackedMipInfo = {};
    if (StandardTileShape)
      *StandardTileShape = {};
    if (SubresourceTilingCount)
      *SubresourceTilingCount = 0;
  }
};

} // namespace

HRESULT
SamplerFeedbackAllocationInfo(MTLD3D12Device *pDevice, const sampler_feedback::Layout &layout,
                              D3D12_RESOURCE_ALLOCATION_INFO1 &info) {
  const uint64_t bytes = layout.totalCells() * 4;
  if (bytes > pDevice->GetMTLDevice().maxBufferLength())
    return E_INVALIDARG;
  const auto native = pDevice->GetMTLDevice().heapBufferSizeAndAlign(bytes, {});
  info.Alignment = std::max<uint64_t>(native.align, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
  // D3D12 resource sizes are whole placement-alignment units, so a single description's aggregate size matches.
  info.SizeInBytes = (native.size + info.Alignment - 1) & ~(info.Alignment - 1);
  return S_OK;
}

HRESULT
CreateSamplerFeedbackMap(MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES &HeapProps, D3D12_HEAP_FLAGS HeapFlags,
                         const D3D12_RESOURCE_DESC &Desc, const sampler_feedback::Layout &Layout, MTLD3D12Heap *pHeap,
                         UINT64 HeapOffset, D3D12_RESOURCE_STATES InitialState, REFIID riid, void **ppResource) {
  auto *impl = new (std::nothrow) MTLD3D12SamplerFeedbackMap(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto resource = Com(impl);
  HRESULT hr = resource->Initialize(HeapProps, HeapFlags, Desc, Layout, pHeap, HeapOffset);
  if (FAILED(hr))
    return hr;
  resource->owned_initial_state = InitialState;
  if (!ppResource)
    return S_FALSE;
  return resource->QueryInterface(riid, ppResource);
}

} // namespace dxmt
