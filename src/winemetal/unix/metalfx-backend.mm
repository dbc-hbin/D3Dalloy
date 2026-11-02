#import "metalfx-backend.hpp"
#include "fsr-kernels.inc"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace yaagl::pso::metalfx {
namespace {

template <typename T>
T retainObject(T object) noexcept {
    return object ? static_cast<T>([(id)object retain]) : nil;
}

template <typename T>
void releaseObject(T& object) noexcept {
    if (!object) return;
    [(id)object release];
    object = nil;
}

void clearError(Error* error) noexcept {
    if (!error) return;
    error->code = ErrorCode::None;
    error->message.clear();
}

void setError(Error* error, ErrorCode code, const char* message) noexcept {
    if (!error) return;
    error->code = code;
    try {
        error->message = message ? message : "";
    } catch (...) {
        error->message.clear();
    }
}

bool finite(float value) noexcept {
    return std::isfinite(value);
}

id<MTLTexture> asTexture(void* value) noexcept {
    return reinterpret_cast<id<MTLTexture>>(value);
}

id<MTLDevice> asDevice(void* value) noexcept {
    return reinterpret_cast<id<MTLDevice>>(value);
}

id asCompiler(void* value) noexcept {
    return reinterpret_cast<id>(value);
}

id<MTLFence> asFence(void* value) noexcept {
    return reinterpret_cast<id<MTLFence>>(value);
}

bool sameDevice(id<MTLDevice> expected, id<MTLTexture> texture) noexcept {
    return texture && texture.device == expected;
}

bool basicTextureShape(id<MTLTexture> texture) noexcept {
    if (!texture || texture.textureType != MTLTextureType2D || texture.depth != 1 ||
        texture.arrayLength != 1 || texture.sampleCount != 1 ||
        texture.mipmapLevelCount == 0 || texture.framebufferOnly) {
        return false;
    }
    return true;
}

bool rectFits(const Rect& rect, id<MTLTexture> texture) noexcept {
    if (!texture || rect.width == 0 || rect.height == 0) return false;
    const std::uint64_t right = static_cast<std::uint64_t>(rect.x) + rect.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(rect.y) + rect.height;
    return right <= texture.width && bottom <= texture.height;
}

bool hasUsage(id<MTLTexture> texture, MTLTextureUsage usage) noexcept {
    return texture && (texture.usage & usage) == usage;
}

bool exposureShaderReadable(MTLPixelFormat format) noexcept {
    switch (format) {
    case MTLPixelFormatR8Unorm:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR16Unorm:
    case MTLPixelFormatR16Snorm:
    case MTLPixelFormatR16Float:
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGBA32Float:
        return true;
    default:
        return false;
    }
}

bool requiresExposureConversion(id<MTLTexture> texture) noexcept {
    return texture && (texture.pixelFormat != MTLPixelFormatR16Float ||
                       texture.width != 1 || texture.height != 1);
}

constexpr std::uint32_t kKnownFlags = FeatureFlagIsHDR | FeatureFlagMVLowRes |
                                      FeatureFlagMVJittered | FeatureFlagDepthInverted |
                                      FeatureFlagAutoExposure | FeatureFlagAlphaUpscaling;

const char* kExposureKernel = R"METAL(
#include <metal_stdlib>
using namespace metal;

kernel void yaagl_metalfx_exposure_r_to_r16(
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<half, access::write> destination [[texture(1)]]) {
    const float exposure = source.read(uint2(0, 0)).r;
    destination.write(half4(half(exposure), half(0.0), half(0.0), half(1.0)), uint2(0, 0));
}
)METAL";

id<MTLLibrary> makeLibrary(id<MTLDevice> device, const char* sourceText) noexcept {
    @try {
        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:sourceText];
        return [device newLibraryWithSource:source options:nil error:&error];
    } @catch (id) {
        return nil;
    }
}

id<MTLComputePipelineState> makePipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                        NSString* functionName) noexcept {
    id<MTLFunction> function = nil;
    @try {
        NSError* error = nil;
        function = [library newFunctionWithName:functionName];
        if (!function) return nil;
        return [device newComputePipelineStateWithFunction:function error:&error];
    } @catch (id) {
        return nil;
    } @finally {
        [function release];
    }
}

id<MTLComputePipelineState> makeExposurePipeline(id<MTLDevice> device) noexcept {
    id<MTLLibrary> library = makeLibrary(device, kExposureKernel);
    auto pipeline = makePipeline(device, library, @"yaagl_metalfx_exposure_r_to_r16");
    [library release];
    return pipeline;
}

// Immutable libraries and pipelines depend only on the device. Keep their
// existing lazy preparation: exposure-only features need not compile FSR.
// The weak cache does not outlive its features; failed entries remain retryable.
struct FsrLibrary {
    id<MTLDevice> device = nil;
    mutable id<MTLLibrary> library = nil;
    mutable id<MTLComputePipelineState> exposure = nil;
    mutable std::mutex pipelineMutex;
    mutable std::array<std::pair<NSString*, id<MTLComputePipelineState>>, 10> pipelines{};

    bool ensureLibrary() const noexcept {
        std::lock_guard lock(pipelineMutex);
        if (!library) library = makeLibrary(device, kFsrKernelsSource);
        return library != nil;
    }

    id<MTLComputePipelineState> getExposurePipeline() const noexcept {
        std::lock_guard lock(pipelineMutex);
        if (!exposure) exposure = makeExposurePipeline(device);
        return retainObject(exposure);
    }

    id<MTLComputePipelineState> getPipeline(NSString* name) const noexcept {
        std::lock_guard lock(pipelineMutex);
        for (const auto& entry : pipelines)
            if ([entry.first isEqualToString:name]) return retainObject(entry.second);
        for (auto& entry : pipelines) {
            if (entry.second) continue;
            if (!library) library = makeLibrary(device, kFsrKernelsSource);
            if (!library) return nil;
            auto pipeline = makePipeline(device, library, name);
            if (!pipeline) return nil;
            entry = {[name retain], pipeline};
            return retainObject(pipeline);
        }
        return nil;
    }

    ~FsrLibrary() {
        for (auto& entry : pipelines) {
            releaseObject(entry.first);
            releaseObject(entry.second);
        }
        releaseObject(exposure);
        releaseObject(library);
        releaseObject(device);
    }
};

std::mutex fsrLibraryMutex;
std::vector<std::weak_ptr<const FsrLibrary>> fsrLibraryCache;

std::shared_ptr<const FsrLibrary> deviceFsrLibrary(id<MTLDevice> device) noexcept {
    try {
        std::lock_guard<std::mutex> lock(fsrLibraryMutex);
        std::shared_ptr<const FsrLibrary> found;
        std::erase_if(fsrLibraryCache, [&](const std::weak_ptr<const FsrLibrary>& entry) {
            auto library = entry.lock();
            if (library && library->device == device) found = library;
            return !library;
        });
        if (found) return found;
        auto result = std::make_shared<FsrLibrary>();
        result->device = retainObject(device);
        fsrLibraryCache.push_back(result);
        return result;
    } catch (...) {
        return {};
    }
}

bool isDepthPixelFormat(MTLPixelFormat format) noexcept {
    return format == MTLPixelFormatDepth16Unorm || format == MTLPixelFormatDepth32Float ||
           format == MTLPixelFormatDepth32Float_Stencil8 ||
           // Depth24Unorm_Stencil8: deprecated name in macOS 27, wire value still valid.
           format == static_cast<MTLPixelFormat>(255);
}

bool isSrgbFormat(MTLPixelFormat format) noexcept {
    return format == MTLPixelFormatRGBA8Unorm_sRGB ||
           format == MTLPixelFormatBGRA8Unorm_sRGB;
}

MTLPixelFormat linearFormat(MTLPixelFormat format) noexcept {
    switch (format) {
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    default: return format;
    }
}

// Inverse transfer must not quantize dark linear values back into UNORM. Keep
// floating formats; 16-bit normalized channels need more mantissa than half.
MTLPixelFormat workingColorFormat(MTLPixelFormat format, ColorTransfer transfer) noexcept {
    if (transfer == ColorTransfer::Linear) return format;
    switch (linearFormat(format)) {
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatBGR10A2Unorm:
    case MTLPixelFormatRG11B10Float:
    case MTLPixelFormatRGB9E5Float:
        return MTLPixelFormatRGBA16Float;
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
        return MTLPixelFormatRGBA32Float;
    default:
        return linearFormat(format);
    }
}

// The caller's exact output format is written by the finish pass.
MTLPixelFormat workingOutputFormat(MTLPixelFormat format,
                                   const FrameOperations& operations) noexcept {
    if (operations.colorTransfer != ColorTransfer::Linear)
        return workingColorFormat(format, operations.colorTransfer);
    return operations.sharpening ? linearFormat(format) : format;
}

id<MTLTexture> makeScratchTexture(id<MTLDevice> device, id<MTLTexture> source,
                                   MTLPixelFormat format, MTLTextureUsage usage,
                                   NSString* suffix, NSUInteger width = 0,
                                   NSUInteger height = 0,
                                   bool allowGPUOptimizedContents = true) noexcept {
    id<MTLTexture> texture = nil;
    @try {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                               width:width ? width : source.width
                                                              height:height ? height : source.height
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
        descriptor.usage = usage;
        descriptor.allowGPUOptimizedContents = allowGPUOptimizedContents;
        texture = [device newTextureWithDescriptor:descriptor];
        if (texture && source.label) texture.label = [source.label stringByAppendingString:suffix];
        return texture;
    } @catch (id) {
        [texture release];
        return nil;
    }
}

id<MTLTexture> makeExposureR16(id<MTLDevice> device) noexcept {
    id<MTLTexture> texture = nil;
    @try {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        texture = [device newTextureWithDescriptor:descriptor];
        if (texture) texture.label = @"YAAGL MetalFX exposure R16F";
        return texture;
    } @catch (id) {
        [texture release];
        return nil;
    }
}

// Feature-owned scratch only. Caller textures are D3D12 resources, resident
// through the device residency set every D3D12 queue attaches at commit.
using ResidencyBindings = std::array<id, 16>;

ResidencyBindings residencyBindings(id<MTLTexture> privateOutput, id<MTLTexture> convertedExposure,
                                    id<MTLTexture> linearColor, id<MTLTexture> combinedMask,
                                    id<MTLTexture> stagedColor, id<MTLTexture> stagedDepth,
                                    id<MTLTexture> stagedMotion, id<MTLTexture> stagedReactive,
                                    id<MTLBuffer> parameterArena, id<MTLTexture> alphaColor,
                                    id<MTLTexture> alphaOutput) noexcept {
    return {{
        privateOutput, convertedExposure, linearColor,
        combinedMask, stagedColor, stagedDepth, stagedMotion, stagedReactive,
        parameterArena, alphaColor, alphaOutput,
    }};
}

bool containsBinding(const ResidencyBindings& bindings, id value) noexcept {
    return std::find(bindings.begin(), bindings.end(), value) != bindings.end();
}

// Order-insensitive equality of the non-null allocations.
bool sameBindings(const ResidencyBindings& left, const ResidencyBindings& right) noexcept {
    for (id value : left)
        if (value && !containsBinding(right, value)) return false;
    for (id value : right)
        if (value && !containsBinding(left, value)) return false;
    return true;
}

// The set belongs to a retired lease, or is new, so no committed or committing
// command buffer uses it. An unchanged set is reused without a commit. On any
// binding failure the set is dropped, never returned with partial bindings.
id makeResidencySet(id<MTLDevice> device, const ResidencyBindings& candidates,
                    ResidencyBindings& contents, id residency = nil) noexcept {
    if (residency && sameBindings(candidates, contents)) return residency;
    if (@available(macOS 15.0, *)) {
        MTLResidencySetDescriptor* descriptor = nil;
        @try {
            if (!residency) {
                descriptor = [MTLResidencySetDescriptor new];
                descriptor.initialCapacity = std::tuple_size_v<ResidencyBindings>;
                descriptor.label = @"YAAGL MetalFX execution";
                NSError* error = nil;
                residency = [device newResidencySetWithDescriptor:descriptor error:&error];
                if (!residency) return nil;
            } else {
                [reinterpret_cast<id<MTLResidencySet>>(residency) removeAllAllocations];
            }
            contents = {};
            for (std::size_t i = 0; i != std::size(candidates); ++i) {
                id candidate = candidates[i];
                if (!candidate) continue;
                bool duplicate = false;
                for (std::size_t j = 0; j != i; ++j)
                    if (candidates[j] == candidate) duplicate = true;
                if (!duplicate)
                    [residency addAllocation:reinterpret_cast<id<MTLAllocation>>(candidate)];
            }
            [residency commit];
            contents = candidates;
            return residency;
        } @catch (id) {
            [residency release];
            return nil;
        } @finally {
            [descriptor release];
        }
    }
    [residency release];
    return nil;
}

} // namespace

struct ScalerGeneration {
    static inline std::atomic<std::uint64_t> nextId{1};
    const std::uint64_t serial = nextId.fetch_add(1, std::memory_order_relaxed);
    id scaler = nil;
    id alphaScaler = nil;
    NSUInteger inputCapacityWidth = 0;
    NSUInteger inputCapacityHeight = 0;
    MTLPixelFormat colorFormat = MTLPixelFormatInvalid;
    MTLPixelFormat depthFormat = MTLPixelFormatInvalid;
    MTLPixelFormat motionFormat = MTLPixelFormatInvalid;
    MTLPixelFormat outputFormat = MTLPixelFormatInvalid;
    MTLPixelFormat reactiveFormat = MTLPixelFormatInvalid;
    bool reactiveEnabled = false;
    NSUInteger outputWidth = 0;
    NSUInteger outputHeight = 0;
    MTLTextureUsage colorUsage = MTLTextureUsageUnknown;
    MTLTextureUsage depthUsage = MTLTextureUsageUnknown;
    MTLTextureUsage motionUsage = MTLTextureUsageUnknown;
    MTLTextureUsage outputUsage = MTLTextureUsageUnknown;
    MTLTextureUsage reactiveUsage = MTLTextureUsageUnknown;
    uint64_t activation = 1; /* protected by the feature mutex */
    uint64_t encodedActivation = 0; /* protected by encodeMutex */
    bool hasLastInputContent = false;
    NSUInteger lastInputContentWidth = 0;
    NSUInteger lastInputContentHeight = 0;
    std::mutex encodeMutex;

