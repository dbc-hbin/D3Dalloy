#pragma once

#include "nvngx.hpp"

namespace dxmt {

NVNGX_RESULT NVNGXD3D12InitDevice(ID3D12Device *device);
void NVNGXD3D12ShutdownAll();
NVNGX_RESULT NVNGXD3D12ShutdownDevice(ID3D12Device *device);

} // namespace dxmt
