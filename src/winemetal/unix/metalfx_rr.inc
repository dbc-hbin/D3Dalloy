// SPDX-License-Identifier: LGPL-2.1-or-later
// Included by the owned Metal4FX dispatcher. RR is a denoiser, never an SR alias.
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

@interface MTLFXTemporalDenoisedScalerDescriptor (D3DALLOYRRBackend)
- (void)setVersion:(NSUInteger)version;
@end

namespace d3dalloy_rr {
constexpr uint32_t Flag = 256;
constexpr size_t Count = 12;
struct Generation {
  const uint64_t serial;
  id<MTL4FXTemporalDenoisedScaler> scaler = nil;
  uint32_t width = 0, height = 0, outputWidth = 0, outputHeight = 0;
  bool hit = false, reactive = false;
  std::array<MTLPixelFormat, Count> formats{};
  explicit Generation(uint64_t value) : serial(value) {}
  ~Generation() { [scaler release]; }
};
struct Resources {
  std::array<id<MTLTexture>, Count> textures{};
  id<MTLBuffer> params = nil;
  id<MTL4ArgumentTable> table = nil;
  // Feature-owned scratch only; app inputs are D3D12 resources resident through
  // the device set on every D3D12 queue. Rebuilt only after scratch changes.
  id<MTLResidencySet> residency = nil;
  bool residencyDirty = true;
  ~Resources() {
    for (auto texture : textures) [texture release];
    [params release]; [table release]; [residency release];
  }
};
// Conversion pipelines depend only on the device. Features share them; the
// weak cache never extends their lifetime past the last feature, and a failed
// compilation is not cached so a later create retries.
struct Pipelines {
  id<MTLDevice> device = nil;
  id<MTLComputePipelineState> conversion = nil;
  id<MTLComputePipelineState> depthConversion = nil;
  ~Pipelines() { [conversion release]; [depthConversion release]; [device release]; }
};
struct Core {
  id<MTLDevice> device = nil;
  id<MTL4Compiler> compiler = nil;
  std::shared_ptr<const Pipelines> pipelines;
  WMTMetal4FXTemporalInfo info{};
  std::mutex mutex;
  std::array<std::shared_ptr<Generation>, 8> generations{};
  size_t nextGenerationSlot = 0;
  std::vector<std::unique_ptr<Resources>> pool;
  uint64_t lastGeneration = 0;
  float lastJitterX = 0, lastJitterY = 0;
  ~Core() { [compiler release]; [device release]; }
};
struct Params {
  uint32_t x, y, width, height, sourceWidth, sourceHeight, mode, destinationX, destinationY, padding[3];
  float motionX, motionY, jitterX, jitterY;
  float projection[16];
};
static_assert(sizeof(Params) <= 256);
const char *Kernel = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Params { uint x,y,width,height,sourceWidth,sourceHeight,mode,destinationX,destinationY,padding[3];
  float motionX,motionY,jitterX,jitterY; float4x4 projection; };
inline uint2 d3dalloy_rr_source(constant Params &p, uint2 gid) {
  return uint2(p.x,p.y) + min(uint2((float2(gid)+0.5f)*
    float2(p.sourceWidth,p.sourceHeight)/float2(p.width,p.height)), uint2(p.sourceWidth-1,p.sourceHeight-1));
}
inline float4 d3dalloy_rr_value(float4 value, constant Params &p, uint2 gid) {
  if (p.mode == 1) value = float4(value.w,0,0,1);
  if (p.mode == 2) {
    // Recover view X/Y at the pixel center with the caller's view-space Z.
    // Unlike projecting (0,0,Z), this preserves off-axis/oblique projections.
    float2 ndc = (float2(gid)+0.5f)/float2(p.width,p.height)*2.0f-1.0f;
    ndc.y = -ndc.y;
    float4x4 m = p.projection;
    float2 first = float2(m[0].x,m[1].x)-ndc.x*float2(m[0].w,m[1].w);
    float2 second = float2(m[0].y,m[1].y)-ndc.y*float2(m[0].w,m[1].w);
    float w = m[2].w*value.x+m[3].w;
    float2 rhs = ndc*w-float2(m[2].x,m[2].y)*value.x-float2(m[3].x,m[3].y);
    float determinant = first.x*second.y-first.y*second.x;
    float2 view = float2(rhs.x*second.y-first.y*rhs.y,first.x*rhs.y-rhs.x*second.x)/determinant;
    float4 clip = m * float4(view,value.x,1);
    value = float4(clip.z/clip.w,0,0,1);
  }
  if (p.mode == 3) value.xy = value.xy * float2(p.motionX,p.motionY) + float2(p.jitterX,p.jitterY);
  return value;
}
kernel void d3dalloy_rr_convert(texture2d<float,access::read> source [[texture(0)]],
  texture2d<float,access::write> destination [[texture(1)]],
  constant Params &p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) return;
  destination.write(d3dalloy_rr_value(source.read(d3dalloy_rr_source(p,gid)),p,gid),
    gid + uint2(p.destinationX,p.destinationY));
}
kernel void d3dalloy_rr_convert_depth(depth2d<float,access::read> source [[texture(0)]],
  texture2d<float,access::write> destination [[texture(1)]],
  constant Params &p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) return;
  destination.write(d3dalloy_rr_value(float4(source.read(d3dalloy_rr_source(p,gid)),0,0,1),p,gid),gid);
}
)METAL";
std::mutex pipelineMutex;
std::vector<std::weak_ptr<const Pipelines>> pipelineCache;
std::shared_ptr<const Pipelines> devicePipelines(id<MTLDevice> device) {
  std::lock_guard lock(pipelineMutex);
  std::shared_ptr<const Pipelines> found;
  std::erase_if(pipelineCache, [&](const std::weak_ptr<const Pipelines> &entry) {
    auto pipelines = entry.lock();
    if (pipelines && pipelines->device == device) found = pipelines;
    return !pipelines;
  });
  if (found) return found;
  auto result = std::make_shared<Pipelines>();
  NSError *error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:Kernel] options:nil error:&error];
  id<MTLFunction> function = [library newFunctionWithName:@"d3dalloy_rr_convert"];
  if (function) result->conversion = [device newComputePipelineStateWithFunction:function error:&error];
  [function release]; function = [library newFunctionWithName:@"d3dalloy_rr_convert_depth"];
  if (function) result->depthConversion = [device newComputePipelineStateWithFunction:function error:&error];
  [function release]; [library release];
  if (!result->conversion || !result->depthConversion) return {};
  result->device = [device retain];
  pipelineCache.push_back(result);
  return result;
}
MTLPixelFormat format(size_t i) {
  switch (i) {
  case 1: case 10: case 11: return MTLPixelFormatR32Float;
  case 2: return MTLPixelFormatRG16Float;
  case 4: case 5: return MTLPixelFormatR16Float;
  default: return MTLPixelFormatRGBA16Float;
  }
}
std::array<MTLPixelFormat, Count> fixedFormats() {
  std::array<MTLPixelFormat, Count> result{};
  for (size_t i = 0; i < Count; ++i) result[i] = format(i);
  return result;
}
// Scaler input format for slot i. App formats are bound directly only where the
// native denoiser output was measured bit-identical to the mode-0 conversion
// into format(i): exact widenings (Depth16Unorm/Depth32Float/R16Float depth,
// R16Float roughness and hit distance, RG11B10Float color) and RG32Float/RG16Snorm motion,
// which the scaler quantizes like the conversion. Sub-rects, size mismatch and
// value-changing modes (linear depth, packed roughness, scaled motion) convert.
MTLPixelFormat descriptorFormat(const Core &core, const WMTMetal4FXTemporalFrame &p, size_t i,
                                id<MTLTexture> input, const WMTMetal4FXRect &rect) {
  if (!input || rect.x || rect.y || input.width != p.input_width || input.height != p.input_height) return format(i);
  const auto value = input.pixelFormat;
  switch (i) {
  case 0: return value == MTLPixelFormatRG11B10Float ? value : format(i);
  case 1:
    return !p.depth_linear && (value == MTLPixelFormatDepth32Float || value == MTLPixelFormatDepth16Unorm ||
                               value == MTLPixelFormatR16Float) ? value : format(i);
  case 2:
    return (core.info.feature_flags & 6) == 2 && p.motion_scale_x == 1 && p.motion_scale_y == 1 &&
      (value == MTLPixelFormatRG32Float || value == MTLPixelFormatRG16Snorm) ? value : format(i);
  case 10: return !p.roughness_packed && value == MTLPixelFormatR16Float ? value : format(i);
  case 11: return value == MTLPixelFormatR16Float ? value : format(i);
  default: return format(i);
  }
}
bool readableFormat(MTLPixelFormat value) {
  switch (value) {
  case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm:
  case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Float:
  case MTLPixelFormatR32Float: case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float:
  case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatDepth24Unorm_Stencil8:
  case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm:
  case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Float:
  case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatBGRA8Unorm:
  case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRG11B10Float:
  case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Float:
  case MTLPixelFormatRGBA32Float: return true;
  default: return false;
  }
}
bool shape(id<MTLTexture> texture, id<MTLDevice> device, const WMTMetal4FXRect &r) {
  return texture && texture.device == device && texture.textureType == MTLTextureType2D &&
    texture.depth == 1 && texture.arrayLength == 1 && texture.sampleCount == 1 &&
    !texture.framebufferOnly && r.width && r.height &&
    uint64_t(r.x) + r.width <= texture.width && uint64_t(r.y) + r.height <= texture.height &&
    (texture.usage & MTLTextureUsageShaderRead) && readableFormat(texture.pixelFormat);
}
void matrix(simd_float4x4 &result, const float *input) {
  // NGX rows multiply on the left: each row becomes a SIMD column.
  // GPTK4 4.0b2 convertMatrix + EncodeDenoiseMTLFX preserve these 64 bytes.
  std::memcpy(&result,input,sizeof(result));
}
std::shared_ptr<Generation> generation(Core &core, const WMTMetal4FXTemporalFrame &p,
                                       const std::array<MTLPixelFormat, Count> &formats = fixedFormats()) {
  const bool hit = p.specular_hit_distance != 0, reactive = p.reactive != 0;
  for (const auto &cached : core.generations) {
    if (cached && cached->width == p.input_width && cached->height == p.input_height &&
        cached->outputWidth == p.output_rect.width && cached->outputHeight == p.output_rect.height &&
        cached->hit == hit && cached->reactive == reactive && cached->formats == formats) {
      // History is reset by encode order (lastGeneration), not descriptor lookup.
      return cached;
    }
  }
  static std::atomic<uint64_t> next{1};
  auto result = std::make_shared<Generation>(next.fetch_add(1, std::memory_order_relaxed));
  MTLFXTemporalDenoisedScalerDescriptor *d = [MTLFXTemporalDenoisedScalerDescriptor new];
  @try {
    d.inputWidth = p.input_width; d.inputHeight = p.input_height;
    d.outputWidth = p.output_rect.width; d.outputHeight = p.output_rect.height;
    d.colorTextureFormat = formats[0]; d.outputTextureFormat = formats[3];
    d.depthTextureFormat = formats[1]; d.motionTextureFormat = formats[2];
    d.roughnessTextureFormat = formats[10]; d.specularHitDistanceTextureFormat = formats[11];
    d.diffuseAlbedoTextureFormat = formats[7]; d.specularAlbedoTextureFormat = formats[8];
    d.normalTextureFormat = formats[9];
    d.reactiveMaskTextureEnabled = reactive; d.reactiveMaskTextureFormat = formats[5];
    d.specularHitDistanceTextureEnabled = hit;
    d.autoExposureEnabled = (core.info.feature_flags & 64) != 0;
    // Build the final model before submission instead of encoding the slower
    // interim model while its pipelines are still compiling in the background.
    d.requiresSynchronousInitialization = YES;
    // Apple non-fused GPU backend avoids the macOS 27 fused UBF K44 weight overread.
    if (![d respondsToSelector:@selector(setVersion:)]) return {};
    [d setVersion:0x10000];
    result->scaler = [d newTemporalDenoisedScalerWithDevice:core.device compiler:core.compiler];
    if (!result->scaler) return {};
    result->width = p.input_width; result->height = p.input_height;
    result->outputWidth = p.output_rect.width; result->outputHeight = p.output_rect.height;
    result->hit = hit; result->reactive = reactive; result->formats = formats;
    // Dynamic output keeps one feature across output extents. Cache only the
    // new and the last encoded output extent; prepared frames keep evictees.
    const Generation *last = nullptr;
    for (const auto &cached : core.generations)
      if (cached && cached->serial == core.lastGeneration) last = cached.get();
    for (auto &cached : core.generations) {
      if (!cached || (cached->outputWidth == result->outputWidth && cached->outputHeight == result->outputHeight) ||
          (last && cached->outputWidth == last->outputWidth && cached->outputHeight == last->outputHeight)) continue;
      cached.reset();
    }
    core.generations[core.nextGenerationSlot] = result;
    core.nextGenerationSlot = (core.nextGenerationSlot + 1) % core.generations.size();
    return result;
  } @finally { [d release]; }
}
}

