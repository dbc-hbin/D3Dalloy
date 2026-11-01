#pragma once

#include "dxgiformat.h"
#include "log/log.hpp"
#include <cstdint>

namespace d3dalloy {

/*
 * https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/DirectXTexUtil.cpp
 * Keep internal linkage so each frontend uses its own DLL's logger.
 */
static inline uint32_t
GetMonitorFormatBpp(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    return 32;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return 64;

  default:
    Logger::warn(str::format("GetMonitorFormatBpp: Unknown format: ", format));
    return 32;
  }
}

} // namespace d3dalloy
