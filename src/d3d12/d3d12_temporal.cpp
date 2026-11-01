// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned replayable D3D12 transport. All SR kernels/scalers execute in WineMetal.
#include "d3d12_temporal.hpp"
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "Metal4FX.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace d3dalloy {
namespace {
constexpr UINT kLowResMV = 2, kAutoExposure = 64;
constexpr UINT kSupportedFlags = 1 | 2 | 4 | 8 | 64 | 128 | 256;
struct Core {
  Com<MTLD3D12Device> device;
  const uint64_t id = owned_graphics::NewObjectId();
  MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC create{};
  WMT::Reference<WMT4::TemporalFeature> feature;
  WMT::Reference<WMT::SharedEvent> timeline;
  std::mutex mutex;
  uint64_t sequence = 0, last_executed_generation = 0;
  uint64_t last_queue = 0; // owned id of the queue that encoded `sequence`
};
constexpr GUID kInternalStateId = {0xc5b7eebc, 0xa4fc, 0x45b2, {0x9b, 0x1c, 0xd9, 0xb1, 0x31, 0xf1, 0x37, 0x92}};
struct IOwnedTemporalStateInternal : IUnknown {
  virtual HRESULT Prepare(MTLD3D12Device *, const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC &,
                          std::shared_ptr<OwnedTemporalFrame> &) = 0;
};
WMTPixelFormat motionFormat(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatRG16Float: case WMTPixelFormatRG16Uint: case WMTPixelFormatRG16Sint:
  case WMTPixelFormatRG16Unorm: case WMTPixelFormatRG16Snorm: return WMTPixelFormatRG16Float;
  case WMTPixelFormatRG32Float: case WMTPixelFormatRG32Uint: case WMTPixelFormatRG32Sint:
    return WMTPixelFormatRG32Float;
  default: return WMTPixelFormatInvalid;
  }
}
HRESULT retainTexture(MTLD3D12Device *device, ID3D12Resource *input,
                      Com<MTLD3D12Resource, false> &resource, TextureViewRef &view,
                      bool motion = false) {
  if (!input) return E_INVALIDARG;
  Com<ID3D12Device> owner;
  if (FAILED(input->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
      owner.ptr() != static_cast<ID3D12Device *>(device)) return E_INVALIDARG;
  const auto desc = input->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height) return E_INVALIDARG;
  auto owned = static_cast<MTLD3D12Resource *>(input);
  if (!owned->texture || !owned->texture->current()) return E_INVALIDARG;
  resource = owned;
  auto texture = owned->texture;
  const auto format = motion ? motionFormat(texture->pixelFormat()) : texture->pixelFormat();
  if (motion && format == WMTPixelFormatInvalid) return DXGI_ERROR_UNSUPPORTED;
  const auto key = texture->createView({.format = format,
                                      .type = WMTTextureType2D, .miplevelCount = 1, .arraySize = 1});
  view = texture->view(key);
  return view.texture() ? S_OK : E_FAIL;
}
} // namespace
struct OwnedTemporalFrame {
  std::shared_ptr<Core> core;
  const uint64_t id = owned_graphics::NewObjectId();
  uint64_t generation = 0;
  std::array<Com<MTLD3D12Resource, false>, 12> resources;
  std::array<TextureViewRef, 12> views;
  WMT::Reference<WMT4::TemporalFrame> native;
  WMTMetal4FXTemporalFrame props{};
  ~OwnedTemporalFrame() {
    owned_graphics::Trace("temporal_frame_destroy", ",\"feature\":", core ? core->id : 0,
                         ",\"evaluation\":", id, ",\"generation\":", generation);
  }
};
namespace {
class OwnedTemporalState final : public ComObject<IMTLD3D12TemporalUpscaleState, IOwnedTemporalStateInternal> {
  std::shared_ptr<Core> core_;
public:
  explicit OwnedTemporalState(std::shared_ptr<Core> core) : core_(std::move(core)) {}
  ~OwnedTemporalState() { core_->feature.markDormant(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) final {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMTLD3D12TemporalUpscaleState))
      *object = static_cast<IMTLD3D12TemporalUpscaleState *>(this);
    else if (iid == kInternalStateId) *object = static_cast<IOwnedTemporalStateInternal *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  HRESULT Prepare(MTLD3D12Device *device, const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC &desc,
                  std::shared_ptr<OwnedTemporalFrame> &result) final {
    if (device != core_->device.ptr() || !desc.InputContentWidth || !desc.InputContentHeight) return E_INVALIDARG;
    for (float scalar : {desc.JitterOffsetX, desc.JitterOffsetY, desc.MotionVectorScaleX,
                         desc.MotionVectorScaleY, desc.PreExposure, desc.Sharpness})
      if (!std::isfinite(scalar)) return E_INVALIDARG;
    if (!desc.MotionVectorScaleX || !desc.MotionVectorScaleY || desc.PreExposure <= 0)
      return E_INVALIDARG;
    if ((!desc.BiasCurrentColorMask && (desc.BiasBaseX || desc.BiasBaseY)) ||
        (!desc.TransparencyMask && (desc.CompositionBaseX || desc.CompositionBaseY))) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedTemporalFrame>(); frame->core = core_;
    const auto &create = core_->create;
    std::array<ID3D12Resource *, 12> inputs{desc.Color, desc.Depth, desc.MotionVector, desc.Output,
        (create.FeatureFlags & kAutoExposure) ? nullptr : desc.ExposureTexture,
        desc.BiasCurrentColorMask, desc.TransparencyMask, desc.DiffuseAlbedo, desc.SpecularAlbedo,
        desc.Normals, desc.Roughness, desc.SpecularHitDistance};
    const bool rr = (create.FeatureFlags & 256u) != 0;
    if (rr) {
      if (!desc.DiffuseAlbedo || !desc.SpecularAlbedo || !desc.Normals ||
          (!desc.RoughnessPacked && !desc.Roughness) || desc.RoughnessPacked > 1 || desc.DepthLinear > 1) return E_INVALIDARG;
      for (float value : desc.WorldToViewMatrix) if (!std::isfinite(value)) return E_INVALIDARG;
      for (float value : desc.ViewToClipMatrix) if (!std::isfinite(value)) return E_INVALIDARG;
    } else if (desc.DiffuseAlbedo || desc.SpecularAlbedo || desc.Normals || desc.Roughness ||
               desc.SpecularHitDistance || desc.RoughnessPacked || desc.DepthLinear) return E_INVALIDARG;
    HRESULT hr;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (!inputs[i] && i >= 4) continue;
      if (i != 3 && inputs[i] == desc.Output) return E_INVALIDARG;
      if (FAILED(hr = retainTexture(device, inputs[i], frame->resources[i], frame->views[i], i == 2))) return hr;
    }
    auto &p = frame->props;
    p.color = frame->views[0].texture().handle; p.depth = frame->views[1].texture().handle;
    p.motion = frame->views[2].texture().handle; p.output = frame->views[3].texture().handle;
    if (inputs[4]) p.exposure = frame->views[4].texture().handle;
    if (inputs[5]) p.reactive = frame->views[5].texture().handle;
    if (inputs[6]) p.composition = frame->views[6].texture().handle;
    if (inputs[7]) p.diffuse_albedo = frame->views[7].texture().handle;
    if (inputs[8]) p.specular_albedo = frame->views[8].texture().handle;
    if (inputs[9]) p.normals = frame->views[9].texture().handle;
    if (inputs[10]) p.roughness = frame->views[10].texture().handle;
    if (inputs[11]) p.specular_hit_distance = frame->views[11].texture().handle;
    std::memcpy(p.world_to_view, desc.WorldToViewMatrix, sizeof(p.world_to_view));
    std::memcpy(p.view_to_clip, desc.ViewToClipMatrix, sizeof(p.view_to_clip));
    p.roughness_packed = desc.RoughnessPacked; p.depth_linear = desc.DepthLinear;
    p.diffuse_albedo_rect = {desc.DiffuseAlbedoBaseX, desc.DiffuseAlbedoBaseY, desc.InputContentWidth, desc.InputContentHeight};
    p.specular_albedo_rect = {desc.SpecularAlbedoBaseX, desc.SpecularAlbedoBaseY, desc.InputContentWidth, desc.InputContentHeight};
    p.normals_rect = {desc.NormalsBaseX, desc.NormalsBaseY, desc.InputContentWidth, desc.InputContentHeight};
    p.roughness_rect = {desc.RoughnessBaseX, desc.RoughnessBaseY, desc.InputContentWidth, desc.InputContentHeight};
    p.specular_hit_distance_rect = {desc.SpecularHitDistanceBaseX, desc.SpecularHitDistanceBaseY, desc.InputContentWidth, desc.InputContentHeight};
    p.input_width = desc.InputContentWidth; p.input_height = desc.InputContentHeight;
    const UINT ow = desc.OutputWidth ? desc.OutputWidth : create.OutputWidth;
    const UINT oh = desc.OutputHeight ? desc.OutputHeight : create.OutputHeight;
    const UINT mw = desc.MotionContentWidth ? desc.MotionContentWidth : ((create.FeatureFlags & kLowResMV) ? p.input_width : ow);
    const UINT mh = desc.MotionContentHeight ? desc.MotionContentHeight : ((create.FeatureFlags & kLowResMV) ? p.input_height : oh);
    p.color_rect = {desc.ColorBaseX, desc.ColorBaseY, p.input_width, p.input_height};
    p.depth_rect = {desc.DepthBaseX, desc.DepthBaseY, p.input_width, p.input_height};
    p.motion_rect = {desc.MotionBaseX, desc.MotionBaseY, mw, mh};
    p.reactive_rect = {desc.BiasBaseX, desc.BiasBaseY, p.input_width, p.input_height};
    p.composition_rect = {desc.CompositionBaseX, desc.CompositionBaseY, p.input_width, p.input_height};
    p.output_rect = {desc.OutputBaseX, desc.OutputBaseY, ow, oh};
    p.jitter_x = desc.JitterOffsetX; p.jitter_y = desc.JitterOffsetY;
    p.motion_scale_x = desc.MotionVectorScaleX; p.motion_scale_y = desc.MotionVectorScaleY;
    p.pre_exposure = desc.PreExposure; p.reset = desc.ResetHistory;
    p.exposure_mode = (create.FeatureFlags & kAutoExposure) ? 2 : (p.exposure ? 1 : 0);
    p.color_transfer = desc.ColorTransfer; p.combine_composition = desc.CombineCompositionMask;
    p.sharpening = desc.EnableSharpening; p.sharpness = desc.Sharpness; p.cap_output = desc.CapOutputToTemporalMaxScale;
    p.debug_view = desc.DrawDebugView;
    int32_t native_hr;
    std::lock_guard lock(core_->mutex);
    // Dynamic output stays on one native feature: its bounded scaler cache
    // keys generations by output extent and resets history on a switch.
    frame->native = core_->feature.prepare(p, native_hr, frame->generation);
    if (native_hr < 0) return HRESULT(native_hr);
    if (!frame->native) return E_FAIL;
    owned_graphics::Trace("temporal_record", ",\"feature\":", core_->id, ",\"evaluation\":", frame->id,
        ",\"generation\":", frame->generation, ",\"color\":", frame->resources[0]->owned_id,
        ",\"depth\":", frame->resources[1]->owned_id, ",\"motion\":", frame->resources[2]->owned_id,
        ",\"output\":", frame->resources[3]->owned_id, ",\"output_x\":", desc.OutputBaseX, ",\"output_y\":", desc.OutputBaseY);
    result = std::move(frame); return S_OK;
  }
};
} // namespace
HRESULT CreateOwnedTemporalState(MTLD3D12Device *device,
    const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc, IMTLD3D12TemporalUpscaleState **state) {
  if (!state) return E_POINTER;
  *state = nullptr;
  if (!desc || desc->StructSize != sizeof(*desc) || !desc->InputWidth || !desc->InputHeight ||
      !desc->OutputWidth || !desc->OutputHeight || desc->InputWidth > 16384 || desc->InputHeight > 16384 ||
      desc->OutputWidth > 16384 || desc->OutputHeight > 16384) return E_INVALIDARG;
  if (desc->CreationNodeMask > 1 || desc->VisibilityNodeMask > 1 || (desc->FeatureFlags & ~kSupportedFlags)) return E_NOTIMPL;
  if (!WMT4::TemporalFeature::Supports(device->GetMTLDevice(), desc->FeatureFlags & 256u)) return DXGI_ERROR_UNSUPPORTED;
  try {
    auto core = std::make_shared<Core>(); core->device = device; core->create = *desc;
    core->timeline = device->GetMTLDevice().newSharedEvent();
    if (!core->timeline) return E_OUTOFMEMORY;
    const WMTMetal4FXTemporalInfo p{desc->InputWidth, desc->InputHeight, desc->OutputWidth, desc->OutputHeight,
        desc->FeatureFlags, uint32_t(desc->OutputSubrectsEnabled), uint32_t(desc->DynamicOutputEnabled)};
    int32_t hr;
    core->feature = WMT4::TemporalFeature::Create(device->GetMTLDevice(), device->GetMetal4Compiler(), p, hr);
    if (hr < 0) return HRESULT(hr);
    if (!core->feature) return E_FAIL;
    Com<OwnedTemporalState> implementation = new OwnedTemporalState(std::move(core));
    return implementation->QueryInterface(__uuidof(IMTLD3D12TemporalUpscaleState), reinterpret_cast<void **>(state));
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT PrepareOwnedTemporalFrame(MTLD3D12Device *device, IMTLD3D12TemporalUpscaleState *state,
    const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc, std::shared_ptr<OwnedTemporalFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedTemporalStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Prepare(device, *desc, frame); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
std::array<MTLD3D12Resource *, 12> GetOwnedTemporalFrameResources(const std::shared_ptr<OwnedTemporalFrame> &frame) {
  return {frame->resources[0].ptr(), frame->resources[1].ptr(), frame->resources[2].ptr(), frame->resources[3].ptr(),
          frame->resources[4].ptr(), frame->resources[5].ptr(), frame->resources[6].ptr(),
          frame->resources[7].ptr(), frame->resources[8].ptr(), frame->resources[9].ptr(),
          frame->resources[10].ptr(), frame->resources[11].ptr()};
}
uint64_t OwnedTemporalFrameHistoryQueue(const std::shared_ptr<OwnedTemporalFrame> &frame) {
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  return core.sequence ? core.last_queue : 0;
}
void EncodeOwnedTemporalFrame(const std::shared_ptr<OwnedTemporalFrame> &frame,
    WMT::MTL4CommandBuffer cmdbuf, WMT::Fence fence, uint64_t queue, uint64_t submission, uint64_t encoder,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event) {
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  if (core.sequence && core.last_queue != queue) wait_event(core.timeline, core.sequence);
  const auto sequence = ++core.sequence;
  core.last_queue = queue;
  const bool reset = frame->props.reset || core.last_executed_generation != frame->generation;
  owned_graphics::Trace("temporal_encode", ",\"queue\":", queue, ",\"submission\":", submission,
      ",\"encoder\":", encoder, ",\"feature\":", core.id, ",\"evaluation\":", frame->id,
      ",\"generation\":", frame->generation, ",\"history_sequence\":", sequence,
      ",\"caller_reset\":", frame->props.reset, ",\"reset\":", unsigned(reset),
      ",\"color\":", frame->resources[0]->owned_id, ",\"output\":", frame->resources[3]->owned_id);
  const auto hr = frame->native.encode(cmdbuf, fence, reset);
  if (hr < 0) core.device->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
  // Partial commands/leases remain attached. Always commit then signal: no stranded timeline.
  signal_event(core.timeline, sequence);
  core.last_executed_generation = hr < 0 ? 0 : frame->generation;
}
} // namespace d3dalloy
