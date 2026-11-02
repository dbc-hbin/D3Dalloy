// SPDX-License-Identifier: LGPL-2.1-or-later
// Replayable D3D12 translation of AMD Ray Regeneration to the shared native MetalFX RR provider.
#include "d3d12_ray_regeneration.hpp"
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "winemetal_metal4_rr.h"
#include <cstring>
#include <mutex>
#include <new>

namespace d3dalloy {
namespace {
HRESULT nativeResult(uint64_t result) {
  switch (result) {
  case WMTRR_OK: return S_OK;
  case WMTRR_NO_PROVIDER: return DXGI_ERROR_UNSUPPORTED;
  case WMTRR_MEMORY: return E_OUTOFMEMORY;
  case WMTRR_PARAMETER: return E_INVALIDARG;
  default: return E_FAIL;
  }
}
static_assert(MTL_RAY_REGENERATION_RESOURCE_COUNT == WMTRR_RESOURCE_COUNT);
static_assert(sizeof(MTL_RAY_REGENERATION_SETTINGS) == sizeof(WMTRRSettings));
struct Core {
  Com<MTLD3D12Device> device;
  WMT::Reference<WMT::Object> feature;
  WMT::Reference<WMT::SharedEvent> timeline;
  std::mutex mutex;
  std::mutex submission_mutex;
  uint64_t sequence = 0;
  uint64_t last_queue = 0; // owned id of the queue that encoded `sequence`
  ~Core() {
    if (feature) WMT::Metal4Invoke(WMTMetal4RRDormant, feature.handle);
  }
};
constexpr GUID kInternalStateId = {0x6f0d6f42, 0x1c8e, 0x4b2f, {0x9d, 0x3e, 0x2a, 0x7b, 0x5c, 0x1e, 0x9a, 0x3f}};
struct IOwnedRayRegenerationStateInternal : IUnknown {
  virtual HRESULT Prepare(MTLD3D12Device *, const MTL_RAY_REGENERATION_FRAME_DESC &,
      std::shared_ptr<OwnedRayRegenerationFrame> &) = 0;
};
HRESULT retainTexture(MTLD3D12Device *device, ID3D12Resource *input, bool output,
    Com<MTLD3D12Resource, false> &resource, TextureViewRef &view) {
  Com<ID3D12Device> owner;
  if (FAILED(input->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
      owner.ptr() != static_cast<ID3D12Device *>(device)) return E_INVALIDARG;
  const auto desc = input->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height ||
      (output && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) return E_INVALIDARG;
  auto owned = static_cast<MTLD3D12Resource *>(input);
  if (!owned->texture || !owned->texture->current()) return E_INVALIDARG;
  auto texture = owned->texture;
  const auto key = texture->createView({.format = texture->pixelFormat(),
      .type = WMTTextureType2D, .miplevelCount = 1, .arraySize = 1});
  resource = owned;
  view = texture->view(key);
  return view.texture() ? S_OK : E_FAIL;
}
} // namespace
struct OwnedRayRegenerationFrame {
  std::shared_ptr<Core> core;
  std::array<Com<MTLD3D12Resource, false>, MTL_RAY_REGENERATION_RESOURCE_COUNT> resources;
  std::array<TextureViewRef, MTL_RAY_REGENERATION_RESOURCE_COUNT> views;
  std::array<D3D12_RESOURCE_STATES, MTL_RAY_REGENERATION_RESOURCE_COUNT> states{};
  WMT::Reference<WMT::Object> native;
};
bool IsOwnedRayRegenerationOutput(size_t index) {
  return index == MTL_RAY_REGENERATION_RESOURCE_COUNT - 1 ||
      (index >= 5 && index < 5 + 2 * MTL_RAY_REGENERATION_SIGNAL_COUNT && (index - 5) % 2 == 1);
}
namespace {
class OwnedRayRegenerationState final
    : public ComObject<IMTLD3D12RayRegenerationState, IOwnedRayRegenerationStateInternal> {
  std::shared_ptr<Core> core_;
public:
  explicit OwnedRayRegenerationState(std::shared_ptr<Core> core) : core_(std::move(core)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) final {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMTLD3D12RayRegenerationState))
      *object = static_cast<IMTLD3D12RayRegenerationState *>(this);
    else if (iid == kInternalStateId) *object = static_cast<IOwnedRayRegenerationStateInternal *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMemoryUsage(UINT64 *bytes) final {
    if (!bytes) return E_POINTER;
    *bytes = WMT::Metal4Invoke(WMTMetal4RRMemory, core_->feature.handle).ret[0];
    return S_OK;
  }
  HRESULT Prepare(MTLD3D12Device *device, const MTL_RAY_REGENERATION_FRAME_DESC &desc,
      std::shared_ptr<OwnedRayRegenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedRayRegenerationFrame>(); frame->core = core_;
    WMTRRFrame p{};
    for (size_t i = 0; i < MTL_RAY_REGENERATION_RESOURCE_COUNT; ++i) {
      auto input = desc.Resources[i];
      if (!input) continue;
      const bool output = IsOwnedRayRegenerationOutput(i);
      // Distinct AMD outputs cannot alias; an in-place output may alias its own input.
      if (output) for (size_t j = 0; j < i; ++j)
        if (IsOwnedRayRegenerationOutput(j) && desc.Resources[j] == input) return E_INVALIDARG;
      if (auto hr = retainTexture(device, input, output, frame->resources[i], frame->views[i]); FAILED(hr)) return hr;
      frame->states[i] = desc.States[i];
      p.textures[i] = frame->views[i].texture().handle;
    }
    std::memcpy(&p.settings, &desc.Settings, sizeof(p.settings));
    std::memcpy(p.motion_scale, desc.MotionScale, sizeof(p.motion_scale));
    std::memcpy(p.jitter, desc.Jitter, sizeof(p.jitter));
    std::memcpy(p.camera_delta, desc.CameraDelta, sizeof(p.camera_delta));
    std::memcpy(p.view, desc.View, sizeof(p.view));
    std::memcpy(p.projection, desc.Projection, sizeof(p.projection));
    p.depth_min = desc.DepthMin; p.depth_max = desc.DepthMax;
    p.width = desc.Width; p.height = desc.Height; p.frame_index = desc.FrameIndex; p.flags = desc.Flags;
    std::memcpy(p.checkerboard_origin, desc.CheckerboardOrigin, sizeof(p.checkerboard_origin));
    std::memcpy(p.light_direction, desc.LightDirection, sizeof(p.light_direction));
    std::memcpy(p.light_emission, desc.LightEmission, sizeof(p.light_emission));
    p.light_angular_radius = desc.LightAngularRadius;
    p.debug_width = desc.DebugWidth; p.debug_height = desc.DebugHeight;
    p.debug_mode = desc.DebugMode; p.debug_viewport = desc.DebugViewport;
    p.legacy_mode = desc.LegacyMode;
    const auto call = WMT::Metal4Invoke(WMTMetal4RRPrepare, core_->feature.handle, WMT::Metal4CPUPointer(&p));
    frame->native = WMT::Reference<WMT::Object>(call.ret[0]);
    if (call.ret[1]) return nativeResult(call.ret[1]);
    if (!frame->native) return E_FAIL;
    result = std::move(frame); return S_OK;
  }
};
} // namespace
HRESULT CreateOwnedRayRegenerationState(MTLD3D12Device *device, const MTL_RAY_REGENERATION_CREATE_DESC *desc,
    IMTLD3D12RayRegenerationState **state) {
  if (!state) return E_POINTER;
  *state = nullptr;
  if (!device || !desc || desc->StructSize != sizeof(*desc) || !desc->Width || !desc->Height ||
      desc->Width > 16384 || desc->Height > 16384) return E_INVALIDARG;
  try {
    auto core = std::make_shared<Core>(); core->device = device;
    core->timeline = device->GetMTLDevice().newSharedEvent();
    if (!core->timeline) return E_OUTOFMEMORY;
    const WMTRRCreateInfo info{desc->Version, desc->Width, desc->Height, desc->Signals,
        desc->CheckerboardSignals, desc->Flags};
    const auto call = WMT::Metal4Invoke(WMTMetal4RRCreate, device->GetMTLDevice().handle,
        device->GetMetal4Compiler().handle, WMT::Metal4CPUPointer(&info));
    core->feature = WMT::Reference<WMT::Object>(call.ret[0]);
    if (call.ret[1]) return nativeResult(call.ret[1]);
    if (!core->feature) return E_FAIL;
    Com<OwnedRayRegenerationState> implementation = new OwnedRayRegenerationState(std::move(core));
    return implementation->QueryInterface(__uuidof(IMTLD3D12RayRegenerationState), reinterpret_cast<void **>(state));
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT PrepareOwnedRayRegenerationFrame(MTLD3D12Device *device, IMTLD3D12RayRegenerationState *state,
    const MTL_RAY_REGENERATION_FRAME_DESC *desc, std::shared_ptr<OwnedRayRegenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedRayRegenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Prepare(device, *desc, frame); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
std::array<MTLD3D12Resource *, MTL_RAY_REGENERATION_RESOURCE_COUNT>
GetOwnedRayRegenerationResources(const std::shared_ptr<OwnedRayRegenerationFrame> &frame) {
  std::array<MTLD3D12Resource *, MTL_RAY_REGENERATION_RESOURCE_COUNT> result{};
  for (size_t i = 0; i < result.size(); ++i) result[i] = frame->resources[i].ptr();
  return result;
}
std::array<D3D12_RESOURCE_STATES, MTL_RAY_REGENERATION_RESOURCE_COUNT>
GetOwnedRayRegenerationStates(const std::shared_ptr<OwnedRayRegenerationFrame> &frame) {
  return frame->states;
}
std::mutex &OwnedRayRegenerationSubmissionMutex(const std::shared_ptr<OwnedRayRegenerationFrame> &frame) {
  return frame->core->submission_mutex;
}
uint64_t OwnedRayRegenerationFrameHistoryQueue(const std::shared_ptr<OwnedRayRegenerationFrame> &frame) {
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  return core.sequence ? core.last_queue : 0;
}
void EncodeOwnedRayRegenerationFrame(const std::shared_ptr<OwnedRayRegenerationFrame> &frame,
    WMT::MTL4CommandBuffer cmdbuf, WMT::Fence fence, uint64_t queue,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event) {
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  // Native history is per context: order executions across queues.
  if (core.sequence && core.last_queue != queue) wait_event(core.timeline, core.sequence);
  const auto sequence = ++core.sequence;
  core.last_queue = queue;
  const auto call = WMT::Metal4Invoke(WMTMetal4RREncode, cmdbuf.handle, frame->native.handle, fence.handle);
  if (call.ret[0]) core.device->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
  // Commit even partial native work before signaling, so another queue cannot strand.
  signal_event(core.timeline, sequence);
}
} // namespace d3dalloy