    ~ScalerGeneration() {
        releaseObject(alphaScaler);
        releaseObject(scaler);
    }
};

bool scratchMatches(id<MTLTexture> texture, MTLPixelFormat format,
                    NSUInteger width, NSUInteger height, MTLTextureUsage usage,
                    bool allowGPUOptimizedContents) noexcept {
    @try {
        return texture && texture.pixelFormat == format && texture.width == width &&
               texture.height == height && texture.usage == usage &&
               texture.allowGPUOptimizedContents == allowGPUOptimizedContents;
    } @catch (id) {
        return false;
    }
}

NSUInteger scratchBytes(id<MTLTexture> texture) noexcept {
    @try { return texture.allocatedSize; }
    @catch (id) { return 0; }
}

// A committed set and every allocation it names travel together. Caller textures
// remain owned by PreparedFrame; only the scratch side is transferred here.
struct ParameterSlice {
    // Non-owning view into this execution's immutable parameter arena.
    id<MTLBuffer> buffer = nil;
    NSUInteger length = 0;
    void* contents = nullptr;
    uint64_t gpuAddress = 0;
    explicit operator bool() const noexcept { return buffer != nil; }
};
struct ExecutionResources {
    id<MTLTexture> alphaColor = nil;
    id<MTLTexture> alphaOutput = nil;
    id<MTLBuffer> parameterArena = nil;
    ParameterSlice alphaParams, debugParams;
    id<MTLTexture> privateOutput = nil;
    id<MTLTexture> convertedExposure = nil;
    id<MTLTexture> linearColor = nil;
    id<MTLTexture> combinedMask = nil;
    id<MTLTexture> stagedColor = nil;
    id<MTLTexture> stagedDepth = nil;
    id<MTLTexture> stagedMotion = nil;
    id<MTLTexture> stagedReactive = nil;
    ParameterSlice linearizeParams, maskParams, finishParams, copyParams;
    id residency = nil;
    // Allocations last committed to residency; a retired set keeps them.
    ResidencyBindings residencyContents{};
    // Argument tables hold resource IDs, not ownership; they are rebound on
    // every lease and recycled with the completed scratch side.
    id argumentTable = nil;
    id fsrArgumentTable = nil;

    void release() noexcept {
        releaseObject(fsrArgumentTable);
        releaseObject(argumentTable);
        releaseObject(residency);
        residencyContents = {};
        releaseObject(parameterArena);
        debugParams = {}; alphaParams = {};
        releaseObject(alphaOutput);
        releaseObject(alphaColor);
        copyParams = {}; finishParams = {}; maskParams = {}; linearizeParams = {};
        releaseObject(stagedReactive);
        releaseObject(stagedMotion);
        releaseObject(stagedDepth);
        releaseObject(stagedColor);
        releaseObject(combinedMask);
        releaseObject(linearColor);
        releaseObject(convertedExposure);
        releaseObject(privateOutput);
    }

    void swap(ExecutionResources& other) noexcept {
        std::swap(parameterArena, other.parameterArena);
        std::swap(alphaColor, other.alphaColor);
        std::swap(alphaOutput, other.alphaOutput);
        std::swap(alphaParams, other.alphaParams);
        std::swap(debugParams, other.debugParams);
        std::swap(privateOutput, other.privateOutput);
        std::swap(convertedExposure, other.convertedExposure);
        std::swap(linearColor, other.linearColor);
        std::swap(combinedMask, other.combinedMask);
        std::swap(stagedColor, other.stagedColor);
        std::swap(stagedDepth, other.stagedDepth);
        std::swap(stagedMotion, other.stagedMotion);
        std::swap(stagedReactive, other.stagedReactive);
        std::swap(linearizeParams, other.linearizeParams);
        std::swap(maskParams, other.maskParams);
        std::swap(finishParams, other.finishParams);
        std::swap(copyParams, other.copyParams);
        std::swap(residency, other.residency);
        std::swap(residencyContents, other.residencyContents);
        std::swap(argumentTable, other.argumentTable);
        std::swap(fsrArgumentTable, other.fsrArgumentTable);
    }
};

struct Feature::Impl {
    CreateInfo create{};
    id<MTLDevice> device = nil;
    id compiler = nil;
    id<MTLComputePipelineState> exposurePipeline = nil;
    id<MTLComputePipelineState> linearizePipeline = nil;
    id<MTLComputePipelineState> linearizeAlphaPipeline = nil;
    id<MTLComputePipelineState> combineMaskPipeline = nil;
    id<MTLComputePipelineState> finishPipeline = nil;
    id<MTLComputePipelineState> alphaPipeline = nil;
    id<MTLComputePipelineState> debugPipeline = nil;
    id<MTLComputePipelineState> debugDepthPipeline = nil;
    id<MTLComputePipelineState> paddingPipeline = nil;
    id<MTLComputePipelineState> copyHalfPipeline = nil;
    id<MTLComputePipelineState> copyFloatPipeline = nil;
    std::shared_ptr<const FsrLibrary> fsrLibrary;
    float minScale = 1.0f;
    float maxScale = 1.0f;
    std::shared_ptr<ScalerGeneration> currentGeneration;
    // Eviction drops cache ownership, never prepared frames or GPU leases.
    std::array<std::shared_ptr<ScalerGeneration>, 8> generations{};
    std::size_t nextGenerationSlot = 0;
    std::mutex mutex;
    // Scratch never includes caller textures. Fixed capacity bounds idle memory
    // across resolution changes and avoids allocations in completion callbacks.
    std::mutex scratchMutex;
    static constexpr NSUInteger kMaxIdleTextureBytes = 128u * 1024u * 1024u;
    std::array<id<MTLTexture>, 12> idleTextures{};
    std::array<id<MTLBuffer>, 6> idleBuffers{};
    std::array<id, 2> idleExposureTables{};
    std::array<id, 2> idleFsrTables{};
    NSUInteger idleTextureBytes = 0;
    std::size_t nextTextureSlot = 0;
    // Set once the owner drops this feature. Checked under the pool mutexes
    // so late GPU-completion returns release instead of refilling.
    std::atomic<bool> dormant{false};

    struct CachedBundle {
        ExecutionResources resources;
        const PreparedFrame::Impl* owner = nullptr;
    };
    // Completed bundles preserve same-PreparedFrame replay. Retired residency
    // sets keep their committed scratch, are bounded separately and can be
    // rebound for a different prepared frame.
    std::mutex bundleMutex;
    std::array<CachedBundle, 2> idleBundles{};
    std::array<id, 2> idleResidencies{};
    std::array<ResidencyBindings, 2> idleResidencyContents{};
    std::size_t nextBundleSlot = 0;

    // Prefer the allocations already committed to the lease's residency set,
    // so a steady frame rebinds the same scratch without another commit.
    id<MTLTexture> takeTexture(MTLPixelFormat format, NSUInteger width, NSUInteger height,
                               MTLTextureUsage usage, bool allowGPUOptimizedContents = true,
                               const ResidencyBindings* prefer = nullptr) noexcept {
        std::lock_guard<std::mutex> lock(scratchMutex);
        for (bool preferred : {true, false}) {
            if (preferred && !prefer) continue;
            for (auto& texture : idleTextures) {
                if ((!preferred || containsBinding(*prefer, texture)) &&
                    scratchMatches(texture, format, width, height, usage, allowGPUOptimizedContents)) {
                    idleTextureBytes -= scratchBytes(texture);
                    return std::exchange(texture, nil);
                }
            }
        }
        return nil;
    }
    void putTexture(id<MTLTexture>& texture) noexcept {
        if (!texture) return;
        const NSUInteger bytes = scratchBytes(texture);
        if (!bytes || bytes > kMaxIdleTextureBytes) {
            releaseObject(texture);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(scratchMutex);
            if (dormant.load(std::memory_order_relaxed)) {
                releaseObject(texture);
                return;
            }
            // A hard byte cap prevents old output resolutions accumulating GPU
            // allocations even when the fixed slot count has spare entries.
            for (auto& slot : idleTextures) {
                if (!slot || idleTextureBytes <= kMaxIdleTextureBytes - bytes) continue;
                idleTextureBytes -= scratchBytes(slot);
                releaseObject(slot);
            }
            for (auto& slot : idleTextures) {
                if (slot) continue;
                idleTextureBytes += bytes;
                slot = std::exchange(texture, nil);
                return;
            }
            // Full at the byte cap: discard the oldest slot for this completion.
            auto& slot = idleTextures[nextTextureSlot];
            idleTextureBytes -= scratchBytes(slot);
            releaseObject(slot);
            idleTextureBytes += bytes;
            slot = std::exchange(texture, nil);
            nextTextureSlot = (nextTextureSlot + 1) % idleTextures.size();
        }
    }
    id<MTLBuffer> takeBuffer(NSUInteger length, const ResidencyBindings* prefer = nullptr) noexcept {
        std::lock_guard<std::mutex> lock(scratchMutex);
        if (prefer)
            for (auto& buffer : idleBuffers)
                if (buffer && buffer.length == length && containsBinding(*prefer, buffer))
                    return std::exchange(buffer, nil);
        for (auto& buffer : idleBuffers)
            if (buffer && buffer.length == length) return std::exchange(buffer, nil);
        return nil;
    }
    void putBuffer(id<MTLBuffer>& buffer) noexcept {
        if (!buffer) return;
        {
            std::lock_guard<std::mutex> lock(scratchMutex);
            if (!dormant.load(std::memory_order_relaxed)) {
                for (auto& slot : idleBuffers) {
                    if (slot) continue;
                    slot = std::exchange(buffer, nil);
                    return;
                }
            }
        }
        releaseObject(buffer);
    }
    template <std::size_t N>
    id takeTable(std::array<id, N>& pool) noexcept {
        std::lock_guard<std::mutex> lock(scratchMutex);
        for (auto& table : pool)
            if (table) return std::exchange(table, nil);
        return nil;
    }
    template <std::size_t N>
    void putTable(std::array<id, N>& pool, id& table) noexcept {
        if (!table) return;
        {
            std::lock_guard<std::mutex> lock(scratchMutex);
            if (!dormant.load(std::memory_order_relaxed)) {
                for (auto& slot : pool) {
                    if (slot) continue;
                    slot = std::exchange(table, nil);
                    return;
                }
            }
        }
        releaseObject(table);
    }

    void returnResources(ExecutionResources& resources) noexcept {
        // The lease is retained until GPU completion, so no command buffer uses
        // this set any more. It keeps its committed scratch: the set retains
        // those allocations, is never attached while idle, and is mutated only
        // by the next lease that takes it. Pooled scratch may meanwhile join
        // another set; residency is the union and hazards are fence-tracked.
        if (resources.residency && !dormant.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(bundleMutex);
            for (std::size_t i = 0; i != idleResidencies.size(); ++i) {
                if (idleResidencies[i] || dormant.load(std::memory_order_relaxed)) continue;
                idleResidencies[i] = std::exchange(resources.residency, nil);
                idleResidencyContents[i] = resources.residencyContents;
                break;
            }
        }
        releaseObject(resources.residency);
        resources.residencyContents = {};
        putTable(idleFsrTables, resources.fsrArgumentTable);
        putTable(idleExposureTables, resources.argumentTable);
        resources.debugParams = {}; resources.alphaParams = {};
        resources.copyParams = {}; resources.finishParams = {};
        resources.maskParams = {}; resources.linearizeParams = {};
        putBuffer(resources.parameterArena);
        putTexture(resources.alphaOutput);
        putTexture(resources.alphaColor);
        putTexture(resources.stagedReactive);
        putTexture(resources.stagedMotion);
        putTexture(resources.stagedDepth);
        putTexture(resources.stagedColor);
        putTexture(resources.combinedMask);
        putTexture(resources.linearColor);
        putTexture(resources.convertedExposure);
        putTexture(resources.privateOutput);
    }

    // An idle set keeps its last committed scratch. If all sets are still in
    // completed bundles, reclaim one before scratch acquisition; its scratch
    // returns to the pool, where the new lease prefers it. Active leases are
    // never touched.
    id takeResidency(ResidencyBindings& contents) noexcept {
        ExecutionResources displaced;
        id residency = nil;
        {
            std::lock_guard<std::mutex> lock(bundleMutex);
            for (std::size_t i = 0; i != idleResidencies.size(); ++i) {
                if (!idleResidencies[i]) continue;
                residency = std::exchange(idleResidencies[i], nil);
                contents = std::exchange(idleResidencyContents[i], {});
                break;
            }
            if (!residency) {
                for (auto& entry : idleBundles) {
                    if (!entry.resources.residency) continue;
                    entry.resources.swap(displaced);
                    entry.owner = nullptr;
                    residency = std::exchange(displaced.residency, nil);
                    contents = std::exchange(displaced.residencyContents, {});
                    break;
                }
            }
        }
        returnResources(displaced);
        return residency;
    }

    bool takeBundle(ExecutionResources& resources, const PreparedFrame::Impl* owner) noexcept {
        std::lock_guard<std::mutex> lock(bundleMutex);
        for (auto& entry : idleBundles) {
            if (entry.owner != owner || !entry.resources.residency) continue;
            resources.swap(entry.resources);
            entry.owner = nullptr;
            return true;
        }
        return false;
    }

    void putBundle(ExecutionResources& resources, const PreparedFrame::Impl* owner) noexcept {
        if (!resources.residency) return;
        ExecutionResources displaced;
        {
            std::lock_guard<std::mutex> lock(bundleMutex);
            // The caller releases the resources through returnResources().
            if (dormant.load(std::memory_order_relaxed)) return;
            CachedBundle* slot = nullptr;
            for (auto& entry : idleBundles) {
                if (entry.resources.residency) continue;
                slot = &entry;
                break;
            }
            if (!slot) {
                slot = &idleBundles[nextBundleSlot];
                nextBundleSlot = (nextBundleSlot + 1) % idleBundles.size();
                slot->resources.swap(displaced);
            }
            resources.swap(slot->resources);
            slot->owner = owner;
        }
        returnResources(displaced);
    }

    void discardBundles(const PreparedFrame::Impl* owner) noexcept {
        std::array<ExecutionResources, 2> displaced;
        {
            std::lock_guard<std::mutex> lock(bundleMutex);
            for (std::size_t i = 0; i != idleBundles.size(); ++i) {
                if (idleBundles[i].owner != owner) continue;
                idleBundles[i].resources.swap(displaced[i]);
                idleBundles[i].owner = nullptr;
            }
        }
        for (auto& resources : displaced) returnResources(resources);
    }

