/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "Metal.hpp"
#include "Metal4Pipeline.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "d3d12_shader_failure.hpp"
#include "log/log.hpp"
#include <chrono>

namespace dxmt {

class MTLD3D12ComputePipelineStateImpl : public MTLD3D12Pageable<MTLD3D12ComputePipelineState> {
  friend HRESULT CreateComputePipelineState(MTLD3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
                                             REFIID, void **);

  PipelineCache cache_;
  MTL_SHADER_REFLECTION ref_cs{};

  /* Everything the indirect-command-buffer variant of this pipeline is rebuilt from.
   * The function is held so its handle stays valid until that variant exists. */
  WMTComputePipelineInfo indirect_info{};
  WMT::Reference<WMT4::Function> indirect_compute_function;
  WMT::Reference<WMT::ComputePipelineState> indirect_pso;
  std::mutex indirect_pso_mutex;
  std::vector<uint8_t> statistics_cs_bytecode;
  std::vector<uint8_t> statistics_root_blob;
  WMT::Reference<WMT::ComputePipelineState> statistics_pso;
  WMT::Reference<WMT::ComputePipelineState> statistics_indirect_pso;
  WMT::Reference<WMT4::Function> statistics_function;
  std::mutex statistics_pso_mutex;

public:
  MTLD3D12ComputePipelineStateImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12ComputePipelineState>(pDevice) {
    IsComputePipelineState = 1;
  }

  HRESULT
  Initialize(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc) {
    const bool trace = owned_graphics::TraceEnabled();
    using Clock = std::chrono::steady_clock;
    std::array<Clock::time_point, 8> times{};
    if (trace) times[0] = Clock::now();
    Sha1Digest key;
    if (HRESULT hr = PipelineCacheKey(*pDesc, key); FAILED(hr))
      return hr;
    if (HRESULT hr = cache_.Initialize(pDesc->CachedPSO, key, 1u << PipelineCache::Compute,
                                      device_->GetMTLDevice().registryID()); FAILED(hr))
      return hr;

    if (trace) times[1] = Clock::now();
    SM50Shader shader_cs;
    SM50Error sm50_err;

    SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
    rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
    if (pDesc->pRootSignature) {
      rootsig.bytecode_length = static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
    } else {
      rootsig.bytecode = pDesc->CS.pShaderBytecode;
      rootsig.bytecode_length = pDesc->CS.BytecodeLength;
    }
    rootsig.next = nullptr;
    const auto *cs_source = static_cast<const uint8_t *>(pDesc->CS.pShaderBytecode);
    statistics_cs_bytecode.assign(cs_source, cs_source + pDesc->CS.BytecodeLength);
    const auto *root_source = static_cast<const uint8_t *>(rootsig.bytecode);
    if (pDesc->pRootSignature)
      statistics_root_blob.assign(root_source, root_source + rootsig.bytecode_length);

    SM50_SHADER_COMMON_DATA common;
    common.flags = {};
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    common.next = &rootsig;

    if (HRESULT hr = InitializeShader(device_->GetShaderArtifactCache(), "CS", pDesc->CS, &shader_cs, &ref_cs, &resource_bindings); FAILED(hr))
      return hr;
    uses_atomic64 = ref_cs.HasAtomic64 != 0;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
    atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
    atomic_phase.next = common.next;
    if (uses_atomic64) { common.next = &atomic_phase; common.metal_version = SM50_SHADER_METAL_410; }

    threadgroup_size = {ref_cs.ThreadgroupSize[0], ref_cs.ThreadgroupSize[1], ref_cs.ThreadgroupSize[2]};
    if (trace) times[2] = Clock::now();

    if (cache_.Compile(PipelineCache::Compute, device_->GetShaderArtifactCache(), pDesc->CS, shader_cs,
                       (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, "cs_main", &sm50_err)) {
      RecordShaderCompileFailure("CS", pDesc->CS, rootsig,
                                 reinterpret_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&common),
                                 "cs_main", SM50GetErrorMessageString(sm50_err), pDesc->pRootSignature != nullptr);
      ERR("Failed to compile cs shader: ", SM50GetErrorMessageString(sm50_err));
      return E_FAIL;
    }
    if (trace) times[3] = Clock::now();

    const auto cs_bitcode_compiled = cache_.view(PipelineCache::Compute);

    if (HRESULT hr = compute_constants.Initialize(device_, cs_bitcode_compiled, cache_.bitcode(PipelineCache::Compute)); FAILED(hr))
      return hr;
    if (trace) times[4] = Clock::now();

    WMT::Reference<WMT::Error> err;

    auto cs_lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), cs_bitcode_compiled,
                                                          cache_.bitcode(PipelineCache::Compute), err);
    if (!cs_lib)
      return cache_.loaded() ? E_INVALIDARG : E_FAIL;
    if (trace) times[5] = Clock::now();

