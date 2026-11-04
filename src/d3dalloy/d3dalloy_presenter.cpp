
#include <algorithm>
#include "Metal.hpp"
#include "d3dalloy_format.hpp"
#include "d3dalloy_presenter.hpp"
#include "util_likely.hpp"


namespace d3dalloy {

Presenter::Presenter(WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count, WMT4::Compiler compiler, bool composition) :
    device_(device),
    composition_(composition),
    compiler_(compiler),
    layer_(layer),
    lib_(lib.getLibrary()),
    sample_count_(sample_count) {
  layer_.getProps(layer_props_);
  layer_props_.device = device;
  layer_props_.opaque = true;
  layer_props_.display_sync_enabled = false;
  layer_props_.framebuffer_only = false; // how strangely setting it true results in worse performance
  layer_props_.contents_scale = layer_props_.contents_scale * scale_factor;
  present_order_event_ = device.newSharedEvent();
  restart_token_ = device.newSharedEvent();

  WMTTextureInfo texture_info;
  texture_info.type = WMTTextureType2D;
  texture_info.pixel_format = WMTPixelFormatRGBA32Float;
  texture_info.usage = WMTTextureUsageShaderRead;
  texture_info.options = WMTResourceStorageModeShared;
  texture_info.width = D3DALLOY_GAMMA_CP_COUNT;
  texture_info.height = 1;
  texture_info.depth = 1;
  texture_info.mipmap_level_count = 1;
  texture_info.sample_count = 1;
  texture_info.array_length = 1;
  gamma_lut_texture_ = device.newTexture(texture_info);
}

bool
Presenter::changeLayerProperties(
    WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count,
    D3DalloyPresentAlpha alpha
) {
  bool should_invalidated = colorspace_ != colorspace;
  should_invalidated |= alpha_ != alpha;
  alpha_ = alpha;
  layer_props_.opaque = alpha == D3DalloyPresentAlpha::Opaque;
  should_invalidated |= source_format_ != format;
  // Drawable size is not a pipeline input; it is applied to the layer directly.
  const bool resized = layer_props_.drawable_width != width || layer_props_.drawable_height != height;
  source_format_ = format;
  should_invalidated |= sample_count_ != sample_count;
  sample_count_ = sample_count;
  layer_props_.drawable_height = height;
  layer_props_.drawable_width = width;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear(); // defer changes
  else if (resized)
    layer_resized_.store(true, std::memory_order_release); // drain, but keep pipelines
  else
    layer_.setProps(layer_props_);
  return should_invalidated || resized;
}

void
Presenter::drainNativeRetirements() {
  std::unique_lock lock(native_retire_mutex_);
  for (;;) {
    if (native_retirement_aborted_) {
      native_retirement_draining_ = false;
      return;
    }
    ++next_native_retirement_;
    auto next = native_retirements_.find(next_native_retirement_);
    if (next == native_retirements_.end()) {
      native_retirement_draining_ = false;
      return;
    }
    auto retirement = std::move(next->second);
    native_retirements_.erase(next);
    lock.unlock();
    retirement();
    lock.lock();
  }
}

void
Presenter::abortNativeRetirements() {
  Rc<Presenter> keep_alive(this);
  std::map<uint64_t, std::function<void()>> canceled;
  {
    std::lock_guard lock(native_retire_mutex_);
    native_retirement_aborted_ = true;
    canceled.swap(native_retirements_);
  }
  // The caller marked the device removed before aborting. These callbacks
  // cancel observer records and release held drawables/surfaces, never present.
  for (auto &[frame_id, retirement] : canceled) retirement();
}

bool
Presenter::changeLayerColorSpace(WMTColorSpace colorspace) {
  bool should_invalidated = colorspace_ != colorspace;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear();
  return should_invalidated;
}

void
Presenter::changeHDRMetadata(const WMTHDRMetadata *metadata) {
  if (metadata) {
    if (!has_hdr_metadata_)
      pso_valid.clear();
    has_hdr_metadata_ = true;
    hdr_metadata_ = *metadata;
  } else {
    if (has_hdr_metadata_) {
      has_hdr_metadata_ = false;
      pso_valid.clear();
    }
  }
}

void
Presenter::changeGammaRamp(const D3DalloyGammaRamp *gamma_ramp) {
  std::lock_guard lock(gamma_mutex_);
  if (!gamma_ramp && gamma_version_ != 0) {
    gamma_version_ = 0;
    gamma_lut_dirty_ = false;
    pso_valid.clear();
    return;
  }
  if (gamma_ramp && gamma_version_ != gamma_ramp->version) {
    // Only enabling gamma changes the pipeline. A new ramp is uploaded at the
    // next synchronization, after earlier presents stopped reading the LUT.
    if (gamma_version_ == 0)
      pso_valid.clear();
    gamma_version_ = gamma_ramp->version;
    for (uint32_t i = 0; i < D3DALLOY_GAMMA_CP_COUNT; i++) {
      gamma_lut_rgba_[i * 4 + 0] = std::clamp(gamma_ramp->red[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 1] = std::clamp(gamma_ramp->green[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 2] = std::clamp(gamma_ramp->blue[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 3] = 1.0f;
    }
    gamma_lut_dirty_ = true;
  }
}

Presenter::PresentState
Presenter::synchronizeLayerProperties(bool wait_for_gpu_completion) {
  return std::move(*synchronizeLayerPropertiesImpl(wait_for_gpu_completion, false));
}

std::optional<Presenter::PresentState>
Presenter::trySynchronizeLayerProperties() {
  return synchronizeLayerPropertiesImpl(true, true);
}

std::optional<Presenter::PresentState>
Presenter::synchronizeLayerPropertiesImpl(bool wait_for_gpu_completion, bool do_not_wait) {
  uint64_t display_setting_version = 0;
  WMTColorSpace display_colorspace = display_colorspace_;
  WMTHDRMetadata display_hdr_metadata = display_hdr_metadata_;
  WMTEDRValue display_edr_value = display_edr_value_;

  WMTQueryDisplaySettingForLayer(
      layer_.handle, &display_setting_version, &display_colorspace, &display_hdr_metadata, &display_edr_value
  );
  std::unique_lock gamma_lock(gamma_mutex_);
  const bool layer_resized = layer_resized_.load(std::memory_order_acquire);
  if (do_not_wait &&
      (display_setting_version != display_setting_version_ || !pso_valid.test() || gamma_lut_dirty_ || layer_resized) &&
      frames_completed_.load(std::memory_order_acquire) < frame_requested_)
    return std::nullopt;
  display_colorspace_ = display_colorspace;
  display_hdr_metadata_ = display_hdr_metadata;
  display_edr_value_ = display_edr_value;

  if (display_setting_version != display_setting_version_) {
    display_setting_version_ = display_setting_version;
    pso_valid.clear();
  }

  const bool composition_residual = composition_ && alpha_ == D3DalloyPresentAlpha::Premultiplied;
  // Composition converts the selected source space into extended linear sRGB.
  // Display overrides must not reinterpret the source transfer or primaries.
  auto final_colorspace = composition_ ? colorspace_
      : display_setting_version_ > 0 ? display_colorspace_ : colorspace_;
  auto is_hdr = WMT_COLORSPACE_IS_HDR(final_colorspace);
  const bool linear_composition_transport = composition_ && alpha_ != D3DalloyPresentAlpha::Opaque;
  auto hdr_metadata = !composition_residual && display_setting_version_ > 0 ? &display_hdr_metadata_
                      : has_hdr_metadata_          ? &hdr_metadata_
                                                   : nullptr;
  const bool pso_invalid = !pso_valid.test_and_set();
  // Earlier presents must stop sampling the LUT and use the old drawable size
  // before either changes. Only a pipeline-key change rebuilds pipelines.
  if (unlikely(pso_invalid || gamma_lut_dirty_ || layer_resized)) {
    if (!do_not_wait) {
      if (wait_for_gpu_completion)
        waitPresentsCompleted();
      else
        waitPresentsEncoded();
    }
    if (gamma_lut_dirty_) {
      gamma_lut_texture_.replaceRegion(
            {0, 0, 0}, {D3DALLOY_GAMMA_CP_COUNT, 1, 1}, 0, 0,
            gamma_lut_rgba_.data(),
            D3DALLOY_GAMMA_CP_COUNT * sizeof(float) * 4,
            0);
      gamma_lut_dirty_ = false;
    }
  }
  const bool gamma_enable = gamma_version_ != 0;
  gamma_lock.unlock();
  if (unlikely(layer_resized && !pso_invalid)) {
    layer_resized_.store(false, std::memory_order_relaxed);
    layer_.setProps(layer_props_);
  }
  if (unlikely(pso_invalid)) {
    layer_resized_.store(false, std::memory_order_relaxed);
    // Transparent remote exports retain signed/extended energy in both planes.
    layer_props_.pixel_format = linear_composition_transport ? WMTPixelFormatRGBA16Float : Forget_sRGB(source_format_);
    const PipelineKey key{layer_props_.pixel_format, Is_sRGBVariant(source_format_),
                          final_colorspace == WMTColorSpaceHDR_PQ, is_hdr && hdr_metadata != nullptr,
                          sample_count_ > 1, gamma_enable, composition_residual, linear_composition_transport};
    // Invalidation means a layer transition, not necessarily a new pipeline.
    if (!present_blit_ || !present_scale_ || !(key == pipeline_key_)) {
      buildRenderPipelineState(key.is_pq, key.with_hdr_metadata, key.is_ms, key.gamma_enable,
                               key.composition_residual, key.linear_composition_transport);
      pipeline_key_ = key;
    }
    layer_.setProps(layer_props_);
    layer_.setColorSpace(linear_composition_transport
        ? (is_hdr ? WMTColorSpaceHDR_scRGB : WMTColorSpaceSRGBLinear) : final_colorspace);
  }

  D3DalloyPresentMetadata metadata{};
  metadata.alpha = alpha_;
  if (linear_composition_transport)
    metadata.composition_color_space = final_colorspace == WMTColorSpaceSRGB ? 1 :
        final_colorspace == WMTColorSpaceHDR_PQ ? 3 : final_colorspace == WMTColorSpaceBT2020 ? 4 : 2;

  metadata.edr_scale = 1.0;
  metadata.max_content_luminance = 10000;
  metadata.max_display_luminance = display_edr_value_.maximum_potential_edr_color_component_value * 100;

  if (is_hdr) {
    metadata.edr_scale = display_edr_value_.maximum_edr_color_component_value /
                display_edr_value_.maximum_potential_edr_color_component_value;

    if (hdr_metadata) {
      metadata.max_content_luminance = std::max(
          {metadata.max_display_luminance, static_cast<float>(hdr_metadata->max_content_light_level),
           static_cast<float>(hdr_metadata->max_mastering_luminance),
           static_cast<float>(hdr_metadata->max_frame_average_light_level)}
      );
    }
  }

  if (final_colorspace == WMTColorSpaceHDR_scRGB)
    metadata.edr_scale *= 0.8;
  // scRGB white is 80 nits; Core Animation extended-linear white is 100 nits.
  // Core Animation owns output headroom/tone mapping, not each source plane.
  if (linear_composition_transport)
    metadata.edr_scale = final_colorspace == WMTColorSpaceHDR_scRGB ? 0.8f : 1.0f;
  // HDR10 metadata remains advisory for transparent composition. Per-plane
  // tone mapping would alter additive/source-over algebra; CA maps the sum.

  return std::optional<PresentState>(std::in_place, metadata, ++frame_requested_, this);
}

WMT::MetalDrawable
Presenter::encodeCommands(
    WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, D3DalloyPresentMetadata metadata,
    std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
    std::function<void(WMT::RenderCommandEncoder)> &&update_fences
) {
  assert(!compiler_ && "D3D12 presentation requires Metal4 encoding");
  auto drawable = layer_.nextDrawable();

  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionDontCare;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = drawable.texture();
  auto encoder = cmdbuf.renderCommandEncoder(info);
  wait_fences(encoder);
  encoder.setFragmentTexture(backbuffer, 0);
  encoder.setFragmentTexture(gamma_lut_texture_, 1);

  double width = layer_props_.drawable_width;
  double height = layer_props_.drawable_height;

  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  const auto &geometry = metadata.geometry;
  const bool full_source = geometry.source_width == 0 ||
      (geometry.source_width == backbuffer.width() && geometry.source_height == backbuffer.height());
  if (backbuffer.width() == (uint64_t)width && backbuffer.height() == (uint64_t)height &&
      full_source && geometry.rotation <= 1 && !geometry.scaling_none && !geometry.restrict_output) {
    encoder.setRenderPipelineState(present_blit_);
  } else {
    encoder.setRenderPipelineState(present_scale_);
  }
  encoder.setViewport({0, 0, width, height, 0, 1});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  update_fences(encoder);
  encoder.endEncoding();

  return drawable;
}

WMT::MetalDrawable
Presenter::encodeCommandsMetal4(
    WMT::MTL4CommandBuffer cmdbuf, WMT::Texture backbuffer, D3DalloyPresentMetadata metadata,
    WMT::Fence fence, PresentWork &work
) {
  assert(compiler_ && "Metal4 presentation requires a Metal4 compiler");
  work = {};
  WMTPresentDrawable present{};
  present.layer = layer_.handle;
  present.source = backbuffer.handle;
  MetalLayer_acquirePresentDrawable(&present);
  WMT::MetalDrawable drawable{present.drawable};
  if (!drawable) return {};
  if (!cmdbuf.useDrawable(drawable)) return {};
  WMT::Texture destination{present.texture};
  const double width = layer_props_.drawable_width;
  const double height = layer_props_.drawable_height;
  const uint64_t source_width = present.source_width, source_height = present.source_height;
  const uint64_t destination_width = present.destination_width, destination_height = present.destination_height;
  work.width = uint32_t(destination_width);
  work.height = uint32_t(destination_height);
  const auto &geometry = metadata.geometry;
  const bool full_source = geometry.source_width == 0 ||
      (geometry.source_width == source_width && geometry.source_height == source_height);
  // A top-left source crop is also an exact texel copy if its dimensions
  // match the drawable. For SCALING_NONE this also proves that the RTL offset
  // is zero and there are no uncovered background pixels.
  const bool full_clip = !geometry.restrict_output ||
      (geometry.output_clip[0] <= 0 && geometry.output_clip[1] <= 0 &&
       geometry.output_clip[2] >= width && geometry.output_clip[3] >= height);
  const bool unit_extent = geometry.source_width == 0
      ? source_width == destination_width && source_height == destination_height
      : geometry.source_width == width && geometry.source_height == height &&
        geometry.destination_width == width && geometry.destination_height == height &&
        source_width >= destination_width && source_height >= destination_height;
  const bool identity_geometry = width == double(destination_width) && height == double(destination_height) &&
      unit_extent && geometry.rotation <= 1 && full_clip;
  // An untonemapped, unit-EDR PQ10 flip has the same ST.2084 input/output
  // transfer. Its decode/encode round trip preserves all 1024 stored UNORM
  // codes. No such inference is made for floating-point or transformed color.
  const auto format = present.source_format;
  const bool copy_format = format == WMTPixelFormatRGBA8Unorm || format == WMTPixelFormatBGRA8Unorm ||
      format == WMTPixelFormatRGB10A2Unorm;
  if (!composition_ && identity_geometry && copy_format && format == present.destination_format &&
      sample_count_ == 1 && metadata.alpha == D3DalloyPresentAlpha::Opaque && metadata.edr_scale == 1.0f &&
      !metadata.composition_color_space && !pipeline_key_.source_srgb &&
      (!pipeline_key_.is_pq || format == WMTPixelFormatRGB10A2Unorm) &&
      !pipeline_key_.with_hdr_metadata && !pipeline_key_.gamma_enable) {
    wmtcmd_blit_copy_from_texture_to_texture copy{};
    copy.type = WMTBlitCommandCopyFromTextureToTexture;
    copy.src = backbuffer;
    copy.src_size = {uint32_t(width), uint32_t(height), 1};
    copy.dst = destination;
    auto encoder = cmdbuf.blitCommandEncoder();
    if (fence) encoder.waitForFence(fence);
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    if (fence) encoder.updateFence(fence);
    encoder.endEncoding();
    work.identity_copy = true;
    work.pq_code_copy = pipeline_key_.is_pq;
    return drawable;
  }
  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionDontCare;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = destination;
  if (composition_ && alpha_ == D3DalloyPresentAlpha::Premultiplied) {
    WMT::Texture residual{WMTCompositionDrawableResidualTexture(drawable.handle)};
    if (!residual) return {};
    info.colors[1].load_action = WMTLoadActionDontCare;
    info.colors[1].store_action = WMTStoreActionStore;
    info.colors[1].texture = residual;
  }
  auto encoder = cmdbuf.renderCommandEncoder(info);
  if (fence) encoder.waitForFence(fence, WMTRenderStageFragment);
  encoder.setFragmentTexture(backbuffer, 0);
  encoder.setFragmentTexture(gamma_lut_texture_, 1);
  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  if (source_width == uint64_t(width) && source_height == uint64_t(height) &&
      full_source && geometry.rotation <= 1 && !geometry.scaling_none && !geometry.restrict_output)
    encoder.setRenderPipelineState(present_blit_);
  else
    encoder.setRenderPipelineState(present_scale_);
  encoder.setViewport({0, 0, width, height, 0, 1});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  if (fence) encoder.updateFence(fence, WMTRenderStageFragment);
  encoder.endEncoding();
  return drawable;
}

void
Presenter::buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable,
                                   bool composition_residual, bool linear_composition_transport) {
  auto pool = WMT::MakeAutoreleasePool();

  uint32_t true_data = true, false_data = false;
  WMTFunctionConstant constants[8];
  constants[0].data.set(&true_data);
  constants[0].type = WMTDataTypeBool;
  constants[0].index = kPresentFCIndex_BackbufferSizeMatched;
  constants[1].data.set(is_pq ? &true_data : &false_data);
  constants[1].type = WMTDataTypeBool;
  constants[1].index = kPresentFCIndex_HDRPQ;
  constants[2].data.set(with_hdr_metadata ? &true_data : &false_data);
  constants[2].type = WMTDataTypeBool;
  constants[2].index = kPresentFCIndex_WithHDRMetadata;
  constants[3].data.set(Is_sRGBVariant(source_format_) ? &true_data : &false_data);
  constants[3].type = WMTDataTypeBool;
  constants[3].index = kPresentFCIndex_BackbufferIsSRGB;
  constants[4].data.set(is_ms ? &true_data : &false_data);
  constants[4].type = WMTDataTypeBool;
  constants[4].index = kPresentFCIndex_BackbufferIsMS;
  constants[5].data.set(gamma_enable ? &true_data : &false_data);
  constants[5].type  = WMTDataTypeBool;
  constants[5].index = kPresentFCIndex_GammaEnabled;
  constants[6].data.set(composition_residual ? &true_data : &false_data);
  constants[6].type = WMTDataTypeBool;
  constants[6].index = kPresentFCIndex_CompositionResidual;
  constants[7].data.set(linear_composition_transport ? &true_data : &false_data);
  constants[7].type = WMTDataTypeBool;
  constants[7].index = kPresentFCIndex_LinearCompositionTransport;

  // Both frontends consume the same library/constants but distinct compiler
  // domains: descriptors for Metal4, MTLFunction objects for classic D3D11.
  auto build = [&](auto library, auto compile) {
    WMT::Reference<WMT::Error> error;
    auto vs_present_quad = library.newFunction("vs_present_quad");
    auto fs_present_quad = library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);
    constants[0].data.set(&false_data);
    auto fs_present_quad_scaled =
        library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);
    WMTRenderPipelineInfo present_pipeline;
    WMT::InitializeRenderPipelineInfo(present_pipeline);
    present_pipeline.colors[0].pixel_format = layer_props_.pixel_format;
    if (composition_residual) present_pipeline.colors[1].pixel_format = layer_props_.pixel_format;
    present_pipeline.vertex_function = vs_present_quad;
    present_pipeline.fragment_function = fs_present_quad;
    present_blit_ = compile(present_pipeline, error);
    present_pipeline.fragment_function = fs_present_quad_scaled;
    present_scale_ = compile(present_pipeline, error);
  };
  if (compiler_)
    build(WMT4::Library(lib_), [&](const WMTRenderPipelineInfo &info, WMT::Error &error) {
      return compiler_.newRenderPipelineState(info, error);
    });
  else
    build(WMT::Library(lib_), [&](const WMTRenderPipelineInfo &info, WMT::Error &error) {
      return device_.newRenderPipelineState(info, error);
    });
}

} // namespace d3dalloy
