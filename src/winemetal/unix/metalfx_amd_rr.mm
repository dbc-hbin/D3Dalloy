// SPDX-License-Identifier: LGPL-2.1-or-later
// Imported from zzz-wine-yaagl-pr-runtime e9d0e50db84 d3dmetal-sidecar/fsr-rr.mm.
#include "metalfx_amd_rr.hpp"
#include "fsr-rr-kernels.inc"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <simd/simd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace d3dalloy::rr {
namespace {

class Object {
public:
    Object() = default;
    explicit Object(id value) : value_([value retain]) {}
    Object(const Object& other) : Object(other.value_) {}
    Object(Object&& other) noexcept : value_(other.value_) { other.value_ = nil; }
    Object& operator=(Object other) noexcept { std::swap(value_,other.value_); return *this; }
    ~Object() { [value_ release]; }
    id get() const { return value_; }
    explicit operator bool() const { return value_ != nil; }
private:
    id value_ = nil;
};
bool fail(Error* error, ErrorCode code, const char* message) {
    if (error) { error->code = code; error->message = message; }
    return false;
}
void clear(Error* error) { if (error) *error = {}; }

// Threadgroup edge of rr_filter and rr_neural_prepare; must match RrTileSize in fsr-rr-kernels.metal.
constexpr NSUInteger RrTileSize = 8;

// Must match the scalar-only MSL layout. Each dispatch gets distinct constants.
struct Params {
    uint32_t width, height, signal, checkerboard;
    uint32_t origin, flags, hasHistory, legacyMode;
    float normalStrength, stability, maxRadiance, clipK;
    float relaxation, depthThreshold, depthMin, depthMax;
    float motionScale[3], jitter[2], previousJitter[2], cameraDelta[3];
    float view[16], inverseProjection[16], inverseView[16];
    float lightDirection[3], lightEmission[3], lightRadius;
    float historyStrength;
    uint32_t apiVersion;
    float projection[16];
    float previousView[16];
};
constexpr NSUInteger ParameterStride = (sizeof(Params)+255)&~NSUInteger(255);
static_assert(sizeof(Params) == 460);

struct Allocation {
    Object object;
    std::shared_ptr<std::atomic<uint64_t>> total;
    uint64_t bytes;
    Allocation(id<MTLResource> resource, std::shared_ptr<std::atomic<uint64_t>> count)
        : object(resource), total(std::move(count)), bytes(resource.allocatedSize) {
        total->fetch_add(bytes,std::memory_order_relaxed);
    }
    ~Allocation() { total->fetch_sub(bytes,std::memory_order_relaxed); }
};
using Storage = std::shared_ptr<Allocation>;
struct History {
    std::weak_ptr<Feature::Impl> owner;
    uint32_t width = 0, height = 0, frame = 0;
    float jitter[2]{}, lightDirection[3]{}, lightEmission[3]{}, lightRadius = 0;
    simd_float3 cameraPosition{};
    float view[16]{};
    Storage depth, normals;
    std::array<Storage,WMTRR_SIGNAL_COUNT> signals;
    std::array<Storage,WMTRR_SIGNAL_COUNT> reconstructedAlpha;
    ~History();
};
// Per-execution constants, bindings and residency. Each in-flight execution
// owns one set; only retired sets return to the feature pool. A retired set
// keeps its committed scratch (sorted in contents) and is rebuilt only when the
// next execution's scratch differs. Caller textures are D3D12 resources made
// resident by the device set attached to every D3D12 queue.
struct ExecutionBindings {
    Storage parameters;
    Object table, residency;
    std::vector<id> contents;
};
struct NeuralGeneration {
    uint32_t width = 0, height = 0;
    std::array<Object,WMTRR_SIGNAL_COUNT> scalers;
    // Albedo for the absent lighting component. Its first execution clears it
    // on the consuming command stream; later executions only read it.
    Storage zero;
    bool zeroInitialized = false; // feature mutex; committed with successful encode state
};
bool radiance(unsigned s) { return s == 1 || s == 2 || s == 4 || s == 5; }
bool hasRadiance(uint32_t signals) {
    for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s)
        if (radiance(s) && (signals&(1u<<s))) return true;
    return false;
}
bool rgba(MTLPixelFormat f) {
    return f == MTLPixelFormatRGBA16Float || f == MTLPixelFormatRGBA32Float;
}
bool readable(MTLPixelFormat f) {
    return rgba(f) || f == MTLPixelFormatR32Float || f == MTLPixelFormatR16Float ||
        f == MTLPixelFormatR8Unorm || f == MTLPixelFormatRGBA8Unorm ||
        f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatRGB10A2Unorm;
}
bool finite(const float* values, size_t count) {
    for (size_t i=0;i<count;++i) if (!std::isfinite(values[i])) return false;
    return true;
}
// The RR kernel library and immutable PSOs depend only on the device. Features share them; the
// weak cache never extends its lifetime past the last feature, and a failed
// compilation is not cached so a later create retries.
struct KernelLibrary {
    id<MTLDevice> device = nil;
    Object library;
    mutable std::mutex pipelineMutex;
    mutable std::array<std::pair<Object,Object>,11> pipelines;
    Object getPipeline(NSString* name) const;
    ~KernelLibrary() { [device release]; }
};
std::mutex kernelLibraryMutex;
std::vector<std::weak_ptr<const KernelLibrary>> kernelLibraryCache;
std::shared_ptr<const KernelLibrary> deviceKernelLibrary(id<MTLDevice> device, NSError** nativeError) {
    std::lock_guard lock(kernelLibraryMutex);
    std::shared_ptr<const KernelLibrary> found;
    std::erase_if(kernelLibraryCache,[&](const std::weak_ptr<const KernelLibrary>& entry) {
        auto library = entry.lock();
        if (library && library->device == device) found = library;
        return !library;
    });
    if (found) return found;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kFsrRrKernelsSource]
        options:nil error:nativeError];
    if (!library) return {};
    auto result = std::make_shared<KernelLibrary>();
    result->library = Object(library); [library release];
    result->device = [device retain];
    kernelLibraryCache.push_back(result);
    return result;
}
Object pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString* name) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function) return {};
    NSError* error = nil;
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:function error:&error];
    [function release];
    Object result(state); [state release]; return result;
}
Object KernelLibrary::getPipeline(NSString* name) const {
    std::lock_guard lock(pipelineMutex);
    for (const auto& entry : pipelines)
        if ([(NSString*)entry.first.get() isEqualToString:name]) return entry.second;
    for (auto& entry : pipelines) {
        if (entry.second) continue;
        auto state = pipeline(device,(id<MTLLibrary>)library.get(),name);
        if (!state) return {};
        entry = {Object(name),std::move(state)};
        return entry.second;
    }
    return {};
}
}

