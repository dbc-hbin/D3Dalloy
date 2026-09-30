// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned, replayable D3D12 -> public MetalFX temporal command implementation.
// The fence bridge follows DXMT's DX11 TemporalUpscale encoder; D3D12 recording,
// per-feature GPU ordering, scratch output, ownership, and validation are here.
#include "d3d12_temporal.hpp"
#include "d3d12_device.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_scaler.hpp"
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>

namespace dxmt {
namespace {
// NVIDIA nvsdk_ngx_defs.h: retain these values, not a new backend flag encoding.
constexpr UINT kHDR = 1, kLowResMV = 2, kDepthInverted = 8, kSharpening = 32, kAutoExposure = 64;
constexpr UINT kSupportedFlags = kHDR | kLowResMV | kDepthInverted | kSharpening | kAutoExposure;

struct GenerationKey {
  WMTPixelFormat color, depth, motion, output;
  UINT width, height;
  bool operator==(const GenerationKey &other) const {
    return std::tie(color, depth, motion, output, width, height) ==
           std::tie(other.color, other.depth, other.motion, other.output, other.width, other.height);
  }
};

struct Generation {
  const uint64_t id = owned_graphics::NewObjectId();
  GenerationKey key{};
  Rc<TemporalScaler> scaler;
};

struct Core {
  Com<MTLD3D12Device> device;
  const uint64_t id = owned_graphics::NewObjectId();
  MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC create{};
  std::mutex mutex;
  std::shared_ptr<Generation> current;
  WMT::Reference<WMT::SharedEvent> timeline;
  uint64_t sequence = 0;
  // A retained closed list may revisit an old generation after another one ran.
  // Its old history is then discontinuous even though it was initialized once.
  uint64_t last_executed_generation = 0;
};

// A second, DLL-private QI boundary avoids interpreting an arbitrary public
// IUnknown's memory layout as an OwnedTemporalState implementation.
constexpr GUID kInternalStateId = {0xc5b7eebc, 0xa4fc, 0x45b2, {0x9b, 0x1c, 0xd9, 0xb1, 0x31, 0xf1, 0x37, 0x92}};
struct IOwnedTemporalStateInternal : IUnknown {
  virtual HRESULT Prepare(MTLD3D12Device *, const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC &,
                          std::shared_ptr<OwnedTemporalFrame> &) = 0;
};

WMTPixelFormat motionFormat(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatRG16Float: case WMTPixelFormatRG16Uint: case WMTPixelFormatRG16Sint:
  case WMTPixelFormatRG16Unorm: case WMTPixelFormatRG16Snorm:
    return WMTPixelFormatRG16Float;
  case WMTPixelFormatRG32Float: case WMTPixelFormatRG32Uint: case WMTPixelFormatRG32Sint:
    return WMTPixelFormatRG32Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

HRESULT retainTexture(MTLD3D12Device *device, ID3D12Resource *input,
                      Com<MTLD3D12Resource, false> &resource, TextureViewRef &view,
                      WMTPixelFormat format = WMTPixelFormatInvalid) {
  if (!input)
    return E_INVALIDARG;
  Com<ID3D12Device> owner;
  // Com::ref() returns an AddRef'd T*, not the address of its output storage.
  if (FAILED(input->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
      owner.ptr() != static_cast<ID3D12Device *>(device))
    return E_INVALIDARG;
  auto desc = input->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height)
    return E_INVALIDARG;
  auto owned = static_cast<MTLD3D12Resource *>(input);
  if (!owned->texture || !owned->texture->current())
    return E_INVALIDARG;
  resource = owned;
  auto texture = owned->texture;
  const auto key = texture->createView({.format = format == WMTPixelFormatInvalid ? texture->pixelFormat() : format,
                                      .type = WMTTextureType2D, .miplevelCount = 1, .arraySize = 1});
  view = texture->view(key);
  return view.texture() ? S_OK : E_FAIL;
}
} // namespace

struct OwnedTemporalFrame {
  std::shared_ptr<Core> core;
  std::shared_ptr<Generation> generation;
  const uint64_t id = owned_graphics::NewObjectId();
  std::array<Com<MTLD3D12Resource, false>, 5> resources;
  TextureViewRef color, depth, motion, output, exposure;
  WMT::Reference<WMT::Texture> scratch_output;
  WMTFXTemporalScalerProps props{};
  UINT output_width = 0, output_height = 0;
  UINT output_x = 0, output_y = 0;
  bool scratch_resident = false;

