// SPDX-License-Identifier: MIT
#pragma once
#include "ffx_dx12.h"
#include <algorithm>
#include <vector>
// Match the SDK dynamic table layout, including its trailing slot. The same
// reflected span sizes both the immutable packet and each recorded table.
inline uint32_t ffxGetDX12DescriptorTableSize(const FfxResourceBinding* textures, uint32_t textureCount,
    const FfxResourceBinding* buffers, uint32_t bufferCount) {
    uint32_t maximum = textureCount + bufferCount;
    if (!maximum) return 0;
    for (uint32_t i = 0; i < textureCount; ++i)
        maximum = std::max(maximum, textures[i].slotIndex + textures[i].arrayIndex);
    for (uint32_t i = 0; i < bufferCount; ++i)
        maximum = std::max(maximum, buffers[i].slotIndex + buffers[i].arrayIndex);
    return maximum + 1;
}
// One immutable upload/descriptor arena per recorded REAL batch. Queue retirement
// owns its lifetime; another CPU recording cannot wrap over pending GPU inputs.
struct FfxDX12RecordingResources {
    ID3D12DescriptorHeap* descriptors = nullptr;
    ID3D12Resource* constants = nullptr;
    void* mapped = nullptr;
    uint32_t descriptorCount = 0, bytes = 0, offset = 0;
    std::vector<uint32_t*> interpolationResets, opticalFlowFrameIndices;
    ID3D12DescriptorHeap* previousDescriptors = nullptr;
    uint32_t previousBase = 0;
    ~FfxDX12RecordingResources();
};
HRESULT ffxBeginRecordingDX12(FfxInterface*, FfxDX12RecordingResources*);
void ffxEndRecordingDX12(FfxInterface*, FfxDX12RecordingResources*);