struct Feature::Impl {
    Object device, compiler, snapshotPipeline, geometryPipeline, unpackPipeline, filterPipeline;
    Object neuralCommonPipeline, neuralPreparePipeline, neuralResolvePipeline, neuralResolveWritePipeline, zeroPipeline;
    Object resolvePipeline, resolveWritePipeline, historyFence;
    // Keeps the device cache entry alive while this feature exists.
    std::shared_ptr<const KernelLibrary> kernelLibrary;
    WMTRRCreateInfo info{};
    std::shared_ptr<std::atomic<uint64_t>> bytes = std::make_shared<std::atomic<uint64_t>>(0);
    std::mutex mutex;
    std::shared_ptr<History> history;
    std::shared_ptr<NeuralGeneration> neural;
    // One alternate extent avoids rebuilding A after A->B->A. These objects
    // may be reused, but a nonconsecutive history must still be reset.
    std::shared_ptr<NeuralGeneration> alternateNeural;
    bool dormant = false;
    // History textures join scratch only after the last history reference retires.
    // Both limits come from the current execution's actual texture working set.
    std::mutex scratchMutex;
    std::vector<Storage> idleTextures;
    uint64_t idleBytes = 0, idleByteLimit = 0;
    uint32_t scratchWidth = 0, scratchHeight = 0;
    size_t nextIdleTexture = 0;
    static constexpr size_t kMaxIdleBindings = 3;
    std::vector<ExecutionBindings> idleBindings;

    Storage texture(uint32_t width, uint32_t height, MTLPixelFormat format = MTLPixelFormatRGBA32Float,
        MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite) {
        {
            std::lock_guard lock(scratchMutex);
            for (auto& allocation : idleTextures) if (allocation) {
                auto cached = (id<MTLTexture>)allocation->object.get();
                if (cached.width == width && cached.height == height &&
                    cached.pixelFormat == format && cached.usage == usage) {
                    idleBytes -= allocation->bytes;
                    return std::exchange(allocation,{});
                }
            }
        }
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
            width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = usage;
        id<MTLTexture> value = [(id<MTLDevice>)device.get() newTextureWithDescriptor:descriptor];
        if (!value) return {};
        Object owner(value); [value release];
        return std::make_shared<Allocation>((id<MTLResource>)owner.get(),bytes);
    }

    // Caller holds scratchMutex. No allocation is performed during retirement.
    void recycle(Storage& allocation, uint32_t width, uint32_t height) noexcept {
        if (dormant || idleTextures.empty() || width != scratchWidth || height != scratchHeight ||
            allocation.use_count() != 1 || allocation->bytes > idleByteLimit) return;
        for (auto& slot : idleTextures) {
            if (slot && idleBytes > idleByteLimit-allocation->bytes) {
                idleBytes -= slot->bytes;
                slot.reset();
            }
        }
        auto slot = std::find_if(idleTextures.begin(),idleTextures.end(),
            [](const Storage& value) { return !value; });
        if (slot == idleTextures.end()) {
            slot = idleTextures.begin()+nextIdleTexture++%idleTextures.size();
            idleBytes -= (*slot)->bytes;
        }
        idleBytes += allocation->bytes;
        *slot = std::move(allocation);
    }
};
History::~History() {
    if (auto feature = owner.lock()) {
        std::lock_guard lock(feature->scratchMutex);
        feature->recycle(depth,width,height);
        feature->recycle(normals,width,height);
        for (auto& allocation : signals) feature->recycle(allocation,width,height);
        for (auto& allocation : reconstructedAlpha) feature->recycle(allocation,width,height);
    }
}
} // namespace d3dalloy::rr

@interface MTLFXTemporalDenoisedScalerDescriptor (D3DalloyAmdRRBackend)
- (void)setVersion:(NSUInteger)version;
@end

namespace d3dalloy::rr {
static std::shared_ptr<NeuralGeneration> makeNeuralGeneration(Feature::Impl& feature,
    uint32_t width, uint32_t height, Error* error) {
    if (@available(macOS 26.0,*)) {
        auto generation = std::make_shared<NeuralGeneration>();
        generation->width = width; generation->height = height;
        MTLFXTemporalDenoisedScalerDescriptor* descriptor = [MTLFXTemporalDenoisedScalerDescriptor new];
        @try {
            descriptor.inputWidth = descriptor.outputWidth = width;
            descriptor.inputHeight = descriptor.outputHeight = height;
            descriptor.colorTextureFormat = descriptor.outputTextureFormat = MTLPixelFormatRGBA32Float;
            descriptor.normalTextureFormat = descriptor.diffuseAlbedoTextureFormat =
                descriptor.specularAlbedoTextureFormat = MTLPixelFormatRGBA32Float;
            descriptor.depthTextureFormat = descriptor.roughnessTextureFormat = MTLPixelFormatR32Float;
            descriptor.motionTextureFormat = MTLPixelFormatRG32Float;
            descriptor.specularHitDistanceTextureFormat = MTLPixelFormatR32Float;
            descriptor.reactiveMaskTextureEnabled = YES;
            descriptor.reactiveMaskTextureFormat = MTLPixelFormatR32Float;
            descriptor.denoiseStrengthMaskTextureEnabled = YES;
            descriptor.denoiseStrengthMaskTextureFormat = MTLPixelFormatR32Float;
            descriptor.autoExposureEnabled = YES;
            // Same backend as the NGX RR path: the non-fused GPU backend avoids the macOS 27
            // fused-kernel Conv3x3Stride1U8 weight overread reported by Metal validation.
            if (![descriptor respondsToSelector:@selector(setVersion:)]) {
                fail(error,ErrorCode::UnsupportedFeature,"MetalFX RR backend selection is unavailable"); return {};
            }
            [descriptor setVersion:0x10000];
            for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) if (radiance(s) && (feature.info.signals&(1u<<s))) {
                descriptor.specularHitDistanceTextureEnabled = s == 5;
                id<MTL4FXTemporalDenoisedScaler> scaler = [descriptor
                    newTemporalDenoisedScalerWithDevice:(id<MTLDevice>)feature.device.get()
                    compiler:(id<MTL4Compiler>)feature.compiler.get()];
                generation->scalers[s] = Object(scaler); [scaler release];
                if (!generation->scalers[s]) {
                    fail(error,ErrorCode::ScalerCreationFailed,"MetalFX neural RR scaler creation failed"); return {};
                }
            }
        } @finally { [descriptor release]; }
        auto device = (id<MTLDevice>)feature.device.get();
        MTLTextureUsage zeroUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        for (const auto& value : generation->scalers) if (value) {
            auto scaler = (id<MTL4FXTemporalDenoisedScaler>)value.get();
            zeroUsage |= scaler.diffuseAlbedoTextureUsage | scaler.specularAlbedoTextureUsage;
        }
        auto zeroDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:width height:height mipmapped:NO];
        zeroDescriptor.storageMode = MTLStorageModePrivate;
        zeroDescriptor.usage = zeroUsage;
        id<MTLTexture> zero = [device newTextureWithDescriptor:zeroDescriptor];
        if (!zero) { fail(error,ErrorCode::ResourceCreationFailed,"RR zero albedo allocation failed"); return {}; }
        Object zeroOwner(zero); [zero release];
        // Private contents remain undefined until the first prefix dispatch. The
        // execution lease/residency owns this allocation and existing prefix
        // barriers order that clear before the isolated MetalFX consumers.
        generation->zero = std::make_shared<Allocation>((id<MTLResource>)zeroOwner.get(),feature.bytes);
        return generation;
    }
    return {};
}
struct PreparedFrame::Impl {
    std::shared_ptr<Feature::Impl> feature;
    WMTRRFrame frame{};
    WMTRRSettings settings{};
    std::array<Object,WMTRR_RESOURCE_COUNT> textures;
    uint32_t snapshotMask = 0;
    // Private albedo guides read by the fused neural resolve that share a root
    // with a caller output; these are staged like CPU-visible guides.
    bool aliasedAlbedo[WMTRR_RESOURCE_COUNT]{};
    Params parameters{};
};
struct ExecutionLease::Impl {
    std::shared_ptr<PreparedFrame::Impl> prepared;
    std::shared_ptr<History> previous, current;
    std::shared_ptr<NeuralGeneration> neural;
    std::vector<Storage> allocations;
    ExecutionBindings bindings;
    bool resident = false;
    std::vector<Object> objects;
    ~Impl() {
        // No command buffer uses this set after retirement; it is mutated only
        // by its next owner. The separate mutex also permits retirement during
        // an encode failure.
        if (resident) {
            if (@available(macOS 15.0,*)) {
                @try { [(id<MTLResidencySet>)bindings.residency.get() endResidency]; } @catch (id) {}
            }
        }
        auto& feature = *prepared->feature;
        std::lock_guard lock(feature.scratchMutex);
        for (auto& allocation : allocations)
            feature.recycle(allocation,prepared->frame.width,prepared->frame.height);
        // Only a complete set is reusable; partial sets are dropped.
        if (bindings.residency && bindings.parameters && bindings.table && !feature.dormant &&
            feature.idleBindings.size() < Feature::Impl::kMaxIdleBindings) {
            try { feature.idleBindings.push_back(std::move(bindings)); } catch (...) {}
        }
    }
};