    // Pools only hold GPU-completed resources; active leases are untouched.
    // Cached generations drop too: prepared frames and leases keep their own.
    void markDormant() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            dormant.store(true);
            currentGeneration.reset();
            for (auto& generation : generations) generation.reset();
            nextGenerationSlot = 0;
        }
        std::array<ExecutionResources, 2> bundles;
        std::array<id, 2> residencies{};
        {
            std::lock_guard<std::mutex> lock(bundleMutex);
            for (std::size_t i = 0; i != idleBundles.size(); ++i) {
                idleBundles[i].resources.swap(bundles[i]);
                idleBundles[i].owner = nullptr;
            }
            residencies.swap(idleResidencies);
            idleResidencyContents = {};
        }
        for (auto& resources : bundles) resources.release();
        for (auto& set : residencies) releaseObject(set);
        std::lock_guard<std::mutex> lock(scratchMutex);
        for (auto& texture : idleTextures) releaseObject(texture);
        for (auto& buffer : idleBuffers) releaseObject(buffer);
        for (auto& table : idleExposureTables) releaseObject(table);
        for (auto& table : idleFsrTables) releaseObject(table);
        idleTextureBytes = 0;
        nextTextureSlot = 0;
    }

    ~Impl() {
        for (auto& entry : idleBundles) returnResources(entry.resources);
        for (auto& set : idleResidencies) releaseObject(set);
        for (auto& texture : idleTextures) releaseObject(texture);
        for (auto& buffer : idleBuffers) releaseObject(buffer);
        for (auto& table : idleExposureTables) releaseObject(table);
        for (auto& table : idleFsrTables) releaseObject(table);
        for (auto& generation : generations) generation.reset();
        currentGeneration.reset();
        releaseObject(copyFloatPipeline);
        releaseObject(copyHalfPipeline);
        releaseObject(paddingPipeline);
        releaseObject(debugDepthPipeline);
        releaseObject(debugPipeline);
        releaseObject(alphaPipeline);
        releaseObject(finishPipeline);
        releaseObject(combineMaskPipeline);
        releaseObject(linearizePipeline);
        releaseObject(linearizeAlphaPipeline);
        releaseObject(exposurePipeline);
        releaseObject(compiler);
        releaseObject(device);
    }
};

struct ExecutionLease::Impl : ExecutionResources {
    std::weak_ptr<Feature::Impl> feature;
    std::weak_ptr<PreparedFrame::Impl> prepared;
    std::shared_ptr<ScalerGeneration> generation;
    id<MTLFence> fence = nil;
    bool replayEligible = false;
    ~Impl() {
        releaseObject(fence);
        // The transport retains this lease through GPU completion. Only a live
        // frame may reuse its committed set and owned scratch allocations.
        if (auto owner = feature.lock()) {
            if (auto frame = prepared.lock(); replayEligible && frame)
                owner->putBundle(*this, frame.get());
            owner->returnResources(*this);
        } else {
            release();
        }
    }
};

struct PreparedFrame::Impl {
    std::shared_ptr<Feature::Impl> feature;
    std::weak_ptr<PreparedFrame::Impl> self;
    std::shared_ptr<ScalerGeneration> generation;
    FrameInfo frame{};
    uint64_t activation = 0;
    TextureSet textures{};
    FrameOperations operations{};
    id<MTLTexture> color = nil;
    id<MTLTexture> depth = nil;
    id<MTLTexture> motion = nil;
    id<MTLTexture> output = nil;
    id<MTLTexture> exposure = nil;
    id<MTLTexture> reactive = nil;
    id<MTLTexture> composition = nil;
    NSUInteger temporalOutputWidth = 0;
    NSUInteger temporalOutputHeight = 0;
    NSUInteger placementX = 0;
    NSUInteger placementY = 0;
    bool cappedOutput = false;
    bool needsOutputShadow = false;
    bool needsColorStaging = false;
    bool needsDepthStaging = false;
    bool needsMotionStaging = false;
    bool needsReactiveStaging = false;
    Rect scalerMotionRect{};
    bool needsExposureConversion = false;
    // An exact R8 composition (or identical reactive/composition) view the
    // scaler consumes directly instead of a combined copy. Aliases (does not
    // retain) the frame's reactive or composition texture.
    id<MTLTexture> directMask = nil;
    Rect directMaskRect{};
    // Copied inputs padded by compute: 1 color, 2 depth, 4 motion, 8 reactive.
    unsigned computeCopyMask = 0;
    mutable std::mutex firstLeaseMutex;
    mutable std::shared_ptr<ExecutionLease::Impl> firstLease;

    ~Impl() {
        feature->discardBundles(this);
        releaseObject(composition);
        releaseObject(reactive);
        releaseObject(exposure);
        releaseObject(output);
        releaseObject(motion);
        releaseObject(depth);
        releaseObject(color);
    }
};

