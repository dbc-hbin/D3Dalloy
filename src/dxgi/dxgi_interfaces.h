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

enum class D3DalloySwapChainKind { Hwnd, CoreWindow, Composition };

// D3D12-only creation path: validate the optional output before fullscreen
// setup, without changing the shared swapchain factory ABI used by D3D11.
DEFINE_COM_INTERFACE("ba5a922b-fee8-40ae-8c74-599855edebb7", IMTLD3D12SwapChainFactory)
    : public IMTLSwapChainFactory {
  virtual HRESULT STDMETHODCALLTYPE CreateSwapChainWithOutput(
      IDXGIFactory1 *factory, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc, IDXGIOutput *restrict_output,
      IDXGISwapChain1 **swapchain, IUnknown *core_window, D3DalloySwapChainKind kind) = 0;
};

// Public CoreWindow.idl ABI; the MinGW SDK does not ship CoreWindow.h.
DEFINE_COM_INTERFACE("45d64a29-a63e-4cb6-b498-5781d298cb4f", D3DalloyCoreWindowInterop)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND *window) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(boolean value) = 0;
};
static constexpr IID D3DALLOY_CORE_WINDOW_IID = d3dalloy::guid::make_guid("79b9d5f2-879e-4b89-b798-79e47598030c");

static constexpr IID D3DALLOY_NVEXT_GUID = d3dalloy::guid::make_guid("ba0af616-4a43-4259-815c-db3b89829905");

namespace d3dalloy {
enum class VendorExtension {
  None,
  Nvidia,
};

extern VendorExtension g_extension_enabled;
} // namespace d3dalloy
