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
#include "d3d12_device_child.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include <new>
#include <cwchar>

namespace dxmt {

class MTLD3D12FenceImpl : public MTLD3D12DeviceChild<MTLD3D12Fence> {
  D3D12_FENCE_FLAGS flags_;

public:
  MTLD3D12FenceImpl(MTLD3D12Device *pDevice, D3D12_FENCE_FLAGS flags) :
      MTLD3D12DeviceChild<MTLD3D12Fence>(pDevice),
      flags_(flags) {}

  HRESULT
  Initialize(UINT64 InitialValue) {
    fence = new Fence();
    if (!fence->initialize(InitialValue))
      return E_OUTOFMEMORY;
    if (!device_->RegisterFenceOrder(fence->currentOrder()))
      return E_OUTOFMEMORY;
    if (!(flags_ & (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER)))
      return S_OK;
    auto event = device_->GetMTLDevice().newSharedEvent();
    if (!event)
      return E_OUTOFMEMORY;
    HRESULT hr = sharing::Create(sharing::Kind::Fence, sizeof(SharedFenceTimeline), share);
    if (FAILED(hr))
      return hr;
    share->header().fence_flags = flags_;
    return Publish(std::move(event), true, InitialValue);
  }

  /*
   * Logical completion lives in the versioned shared section. The Metal event
   * carries completion-publication serials only to wake observers; producer
   * reservation order must never become logical completion order.
   */
  HRESULT
  Publish(WMT::Reference<WMT::SharedEvent> event, bool publish_initial, uint64_t initial_value) {
    if (share->payloadSize() < sizeof(SharedFenceTimeline))
      return E_INVALIDARG;
    const uint64_t identity = WMTShareObjectIdentity(event.handle, 0);
    if (!identity)
      return E_FAIL;
    if (!share->header().identity)
      share->header().identity = identity;
    else if (share->header().identity != identity)
      return E_INVALIDARG;
    mach_port_t port = event.createMachPort();
    if (!port)
      return E_FAIL;
    HRESULT hr = share->publishPort(port);
    WMTMachPortDeallocate(port);
    if (FAILED(hr))
      return hr;
    auto *timeline = reinterpret_cast<SharedFenceTimeline *>(share->payload());
    if (!publish_initial && timeline->version != SharedFenceTimeline::kVersion)
      return E_INVALIDARG;
    // Use the random section token, not a recyclable native event identity.
    WCHAR mutex_name[80];
    const auto *token = share->header().token;
    std::swprintf(mutex_name, 80, L"Local\\dxmt.fence.%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                  token[0], token[1], token[2], token[3], token[4], token[5], token[6], token[7],
                  token[8], token[9], token[10], token[11], token[12], token[13], token[14], token[15]);
    return fence->initializeShared(std::move(event), timeline, share, mutex_name, publish_initial, initial_value) ? S_OK
                                                                                                    : E_OUTOFMEMORY;
  }

  HRESULT
  InitializeOpened(std::shared_ptr<sharing::SharedObject> object) {
    share = std::move(object);
    mach_port_t port = share->lookUpPort();
    if (!port)
      return E_INVALIDARG;
    auto event = WMT::Reference<WMT::SharedEvent>(MTLDevice_newSharedEventWithMachPort(device_->GetMTLDevice().handle, port));
    WMTMachPortDeallocate(port);
    if (!event)
      return E_INVALIDARG;
    fence = new Fence();
    if (!fence->initialize(0)) // replaced by the published logical value in Publish
      return E_OUTOFMEMORY;
    if (!device_->RegisterFenceOrder(fence->currentOrder()))
      return E_OUTOFMEMORY;
    return Publish(std::move(event), false, 0);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Fence) || riid == __uuidof(ID3D12Fence1)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Fence1), riid)) {
      WARN("ID3D12Fence: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  UINT64 STDMETHODCALLTYPE
  GetCompletedValue() {
    if (device_->RemovedReason() != S_OK)
      return UINT64_MAX;
    auto value = fence->completedValue();
    return device_->RemovedReason() != S_OK ? UINT64_MAX : value;
  }

  HRESULT STDMETHODCALLTYPE
  SetEventOnCompletion(UINT64 Value, HANDLE Event) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    // Already reached: completion registration would latch immediately anyway,
    // so skip its latch/subscription/registration allocations.
    if (fence->completedValue() >= Value) {
      if (Event)
        SetEvent(Event);
      return device_->RemovedReason() == S_OK || Event ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
    }
    try {
      if (!Event) {
        fence->wait(Value);
        return device_->RemovedReason() == S_OK ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
      }
      return device_->event_listener.setEventOnValue(fence.ptr(), Event, Value) ? S_OK : E_OUTOFMEMORY;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  HRESULT STDMETHODCALLTYPE
  Signal(UINT64 Value) {
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    return fence->signal(Value) ? S_OK : E_OUTOFMEMORY;
  }

  D3D12_FENCE_FLAGS STDMETHODCALLTYPE
  GetCreationFlags() {
    return flags_;
  }
};

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
  auto fence = Com(new MTLD3D12FenceImpl(pDevice, Flags));
  HRESULT hr = fence->Initialize(InitialValue);
  if (FAILED(hr))
    return hr;
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenSharedFence(MTLD3D12Device *pDevice, std::shared_ptr<sharing::SharedObject> share, REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);
  if (share->kind() != sharing::Kind::Fence)
    return E_INVALIDARG;
  auto fence = Com(new MTLD3D12FenceImpl(pDevice, share->header().fence_flags));
  HRESULT hr = fence->InitializeOpened(std::move(share));
  if (FAILED(hr))
    return hr;
  return fence->QueryInterface(riid, ppFence);
}

}; // namespace dxmt
