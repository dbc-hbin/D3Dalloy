#pragma once

#include "d3d12_pipeline.hpp"
#include "sha1/sha1_util.hpp"
#include <d3d12.h>
#include <array>
#include <vector>

namespace dxmt {

inline constexpr auto kD3D12ShaderMetalVersion = SM50_SHADER_METAL_310;

HRESULT PipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Sha1Digest &key);
HRESULT PipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Sha1Digest &key);

// Cold PSOs retain compiler-owned bytes without copying them. Restored PSOs own
// one validated blob. Neither representation contains process/GPU addresses.
class PipelineCache {
public:
  enum Stage : uint32_t { Vertex, Fragment, Compute, StageCount };

  PipelineCache() = default;
  PipelineCache(const PipelineCache &) = delete;
  PipelineCache &operator=(const PipelineCache &) = delete;

  HRESULT Initialize(const D3D12_CACHED_PIPELINE_STATE &cached, const Sha1Digest &key,
                     uint32_t stages, uint64_t adapter);
  bool loaded() const { return !blob_.empty(); }
  sm50_bitcode_t *output(Stage stage) { return &compiled_[stage]; }
  SM50_COMPILED_BITCODE view(Stage stage);
  HRESULT GetBlob(ID3DBlob **blob);

private:
  Sha1Digest key_{};
  uint32_t stages_ = 0;
  uint64_t adapter_ = 0;
  std::array<SM50ShaderBitcode, StageCount> compiled_;
  std::array<SM50_COMPILED_BITCODE, StageCount> views_{};
  std::vector<uint8_t> blob_;
};

} // namespace dxmt
