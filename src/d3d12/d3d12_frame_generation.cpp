// SPDX-License-Identifier: LGPL-2.1-or-later
// Replayable D3D12 translation to the shared native MetalFX frame interpolator.
#include "d3d12_frame_generation.hpp"
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "winemetal_metal4_fg.h"
#include "d3dalloy_buffer.hpp"
#include <cstring>
#include <mutex>
#include <new>

namespace d3dalloy {
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
  uint64_t context = 0;
  // Queue-thread encode order only. Native requests serialize on their own
  // context state, so app-thread Configure/Prepare/Generate never wait on encode.
  uint64_t sequence = 0;
  uint64_t last_queue = 0; // owned id of the queue that encoded `sequence`
  WMT::Reference<WMT::SharedEvent> timeline;
  std::mutex encode_mutex;
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
  std::array<Com<MTLD3D12Resource, false>, 8> resources;
  std::array<TextureViewRef, 7> views;
  std::array<D3D12_RESOURCE_STATES, 8> states{};
  Rc<BufferAllocation> disable_allocation;
  WMT::Reference<WMT::Object> native;
  // Generate only: a direct Prepare's depth/motion, read in place by this pass.
  std::shared_ptr<OwnedFrameGenerationFrame> inputs;
  uint64_t frame_id = 0;
  bool generate = false;
};
namespace {
class OwnedFrameGenerationState final : public ComObject<IMTLD3D12FrameGenerationState, IOwnedFrameGenerationStateInternal> {
  std::shared_ptr<Core> core_;
  // A direct Prepare records nothing; its Generate on the same list adopts these inputs.
  std::mutex direct_mutex_;
  std::shared_ptr<OwnedFrameGenerationFrame> direct_inputs_;
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
    WMTFGconfigure_packet p{{sizeof(p), WMTFG_BRIDGE_VERSION, WMTFG_CONFIGURE, 0, core_->context}, UINT(enabled)};
    return request(p.header);
  }
  HRESULT STDMETHODCALLTYPE RetireFrame(UINT64 frame_id, UINT64 *oldest, BOOL *pending) final {
    if (!oldest || !pending) return E_POINTER;
    *oldest = 0; *pending = FALSE;
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
    p.render_width = desc.render_width; p.render_height = desc.render_height;
    p.motion_width = desc.motion_width; p.motion_height = desc.motion_height;
    p.flags = desc.flags; p.reset = desc.reset;
    p.jitter_x = desc.jitter_x; p.jitter_y = desc.jitter_y;
    p.motion_scale_x = desc.motion_scale_x; p.motion_scale_y = desc.motion_scale_y;
    p.frame_time_delta_ms = desc.frame_time_delta_ms; p.camera_near = desc.camera_near; p.camera_far = desc.camera_far;
    p.camera_fov_vertical_radians = desc.camera_fov_vertical_radians; p.view_space_to_meters = desc.view_space_to_meters;
    p.camera_aspect_ratio = desc.camera_aspect_ratio;
    p.motion_vectors_invalid_value = desc.motion_vectors_invalid_value;
    p.motion_vectors_invalid_value_present = desc.motion_vectors_invalid_value_present;
    std::memcpy(p.camera_position, desc.camera_position, sizeof(p.camera_position));
    std::memcpy(p.camera_up, desc.camera_up, sizeof(p.camera_up));
    std::memcpy(p.camera_right, desc.camera_right, sizeof(p.camera_right));
    std::memcpy(p.camera_forward, desc.camera_forward, sizeof(p.camera_forward));
    p.camera_info_present = desc.camera_info_present;
    p.depth_subrect_x = desc.depth_subrect_x; p.depth_subrect_y = desc.depth_subrect_y;
    p.motion_subrect_x = desc.motion_subrect_x; p.motion_subrect_y = desc.motion_subrect_y;
    p.reconstruct_camera_motion = desc.reconstruct_camera_motion;
    std::memcpy(p.clip_to_previous_clip, desc.clip_to_previous_clip, sizeof(p.clip_to_previous_clip));
    p.projection_matrix_present = desc.projection_matrix_present;
    std::memcpy(p.view_to_clip, desc.view_to_clip, sizeof(p.view_to_clip));
    if (FAILED(hr = request(p.header, &frame->native))) return hr;
    if (!frame->native) {
      // Native accepted the frame without a snapshot command (direct inputs or FG off).
      if (p.flags & WMTFG_PREPARE_DIRECT_INPUTS) {
        frame->frame_id = desc.frame_id;
        std::lock_guard lock(direct_mutex_);
        direct_inputs_ = std::move(frame);
      }
      return S_FALSE;
    }
    result = std::move(frame); return S_OK;
  }
  HRESULT Generate(MTLD3D12Device *device, const MTL_FRAME_GENERATION_GENERATE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    if (desc.num_generated_frames != 1) return DXGI_ERROR_UNSUPPORTED;
    if (desc.outputs[1] || desc.outputs[2] || desc.outputs[3] || desc.outputs[0] == desc.present_color ||
        (desc.hudless_color && desc.outputs[0] == desc.hudless_color)) return E_INVALIDARG;
    if (desc.real_output && (desc.real_output == desc.outputs[0] || desc.real_output == desc.present_color ||
        desc.real_output == desc.hudless_color)) return E_INVALIDARG;
    if (desc.distortion && (desc.distortion == desc.outputs[0] || desc.distortion == desc.real_output)) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>(); frame->core = core_; frame->generate = true;
    HRESULT hr;
    if (FAILED(hr = retainTexture(device, desc.present_color, frame->resources[0], frame->views[0], false, false)) ||
        FAILED(hr = retainTexture(device, desc.outputs[0], frame->resources[1], frame->views[1], false, true))) return hr;
    if (desc.hudless_color && FAILED(hr = retainTexture(device, desc.hudless_color,
        frame->resources[2], frame->views[2], false, false))) return hr;
    if (desc.real_output && FAILED(hr = retainTexture(device, desc.real_output,
        frame->resources[3], frame->views[3], false, true))) return hr;
    if (desc.distortion && FAILED(hr = retainTexture(device, desc.distortion,
        frame->resources[4], frame->views[4], false, false))) return hr;
    if (desc.ui && FAILED(hr = retainTexture(device, desc.ui,
        frame->resources[5], frame->views[5], false, false))) return hr;
    if (desc.ui_alpha && FAILED(hr = retainTexture(device, desc.ui_alpha,
        frame->resources[6], frame->views[6], false, false))) return hr;
    frame->states = {desc.present_color_state, desc.output_states[0], desc.hudless_color_state,
        desc.real_output_state, desc.distortion_state, desc.ui_state, desc.ui_alpha_state};
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
    if (desc.real_output) p.real_output = frame->views[3].texture().handle;
    p.real_output_state = desc.real_output_state;
    p.output_subrect_only = desc.output_subrect_only;
    if (desc.distortion) p.distortion = frame->views[4].texture().handle;
    p.distortion_state = desc.distortion_state;
    p.distortion_x = desc.distortion_x; p.distortion_y = desc.distortion_y;
    p.distortion_width = desc.distortion_width; p.distortion_height = desc.distortion_height;
    p.independent_subrects = desc.independent_subrects;
    p.present_x = desc.present_x; p.present_y = desc.present_y;
    p.hudless_x = desc.hudless_x; p.hudless_y = desc.hudless_y;
    p.output_x = desc.output_x; p.output_y = desc.output_y;
    p.real_x = desc.real_x; p.real_y = desc.real_y;
    p.distortion_low_precision = desc.distortion_low_precision;
    p.distortion_scale = desc.distortion_scale; p.distortion_bias = desc.distortion_bias;
    if (desc.ui) p.ui = frame->views[5].texture().handle;
    if (desc.ui_alpha) p.ui_alpha = frame->views[6].texture().handle;
    p.ui_x = desc.ui_x; p.ui_y = desc.ui_y;
    p.ui_alpha_x = desc.ui_alpha_x; p.ui_alpha_y = desc.ui_alpha_y;
    if (desc.disable_interpolation) {
      Com<ID3D12Device> owner;
      const auto output = desc.disable_interpolation->GetDesc();
      if (FAILED(desc.disable_interpolation->GetDevice(IID_PPV_ARGS(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device) ||
          output.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || output.Width < sizeof(UINT) ||
          !(output.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return E_INVALIDARG;
      auto resource = static_cast<MTLD3D12Resource *>(desc.disable_interpolation);
      if (!resource->buffer || !resource->buffer->current()) return E_INVALIDARG;
      frame->resources[7] = resource;
      frame->states[7] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      frame->disable_allocation = resource->buffer->current();
      p.disable_interpolation = frame->disable_allocation->buffer().handle;
      p.disable_interpolation_offset = frame->disable_allocation->currentSuballocationOffset();
    }
    if (FAILED(hr = request(p.header, &frame->native))) return hr;
    if (!frame->native) return E_FAIL;
    {
      std::lock_guard lock(direct_mutex_);
      if (direct_inputs_ && direct_inputs_->frame_id == desc.frame_id) frame->inputs = std::move(direct_inputs_);
      direct_inputs_.reset();
    }
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
// Slots 8-9 are a direct Prepare's depth/motion, which the Generate pass reads in place.
std::array<MTLD3D12Resource *, 10> GetOwnedFrameGenerationResources(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  const auto &inputs = frame->inputs;
  return {frame->resources[0].ptr(), frame->resources[1].ptr(), frame->resources[2].ptr(),
      frame->resources[3].ptr(), frame->resources[4].ptr(), frame->resources[5].ptr(), frame->resources[6].ptr(),
      frame->resources[7].ptr(), inputs ? inputs->resources[0].ptr() : nullptr,
      inputs ? inputs->resources[1].ptr() : nullptr};
}
std::array<D3D12_RESOURCE_STATES, 10> GetOwnedFrameGenerationStates(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  const auto &s = frame->states;
  const auto &inputs = frame->inputs;
  return {s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
      inputs ? inputs->states[0] : D3D12_RESOURCE_STATE_COMMON, inputs ? inputs->states[1] : D3D12_RESOURCE_STATE_COMMON};
}
bool IsOwnedFrameGenerationOutput(const std::shared_ptr<OwnedFrameGenerationFrame> &frame, size_t index) {
  return frame->generate && (index == 1 || index == 3 || index == 7);
}
uint64_t OwnedFrameGenerationFrameHistoryQueue(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  auto &core = *frame->core;
  std::lock_guard lock(core.encode_mutex);
  return core.sequence ? core.last_queue : 0;
}
void EncodeOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame,
    WMT::MTL4CommandBuffer cmdbuf, WMT::Fence fence, uint64_t queue,
    const std::function<void(WMT::Event, uint64_t)> &wait_event,
    const std::function<void(WMT::Event, uint64_t)> &signal_event) {
  auto &core = *frame->core;
  std::lock_guard lock(core.encode_mutex);
  if (core.sequence && core.last_queue != queue) wait_event(core.timeline, core.sequence);
  const auto sequence = ++core.sequence;
  core.last_queue = queue;
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4FGEncode;
  call.args[0] = cmdbuf.handle; call.args[1] = frame->native.handle; call.args[2] = fence.handle;
  if (WMTMetal4Call(&call) || !call.ret[0]) core.device->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
  // Commit even partial native work before signaling, so another queue cannot strand.
  signal_event(core.timeline, sequence);
}
} // namespace d3dalloy
