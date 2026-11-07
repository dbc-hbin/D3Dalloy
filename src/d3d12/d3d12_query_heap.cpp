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
#include "Metal4Pipeline.hpp"
#include "Metal4Query.hpp"

namespace d3dalloy {

static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) == 88);
static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1) == 112);

class MTLD3D12QueryHeapImpl : public MTLD3D12Pageable<MTLD3D12QueryHeap> {
public:
  MTLD3D12QueryHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12QueryHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_QUERY_HEAP_DESC *pDesc) {
    desc = *pDesc;
    switch (desc.Type) {
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
    case D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP: {
      timestamp_samples = WMT::newTimestampCounterHeap(device_->GetMTLDevice(), desc.Count);
      if (!timestamp_samples)
        return E_NOTIMPL;

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
      auto function = WMT4::Library(device_->GetLib().getLibrary()).newFunction("query_accumulate_binary_visibility");
      WMT::Reference<WMT::Error> error;
      binary_accumulate_pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!binary_accumulate_pipeline)
        return E_FAIL;
      auto accumulate_function = WMT4::Library(device_->GetLib().getLibrary()).newFunction("query_accumulate_visibility");
      visibility_accumulate_pipeline =
          device_->GetMetal4Compiler().newComputePipelineState(accumulate_function, error);
      if (!visibility_accumulate_pipeline)
        return E_FAIL;
      break;
    }
    case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS: {
      // Results (Count entries) followed by BeginQuery snapshots (Count entries).
      WMTBufferInfo info{};
      info.length = uint64_t(desc.Count) * 2 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
      info.options = WMTResourceHazardTrackingModeUntracked | WMT_RESOURCE_CPU_ADDRESSABLE;
      so_statistics = device_->GetMTLDevice().newBuffer(info);
      if (!so_statistics)
        return E_OUTOFMEMORY;
      if (auto *memory = info.memory.get_accessible_or_null())
        memset(memory, 0, info.length);
      auto function = WMT4::Library(device_->GetLib().getLibrary()).newFunction("query_so_statistics_end");
      WMT::Reference<WMT::Error> error;
      so_statistics_end_pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!so_statistics_end_pipeline)
        return E_FAIL;
      break;
    }
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1: {
      const bool mesh = desc.Type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1;
      pipeline_statistics_record = mesh ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1)
                                        : sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
      WMTBufferInfo info{};
      info.length = uint64_t(desc.Count) * 2 * pipeline_statistics_record;
      info.options = WMTResourceHazardTrackingModeUntracked;
      pipeline_statistics = device_->GetMTLDevice().newBuffer(info);
      if (!pipeline_statistics)
        return E_OUTOFMEMORY;
      auto function = WMT4::Library(device_->GetLib().getLibrary()).newFunction(mesh ? "query_pipeline_statistics1_end" : "query_pipeline_statistics_end");
      WMT::Reference<WMT::Error> error;
      pipeline_statistics_end_pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, error);
      if (!pipeline_statistics_end_pipeline)
        return E_FAIL;
      break;
    }
    default:
      return E_NOTIMPL;
    }
    HRESULT hr = device_->RegisterResidency(ResidencyAllocation(), this);
    if (FAILED(hr))
      return hr;
    owned_graphics::Trace("query_heap_create", ",\"heap\":", owned_id,
                          ",\"type\":", uint32_t(desc.Type), ",\"count\":", desc.Count);
    return S_OK;
  }

  WMT::Allocation
  ResidencyAllocation() const {
    switch (desc.Type) {
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
    case D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:
      return WMT::Allocation(timestamp_dummy);
    case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
      return WMT::Allocation(so_statistics);
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1:
      return WMT::Allocation(pipeline_statistics);
    default:
      return WMT::Allocation(visibility_results);
    }
  }

  ~MTLD3D12QueryHeapImpl() {
    WMT::Allocation allocation = ResidencyAllocation();
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
  if (!pDesc || !pDesc->Count || pDesc->NodeMask > 1)
    return E_INVALIDARG;
  auto heap = Com(new MTLD3D12QueryHeapImpl(pDevice));
  HRESULT hr = heap->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  if (!ppQueryHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppQueryHeap);
}

} // namespace d3dalloy
