#include "arm64_abi_probe.h"
#include <new>

class Probe final : public AbiObject {
  ULONG refs_ = 1;
public:
  dxmt::Com<AbiObject> STDMETHODCALLTYPE Retain(UINT tag, void *address) override {
    if (tag != 0x41424931 || address != this) return nullptr;
    return this;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown)) return E_NOINTERFACE;
    *object = static_cast<AbiObject *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG remaining = --refs_;
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE Exchange(AbiPacket packet, AbiCallback *callback, AbiPacket *reply) override {
    if (!callback || !reply || packet.tag != 0x41424931 || packet.delta != -37 ||
        packet.token != 0xfedcba9876543210ull || !packet.address || packet.value != 6.25)
      return E_INVALIDARG;
    uint64_t response = 0;
    HRESULT hr = callback->Check(packet, &response);
    if (FAILED(hr) || response != (packet.token ^ packet.address)) return E_FAIL;
    *reply = packet;
    reply->token = response;
    reply->value += packet.delta;
    return S_OK;
  }
};

extern "C" __declspec(dllexport) HRESULT WINAPI AbiProbeCreate(AbiObject **object) {
  if (!object) return E_POINTER;
  *object = new (std::nothrow) Probe;
  return *object ? S_OK : E_OUTOFMEMORY;
}

extern "C" __declspec(dllexport) double WINAPI AbiProbeArithmetic(uint64_t integer, int32_t delta,
                                                                  double value, void *address) {
  if (integer != 0xfedcba9876543210ull || !address) return -999.0;
  return value + delta;
}