bool validateSettings(const WMTRRSettings& s) noexcept {
    return std::isfinite(s.normal_strength) && s.normal_strength >= 0 && s.normal_strength <= 1 &&
        std::isfinite(s.stability_bias) && s.stability_bias >= 0 && s.stability_bias <= 1 &&
        std::isfinite(s.max_radiance) && s.max_radiance >= 0 && s.max_radiance <= 65504 &&
        std::isfinite(s.radiance_clip_std_k) && s.radiance_clip_std_k >= 0 &&
        std::isfinite(s.gaussian_relaxation) && s.gaussian_relaxation >= 0 && s.gaussian_relaxation <= 1 &&
        std::isfinite(s.disocclusion_threshold) && s.disocclusion_threshold >= 0 &&
        std::isfinite(s.debug_depth_min) && std::isfinite(s.debug_depth_max) &&
        s.debug_depth_min >= 0 && s.debug_depth_max > s.debug_depth_min &&
        std::isfinite(s.history_rejection_strength) && s.history_rejection_strength >= 0 &&
        s.history_rejection_strength <= 1;
}
Feature::Feature(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Feature::~Feature() = default;
PreparedFrame::PreparedFrame(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PreparedFrame::~PreparedFrame() = default;
ExecutionLease::ExecutionLease(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ExecutionLease::~ExecutionLease() = default;

std::shared_ptr<Feature> Feature::create(void* nativeDevice, void* nativeCompiler,
    const WMTRRCreateInfo& info, Error* error) noexcept {
    clear(error);
    try { @try {
        bool needsNeural = hasRadiance(info.signals);
        if (!nativeDevice || (needsNeural && !nativeCompiler)) {
            fail(error,ErrorCode::InvalidContext,"RR requires a Metal4 device; radiance also requires a compiler"); return {};
        }
        if ((info.version != 0x00400000 && info.version != 0x00401000 && info.version != 0x00402000) ||
            !info.width || !info.height ||
            (info.signals & ~127u) || !(info.signals & 62u) ||
            (info.checkerboard & ~info.signals) || (info.flags & ~3u)) {
            fail(error,ErrorCode::InvalidContext,"Invalid RR context contract"); return {};
        }
        if (@available(macOS 26.0,*)) {
            auto state = std::make_shared<Impl>();
            state->device = Object((id)nativeDevice); state->info = info;
            state->compiler = Object((id)nativeCompiler);
            auto device = (id<MTLDevice>)nativeDevice;
            if (needsNeural && (![MTLFXTemporalDenoisedScalerDescriptor supportsMetal4FX:device] ||
                [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device] > 1 ||
                [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device] < 1)) {
                fail(error,ErrorCode::UnsupportedFeature,"RR requires MetalFX neural denoising at 1:1 scale"); return {};
            }
            NSError* nativeError = nil;
            state->kernelLibrary = deviceKernelLibrary(device,&nativeError);
            if (!state->kernelLibrary) {
                fail(error,ErrorCode::ResourceCreationFailed,nativeError ? nativeError.localizedDescription.UTF8String : "RR shader compilation failed"); return {};
            }
            const auto& kernels = *state->kernelLibrary;
            state->snapshotPipeline = kernels.getPipeline(@"rr_snapshot");
            state->geometryPipeline = kernels.getPipeline(@"rr_geometry");
            state->unpackPipeline = kernels.getPipeline(@"rr_unpack");
            state->filterPipeline = kernels.getPipeline(@"rr_filter");
            if (needsNeural) {
                state->zeroPipeline = kernels.getPipeline(@"rr_zero_albedo");
                state->neuralCommonPipeline = kernels.getPipeline(@"rr_neural_common");
                state->neuralPreparePipeline = kernels.getPipeline(@"rr_neural_prepare");
                state->neuralResolvePipeline = kernels.getPipeline(@"rr_neural_resolve");
                state->neuralResolveWritePipeline = kernels.getPipeline(@"rr_neural_resolve_write");
            }
            state->resolvePipeline = kernels.getPipeline(@"rr_resolve");
            state->resolveWritePipeline = kernels.getPipeline(@"rr_resolve_write");
            id<MTLFence> fence = [device newFence];
            state->historyFence = Object(fence); [fence release];
            auto tileFits = [](const Object& pipeline) {
                return pipeline && ((id<MTLComputePipelineState>)pipeline.get()).maxTotalThreadsPerThreadgroup >=
                    RrTileSize*RrTileSize;
            };
            if (!state->snapshotPipeline || !state->geometryPipeline || !state->unpackPipeline ||
                !tileFits(state->filterPipeline) ||
                (needsNeural && (!state->zeroPipeline || !state->neuralCommonPipeline || !tileFits(state->neuralPreparePipeline) ||
                                 !state->neuralResolvePipeline || !state->neuralResolveWritePipeline)) ||
                !state->resolvePipeline || !state->resolveWritePipeline || !state->historyFence) {
                fail(error,ErrorCode::ResourceCreationFailed,"RR pipeline or fence allocation failed"); return {};
            }
            return std::shared_ptr<Feature>(new Feature(std::move(state)));
        }
        fail(error,ErrorCode::UnsupportedFeature,"RR requires macOS 26 Metal4");
    } @catch (NSException* e) { fail(error,ErrorCode::ResourceCreationFailed,e.reason.UTF8String); }
    } catch (...) { fail(error,ErrorCode::ResourceCreationFailed,"RR context allocation failed"); }
    return {};
}

std::shared_ptr<const PreparedFrame> Feature::prepare(const WMTRRFrame& frame,
    const TextureSet& textures, const WMTRRSettings& settings, Error* error) noexcept {
    clear(error);
    try { @try {
        if ((impl_->info.version == 0x00402000 && frame.legacy_mode) ||
            (impl_->info.version != 0x00402000 && (frame.legacy_mode < 1 || frame.legacy_mode > 3 ||
             (impl_->info.signals & ~8u) != (frame.legacy_mode == 1 ? 0x36u : frame.legacy_mode == 2 ? 0x30u : 0x20u)))) {
            fail(error,ErrorCode::InvalidFrame,"RR dispatch mode does not match its API contract"); return {};
        }
        if (!validateSettings(settings) || !frame.width || !frame.height ||
            frame.width > impl_->info.width || frame.height > impl_->info.height ||
            (impl_->info.checkerboard && (frame.width&1)) || (frame.flags&~3u) ||
            !finite(frame.motion_scale,3) || !finite(frame.jitter,2) || !finite(frame.camera_delta,3) ||
            !finite(frame.view,16) || !finite(frame.projection,16) ||
            !std::isfinite(frame.depth_min) || !std::isfinite(frame.depth_max) ||
            frame.depth_min < 0 || frame.depth_max < frame.depth_min) {
            fail(error,ErrorCode::InvalidFrame,"Invalid RR frame, bounds or settings"); return {};
        }
        if (textures.back() || frame.debug_width || frame.debug_height) {
            fail(error,ErrorCode::UnsupportedFeature,"AMD debug viewport identities and neural tensors are unavailable in this provider"); return {};
        }
        if (impl_->info.signals & 8) {
            float length2 = 0; for (float v : frame.light_direction) length2 += v*v;
            if (!finite(frame.light_direction,3) || !finite(frame.light_emission,3) ||
                !std::isfinite(length2) || length2 <= 0 ||
                !std::isfinite(frame.light_angular_radius) || frame.light_angular_radius < 0 || frame.light_angular_radius >= 1.5707963f ||
                frame.light_emission[0] < 0 || frame.light_emission[1] < 0 || frame.light_emission[2] < 0) {
                fail(error,ErrorCode::InvalidFrame,"Invalid dominant-light metadata"); return {};
            }
        }
        simd_float4x4 view, projection;
        std::memcpy(&view,frame.view,sizeof(view)); std::memcpy(&projection,frame.projection,sizeof(projection));
        float vd = simd_determinant(view), pd = simd_determinant(projection);
        if (!std::isfinite(vd) || !std::isfinite(pd) || vd == 0 || pd == 0) {
            fail(error,ErrorCode::InvalidFrame,"RR requires invertible camera matrices"); return {};
        }
        auto iv = simd_inverse(view), ip = simd_inverse(projection);
        if (!finite((const float*)&iv,16) || !finite((const float*)&ip,16)) {
            fail(error,ErrorCode::InvalidFrame,"RR camera inverse is non-finite"); return {};
        }
        auto prepared = std::make_shared<PreparedFrame::Impl>();
        prepared->feature = impl_; prepared->frame = frame; prepared->settings = settings;
        for (unsigned i=0;i<WMTRR_RESOURCE_COUNT-1;++i) {
            bool signal = i >= 5 && i < WMTRR_FUSED_ALBEDO;
            bool output = signal && ((i-5)&1);
            unsigned s = i >= 5 ? (i-5)/2 : 0;
            bool required = i < 3 || (i == 3 && (impl_->info.signals&44u)) ||
                (i == 4 && ((impl_->info.signals&26u) || frame.legacy_mode == 3)) ||
                (signal && (impl_->info.signals&(1u<<s))) ||
                (i == WMTRR_FUSED_ALBEDO && frame.legacy_mode == 3);
            if (!required && !textures[i]) continue;
            auto t = (id<MTLTexture>)textures[i];
            uint32_t width = frame.width;
            NSUInteger outputUsage = MTLTextureUsageShaderWrite;
            if (output && radiance(s) && !frame.legacy_mode && !(impl_->info.checkerboard&(1u<<s)))
                outputUsage |= MTLTextureUsageShaderRead;
            if (signal && !output && (impl_->info.checkerboard&(1u<<s))) width /= 2;
            if (!t || t.device != (id<MTLDevice>)impl_->device.get() ||
                t.textureType != MTLTextureType2D || t.sampleCount != 1 ||
                t.width < width || t.height < frame.height || !readable(t.pixelFormat) ||
                (i == 1 && !rgba(t.pixelFormat)) ||
                (i == 2 && !rgba(t.pixelFormat) && t.pixelFormat != MTLPixelFormatRGB10A2Unorm) ||
                ((i == 3 || i == 4 || i == WMTRR_FUSED_ALBEDO) && !rgba(t.pixelFormat) &&
                    t.pixelFormat != MTLPixelFormatRGBA8Unorm && t.pixelFormat != MTLPixelFormatBGRA8Unorm) ||
                (signal && radiance(s) && !rgba(t.pixelFormat)) ||
                (output && t.usage != MTLTextureUsageUnknown && (t.usage&outputUsage) != outputUsage)) {
                fail(error,ErrorCode::IncompatibleTexture,"RR needs compatible 2D textures and output usage matching shader accesses"); return {};
            }
            prepared->textures[i] = Object(t);
            if (output) for (unsigned j=6;j<i;j+=2) {
                if (textures[j] == textures[i]) {
                    fail(error,ErrorCode::IncompatibleTexture,"Distinct RR outputs cannot alias each other"); return {};
                }
            }
        }
        for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) {
            if (frame.checkerboard_origin[s] > 1 ||
                (!(impl_->info.checkerboard&(1u<<s)) && frame.checkerboard_origin[s])) {
                fail(error,ErrorCode::InvalidFrame,"Invalid RR checkerboard origin"); return {};
            }
        }
        // All filters finish before any caller output is written. Only inputs
        // needed by resolve need alias protection; texture views share a root.
        auto root = [](id<MTLTexture> texture) {
            while (texture.parentTexture) texture = texture.parentTexture;
            return texture;
        };
        std::array<id<MTLTexture>,WMTRR_SIGNAL_COUNT> outputRoots{};
        for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s)
            if (impl_->info.signals&(1u<<s)) outputRoots[s] = root((id<MTLTexture>)textures[6+2*s]);
        for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) if (impl_->info.signals&(1u<<s)) {
            auto input = (id<MTLTexture>)textures[5+2*s];
            // Repeated neighborhood reads of CPU-visible textures benefit from
            // private staging; game-private textures can be sampled directly.
            bool snapshot = !radiance(s) || (impl_->info.checkerboard&(1u<<s)) ||
                input.storageMode != MTLStorageModePrivate;
            if (!snapshot) {
                auto inputRoot = root(input);
                // Separate textures over one buffer need not have a parent
                // texture, even when their backing storage overlaps.
                snapshot = inputRoot.buffer ||
                    std::find(outputRoots.begin(),outputRoots.end(),inputRoot) != outputRoots.end();
            }
            if (snapshot) prepared->snapshotMask |= 1u<<s;
        }
        // Fused neural resolve writes one signal's output before a later
        // signal's finish reads the albedo guides; alias-protect them.
        for (unsigned i : {3u,4u,unsigned(WMTRR_FUSED_ALBEDO)}) {
            auto guide = (id<MTLTexture>)textures[i];
            if (!guide) continue;
            auto guideRoot = root(guide);
            prepared->aliasedAlbedo[i] = guideRoot.buffer ||
                std::find(outputRoots.begin(),outputRoots.end(),guideRoot) != outputRoots.end();
        }
        auto& c = prepared->parameters;
        c.width = frame.width; c.height = frame.height; c.flags = frame.flags;
        c.normalStrength = settings.normal_strength; c.stability = settings.stability_bias;
        c.maxRadiance = settings.max_radiance; c.clipK = settings.radiance_clip_std_k;
        c.relaxation = settings.gaussian_relaxation; c.depthThreshold = settings.disocclusion_threshold;
        c.depthMin = frame.depth_min; c.depthMax = frame.depth_max;
        std::memcpy(c.motionScale,frame.motion_scale,sizeof(c.motionScale));
        std::memcpy(c.jitter,frame.jitter,sizeof(c.jitter));
        std::memcpy(c.cameraDelta,frame.camera_delta,sizeof(c.cameraDelta));
        std::memcpy(c.view,frame.view,sizeof(c.view));
        std::memcpy(c.inverseView,&iv,sizeof(iv)); std::memcpy(c.inverseProjection,&ip,sizeof(ip));
        std::memcpy(c.lightDirection,frame.light_direction,sizeof(c.lightDirection));
        std::memcpy(c.lightEmission,frame.light_emission,sizeof(c.lightEmission));
        c.lightRadius = frame.light_angular_radius;
        c.legacyMode = frame.legacy_mode;
        c.historyStrength = settings.history_rejection_strength;
        c.apiVersion = impl_->info.version;
        std::memcpy(c.projection,frame.projection,sizeof(c.projection));
        return std::shared_ptr<const PreparedFrame>(new PreparedFrame(std::move(prepared)));
    } @catch (NSException* e) { fail(error,ErrorCode::InvalidFrame,e.reason.UTF8String); }
    } catch (...) { fail(error,ErrorCode::ResourceCreationFailed,"RR prepared-frame allocation failed"); }
    return {};
}

