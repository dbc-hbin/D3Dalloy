#pragma once

#include "d3d12_pipeline.hpp"
#include "sha1/sha1_util.hpp"
#include <d3d12.h>
#include <array>
#include <memory>

namespace dxmt {

// macOS 27 permits device/texture fences without imposing a threadgroup rendezvous.
inline constexpr auto kD3D12ShaderMetalVersion = SM50_SHADER_METAL_320;

struct GraphicsPipelineExtras;
HRESULT PipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Sha1Digest &key,
                         const GraphicsPipelineExtras *extras = nullptr);
HRESULT PipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Sha1Digest &key);

/** Native compiler/driver identity every cache and library blob is tied to; false when unavailable. */
bool PipelineCacheIdentity(WMTPipelineCacheIdentity &identity);
/**
 * Key of a single-PSO cache blob, read from its header without validating the payload.
 * E_INVALIDARG: not a DXMT PSO blob. D3D12_ERROR_DRIVER_VERSION_MISMATCH: older/newer blob format.
 */
HRESULT PipelineCacheBlobKey(const void *data, size_t size, Sha1Digest &key);
HRESULT AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAGS control);
struct AutomaticPipelineCacheFlight;
struct PipelineCacheStorage;

// Cold uncached PSOs retain compiler-owned bytes. Restored/automatic PSOs use
// immutable serialized storage; neither representation contains GPU addresses.
class PipelineCache {
public:
  enum Stage : uint32_t { Vertex, Fragment, Compute, StageCount };

  PipelineCache() = default;
  ~PipelineCache();
  PipelineCache(const PipelineCache &) = delete;
  PipelineCache &operator=(const PipelineCache &) = delete;

  HRESULT Initialize(const D3D12_CACHED_PIPELINE_STATE &cached, const Sha1Digest &key,
                     uint32_t stages, uint64_t adapter);
  bool loaded() const { return bool(blob_); }
  sm50_bitcode_t *output(Stage stage) { return &compiled_[stage]; }
  SM50_COMPILED_BITCODE view(Stage stage);
  /** Compiler handle owning view(stage)'s parts, or null when they live in the loaded blob. */
  sm50_bitcode_t bitcode(Stage stage);
  HRESULT GetBlob(ID3DBlob **blob);
  // Called after the complete native PSO initialization, never just shader compilation.
  void Complete(HRESULT result) noexcept;
  bool CanRetryAutomatic(HRESULT result) const {
    return result == E_INVALIDARG && automatic_loaded_ && !retry_;
  }
  // Transfers an unfinished producer flight to a fresh PSO, never reuses native state.
  void RetryAutomatic(PipelineCache &fresh);

private:
  HRESULT LoadBlob(const D3D12_CACHED_PIPELINE_STATE &cached, ID3DBlob *owned = nullptr);

  Sha1Digest key_{};
  uint32_t stages_ = 0;
  uint64_t adapter_ = 0;
  std::array<SM50ShaderBitcode, StageCount> compiled_;
  std::shared_ptr<const PipelineCacheStorage> blob_;
  std::shared_ptr<AutomaticPipelineCacheFlight> flight_;
  Sha1Digest automatic_key_{};
  uint64_t automatic_generation_ = 0;
  bool automatic_loaded_ = false, retry_ = false, bypass_automatic_ = false;
};

} // namespace dxmt
