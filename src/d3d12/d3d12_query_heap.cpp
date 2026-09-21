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

#include "com/com_pointer.hpp"
#include "d3d12_pageable.hpp"

namespace dxmt {

class MTLD3D12QueryHeapImpl : public MTLD3D12Pageable<MTLD3D12QueryHeap> {
public:
  MTLD3D12QueryHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12QueryHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_QUERY_HEAP_DESC *pDesc) {
    if (!pDesc || !pDesc->Count || pDesc->NodeMask > 1)
      return E_INVALIDARG;

    desc = *pDesc;
    switch (desc.Type) {
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP: {
      timestamp_samples = device_->GetMTLDevice().newCounterSampleBuffer(desc.Count, true);
      if (!timestamp_samples)
        return E_NOTIMPL;
      try {
        timestamp_producers = std::make_shared<owned_graphics::QueryTimestampProducers>(desc.Count);
      } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
      }
      WMTBufferInfo dummy_info{};
      dummy_info.length = 4;
      dummy_info.options = WMTResourceHazardTrackingModeUntracked;
      timestamp_dummy = device_->GetMTLDevice().newBuffer(dummy_info);
      if (!timestamp_dummy)
        return E_OUTOFMEMORY;
      break;
    }
    case D3D12_QUERY_HEAP_TYPE_OCCLUSION: {
      WMTBufferInfo visibility_info{};
      visibility_info.length = uint64_t(desc.Count) * sizeof(uint64_t);
      visibility_info.options = WMTResourceHazardTrackingModeUntracked;
      visibility_results = device_->GetMTLDevice().newBuffer(visibility_info);
      if (!visibility_results)
        return E_OUTOFMEMORY;
      auto function = device_->GetLib().getLibrary().newFunction("query_binary_normalize");
      WMT::Reference<WMT::Error> error;
      binary_normalize_pipeline = device_->GetMTLDevice().newComputePipelineState(function, error);
      if (!binary_normalize_pipeline)
        return E_FAIL;
      auto accumulate_function = device_->GetLib().getLibrary().newFunction("query_accumulate_visibility");
      visibility_accumulate_pipeline =
          device_->GetMTLDevice().newComputePipelineState(accumulate_function, error);
      if (!visibility_accumulate_pipeline)
        return E_FAIL;
      break;
    }
    default:
      return E_NOTIMPL;
    }
    WMT::Allocation allocation = desc.Type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP
                                     ? WMT::Allocation(timestamp_dummy)
                                     : WMT::Allocation(visibility_results);
    HRESULT hr = device_->RegisterResidency(allocation, this);
    if (FAILED(hr))
      return hr;
    owned_graphics::Trace("query_heap_create", ",\"heap\":", owned_id,
                          ",\"type\":", uint32_t(desc.Type), ",\"count\":", desc.Count);
    return S_OK;
  }

  ~MTLD3D12QueryHeapImpl() {
    WMT::Allocation allocation = desc.Type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP
                                     ? WMT::Allocation(timestamp_dummy)
                                     : WMT::Allocation(visibility_results);
    if (allocation)
      device_->UnregisterResidency(allocation, this);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12QueryHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12QueryHeap), riid)) {
      WARN("D3D12QueryHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }
};

HRESULT
CreateQueryHeap(MTLD3D12Device *pDevice, const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppQueryHeap) {
  if (ppQueryHeap)
    *ppQueryHeap = nullptr;
  if (!pDesc)
    return E_INVALIDARG;
  auto heap = Com(new MTLD3D12QueryHeapImpl(pDevice));
  HRESULT hr = heap->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  if (!ppQueryHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppQueryHeap);
}

} // namespace dxmt