@interface D3DALLOYRRFeature : NSObject {
@public std::shared_ptr<d3dalloy_rr::Core> core;
}
@end
@implementation D3DALLOYRRFeature
@end
@interface D3DALLOYRRFrame : NSObject {
@public std::shared_ptr<d3dalloy_rr::Core> core;
@public std::shared_ptr<d3dalloy_rr::Generation> generation;
@public WMTMetal4FXTemporalFrame props;
@public std::array<id<MTLTexture>, d3dalloy_rr::Count> inputs;
@public std::array<WMTMetal4FXRect, d3dalloy_rr::Count> rects;
@public std::unique_ptr<d3dalloy_rr::Resources> firstResources;
}
@end
@implementation D3DALLOYRRFrame
- (void)dealloc { for (auto input : inputs) [input release]; [super dealloc]; }
@end
@interface D3DALLOYRRExecution : NSObject {
@public std::shared_ptr<d3dalloy_rr::Core> core;
@public std::shared_ptr<d3dalloy_rr::Generation> generation;
@public std::unique_ptr<d3dalloy_rr::Resources> resources;
@public D3DALLOYRRFrame *frame;
}
@end
@implementation D3DALLOYRRExecution
- (void)dealloc {
  if (resources) {
    // Retirement runs after recording ownership has ended, including partial encode failure.
    // The retired set keeps its scratch; only the next owner mutates it.
    try {
      std::lock_guard lock(core->mutex);
      core->pool.push_back(std::move(resources));
    } catch (...) { resources.reset(); }
  }
  [frame release]; [super dealloc];
}
@end