  ~OwnedTemporalFrame() {
    if (scratch_resident)
      core->device->UnregisterResidency(scratch_output);
    if (core)
      owned_graphics::Trace("temporal_frame_destroy", ",\"feature\":", core->id,
                            ",\"evaluation\":", id,
                            ",\"generation\":", generation ? generation->id : 0);
  }
};

namespace {
class OwnedTemporalState final : public ComObject<IMTLD3D12TemporalUpscaleState, IOwnedTemporalStateInternal> {
  std::shared_ptr<Core> core_;

public:
  explicit OwnedTemporalState(std::shared_ptr<Core> core) : core_(std::move(core)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) final {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMTLD3D12TemporalUpscaleState))
      *object = static_cast<IMTLD3D12TemporalUpscaleState *>(this);
    else if (iid == kInternalStateId)
      *object = static_cast<IOwnedTemporalStateInternal *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }

  HRESULT Prepare(MTLD3D12Device *device, const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC &desc,
                  std::shared_ptr<OwnedTemporalFrame> &result) final {
    if (device != core_->device.ptr() || !desc.InputContentWidth || !desc.InputContentHeight)
      return E_INVALIDARG;
    for (float scalar : {desc.JitterOffsetX, desc.JitterOffsetY, desc.MotionVectorScaleX,
                         desc.MotionVectorScaleY, desc.PreExposure, desc.ExposureScale})
      if (!std::isfinite(scalar))
        return E_INVALIDARG;
    if (!desc.MotionVectorScaleX || !desc.MotionVectorScaleY || desc.PreExposure <= 0 || desc.ExposureScale <= 0)
      return E_INVALIDARG;
    if (desc.ExposureScale != 1.0f || desc.BiasCurrentColorMask || desc.TransparencyMask ||
        desc.ColorBaseX || desc.ColorBaseY || desc.DepthBaseX || desc.DepthBaseY ||
        desc.MotionBaseX || desc.MotionBaseY || desc.BiasBaseX || desc.BiasBaseY)
      return E_NOTIMPL;

    const auto &create = core_->create;
    if ((desc.OutputBaseX || desc.OutputBaseY) && !create.OutputSubrectsEnabled)
      return E_NOTIMPL;
    // Initial supported subset is the existing DXMT temporal range. The native
    // factory is still authoritative and can reject unsupported device formats.
    const float sx = float(create.OutputWidth) / desc.InputContentWidth;
    const float sy = float(create.OutputHeight) / desc.InputContentHeight;
    if (sx < 1.0f || sy < 1.0f || sx > 3.0f || sy > 3.0f)
      return E_NOTIMPL;

    auto frame = std::make_shared<OwnedTemporalFrame>();
    frame->core = core_;
    HRESULT hr;
    if (FAILED(hr = retainTexture(device, desc.Color, frame->resources[0], frame->color)) ||
        FAILED(hr = retainTexture(device, desc.Depth, frame->resources[1], frame->depth)) ||
        FAILED(hr = retainTexture(device, desc.MotionVector, frame->resources[2], frame->motion)) ||
        FAILED(hr = retainTexture(device, desc.Output, frame->resources[3], frame->output)))
      return hr;
    auto color = frame->resources[0]->texture;
    auto depth = frame->resources[1]->texture;
    auto motion = frame->resources[2]->texture;
    auto output = frame->resources[3]->texture;
    if (color->width() != depth->width() || color->height() != depth->height() ||
        color->width() != motion->width() || color->height() != motion->height() ||
        desc.InputContentWidth > color->width() || desc.InputContentHeight > color->height() ||
        desc.OutputBaseX > output->width() || desc.OutputBaseY > output->height() ||
        create.OutputWidth > output->width() - desc.OutputBaseX ||
        create.OutputHeight > output->height() - desc.OutputBaseY)
      return E_INVALIDARG;
    if (desc.Color == desc.Output || desc.Depth == desc.Output || desc.MotionVector == desc.Output)
      return E_INVALIDARG;
    if (!(color->usage() & WMTTextureUsageShaderRead) || !(depth->usage() & WMTTextureUsageShaderRead) ||
        !(motion->usage() & WMTTextureUsageShaderRead))
      return E_INVALIDARG;
    const auto mv_format = motionFormat(motion->pixelFormat());
    if (mv_format == WMTPixelFormatInvalid)
      return E_NOTIMPL;
    if (FAILED(hr = retainTexture(device, desc.MotionVector, frame->resources[2], frame->motion, mv_format)))
      return hr;
    if (!(create.FeatureFlags & kAutoExposure) && desc.ExposureTexture) {
      if (FAILED(hr = retainTexture(device, desc.ExposureTexture, frame->resources[4], frame->exposure)))
        return hr;
      auto exposure = frame->resources[4]->texture;
      if (exposure->pixelFormat() != WMTPixelFormatR16Float || exposure->width() != 1 || exposure->height() != 1)
        return E_NOTIMPL; // Numerical R32 -> R16 exposure conversion is not silently skipped.
    }

    GenerationKey key{color->pixelFormat(), depth->pixelFormat(), mv_format, output->pixelFormat(),
                      color->width(), color->height()};
    std::lock_guard lock(core_->mutex);
    if (!core_->current || !(core_->current->key == key)) {
      auto generation = std::make_shared<Generation>();
      generation->key = key;
      WMTFXTemporalScalerInfo info{};
      info.color_format = key.color;
      info.depth_format = key.depth;
      info.motion_format = key.motion;
      info.output_format = key.output;
      info.input_width = key.width;
      info.input_height = key.height;
      info.output_width = create.OutputWidth;
      info.output_height = create.OutputHeight;
      info.input_content_min_scale = 1.0f;
      info.input_content_max_scale = 3.0f;
      info.input_content_properties_enabled = true;
      info.requires_synchronous_initialization = true;
      info.auto_exposure = (create.FeatureFlags & kAutoExposure) != 0;
      generation->scaler = new TemporalScaler(device->GetMTLDevice(), info);
      if (!generation->scaler->scaler() || !generation->scaler->fence()) {
        ERR("OwnedTemporal: MetalFX factory rejected formats ", key.color, "/", key.depth, "/", key.motion, "/", key.output);
        return E_FAIL;
      }
      core_->current = std::move(generation);
    }
    frame->generation = core_->current;

    // Always render into a private logical-sized texture. The application's
    // resource can have larger backing capacity or shared storage. Copy only
    // the requested output rectangle at its caller-specified base, preserving
    // every pixel outside it. MetalFX history remains in logical coordinates.
    WMTTextureInfo info{};
    info.width = create.OutputWidth;
    info.height = create.OutputHeight;
    info.depth = 1;
    info.array_length = 1;
    info.mipmap_level_count = 1;
    info.pixel_format = key.output;
    info.sample_count = 1;
    info.type = WMTTextureType2D;
    info.usage = WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite | WMTTextureUsageRenderTarget;
    info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
    frame->scratch_output = device->GetMTLDevice().newTexture(info);
    if (!frame->scratch_output)
      return E_OUTOFMEMORY;
    if (FAILED(hr = device->RegisterResidency(frame->scratch_output)))
      return hr;
    frame->scratch_resident = true;
    frame->output_width = create.OutputWidth;
    frame->output_height = create.OutputHeight;
    frame->output_x = desc.OutputBaseX;
    frame->output_y = desc.OutputBaseY;
    frame->props = {desc.InputContentWidth, desc.InputContentHeight, bool(desc.ResetHistory),
                    bool(create.FeatureFlags & kDepthInverted), desc.MotionVectorScaleX, desc.MotionVectorScaleY,
                    desc.JitterOffsetX, desc.JitterOffsetY, desc.PreExposure};
    owned_graphics::Trace("temporal_record", ",\"feature\":", core_->id, ",\"evaluation\":", frame->id,
                          ",\"generation\":", frame->generation->id,
                          ",\"color\":", frame->resources[0]->owned_id, ",\"depth\":", frame->resources[1]->owned_id,
                          ",\"motion\":", frame->resources[2]->owned_id, ",\"output\":", frame->resources[3]->owned_id,
                          ",\"output_x\":", frame->output_x, ",\"output_y\":", frame->output_y);
    result = std::move(frame);
    return S_OK;
  }
};
} // namespace

