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
#include <memory>
#include <new>
#include "com/com_pointer.hpp"
#include "wsi_platform.hpp"

namespace d3dalloy {

class MTLD3D12HeapImpl : public MTLD3D12Pageable<MTLD3D12Heap> {

  D3D12_HEAP_DESC desc_;

public:
  MTLD3D12HeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Heap>(pDevice) {}

  ~MTLD3D12HeapImpl() {
    if (heap)
      device_->UnregisterResidency(heap, this);
    heap = {};
    if (shared_buffer)
      device_->UnregisterResidency(shared_buffer, this);
    shared_buffer = {};
#ifdef __i386__
    if (!external_memory)
      wsi::aligned_free(host_memory);
#endif
    if (owned_view_)
      UnmapViewOfFile(owned_view_);
  }

  /* OpenExistingHeapFromFileMapping: the view this heap mapped and unmaps on release. */
  void *owned_view_ = nullptr;

  /*
   * A shared heap has no MTLHeap: its memory is the section payload, mapped in
   * every process, and each placed buffer is a no-copy MTLBuffer over its
   * offset. `shared_buffer` spans the payload so residency accounting and
   * MakeResident/Evict act on the heap as one allocation.
   */
  HRESULT
  InitializeShared(std::shared_ptr<sharing::SharedObject> object) {
    share = std::move(object);
    return InitializeExternal(share->payload(), share->header().heap_desc);
  }

  /*
   * Heaps over memory Metal does not own: a section payload or application host memory
   * (OpenExistingHeapFrom*). The memory must stay mapped for the heap's lifetime; the
   * no-copy buffer never frees it.
   */
  HRESULT
  InitializeExternal(uint8_t *memory, const D3D12_HEAP_DESC &desc) {
    desc_ = desc;
    external_memory = memory;
    WMTBufferInfo info{};
    info.memory.set(memory);
    info.length = desc_.SizeInBytes;
    info.options = WMTResourceHazardTrackingModeUntracked;
    if (desc_.Properties.Type == D3D12_HEAP_TYPE_UPLOAD || desc_.Properties.Type == kHeapTypeGpuUpload ||
        desc_.Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)
      info.options = WMTResourceOptions(info.options | WMTResourceOptionCPUCacheModeWriteCombined);
    shared_buffer = device_->GetMTLDevice().newBuffer(info);
    if (!shared_buffer)
      return E_OUTOFMEMORY;
#ifdef __i386__
    host_memory = memory;
#endif
    return device_->RegisterResidency(shared_buffer, this, !(desc_.Flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT));
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Heap) || riid == __uuidof(ID3D12Heap1)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Heap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT
  Initialize(const D3D12_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    if (pDesc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
      return E_INVALIDARG; // must be committed resource

    desc_ = *pDesc;
    desc_.Properties.CreationNodeMask = 1;
    desc_.Properties.VisibleNodeMask = 1;


    switch (pDesc->Alignment) {
    case 0:
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      [[fallthrough]];
    case D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT:
    case D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT:
      break;
    default:
      return E_INVALIDARG;
    }

    uint64_t size_aligned = 0;
    if (!pDesc->SizeInBytes ||
        !resource_contract::CheckedAlign(pDesc->SizeInBytes, desc_.Alignment, size_aligned))
      return E_INVALIDARG;

    WMTHeapInfo info = {};
    // CPU-visible heaps use Shared storage (CPU-visible on Apple unified memory); GPU upload heaps are
    // CPU-write-combined. A heap without CPU pages is Private: none of its placed resources can be mapped,
    // and each requests the heap's storage mode (Metal rejects a mismatch). Placement sizes and alignments
    // do not depend on the storage mode, so GetResourceAllocationInfo stays valid for either heap.
    if (desc_.Properties.Type == kHeapTypeGpuUpload)
      info.options = WMTResourceOptionCPUCacheModeWriteCombined;
    else if (!IsCpuVisibleHeap(&desc_.Properties))
      info.options = WMTResourceStorageModePrivate;
    info.size = size_aligned;
    info.sparse_page_size = WMTSparsePageSize16;
    info.type = WMTHeapTypePlacement;

    if (desc_.Flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)) {
      desc_.Flags |= D3D12_HEAP_FLAG_SHARED;
      std::shared_ptr<sharing::SharedObject> created;
      HRESULT hr = sharing::Create(sharing::Kind::Heap, size_aligned, created);
      if (FAILED(hr))
        return hr;
      created->header().heap_desc = desc_;
      return InitializeShared(std::move(created));
    }

    /* Every heap can back reserved-resource tiles (64KB Metal sparse pages) as well as placed resources. */
    auto metal = device_->GetMTLDevice();
    heap = metal.supportsPlacementSparse() ? metal.newPlacementHeap(info, WMTSparsePageSize64) : metal.newHeap(info);
    if (!heap)
      return E_OUTOFMEMORY;
    if (!IsCpuVisibleHeap(&desc_.Properties) && !(desc_.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)) {
      try { placement_ranges = std::make_shared<owned_graphics::RTASPlacementRanges>(); }
      catch (const std::bad_alloc &) { /* Optional proof; use the GPU metadata readback. */ }
    }
#ifdef __i386__
    if (IsCpuVisibleHeap(&desc_.Properties)) {
      if (size_aligned > SIZE_MAX)
        return E_OUTOFMEMORY;
      host_memory = static_cast<uint8_t *>(wsi::aligned_malloc(size_t(size_aligned), desc_.Alignment));
      if (!host_memory)
        return E_OUTOFMEMORY;
    }
#endif

    return device_->RegisterResidency(
        heap, this, !(desc_.Flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT)
    );
  }

