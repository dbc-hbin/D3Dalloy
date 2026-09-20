// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_interfaces.hpp"
#include "d3d12_command_encoder.hpp"
#include <memory>

namespace dxmt {
class MTLD3D12Device;
struct OwnedTemporalFrame;

struct TemporalEncoderData : EncoderData {
  std::shared_ptr<OwnedTemporalFrame> frame;
};

HRESULT CreateOwnedTemporalState(MTLD3D12Device *device,
    const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc, IMTLD3D12TemporalUpscaleState **state);
HRESULT PrepareOwnedTemporalFrame(MTLD3D12Device *device, IMTLD3D12TemporalUpscaleState *state,
    const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc, std::shared_ptr<OwnedTemporalFrame> &frame);
void EncodeOwnedTemporalFrame(const std::shared_ptr<OwnedTemporalFrame> &frame,
    WMT::CommandBuffer cmdbuf, WMT::Fence fence, uint64_t queue, uint64_t submission, uint64_t encoder);
} // namespace dxmt