HRESULT CreateOwnedTemporalState(MTLD3D12Device *device,
    const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc, IMTLD3D12TemporalUpscaleState **state) {
  if (!state)
    return E_POINTER;
  *state = nullptr;
  if (!desc || desc->StructSize != sizeof(*desc) || !desc->InputWidth || !desc->InputHeight ||
      !desc->OutputWidth || !desc->OutputHeight || desc->InputWidth > 16384 || desc->InputHeight > 16384 ||
      desc->OutputWidth > 16384 || desc->OutputHeight > 16384)
    return E_INVALIDARG;
  if (desc->CreationNodeMask > 1 || desc->VisibilityNodeMask > 1 ||
      (desc->FeatureFlags & ~kSupportedFlags) || !(desc->FeatureFlags & kLowResMV))
    return E_NOTIMPL;
  if (!device->GetMTLDevice().supportsFXTemporalScaler())
    return DXGI_ERROR_UNSUPPORTED;
  try {
    auto core = std::make_shared<Core>();
    core->device = device;
    core->create = *desc;
    core->timeline = device->GetMTLDevice().newSharedEvent();
    if (!core->timeline)
      return E_OUTOFMEMORY;
    Com<OwnedTemporalState> implementation = new OwnedTemporalState(std::move(core));
    return implementation->QueryInterface(__uuidof(IMTLD3D12TemporalUpscaleState), reinterpret_cast<void **>(state));
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

HRESULT PrepareOwnedTemporalFrame(MTLD3D12Device *device, IMTLD3D12TemporalUpscaleState *state,
    const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc, std::shared_ptr<OwnedTemporalFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc))
    return E_INVALIDARG;
  Com<IOwnedTemporalStateInternal> internal;
  auto hr = state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal));
  if (FAILED(hr))
    return E_INVALIDARG;
  try {
    return internal->Prepare(device, *desc, frame);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

std::array<MTLD3D12Resource *, 5> GetOwnedTemporalFrameResources(const std::shared_ptr<OwnedTemporalFrame> &frame) {
  return {frame->resources[0].ptr(), frame->resources[1].ptr(), frame->resources[2].ptr(),
          frame->resources[3].ptr(), frame->resources[4].ptr()};
}

void EncodeOwnedTemporalFrame(const std::shared_ptr<OwnedTemporalFrame> &frame,
    WMT::CommandBuffer cmdbuf, WMT::Fence fence, uint64_t queue, uint64_t submission, uint64_t encoder) {
  auto &core = *frame->core;
  auto &generation = *frame->generation;
  std::lock_guard lock(core.mutex);
  // The timeline protects history even when the same feature is submitted on
  // different D3D12 queues. No CPU wait or waitUntilCompleted is introduced.
  if (core.sequence)
    cmdbuf.encodeWaitForEvent(core.timeline, core.sequence);
  const auto sequence = ++core.sequence;
  auto props = frame->props;
  props.reset = props.reset || core.last_executed_generation != generation.id;
  auto begin = cmdbuf.blitCommandEncoder();
  begin.setLabel(WMT::String::string("OwnedTemporal.Begin", WMTUTF8StringEncoding));
  begin.waitForFence(fence);
  begin.updateFence(generation.scaler->fence());
  begin.endEncoding();
  owned_graphics::Trace("temporal_encode", ",\"queue\":", queue, ",\"submission\":", submission,
                        ",\"encoder\":", encoder, ",\"feature\":", core.id, ",\"evaluation\":", frame->id,
                        ",\"generation\":", generation.id, ",\"history_sequence\":", sequence,
                        ",\"caller_reset\":", unsigned(frame->props.reset), ",\"reset\":", unsigned(props.reset),
                        ",\"color\":", frame->resources[0]->owned_id, ",\"output\":", frame->resources[3]->owned_id);
  cmdbuf.encodeTemporalScale(generation.scaler->scaler(), frame->color.texture(), frame->scratch_output,
                            frame->depth.texture(), frame->motion.texture(), frame->exposure.texture(),
                            generation.scaler->fence(), props);
  auto end = cmdbuf.blitCommandEncoder();
  end.setLabel(WMT::String::string("OwnedTemporal.CopyOutput", WMTUTF8StringEncoding));
  end.waitForFence(generation.scaler->fence());
  wmtcmd_blit_copy_from_texture_to_texture copy{};
  copy.type = WMTBlitCommandCopyFromTextureToTexture;
  copy.next.set(nullptr);
  copy.src = frame->scratch_output;
  copy.src_size = {frame->output_width, frame->output_height, 1};
  copy.dst = frame->output.texture();
  copy.dst_origin = {frame->output_x, frame->output_y, 0};
  end.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
  end.updateFence(fence);
  end.endEncoding();
  cmdbuf.encodeSignalEvent(core.timeline, sequence);
  core.last_executed_generation = generation.id;
}
} // namespace dxmt
