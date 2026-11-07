#pragma once

#include "d3d12_pipeline.hpp"
#include "owned_shader_bytecode.hpp"
#include "owned_stage_key.hpp"
#include "sha1/sha1_util.hpp"
#include "Metal4Pipeline.hpp"
#include "dxil_raytracing.h"
#include "dxil_workgraph.h"
#include <d3d12.h>
#include <array>
#include <memory>
#include <functional>
#include <initializer_list>
#include <string>

namespace d3dalloy {

// D3D12 device admission requires a Metal 4 compiler; Atomic64 stages select 4.1.
inline constexpr auto kD3D12ShaderMetalVersion = SM50_SHADER_METAL_400;

class MTLD3D12Device;
struct ShaderImmutableConstants;
WMT::Reference<WMT4::Function> CompilePipelineVariant(
    MTLD3D12Device *device, const SharedShaderBytecode &shader, const SharedShaderBytecode &explicit_root,
    const char *name, void *next, ShaderImmutableConstants &constants, bool root_independent = false);

struct GraphicsPipelineExtras;
HRESULT PipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Sha1Digest &key,
                         const GraphicsPipelineExtras *extras = nullptr, Sha1Digest *effective = nullptr);
HRESULT PipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Sha1Digest &key);

/** Native compiler/driver identity every cache and library blob is tied to; false when unavailable. */
bool PipelineCacheIdentity(WMTPipelineCacheIdentity &identity);
/**
 * Key of a single-PSO cache blob, read from its header without validating the payload.
 * E_INVALIDARG: not a D3Dalloy PSO blob. D3D12_ERROR_DRIVER_VERSION_MISMATCH: older/newer blob format.
 */
HRESULT PipelineCacheBlobKey(const void *data, size_t size, Sha1Digest &key);
HRESULT AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAGS control);
struct AutomaticPipelineCacheFlight;
struct PipelineCacheStorage;
struct ShaderStageArtifact {
  SM50Error failure;
  int failure_result = 0;
  size_t failure_bytes = 0;
  std::vector<uint8_t> library, metadata;
  std::vector<uint32_t> metadata_sizes;
  std::vector<uint64_t> constants;
  std::vector<SM50_CONSTANT_RELOCATION> relocations;
  SM50_COMPILED_BITCODE view{};
  size_t size() const {
    return failure_bytes + library.size() + metadata.size() + constants.size() * sizeof(uint64_t) +
           relocations.size() * sizeof(SM50_CONSTANT_RELOCATION);
  }
};
class DeviceShaderArtifactCache;

struct NativePipelineArtifact {
  WMT::Reference<WMT::ComputePipelineState> compute;
  WMT::Reference<WMT::RenderPipelineState> render;
  std::vector<WMT::Reference<WMT4::Function>> parents;
  WMT::Reference<WMT::Error> error;
  bool retained() const { return compute || render; }
  size_t size() const { return 0; }
};

// Immutable native DXR executable only. State-object identifiers, relocated
// constants, visible/intersection tables and dispatch bindings are never cached.
struct NativeRaytracingArtifact : NativePipelineArtifact {
  bool shader_validation = false;
  WMT::Reference<WMT4::Library> library;
  std::vector<WMT::Reference<WMT::FunctionHandle>> handles;
};

// CPU-only output; every consumer creates its own native objects and relocated constants.
struct WorkGraphArtifact : ShaderStageArtifact {
  SM50_WG_NODE_COMPILED_INFO info{};
  size_t size() const { return ShaderStageArtifact::size() + sizeof(info); }
};
struct RaytracingArtifact : ShaderStageArtifact {
  SM50ShaderBitcode reusable_exports;
  size_t reusable_bytes = 0;
  std::vector<SM50_RT_FUNCTION_INFO> functions;
  std::vector<std::string> names, export_names;
  size_t size() const {
    size_t bytes = ShaderStageArtifact::size() + reusable_bytes + functions.size() * sizeof(SM50_RT_FUNCTION_INFO);
    for (const auto &name : names) bytes += name.size();
    for (const auto &name : export_names) bytes += name.size();
    return bytes;
  }
};

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
                     uint32_t stages, uint64_t adapter, const Sha1Digest *effective = nullptr);
  bool loaded() const { return bool(blob_); }
  sm50_bitcode_t *output(Stage stage) { return &compiled_[stage]; }
  SM50_COMPILED_BITCODE view(Stage stage);
  /** Compiler handle owning view(stage)'s parts, or null for immutable serialized storage. */
  sm50_bitcode_t bitcode(Stage stage);
  // Only immutable serialized output is shared; shader/LLVM handles stay invocation-local.
  int Compile(Stage stage, DeviceShaderArtifactCache &artifacts, D3D12_SHADER_BYTECODE source,
              sm50_shader_t shader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
              const char *name, sm50_error_t *error, ID3D12RootSignature *root,
              const MTL_SHADER_REFLECTION &reflection);
  const Sha1Digest &stageKey(Stage stage) const { return stage_keys_[stage]; }
  HRESULT GetBlob(ID3DBlob **blob);
  // Internal immutable owner only. Never return this writable COM view to an app.
  HRESULT GetPrivateBlob(ID3DBlob **blob);
  // Called after the complete native PSO initialization, never just shader compilation.
  // `retain = false` (deferred native link) releases same-key waiters now but holds memory
  // retention and disk publication until CommitDeferred(true) confirms the link succeeded.
  void Complete(HRESULT result, bool retain = true) noexcept;
  // Deferred link result: success retains/publishes the held entry, failure drops a disk entry.
  void CommitDeferred(bool linked) noexcept;
  bool CanRetryAutomatic(HRESULT result) const {
    return result == E_INVALIDARG && automatic_loaded_ && !retry_;
  }
  // Transfers an unfinished producer flight to a fresh PSO, never reuses native state.
  void RetryAutomatic(PipelineCache &fresh);

