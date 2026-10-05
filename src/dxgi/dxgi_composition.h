#pragma once

#include <stdint.h>
#include <unknwn.h>

/* Private, shared by owned DXGI/D3D12 and the isolated Wine dcomp frontend.
 * The returned native surface is borrowed while the COM interface is retained.
 * A commit snapshot must retain its native objects before releasing pending state.
 */
static const IID IID_IMTLDXGICompositionSurface = {
  0xcffad8a2, 0xf8e6, 0x4bfe, {0x95, 0xf1, 0xd2, 0x9b, 0x41, 0x73, 0x78, 0x32}
};

#undef INTERFACE
#define INTERFACE IMTLDXGICompositionSurface
DECLARE_INTERFACE_(IMTLDXGICompositionSurface, IUnknown) {
  STDMETHOD(QueryInterface)(THIS_ REFIID iid, void **object) PURE;
  STDMETHOD_(ULONG, AddRef)(THIS) PURE;
  STDMETHOD_(ULONG, Release)(THIS) PURE;
  STDMETHOD(GetCompositionSurface)(THIS_ uint64_t *surface) PURE;
};
#undef INTERFACE
