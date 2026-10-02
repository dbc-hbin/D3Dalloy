// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned PE validation and immutable recording; algorithms live in the native reference port.
#include "d3d12_frame_generation.hpp"
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "winemetal_metal4_fg.h"
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <new>

namespace dxmt {
namespace {
HRESULT request(UINT operation, WMTFGpacket_header &packet, uint64_t *recorded = nullptr) {
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4FGRequest;
  call.args[0] = operation;
  call.args[1] = uintptr_t(&packet);
  if (WMTMetal4Call(&call)) return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
  if (recorded) *recorded = call.ret[1];
  switch (call.ret[0]) {
  case 0: return S_OK;
  case 4: return DXGI_ERROR_UNSUPPORTED;
  case 5: return E_OUTOFMEMORY;
  case 6: return E_INVALIDARG;
  default: return E_FAIL;
  }
}
struct Core {
  Com<MTLD3D12Device> device;
  uint64_t context = 0;
  std::mutex mutex;
  WMT::Reference<WMT::SharedEvent> timeline;
  uint64_t sequence = 0;
};
constexpr GUID kInternalStateId = {0xb2a31090,0xa1f2,0x4717,{0xbb,0x50,0x61,0x7a,0x47,0xf2,0xa0,0x13}};
struct IOwnedFrameGenerationStateInternal : IUnknown {
  virtual HRESULT Prepare(MTLD3D12Device *, const MTL_FRAME_GENERATION_PREPARE_DESC &, std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
  virtual HRESULT Generate(MTLD3D12Device *, const MTL_FRAME_GENERATION_GENERATE_DESC &, std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
};
HRESULT retainTexture(MTLD3D12Device *device, ID3D12Resource *input,
    Com<MTLD3D12Resource, false> &resource, TextureViewRef &view) {
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
  const auto key = owned->texture->createView({.format = owned->texture->pixelFormat(),
      .type = WMTTextureType2D, .miplevelCount = 1, .arraySize = 1});
  view = owned->texture->view(key);
  return view.texture() ? S_OK : E_FAIL;
}
}
struct OwnedFrameGenerationFrame {
  std::shared_ptr<Core> core;
  std::array<Com<MTLD3D12Resource, false>,3> resources;
  std::array<TextureViewRef,3> views;
  WMT::Reference<WMT::Object> prepared;
  bool generate = false;
};
namespace {
class OwnedFrameGenerationState final : public ComObject<IMTLD3D12FrameGenerationState, IOwnedFrameGenerationStateInternal> {
  std::shared_ptr<Core> core_;
public:
  explicit OwnedFrameGenerationState(std::shared_ptr<Core> core) : core_(std::move(core)) {}
  ~OwnedFrameGenerationState() {
    std::lock_guard lock(core_->mutex);
    if (core_->context) {
      WMTFGdestroy_packet packet{{sizeof(packet),WMTFG_BRIDGE_VERSION,WMTFG_DESTROY,0,core_->context}};
      request(WMTFG_DESTROY,packet.header);
    }
  }
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
    WMTFGconfigure_packet packet{{sizeof(packet),WMTFG_BRIDGE_VERSION,WMTFG_CONFIGURE,0,core_->context},UINT(enabled)};
    return request(WMTFG_CONFIGURE,packet.header);
  }
  HRESULT STDMETHODCALLTYPE RetireFrame(UINT64 frame, UINT64 *oldest, BOOL *pending) final {
    if (!oldest || !pending) return E_POINTER;
    *oldest = 0; *pending = FALSE;
    std::lock_guard lock(core_->mutex);
    WMTFGretire_frame_packet packet{{sizeof(packet),WMTFG_BRIDGE_VERSION,WMTFG_RETIRE_FRAME,0,core_->context},frame,0,0};
    const auto hr = request(WMTFG_RETIRE_FRAME,packet.header);
    if (SUCCEEDED(hr)) { *oldest = packet.oldest_pending_frame_id; *pending = packet.has_pending_frame; }
    return hr;
  }
  HRESULT Prepare(MTLD3D12Device *device, const MTL_FRAME_GENERATION_PREPARE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>();
    frame->core = core_;
    HRESULT hr;
    if (FAILED(hr = retainTexture(device,desc.depth,frame->resources[0],frame->views[0])) ||
        FAILED(hr = retainTexture(device,desc.motion_vectors,frame->resources[1],frame->views[1]))) return hr;
    WMTFGNativeCommand command{device->GetMTLDevice().handle,device->GetMetal4Compiler().handle};
    WMTFGprepare_packet packet{};
    packet.header = {sizeof(packet),WMTFG_BRIDGE_VERSION,WMTFG_PREPARE,0,core_->context};
    packet.command_list = uintptr_t(&command);
    packet.frame_id = desc.frame_id;
    packet.depth = frame->views[0].texture().handle;
    packet.motion_vectors = frame->views[1].texture().handle;
    packet.depth_state = desc.depth_state; packet.motion_vectors_state = desc.motion_vectors_state;
    packet.render_width = desc.render_width; packet.render_height = desc.render_height;
    packet.flags = desc.flags; packet.reset = desc.reset;
    packet.jitter_x = desc.jitter_x; packet.jitter_y = desc.jitter_y;
    packet.motion_scale_x = desc.motion_scale_x; packet.motion_scale_y = desc.motion_scale_y;
    packet.frame_time_delta_ms = desc.frame_time_delta_ms;
    packet.camera_near = desc.camera_near; packet.camera_far = desc.camera_far;
    packet.camera_fov_vertical_radians = desc.camera_fov_vertical_radians;
    packet.view_space_to_meters = desc.view_space_to_meters;
    std::copy(std::begin(desc.camera_position),std::end(desc.camera_position),packet.camera_position);
    std::copy(std::begin(desc.camera_up),std::end(desc.camera_up),packet.camera_up);
    std::copy(std::begin(desc.camera_right),std::end(desc.camera_right),packet.camera_right);
    std::copy(std::begin(desc.camera_forward),std::end(desc.camera_forward),packet.camera_forward);
    packet.camera_info_present = desc.camera_info_present;
    uint64_t recorded = 0;
    std::lock_guard lock(core_->mutex);
    hr = request(WMTFG_PREPARE,packet.header,&recorded);
    if (FAILED(hr)) return hr;
    // Disabled preparation validates eligibility but intentionally records no GPU pass.
    if (!recorded) return S_FALSE;
    frame->prepared = WMT::Reference<WMT::Object>(recorded);
    result = std::move(frame);
    return S_OK;
  }
  HRESULT Generate(MTLD3D12Device *device, const MTL_FRAME_GENERATION_GENERATE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>();
    frame->core = core_; frame->generate = true;
    HRESULT hr;
    if (FAILED(hr = retainTexture(device,desc.present_color,frame->resources[0],frame->views[0])) ||
        FAILED(hr = retainTexture(device,desc.output,frame->resources[1],frame->views[1]))) return hr;
    if (desc.hudless_color && FAILED(hr = retainTexture(device,desc.hudless_color,frame->resources[2],frame->views[2]))) return hr;
    WMTFGNativeCommand command{device->GetMTLDevice().handle,device->GetMetal4Compiler().handle};
    WMTFGdispatch_packet packet{};
    packet.header = {sizeof(packet),WMTFG_BRIDGE_VERSION,WMTFG_DISPATCH,0,core_->context};
    packet.command_list = uintptr_t(&command); packet.frame_id = desc.frame_id;
    packet.present_color = frame->views[0].texture().handle;
    packet.output = frame->views[1].texture().handle;
    packet.hudless_color = frame->views[2].texture().handle;
    packet.present_color_state = desc.present_color_state; packet.output_state = desc.output_state;
    packet.hudless_color_state = desc.hudless_color_state;
    packet.num_generated_frames = desc.num_generated_frames; packet.reset = desc.reset;
    packet.backbuffer_transfer_function = desc.backbuffer_transfer_function;
    packet.generation_rect_left = desc.generation_rect_left; packet.generation_rect_top = desc.generation_rect_top;
    packet.generation_rect_width = desc.generation_rect_width; packet.generation_rect_height = desc.generation_rect_height;
    packet.min_luminance = desc.min_luminance; packet.max_luminance = desc.max_luminance;
    uint64_t recorded = 0;
    std::lock_guard lock(core_->mutex);
    hr = request(WMTFG_DISPATCH,packet.header,&recorded);
    if (FAILED(hr)) return hr;
    if (!recorded) return E_FAIL;
    frame->prepared = WMT::Reference<WMT::Object>(recorded);
    result = std::move(frame);
    return S_OK;
  }
};
}
HRESULT CreateOwnedFrameGenerationState(MTLD3D12Device *device, const MTL_FRAME_GENERATION_CREATE_DESC *desc,
    IMTLD3D12FrameGenerationState **state) {
  if (!state) return E_POINTER;
  *state = nullptr;
  if (!desc || desc->StructSize != sizeof(*desc) || !desc->display_width || !desc->display_height ||
      !desc->max_render_width || !desc->max_render_height || desc->display_width > 16384 ||
      desc->display_height > 16384 || (desc->flags & ~15u)) return E_INVALIDARG;
  WMTPixelFormat format;
  switch(desc->backbuffer_format) {
  case DXGI_FORMAT_R16G16B16A16_FLOAT: format = WMTPixelFormatRGBA16Float; break;
  case DXGI_FORMAT_R10G10B10A2_UNORM: format = WMTPixelFormatRGB10A2Unorm; break;
  case DXGI_FORMAT_R8G8B8A8_UNORM: format = WMTPixelFormatRGBA8Unorm; break;
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: format = WMTPixelFormatRGBA8Unorm_sRGB; break;
  case DXGI_FORMAT_B8G8R8A8_UNORM: format = WMTPixelFormatBGRA8Unorm; break;
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: format = WMTPixelFormatBGRA8Unorm_sRGB; break;
  default: return DXGI_ERROR_UNSUPPORTED;
  }
  try {
    auto core = std::make_shared<Core>();
    core->device = device;
    core->timeline = device->GetMTLDevice().newSharedEvent();
    if (!core->timeline) return E_OUTOFMEMORY;
    Com<OwnedFrameGenerationState> implementation = new OwnedFrameGenerationState(core);
    WMTFGcreate_packet packet{{sizeof(packet),WMTFG_BRIDGE_VERSION,WMTFG_CREATE,0,0},
        device->GetMTLDevice().handle,desc->display_width,desc->display_height,
        desc->max_render_width,desc->max_render_height,UINT(format),desc->flags};
    const auto hr = request(WMTFG_CREATE,packet.header);
    if (FAILED(hr)) return hr;
    core->context = packet.header.context;
    return implementation->QueryInterface(__uuidof(IMTLD3D12FrameGenerationState),reinterpret_cast<void **>(state));
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT PrepareOwnedFrameGenerationFrame(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_PREPARE_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId,reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Prepare(device,*desc,frame); }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT GenerateOwnedFrameGenerationFrame(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_GENERATE_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId,reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Generate(device,*desc,frame); }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
std::array<MTLD3D12Resource *,3> GetOwnedFrameGenerationResources(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  return {frame->resources[0].ptr(),frame->resources[1].ptr(),frame->resources[2].ptr()};
}
bool IsOwnedFrameGenerationGenerate(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) { return frame->generate; }
void EncodeOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame,
    WMT::MTL4CommandBuffer buffer, WMT::Fence fence, uint64_t queue, uint64_t submission, uint64_t encoder,
    const std::function<void(WMT::Event,uint64_t)> &wait_event,
    const std::function<void(WMT::Event,uint64_t)> &signal_event) {
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  // Both snapshot production and generation join the same GPU timeline across queues.
  // Commit+signal under this lock publishes a prepared snapshot before a consumer waits.
  if (core.sequence) wait_event(core.timeline,core.sequence);
  const auto sequence = ++core.sequence;
  struct WMTMetal4Call call{};
  call.operation = WMTMetal4FGEncode;
  call.args[0] = buffer.handle; call.args[1] = frame->prepared.handle; call.args[2] = fence.handle;
  const auto status = WMTMetal4Call(&call);
  owned_graphics::Trace("frame_generation_encode",",\"queue\":",queue,",\"submission\":",submission,
      ",\"encoder\":",encoder,",\"generate\":",unsigned(frame->generate),",\"success\":",unsigned(!status && call.ret[0]));
  // Partial encode still owns its native execution lease until this segment retires.
  signal_event(core.timeline,sequence);
  if (status || !call.ret[0]) core.device->ReportRemoval(DXGI_ERROR_DRIVER_INTERNAL_ERROR);
}
} // namespace dxmt