static bool d3dalloyRRResources(D3DALLOYRRFrame *frame, d3dalloy_rr::Resources &resources,
                            std::array<id<MTLTexture>, d3dalloy_rr::Count> &bound) {
  using namespace d3dalloy_rr;
  const auto &p = frame->props;
  const auto &core = *frame->core;
  const auto scaler = frame->generation->scaler;
  const MTLTextureUsage usages[Count] = {scaler.colorTextureUsage, scaler.depthTextureUsage,
    scaler.motionTextureUsage, scaler.outputTextureUsage, MTLTextureUsageShaderRead,
    0, 0, scaler.diffuseAlbedoTextureUsage, scaler.specularAlbedoTextureUsage,
    scaler.normalTextureUsage, scaler.roughnessTextureUsage, scaler.specularHitDistanceTextureUsage};
  for (size_t i = 0; i < Count; ++i) {
    id<MTLTexture> input = frame->inputs[i]; if (!input) continue;
    const auto &rect = frame->rects[i];
    const uint32_t width = i == 3 ? p.output_rect.width : i == 4 ? 1 : p.input_width;
    const uint32_t height = i == 3 ? p.output_rect.height : i == 4 ? 1 : p.input_height;
    // Only bypass identity MV conversion; native scaling changes FP16 rounding order.
    const bool convert = (i == 1 && p.depth_linear) ||
      (i == 2 && ((core.info.feature_flags & 6) != 2 || p.motion_scale_x != 1 || p.motion_scale_y != 1)) ||
      (i == 10 && p.roughness_packed);
    MTLTextureUsage required = usages[i];
    if (i == 5) {
      if (@available(macOS 27.0, *)) required = scaler.reactiveMaskTextureUsage;
      else required = scaler.reactiveTextureUsage;
    }
    const auto descriptorFormat = frame->generation->formats[i];
    if (!convert && !rect.x && !rect.y && input.width == width && input.height == height &&
        input.pixelFormat == descriptorFormat && (input.usage & required) == required &&
        (i != 3 || input.storageMode == MTLStorageModePrivate)) { bound[i] = input; continue; }
    // Scratch is always written by conversion in the fixed format.
    if (descriptorFormat != format(i)) return false;
    auto &scratch = resources.textures[i];
    const auto usage = required | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (!scratch || scratch.width != width || scratch.height != height ||
        scratch.pixelFormat != format(i) || (scratch.usage & usage) != usage) {
      [scratch release]; scratch = nil;
      auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format(i) width:width height:height mipmapped:NO];
      descriptor.storageMode = MTLStorageModePrivate; descriptor.usage = usage;
      scratch = [core.device newTextureWithDescriptor:descriptor];
      resources.residencyDirty = true;
      if (!scratch) return false;
    }
    bound[i] = scratch;
  }
  if (!resources.params) {
    resources.params = [core.device newBufferWithLength:(Count + 1) * 256 options:MTLResourceStorageModeShared];
    resources.residencyDirty = true;
  }
  if (!resources.table) {
    auto d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 1; d.maxTextureBindCount = 2;
    resources.table = [core.device newArgumentTableWithDescriptor:d error:nil]; [d release];
  }
  if (!resources.residency) {
    auto d = [MTLResidencySetDescriptor new]; d.initialCapacity = Count * 2 + 1;
    resources.residency = [core.device newResidencySetWithDescriptor:d error:nil]; [d release];
  }
  return resources.params && resources.table && resources.residency;
}
static bool d3dalloyRRCall(struct WMTMetal4Call *call) try {
  using namespace d3dalloy_rr;
  if (call->operation == WMTMetal4FXCreateTemporal) {
    const auto &info = *reinterpret_cast<const WMTMetal4FXTemporalInfo *>(call->args[2]);
    if (!(info.feature_flags & Flag)) return false;
    call->ret[0] = 0;
    call->ret[1] = 0x887a0004;
    id<MTLDevice> device = (id<MTLDevice>)call->args[0];
    id<MTL4Compiler> compiler = (id<MTL4Compiler>)call->args[1];
    if (![MTLFXTemporalDenoisedScalerDescriptor supportsMetal4FX:device] || compiler.device != device) return true;
    if (!info.input_width || !info.input_height || !info.output_width || !info.output_height ||
        (info.feature_flags & ~(Flag | 1u | 2u | 4u | 8u | 64u))) return true;
    auto core = std::make_shared<Core>(); core->info = info;
    core->device = [device retain]; core->compiler = [compiler retain];
    core->pipelines = devicePipelines(device);
    if (!core->pipelines) { call->ret[1] = 0x8007000e; return true; }
    auto holder = [D3DALLOYRRFeature new]; holder->core = std::move(core);
    call->ret[0] = uintptr_t(holder); call->ret[1] = 0;
    return true;
  }
  if (call->operation == WMTMetal4FXDormantTemporal && [(id)call->args[0] isKindOfClass:[D3DALLOYRRFeature class]]) {
    auto &core = *((D3DALLOYRRFeature *)call->args[0])->core;
    // Release idle ownership, not history retained by recorded frames and replay.
    std::lock_guard lock(core.mutex); core.generations.fill(nullptr); core.pool.clear();
    return true;
  }
  if (call->operation == WMTMetal4FXPrepareTemporal && [(id)call->args[0] isKindOfClass:[D3DALLOYRRFeature class]]) {
    call->ret[0] = 0;
    call->ret[1] = 0x80070057;
    auto feature = (D3DALLOYRRFeature *)call->args[0];
    const auto &p = *reinterpret_cast<const WMTMetal4FXTemporalFrame *>(call->args[1]);
    if (!p.reactive && (p.reactive_rect.x || p.reactive_rect.y)) return true;
    if (!p.input_width || !p.input_height || p.input_width > feature->core->info.input_width ||
        p.input_height > feature->core->info.input_height || p.roughness_packed > 1 || p.depth_linear > 1 ||
        p.color_transfer || p.combine_composition || p.sharpening || p.cap_output || p.composition || p.debug_view ||
        !std::isfinite(p.pre_exposure) || p.pre_exposure <= 0 || !std::isfinite(p.motion_scale_x) ||
        !std::isfinite(p.motion_scale_y) || !p.motion_scale_x || !p.motion_scale_y ||
        !std::isfinite(p.jitter_x) || !std::isfinite(p.jitter_y) || p.exposure_mode > 2 ||
        (p.exposure_mode == 1 && !p.exposure) ||
        (p.exposure_mode == 2 && !(feature->core->info.feature_flags & 64)) ||
        (!feature->core->info.dynamic_output && (p.output_rect.width != feature->core->info.output_width ||
                                               p.output_rect.height != feature->core->info.output_height)) ||
        (!feature->core->info.output_subrects && (p.output_rect.x || p.output_rect.y))) return true;
    for (size_t i = 0; i < 16; ++i)
      if (!std::isfinite(p.world_to_view[i]) || !std::isfinite(p.view_to_clip[i])) return true;
    simd_float4x4 world, projection; matrix(world,p.world_to_view); matrix(projection,p.view_to_clip);
    if (simd_determinant(world) == 0 || simd_determinant(projection) == 0) return true;
    const float minScale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:feature->core->device];
    const float maxScale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:feature->core->device];
    for (double scale : {double(p.output_rect.width) / p.input_width, double(p.output_rect.height) / p.input_height})
      if (scale < minScale || scale > maxScale) { call->ret[1] = 0x887a0004; return true; }
    auto holder = [[D3DALLOYRRFrame new] autorelease]; holder->core = feature->core; holder->props = p;
    uint64_t handles[Count] = {p.color,p.depth,p.motion,p.output,p.exposure,p.reactive,0,
      p.diffuse_albedo,p.specular_albedo,p.normals,p.roughness_packed ? p.normals : p.roughness,p.specular_hit_distance};
    holder->rects = {p.color_rect,p.depth_rect,p.motion_rect,p.output_rect,{0,0,1,1},p.reactive_rect,{},
      p.diffuse_albedo_rect,p.specular_albedo_rect,p.normals_rect,p.roughness_packed ? p.normals_rect : p.roughness_rect,p.specular_hit_distance_rect};
    for (size_t i = 0; i < Count; ++i) {
      id<MTLTexture> texture = (id<MTLTexture>)handles[i];
      if (!texture && (i == 4 || i == 5 || i == 6 || i == 11)) continue;
      auto r = holder->rects[i];
      // Responsivity is one nonnegative scalar, not an arbitrary color channel.
      if (i == 5 && texture.pixelFormat != MTLPixelFormatR8Unorm &&
          texture.pixelFormat != MTLPixelFormatR16Unorm && texture.pixelFormat != MTLPixelFormatR16Float &&
          texture.pixelFormat != MTLPixelFormatR32Float) return true;
      if (i == 3) {
        if (!texture || texture.device != feature->core->device || texture.textureType != MTLTextureType2D ||
            texture.arrayLength != 1 || texture.sampleCount != 1 || texture.depth != 1 || texture.framebufferOnly ||
            (texture.pixelFormat != MTLPixelFormatRGBA16Float && texture.pixelFormat != MTLPixelFormatRGBA32Float &&
             texture.pixelFormat != MTLPixelFormatRGBA8Unorm && texture.pixelFormat != MTLPixelFormatBGRA8Unorm &&
             texture.pixelFormat != MTLPixelFormatRGB10A2Unorm && texture.pixelFormat != MTLPixelFormatRG11B10Float) ||
            !(texture.usage & MTLTextureUsageShaderWrite) || uint64_t(r.x) + r.width > texture.width ||
            uint64_t(r.y) + r.height > texture.height || !r.width || !r.height) return true;
      } else if (!shape(texture,feature->core->device,r) || texture == (id<MTLTexture>)p.output) {
        return true;
      }
      const bool highMotion = i == 2 && !(feature->core->info.feature_flags & 2);
      if (i != 3 && i != 4 && (r.width != (highMotion ? p.output_rect.width : p.input_width) ||
                              r.height != (highMotion ? p.output_rect.height : p.input_height))) {
        return true;
      }
      holder->inputs[i] = [texture retain];
    }
    std::array<MTLPixelFormat,Count> formats = fixedFormats();
    for (size_t i : {0u,1u,2u,10u,11u})
      formats[i] = descriptorFormat(*feature->core,p,i,holder->inputs[i],holder->rects[i]);
    std::lock_guard lock(feature->core->mutex);
    holder->generation = generation(*feature->core,p,formats);
    if (!holder->generation) { call->ret[1] = 0x887a0004; return true; }
    if (feature->core->pool.empty()) holder->firstResources = std::make_unique<Resources>();
    else { holder->firstResources = std::move(feature->core->pool.back()); feature->core->pool.pop_back(); }
    std::array<id<MTLTexture>,Count> bound{};
    if (!d3dalloyRRResources(holder,*holder->firstResources,bound)) {
      call->ret[1] = 0x8007000e; return true;
    }
    [holder retain];
    call->ret[0] = uintptr_t(holder); call->ret[1] = 0; call->ret[2] = holder->generation->serial;
    return true;
  }
  if (call->operation != WMTMetal4FXEncodeTemporal || ![(id)call->args[1] isKindOfClass:[D3DALLOYRRFrame class]]) return false;
  call->ret[1] = 0x887a0020;
  auto frame = (D3DALLOYRRFrame *)call->args[1];
  auto execution = [D3DALLOYRRExecution new]; execution->core = frame->core;
  execution->generation = frame->generation; execution->frame = [frame retain];
  winemetal_metal4_retain_resource(call->args[0],execution); [execution release];
  winemetal_metal4_retain_resource(call->args[0],(id)call->args[2]);
  auto &core = *frame->core;
  std::lock_guard lock(core.mutex);
  if (frame->firstResources) execution->resources = std::move(frame->firstResources);
  else if (core.pool.empty()) execution->resources = std::make_unique<Resources>();
  else { execution->resources = std::move(core.pool.back()); core.pool.pop_back(); }
  auto &resources = *execution->resources;
  const auto &p = frame->props;
  const bool reset = p.reset || call->args[3] || core.lastGeneration != frame->generation->serial;
  std::array<id<MTLTexture>,Count> bound{};
  if (!d3dalloyRRResources(frame,resources,bound)) { call->ret[1] = 0x8007000e; return true; }
  if (resources.residencyDirty) {
    [resources.residency removeAllAllocations];
    for (auto texture : resources.textures) if (texture) [resources.residency addAllocation:texture];
    [resources.residency addAllocation:resources.params]; [resources.residency commit];
    resources.residencyDirty = false;
  }
  for (size_t i = 0; i < Count; ++i) {
    Params values{}; const auto &r = frame->rects[i]; values.x = r.x; values.y = r.y;
    values.width = uint32_t(bound[i].width); values.height = uint32_t(bound[i].height);
    values.sourceWidth = r.width; values.sourceHeight = r.height;
    values.mode = i == 10 && p.roughness_packed ? 1 : i == 1 && p.depth_linear ? 2 : i == 2 ? 3 : 0;
    values.motionX = p.motion_scale_x; values.motionY = p.motion_scale_y;
    if (i == 2) {
      if (!(core.info.feature_flags & 2)) {
        values.motionX *= float(p.input_width) / p.output_rect.width;
        values.motionY *= float(p.input_height) / p.output_rect.height;
      }
      // Remove previous-minus-current sampling displacement from backward flow.
      if ((core.info.feature_flags & 4) && !reset) {
        values.jitterX = p.jitter_x - core.lastJitterX; values.jitterY = p.jitter_y - core.lastJitterY;
      }
    }
    simd_float4x4 projection; matrix(projection,p.view_to_clip);
    std::memcpy(values.projection,&projection,sizeof(projection));
    std::memcpy(static_cast<char *>(resources.params.contents) + i * 256,&values,sizeof(values));
  }
  // Each dispatch reads immutable parameters, including a separate output slot.
  Params outputParams{}; outputParams.width = p.output_rect.width; outputParams.height = p.output_rect.height;
  outputParams.sourceWidth = outputParams.width; outputParams.sourceHeight = outputParams.height;
  outputParams.destinationX = p.output_rect.x; outputParams.destinationY = p.output_rect.y;
  std::memcpy(static_cast<char *>(resources.params.contents) + Count * 256,&outputParams,sizeof(outputParams));
  id<MTL4CommandBuffer> command = (id<MTL4CommandBuffer>)winemetal_metal4_command_buffer(call->args[0]);
  id<MTLFence> fence = (id<MTLFence>)call->args[2];
  if (!command || command.device != core.device || !fence || fence.device != core.device) {
    call->ret[1] = 0x80070057; return true;
  }
  [command useResidencySet:resources.residency];
  // Conversions read only app inputs and write disjoint scratch, so one encoder
  // behind one fence wait needs no intra-encoder barrier.
  id<MTL4ComputeCommandEncoder> prepass = nil;
  for (size_t i = 0; i < Count; ++i) {
    if (i == 3 || !bound[i] || bound[i] == frame->inputs[i]) continue;
    if (!prepass) {
      prepass = [command computeCommandEncoder];
      if (!prepass) return true;
      [prepass waitForFence:fence beforeEncoderStages:MTLStageDispatch];
    }
    [resources.table setTexture:frame->inputs[i].gpuResourceID atIndex:0];
    [resources.table setTexture:bound[i].gpuResourceID atIndex:1];
    [resources.table setAddress:resources.params.gpuAddress + i * 256 atIndex:0];
    const auto sourceFormat = frame->inputs[i].pixelFormat;
    const bool depthSource = sourceFormat == MTLPixelFormatDepth16Unorm || sourceFormat == MTLPixelFormatDepth32Float ||
      sourceFormat == MTLPixelFormatDepth32Float_Stencil8 || sourceFormat == MTLPixelFormatDepth24Unorm_Stencil8;
    [prepass setComputePipelineState:depthSource ? core.pipelines->depthConversion : core.pipelines->conversion];
    [prepass setArgumentTable:resources.table];
    [prepass dispatchThreads:MTLSizeMake(bound[i].width,bound[i].height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
  }
  if (prepass) {
    [prepass updateFence:fence afterEncoderStages:MTLStageDispatch]; [prepass endEncoding];
  }
  auto scaler = frame->generation->scaler;
  @try {
    // Metal4FX is isolated between preprocessing and suffix segments, as for SR.
    // A recording split alone does not order the converter writes before RR.
    auto prefixBoundary = [command computeCommandEncoder];
    if (!prefixBoundary) return true;
    [prefixBoundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [prefixBoundary endEncoding];
    command = (id<MTL4CommandBuffer>)winemetal_metal4_split_command_buffer((void *)call->args[0]);
    if (!command) return true;
    [command useResidencySet:resources.residency];
    scaler.colorTexture = bound[0]; scaler.depthTexture = bound[1]; scaler.motionTexture = bound[2];
    scaler.outputTexture = bound[3]; scaler.exposureTexture = bound[4]; scaler.reactiveMaskTexture = bound[5];
    scaler.diffuseAlbedoTexture = bound[7]; scaler.specularAlbedoTexture = bound[8]; scaler.normalTexture = bound[9];
    scaler.roughnessTexture = bound[10]; scaler.specularHitDistanceTexture = bound[11];
    scaler.preExposure = p.pre_exposure; scaler.jitterOffsetX = p.jitter_x; scaler.jitterOffsetY = p.jitter_y;
    scaler.motionVectorScaleX = scaler.motionVectorScaleY = 1;
    scaler.shouldResetHistory = reset; scaler.depthReversed = (core.info.feature_flags & 8) != 0;
    simd_float4x4 world, projection; matrix(world,p.world_to_view); matrix(projection,p.view_to_clip);
    scaler.worldToViewMatrix = world; scaler.viewToClipMatrix = projection; scaler.fence = fence;
    [scaler encodeToCommandBuffer:command];
    command = (id<MTL4CommandBuffer>)winemetal_metal4_split_command_buffer((void *)call->args[0]);
    if (!command) return true;
    [command useResidencySet:resources.residency];
    auto boundary = [command computeCommandEncoder];
    if (!boundary) return true;
    [boundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    // The queue barrier covers RR in the previous segment; fence scopes are encoder-local.
    [boundary updateFence:fence afterEncoderStages:MTLStageDispatch | MTLStageBlit | MTLStageAccelerationStructure];
    [boundary endEncoding];
    if (bound[3] != frame->inputs[3]) {
      auto encoder = [command computeCommandEncoder]; if (!encoder) return true;
      if (bound[3].pixelFormat == frame->inputs[3].pixelFormat) {
        [encoder waitForFence:fence beforeEncoderStages:MTLStageBlit];
        [encoder copyFromTexture:bound[3] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
          sourceSize:MTLSizeMake(p.output_rect.width,p.output_rect.height,1) toTexture:frame->inputs[3]
          destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(p.output_rect.x,p.output_rect.y,0)];
        [encoder updateFence:fence afterEncoderStages:MTLStageBlit];
      } else {
        // Conversion also supports UAV outputs with a different channel precision.
        [resources.table setTexture:bound[3].gpuResourceID atIndex:0];
        [resources.table setTexture:frame->inputs[3].gpuResourceID atIndex:1];
        [resources.table setAddress:resources.params.gpuAddress + Count * 256 atIndex:0];
        [encoder waitForFence:fence beforeEncoderStages:MTLStageDispatch];
        [encoder setComputePipelineState:core.pipelines->conversion]; [encoder setArgumentTable:resources.table];
        [encoder dispatchThreads:MTLSizeMake(outputParams.width,outputParams.height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        [encoder updateFence:fence afterEncoderStages:MTLStageDispatch];
      }
      [encoder endEncoding];
    }
    core.lastGeneration = frame->generation->serial; core.lastJitterX = p.jitter_x; core.lastJitterY = p.jitter_y;
    call->ret[1] = 0;
  } @finally {
    scaler.colorTexture = scaler.depthTexture = scaler.motionTexture = scaler.outputTexture = nil;
    scaler.exposureTexture = scaler.reactiveMaskTexture = scaler.diffuseAlbedoTexture = scaler.specularAlbedoTexture = nil;
    scaler.normalTexture = scaler.roughnessTexture = scaler.specularHitDistanceTexture = nil; scaler.fence = nil;
  }
  return true;
} catch (...) {
  call->ret[1] = 0x8007000e;
  return true;
}
