#pragma once

#include "d3d12_pipeline.hpp"
#include "sha1/sha1_util.hpp"
#include "Metal4Pipeline.hpp"
#include <d3d12.h>
#include <array>
#include <memory>
#include <functional>
#include <initializer_list>

namespace dxmt {

// D3D12 device admission requires a Metal 4 compiler; Atomic64 stages select 4.1.
inline constexpr auto kD3D12ShaderMetalVersion = SM50_SHADER_METAL_400;

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
struct ShaderStageArtifact {
  std::vector<uint8_t> library, metadata;
  std::vector<uint64_t> constants;
  std::vector<SM50_CONSTANT_RELOCATION> relocations;
  SM50_COMPILED_BITCODE view{};
  size_t size() const {
    return library.size() + metadata.size() + constants.size() * sizeof(uint64_t) +
           relocations.size() * sizeof(SM50_CONSTANT_RELOCATION);
  }
};
class DeviceShaderArtifactCache;

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
  /** Compiler handle owning view(stage)'s parts, or null for immutable serialized storage. */
  sm50_bitcode_t bitcode(Stage stage);
  // Only immutable serialized output is shared; shader/LLVM handles stay invocation-local.
  int Compile(Stage stage, DeviceShaderArtifactCache &artifacts, D3D12_SHADER_BYTECODE source,
              sm50_shader_t shader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
              const char *name, sm50_error_t *error);
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
  std::array<std::shared_ptr<const ShaderStageArtifact>, StageCount> stage_artifacts_;
  std::shared_ptr<const PipelineCacheStorage> blob_;
  std::shared_ptr<AutomaticPipelineCacheFlight> flight_;
  Sha1Digest automatic_key_{};
  uint64_t automatic_generation_ = 0;
  bool automatic_loaded_ = false, retry_ = false, bypass_automatic_ = false;
};

// Owned by one D3D12 device/compiler. Functions retain libraries independently of eviction.
class DeviceShaderArtifactCache {
public:
  DeviceShaderArtifactCache();
  ~DeviceShaderArtifactCache();
  DeviceShaderArtifactCache(const DeviceShaderArtifactCache &) = delete;
  DeviceShaderArtifactCache &operator=(const DeviceShaderArtifactCache &) = delete;
  HRESULT Reflect(D3D12_SHADER_BYTECODE source, sm50_shader_t *shader, MTL_SHADER_REFLECTION &reflection,
                  std::vector<MTL_SM50_SHADER_ARGUMENT> *bindings);
  bool RasterOrderedMarker(D3D12_SHADER_BYTECODE source, D3D12_SHADER_BYTECODE root, sm50_shader_t shader);
  int InitializeCompiler(D3D12_SHADER_BYTECODE source, sm50_shader_t *shader, sm50_error_t *error);
  WMT::Reference<WMT4::Library> Library(WMT4::Compiler compiler, const SM50_COMPILED_BITCODE &code,
                                      sm50_bitcode_t owner, WMT::Error &error);
  WMT::Reference<WMT4::Function> SampleMaskFunction(WMT4::Library library, const char *name,
      D3D12_SHADER_BYTECODE root, uint32_t mask, WMT::Error &error);
  // Synchronous/non-escaping: pass std::ref(local lowering lambda) to avoid callable allocation.
  int Compile(const char *family, std::initializer_list<D3D12_SHADER_BYTECODE> sources,
              SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name, sm50_bitcode_t *output,
              sm50_error_t *error, std::shared_ptr<const ShaderStageArtifact> &artifact,
              const std::function<int(sm50_bitcode_t *, sm50_error_t *)> &compile);
  int CompileShader(D3D12_SHADER_BYTECODE source, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                    const char *name, sm50_bitcode_t *output, sm50_error_t *error,
                    std::shared_ptr<const ShaderStageArtifact> &artifact);

private:
  friend class PipelineCache;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dxmt
