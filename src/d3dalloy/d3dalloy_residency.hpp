#pragma once
#include "winemetal.h"
#include <cstdint>
#include "ftl.hpp"

namespace d3dalloy {

enum D3DALLOY_RESOURCE_RESIDENCY : uint32_t {
  D3DALLOY_RESOURCE_RESIDENCY_NULL = 0,
  D3DALLOY_RESOURCE_RESIDENCY_VERTEX_READ = 1 << 0,
  D3DALLOY_RESOURCE_RESIDENCY_VERTEX_WRITE = 1 << 1,
  D3DALLOY_RESOURCE_RESIDENCY_FRAGMENT_READ = 1 << 2,
  D3DALLOY_RESOURCE_RESIDENCY_FRAGMENT_WRITE = 1 << 3,
  D3DALLOY_RESOURCE_RESIDENCY_OBJECT_READ = 1 << 4,
  D3DALLOY_RESOURCE_RESIDENCY_OBJECT_WRITE = 1 << 5,
  D3DALLOY_RESOURCE_RESIDENCY_MESH_READ = 1 << 6,
  D3DALLOY_RESOURCE_RESIDENCY_MESH_WRITE = 1 << 7,
  D3DALLOY_RESOURCE_RESIDENCY_MESH_GS_READ = 1 << 8,
  D3DALLOY_RESOURCE_RESIDENCY_MESH_GS_WRITE = 1 << 9,
  D3DALLOY_RESOURCE_RESIDENCY_OBJECT_GS_READ = 1 << 10,
  D3DALLOY_RESOURCE_RESIDENCY_OBJECT_GS_WRITE = 1 << 11,
  D3DALLOY_RESOURCE_RESIDENCY_READ = D3DALLOY_RESOURCE_RESIDENCY_VERTEX_READ | D3DALLOY_RESOURCE_RESIDENCY_FRAGMENT_READ |
                                 D3DALLOY_RESOURCE_RESIDENCY_OBJECT_READ | D3DALLOY_RESOURCE_RESIDENCY_MESH_READ |
                                 D3DALLOY_RESOURCE_RESIDENCY_MESH_GS_READ | D3DALLOY_RESOURCE_RESIDENCY_OBJECT_GS_READ,
  D3DALLOY_RESOURCE_RESIDENCY_WRITE = D3DALLOY_RESOURCE_RESIDENCY_VERTEX_WRITE | D3DALLOY_RESOURCE_RESIDENCY_FRAGMENT_WRITE |
                                  D3DALLOY_RESOURCE_RESIDENCY_OBJECT_WRITE | D3DALLOY_RESOURCE_RESIDENCY_MESH_WRITE |
                                  D3DALLOY_RESOURCE_RESIDENCY_MESH_GS_WRITE | D3DALLOY_RESOURCE_RESIDENCY_OBJECT_GS_WRITE,
};

struct D3DALLOY_RESOURCE_RESIDENCY_STATE {
  uint64_t last_encoder_id = 0;
  D3DALLOY_RESOURCE_RESIDENCY last_residency_mask = D3DALLOY_RESOURCE_RESIDENCY_NULL;
};

constexpr bool
CheckResourceResidency(D3DALLOY_RESOURCE_RESIDENCY_STATE &state, uint64_t encoder_id, D3DALLOY_RESOURCE_RESIDENCY &requested) {
  if (encoder_id > state.last_encoder_id) {
    state.last_residency_mask = requested;
    state.last_encoder_id = encoder_id;
    requested = state.last_residency_mask;
    return true;
  }
  if (encoder_id == state.last_encoder_id) {
    if ((state.last_residency_mask & requested) == requested) {
      // it's already resident
      return false;
    }
    state.last_residency_mask = requested | state.last_residency_mask;
    requested = state.last_residency_mask;
    return true;
  }
  // invalid
  return false;
};

constexpr WMTResourceUsage
GetUsageFromResidencyMask(D3DALLOY_RESOURCE_RESIDENCY mask) {
  return ((mask & D3DALLOY_RESOURCE_RESIDENCY_READ) ? WMTResourceUsageRead : WMTResourceUsage(0)) |
         ((mask & D3DALLOY_RESOURCE_RESIDENCY_WRITE) ? WMTResourceUsageWrite : WMTResourceUsage(0));
}

constexpr WMTRenderStages
GetStagesFromResidencyMask(D3DALLOY_RESOURCE_RESIDENCY mask) {
  return ((mask & (D3DALLOY_RESOURCE_RESIDENCY_FRAGMENT_READ | D3DALLOY_RESOURCE_RESIDENCY_FRAGMENT_WRITE))
              ? WMTRenderStageFragment
              : WMTRenderStages(0)) |
         ((mask & (D3DALLOY_RESOURCE_RESIDENCY_VERTEX_READ | D3DALLOY_RESOURCE_RESIDENCY_VERTEX_WRITE))
              ? WMTRenderStageVertex
              : WMTRenderStages(0)) |
         ((mask & (D3DALLOY_RESOURCE_RESIDENCY_OBJECT_READ | D3DALLOY_RESOURCE_RESIDENCY_OBJECT_WRITE |
                   D3DALLOY_RESOURCE_RESIDENCY_OBJECT_GS_READ | D3DALLOY_RESOURCE_RESIDENCY_OBJECT_GS_WRITE))
              ? WMTRenderStageObject
              : WMTRenderStages(0)) |
         ((mask & (D3DALLOY_RESOURCE_RESIDENCY_MESH_READ | D3DALLOY_RESOURCE_RESIDENCY_MESH_WRITE |
                   D3DALLOY_RESOURCE_RESIDENCY_MESH_GS_READ | D3DALLOY_RESOURCE_RESIDENCY_MESH_GS_WRITE))
              ? WMTRenderStageMesh
              : WMTRenderStages(0));
}

} // namespace d3dalloy
