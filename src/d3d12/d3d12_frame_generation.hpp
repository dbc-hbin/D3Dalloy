// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_frame_generation_interfaces.hpp"
#include "Metal4.hpp"
#include <array>
#include <functional>
#include <memory>

namespace dxmt {
class MTLD3D12Device;
class MTLD3D12Resource;
struct OwnedFrameGenerationFrame;
HRESULT CreateOwnedFrameGenerationState(MTLD3D12Device *, const MTL_FRAME_GENERATION_CREATE_DESC *,
    IMTLD3D12FrameGenerationState **);
HRESULT PrepareOwnedFrameGenerationFrame(MTLD3D12Device *, IMTLD3D12FrameGenerationState *,
    const MTL_FRAME_GENERATION_PREPARE_DESC *, std::shared_ptr<OwnedFrameGenerationFrame> &);
HRESULT GenerateOwnedFrameGenerationFrame(MTLD3D12Device *, IMTLD3D12FrameGenerationState *,
    const MTL_FRAME_GENERATION_GENERATE_DESC *, std::shared_ptr<OwnedFrameGenerationFrame> &);
std::array<MTLD3D12Resource *, 3> GetOwnedFrameGenerationResources(const std::shared_ptr<OwnedFrameGenerationFrame> &);
std::array<D3D12_RESOURCE_STATES, 3> GetOwnedFrameGenerationStates(const std::shared_ptr<OwnedFrameGenerationFrame> &);
bool IsOwnedFrameGenerationOutput(const std::shared_ptr<OwnedFrameGenerationFrame> &, size_t index);
void EncodeOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &,
    WMT::MTL4CommandBuffer, WMT::Fence,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event);
} // namespace dxmt
