#pragma once

#include "d3d12_device.hpp"

namespace dxmt {

/**
 * One validated D3D12_PIPELINE_STATE_STREAM_DESC, flattened into the classic desc it selects.
 * `serialized_root` owns a root signature created from a SERIALIZED_ROOT_SIGNATURE subobject;
 * the descs point at it, so they are valid only while this object lives.
 */
struct PipelineStateStream {
  bool compute = false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};
  GraphicsPipelineExtras graphics_extras;
  Com<ID3D12RootSignature> serialized_root;
};

/** Shared by ID3D12Device2::CreatePipelineState and ID3D12PipelineLibrary1::LoadPipeline. */
HRESULT ParsePipelineStateStream(MTLD3D12Device *device, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                 PipelineStateStream &stream);

HRESULT CreatePipelineLibrary(MTLD3D12Device *device, const void *blob, SIZE_T blob_size, REFIID riid,
                              void **library);

} // namespace dxmt
