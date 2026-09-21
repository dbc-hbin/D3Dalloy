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
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "log/log.hpp"

namespace dxmt {

class MTLD3D12ComputePipelineStateImpl : public MTLD3D12Pageable<MTLD3D12ComputePipelineState> {

  PipelineCache cache_;
  MTL_SHADER_REFLECTION ref_cs{};

  /* Everything the indirect-command-buffer variant of this pipeline is rebuilt from.
   * The function is held so its handle stays valid until that variant exists. */
  WMTComputePipelineInfo indirect_info{};
  WMT::Reference<WMT::Function> indirect_compute_function;
  WMT::Reference<WMT::ComputePipelineState> indirect_pso;
  std::mutex indirect_pso_mutex;

public:
  MTLD3D12ComputePipelineStateImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12ComputePipelineState>(pDevice) {
    IsComputePipelineState = 1;
  }

  HRESULT
  Initialize(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc) {
    Sha1Digest key;
    if (HRESULT hr = PipelineCacheKey(*pDesc, key); FAILED(hr))
      return hr;
    auto metal = device_->GetMTLDevice();
    if (HRESULT hr = cache_.Initialize(pDesc->CachedPSO, key, 1u << PipelineCache::Compute, metal.registryID()); FAILED(hr))
      return hr;

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

    SM50_SHADER_COMMON_DATA common;
    common.flags = {};
    common.type = SM50_SHADER_COMMON;
    common.metal_version = kD3D12ShaderMetalVersion;
    common.next = &rootsig;

    if (HRESULT hr = InitializeShader("CS", pDesc->CS, &shader_cs, &ref_cs); FAILED(hr))
      return hr;

    threadgroup_size = {ref_cs.ThreadgroupSize[0], ref_cs.ThreadgroupSize[1], ref_cs.ThreadgroupSize[2]};

    if (!cache_.loaded() && SM50Compile(shader_cs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, "cs_main",
                                       cache_.output(PipelineCache::Compute), &sm50_err)) {
      ERR("Failed to compile cs shader");
      return E_FAIL;
    }

    const auto cs_bitcode_compiled = cache_.view(PipelineCache::Compute);

    auto cs_data = WMT::MakeDispatchData(cs_bitcode_compiled.Data, cs_bitcode_compiled.Size);

    if (HRESULT hr = compute_constants.Initialize(metal, cs_bitcode_compiled); FAILED(hr))
      return hr;

    WMT::Reference<WMT::Error> err;

    auto cs_lib = metal.newLibrary(cs_data, err);
    if (!cs_lib)
      return cache_.loaded() ? E_INVALIDARG : E_FAIL;

    auto cs_func = cs_lib.newFunction("cs_main");
    if (!cs_func)
      return cache_.loaded() ? E_INVALIDARG : E_FAIL;

    // PSO
    {
      WMTComputePipelineInfo info;
      WMT::InitializeComputePipelineInfo(info);
      info.compute_function = cs_func;
      if (compute_constants.buffer)
        info.immutable_buffers |= 1u << SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS;
      /* The indirect-command-buffer variant is built separately, on demand:
       * GetIndirectCommandBufferPipelineState. */
      info.support_indirect_command_buffers = false;

      pso = metal.newComputePipelineState(info, err);
      if (!pso) {
        ERR("Failed to create compute PSO: ", err.description().getUTF8String());
        return E_FAIL;
      }

      indirect_compute_function = cs_func;
      indirect_info = info;
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
      indirect_pso = device_->GetMTLDevice().newComputePipelineState(info, error);
      if (!indirect_pso)
        ERR("Failed to create indirect command buffer PSO: ", error.description().getUTF8String());
    }
    return indirect_pso;
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
  if (FAILED(hr))
    return hr;
  return pso->QueryInterface(riid, ppPipelineState);
};

} // namespace dxmt