uint64_t Feature::memoryUsage() const noexcept { return impl_->bytes->load(std::memory_order_relaxed); }
void Feature::markDormant() noexcept {
    std::lock_guard lock(impl_->mutex);
    {
        std::lock_guard scratchLock(impl_->scratchMutex);
        impl_->dormant = true;
        impl_->idleTextures.clear(); impl_->idleBytes = 0;
        impl_->idleBindings.clear();
    }
    impl_->history.reset(); impl_->neural.reset(); impl_->alternateNeural.reset();
}

bool PreparedFrame::encode(void* commandBuffer, void* fence,
    std::shared_ptr<const ExecutionLease>& lease, void* (*split)(void*), void* splitContext,
    Error* error) const noexcept {
    clear(error); lease.reset();
    if (!commandBuffer || !fence || !split) return fail(error,ErrorCode::EncodeFailed,"RR requires caller command buffer, dependency fence and recording segments");
    try { @try {
        if (@available(macOS 26.0,*)) {
            auto feature = impl_->feature;
            // This mutex orders actual replay selection, not prepare/recording.
            // The application must synchronize sequential use of a context on
            // its queue(s); a CPU mutex is not cross-queue GPU synchronization.
            std::lock_guard lock(feature->mutex);
            auto execution = std::make_shared<ExecutionLease::Impl>();
            execution->prepared = impl_;
            auto previous = feature->history;
            const auto& frame = impl_->frame;
            {
                std::lock_guard scratchLock(feature->scratchMutex);
                if (feature->scratchWidth != frame.width || feature->scratchHeight != frame.height) {
                    // Idle sets would otherwise keep old-extent scratch alive.
                    feature->idleTextures.clear(); feature->idleBytes = 0; feature->idleBindings.clear();
                    feature->scratchWidth = frame.width; feature->scratchHeight = frame.height;
                }
            }
            bool reuse = previous && !(frame.flags&1) && previous->width == frame.width &&
                previous->height == frame.height && frame.frame_index == previous->frame+1u;
            if (reuse) execution->previous = previous;
            auto current = std::make_shared<History>();
            current->owner = feature;
            execution->current = current;
            current->width = frame.width; current->height = frame.height; current->frame = frame.frame_index;
            std::memcpy(current->view,frame.view,sizeof(current->view));
            std::memcpy(current->jitter,frame.jitter,sizeof(current->jitter));
            std::memcpy(current->lightDirection,frame.light_direction,sizeof(current->lightDirection));
            std::memcpy(current->lightEmission,frame.light_emission,sizeof(current->lightEmission));
            current->lightRadius = frame.light_angular_radius;
            if (reuse && frame.legacy_mode) {
                current->cameraPosition = previous->cameraPosition -
                    simd_make_float3(frame.camera_delta[0],frame.camera_delta[1],frame.camera_delta[2]);
            }
            std::shared_ptr<NeuralGeneration> generation;
            if (hasRadiance(feature->info.signals)) {
                generation = feature->neural;
                if (!generation || generation->width != frame.width || generation->height != frame.height) {
                    generation = feature->alternateNeural;
                    if (!generation || generation->width != frame.width || generation->height != frame.height)
                        generation = makeNeuralGeneration(*feature,frame.width,frame.height,error);
                    if (!generation) return false;
                }
                // A cached model is not evidence that its internal history is current.
                if (generation != feature->neural) reuse = false;
            }
            execution->neural = generation;
            auto device = (id<MTLDevice>)feature->device.get();
            std::array<Storage,5> common;
            Storage fused;
            std::array<Storage,WMTRR_SIGNAL_COUNT> snapshots;
            std::array<id<MTLTexture>,WMTRR_SIGNAL_COUNT> inputs{};
            // Depth, pixel motion, roughness and diffuse/specular albedo. The
            // world normal is the decoded guide; the zero albedo is per generation.
            std::array<Storage,6> neuralCommon;
            struct NeuralFrame { Storage color, reactive, strength, lower, upper, output, hit; };
            std::array<NeuralFrame,WMTRR_SIGNAL_COUNT> neuralFrames;
            execution->allocations.reserve(22); execution->objects.reserve(4);
            // Depth and normals outlive this execution as temporal history.
            // Motion and private albedos are read only before resolve. Stage
            // CPU-visible albedos for repeated neighborhood reads.
            for (unsigned i=0;i<5;++i) {
                auto source = (id<MTLTexture>)impl_->textures[i].get();
                if (!source || i == 1 ||
                    (i > 2 && source.storageMode == MTLStorageModePrivate && !impl_->aliasedAlbedo[i])) continue;
                common[i] = feature->texture(frame.width,frame.height,
                    i == 0 ? MTLPixelFormatR32Float : MTLPixelFormatRGBA32Float);
                if (!common[i]) return fail(error,ErrorCode::ResourceCreationFailed,"RR common snapshot allocation failed");
                execution->allocations.push_back(common[i]);
            }
            current->depth = common[0]; current->normals = common[2];
            // Signal-independent neighborhood guides: view position + depth,
            // and the decoded normal. Built once and read by every signal.
            // The decoded normal is also the neural scaler's normal input.
            MTLTextureUsage normalUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) if (generation && generation->scalers[s])
                normalUsage |= ((id<MTL4FXTemporalDenoisedScaler>)generation->scalers[s].get()).normalTextureUsage;
            Storage guide = feature->texture(frame.width,frame.height);
            Storage decoded = feature->texture(frame.width,frame.height,MTLPixelFormatRGBA32Float,normalUsage);
            if (!guide || !decoded) return fail(error,ErrorCode::ResourceCreationFailed,"RR geometry guide allocation failed");
            execution->allocations.push_back(guide); execution->allocations.push_back(decoded);
            if (frame.legacy_mode == 3 &&
                (((id<MTLTexture>)impl_->textures[WMTRR_FUSED_ALBEDO].get()).storageMode != MTLStorageModePrivate ||
                 impl_->aliasedAlbedo[WMTRR_FUSED_ALBEDO])) {
                fused = feature->texture(frame.width,frame.height);
                if (!fused) return fail(error,ErrorCode::ResourceCreationFailed,"RR fused albedo staging allocation failed");
                execution->allocations.push_back(fused);
            }
            for (unsigned s=0;s<7;++s) if (feature->info.signals&(1u<<s)) {
                inputs[s] = (id<MTLTexture>)impl_->textures[5+2*s].get();
                if (impl_->snapshotMask&(1u<<s)) {
                    snapshots[s] = feature->texture(frame.width,frame.height);
                    if (!snapshots[s]) return fail(error,ErrorCode::ResourceCreationFailed,"RR input snapshot allocation failed");
                    inputs[s] = (id<MTLTexture>)snapshots[s]->object.get();
                    execution->allocations.push_back(snapshots[s]);
                }
                current->signals[s] = feature->texture(frame.width,frame.height);
                if (!current->signals[s]) return fail(error,ErrorCode::ResourceCreationFailed,"RR independent signal allocation failed");
                execution->allocations.push_back(current->signals[s]);
            }
            std::array<MTLTextureUsage,6> commonUsage{};
            for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) if (generation && generation->scalers[s]) {
                auto scaler = (id<MTL4FXTemporalDenoisedScaler>)generation->scalers[s].get();
                commonUsage[0] |= scaler.depthTextureUsage;
                commonUsage[1] |= scaler.motionTextureUsage;
                commonUsage[2] |= scaler.normalTextureUsage;
                commonUsage[3] |= scaler.roughnessTextureUsage;
                commonUsage[4] |= scaler.diffuseAlbedoTextureUsage;
                commonUsage[5] |= scaler.specularAlbedoTextureUsage;
                auto allocate = [&](MTLPixelFormat format, MTLTextureUsage usage) {
                    auto value = feature->texture(frame.width,frame.height,format,
                        usage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
                    if (value) execution->allocations.push_back(value);
                    return value;
                };
                auto& n = neuralFrames[s];
                n.color = allocate(MTLPixelFormatRGBA32Float,scaler.colorTextureUsage);
                MTLTextureUsage reactiveUsage;
                if (@available(macOS 27.0,*)) reactiveUsage = scaler.reactiveMaskTextureUsage;
                else {
                    // macOS 26 exposes this usage under the earlier public
                    // property name; the 27 SDK renamed its typed accessor.
                    reactiveUsage = [[(id)scaler valueForKey:@"reactiveTextureUsage"] unsignedIntegerValue];
                }
                n.reactive = allocate(MTLPixelFormatR32Float,reactiveUsage);
                n.strength = allocate(MTLPixelFormatR32Float,scaler.denoiseStrengthMaskTextureUsage);
                n.lower = allocate(MTLPixelFormatRGBA32Float,MTLTextureUsageShaderRead);
                n.upper = allocate(MTLPixelFormatRGBA32Float,MTLTextureUsageShaderRead);
                n.output = allocate(MTLPixelFormatRGBA32Float,scaler.outputTextureUsage);
                if (s == 5) n.hit = allocate(MTLPixelFormatR32Float,scaler.specularHitDistanceTextureUsage);
                if (!n.color || !n.reactive || !n.strength || !n.lower || !n.upper || !n.output || (s == 5 && !n.hit))
                    return fail(error,ErrorCode::ResourceCreationFailed,"RR neural signal resource allocation failed");
                // Direct alpha is undefined for filtering but still belongs to
                // the caller. Retain it separately for history-only holes.
                if ((s == 1 || s == 2) && (feature->info.checkerboard&(1u<<s)))
                    current->reconstructedAlpha[s] = n.upper;
            }
            for (unsigned i=0;generation && i<neuralCommon.size();++i) {
                if (i == 2) continue;
                MTLPixelFormat format = i == 1 ? MTLPixelFormatRG32Float :
                    i == 0 || i == 3 ? MTLPixelFormatR32Float : MTLPixelFormatRGBA32Float;
                neuralCommon[i] = feature->texture(frame.width,frame.height,format,
                    commonUsage[i] | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
                if (!neuralCommon[i]) return fail(error,ErrorCode::ResourceCreationFailed,"RR neural common resource allocation failed");
                execution->allocations.push_back(neuralCommon[i]);
            }
            {
                std::lock_guard scratchLock(feature->scratchMutex);
                if (!feature->dormant) {
                    auto count = execution->allocations.size();
                    for (size_t i=count;i<feature->idleTextures.size();++i)
                        if (feature->idleTextures[i]) feature->idleBytes -= feature->idleTextures[i]->bytes;
                    feature->idleTextures.resize(count);
                    feature->idleByteLimit = 0;
                    for (const auto& allocation : execution->allocations)
                        feature->idleByteLimit += allocation->bytes;
                    for (auto& slot : feature->idleTextures) {
                        if (slot && feature->idleBytes > feature->idleByteLimit) {
                            feature->idleBytes -= slot->bytes;
                            slot.reset();
                        }
                    }
                }
            }
            auto& bindings = execution->bindings;
            {
                std::lock_guard scratchLock(feature->scratchMutex);
                if (!feature->idleBindings.empty()) {
                    bindings = std::move(feature->idleBindings.back());
                    feature->idleBindings.pop_back();
                }
            }
            if (!bindings.parameters) {
                id<MTLBuffer> buffer = [device newBufferWithLength:ParameterStride*8 options:MTLResourceStorageModeShared];
                Object owner(buffer); [buffer release];
                if (!buffer) return fail(error,ErrorCode::ResourceCreationFailed,"RR constant allocation failed");
                bindings.parameters = std::make_shared<Allocation>(buffer,feature->bytes);
            }
            auto parameters = (id<MTLBuffer>)bindings.parameters->object.get();
            Params base = impl_->parameters;
            std::memcpy(parameters.contents,&base,sizeof(base));
            for (unsigned s=0;s<7;++s) {
                Params c = base; c.signal = s; c.checkerboard = !!(feature->info.checkerboard&(1u<<s));
                c.origin = frame.checkerboard_origin[s]; c.hasHistory = reuse;
                if (reuse) {
                    std::memcpy(c.previousJitter,previous->jitter,sizeof(c.previousJitter));
                    std::memcpy(c.previousView,previous->view,sizeof(c.previousView));
                    if (s == 3 && (std::memcmp(previous->lightDirection,frame.light_direction,sizeof(current->lightDirection)) ||
                        std::memcmp(previous->lightEmission,frame.light_emission,sizeof(current->lightEmission)) ||
                        previous->lightRadius != frame.light_angular_radius)) c.hasHistory = 0;
                }
                std::memcpy((char*)parameters.contents+ParameterStride*(s+1),&c,sizeof(c));
            }
            using Bindings = std::array<id<MTLTexture>,20>;
            NSError* nativeError = nil;
            if (!bindings.table) {
                MTL4ArgumentTableDescriptor* td = [MTL4ArgumentTableDescriptor new];
                td.maxBufferBindCount = 1; td.maxTextureBindCount = Bindings{}.size(); td.initializeBindings = YES;
                id<MTL4ArgumentTable> created = [device newArgumentTableWithDescriptor:td error:&nativeError];
                [td release]; bindings.table = Object(created); [created release];
                if (!bindings.table) return fail(error,ErrorCode::ResourceCreationFailed,"RR argument table allocation failed");
            }
            auto table = (id<MTL4ArgumentTable>)bindings.table.get();
            if (!bindings.residency) {
                MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
                rd.initialCapacity = 48;
                id<MTLResidencySet> created = [device newResidencySetWithDescriptor:rd error:&nativeError];
                [rd release]; bindings.residency = Object(created); [created release];
                if (!bindings.residency) return fail(error,ErrorCode::ResourceCreationFailed,"RR residency allocation failed");
            }
            auto residency = (id<MTLResidencySet>)bindings.residency.get();
            std::vector<id> contents;
            contents.reserve(execution->allocations.size()+16);
            for (const auto& a : execution->allocations) contents.push_back(a->object.get());
            contents.push_back(parameters);
            if (generation) contents.push_back(generation->zero->object.get());
            if (reuse) {
                contents.push_back(previous->depth->object.get());
                contents.push_back(previous->normals->object.get());
                for (const auto& a : previous->signals) if (a) contents.push_back(a->object.get());
                for (const auto& a : previous->reconstructedAlpha) if (a) contents.push_back(a->object.get());
            }
            std::sort(contents.begin(),contents.end());
            contents.erase(std::unique(contents.begin(),contents.end()),contents.end());
            if (contents != bindings.contents) {
                bindings.contents.clear();
                [residency removeAllAllocations];
                for (id allocation : contents) [residency addAllocation:(id<MTLAllocation>)allocation];
                [residency commit];
                bindings.contents = std::move(contents);
            }
            [residency requestResidency]; execution->resident = true;
            // Publish before any GPU commands: failures retain captured objects.
            lease = std::shared_ptr<const ExecutionLease>(new ExecutionLease(execution));
            auto buffer = (id<MTL4CommandBuffer>)commandBuffer;
            [buffer useResidencySet:residency];
            id<MTL4ComputeCommandEncoder> encoder = [buffer computeCommandEncoder];
            if (!encoder) return fail(error,ErrorCode::EncodeFailed,"RR compute encoder creation failed");
            execution->objects.emplace_back(encoder);
            bool completed = false;
            @try {
                [encoder waitForFence:(id<MTLFence>)fence beforeEncoderStages:MTLStageDispatch];
                if (reuse) [encoder waitForFence:(id<MTLFence>)feature->historyFence.get() beforeEncoderStages:MTLStageDispatch];
                auto dispatch = [&](id<MTLComputePipelineState> state, unsigned parameter,
                                    const Bindings& textures, bool tiled = false) {
                    [table setAddress:parameters.gpuAddress+ParameterStride*parameter atIndex:0];
                    for (NSUInteger i=0;i<textures.size();++i)
                        [table setTexture:(textures[i] ? textures[i].gpuResourceID : MTLResourceID{}) atIndex:i];
                    [encoder setComputePipelineState:state]; [encoder setArgumentTable:table];
                    // rr_filter and rr_neural_prepare stage their 7x7 neighborhood in an
                    // RrTileSize^2 threadgroup tile filled by all threads, so every group,
                    // including the edges, must be complete.
                    if (tiled) {
                        [encoder dispatchThreadgroups:MTLSizeMake((frame.width+RrTileSize-1)/RrTileSize,
                                                                  (frame.height+RrTileSize-1)/RrTileSize,1)
                            threadsPerThreadgroup:MTLSizeMake(RrTileSize,RrTileSize,1)];
                        return;
                    }
                    NSUInteger w = std::min<NSUInteger>(8,state.threadExecutionWidth);
                    NSUInteger h = std::min<NSUInteger>(8,state.maxTotalThreadsPerThreadgroup/w);
                    [encoder dispatchThreads:MTLSizeMake(frame.width,frame.height,1)
                        threadsPerThreadgroup:MTLSizeMake(w,h,1)];
                };
                auto tex = [](const Storage& a) -> id<MTLTexture> { return a ? (id<MTLTexture>)a->object.get() : nil; };
                if (generation && !generation->zeroInitialized)
                    dispatch((id<MTLComputePipelineState>)feature->zeroPipeline.get(),0,{tex(generation->zero)});
                dispatch((id<MTLComputePipelineState>)feature->geometryPipeline.get(),0,
                    {(id<MTLTexture>)impl_->textures[0].get(),(id<MTLTexture>)impl_->textures[2].get(),
                     tex(common[0]),tex(common[2]),tex(guide),tex(decoded)});
                for (unsigned i=3;i<5;++i) if (common[i])
                    dispatch((id<MTLComputePipelineState>)feature->snapshotPipeline.get(),0,
                        {(id<MTLTexture>)impl_->textures[i].get(),tex(common[i])});
                if (fused)
                    dispatch((id<MTLComputePipelineState>)feature->snapshotPipeline.get(),0,
                        {(id<MTLTexture>)impl_->textures[WMTRR_FUSED_ALBEDO].get(),tex(fused)});
                for (unsigned s=0;s<7;++s) if (snapshots[s])
                    dispatch((id<MTLComputePipelineState>)feature->unpackPipeline.get(),s+1,
                        {(id<MTLTexture>)impl_->textures[5+2*s].get(),inputs[s]});
                [encoder barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
                auto specAlbedo = common[3] ? tex(common[3]) : impl_->textures[3] ?
                    (id<MTLTexture>)impl_->textures[3].get() : tex(common[2]);
                auto diffAlbedo = common[4] ? tex(common[4]) : impl_->textures[4] ?
                    (id<MTLTexture>)impl_->textures[4].get() : tex(common[2]);
                auto fusedAlbedo = fused ? tex(fused) : impl_->textures[WMTRR_FUSED_ALBEDO] ?
                    (id<MTLTexture>)impl_->textures[WMTRR_FUSED_ALBEDO].get() : tex(common[2]);
                if (generation)
                    dispatch((id<MTLComputePipelineState>)feature->neuralCommonPipeline.get(),0,
                        {tex(common[0]),(id<MTLTexture>)impl_->textures[1].get(),tex(common[2]),specAlbedo,diffAlbedo,
                         tex(neuralCommon[0]),tex(neuralCommon[1]),nil,tex(neuralCommon[3]),
                         tex(neuralCommon[4]),tex(neuralCommon[5])});
                for (unsigned s=0;s<7;++s) if (inputs[s]) {
                    if (radiance(s)) {
                        const auto& n = neuralFrames[s];
                        dispatch((id<MTLComputePipelineState>)feature->neuralPreparePipeline.get(),s+1,
                            {inputs[s],tex(common[0]),(id<MTLTexture>)impl_->textures[1].get(),tex(common[2]),
                             specAlbedo,diffAlbedo,fusedAlbedo,
                             tex(reuse ? previous->depth : common[0]),tex(reuse ? previous->normals : common[2]),
                             reuse ? tex(previous->signals[s]) : inputs[s],
                             tex(n.color),tex(n.reactive),tex(n.strength),tex(n.lower),tex(n.upper),
                             tex(current->signals[s]),n.hit ? tex(n.hit) : tex(n.color),
                             reuse && previous->reconstructedAlpha[s] ? tex(previous->reconstructedAlpha[s]) :
                                 reuse ? tex(previous->signals[s]) : inputs[s],
                             tex(guide),tex(decoded)},true);
                        continue;
                    }
                    dispatch((id<MTLComputePipelineState>)feature->filterPipeline.get(),s+1,
                        {inputs[s],tex(current->signals[s]),tex(common[0]),
                         (id<MTLTexture>)impl_->textures[1].get(),tex(common[2]),
                         specAlbedo,diffAlbedo,
                         reuse ? tex(previous->signals[s]) : inputs[s],
                         tex(reuse ? previous->depth : common[0]),tex(reuse ? previous->normals : common[2]),
                         fusedAlbedo,tex(guide),tex(decoded)},true);
                }
                [encoder updateFence:(id<MTLFence>)fence afterEncoderStages:MTLStageDispatch];
                [encoder endEncoding]; encoder = nil;
                simd_float4x4 view, projection, inverseProjection;
                std::memcpy(&view,frame.view,sizeof(view));
                std::memcpy(&projection,frame.projection,sizeof(projection));
                std::memcpy(&inverseProjection,base.inverseProjection,sizeof(inverseProjection));
                if (frame.legacy_mode) {
                    view.columns[3] = -simd_mul(view,simd_make_float4(current->cameraPosition,0));
                    view.columns[3].w = 1;
                }
                auto a = inverseProjection.columns[3], b = a+inverseProjection.columns[2];
                bool reversed = std::abs(a.z*b.w) > std::abs(b.z*a.w);
                // Keep each MetalFX encode in its own native segment, as SR does:
                // an external fence consumer in the same buffer can deadlock Metal4FX.
                // Queue barriers order and publish work across the segments.
                auto nextSegment = [&]() -> bool {
                    buffer = (id<MTL4CommandBuffer>)split(splitContext);
                    if (!buffer) return false;
                    [buffer useResidencySet:residency];
                    return true;
                };
                auto queueBarrier = [&]() -> bool {
                    id<MTL4ComputeCommandEncoder> boundary = [buffer computeCommandEncoder];
                    if (!boundary) return false;
                    [boundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                        visibilityOptions:MTL4VisibilityOptionDevice];
                    [boundary endEncoding];
                    return true;
                };
                if (generation && !queueBarrier())
                    return fail(error,ErrorCode::EncodeFailed,"RR neural prefix barrier creation failed");
                for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) if (generation && generation->scalers[s]) {
                    if (!nextSegment()) return fail(error,ErrorCode::EncodeFailed,"RR neural segment split failed");
                    auto scaler = (id<MTL4FXTemporalDenoisedScaler>)generation->scalers[s].get();
                    const auto& n = neuralFrames[s];
                    @try {
                        scaler.colorTexture = tex(n.color); scaler.outputTexture = tex(n.output);
                        scaler.depthTexture = tex(neuralCommon[0]); scaler.motionTexture = tex(neuralCommon[1]);
                        scaler.normalTexture = tex(decoded); scaler.roughnessTexture = tex(neuralCommon[3]);
                        bool composite = frame.legacy_mode == 3;
                        scaler.diffuseAlbedoTexture = composite || s == 1 || s == 4 ? tex(neuralCommon[4]) : tex(generation->zero);
                        scaler.specularAlbedoTexture = composite || s == 2 || s == 5 ? tex(neuralCommon[5]) : tex(generation->zero);
                        scaler.reactiveMaskTexture = tex(n.reactive); scaler.denoiseStrengthMaskTexture = tex(n.strength);
                        scaler.specularHitDistanceTexture = tex(n.hit);
                        scaler.preExposure = 1;
                        scaler.motionVectorScaleX = scaler.motionVectorScaleY = 1;
                        scaler.jitterOffsetX = frame.jitter[0]; scaler.jitterOffsetY = frame.jitter[1];
                        scaler.worldToViewMatrix = view; scaler.viewToClipMatrix = projection;
                        scaler.depthReversed = reversed; scaler.shouldResetHistory = !reuse;
                        scaler.fence = (id<MTLFence>)fence;
                        [scaler encodeToCommandBuffer:buffer];
                    } @finally {
                        // The execution lease owns these until GPU retirement;
                        // mutable scaler bindings must not retain old scratch.
                        scaler.colorTexture = nil; scaler.outputTexture = nil;
                        scaler.depthTexture = nil; scaler.motionTexture = nil;
                        scaler.normalTexture = nil; scaler.roughnessTexture = nil;
                        scaler.diffuseAlbedoTexture = nil; scaler.specularAlbedoTexture = nil;
                        scaler.reactiveMaskTexture = nil; scaler.denoiseStrengthMaskTexture = nil;
                        scaler.specularHitDistanceTexture = nil; scaler.fence = nil;
                    }
                }
                if (generation && (!nextSegment() || !queueBarrier()))
                    return fail(error,ErrorCode::EncodeFailed,"RR neural suffix segment creation failed");
                encoder = [buffer computeCommandEncoder];
                if (!encoder) return fail(error,ErrorCode::EncodeFailed,"RR neural correction encoder creation failed");
                execution->objects.emplace_back(encoder);
                [encoder waitForFence:(id<MTLFence>)fence beforeEncoderStages:MTLStageDispatch];
                // Each dispatch reads its own signal's working/history texture
                // at the written pixel only. Every guide a later dispatch reads
                // is internal or alias-protected, so no barrier is needed.
                for (unsigned s=0;s<7;++s) if (inputs[s]) {
                    bool readsDestination = radiance(s) && !frame.legacy_mode && !(feature->info.checkerboard&(1u<<s));
                    auto output = (id<MTLTexture>)impl_->textures[6+2*s].get();
                    if (generation && generation->scalers[s]) {
                        const auto& n = neuralFrames[s];
                        id pipeline = readsDestination ?
                            feature->neuralResolvePipeline.get() : feature->neuralResolveWritePipeline.get();
                        dispatch((id<MTLComputePipelineState>)pipeline,s+1,
                            {tex(current->signals[s]),tex(n.output),tex(n.lower),tex(n.upper),
                             specAlbedo,diffAlbedo,fusedAlbedo,output,inputs[s],tex(common[0])});
                        continue;
                    }
                    id pipeline = readsDestination ?
                        feature->resolvePipeline.get() : feature->resolveWritePipeline.get();
                    dispatch((id<MTLComputePipelineState>)pipeline,s+1,
                        {tex(current->signals[s]),output,inputs[s],
                         !readsDestination && radiance(s) ? tex(neuralFrames[s].upper) : tex(common[0])});
                }
                [encoder updateFence:(id<MTLFence>)fence afterEncoderStages:MTLStageDispatch];
                [encoder updateFence:(id<MTLFence>)feature->historyFence.get() afterEncoderStages:MTLStageDispatch];
                completed = true;
            } @finally {
                [encoder endEncoding];
                // Partial execution cannot become the next frame's history.
                if (!completed) feature->history.reset();
            }
            if (!feature->dormant) {
                feature->history = current;
                if (feature->neural != generation) feature->alternateNeural = feature->neural;
                feature->neural = generation;
            }
            if (generation) generation->zeroInitialized = true;
            return true;
        }
        return fail(error,ErrorCode::UnsupportedFeature,"RR requires macOS 26 Metal4");
    } @catch (NSException* e) { return fail(error,ErrorCode::EncodeFailed,e.reason.UTF8String); }
    } catch (...) { return fail(error,ErrorCode::ResourceCreationFailed,"RR execution allocation failed"); }
}
} // namespace d3dalloy::rr
