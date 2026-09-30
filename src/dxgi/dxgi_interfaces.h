#pragma once

#include <dxgi1_6.h>
#include "util_d3dkmt.h"
#include "Metal.hpp"
#include "com/com_guid.hpp"

DEFINE_COM_INTERFACE("acdf3ef1-b33a-4cb6-97bd-1c1974827e6d", IMTLDXGIAdapter)
    : public IDXGIAdapter4 {
  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;
  virtual D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() = 0;
};

DEFINE_COM_INTERFACE("6bfa1657-9cb1-471a-a4fb-7cacf8a81207", IMTLDXGIDevice)
    : public IDXGIDevice3 {
  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;
  virtual D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 * pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) = 0;
};

DEFINE_COM_INTERFACE("6c030003-460a-4b19-909b-38f97d203e45",
                     IMTLSwapChainFactory)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 * pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) = 0;
};

// D3D12-only creation path: validate the optional output before fullscreen
// setup, without changing the shared swapchain factory ABI used by D3D11.
DEFINE_COM_INTERFACE("ba5a922b-fee8-40ae-8c74-599855edebb7", IMTLD3D12SwapChainFactory)
    : public IMTLSwapChainFactory {
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChainWithOutput(
      IDXGIFactory1 *factory, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc, IDXGIOutput *restrict_output,
      IDXGISwapChain1 **swapchain) = 0;
};

static constexpr IID DXMT_NVEXT_GUID = dxmt::guid::make_guid("ba0af616-4a43-4259-815c-db3b89829905");

namespace dxmt {
enum class VendorExtension {
  None,
  Nvidia,
};

extern VendorExtension g_extension_enabled;
} // namespace dxmt