  virtual D3D12_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };
};

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  auto *impl = new (std::nothrow) MTLD3D12HeapImpl(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto heap = Com(impl);
  HRESULT hr = heap->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  if (!ppHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppHeap);
}

HRESULT
OpenSharedHeap(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  if (share->kind() != sharing::Kind::Heap)
    return E_INVALIDARG;
  auto *impl = new (std::nothrow) MTLD3D12HeapImpl(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto heap = Com(impl);
  HRESULT hr = heap->InitializeShared(std::move(share));
  if (FAILED(hr))
    return hr;
  return heap->QueryInterface(riid, ppHeap);
}

namespace {

// Size 0 means the committed read-write run starting at the address, within one allocation.
HRESULT
ValidateHostMemory(void *pAddress, UINT64 Size, uint64_t &size) {
  // Metal no-copy buffers need host-page (vm_page_size) alignment for both base and length:
  // 16 KiB on Apple silicon, including x86_64 processes under Rosetta, whose 4 KiB-aligned
  // allocations are therefore rejected unless they happen to be 16 KiB aligned.
  constexpr uint64_t kHostPage = 16384;
  if (!pAddress || (reinterpret_cast<uintptr_t>(pAddress) & (kHostPage - 1)))
    return E_INVALIDARG;
  auto writable = [](const MEMORY_BASIC_INFORMATION &run) {
    return run.State == MEM_COMMIT && (run.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
           !(run.Protect & (PAGE_GUARD | PAGE_NOACCESS));
  };
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(pAddress, &info, sizeof(info)) || !writable(info))
    return E_INVALIDARG;
  uint64_t available = 0;
  for (auto *cursor = static_cast<uint8_t *>(pAddress);;) {
    MEMORY_BASIC_INFORMATION run{};
    if (!VirtualQuery(cursor, &run, sizeof(run)) || !writable(run) || run.AllocationBase != info.AllocationBase)
      break;
    const uint64_t skip = static_cast<uint8_t *>(run.BaseAddress) + run.RegionSize - cursor;
    available += skip;
    cursor += skip;
    if (Size && available >= Size)
      break;
  }
  size = Size ? Size : available & ~(kHostPage - 1);
  if (!size || size > available || (size & (kHostPage - 1)))
    return E_INVALIDARG;
  return S_OK;
}

HRESULT
CreateHostMemoryHeap(MTLD3D12Device *pDevice, void *pAddress, UINT64 Size, void *owned_view, REFIID riid,
                     void **ppHeap) {
  // Own the mapped view before either validation or heap allocation can fail.
  std::unique_ptr<void, decltype(&UnmapViewOfFile)> view(owned_view, &UnmapViewOfFile);
  uint64_t size = 0;
  HRESULT hr = ValidateHostMemory(pAddress, Size, size);
  if (FAILED(hr))
    return hr;
  D3D12_HEAP_DESC desc{};
  desc.SizeInBytes = size;
  desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
  desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  desc.Properties.CreationNodeMask = 1;
  desc.Properties.VisibleNodeMask = 1;
  desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  desc.Flags = D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

  auto *impl = new (std::nothrow) MTLD3D12HeapImpl(pDevice);
  if (!impl)
    return E_OUTOFMEMORY;
  auto heap = Com(impl);
  heap->owned_view_ = view.release(); // the heap releases the view after its no-copy Metal buffer
  hr = heap->InitializeExternal(static_cast<uint8_t *>(pAddress), desc);
  if (FAILED(hr))
    return hr;
  if (!ppHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppHeap);
}

} // namespace

HRESULT
CreateHeapFromHostMemory(MTLD3D12Device *pDevice, void *pAddress, UINT64 Size, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  return CreateHostMemoryHeap(pDevice, pAddress, Size, nullptr, riid, ppHeap);
}

HRESULT
CreateHeapFromFileMapping(MTLD3D12Device *pDevice, HANDLE hFileMapping, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  if (!hFileMapping)
    return E_INVALIDARG;
  // The heap owns one whole view of the mapping for its lifetime.
  void *view = MapViewOfFile(hFileMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (!view)
    return E_INVALIDARG;
  return CreateHostMemoryHeap(pDevice, view, 0, view, riid, ppHeap);
}

}; // namespace d3dalloy
