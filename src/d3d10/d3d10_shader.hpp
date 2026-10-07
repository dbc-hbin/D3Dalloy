#pragma once

#include "d3d10_util.hpp"

namespace dxmt {

template <typename D3D10Interface, typename D3D11Interface>
class MTLD3D10Shader final : public D3D10Interface {

public:
  using ImplementedInterface = D3D10Interface;

  MTLD3D10Shader(D3D11Interface *d3d11) : d3d11(d3d11) {}

  D3D11Interface *d3d11;

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    return d3d11->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() {
    return d3d11->AddRef();
  }

  ULONG STDMETHODCALLTYPE
  Release() {
    return d3d11->Release();
  }

  void STDMETHODCALLTYPE
  GetDevice(ID3D10Device **ppDevice) {
    GetD3D10Device(d3d11, ppDevice);
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) {
    return d3d11->GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) {
    return d3d11->SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) {
    return d3d11->SetPrivateDataInterface(guid, pData);
  }
};

using MTLD3D10VertexShader = MTLD3D10Shader<ID3D10VertexShader, ID3D11VertexShader>;
using MTLD3D10PixelShader = MTLD3D10Shader<ID3D10PixelShader, ID3D11PixelShader>;
using MTLD3D10GeometryShader = MTLD3D10Shader<ID3D10GeometryShader, ID3D11GeometryShader>;

} // namespace dxmt
