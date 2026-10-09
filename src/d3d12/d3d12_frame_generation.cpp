// SPDX-License-Identifier: LGPL-2.1-or-later
// Replayable D3D12 translation to the shared native MetalFX frame interpolator.
#include "d3d12_frame_generation.hpp"
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "winemetal_metal4_fg.h"
#include <cstring>
#include <mutex>
#include <new>

namespace dxmt {
namespace {
HRESULT nativeResult(uint64_t result) {
  switch (result) {
  case 0: return S_OK;
  case 4: return DXGI_ERROR_UNSUPPORTED;
  case 5: return E_OUTOFMEMORY;
  case 6: return E_INVALIDARG;
  default: return E_FAIL;
  }
}
HRESULT request(WMTFGpacket_header &packet, WMT::Reference<WMT::Object> *recorded = nullptr) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4FGRequest;
  call.args[0] = packet.operation;
  call.args[1] = WMT::Metal4CPUPointer(&packet);
  if (WMTMetal4Call(&call)) return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
  if (recorded) *recorded = WMT::Reference<WMT::Object>(call.ret[1]);
  return nativeResult(call.ret[0]);
}
struct Core {
  Com<MTLD3D12Device> device;
  uint64_t context = 0, sequence = 0;
  WMT::Reference<WMT::SharedEvent> timeline;
  std::mutex mutex;
  ~Core() {
    if (context) {
      WMTFGdestroy_packet p{{sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_DESTROY, 0, context}};
      request(p.header);
    }
  }
};
constexpr GUID kInternalStateId = {0xb2a31090,0xa1f2,0x4717,{0xbb,0x50,0x61,0x7a,0x47,0xf2,0xa0,0x13}};
struct IOwnedFrameGenerationStateInternal : IUnknown {
  virtual HRESULT Prepare(MTLD3D12Device *, const MTL_FRAME_GENERATION_PREPARE_DESC &,
      std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
  virtual HRESULT Generate(MTLD3D12Device *, const MTL_FRAME_GENERATION_GENERATE_DESC &,
      std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
};
HRESULT retainTexture(MTLD3D12Device *device, ID3D12Resource *input,
    Com<MTLD3D12Resource, false> &resource, TextureViewRef &view, bool motion, bool output) {
  if (!input) return E_INVALIDARG;
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
  auto format = texture->pixelFormat();
  if (motion) {
    switch (format) {
    case WMTPixelFormatRG16Float: case WMTPixelFormatRG16Uint: case WMTPixelFormatRG16Sint:
    case WMTPixelFormatRG16Unorm: case WMTPixelFormatRG16Snorm: format = WMTPixelFormatRG16Float; break;
    case WMTPixelFormatRG32Float: case WMTPixelFormatRG32Uint: case WMTPixelFormatRG32Sint:
      format = WMTPixelFormatRG32Float; break;
    default: return DXGI_ERROR_UNSUPPORTED;
    }
  }
  const auto key = texture->createView({.format = format,
      .type = WMTTextureType2D, .miplevelCount = 1, .arraySize = 1});
  resource = owned;
  view = texture->view(key);
  return view.texture() ? S_OK : E_FAIL;
}
}
struct OwnedFrameGenerationFrame {
  std::shared_ptr<Core> core;
  std::array<Com<MTLD3D12Resource, false>, 3> resources;
  std::array<TextureViewRef, 3> views;
  std::array<D3D12_RESOURCE_STATES, 3> states{};
  WMT::Reference<WMT::Object> native;
  bool generate = false;
};
namespace {
class OwnedFrameGenerationState final : public ComObject<IMTLD3D12FrameGenerationState, IOwnedFrameGenerationStateInternal> {
  std::shared_ptr<Core> core_;
public:
  explicit OwnedFrameGenerationState(std::shared_ptr<Core> core) : core_(std::move(core)) {}
  ~OwnedFrameGenerationState() { Configure(FALSE); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) final {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMTLD3D12FrameGenerationState))
      *object = static_cast<IMTLD3D12FrameGenerationState *>(this);
    else if (iid == kInternalStateId) *object = static_cast<IOwnedFrameGenerationStateInternal *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Configure(BOOL enabled) final {
    if (enabled != FALSE && enabled != TRUE) return E_INVALIDARG;
    std::lock_guard lock(core_->mutex);
    WMTFGconfigure_packet p{{sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_CONFIGURE, 0, core_->context}, UINT(enabled)};
    return request(p.header);
  }
  HRESULT STDMETHODCALLTYPE RetireFrame(UINT64 frame_id, UINT64 *oldest, BOOL *pending) final {
    if (!oldest || !pending) return E_POINTER;
    *oldest = 0; *pending = FALSE;
    std::lock_guard lock(core_->mutex);
    WMTFGretire_frame_packet p{{sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_RETIRE_FRAME, 0, core_->context}, frame_id, 0, 0};
    const auto hr = request(p.header);
    if (SUCCEEDED(hr)) { *oldest = p.oldest_pending_frame_id; *pending = p.has_pending_frame; }
    return hr;
  }
  HRESULT Prepare(MTLD3D12Device *device, const MTL_FRAME_GENERATION_PREPARE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>(); frame->core = core_;
    HRESULT hr;
    if (FAILED(hr = retainTexture(device, desc.depth, frame->resources[0], frame->views[0], false, false)) ||
        FAILED(hr = retainTexture(device, desc.motion_vectors, frame->resources[1], frame->views[1], true, false))) return hr;
    frame->states = {desc.depth_state, desc.motion_vectors_state, D3D12_RESOURCE_STATE_COMMON};
    WMTFGNativeCommand command{device->GetMTLDevice().handle, device->GetMetal4Compiler().handle};
    WMTFGprepare_packet p{};
    p.header = {sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_PREPARE, 0, core_->context};
    p.command_list = WMT::Metal4CPUPointer(&command); p.frame_id = desc.frame_id;
    p.depth = frame->views[0].texture().handle; p.motion_vectors = frame->views[1].texture().handle;
    p.depth_state = desc.depth_state; p.motion_vectors_state = desc.motion_vectors_state;
    p.render_width = desc.render_width; p.render_height = desc.render_height; p.flags = desc.flags; p.reset = desc.reset;
    p.jitter_x = desc.jitter_x; p.jitter_y = desc.jitter_y;
    p.motion_scale_x = desc.motion_scale_x; p.motion_scale_y = desc.motion_scale_y;
    p.frame_time_delta_ms = desc.frame_time_delta_ms; p.camera_near = desc.camera_near; p.camera_far = desc.camera_far;
    p.camera_fov_vertical_radians = desc.camera_fov_vertical_radians; p.view_space_to_meters = desc.view_space_to_meters;
    std::memcpy(p.camera_position, desc.camera_position, sizeof(p.camera_position));
    std::memcpy(p.camera_up, desc.camera_up, sizeof(p.camera_up));
    std::memcpy(p.camera_right, desc.camera_right, sizeof(p.camera_right));
    std::memcpy(p.camera_forward, desc.camera_forward, sizeof(p.camera_forward));
    p.camera_info_present = desc.camera_info_present;
    std::lock_guard lock(core_->mutex);
    if (FAILED(hr = request(p.header, &frame->native))) return hr;
    if (!frame->native) return S_FALSE;
    result = std::move(frame); return S_OK;
  }
  HRESULT Generate(MTLD3D12Device *device, const MTL_FRAME_GENERATION_GENERATE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    if (desc.num_generated_frames != 1) return DXGI_ERROR_UNSUPPORTED;
    if (desc.outputs[1] || desc.outputs[2] || desc.outputs[3] || desc.outputs[0] == desc.present_color ||
        (desc.hudless_color && desc.outputs[0] == desc.hudless_color)) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>(); frame->core = core_; frame->generate = true;
    HRESULT hr;
    if (FAILED(hr = retainTexture(device, desc.present_color, frame->resources[0], frame->views[0], false, false)) ||
        FAILED(hr = retainTexture(device, desc.outputs[0], frame->resources[1], frame->views[1], false, true))) return hr;
    if (desc.hudless_color && FAILED(hr = retainTexture(device, desc.hudless_color,
        frame->resources[2], frame->views[2], false, false))) return hr;
    frame->states = {desc.present_color_state, desc.output_states[0], desc.hudless_color_state};
    WMTFGNativeCommand command{device->GetMTLDevice().handle, device->GetMetal4Compiler().handle};
    WMTFGdispatch_packet p{};
    p.header = {sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_DISPATCH, 0, core_->context};
    p.command_list = WMT::Metal4CPUPointer(&command); p.frame_id = desc.frame_id;
    p.present_color = frame->views[0].texture().handle; p.output = frame->views[1].texture().handle;
    if (desc.hudless_color) p.hudless_color = frame->views[2].texture().handle;
    p.present_color_state = desc.present_color_state; p.output_state = desc.output_states[0]; p.hudless_color_state = desc.hudless_color_state;
    p.num_generated_frames = 1; p.reset = desc.reset; p.backbuffer_transfer_function = desc.backbuffer_transfer_function;
    p.generation_rect_left = desc.generation_rect_left; p.generation_rect_top = desc.generation_rect_top;
    p.generation_rect_width = desc.generation_rect_width; p.generation_rect_height = desc.generation_rect_height;
    p.min_luminance = desc.min_luminance; p.max_luminance = desc.max_luminance; p.flags = desc.flags;
    std::lock_guard lock(core_->mutex);
    if (FAILED(hr = request(p.header, &frame->native))) return hr;
    if (!frame->native) return E_FAIL;
    result = std::move(frame); return S_OK;
  }
};
}
HRESULT CreateOwnedFrameGenerationState(MTLD3D12Device *device, const MTL_FRAME_GENERATION_CREATE_DESC *desc,
    IMTLD3D12FrameGenerationState **state) {
  if (!state) return E_POINTER;
  *state = nullptr;
  if (!device || !desc || desc->StructSize != sizeof(*desc) || !desc->display_width || !desc->display_height ||
      !desc->max_render_width || !desc->max_render_height || desc->display_width > 16384 || desc->display_height > 16384 ||
      desc->max_render_width > desc->display_width || desc->max_render_height > desc->display_height || (desc->flags & ~15u)) return E_INVALIDARG;
  try {
    auto core = std::make_shared<Core>(); core->device = device;
    core->timeline = device->GetMTLDevice().newSharedEvent();
    if (!core->timeline) return E_OUTOFMEMORY;
    MTL_DXGI_FORMAT_DESC format{};
    if (MTLQueryDXGIFormat(device->GetMTLDevice(), desc->backbuffer_format, format) < 0) return DXGI_ERROR_UNSUPPORTED;
    WMTFGcreate_packet p{{sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_CREATE, 0, 0}, device->GetMTLDevice().handle,
        desc->display_width, desc->display_height, desc->max_render_width, desc->max_render_height,
        uint32_t(format.PixelFormat), desc->flags};
    const auto hr = request(p.header);
    if (FAILED(hr)) return hr;
    core->context = p.header.context;
    Com<OwnedFrameGenerationState> implementation = new OwnedFrameGenerationState(std::move(core));
    return implementation->QueryInterface(__uuidof(IMTLD3D12FrameGenerationState), reinterpret_cast<void **>(state));
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT PrepareOwnedFrameGenerationFrame(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_PREPARE_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Prepare(device, *desc, frame); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT GenerateOwnedFrameGenerationFrame(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_GENERATE_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Generate(device, *desc, frame); } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
std::array<MTLD3D12Resource *, 3> GetOwnedFrameGenerationResources(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  return {frame->resources[0].ptr(), frame->resources[1].ptr(), frame->resources[2].ptr()};
}
std::array<D3D12_RESOURCE_STATES, 3> GetOwnedFrameGenerationStates(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  return frame->states;
}
bool IsOwnedFrameGenerationOutput(const std::shared_ptr<OwnedFrameGenerationFrame> &frame, size_t index) {
  return frame->generate && index == 1;
}
void EncodeOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame,
    WMT::MTL4CommandBuffer cmdbuf, WMT::Fence fence,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event) {
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  if (core.sequence) wait_event(core.timeline, core.sequence);
  const auto sequence = ++core.sequence;
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4FGEncode;
  call.args[0] = cmdbuf.handle; call.args[1] = frame->native.handle; call.args[2] = fence.handle;
  if (WMTMetal4Call(&call) || !call.ret[0]) core.device->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
  // Commit even partial native work before signaling, so another queue cannot strand.
  signal_event(core.timeline, sequence);
}
} // namespace dxmt