    auto cs_func = cs_lib.newFunction("cs_main");
    if (!cs_func)
      return cache_.loaded() ? E_INVALIDARG : E_FAIL;
    if (trace) times[6] = Clock::now();

    // PSO
    {
      WMTComputePipelineInfo info;
      WMT::InitializeComputePipelineInfo(info);
      info.compute_function = cs_func;
      
      /* The indirect-command-buffer variant is built separately, on demand:
       * GetIndirectCommandBufferPipelineState. */
      info.support_indirect_command_buffers = false;

      pso = device_->GetMetal4Compiler().newComputePipelineState(info, err);
      if (!pso) {
        ERR("Failed to create compute PSO: ", err.description().getUTF8String());
        return E_FAIL;
      }

      indirect_compute_function = cs_func;
      indirect_info = info;
    }

    if (trace) {
      times[7] = Clock::now();
      const char *phases[] = {"cache", "reflection", "compile", "constants", "library", "function", "pipeline"};
      const auto creation = owned_graphics::NewObjectId();
      for (size_t i = 0; i < std::size(phases); ++i)
        owned_graphics::Trace("pipeline_compute_timing", ",\"creation\":", creation, ",\"key\":\"", key.string(),
                              "\",\"loaded\":", unsigned(cache_.loaded()), ",\"phase\":\"", phases[i], "\",\"ns\":",
                              std::chrono::duration_cast<std::chrono::nanoseconds>(times[i + 1] - times[i]).count(),
                              ",\"library_bytes\":", cs_bitcode_compiled.Size,
                              ",\"constant_bytes\":", cs_bitcode_compiled.ConstantDataSize,
                              ",\"reflection_copy_bytes\":", statistics_cs_bytecode.size() + statistics_root_blob.size());
    }
    return S_OK;
  }

  WMT::ComputePipelineState
  GetIndirectCommandBufferPipelineState() override {
    std::lock_guard<std::mutex> lock(indirect_pso_mutex);
    if (!indirect_pso) {
      WMT::Reference<WMT::Error> error;
      auto info = indirect_info;
      info.compute_function = indirect_compute_function.handle;
      info.support_indirect_command_buffers = true;
      indirect_pso = device_->GetMetal4Compiler().newComputePipelineState(info, error);
      if (!indirect_pso)
        ERR("Failed to create indirect command buffer PSO: ", error.description().getUTF8String());
    }
    return indirect_pso;
  }

  WMT::ComputePipelineState
  GetPipelineStatisticsPipelineState(bool indirect) override {
    std::lock_guard<std::mutex> lock(statistics_pso_mutex);
    WMT::Reference<WMT::Error> error;
    if (!statistics_function) {
      SM50Error compile_error;
      SM50_SHADER_COMMON_DATA common{};
      common.type = SM50_SHADER_COMMON;
      common.metal_version = kD3D12ShaderMetalVersion;
      SM50_SHADER_COMPILATION_ARGUMENT_DATA atomic_phase{};
      atomic_phase.type = SM50_SHADER_D3D12_ATOMIC64_PHASE;
      if (uses_atomic64) { common.next = &atomic_phase; common.metal_version = SM50_SHADER_METAL_410; }
      SM50_SHADER_COMPILATION_ARGUMENT_DATA statistics{};
      statistics.type = SM50_SHADER_D3D12_PIPELINE_STATS;
      statistics.next = &common;
      SM50_SHADER_ROOT_SIGNATURE_DATA root{};
      root.type = SM50_SHADER_ROOT_SIGNATURE;
      root.bytecode = statistics_root_blob.empty() ? statistics_cs_bytecode.data() : statistics_root_blob.data();
      root.bytecode_length = statistics_root_blob.empty() ? statistics_cs_bytecode.size()
                                                         : statistics_root_blob.size();
      root.next = &statistics;
      SM50ShaderBitcode compiled;
      std::shared_ptr<const ShaderStageArtifact> artifact;
      if (device_->GetShaderArtifactCache().CompileShader({statistics_cs_bytecode.data(), statistics_cs_bytecode.size()},
                      reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&root),
                      "cs_main", &compiled, &compile_error, artifact)) {
        ERR("Failed to compile statistics CS: ", SM50GetErrorMessageString(compile_error));
        return {};
      }
      SM50_COMPILED_BITCODE bytecode{};
      if (artifact) bytecode = artifact->view;
      else SM50GetCompiledBitcode(compiled, &bytecode);
      if (FAILED(pipeline_statistics_compute_constants.Initialize(device_, bytecode, compiled)))
        return {};
      auto lib = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), bytecode, compiled, error);
      if (!lib)
        return {};
      statistics_function = lib.newFunction("cs_main");
      if (!statistics_function)
        return {};
    }
    auto &variant = indirect ? statistics_indirect_pso : statistics_pso;
    if (!variant) {
      auto info = indirect_info;
      info.compute_function = statistics_function.handle;
      info.support_indirect_command_buffers = indirect;
      
      
      variant = device_->GetMetal4Compiler().newComputePipelineState(info, error);
      if (!variant)
        ERR("Failed to create statistics compute PSO: ", error.description().getUTF8String());
    }
    return variant;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12ComputePipelineState: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) override {
    return cache_.GetBlob(blob);
  }
};

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
) {
  InitReturnPtr(ppPipelineState);
  if (!ppPipelineState || !pDesc || pDesc->NodeMask > 1 || !pDesc->CS.pShaderBytecode || !pDesc->CS.BytecodeLength ||
      (static_cast<UINT>(pDesc->Flags) & ~static_cast<UINT>(D3D12_PIPELINE_STATE_FLAG_DEBUG)) ||
      (pDesc->CachedPSO.CachedBlobSizeInBytes && !pDesc->CachedPSO.pCachedBlob))
    return E_INVALIDARG;
  if (pDesc->pRootSignature) {
    Com<ID3D12Device> owner;
    if (FAILED(pDesc->pRootSignature->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
        owner.ptr() != static_cast<ID3D12Device *>(pDevice))
      return E_INVALIDARG;
  }
  auto pso = Com(new MTLD3D12ComputePipelineStateImpl(pDevice));
  HRESULT hr = pso->Initialize(pDesc);
  if (pso->cache_.CanRetryAutomatic(hr)) {
    auto fresh = Com(new MTLD3D12ComputePipelineStateImpl(pDevice));
    pso->cache_.RetryAutomatic(fresh->cache_);
    pso = std::move(fresh); // Rebuild bindings/constants/native objects on a fresh PSO.
    hr = pso->Initialize(pDesc);
  }
  pso->cache_.Complete(hr);
  if (FAILED(hr))
    return hr;
  return pso->QueryInterface(riid, ppPipelineState);
};

} // namespace dxmt