namespace {

id<MTLTexture> acquireScratch(Feature::Impl& feature, id<MTLTexture> source,
                              MTLPixelFormat format, MTLTextureUsage usage,
                              NSString* suffix, NSUInteger width = 0,
                              NSUInteger height = 0,
                              bool allowGPUOptimizedContents = true,
                              const ResidencyBindings* prefer = nullptr) noexcept {
    const NSUInteger actualWidth = width ? width : source.width;
    const NSUInteger actualHeight = height ? height : source.height;
    // A pooled texture keeps the label from its creation. Relabeling every
    // acquisition would build a new string per scratch texture per frame.
    if (id<MTLTexture> texture = feature.takeTexture(format, actualWidth, actualHeight, usage,
                                                     allowGPUOptimizedContents, prefer))
        return texture;
    return makeScratchTexture(feature.device, source, format, usage, suffix,
                              actualWidth, actualHeight, allowGPUOptimizedContents);
}

id<MTLTexture> acquireExposure(Feature::Impl& feature,
                               const ResidencyBindings* prefer = nullptr) noexcept {
    id<MTLTexture> texture = feature.takeTexture(
        MTLPixelFormatR16Float, 1, 1,
        MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, true, prefer);
    return texture ? texture : makeExposureR16(feature.device);
}

bool validateCreate(const CreateContext& context, const CreateInfo& create,
                    Error* error) noexcept {
    id<MTLDevice> device = asDevice(context.device);
    if (!device || create.input.width == 0 || create.input.height == 0 ||
        create.output.width == 0 || create.output.height == 0) {
        setError(error, ErrorCode::InvalidContext, "invalid Metal device or temporal feature dimensions");
        return false;
    }
    if ((create.flags() & ~kKnownFlags) != 0) {
        setError(error, ErrorCode::UnsupportedFeature, "temporal feature contains unknown creation flags");
        return false;
    }
    @try {
        if (@available(macOS 26.0, *)) {
            id<MTL4Compiler> compiler =
                reinterpret_cast<id<MTL4Compiler>>(asCompiler(context.compiler));
            if (!compiler || compiler.device != device ||
                ![MTLFXTemporalScalerDescriptor supportsMetal4FX:device]) {
                setError(error, ErrorCode::InvalidContext,
                         "Metal4 compiler/device does not support Metal4FX temporal scaling");
                return false;
            }
        } else {
            setError(error, ErrorCode::UnsupportedFeature, "Metal4 MetalFX translation requires macOS 26 or newer");
            return false;
        }

        if (!create.lowResolutionMotionVectors() || create.jitteredMotionVectors()) {
            if (@available(macOS 27.0, *)) {
            } else {
                setError(error, ErrorCode::UnsupportedFeature,
                         "output-resolution or jittered motion vectors require the macOS 27 MetalFX API");
                return false;
            }
        }
    } @catch (id) {
        setError(error, ErrorCode::InvalidContext, "MetalFX capability query raised an exception");
        return false;
    }
    return true;
}

bool validateFrameScalars(const FrameInfo& frame, Error* error) noexcept {
    if (!finite(frame.jitterOffsetX) || !finite(frame.jitterOffsetY) ||
        !finite(frame.motionVectorScaleX) || !finite(frame.motionVectorScaleY) ||
        !finite(frame.preExposure) || frame.motionVectorScaleX == 0.0f ||
        frame.motionVectorScaleY == 0.0f || frame.preExposure <= 0.0f) {
        setError(error, ErrorCode::InvalidFrame, "MetalFX frame contains an invalid scalar");
        return false;
    }
    return true;
}

struct TemporalOutputLayout {
    NSUInteger width = 0;
    NSUInteger height = 0;
    NSUInteger placementX = 0;
    NSUInteger placementY = 0;
    bool capped = false;
};

TemporalOutputLayout temporalOutputLayout(const Feature::Impl& feature,
                                          const FrameInfo& frame,
                                          const FrameOperations& operations) noexcept {
    TemporalOutputLayout layout{frame.outputRect.width, frame.outputRect.height, 0, 0, false};
    if (!operations.capOutputToTemporalMaxScale) return layout;

    const double callerScaleX = static_cast<double>(frame.outputRect.width) /
                                static_cast<double>(frame.inputContent.width);
    const double callerScaleY = static_cast<double>(frame.outputRect.height) /
                                static_cast<double>(frame.inputContent.height);
    if (callerScaleX <= feature.maxScale && callerScaleY <= feature.maxScale) return layout;

    const double uniformScale = std::min({static_cast<double>(feature.maxScale),
                                          callerScaleX, callerScaleY});
    layout.width = static_cast<NSUInteger>(std::floor(
        static_cast<double>(frame.inputContent.width) * uniformScale));
    layout.height = static_cast<NSUInteger>(std::floor(
        static_cast<double>(frame.inputContent.height) * uniformScale));
    layout.capped = true;
    if (layout.capped) {
        layout.placementX = (frame.outputRect.width - layout.width) / 2;
        layout.placementY = (frame.outputRect.height - layout.height) / 2;
    }
    return layout;
}

bool validateFrameTextures(const Feature::Impl& feature, const FrameInfo& frame,
                           const TextureSet& set, const FrameOperations& operations,
                           Error* error) noexcept {
    id<MTLTexture> color = asTexture(set.color);
    id<MTLTexture> depth = asTexture(set.depth);
    id<MTLTexture> motion = asTexture(set.motion);
    id<MTLTexture> output = asTexture(set.output);
    id<MTLTexture> exposure = asTexture(set.exposure);
    id<MTLTexture> reactive = asTexture(set.reactive);
    id<MTLTexture> composition = asTexture(set.composition);

    if (operations.colorTransfer != ColorTransfer::Linear &&
        operations.colorTransfer != ColorTransfer::SRGB &&
        operations.colorTransfer != ColorTransfer::PQ) {
        setError(error, ErrorCode::InvalidFrame, "FSR frame requested an unknown color transfer");
        return false;
    }
    if (!finite(operations.sharpness) || operations.sharpness < 0.0f ||
        operations.sharpness > 1.0f) {
        setError(error, ErrorCode::InvalidFrame, "FSR RCAS sharpness must be in [0,1]");
        return false;
    }
    if (!operations.combineCompositionMask && composition) {
        setError(error, ErrorCode::InvalidFrame,
                 "composition texture requires composition-mask translation");
        return false;
    }

    if (feature.create.autoExposure()) {
        if (frame.exposureMode != ExposureMode::Automatic) {
            setError(error, ErrorCode::InvalidFrame,
                     "MetalFX auto-exposure feature received a non-automatic frame exposure mode");
            return false;
        }
    } else if (frame.exposureMode == ExposureMode::Automatic) {
        setError(error, ErrorCode::InvalidFrame,
                 "MetalFX frame requested automatic exposure without create-time auto-exposure");
        return false;
    }

    if (!frame.color || !frame.depth || !frame.motionVectors || !frame.output ||
        !color || !depth || !motion || !output) {
        setError(error, ErrorCode::InvalidFrame, "MetalFX frame is missing a required texture");
        return false;
    }
    for (id<MTLTexture> texture : {color, depth, motion, output}) {
        if (!basicTextureShape(texture) || !sameDevice(feature.device, texture)) {
            setError(error, ErrorCode::IncompatibleTexture,
                     "required MetalFX texture is not a compatible 2D texture on the feature device");
            return false;
        }
    }
    if (!rectFits(frame.colorRect, color) || !rectFits(frame.depthRect, depth) ||
        !rectFits(frame.motionRect, motion) || !rectFits(frame.outputRect, output)) {
        setError(error, ErrorCode::InvalidFrame, "MetalFX subrect exceeds a supplied Metal texture view");
        return false;
    }
    if (frame.inputContent.width == 0 || frame.inputContent.height == 0 ||
        (!feature.create.dynamicOutput &&
         (frame.outputRect.width != feature.create.output.width ||
          frame.outputRect.height != feature.create.output.height))) {
        setError(error, ErrorCode::InvalidFrame, "MetalFX dynamic content/output dimensions are inconsistent with the feature");
        return false;
    }
    // NGX creation dimensions are nominal; aligned/dynamic render extents may
    // grow. Texture bounds above and device scale limits below bound each frame.
    // ensureScaler grows the native descriptor capacity for the validated extent.
    if (frame.colorRect.width != frame.inputContent.width ||
        frame.colorRect.height != frame.inputContent.height ||
        frame.depthRect.width != frame.inputContent.width ||
        frame.depthRect.height != frame.inputContent.height) {
        setError(error, ErrorCode::InvalidFrame,
                 "MetalFX color/depth subrect dimensions must match the dynamic input content");
        return false;
    }
    const Extent expectedMotion = feature.create.lowResolutionMotionVectors()
        ? frame.inputContent : Extent{frame.outputRect.width, frame.outputRect.height};
    if (frame.motionRect.width != expectedMotion.width ||
        frame.motionRect.height != expectedMotion.height) {
        setError(error, ErrorCode::InvalidFrame,
                 "MetalFX motion-vector subrect dimensions do not match its creation mode");
        return false;
    }
    if (frame.reactiveMask &&
        (frame.reactiveRect.width != frame.inputContent.width ||
         frame.reactiveRect.height != frame.inputContent.height)) {
        setError(error, ErrorCode::InvalidFrame,
                 "MetalFX reactive-mask subrect dimensions must match the dynamic input content");
        return false;
    }
    if (!feature.create.outputSubrects &&
        (frame.outputRect.x != 0 || frame.outputRect.y != 0)) {
        setError(error, ErrorCode::InvalidFrame,
                 "MetalFX output subrect offset requires create-time output-subrect opt-in");
        return false;
    }
    const TemporalOutputLayout temporal = temporalOutputLayout(feature, frame, operations);
    const float scaleX = static_cast<float>(temporal.width) /
                          static_cast<float>(frame.inputContent.width);
    const float scaleY = static_cast<float>(temporal.height) /
                          static_cast<float>(frame.inputContent.height);
    if (temporal.width == 0 || temporal.height == 0 || !finite(scaleX) || !finite(scaleY) ||
        scaleX < feature.minScale || scaleX > feature.maxScale ||
        scaleY < feature.minScale || scaleY > feature.maxScale) {
        setError(error, ErrorCode::UnsupportedFeature,
                 "dynamic input scale lies outside the MetalFX device range");
        return false;
    }
    if (frame.exposureMode == ExposureMode::Texture) {
        if (!frame.exposureTexture || !exposure || !basicTextureShape(exposure) ||
            !sameDevice(feature.device, exposure) || exposure.width == 0 || exposure.height == 0) {
            setError(error, ErrorCode::IncompatibleTexture, "manual MetalFX exposure texture is invalid");
            return false;
        }
        if (!hasUsage(exposure, MTLTextureUsageShaderRead)) {
            setError(error, ErrorCode::IncompatibleTexture, "manual MetalFX exposure texture is not shader-readable");
            return false;
        }
        if (requiresExposureConversion(exposure) && !exposureShaderReadable(exposure.pixelFormat)) {
            setError(error, ErrorCode::IncompatibleTexture,
                     "manual MetalFX exposure format cannot be numerically converted to R16Float");
            return false;
        }
    } else if (exposure) {
        // Auto exposure intentionally ignores the supplied exposure texture,
        // matching the public MetalFX contract. Do not reject its D3D presence.
        exposure = nil;
    }

    if (reactive) {
        if (!frame.reactiveMask || !basicTextureShape(reactive) ||
            !sameDevice(feature.device, reactive) || !rectFits(frame.reactiveRect, reactive)) {
            setError(error, ErrorCode::IncompatibleTexture, "MetalFX bias-current-color mask texture is invalid");
            return false;
        }
        if (@available(macOS 27.0, *)) {
        } else {
            setError(error, ErrorCode::UnsupportedFeature, "reactive-mask translation requires the macOS 27 MetalFX usage contract");
            return false;
        }
    } else if (frame.reactiveMask && !operations.combineCompositionMask) {
        setError(error, ErrorCode::InvalidFrame, "MetalFX reactive mask resource was not resolved to a Metal texture");
        return false;
    }

    if (operations.combineCompositionMask) {
        if (!reactive && !composition) {
            setError(error, ErrorCode::InvalidFrame,
                     "composition-mask translation requires at least one source mask");
            return false;
        }
        for (id<MTLTexture> mask : {reactive, composition}) {
            if (!mask) continue;
            if (!basicTextureShape(mask) || !sameDevice(feature.device, mask) ||
                !rectFits(mask == composition ? frame.compositionRect : frame.reactiveRect, mask) ||
                !hasUsage(mask, MTLTextureUsageShaderRead)) {
                setError(error, ErrorCode::IncompatibleTexture,
                         "FSR reactive/composition mask is not a shader-readable render-size texture");
                return false;
            }
        }
        if (@available(macOS 27.0, *)) {
        } else {
            setError(error, ErrorCode::UnsupportedFeature,
                     "composition-mask translation requires macOS 27 MetalFX");
            return false;
        }
    }
    if (operations.colorTransfer != ColorTransfer::Linear &&
        (!hasUsage(color, MTLTextureUsageShaderRead) ||
         !hasUsage(output, MTLTextureUsageShaderWrite))) {
        setError(error, ErrorCode::IncompatibleTexture,
                 "FSR transfer conversion requires shader-readable color and shader-writable output");
        return false;
    }
    const TemporalOutputLayout outputLayout = temporalOutputLayout(feature, frame, operations);
    if (((feature.create.flags() & FeatureFlagAlphaUpscaling) || operations.drawDebugView) &&
        (!hasUsage(color, MTLTextureUsageShaderRead) || !hasUsage(depth, MTLTextureUsageShaderRead) ||
         !hasUsage(motion, MTLTextureUsageShaderRead))) {
        setError(error, ErrorCode::IncompatibleTexture, "SR alpha/debug inputs must be shader readable");
        return false;
    }
    if ((operations.sharpening || outputLayout.capped || operations.drawDebugView ||
         (feature.create.flags() & FeatureFlagAlphaUpscaling)) &&
        !hasUsage(output, MTLTextureUsageShaderWrite)) {
        setError(error, ErrorCode::IncompatibleTexture,
                 "FSR output finishing requires a shader-writable output texture");
        return false;
    }

    const MTLStorageMode storage = output.storageMode;
    if (storage != MTLStorageModePrivate && storage != MTLStorageModeShared) {
        setError(error, ErrorCode::IncompatibleTexture,
                 "MetalFX output must use Private or Shared Metal storage");
        return false;
    }
    return true;
}

bool ensureExposurePipeline(Feature::Impl& feature, Error* error) noexcept {
    if (feature.exposurePipeline) return true;
    if (!feature.fsrLibrary) feature.fsrLibrary = deviceFsrLibrary(feature.device);
    if (feature.fsrLibrary) feature.exposurePipeline = feature.fsrLibrary->getExposurePipeline();
    if (!feature.exposurePipeline) {
        setError(error, ErrorCode::ResourceCreationFailed,
                 "failed to synchronously create numerical MetalFX exposure conversion pipeline");
        return false;
    }
    return true;
}

// Each prepared frame compiles only the SR passes it encodes.
struct FsrPipelineNeeds {
    bool linearize = false;
    bool linearizeAlpha = false;
    bool combineMasks = false;
    bool finish = false;
    bool alpha = false;
    bool debug = false;
    bool debugDepth = false;
    bool padding = false;
};

// Callers resolve feature.fsrLibrary first; a missing library fails the pipeline.
bool ensureFsrPipeline(Feature::Impl& feature, bool needed, id<MTLComputePipelineState>& pipeline,
                       NSString* name) noexcept {
    if (!needed || pipeline) return true;
    if (feature.fsrLibrary)
        pipeline = feature.fsrLibrary->getPipeline(name);
    return pipeline != nil;
}

bool ensureFsrLibrary(Feature::Impl& feature) noexcept {
    if (!feature.fsrLibrary) feature.fsrLibrary = deviceFsrLibrary(feature.device);
    return feature.fsrLibrary && feature.fsrLibrary->ensureLibrary();
}

bool ensureFsrPipelines(Feature::Impl& feature, const FsrPipelineNeeds& needs,
                        Error* error) noexcept {
    const bool missing = (needs.linearize && !feature.linearizePipeline) ||
        (needs.linearizeAlpha && !feature.linearizeAlphaPipeline) ||
        (needs.combineMasks && !feature.combineMaskPipeline) || (needs.finish && !feature.finishPipeline) ||
        (needs.alpha && !feature.alphaPipeline) || (needs.padding && !feature.paddingPipeline) ||
        (needs.debug && !(needs.debugDepth ? feature.debugDepthPipeline : feature.debugPipeline));
    // The shared cache mutex belongs to preparation misses, not the steady frame path.
    if (!missing) return true;
    if (!ensureFsrLibrary(feature)) {
        setError(error, ErrorCode::ResourceCreationFailed,
                 "failed to compile FSR translation kernel library");
        return false;
    }
    // Attempt every needed pipeline so one failure does not hide another.
    bool ok = ensureFsrPipeline(feature, needs.linearize, feature.linearizePipeline,
                                @"yaagl_fsr_linearize");
    ok &= ensureFsrPipeline(feature, needs.linearizeAlpha, feature.linearizeAlphaPipeline,
                            @"yaagl_sr_linearize_alpha");
    ok &= ensureFsrPipeline(feature, needs.combineMasks, feature.combineMaskPipeline,
                            @"yaagl_fsr_combine_masks");
    ok &= ensureFsrPipeline(feature, needs.finish, feature.finishPipeline, @"yaagl_fsr_finish");
    ok &= ensureFsrPipeline(feature, needs.alpha, feature.alphaPipeline, @"yaagl_sr_extract_alpha");
    ok &= ensureFsrPipeline(feature, needs.debug && !needs.debugDepth, feature.debugPipeline,
                            @"yaagl_sr_debug_view");
    ok &= ensureFsrPipeline(feature, needs.debug && needs.debugDepth, feature.debugDepthPipeline,
                            @"yaagl_sr_debug_depth_view");
    ok &= ensureFsrPipeline(feature, needs.padding, feature.paddingPipeline,
                            @"yaagl_fsr_extend_edges");
    if (!ok) {
        setError(error, ErrorCode::ResourceCreationFailed,
                 "failed to create FSR translation compute pipelines");
        return false;
    }
    return true;
}

bool exactDescriptorInput(id<MTLTexture> texture, const Rect& rect,
                           NSUInteger activeWidth, NSUInteger activeHeight,
                           NSUInteger capacityWidth, NSUInteger capacityHeight,
                           MTLTextureUsage usage) noexcept {
    bool offsets = false;
    if (@available(macOS 27.0, *)) offsets = true;
    return texture && rectFits(rect, texture) && (offsets || (!rect.x && !rect.y)) &&
           rect.width == activeWidth && rect.height == activeHeight &&
           texture.width == capacityWidth && texture.height == capacityHeight &&
           hasUsage(texture, usage);
}

bool generationMatches(const ScalerGeneration& generation, const FrameInfo& frame,
                       id<MTLTexture> color, id<MTLTexture> depth,
                       id<MTLTexture> motion, id<MTLTexture> output,
                       id<MTLTexture> reactive, const FrameOperations& operations,
                       const TemporalOutputLayout& temporal) noexcept {
    if (frame.inputContent.width > generation.inputCapacityWidth ||
        frame.inputContent.height > generation.inputCapacityHeight ||
        generation.colorFormat != workingColorFormat(color.pixelFormat, operations.colorTransfer) ||
        generation.depthFormat != depth.pixelFormat ||
        generation.motionFormat != motion.pixelFormat ||
        generation.outputWidth != temporal.width || generation.outputHeight != temporal.height ||
        generation.outputFormat != workingOutputFormat(output.pixelFormat, operations)) {
        return false;
    }
    const bool combined = operations.combineCompositionMask;
    if (!reactive && !combined) return !generation.reactiveEnabled;
    return generation.reactiveEnabled &&
           generation.reactiveFormat == (combined ? MTLPixelFormatR8Unorm : reactive.pixelFormat);
}

std::shared_ptr<ScalerGeneration> ensureScaler(Feature::Impl& feature,
                                               const FrameInfo& frame,
                                               const TextureSet& set,
                                               const FrameOperations& operations,
                                               Error* error) noexcept {
    id<MTLTexture> color = asTexture(set.color);
    id<MTLTexture> depth = asTexture(set.depth);
    id<MTLTexture> motion = asTexture(set.motion);
    id<MTLTexture> output = asTexture(set.output);
    id<MTLTexture> reactive = asTexture(set.reactive);
    const TemporalOutputLayout temporal = temporalOutputLayout(feature, frame, operations);

    std::shared_ptr<ScalerGeneration> selected;
    if (feature.currentGeneration &&
        generationMatches(*feature.currentGeneration, frame, color, depth, motion, output, reactive,
                          operations, temporal))
        selected = feature.currentGeneration;
    // A fitting high-water generation forces padded staging; prefer a cached
    // generation whose capacity equals the active extent.
    if (!selected || selected->inputCapacityWidth != frame.inputContent.width ||
        selected->inputCapacityHeight != frame.inputContent.height) {
        for (const auto& cached : feature.generations) {
            const bool exact = cached && cached->inputCapacityWidth == frame.inputContent.width &&
                               cached->inputCapacityHeight == frame.inputContent.height;
            if (cached && (!selected || exact) &&
                generationMatches(*cached, frame, color, depth, motion, output, reactive,
                                  operations, temporal)) {
                selected = cached;
                if (exact) break;
            }
        }
    }
    if (selected) {
        if (selected != feature.currentGeneration) {
            feature.currentGeneration = selected;
            // Prepared frames retain their activation so an older encode cannot
            // consume the reset owed to this nonconsecutive sequence.
            ++feature.currentGeneration->activation;
        }
        return selected;
    }

    NSUInteger inputCapacityWidth = frame.inputContent.width;
    NSUInteger inputCapacityHeight = frame.inputContent.height;
    if (feature.currentGeneration) {
        inputCapacityWidth = std::max(inputCapacityWidth,
                                      feature.currentGeneration->inputCapacityWidth);
        inputCapacityHeight = std::max(inputCapacityHeight,
                                       feature.currentGeneration->inputCapacityHeight);
    }
    const auto supportedCapacityScale = [&](NSUInteger width, NSUInteger height) noexcept {
        const float scaleX = static_cast<float>(temporal.width) / static_cast<float>(width);
        const float scaleY = static_cast<float>(temporal.height) / static_cast<float>(height);
        return finite(scaleX) && finite(scaleY) &&
               scaleX >= feature.minScale && scaleX <= feature.maxScale &&
               scaleY >= feature.minScale && scaleY <= feature.maxScale;
    };
    // Keep only a high-water extent observed by this feature. If the componentwise
    // high-water would make this descriptor scale unsupported after an output cap
    // or ratio change, fall back to the current frame's valid exact extent.
    if (!supportedCapacityScale(inputCapacityWidth, inputCapacityHeight)) {
        inputCapacityWidth = frame.inputContent.width;
        inputCapacityHeight = frame.inputContent.height;
    }

    std::shared_ptr<ScalerGeneration> generation;
    MTLFXTemporalScalerDescriptor* descriptor = nil;
    @try {
        try {
            generation = std::make_shared<ScalerGeneration>();
        } catch (...) {
            setError(error, ErrorCode::ResourceCreationFailed,
                     "failed to allocate MetalFX scaler generation state");
            return {};
        }
        descriptor = [MTLFXTemporalScalerDescriptor new];
        descriptor.colorTextureFormat = workingColorFormat(color.pixelFormat, operations.colorTransfer);
        descriptor.depthTextureFormat = depth.pixelFormat;
        descriptor.motionTextureFormat = motion.pixelFormat;
        descriptor.outputTextureFormat = workingOutputFormat(output.pixelFormat, operations);
        // MetalFX requires bound textures to match these exact dimensions. Dynamic
        // content lets one generation serve smaller active extents; any smaller
        // source texture is staged into this generation's exact capacity.
        descriptor.inputWidth = inputCapacityWidth;
        descriptor.inputHeight = inputCapacityHeight;
        descriptor.outputWidth = temporal.width;
        descriptor.outputHeight = temporal.height;
        descriptor.autoExposureEnabled = feature.create.autoExposure();
        descriptor.requiresSynchronousInitialization = NO;
        descriptor.inputContentPropertiesEnabled = YES;
        descriptor.inputContentMinScale = feature.minScale;
        descriptor.inputContentMaxScale = feature.maxScale;
        if (@available(macOS 27.0, *)) {
            descriptor.outputResolutionMotionVectorsEnabled =
                !feature.create.lowResolutionMotionVectors();
            descriptor.jitteredMotionVectorsEnabled = feature.create.jitteredMotionVectors();
        }
        if (reactive || operations.combineCompositionMask) {
            if (@available(macOS 27.0, *)) {
                descriptor.reactiveMaskTextureEnabled = YES;
                descriptor.reactiveMaskTextureFormat = operations.combineCompositionMask
                                                           ? MTLPixelFormatR8Unorm
                                                           : reactive.pixelFormat;
            }
        }

        id scaler = nil;
        if (@available(macOS 26.0, *)) {
            id<MTL4Compiler> compiler = reinterpret_cast<id<MTL4Compiler>>(feature.compiler);
            scaler = [descriptor newTemporalScalerWithDevice:feature.device compiler:compiler];
        }
        if (!scaler) {
            setError(error, ErrorCode::ScalerCreationFailed,
                     "MetalFX temporal scaler factory returned nil");
            return {};
        }
        generation->scaler = scaler; // new factory returns +1
        generation->inputCapacityWidth = inputCapacityWidth;
        generation->inputCapacityHeight = inputCapacityHeight;
        generation->colorFormat = descriptor.colorTextureFormat;
        generation->depthFormat = depth.pixelFormat;
        generation->motionFormat = motion.pixelFormat;
        generation->outputFormat = descriptor.outputTextureFormat;
        generation->outputWidth = temporal.width;
        generation->outputHeight = temporal.height;
        generation->reactiveEnabled = reactive != nil || operations.combineCompositionMask;
        generation->reactiveFormat = operations.combineCompositionMask
                                         ? MTLPixelFormatR8Unorm
                                         : (reactive ? reactive.pixelFormat : MTLPixelFormatInvalid);
        generation->colorUsage = [scaler colorTextureUsage];
        generation->depthUsage = [scaler depthTextureUsage];
        generation->motionUsage = [scaler motionTextureUsage];
        generation->outputUsage = [scaler outputTextureUsage];
        if (feature.create.flags() & FeatureFlagAlphaUpscaling) {
            descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
            descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
            descriptor.autoExposureEnabled = NO;
            if (@available(macOS 26.0, *))
                generation->alphaScaler = [descriptor newTemporalScalerWithDevice:feature.device
                    compiler:reinterpret_cast<id<MTL4Compiler>>(feature.compiler)];
            if (!generation->alphaScaler) {
                setError(error, ErrorCode::ScalerCreationFailed, "failed to create linear alpha temporal scaler");
                return {};
            }
            generation->depthUsage |= [generation->alphaScaler depthTextureUsage];
            generation->motionUsage |= [generation->alphaScaler motionTextureUsage];
        }
        if (reactive || operations.combineCompositionMask) {
            if (@available(macOS 27.0, *))
                generation->reactiveUsage = [scaler reactiveMaskTextureUsage];
            if (generation->alphaScaler) {
                if (@available(macOS 27.0, *))
                    generation->reactiveUsage |= [generation->alphaScaler reactiveMaskTextureUsage];
            }
        }

        // Dynamic output keeps one feature across output extents. Bound cached
        // scalers (each owning output-sized history) to the new and the
        // previously current output extent; prepared frames keep evicted ones.
        for (auto& cached : feature.generations) {
            if (!cached) continue;
            const bool sameOutput = cached->outputWidth == generation->outputWidth &&
                                    cached->outputHeight == generation->outputHeight;
            const bool previousOutput = feature.currentGeneration &&
                cached->outputWidth == feature.currentGeneration->outputWidth &&
                cached->outputHeight == feature.currentGeneration->outputHeight;
            if (!sameOutput && !previousOutput) cached.reset();
        }
        feature.generations[feature.nextGenerationSlot] = generation;
        feature.nextGenerationSlot = (feature.nextGenerationSlot + 1) % feature.generations.size();
        feature.currentGeneration = generation;
        return generation;
    } @catch (id) {
        generation.reset();
        setError(error, ErrorCode::ScalerCreationFailed,
                 "MetalFX temporal scaler creation raised an exception");
        return {};
    } @finally {
        [descriptor release];
    }
}

// Mirrors YaaglFsrParams in fsr-kernels.metal.
struct alignas(8) Params {
    std::uint32_t sourceOrigin[2];
    std::uint32_t compositionOrigin[2];
    std::uint32_t destinationOrigin[2];
    std::uint32_t extent[2];
    std::uint32_t dispatchOrigin[2];
    std::uint32_t dispatchExtent[2];
    std::uint32_t transfer;
    std::uint32_t flags;
    float sharpness;
    float exposure;
};

std::shared_ptr<ExecutionLease::Impl> makeLease(const PreparedFrame::Impl& frame,
                                                Error* error) noexcept {
    try {
        auto lease = std::make_shared<ExecutionLease::Impl>();
        lease->feature = frame.feature;
        lease->prepared = frame.self;
        Feature::Impl& feature = *frame.feature;
        ScalerGeneration& generation = *frame.generation;

        if (!feature.takeBundle(*lease, &frame)) {
            lease->residency = feature.takeResidency(lease->residencyContents);
            const ResidencyBindings* prefer = &lease->residencyContents;
            // Scratch padded by copies or in-place edge writes keeps GPU-optimized
            // contents disabled; the combined mask is written once and keeps it.
            const bool fullInputCapacity =
                frame.frame.inputContent.width == generation.inputCapacityWidth &&
                frame.frame.inputContent.height == generation.inputCapacityHeight;
            const auto copyUsage = [&](unsigned bit) noexcept {
                return (frame.computeCopyMask & bit) ? MTLTextureUsageShaderWrite
                                                     : MTLTextureUsageUnknown;
            };
            const bool transfer = frame.operations.colorTransfer != ColorTransfer::Linear;
            const bool alpha = generation.alphaScaler != nil;
            const bool finish = transfer || frame.operations.sharpening || frame.cappedOutput ||
                                alpha || frame.operations.drawDebugView;
            id<MTLTexture> scalerOutput = frame.output;
            if (frame.needsOutputShadow) {
                const MTLTextureUsage outputUsage = generation.outputUsage |
                    (finish ? MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite
                            : MTLTextureUsageUnknown);
                scalerOutput = acquireScratch(
                    feature, frame.output, generation.outputFormat, outputUsage,
                    @".YAAGL.MetalFX.ActiveOutput", frame.temporalOutputWidth,
                    frame.temporalOutputHeight, true, prefer);
                if (!scalerOutput) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "failed to allocate exact-size Private MetalFX output texture");
                    return {};
                }
                lease->privateOutput = scalerOutput;
            }
            if (transfer) {
                lease->linearColor = acquireScratch(
                    feature, frame.color, generation.colorFormat,
                    generation.colorUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
                    @".YAAGL.FSR.LinearColor", generation.inputCapacityWidth,
                    generation.inputCapacityHeight, fullInputCapacity, prefer);
                if (!lease->linearColor) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "failed to allocate exact-size FSR linear input texture");
                    return {};
                }
            } else if (frame.needsColorStaging) {
                lease->stagedColor = acquireScratch(
                    feature, frame.color, frame.color.pixelFormat,
                    generation.colorUsage | copyUsage(1u),
                    @".YAAGL.MetalFX.ActiveColor", generation.inputCapacityWidth,
                    generation.inputCapacityHeight, fullInputCapacity, prefer);
            }
            if (frame.needsDepthStaging)
                lease->stagedDepth = acquireScratch(
                    feature, frame.depth, frame.depth.pixelFormat,
                    generation.depthUsage | copyUsage(2u),
                    @".YAAGL.MetalFX.ActiveDepth", generation.inputCapacityWidth,
                    generation.inputCapacityHeight, fullInputCapacity, prefer);
            if (frame.needsMotionStaging)
                lease->stagedMotion = acquireScratch(
                    feature, frame.motion, frame.motion.pixelFormat,
                    generation.motionUsage | copyUsage(4u),
                    @".YAAGL.MetalFX.ActiveMotion",
                    feature.create.lowResolutionMotionVectors() ? generation.inputCapacityWidth
                                                                : frame.scalerMotionRect.width,
                    feature.create.lowResolutionMotionVectors() ? generation.inputCapacityHeight
                                                                : frame.scalerMotionRect.height,
                    !feature.create.lowResolutionMotionVectors() || fullInputCapacity, prefer);
            if (frame.operations.combineCompositionMask && !frame.directMask) {
                lease->combinedMask = acquireScratch(
                    feature, frame.reactive ? frame.reactive : frame.composition,
                    MTLPixelFormatR8Unorm,
                    MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
                    @".YAAGL.FSR.CombinedMask", generation.inputCapacityWidth,
                    generation.inputCapacityHeight, true, prefer);
                if (!lease->combinedMask ||
                    !hasUsage(lease->combinedMask, generation.reactiveUsage)) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "failed to allocate exact-size MetalFX combined reactive mask");
                    return {};
                }
            } else if (frame.needsReactiveStaging) {
                lease->stagedReactive = acquireScratch(
                    feature, frame.reactive, frame.reactive.pixelFormat,
                    generation.reactiveUsage | copyUsage(8u), @".YAAGL.MetalFX.ActiveReactive",
                    generation.inputCapacityWidth, generation.inputCapacityHeight, fullInputCapacity, prefer);
            }
            if ((!transfer && frame.needsColorStaging && !lease->stagedColor) ||
                (frame.needsDepthStaging && !lease->stagedDepth) ||
                (frame.needsMotionStaging && !lease->stagedMotion) ||
                (frame.needsReactiveStaging && !lease->stagedReactive)) {
                setError(error, ErrorCode::ResourceCreationFailed,
                         "failed to allocate exact-size MetalFX input texture");
                return {};
            }

            if (frame.needsExposureConversion) {
                lease->convertedExposure = acquireExposure(feature, prefer);
                if (!lease->convertedExposure) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "failed to allocate R16Float exposure conversion texture");
                    return {};
                }
            }

            if (alpha) {
                lease->alphaColor = acquireScratch(feature, frame.color, MTLPixelFormatRGBA16Float,
                    [generation.alphaScaler colorTextureUsage] | MTLTextureUsageShaderWrite,
                    @".YAAGL.Alpha.Input", generation.inputCapacityWidth, generation.inputCapacityHeight, true, prefer);
                lease->alphaOutput = acquireScratch(feature, frame.output, MTLPixelFormatRGBA16Float,
                    [generation.alphaScaler outputTextureUsage] | MTLTextureUsageShaderRead,
                    @".YAAGL.Alpha.Output", frame.temporalOutputWidth, frame.temporalOutputHeight, true, prefer);
                if (!lease->alphaColor || !lease->alphaOutput) {
                    setError(error, ErrorCode::ResourceCreationFailed, "failed to allocate temporal alpha textures");
                    return {};
                }
            }
            const std::uint32_t flags = (frame.reactive ? 1u : 0u) |
                                        (frame.composition ? 2u : 0u) |
                                        (frame.operations.sharpening ? 4u : 0u) |
                                        (frame.frame.exposureMode == ExposureMode::Texture ? 8u : 0u) |
                                        (frame.cappedOutput ? 16u : 0u) | (alpha ? 32u : 0u);
            const auto makeRecord = [&](const Rect& source, const Rect& destination,
                                        const Rect& dispatch, ColorTransfer transfer) {
                return Params{{source.x, source.y}, {frame.frame.compositionRect.x, frame.frame.compositionRect.y},
                              {destination.x, destination.y},
                              {source.width, source.height}, {dispatch.x, dispatch.y},
                              {dispatch.width, dispatch.height}, static_cast<std::uint32_t>(transfer),
                              flags, frame.operations.sharpness, frame.frame.preExposure};
            };
            // Copy, linearization/padding, mask, alpha, debug and finish each
            // have one 256-byte-aligned slot. No live execution shares writable
            // constants; only GPU-retired arenas return to the existing pool.
            constexpr NSUInteger slotBytes = 256, arenaBytes = 6 * slotBytes;
            static_assert(4 * sizeof(Params) <= slotBytes);
            NSUInteger parameterSlot = 0;
            const auto storeParams = [&](const void* records, NSUInteger length) -> ParameterSlice {
                if (length > slotBytes || parameterSlot == 6) return {};
                if (!lease->parameterArena) {
                    lease->parameterArena = feature.takeBuffer(arenaBytes, prefer);
                    if (!lease->parameterArena)
                        lease->parameterArena = [feature.device newBufferWithLength:arenaBytes
                                                                   options:MTLResourceStorageModeShared];
                }
                if (!lease->parameterArena) return {};
                const NSUInteger offset = parameterSlot++ * slotBytes;
                void* contents = static_cast<char*>(lease->parameterArena.contents) + offset;
                std::memcpy(contents, records, length);
                return {lease->parameterArena, length, contents, lease->parameterArena.gpuAddress + offset};
            };
            const auto makeBuffer = [&](const Params* records, std::size_t count) {
                return storeParams(records, count * sizeof(Params));
            };
            const auto makeParams = [&](const Rect& source, const Rect& destination,
                                        const Rect& dispatch, ColorTransfer transfer) {
                const Params params = makeRecord(source, destination, dispatch, transfer);
                return makeBuffer(&params, 1);
            };
            const Rect capacity{0, 0, static_cast<std::uint32_t>(generation.inputCapacityWidth),
                                static_cast<std::uint32_t>(generation.inputCapacityHeight)};
            const Extent active = frame.frame.inputContent;
            if (frame.computeCopyMask) {
                // One record per copied input; each replicates its active edges
                // over the descriptor capacity.
                const Rect sources[] = {frame.frame.colorRect, frame.frame.depthRect,
                                        frame.scalerMotionRect, frame.frame.reactiveRect};
                std::array<Params, 4> records{};
                for (std::size_t i = 0; i != records.size(); ++i)
                    records[i] = makeRecord(sources[i], Rect{}, capacity, ColorTransfer::Linear);
                lease->copyParams = makeBuffer(records.data(), records.size());
                if (!lease->copyParams) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "failed to allocate padded input copy constants");
                    return {};
                }
            }
            const ColorTransfer inputTransfer =
                frame.operations.colorTransfer == ColorTransfer::SRGB &&
                        isSrgbFormat(frame.color.pixelFormat)
                    ? ColorTransfer::Linear : frame.operations.colorTransfer;
            if (transfer) {
                const Rect destination{0, 0, active.width, active.height};
                // Record 0 linearizes the active extent; records 1-2 replicate its
                // converted edges over the disjoint right and bottom strips.
                std::array<Params, 3> records{};
                records.fill(makeRecord(frame.frame.colorRect, destination, destination, inputTransfer));
                records[1].dispatchOrigin[0] = active.width;
                records[1].dispatchExtent[0] = capacity.width - active.width;
                records[1].dispatchExtent[1] = active.height;
                records[2].dispatchOrigin[1] = active.height;
                records[2].dispatchExtent[0] = capacity.width;
                records[2].dispatchExtent[1] = capacity.height - active.height;
                lease->linearizeParams = makeBuffer(records.data(), fullInputCapacity ? 1 : 3);
            }
            if (frame.operations.combineCompositionMask && !frame.directMask)
                // The clamped combine covers the capacity, padding the mask in-place.
                lease->maskParams = makeParams(
                    Rect{frame.frame.reactiveRect.x, frame.frame.reactiveRect.y,
                         active.width, active.height},
                    Rect{0, 0, active.width, active.height}, capacity, ColorTransfer::Linear);
            if (alpha)
                lease->alphaParams = makeParams(frame.frame.colorRect, Rect{},
                    Rect{0, 0, static_cast<uint32_t>(generation.inputCapacityWidth),
                               static_cast<uint32_t>(generation.inputCapacityHeight)}, inputTransfer);
            if (frame.operations.drawDebugView) {
                struct DebugParams {
                    uint32_t outputOrigin[2], outputExtent[2], depthOrigin[2], depthExtent[2];
                    uint32_t motionOrigin[2], motionExtent[2];
                    float motionScale[2];
                    uint32_t depthInverted, transfer;
                };
                static_assert(sizeof(DebugParams) <= slotBytes);
                const auto& f = frame.frame;
                const auto outputTransfer = frame.operations.colorTransfer == ColorTransfer::SRGB &&
                    isSrgbFormat(frame.output.pixelFormat) ? ColorTransfer::Linear : frame.operations.colorTransfer;
                DebugParams params{{f.outputRect.x, f.outputRect.y}, {f.outputRect.width, f.outputRect.height},
                    {f.depthRect.x, f.depthRect.y}, {f.depthRect.width, f.depthRect.height},
                    {f.motionRect.x, f.motionRect.y}, {f.motionRect.width, f.motionRect.height},
                    {f.motionVectorScaleX, f.motionVectorScaleY}, uint32_t(feature.create.depthInverted()),
                    uint32_t(outputTransfer)};
                lease->debugParams = storeParams(&params, sizeof(params));
            }
            if (finish) {
                const ColorTransfer outputTransfer =
                    frame.operations.colorTransfer == ColorTransfer::SRGB &&
                            isSrgbFormat(frame.output.pixelFormat)
                        ? ColorTransfer::Linear : frame.operations.colorTransfer;
                const Rect source{0, 0,
                                  static_cast<std::uint32_t>(frame.temporalOutputWidth),
                                  static_cast<std::uint32_t>(frame.temporalOutputHeight)};
                const Rect destination{static_cast<std::uint32_t>(frame.frame.outputRect.x + frame.placementX),
                                       static_cast<std::uint32_t>(frame.frame.outputRect.y + frame.placementY),
                                       source.width, source.height};
                lease->finishParams = makeParams(source, destination, frame.frame.outputRect,
                                                 outputTransfer);
            }
            if ((transfer && !lease->linearizeParams) ||
                (frame.operations.combineCompositionMask && !frame.directMask && !lease->maskParams) ||
                (finish && !lease->finishParams) || (alpha && !lease->alphaParams) ||
                (frame.operations.drawDebugView && !lease->debugParams)) {
                setError(error, ErrorCode::ResourceCreationFailed,
                         "failed to allocate FSR operation constants");
                return {};
            }

            id reusable = std::exchange(lease->residency, nil);
            lease->residency = makeResidencySet(feature.device,
                residencyBindings(lease->privateOutput, lease->convertedExposure,
                    lease->linearColor, lease->combinedMask, lease->stagedColor,
                    lease->stagedDepth, lease->stagedMotion, lease->stagedReactive,
                    lease->parameterArena, lease->alphaColor, lease->alphaOutput),
                    lease->residencyContents, reusable);
            if (!lease->residency) {
                setError(error, ErrorCode::ResourceCreationFailed,
                         "failed to create Metal4 residency set for MetalFX execution");
                return {};
            }
        }

        const bool fsrPasses = lease->linearizeParams || lease->maskParams ||
                               lease->finishParams || lease->copyParams;
        if (fsrPasses && !lease->fsrArgumentTable)
            lease->fsrArgumentTable = feature.takeTable(feature.idleFsrTables);
        if (fsrPasses && !lease->fsrArgumentTable) {
            if (@available(macOS 26.0, *)) {
                MTL4ArgumentTableDescriptor* descriptor = nil;
                @try {
                    descriptor = [MTL4ArgumentTableDescriptor new];
                    descriptor.maxBufferBindCount = 1;
                    descriptor.maxTextureBindCount = 4;
                    descriptor.initializeBindings = YES;
                    descriptor.label = @"YAAGL FSR translation";
                    NSError* tableError = nil;
                    lease->fsrArgumentTable =
                        [feature.device newArgumentTableWithDescriptor:descriptor error:&tableError];
                    if (!lease->fsrArgumentTable) {
                        setError(error, ErrorCode::ResourceCreationFailed,
                                 "failed to create Metal4 FSR argument table");
                        return {};
                    }
                } @catch (id) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "Metal4 FSR argument-table setup raised an exception");
                    return {};
                } @finally {
                    [descriptor release];
                }
            }
        }

        if (frame.needsExposureConversion) {
            if (@available(macOS 26.0, *)) {
                MTL4ArgumentTableDescriptor* descriptor = nil;
                @try {
                    if (!lease->argumentTable)
                        lease->argumentTable = feature.takeTable(feature.idleExposureTables);
                    if (!lease->argumentTable) {
                        descriptor = [MTL4ArgumentTableDescriptor new];
                        descriptor.maxTextureBindCount = 2;
                        descriptor.initializeBindings = YES;
                        descriptor.label = @"YAAGL MetalFX exposure conversion";
                        NSError* tableError = nil;
                        lease->argumentTable =
                            [feature.device newArgumentTableWithDescriptor:descriptor error:&tableError];
                    }
                    if (!lease->argumentTable) {
                        setError(error, ErrorCode::ResourceCreationFailed,
                                 "failed to create Metal4 exposure argument table");
                        return {};
                    }
                    id<MTL4ArgumentTable> table =
                        reinterpret_cast<id<MTL4ArgumentTable>>(lease->argumentTable);
                    [table setTexture:frame.exposure.gpuResourceID atIndex:0];
                    [table setTexture:lease->convertedExposure.gpuResourceID atIndex:1];
                } @catch (id) {
                    setError(error, ErrorCode::ResourceCreationFailed,
                             "Metal4 exposure argument-table setup raised an exception");
                    return {};
                } @finally {
                    [descriptor release];
                }
            }
        }
        return lease;
    } catch (...) {
        setError(error, ErrorCode::ResourceCreationFailed,
                 "failed to allocate MetalFX execution ownership state");
        return {};
    }
}

