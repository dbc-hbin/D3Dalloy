// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_interfaces.hpp"
#include "d3d12_command_encoder.hpp"
#include <array>
#include <memory>
#include <functional>

namespace dxmt {
class MTLD3D12Device;
class MTLD3D12Resource;
struct OwnedTemporalFrame;
struct OwnedFrameGenerationFrame;
struct OwnedRayRegenerationFrame;

struct TemporalEncoderData : EncoderData {
  std::shared_ptr<OwnedTemporalFrame> frame;
  std::shared_ptr<OwnedFrameGenerationFrame> fg_frame;
  std::shared_ptr<OwnedRayRegenerationFrame> rr_frame;
};

HRESULT CreateOwnedTemporalState(MTLD3D12Device *device,
    const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc, IMTLD3D12TemporalUpscaleState **state);
HRESULT PrepareOwnedTemporalFrame(MTLD3D12Device *device, IMTLD3D12TemporalUpscaleState *state,
    const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc, std::shared_ptr<OwnedTemporalFrame> &frame);
std::array<MTLD3D12Resource *, 12> GetOwnedTemporalFrameResources(const std::shared_ptr<OwnedTemporalFrame> &frame);
// Owned id of the queue that encoded the feature's latest evaluation, or 0 before the first.
// Encode waits for the history timeline only when that queue differs from its own; the
// caller orders a same-queue predecessor on the GPU itself. Stable while the caller holds
// the device temporal submission mutex, which every temporal encode runs under.
uint64_t OwnedTemporalFrameHistoryQueue(const std::shared_ptr<OwnedTemporalFrame> &frame);
void EncodeOwnedTemporalFrame(const std::shared_ptr<OwnedTemporalFrame> &frame,
    WMT::MTL4CommandBuffer cmdbuf, WMT::Fence fence, uint64_t queue, uint64_t submission, uint64_t encoder,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event);
} // namespace dxmt