private:
  HRESULT SerializeBlob(const Sha1Digest &key, ID3DBlob **blob);
  HRESULT LoadBlob(const D3D12_CACHED_PIPELINE_STATE &cached, ID3DBlob *owned = nullptr);
  std::mutex serialized_mutex_;
  Com<ID3DBlob> serialized_;

  Sha1Digest key_{}, effective_key_{};
  uint32_t stages_ = 0;
  uint64_t adapter_ = 0;
  std::array<SM50ShaderBitcode, StageCount> compiled_;
  std::array<std::shared_ptr<const ShaderStageArtifact>, StageCount> stage_artifacts_;
  std::array<Sha1Digest, StageCount> stage_keys_{};
  std::shared_ptr<const PipelineCacheStorage> blob_;
  std::shared_ptr<AutomaticPipelineCacheFlight> flight_;
  Sha1Digest automatic_key_{};
  uint64_t automatic_generation_ = 0;
  bool automatic_loaded_ = false, retry_ = false, bypass_automatic_ = false;
  // Complete(hr, false): the entry CommitDeferred publishes once the native link succeeds.
  bool deferred_retain_ = false, deferred_publish_ = false, deferred_disk_hit_ = false;
  Sha1Digest deferred_key_{};
  uint64_t deferred_generation_ = 0;
};

// Owned by one D3D12 device/compiler. Functions retain libraries independently of eviction.
class DeviceShaderArtifactCache {
public:
  DeviceShaderArtifactCache();
  ~DeviceShaderArtifactCache();
  DeviceShaderArtifactCache(const DeviceShaderArtifactCache &) = delete;
  DeviceShaderArtifactCache &operator=(const DeviceShaderArtifactCache &) = delete;
  SharedShaderBytecode RetainBytecode(const void *data, size_t size);
  int CompileWorkGraph(const SM50_WG_NODE_COMPILE_DESC &desc, sm50_bitcode_t *output,
      SM50_COMPILED_BITCODE &code, SM50_WG_NODE_COMPILED_INFO &info, sm50_error_t *error,
      std::shared_ptr<const WorkGraphArtifact> &artifact, bool bypass = false);
  int CompileRaytracing(const SM50_RT_PIPELINE_DESC &desc, sm50_bitcode_t *output,
      SM50_COMPILED_BITCODE &code, std::vector<SM50_RT_FUNCTION_INFO> &functions,
      uint32_t &count, sm50_error_t *error, std::shared_ptr<const RaytracingArtifact> &artifact,
      bool bypass = false);
  HRESULT Reflect(D3D12_SHADER_BYTECODE source, sm50_shader_t *shader, MTL_SHADER_REFLECTION &reflection,
                  std::vector<MTL_SM50_SHADER_ARGUMENT> *bindings);
  bool RasterOrderedMarker(D3D12_SHADER_BYTECODE source, D3D12_SHADER_BYTECODE root, sm50_shader_t shader);
  int InitializeCompiler(D3D12_SHADER_BYTECODE source, sm50_shader_t *shader, sm50_error_t *error);
  WMT::Reference<WMT4::Library> Library(WMT4::Compiler compiler, const SM50_COMPILED_BITCODE &code,
                                      sm50_bitcode_t owner, WMT::Error &error);
  WMT::Reference<WMT4::Function> SampleMaskFunction(WMT4::Library library, const char *name,
      uint32_t mask, WMT::Error &error);
  WMT::Reference<WMT4::Function> Function(WMT4::Library library, const char *name);
  std::shared_ptr<const NativeRaytracingArtifact> NativeRaytracing(
      WMT4::Compiler compiler, WMT4::Library library, const std::vector<SM50_RT_FUNCTION_INFO> &functions,
      uint32_t max_call_stack_depth, bool indirect, WMT::Error &error);
  std::shared_ptr<const NativePipelineArtifact> NativePipeline(WMT4::Compiler compiler,
      const WMTComputePipelineInfo &info, WMT::Error &error);
  std::shared_ptr<const NativePipelineArtifact> NativePipeline(WMT4::Compiler compiler,
      const WMTRenderPipelineInfo &info, WMT::Error &error);
  std::shared_ptr<const NativePipelineArtifact> NativePipeline(WMT4::Compiler compiler,
      const WMTMeshRenderPipelineInfo &info, WMT::Error &error);
  // Synchronous/non-escaping: pass std::ref(local lowering lambda) to avoid callable allocation.
  int Compile(const char *family, std::initializer_list<D3D12_SHADER_BYTECODE> sources,
              SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name, sm50_bitcode_t *output,
              sm50_error_t *error, std::shared_ptr<const ShaderStageArtifact> &artifact,
              const std::function<int(sm50_bitcode_t *, sm50_error_t *)> &compile,
              Sha1Digest *stage_key = nullptr, bool root_independent = false,
              std::span<const uint32_t> vertex_table_key = {});

private:
  friend class PipelineCache;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace d3dalloy