// R8 mask combining measured faster at 16x16 on Apple M5 Pro Metal4 (fork
// opt audit); every other SR pass keeps 8x8.
constexpr MTLSize kFsrDefaultThreadgroup{8, 8, 1};
constexpr MTLSize kFsrWideThreadgroup{16, 16, 1};

void dispatchFsrPassMetal4(ExecutionLease::Impl& lease, id<MTL4ComputeCommandEncoder> encoder,
                           id<MTLComputePipelineState> pipeline, const ParameterSlice& params,
                           id<MTLTexture> first, id<MTLTexture> second,
                           id<MTLTexture> third, const Rect& rect, id<MTLTexture> fourth = nil,
                           NSUInteger parameterOffset = 0,
                           MTLSize threadgroup = kFsrDefaultThreadgroup) API_AVAILABLE(macos(26.0)) {
    id<MTL4ArgumentTable> table =
        reinterpret_cast<id<MTL4ArgumentTable>>(lease.fsrArgumentTable);
    [table setAddress:params.gpuAddress + parameterOffset atIndex:0];
    [table setTexture:first.gpuResourceID atIndex:0];
    [table setTexture:second.gpuResourceID atIndex:1];
    // Pooled tables never retain another lease's resource IDs.
    [table setTexture:(third ? third : first).gpuResourceID atIndex:2];
    [table setTexture:(fourth ? fourth : first).gpuResourceID atIndex:3];
    [encoder setComputePipelineState:pipeline];
    [encoder setArgumentTable:table];
    [encoder dispatchThreads:MTLSizeMake(rect.width, rect.height, 1)
         threadsPerThreadgroup:threadgroup];
}

