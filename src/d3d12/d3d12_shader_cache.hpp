#pragma once
#include <d3d12.h>
#include <cstddef>
#include <memory>
#include <string>
#include "sha1/sha1_util.hpp"

namespace dxmt {
class MTLD3D12Device;
struct ShaderStageArtifact;
HRESULT CreateShaderCacheSession(MTLD3D12Device *device, const D3D12_SHADER_CACHE_SESSION_DESC *desc,
                                 const void *driver_identity, size_t driver_identity_size,
                                 REFIID riid, void **session);
HRESULT ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS control);
// The key includes the pipeline, adapter and compiler identity. The caller validates the v5 blob.
HRESULT ReadAutomaticPipelineCache(const Sha1Digest &key, ID3DBlob **blob);
void WriteAutomaticPipelineCache(const Sha1Digest &key, ID3DBlob *blob);
void RemoveAutomaticPipelineCache(const Sha1Digest &key);
HRESULT ClearAutomaticPipelineCacheDisk();
// Stage records are independently keyed and validated; failures are cache misses.
HRESULT ReadAutomaticStageArtifact(const std::string &key, std::shared_ptr<const ShaderStageArtifact> &artifact);
void WriteAutomaticStageArtifact(const std::string &key, const ShaderStageArtifact &artifact);
void QueueAutomaticStageArtifact(const std::string &key, std::shared_ptr<const ShaderStageArtifact> artifact,
                                 uint64_t generation);
void PublishAutomaticStageArtifact(const std::string &key, std::shared_ptr<const ShaderStageArtifact> artifact,
                                   uint64_t generation);
} // namespace dxmt
