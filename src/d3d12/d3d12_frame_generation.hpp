// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_frame_generation_interfaces.hpp"
#include <cstdint>
#include <memory>

namespace dxmt {
class MTLD3D12Device;
struct OwnedFrameGenerationFrame;
HRESULT CreateOwnedFrameGenerationState(MTLD3D12Device *, const MTL_FRAME_GENERATION_CREATE_DESC *,
    IMTLD3D12FrameGenerationState **);
HRESULT PrepareOwnedFrameGenerationFrame(MTLD3D12Device *, IMTLD3D12FrameGenerationState *,
    const MTL_FRAME_GENERATION_PREPARE_DESC *, std::shared_ptr<OwnedFrameGenerationFrame> &);
HRESULT GenerateOwnedFrameGenerationFrame(MTLD3D12Device *, IMTLD3D12FrameGenerationState *,
    const MTL_FRAME_GENERATION_GENERATE_DESC *, std::shared_ptr<OwnedFrameGenerationFrame> &);
HRESULT PrepareOwnedFrameGenerationOutput(MTLD3D12Device *, IMTLD3D12FrameGenerationState *,
    const MTL_FRAME_GENERATION_OUTPUT_DESC *, std::shared_ptr<OwnedFrameGenerationFrame> &);
HRESULT RecordOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &,
    ID3D12GraphicsCommandList *);
bool BeginOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &,
    uint64_t queue, uint64_t preceding_submission);
bool HasOwnedFrameGenerationFrameStarted(const std::shared_ptr<OwnedFrameGenerationFrame> &);
void CommitOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &,
    uint64_t queue, uint64_t submission);
void CompleteOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &, HRESULT);
} // namespace dxmt