bool encodeFsrPassMetal4(ExecutionLease::Impl& lease,
                         id<MTL4CommandBuffer> command, id<MTLFence> fence,
                         id<MTLComputePipelineState> pipeline, const ParameterSlice& params,
                         id<MTLTexture> first, id<MTLTexture> second,
                         id<MTLTexture> third, const Rect& rect, id<MTLTexture> fourth = nil,
                         MTLSize threadgroup = kFsrDefaultThreadgroup) API_AVAILABLE(macos(26.0)) {
    id<MTL4ComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) return false;
    @try {
        [encoder waitForFence:fence beforeEncoderStages:MTLStageDispatch];
        dispatchFsrPassMetal4(lease, encoder, pipeline, params, first, second, third, rect,
                              fourth, 0, threadgroup);
        [encoder updateFence:fence afterEncoderStages:MTLStageDispatch];
        [encoder endEncoding];
        return true;
    } @catch (id) {
        @try { [encoder endEncoding]; } @catch (id) {}
        return false;
    }
}

template <typename Encoder, typename Barrier>
void extendTextureEdges(Encoder encoder, id<MTLTexture> texture,
                        NSUInteger activeWidth, NSUInteger activeHeight,
                        Barrier barrier) {
    const NSUInteger capacityWidth = texture.width;
    const NSUInteger capacityHeight = texture.height;
    bool edgeCopyEncoded = false;
    const auto copy = [&](NSUInteger sourceX, NSUInteger sourceY,
                          NSUInteger width, NSUInteger height,
                          NSUInteger destinationX, NSUInteger destinationY) {
        if (edgeCopyEncoded) barrier();
        [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(sourceX, sourceY, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                       toTexture:texture destinationSlice:0 destinationLevel:0
               destinationOrigin:MTLOriginMake(destinationX, destinationY, 0)];
        edgeCopyEncoded = true;
    };
    if (activeWidth < capacityWidth) {
        copy(activeWidth - 1, 0, 1, activeHeight, activeWidth, 0);
        NSUInteger paddedWidth = 1;
        while (activeWidth + paddedWidth < capacityWidth) {
            const NSUInteger width = std::min(paddedWidth,
                                               capacityWidth - activeWidth - paddedWidth);
            copy(activeWidth, 0, width, activeHeight, activeWidth + paddedWidth, 0);
            paddedWidth += width;
        }
    }
    if (activeHeight < capacityHeight) {
        copy(0, activeHeight - 1, capacityWidth, 1, 0, activeHeight);
        NSUInteger paddedHeight = 1;
        while (activeHeight + paddedHeight < capacityHeight) {
            const NSUInteger height = std::min(paddedHeight,
                                                capacityHeight - activeHeight - paddedHeight);
            copy(0, activeHeight, capacityWidth, height, 0, activeHeight + paddedHeight);
            paddedHeight += height;
        }
    }
}

// Metal 4 may overlap blits in one pass; edge copies consume earlier copy output.
void barrierBetweenMetal4Blits(id<MTL4ComputeCommandEncoder> encoder)
    API_AVAILABLE(macos(26.0)) {
    [encoder barrierAfterEncoderStages:MTLStageBlit
                  beforeEncoderStages:MTLStageBlit
                    visibilityOptions:MTL4VisibilityOptionDevice];
}

// Records every SR preprocessing pass in one encoder behind one fence wait and
// one update. Exposure conversion, input staging, alpha extraction, color
// linearization and mask combining read only app inputs and write disjoint
// lease scratch, so they may overlap. Blit edge copies consume earlier copy
// output and the padding strips consume linearized color; only those wait.
bool encodePrepassMetal4(const PreparedFrame::Impl& frame, ExecutionLease::Impl& lease,
                         id<MTL4CommandBuffer> command, id<MTLFence> fence)
                         API_AVAILABLE(macos(26.0)) {
    const bool copies = lease.stagedColor || lease.stagedDepth || lease.stagedMotion ||
                        lease.stagedReactive;
    const bool dispatches = frame.needsExposureConversion || lease.alphaColor ||
                            lease.linearColor || lease.combinedMask ||
                            (copies && frame.computeCopyMask);
    if (!copies && !dispatches) return true;
    id<MTL4ComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) return false;
    @try {
        const MTLStages stages = (copies ? MTLStageBlit : 0) | (dispatches ? MTLStageDispatch : 0);
        [encoder waitForFence:fence beforeEncoderStages:stages];
        if (frame.needsExposureConversion) {
            [encoder setComputePipelineState:frame.feature->exposurePipeline];
            [encoder setArgumentTable:lease.argumentTable];
            [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        }
        const auto barrier = [&] { barrierBetweenMetal4Blits(encoder); };
        const Rect capacity{0, 0, static_cast<std::uint32_t>(frame.generation->inputCapacityWidth),
                            static_cast<std::uint32_t>(frame.generation->inputCapacityHeight)};
        unsigned bit = 1u;
        const auto copy = [&](id<MTLTexture> source, id<MTLTexture> destination,
                              const Rect& rect) {
            const unsigned index = static_cast<unsigned>(__builtin_ctz(bit));
            const bool compute = frame.computeCopyMask & bit;
            bit <<= 1;
            if (!destination) return;
            if (compute) {
                // Writes only lease-owned scratch; inputs are never written.
                dispatchFsrPassMetal4(lease, encoder,
                    source.pixelFormat == MTLPixelFormatRGBA16Float ||
                            source.pixelFormat == MTLPixelFormatRG16Float
                        ? frame.feature->copyHalfPipeline : frame.feature->copyFloatPipeline,
                    lease.copyParams, source, destination, nil, capacity, nil,
                    index * sizeof(Params));
                return;
            }
            [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0
                        sourceOrigin:MTLOriginMake(rect.x, rect.y, 0)
                          sourceSize:MTLSizeMake(rect.width, rect.height, 1)
                           toTexture:destination destinationSlice:0 destinationLevel:0
                   destinationOrigin:MTLOriginMake(0, 0, 0)];
            if (rect.width < destination.width || rect.height < destination.height)
                barrier();
            extendTextureEdges(encoder, destination, rect.width, rect.height, barrier);
        };
        copy(frame.color, lease.stagedColor, frame.frame.colorRect);
        copy(frame.depth, lease.stagedDepth, frame.frame.depthRect);
        copy(frame.motion, lease.stagedMotion, frame.scalerMotionRect);
        copy(frame.reactive, lease.stagedReactive, frame.frame.reactiveRect);
        const bool fusedColorAlpha = lease.alphaColor && lease.linearColor && frame.feature->linearizeAlphaPipeline;
        if (fusedColorAlpha)
            dispatchFsrPassMetal4(lease, encoder, frame.feature->linearizeAlphaPipeline, lease.alphaParams,
                frame.color, lease.linearColor, lease.alphaColor,
                Rect{0, 0, uint32_t(lease.alphaColor.width), uint32_t(lease.alphaColor.height)});
        if (lease.alphaColor && !fusedColorAlpha)
            dispatchFsrPassMetal4(lease, encoder, frame.feature->alphaPipeline, lease.alphaParams,
                frame.color, lease.alphaColor, nil,
                Rect{0, 0, uint32_t(lease.alphaColor.width), uint32_t(lease.alphaColor.height)});
        if (lease.linearColor && !fusedColorAlpha)
            dispatchFsrPassMetal4(lease, encoder, frame.feature->linearizePipeline,
                                  lease.linearizeParams, frame.color, lease.linearColor, nil,
                                  frame.frame.colorRect);
        if (lease.combinedMask) {
            // The clamped capacity dispatch pads the mask itself.
            id<MTLTexture> fallback = frame.reactive ? frame.reactive : frame.composition;
            dispatchFsrPassMetal4(lease, encoder, frame.feature->combineMaskPipeline, lease.maskParams,
                                  frame.reactive ? frame.reactive : fallback,
                                  frame.composition ? frame.composition : fallback,
                                  lease.combinedMask,
                                  Rect{0, 0, static_cast<std::uint32_t>(lease.combinedMask.width),
                                       static_cast<std::uint32_t>(lease.combinedMask.height)},
                                  nil, 0, kFsrWideThreadgroup);
        }
        // Extend already-converted active color edges without repeating transfer math.
        if (lease.linearColor && lease.linearizeParams.length > sizeof(Params)) {
            [encoder barrierAfterEncoderStages:MTLStageDispatch
                          beforeEncoderStages:MTLStageDispatch
                            visibilityOptions:MTL4VisibilityOptionDevice];
            const auto* records = static_cast<const Params*>(lease.linearizeParams.contents);
            for (std::size_t index = 1; index < lease.linearizeParams.length / sizeof(Params);
                 ++index) {
                const Rect strip{records[index].dispatchOrigin[0], records[index].dispatchOrigin[1],
                                 records[index].dispatchExtent[0], records[index].dispatchExtent[1]};
                if (!strip.width || !strip.height) continue;
                dispatchFsrPassMetal4(lease, encoder, frame.feature->paddingPipeline,
                                      lease.linearizeParams, lease.linearColor, lease.linearColor,
                                      nil, strip, nil, index * sizeof(Params));
            }
        }
        [encoder updateFence:fence afterEncoderStages:stages];
        [encoder endEncoding];
        return true;
    } @catch (id) {
        @try { [encoder endEncoding]; } @catch (id) {}
        return false;
    }
}

bool encodeFsrFinishMetal4(const PreparedFrame::Impl& frame, ExecutionLease::Impl& lease,
                           id<MTL4CommandBuffer> command, id<MTLFence> fence)
                           API_AVAILABLE(macos(26.0)) {
    if (!lease.finishParams) return true;
    if (lease.debugParams)
        return encodeFsrPassMetal4(lease, command, fence,
            isDepthPixelFormat(frame.depth.pixelFormat)
                ? frame.feature->debugDepthPipeline : frame.feature->debugPipeline,
            lease.debugParams, lease.privateOutput, frame.depth, frame.motion, frame.frame.outputRect, frame.output);
    return encodeFsrPassMetal4(lease, command, fence, frame.feature->finishPipeline,
                               lease.finishParams, lease.privateOutput, frame.output,
                               lease.convertedExposure ? lease.convertedExposure :
                                   (frame.exposure ? frame.exposure : lease.privateOutput),
                               frame.frame.outputRect, lease.alphaOutput);
}

bool copyOutputMetal4(const PreparedFrame::Impl& frame,
                      ExecutionLease::Impl& lease,
                      id<MTL4CommandBuffer> command,
                      id<MTLFence> fence) API_AVAILABLE(macos(26.0)) {
    if (!lease.privateOutput || lease.finishParams) return true;
    id<MTL4ComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) return false;
    const Rect& rect = frame.frame.outputRect;
    @try {
        [encoder waitForFence:fence beforeEncoderStages:MTLStageBlit];
        [encoder copyFromTexture:lease.privateOutput
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(frame.temporalOutputWidth,
                                             frame.temporalOutputHeight, 1)
                       toTexture:frame.output
                destinationSlice:0
                destinationLevel:0
               destinationOrigin:MTLOriginMake(rect.x + frame.placementX,
                                                rect.y + frame.placementY, 0)];
        [encoder updateFence:fence afterEncoderStages:MTLStageBlit];
        [encoder endEncoding];
        return true;
    } @catch (id) {
        @try { [encoder endEncoding]; } @catch (id) {}
        return false;
    }
}

