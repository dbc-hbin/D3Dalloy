// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12_frame_generation_backend.hpp"
#include "backend/dx12/ffx_recording.h"
#include "framegeneration/fsr3/include/ffx_frameinterpolation.h"
#include "framegeneration/fsr3/include/ffx_opticalflow.h"
#include "framegeneration/fsr3/internal/ffx_frameinterpolation_private.h"
#include "framegeneration/fsr3/internal/ffx_opticalflow_private.h"
#include "owned_publish_dxil.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>
namespace dxmt {
namespace {
HRESULT effectResult(FfxErrorCode value) {
  switch (uint32_t(value)) {
  case FFX_OK: return S_OK;
  case FFX_ERROR_OUT_OF_MEMORY: case FFX_ERROR_INSUFFICIENT_MEMORY: return E_OUTOFMEMORY;
  case FFX_ERROR_INVALID_ARGUMENT: case FFX_ERROR_INVALID_POINTER: case FFX_ERROR_INVALID_SIZE: return E_INVALIDARG;
  default: return E_FAIL;
  }
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (before == after) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}
HRESULT createBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_FLAGS flags, Com<ID3D12Resource> &result) {
  D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1;
  desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
  return device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
      heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&result));
}
HRESULT retainSurface(ID3D12Device *device, ID3D12Resource *source, Com<ID3D12Resource> &result,
    UINT width, UINT height, bool output = false) {
  if (!source) return E_INVALIDARG;
  Com<ID3D12Device> owner;
  auto hr = source->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner));
  if (FAILED(hr) || owner.ptr() != device) return E_INVALIDARG;
  const auto desc = source->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height || desc.Width < width || desc.Height < height ||
      (output && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) return E_INVALIDARG;
  result = source;
  return S_OK;
}
HRESULT createSnapshot(ID3D12Device *device, ID3D12Resource *source, Com<ID3D12Resource> &result) {
  auto desc = source->GetDesc();
  desc.MipLevels = 1;
  desc.Flags = D3D12_RESOURCE_FLAGS(desc.Flags & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&result));
}
void captureSurface(ID3D12GraphicsCommandList *list, ID3D12Resource *source, ID3D12Resource *snapshot,
    D3D12_RESOURCE_STATES sourceState) {
  transition(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{}, to{};
  from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.pResource = snapshot; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
}
DXGI_FORMAT viewFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
  default: return format;
  }
}
}
struct OwnedPhaseRecordingStorage { FfxDX12RecordingResources arena; };
HRESULT OwnedPhaseBatch::GetActualGeneratedCount(UINT &count) const {
  count = 0;
  if (!inputs) return S_OK;
  void *mapped = nullptr; D3D12_RANGE range{8, 12};
  const auto hr = readback_count->Map(0, &range, &mapped);
  if (FAILED(hr)) return hr;
  std::memcpy(&count, static_cast<const char *>(mapped) + 8, sizeof(count));
  D3D12_RANGE noWrites{0, 0}; readback_count->Unmap(0, &noWrites);
  return count <= desc.num_generated_frames ? S_OK : E_FAIL;
}
void OwnedPhaseBatch::ResetRecordedHistory() {
  if (!recording) return;
  for (auto *word : recording->arena.interpolationResets) *word = 1;
  for (auto *word : recording->arena.opticalFlowFrameIndices) *word = 0;
}
OwnedPhasePublication::~OwnedPhasePublication() { if (mapped_control) control->Unmap(0, nullptr); }
void OwnedPhasePublication::SetValid(bool valid) { mapped_control[0] = UINT(valid); }
struct OwnedPhaseBackend::Impl {
  Com<ID3D12Device> device;
  MTL_FRAME_GENERATION_CREATE_DESC description{};
  FfxInterface host{}, shared{};
  std::vector<uint8_t> hostScratch, sharedScratch;
  std::unique_ptr<FfxFrameInterpolationContext> interpolation;
  std::unique_ptr<FfxOpticalflowContext> opticalFlow;
  bool interpolationCreated = false, opticalFlowCreated = false, sharedCreated = false;
  FfxUInt32 sharedId = 0;
  FfxResourceInternal depth{}, motion{}, reconstructed{}, flow{}, sceneChange{};
  Com<ID3D12RootSignature> publishRoot;
  Com<ID3D12PipelineState> publishPipeline;
  UINT recordingBaseDescriptors = 0, recordingPhaseDescriptors = 0;
  UINT recordingBaseConstants = 0, recordingPhaseConstants = 0;
  ~Impl() {
    if (opticalFlowCreated) ffxOpticalflowContextDestroy(opticalFlow.get());
    if (interpolationCreated) ffxFrameInterpolationContextDestroy(interpolation.get());
    if (sharedCreated) {
      for (auto resource : {depth, motion, reconstructed, flow, sceneChange})
        if (resource.internalIndex) shared.fpDestroyResource(&shared, resource, sharedId);
      shared.fpDestroyBackendContext(&shared, sharedId);
    }
  }
};
OwnedPhaseBackend::OwnedPhaseBackend() : impl_(std::make_unique<Impl>()) {}
OwnedPhaseBackend::~OwnedPhaseBackend() = default;
HRESULT CreateOwnedPhaseBackend(ID3D12Device *device, const MTL_FRAME_GENERATION_CREATE_DESC &desc,
    std::shared_ptr<OwnedPhaseBackend> &result) {
  result.reset(); auto backend = std::make_shared<OwnedPhaseBackend>();
  const auto hr = backend->Initialize(device, desc);
  if (SUCCEEDED(hr)) result = std::move(backend);
  return hr;
}
HRESULT OwnedPhaseBackend::Initialize(ID3D12Device *device, const MTL_FRAME_GENERATION_CREATE_DESC &desc) {
  auto &s = *impl_; s.device = device; s.description = desc;
  s.hostScratch.resize(ffxGetScratchMemorySizeDX12(2)); s.sharedScratch.resize(ffxGetScratchMemorySizeDX12(1));
  auto hr = effectResult(ffxGetInterfaceDX12(&s.host, ffxGetDeviceDX12(device), s.hostScratch.data(), s.hostScratch.size(), 2));
  if (FAILED(hr)) return hr;
  hr = effectResult(ffxGetInterfaceDX12(&s.shared, ffxGetDeviceDX12(device), s.sharedScratch.data(), s.sharedScratch.size(), 1));
  if (FAILED(hr)) return hr;
  hr = effectResult(s.shared.fpCreateBackendContext(&s.shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &s.sharedId));
  if (FAILED(hr)) return hr; s.sharedCreated = true;
  s.interpolation = std::make_unique<FfxFrameInterpolationContext>();
  s.opticalFlow = std::make_unique<FfxOpticalflowContext>();
  FfxFrameInterpolationContextDescription fi{}; fi.backendInterface = s.host;
  fi.flags = (desc.flags & 3u) | ((desc.flags & 4u) ? FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0u) |
      ((desc.flags & 8u) ? FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS : 0u);
  fi.maxRenderSize = {desc.max_render_width, desc.max_render_height}; fi.displaySize = {desc.display_width, desc.display_height};
  fi.backBufferFormat = fi.previousInterpolationSourceFormat = FfxApiSurfaceFormat(ffxApiGetSurfaceFormatDX12(desc.backbuffer_format));
  hr = effectResult(ffxFrameInterpolationContextCreate(s.interpolation.get(), &fi));
  if (FAILED(hr)) return hr; s.interpolationCreated = true;
  FfxOpticalflowContextDescription of{}; of.backendInterface = s.host; of.resolution = fi.displaySize;
  hr = effectResult(ffxOpticalflowContextCreate(s.opticalFlow.get(), &of));
  if (FAILED(hr)) return hr; s.opticalFlowCreated = true;
  // Count the pinned graph once per state, using its actual pipeline reflection.
  // Prepare: 1 dispatch. OF: 4 setup/SCD + 7 search + 7 filter + 6 scale.
  // FI: 12 per phase including both debug passes; reverse/splat and jump flood
  // run only in phase zero. Reset/scene/bootstrap skip work, never add passes.
  const auto &fiContext = *reinterpret_cast<FfxFrameInterpolationContext_Private *>(s.interpolation.get());
  const auto &ofContext = *reinterpret_cast<FfxOpticalflowContext_Private *>(s.opticalFlow.get());
  static_assert(sizeof(FrameInterpolationConstants) <= 256 && sizeof(InpaintingPyramidConstants) <= 256 &&
      sizeof(OpticalflowConstants) <= 256); // Every reflected CB occupies one 256-byte slot.
  const auto account = [](const FfxPipelineState &p, UINT repetitions, UINT &descriptors, UINT &constants) {
    descriptors += repetitions * (ffxGetDX12DescriptorTableSize(p.uavTextureBindings, p.uavTextureCount,
        p.uavBufferBindings, p.uavBufferCount) + ffxGetDX12DescriptorTableSize(p.srvTextureBindings,
        p.srvTextureCount, p.srvBufferBindings, p.srvBufferCount));
    constants += repetitions * p.constCount * 256;
  };
  for (const auto *p : {&fiContext.pipelineFiReconstructAndDilate, &ofContext.pipelinePrepareLuma,
      &ofContext.pipelineGenerateOpticalFlowInputPyramid, &ofContext.pipelineGenerateSCDHistogram,
      &ofContext.pipelineComputeSCDDivergence, &fiContext.bidirectionalPipelines[0], &fiContext.bidirectionalPipelines[1]})
    account(*p, 1, s.recordingBaseDescriptors, s.recordingBaseConstants);
  constexpr UINT opticalLevels = FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_6 -
      FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT + 1;
  account(ofContext.pipelineComputeOpticalFlowAdvancedV5, opticalLevels, s.recordingBaseDescriptors, s.recordingBaseConstants);
  account(ofContext.pipelineFilterOpticalFlowV5, opticalLevels, s.recordingBaseDescriptors, s.recordingBaseConstants);
  account(ofContext.pipelineScaleOpticalFlowAdvancedV5, opticalLevels - 1, s.recordingBaseDescriptors, s.recordingBaseConstants);
  account(fiContext.bidirectionalPipelines[2], std::bit_width(std::max(desc.max_render_width, desc.max_render_height) - 1),
      s.recordingBaseDescriptors, s.recordingBaseConstants);
  for (const auto *p : {&fiContext.pipelineFiSetup, &fiContext.pipelineFiReconstructPreviousDepth,
      &fiContext.pipelineFiGameMotionVectorField, &fiContext.bidirectionalPipelines[3],
      &fiContext.pipelineGameVectorFieldInpaintingPyramid, &fiContext.pipelineFiOpticalFlowVectorField,
      &fiContext.pipelineFiDisocclusionMask, &fiContext.pipelineFiScfi, &fiContext.pipelineInpaintingPyramid,
      &fiContext.pipelineInpainting, &fiContext.pipelineGameVectorFieldInpaintingPyramid, &fiContext.pipelineDebugView})
    account(*p, 1, s.recordingPhaseDescriptors, s.recordingPhaseConstants);
  FfxFrameInterpolationSharedResourceDescriptions fiResources{};
  hr = effectResult(ffxFrameInterpolationGetSharedResourceDescriptions(s.interpolation.get(), &fiResources));
  if (FAILED(hr)) return hr;
  for (auto pair : {std::pair{&fiResources.dilatedDepth, &s.depth}, std::pair{&fiResources.dilatedMotionVectors, &s.motion}, std::pair{&fiResources.reconstructedPrevNearestDepth, &s.reconstructed}}) {
    hr = effectResult(s.shared.fpCreateResource(&s.shared, pair.first, s.sharedId, pair.second)); if (FAILED(hr)) return hr;
  }
  FfxOpticalflowSharedResourceDescriptions ofResources{};
  hr = effectResult(ffxOpticalflowGetSharedResourceDescriptions(s.opticalFlow.get(), &ofResources));
  if (FAILED(hr)) return hr;
  for (auto pair : {std::pair{&ofResources.opticalFlowVector, &s.flow}, std::pair{&ofResources.opticalFlowSCD, &s.sceneChange}}) {
    hr = effectResult(s.shared.fpCreateResource(&s.shared, pair.first, s.sharedId, pair.second)); if (FAILED(hr)) return hr;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}; ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2};
  D3D12_ROOT_PARAMETER roots[2]{}; roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  roots[0].DescriptorTable = {2, ranges}; roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  roots[1].Descriptor = {0, 0};
  D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters = 2; signature.pParameters = roots;
  Com<ID3DBlob> serialized, errors;
  hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
  if (FAILED(hr)) return hr;
  hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
      __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&s.publishRoot));
  if (FAILED(hr)) return hr;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = s.publishRoot.ptr();
  pipeline.CS = {owned_publish_dxil, owned_publish_dxil_len};
  return device->CreateComputePipelineState(&pipeline, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&s.publishPipeline));
}
HRESULT OwnedPhaseBackend::CreateInputs(const MTL_FRAME_GENERATION_PREPARE_DESC &desc,
    std::shared_ptr<OwnedPhaseInputs> &result, bool capture) {
  result.reset(); const auto &config = impl_->description;
  if (!desc.render_width || !desc.render_height || desc.render_width > config.max_render_width ||
      desc.render_height > config.max_render_height || desc.flags & ~7u || desc.reset > 1 || desc.camera_info_present > 1 ||
      !std::isfinite(desc.jitter_x) || !std::isfinite(desc.jitter_y) || !std::isfinite(desc.motion_scale_x) ||
      !std::isfinite(desc.motion_scale_y) || !std::isfinite(desc.frame_time_delta_ms) || desc.frame_time_delta_ms < 0 ||
      !std::isfinite(desc.camera_near) || desc.camera_near <= 0 || !std::isfinite(desc.camera_far) || desc.camera_far < 0 ||
      !std::isfinite(desc.camera_fov_vertical_radians) || desc.camera_fov_vertical_radians <= 0 ||
      desc.camera_fov_vertical_radians >= 3.141592654f || !std::isfinite(desc.view_space_to_meters) || desc.view_space_to_meters <= 0)
    return E_INVALIDARG;
  if (desc.camera_info_present) for (const auto *values : {desc.camera_position, desc.camera_up, desc.camera_right, desc.camera_forward})
    for (UINT i = 0; i < 3; ++i) if (!std::isfinite(values[i])) return E_INVALIDARG;
  Com<ID3D12Resource> depth, motion;
  auto hr = retainSurface(impl_->device.ptr(), desc.depth, depth, desc.render_width, desc.render_height);
  if (FAILED(hr)) return hr;
  const bool fullMotion = (config.flags & 4u) != 0;
  hr = retainSurface(impl_->device.ptr(), desc.motion_vectors, motion,
      fullMotion ? config.display_width : desc.render_width, fullMotion ? config.display_height : desc.render_height);
  if (FAILED(hr) || desc.depth == desc.motion_vectors) return E_INVALIDARG;
  if (!capture) return S_OK;
  auto inputs = std::make_shared<OwnedPhaseInputs>(); inputs->desc = desc;
  inputs->source_depth = std::move(depth); inputs->source_motion = std::move(motion);
  hr = createSnapshot(impl_->device.ptr(), desc.depth, inputs->depth); if (FAILED(hr)) return hr;
  hr = createSnapshot(impl_->device.ptr(), desc.motion_vectors, inputs->motion); if (FAILED(hr)) return hr;
  result = std::move(inputs); return S_OK;
}
HRESULT OwnedPhaseBackend::RecordInputs(const std::shared_ptr<OwnedPhaseInputs> &inputs, ID3D12GraphicsCommandList *list) {
  if (!inputs) return S_OK;
  captureSurface(list, inputs->source_depth.ptr(), inputs->depth.ptr(), inputs->desc.depth_state);
  captureSurface(list, inputs->source_motion.ptr(), inputs->motion.ptr(), inputs->desc.motion_vectors_state);
  return S_OK;
}
HRESULT OwnedPhaseBackend::CreateBatch(const MTL_FRAME_GENERATION_GENERATE_DESC &desc,
    const std::shared_ptr<OwnedPhaseInputs> &inputs, std::shared_ptr<OwnedPhaseBatch> &result) {
  result.reset(); const auto &config = impl_->description;
  if (desc.num_generated_frames < 1 || desc.num_generated_frames > 4 || desc.reset > 1 || desc.flags & ~7u ||
      desc.backbuffer_transfer_function > 2 || !std::isfinite(desc.min_luminance) || !std::isfinite(desc.max_luminance) ||
      desc.min_luminance < 0 || desc.max_luminance <= desc.min_luminance || desc.generation_rect_left < 0 ||
      desc.generation_rect_top < 0 || desc.generation_rect_width <= 0 || desc.generation_rect_height <= 0 ||
      UINT64(desc.generation_rect_left) + desc.generation_rect_width > config.display_width ||
      UINT64(desc.generation_rect_top) + desc.generation_rect_height > config.display_height) return E_INVALIDARG;
  auto batch = std::make_shared<OwnedPhaseBatch>(); batch->desc = desc; batch->inputs = inputs;
  auto hr = retainSurface(impl_->device.ptr(), desc.present_color, batch->source_present, config.display_width, config.display_height);
  if (FAILED(hr)) return hr;
  if (desc.present_color->GetDesc().Format != config.backbuffer_format) return E_INVALIDARG;
  if (desc.hudless_color) {
    hr = retainSurface(impl_->device.ptr(), desc.hudless_color, batch->source_hudless, config.display_width, config.display_height);
    if (FAILED(hr) || desc.hudless_color->GetDesc().Format != config.backbuffer_format) return E_INVALIDARG;
  }
  for (UINT i = 0; i < desc.num_generated_frames; ++i) {
    hr = retainSurface(impl_->device.ptr(), desc.outputs[i], batch->outputs[i], config.display_width, config.display_height, true);
    if (FAILED(hr) || desc.outputs[i]->GetDesc().Format != config.backbuffer_format || desc.outputs[i] == desc.present_color ||
        desc.outputs[i] == desc.hudless_color) return E_INVALIDARG;
    for (UINT j = 0; j < i; ++j) if (desc.outputs[j] == desc.outputs[i]) return E_INVALIDARG;
  }
  hr = createBuffer(impl_->device.ptr(), 12, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, batch->gpu_count);
  if (FAILED(hr)) return hr;
  if (inputs) {
    hr = createBuffer(impl_->device.ptr(), 12, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, batch->readback_count);
    if (FAILED(hr)) return hr;
    hr = createSnapshot(impl_->device.ptr(), desc.present_color, batch->present); if (FAILED(hr)) return hr;
    if (desc.hudless_color) { hr = createSnapshot(impl_->device.ptr(), desc.hudless_color, batch->hudless); if (FAILED(hr)) return hr; }
  }
  result = std::move(batch); return S_OK;
}
HRESULT OwnedPhaseBackend::RecordBatch(const std::shared_ptr<OwnedPhaseBatch> &batch, ID3D12GraphicsCommandList *list) {
  if (!batch->inputs) return S_OK;
  auto &s = *impl_; const auto &desc = batch->desc; const auto &input = batch->inputs->desc;
  captureSurface(list, batch->source_present.ptr(), batch->present.ptr(), desc.present_color_state);
  if (batch->hudless) captureSurface(list, batch->source_hudless.ptr(), batch->hudless.ptr(), desc.hudless_color_state);
  ID3D12Resource *snapshots[] = {batch->inputs->depth.ptr(), batch->inputs->motion.ptr(), batch->present.ptr(), batch->hudless.ptr()};
  for (auto *snapshot : snapshots) if (snapshot)
    transition(list, snapshot, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  batch->recording = std::make_shared<OwnedPhaseRecordingStorage>();
  batch->recording->arena.descriptorCount = s.recordingBaseDescriptors + desc.num_generated_frames * s.recordingPhaseDescriptors;
  batch->recording->arena.bytes = s.recordingBaseConstants + desc.num_generated_frames * s.recordingPhaseConstants;
  auto hr = ffxBeginRecordingDX12(&s.host, &batch->recording->arena); if (FAILED(hr)) return hr;
  struct EndRecording { FfxInterface *host; FfxDX12RecordingResources *arena; ~EndRecording() { ffxEndRecordingDX12(host, arena); } } end{&s.host, &batch->recording->arena};
  FfxFrameInterpolationPrepareDescription prepare{};
  prepare.commandList = ffxGetCommandListDX12(list); prepare.renderSize = {input.render_width, input.render_height};
  prepare.jitterOffset = {input.jitter_x, input.jitter_y}; prepare.motionVectorScale = {input.motion_scale_x, input.motion_scale_y};
  prepare.frameTimeDelta = input.frame_time_delta_ms; prepare.cameraNear = input.camera_near; prepare.cameraFar = input.camera_far;
  prepare.cameraFovAngleVertical = input.camera_fov_vertical_radians; prepare.viewSpaceToMetersFactor = input.view_space_to_meters;
  prepare.depth = ffxApiGetResourceDX12(batch->inputs->depth.ptr(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  prepare.motionVectors = ffxApiGetResourceDX12(batch->inputs->motion.ptr(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  prepare.frameID = desc.frame_id;
  prepare.dilatedDepth = s.shared.fpGetResource(&s.shared, s.depth); prepare.dilatedMotionVectors = s.shared.fpGetResource(&s.shared, s.motion);
  prepare.reconstructedPrevDepth = s.shared.fpGetResource(&s.shared, s.reconstructed);
  hr = effectResult(ffxFrameInterpolationPrepare(s.interpolation.get(), &prepare)); if (FAILED(hr)) return hr;
  FfxOpticalflowDispatchDescription optical{}; optical.commandList = prepare.commandList;
  optical.color = ffxApiGetResourceDX12(batch->hudless ? batch->hudless.ptr() : batch->present.ptr(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  optical.opticalFlowVector = s.shared.fpGetResource(&s.shared, s.flow); optical.opticalFlowSCD = s.shared.fpGetResource(&s.shared, s.sceneChange);
  optical.reset = desc.reset || input.reset; optical.backbufferTransferFunction = desc.backbuffer_transfer_function;
  optical.minMaxLuminance = {desc.min_luminance, desc.max_luminance};
  hr = effectResult(ffxOpticalflowContextDispatch(s.opticalFlow.get(), &optical)); if (FAILED(hr)) return hr;
  for (UINT i = 0; i < desc.num_generated_frames; ++i) {
    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = prepare.commandList; dispatch.displaySize = {s.description.display_width, s.description.display_height};
    dispatch.renderSize = prepare.renderSize; dispatch.currentBackBuffer = ffxApiGetResourceDX12(batch->present.ptr(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (batch->hudless) dispatch.currentBackBuffer_HUDLess = ffxApiGetResourceDX12(batch->hudless.ptr(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = ffxApiGetResourceDX12(batch->outputs[i].ptr(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, batch->outputs[i].ptr(), desc.output_states[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.interpolationRect = {desc.generation_rect_left, desc.generation_rect_top, desc.generation_rect_width, desc.generation_rect_height};
    dispatch.opticalFlowVector = optical.opticalFlowVector; dispatch.opticalFlowSceneChangeDetection = optical.opticalFlowSCD;
    dispatch.opticalFlowBufferSize = {(s.description.display_width + 7) / 8, (s.description.display_height + 7) / 8};
    dispatch.opticalFlowScale = {1.f / s.description.display_width, 1.f / s.description.display_height}; dispatch.opticalFlowBlockSize = 8;
    dispatch.cameraNear = input.camera_near; dispatch.cameraFar = input.camera_far; dispatch.cameraFovAngleVertical = input.camera_fov_vertical_radians;
    dispatch.viewSpaceToMetersFactor = input.view_space_to_meters; dispatch.frameTimeDelta = input.frame_time_delta_ms;
    dispatch.reset = desc.reset || input.reset; dispatch.backBufferTransferFunction = FfxApiBackbufferTransferFunction(optical.backbufferTransferFunction);
    dispatch.minMaxLuminance[0] = desc.min_luminance; dispatch.minMaxLuminance[1] = desc.max_luminance; dispatch.frameID = desc.frame_id;
    dispatch.dilatedDepth = prepare.dilatedDepth; dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
    dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth; dispatch.flags = desc.flags | input.flags;
    dispatch.interpolationPhase = FLOAT(i + 1) / FLOAT(desc.num_generated_frames + 1);
    dispatch.phaseIndex = i; dispatch.phaseCount = desc.num_generated_frames;
    hr = effectResult(ffxFrameInterpolationDispatch(s.interpolation.get(), &dispatch)); if (FAILED(hr)) return hr;
    transition(list, batch->outputs[i].ptr(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, desc.output_states[i]);
  }
  auto *privateContext = reinterpret_cast<FfxFrameInterpolationContext_Private *>(s.interpolation.get());
  auto counter = s.host.fpGetResource(&s.host, privateContext->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNTERS]);
  auto *resource = static_cast<ID3D12Resource *>(counter.resource);
  transition(list, resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(batch->gpu_count.ptr(), 0, resource, 0, 12);
  list->CopyBufferRegion(batch->readback_count.ptr(), 0, resource, 0, 12);
  transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  // Each recorded scope has identical entry/exit states even when replay
  // admission suppresses its capture, SDK work and history update entirely.
  for (auto *snapshot : snapshots) if (snapshot)
    transition(list, snapshot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  return S_OK;
}
HRESULT OwnedPhaseBackend::CreatePublication(const std::shared_ptr<OwnedPhaseBatch> &batch,
    const MTL_FRAME_GENERATION_OUTPUT_DESC &desc, std::shared_ptr<OwnedPhasePublication> &result) {
  result.reset(); if (!batch || !desc.output_index || desc.output_index > batch->desc.num_generated_frames) return E_INVALIDARG;
  auto publication = std::make_shared<OwnedPhasePublication>(); publication->desc = desc; publication->batch = batch;
  auto hr = retainSurface(impl_->device.ptr(), desc.output, publication->output,
      impl_->description.display_width, impl_->description.display_height, true);
  if (FAILED(hr) || desc.output->GetDesc().Format != impl_->description.backbuffer_format) return E_INVALIDARG;
  if (desc.output_disable_interpolation) {
    const auto buffer = desc.output_disable_interpolation->GetDesc(); Com<ID3D12Device> owner;
    hr = desc.output_disable_interpolation->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner));
    if (FAILED(hr) || owner.ptr() != impl_->device.ptr() || buffer.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        buffer.Width < 4 || !(buffer.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return E_INVALIDARG;
    publication->disable = desc.output_disable_interpolation;
  }
  hr = createBuffer(impl_->device.ptr(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, publication->control);
  if (FAILED(hr)) return hr; D3D12_RANGE noReads{0, 0};
  hr = publication->control->Map(0, &noReads, reinterpret_cast<void **>(&publication->mapped_control)); if (FAILED(hr)) return hr;
  publication->mapped_control[0] = 0; publication->mapped_control[1] = desc.output_index - 1;
  publication->mapped_control[2] = UINT(desc.output != batch->outputs[desc.output_index - 1].ptr());
  publication->mapped_control[3] = UINT(bool(publication->disable));
  D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 4;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = impl_->device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&publication->descriptors));
  if (FAILED(hr)) return hr;
  auto handle = publication->descriptors->GetCPUDescriptorHandleForHeapStart();
  const auto stride = impl_->device->GetDescriptorHandleIncrementSize(heap.Type);
  D3D12_SHADER_RESOURCE_VIEW_DESC count{}; count.Format = DXGI_FORMAT_R32_TYPELESS; count.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  count.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; count.Buffer.NumElements = 3; count.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  impl_->device->CreateShaderResourceView(batch->gpu_count.ptr(), &count, handle); handle.ptr += stride;
  D3D12_SHADER_RESOURCE_VIEW_DESC color{}; color.Format = viewFormat(batch->outputs[desc.output_index - 1]->GetDesc().Format);
  color.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; color.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; color.Texture2D.MipLevels = 1;
  impl_->device->CreateShaderResourceView(publication->mapped_control[2] ? batch->outputs[desc.output_index - 1].ptr() : nullptr, &color, handle); handle.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC output{}; output.Format = viewFormat(desc.output->GetDesc().Format); output.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  impl_->device->CreateUnorderedAccessView(desc.output, nullptr, &output, handle); handle.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC flag{}; flag.Format = DXGI_FORMAT_R32_TYPELESS; flag.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  flag.Buffer.NumElements = 1; flag.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  impl_->device->CreateUnorderedAccessView(publication->disable.ptr(), nullptr, &flag, handle);
  result = std::move(publication); return S_OK;
}
HRESULT OwnedPhaseBackend::RecordPublication(const std::shared_ptr<OwnedPhasePublication> &publication, ID3D12GraphicsCommandList *list) {
  const auto &desc = publication->desc; auto *source = publication->batch->outputs[desc.output_index - 1].ptr();
  if (!publication->mapped_control[2] && !publication->disable) return S_OK;
  transition(list, publication->batch->gpu_count.ptr(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (publication->mapped_control[2]) transition(list, source, publication->batch->desc.output_states[desc.output_index - 1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list, publication->output.ptr(), desc.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (publication->disable) transition(list, publication->disable.ptr(), desc.output_disable_interpolation_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetPipelineState(impl_->publishPipeline.ptr()); list->SetComputeRootSignature(impl_->publishRoot.ptr());
  auto *heap = publication->descriptors.ptr(); list->SetDescriptorHeaps(1, &heap);
  list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootConstantBufferView(1, publication->control->GetGPUVirtualAddress());
  list->Dispatch((impl_->description.display_width + 7) / 8, (impl_->description.display_height + 7) / 8, 1);
  if (publication->disable) transition(list, publication->disable.ptr(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, desc.output_disable_interpolation_state);
  transition(list, publication->output.ptr(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, desc.output_state);
  if (publication->mapped_control[2]) transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, publication->batch->desc.output_states[desc.output_index - 1]);
  transition(list, publication->batch->gpu_count.ptr(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  return S_OK;
}
} // namespace dxmt
