#pragma once
#include "d3d11_device.hpp"
#include "util_d3dkmt.h"

namespace d3dalloy {

struct MTLD3D11Fence : public ID3D11Fence {
  WMT::Reference<WMT::SharedEvent> event;
  D3DKMT_HANDLE local_kmt = 0;
};

HRESULT
CreateFence(MTLD3D11Device *pDevice, UINT64 InitialValue,
            D3D11_FENCE_FLAG Flags, REFIID riid, void **ppFence);

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource,
                REFIID riid, void **ppFence);

} // namespace d3dalloy