// The cached scaler retains every texture and fence assigned for a frame. The
// execution lease owns them for GPU lifetime, so drop the scaler's references
// on every exit from encode, including failures and exceptions.
class ScalerBindings final {
public:
    explicit ScalerBindings(id scaler) noexcept : scaler_(scaler) {}
    ScalerBindings(const ScalerBindings&) = delete;
    ScalerBindings& operator=(const ScalerBindings&) = delete;
    ~ScalerBindings() {
        @try {
            [scaler_ setColorTexture:nil];
            [scaler_ setDepthTexture:nil];
            [scaler_ setMotionTexture:nil];
            [scaler_ setOutputTexture:nil];
            [scaler_ setExposureTexture:nil];
            if (@available(macOS 27.0, *)) [scaler_ setReactiveMaskTexture:nil];
            [scaler_ setFence:nil];
        } @catch (id) {
        }
    }
private:
    id scaler_;
};

void configureScalerForFrame(Feature::Impl& feature, ScalerGeneration& generation,
                             const PreparedFrame::Impl& frame,
                             ExecutionLease::Impl& lease, id<MTLFence> fence,
                             bool effectiveReset, bool alpha = false) {
    id scaler = alpha ? generation.alphaScaler : generation.scaler;
    [scaler setColorTexture:alpha ? lease.alphaColor : (lease.linearColor ? lease.linearColor :
                                (lease.stagedColor ? lease.stagedColor : frame.color))];
    [scaler setDepthTexture:lease.stagedDepth ? lease.stagedDepth : frame.depth];
    [scaler setMotionTexture:lease.stagedMotion ? lease.stagedMotion : frame.motion];
    [scaler setOutputTexture:alpha ? lease.alphaOutput : (lease.privateOutput ? lease.privateOutput : frame.output)];
    [scaler setExposureTexture:!alpha && frame.frame.exposureMode == ExposureMode::Texture
                                   ? (lease.convertedExposure ? lease.convertedExposure : frame.exposure)
                                   : nil];
    if (@available(macOS 27.0, *))
        [scaler setReactiveMaskTexture:lease.combinedMask ? lease.combinedMask :
                                        frame.directMask ? frame.directMask :
                                        (lease.stagedReactive ? lease.stagedReactive : frame.reactive)];
    [scaler setInputContentWidth:frame.frame.inputContent.width];
    [scaler setInputContentHeight:frame.frame.inputContent.height];
    if (@available(macOS 27.0, *)) {
        [scaler setColorContentOffsetX:alpha || lease.linearColor || lease.stagedColor ? 0 : frame.frame.colorRect.x];
        [scaler setColorContentOffsetY:alpha || lease.linearColor || lease.stagedColor ? 0 : frame.frame.colorRect.y];
        [scaler setDepthContentOffsetX:lease.stagedDepth ? 0 : frame.frame.depthRect.x];
        [scaler setDepthContentOffsetY:lease.stagedDepth ? 0 : frame.frame.depthRect.y];
        [scaler setMotionContentOffsetX:lease.stagedMotion ? 0 : frame.scalerMotionRect.x];
        [scaler setMotionContentOffsetY:lease.stagedMotion ? 0 : frame.scalerMotionRect.y];
        const Rect& reactiveRect = frame.directMask ? frame.directMaskRect : frame.frame.reactiveRect;
        [scaler setReactiveMaskContentOffsetX:lease.combinedMask || lease.stagedReactive ? 0 : reactiveRect.x];
        [scaler setReactiveMaskContentOffsetY:lease.combinedMask || lease.stagedReactive ? 0 : reactiveRect.y];
        const bool directOutput = !alpha && !lease.privateOutput;
        [scaler setOutputOffsetX:directOutput ? frame.frame.outputRect.x : 0];
        [scaler setOutputOffsetY:directOutput ? frame.frame.outputRect.y : 0];
    }
    [scaler setPreExposure:alpha ? 1.0f : frame.frame.preExposure];
    [scaler setJitterOffsetX:frame.frame.jitterOffsetX];
    [scaler setJitterOffsetY:frame.frame.jitterOffsetY];
    [scaler setMotionVectorScaleX:frame.frame.motionVectorScaleX];
    [scaler setMotionVectorScaleY:frame.frame.motionVectorScaleY];
    [scaler setReset:effectiveReset ? YES : NO];
    [scaler setDepthReversed:feature.create.depthInverted() ? YES : NO];
    [scaler setFence:fence];
}

} // namespace

Feature::Feature(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Feature::~Feature() = default;

std::shared_ptr<Feature> Feature::create(const CreateContext& context,
                                         const CreateInfo& info,
                                         Error* error) noexcept {
    clearError(error);
    if (!validateCreate(context, info, error)) return {};
    try {
        auto impl = std::make_shared<Impl>();
        impl->create = info;
        impl->device = retainObject(asDevice(context.device));
        impl->compiler = retainObject(asCompiler(context.compiler));
        @try {
            impl->minScale = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:impl->device];
            impl->maxScale = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:impl->device];
        } @catch (id) {
            setError(error, ErrorCode::InvalidContext, "MetalFX dynamic-scale query raised an exception");
            return {};
        }
        if (!finite(impl->minScale) || !finite(impl->maxScale) || impl->minScale <= 0.0f ||
            impl->maxScale < impl->minScale) {
            setError(error, ErrorCode::InvalidContext, "MetalFX returned an invalid dynamic-scale range");
            return {};
        }
        return std::shared_ptr<Feature>(new Feature(std::move(impl)));
    } catch (...) {
        setError(error, ErrorCode::ResourceCreationFailed, "failed to allocate MetalFX feature state");
        return {};
    }
}

void Feature::markDormant() noexcept {
    if (impl_) impl_->markDormant();
}

