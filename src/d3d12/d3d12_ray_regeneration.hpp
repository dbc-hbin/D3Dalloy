// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_ray_regeneration_interfaces.hpp"
#include "Metal4.hpp"
#include <array>
#include <functional>
#include <memory>

namespace dxmt {
class MTLD3D12Device;
class MTLD3D12Resource;
struct OwnedRayRegenerationFrame;
HRESULT CreateOwnedRayRegenerationState(MTLD3D12Device *, const MTL_RAY_REGENERATION_CREATE_DESC *,
    IMTLD3D12RayRegenerationState **);
HRESULT PrepareOwnedRayRegenerationFrame(MTLD3D12Device *, IMTLD3D12RayRegenerationState *,
    const MTL_RAY_REGENERATION_FRAME_DESC *, std::shared_ptr<OwnedRayRegenerationFrame> &);
std::array<MTLD3D12Resource *, MTL_RAY_REGENERATION_RESOURCE_COUNT>
GetOwnedRayRegenerationResources(const std::shared_ptr<OwnedRayRegenerationFrame> &);
std::array<D3D12_RESOURCE_STATES, MTL_RAY_REGENERATION_RESOURCE_COUNT>
GetOwnedRayRegenerationStates(const std::shared_ptr<OwnedRayRegenerationFrame> &);
bool IsOwnedRayRegenerationOutput(size_t index);
void EncodeOwnedRayRegenerationFrame(const std::shared_ptr<OwnedRayRegenerationFrame> &,
    WMT::MTL4CommandBuffer, WMT::Fence,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event);
} // namespace dxmt