std::shared_ptr<const PreparedFrame> Feature::prepare(
    const FrameInfo& info, const TextureSet& textures, Error* error,
    const FrameOperations& operations) noexcept {
    clearError(error);
    if (!impl_ || !validateFrameScalars(info, error) ||
        !validateFrameTextures(*impl_, info, textures, operations, error)) {
        return {};
    }

    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const TemporalOutputLayout temporal = temporalOutputLayout(*impl_, info, operations);
        std::shared_ptr<ScalerGeneration> generation =
            ensureScaler(*impl_, info, textures, operations, error);
        if (!generation) return {};

        id<MTLTexture> color = asTexture(textures.color);
        id<MTLTexture> depth = asTexture(textures.depth);
        id<MTLTexture> motion = asTexture(textures.motion);
        id<MTLTexture> output = asTexture(textures.output);
        id<MTLTexture> exposure = info.exposureMode == ExposureMode::Texture
                                      ? asTexture(textures.exposure)
                                      : nil;
        id<MTLTexture> reactive = asTexture(textures.reactive);
        id<MTLTexture> composition = asTexture(textures.composition);

        const bool exposureConversion = exposure && requiresExposureConversion(exposure);
        if (exposureConversion && !ensureExposurePipeline(*impl_, error)) return {};

        auto frame = std::make_shared<PreparedFrame::Impl>();
        frame->feature = impl_;
        frame->self = frame;
        frame->generation = generation;
        frame->frame = info;
        frame->activation = generation->activation;
        frame->textures = textures;
        frame->operations = operations;
        if (info.exposureMode != ExposureMode::Texture) frame->textures.exposure = nullptr;
        frame->color = retainObject(color);
        frame->depth = retainObject(depth);
        frame->motion = retainObject(motion);
        frame->output = retainObject(output);
        frame->exposure = retainObject(exposure);
        frame->reactive = retainObject(reactive);
        frame->composition = retainObject(composition);
        frame->temporalOutputWidth = temporal.width;
        frame->temporalOutputHeight = temporal.height;
        frame->placementX = temporal.placementX;
        frame->placementY = temporal.placementY;
        frame->cappedOutput = temporal.capped;
        const auto exactTexture = [](id<MTLTexture> texture, const Rect& rect,
                                     NSUInteger width, NSUInteger height,
                                     MTLTextureUsage usage) noexcept {
            return rect.x == 0 && rect.y == 0 && rect.width == width && rect.height == height &&
                   texture.width == width && texture.height == height && hasUsage(texture, usage);
        };
        frame->scalerMotionRect = info.motionRect;
        if (!impl_->create.lowResolutionMotionVectors()) {
            frame->scalerMotionRect.x += temporal.placementX;
            frame->scalerMotionRect.y += temporal.placementY;
            frame->scalerMotionRect.width = temporal.width;
            frame->scalerMotionRect.height = temporal.height;
        }
        const Rect activeInput{0, 0, info.inputContent.width, info.inputContent.height};
        const bool smallerThanInputCapacity =
            activeInput.width != generation->inputCapacityWidth ||
            activeInput.height != generation->inputCapacityHeight;
        // Inputs smaller than the descriptor capacity need replicated edges: macOS 27
        // Metal4FX still reads color beyond inputContent (outside pixels change output).
        frame->needsColorStaging = operations.colorTransfer == ColorTransfer::Linear &&
            (smallerThanInputCapacity ||
             !exactDescriptorInput(color, info.colorRect, activeInput.width, activeInput.height,
                                   generation->inputCapacityWidth, generation->inputCapacityHeight,
                                   generation->colorUsage));
        frame->needsDepthStaging = smallerThanInputCapacity ||
            !exactDescriptorInput(depth, info.depthRect, activeInput.width, activeInput.height,
                                  generation->inputCapacityWidth, generation->inputCapacityHeight,
                                  generation->depthUsage);
        frame->needsMotionStaging = impl_->create.lowResolutionMotionVectors()
            ? (smallerThanInputCapacity ||
               !exactDescriptorInput(motion, frame->scalerMotionRect,
                                     activeInput.width, activeInput.height,
                                     generation->inputCapacityWidth, generation->inputCapacityHeight,
                                     generation->motionUsage))
            : !exactTexture(motion, frame->scalerMotionRect,
                            frame->scalerMotionRect.width, frame->scalerMotionRect.height,
                            generation->motionUsage);
        frame->needsReactiveStaging = reactive && !operations.combineCompositionMask &&
            (smallerThanInputCapacity ||
             !exactDescriptorInput(reactive, info.reactiveRect, activeInput.width, activeInput.height,
                                   generation->inputCapacityWidth, generation->inputCapacityHeight,
                                   generation->reactiveUsage));
        // macOS 27 MetalFX writes only the outputOffset window of a larger output
        // texture and preserves the texels outside it, so an uncapped validated
        // subrect is written in place; older runtimes need exact output dimensions.
        bool outputOffsets = false;
        if (@available(macOS 27.0, *)) outputOffsets = true;
        const bool directOutput = outputOffsets
            ? info.outputRect.width == temporal.width && info.outputRect.height == temporal.height &&
                  hasUsage(output, generation->outputUsage)
            : exactTexture(output, info.outputRect, temporal.width, temporal.height,
                           generation->outputUsage);
        frame->needsOutputShadow = temporal.capped ||
                                   operations.drawDebugView || generation->alphaScaler ||
                                   operations.colorTransfer != ColorTransfer::Linear ||
                                   operations.sharpening ||
                                   output.storageMode != MTLStorageModePrivate || !directOutput;
        frame->needsExposureConversion = exposureConversion;

        // Bind an exact R8 composition mask directly when nothing must be combined:
        // no reactive mask, or a reactive mask that is the same view and rect.
        if (operations.combineCompositionMask && !smallerThanInputCapacity) {
            const bool sameMask = reactive && reactive == composition &&
                info.reactiveRect.x == info.compositionRect.x &&
                info.reactiveRect.y == info.compositionRect.y;
            id<MTLTexture> mask = !reactive ? composition : (sameMask ? reactive : nil);
            if (mask && mask.pixelFormat == MTLPixelFormatR8Unorm &&
                exactDescriptorInput(mask, info.compositionRect, activeInput.width, activeInput.height,
                                     generation->inputCapacityWidth, generation->inputCapacityHeight,
                                     generation->reactiveUsage)) {
                frame->directMask = mask;
                frame->directMaskRect = info.compositionRect;
            }
        }

        // Pad copied inputs with one compute pass for formats whose shader
        // read/write round trip is exact; depth and others keep blit padding.
        if (smallerThanInputCapacity) {
            const auto readable = [](id<MTLTexture> texture) {
                return hasUsage(texture, MTLTextureUsageShaderRead);
            };
            unsigned candidates = 0;
            if (frame->needsColorStaging && readable(color) &&
                (color.pixelFormat == MTLPixelFormatRGBA16Float ||
                 color.pixelFormat == MTLPixelFormatRGBA8Unorm))
                candidates |= 1u;
            if (frame->needsDepthStaging && readable(depth) &&
                depth.pixelFormat == MTLPixelFormatR32Float)
                candidates |= 2u;
            if (frame->needsMotionStaging && impl_->create.lowResolutionMotionVectors() &&
                readable(motion) && motion.pixelFormat == MTLPixelFormatRG16Float)
                candidates |= 4u;
            if (frame->needsReactiveStaging && readable(reactive) &&
                reactive.pixelFormat == MTLPixelFormatR8Unorm)
                candidates |= 8u;
            const bool half = (candidates & 4u) ||
                ((candidates & 1u) && color.pixelFormat == MTLPixelFormatRGBA16Float);
            const bool single = (candidates & (2u | 8u)) ||
                ((candidates & 1u) && color.pixelFormat == MTLPixelFormatRGBA8Unorm);
            // A missing compute pipeline keeps that input on the blit fallback.
            if ((half && !impl_->copyHalfPipeline) || (single && !impl_->copyFloatPipeline))
                ensureFsrLibrary(*impl_);
            if (half && !ensureFsrPipeline(*impl_, true, impl_->copyHalfPipeline,
                                           @"yaagl_sr_copy_pad_half")) {
                candidates &= ~4u;
                if (color.pixelFormat == MTLPixelFormatRGBA16Float) candidates &= ~1u;
            }
            if (single && !ensureFsrPipeline(*impl_, true, impl_->copyFloatPipeline,
                                             @"yaagl_sr_copy_pad_float")) {
                candidates &= ~(2u | 8u);
                if (color.pixelFormat == MTLPixelFormatRGBA8Unorm) candidates &= ~1u;
            }
            frame->computeCopyMask = candidates;
        }

        FsrPipelineNeeds needs;
        needs.linearize = operations.colorTransfer != ColorTransfer::Linear;
        needs.combineMasks = operations.combineCompositionMask && !frame->directMask;
        needs.alpha = generation->alphaScaler != nil;
        needs.debug = operations.drawDebugView;
        needs.debugDepth = isDepthPixelFormat(depth.pixelFormat);
        // The debug view replaces the finish pass.
        needs.finish = !needs.debug &&
                       (needs.linearize || operations.sharpening || temporal.capped || needs.alpha);
        needs.padding = needs.linearize && smallerThanInputCapacity;
        // Both passes read the same color subrect. Keep alpha linear and the
        // existing converted-color padding; only the redundant read/dispatch goes.
        needs.linearizeAlpha = needs.linearize && needs.alpha;
        if (needs.linearizeAlpha) needs.linearize = needs.alpha = false;
        if (!ensureFsrPipelines(*impl_, needs, error)) return {};

        // Pre-create one complete execution resource set. This makes the first
        // Evaluate fail synchronously on output/exposure/residency allocation
        // instead of recording a command that can only leave stale output.
        frame->firstLease = makeLease(*frame, error);
        if (!frame->firstLease) return {};

        return std::shared_ptr<const PreparedFrame>(
            new PreparedFrame(std::move(frame)));
    } catch (...) {
        setError(error, ErrorCode::ResourceCreationFailed,
                 "failed to prepare immutable MetalFX frame state");
        return {};
    }
}

PreparedFrame::PreparedFrame(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PreparedFrame::~PreparedFrame() = default;

std::uint64_t PreparedFrame::generationId() const noexcept {
    return impl_ ? impl_->generation->serial : 0;
}

bool PreparedFrame::encode(void* commandBuffer, void* fencePointer,
                           std::shared_ptr<const ExecutionLease>& leaseResult,
                           Error* error, bool forceReset, void* splitContext,
                           void* (*splitCommandBuffer)(void*)) const noexcept {
    clearError(error);
    leaseResult.reset();
    if (!impl_ || !impl_->feature || !commandBuffer || !fencePointer) {
        setError(error, ErrorCode::InvalidContext, "MetalFX encode is missing command buffer or fence");
        return false;
    }

    try {
        std::shared_ptr<ExecutionLease::Impl> lease;
        {
            std::lock_guard<std::mutex> lock(impl_->firstLeaseMutex);
            lease = std::move(impl_->firstLease);
        }
        if (!lease) {
            lease = makeLease(*impl_, error);
            if (!lease) return false;
        }
        lease->generation = impl_->generation;
        lease->fence = retainObject(asFence(fencePointer));

        // Publish ownership before recording any command. Keep this non-null
        // on every later failure so GPU-captured scratch cannot be released.
        auto publicLease = std::shared_ptr<ExecutionLease>(new ExecutionLease(lease));
        leaseResult = publicLease;

        Feature::Impl& feature = *impl_->feature;
        ScalerGeneration& generation = *impl_->generation;
        std::lock_guard<std::mutex> lock(generation.encodeMutex);
        const ScalerBindings bindings(generation.scaler);
        const ScalerBindings alphaBindings(generation.alphaScaler);
        const bool inputExtentChanged = generation.hasLastInputContent &&
            (generation.lastInputContentWidth != impl_->frame.inputContent.width ||
             generation.lastInputContentHeight != impl_->frame.inputContent.height);
        const bool effectiveReset = forceReset || impl_->frame.resetHistory ||
                                    generation.encodedActivation != impl_->activation ||
                                    inputExtentChanged;
        @try {
            if (@available(macOS 26.0, *)) {
                id<MTL4CommandBuffer> command =
                    reinterpret_cast<id<MTL4CommandBuffer>>(commandBuffer);
                id<MTLResidencySet> residency =
                    reinterpret_cast<id<MTLResidencySet>>(lease->residency);
                if (command.device != feature.device) {
                    setError(error, ErrorCode::InvalidContext,
                             "Metal4 command buffer belongs to a different device");
                    return false;
                }
                if (residency) [command useResidencySet:residency];
                if (!encodePrepassMetal4(*impl_, *lease, command, lease->fence)) {
                    setError(error, ErrorCode::EncodeFailed,
                             "failed to encode MetalFX input preprocessing on Metal4");
                    return false;
                }
                if (!splitCommandBuffer) {
                    setError(error, ErrorCode::InvalidContext, "Metal4 temporal scaling requires recording segments");
                    return false;
                }
                id<MTL4ComputeCommandEncoder> prefixBoundary = [command computeCommandEncoder];
                if (!prefixBoundary) {
                    setError(error, ErrorCode::EncodeFailed, "failed to create temporal prefix barrier");
                    return false;
                }
                [prefixBoundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                                  visibilityOptions:MTL4VisibilityOptionDevice];
                [prefixBoundary endEncoding];
                // Keep MetalFX in its own native command buffer. An external
                // fence consumer in the same buffer can deadlock Metal4FX;
                // Explicit queue producer barriers supply execution ordering
                // and device visibility across the native recording segments.
                command = reinterpret_cast<id<MTL4CommandBuffer>>(splitCommandBuffer(splitContext));
                if (!command) {
                    setError(error, ErrorCode::EncodeFailed, "failed to split temporal preprocessing segment");
                    return false;
                }
                if (residency) [command useResidencySet:residency];
                configureScalerForFrame(feature, generation, *impl_, *lease, nil, effectiveReset);
                [generation.scaler encodeToCommandBuffer:command];
                if (generation.alphaScaler) {
                    command = reinterpret_cast<id<MTL4CommandBuffer>>(splitCommandBuffer(splitContext));
                    if (!command) {
                        setError(error, ErrorCode::EncodeFailed, "failed to split temporal alpha segment");
                        return false;
                    }
                    if (residency) [command useResidencySet:residency];
                    configureScalerForFrame(feature, generation, *impl_, *lease, nil, effectiveReset, true);
                    [generation.alphaScaler encodeToCommandBuffer:command];
                }
                command = reinterpret_cast<id<MTL4CommandBuffer>>(splitCommandBuffer(splitContext));
                if (!command) {
                    setError(error, ErrorCode::EncodeFailed, "failed to split temporal output segment");
                    return false;
                }
                if (residency) [command useResidencySet:residency];
                id<MTL4ComputeCommandEncoder> suffixBoundary = [command computeCommandEncoder];
                if (!suffixBoundary) {
                    setError(error, ErrorCode::EncodeFailed, "failed to create temporal output barrier");
                    return false;
                }
                [suffixBoundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                                  visibilityOptions:MTL4VisibilityOptionDevice];
                [suffixBoundary endEncoding];
                if (!encodeFsrFinishMetal4(*impl_, *lease, command, lease->fence)) {
                    setError(error, ErrorCode::EncodeFailed,
                             "failed to encode FSR transfer/RCAS output on Metal4");
                    return false;
                }
                if (!copyOutputMetal4(*impl_, *lease, command, lease->fence)) {
                    setError(error, ErrorCode::EncodeFailed,
                             "failed to encode required MetalFX output subrect copy on Metal4");
                    return false;
                }
            } else {
                setError(error, ErrorCode::UnsupportedFeature,
                         "Metal4 MetalFX encode is unavailable on this OS");
                return false;
            }
        } @catch (id) {
            setError(error, ErrorCode::EncodeFailed,
                     "MetalFX temporal frame encoding raised an exception");
            return false;
        }

        generation.lastInputContentWidth = impl_->frame.inputContent.width;
        generation.lastInputContentHeight = impl_->frame.inputContent.height;
        generation.hasLastInputContent = true;
        generation.encodedActivation = impl_->activation;
        lease->replayEligible = true;
        return true;
    } catch (...) {
        setError(error, ErrorCode::EncodeFailed, "failed to retain MetalFX execution state");
        return false;
    }
}

ExecutionLease::ExecutionLease(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ExecutionLease::~ExecutionLease() = default;

} // namespace yaagl::pso::metalfx
