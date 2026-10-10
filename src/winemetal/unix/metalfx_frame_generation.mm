// FSR/NGX resource translation to native MetalFX; owned Metal4 transport.
#include "metalfx_frame_generation.hpp"
#include "../winemetal_metal4_fg.h"
#include "fsr-kernels.inc"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <objc/message.h>

#include <array>
extern "C" MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format);
static void retainFGLease(std::uint64_t, std::shared_ptr<const dxmt::framegeneration::ExecutionLease>);
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxmt::framegeneration {
namespace {



constexpr std::uint32_t Ok = 0;
constexpr std::uint32_t Runtime = 3;
constexpr std::uint32_t Unsupported = 4;
constexpr std::uint32_t Memory = 5;
constexpr std::uint32_t Parameter = 6;


std::atomic<std::uint64_t> nextContext{1};
std::atomic<unsigned> errorCount{0};
std::mutex registryMutex;

void logFailure(std::uint32_t operation, const char* stage, std::uint32_t result,
                std::uint32_t flags = 0, std::uint32_t width = 0,
                std::uint32_t height = 0, float cameraNear = 0,
                float cameraFar = 0, float fov = 0, float scale = 0) noexcept {
    if (errorCount.fetch_add(1, std::memory_order_relaxed) >= 120) return;
    std::fprintf(stderr,
        "dxmt-frame-generation: operation=%u stage=%s result=%u flags=0x%x "
        "input=%ux%u camera_near=%.9g camera_far=%.9g fov=%.9g scale=%.9g\n",
        operation, stage, result, flags, width, height, cameraNear, cameraFar, fov, scale);
}

template<class R, class... A>
R send(id object, SEL selector, A... arguments) {
    return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(
        object, selector, arguments...);
}

/*
 * Metal HUD adds its "Frame Interpolator: Enabled" row on the first MetalFX
 * interpolation encode and never removes it, although it has a removal method.
 * Call that method when this process stops interpolating so the row reflects
 * the current state; the next encode adds it back. The class exists only while
 * MTL_HUD_ENABLED loads libMTLHud, so this is a no-op without the HUD.
 */
void hideHudFrameInterpolator() noexcept {
    @try {
        Class service = NSClassFromString(@"MTLHUDService");
        SEL instance = NSSelectorFromString(@"instance");
        SEL disable = NSSelectorFromString(@"metalFXFrameInterpolatorDisable");
        if (!service || ![service respondsToSelector:instance]) return;
        id hud = send<id>((id)service, instance);
        if ([hud respondsToSelector:disable]) send<void>(hud, disable);
    } @catch (NSException*) {
    }
}

/*
 * The row is process-wide. `interpolating` counts contexts that have
 * interpolated since their last OFF; only a native FSR fallback never counts.
 * MetalFX reports each encode to the HUD asynchronously, when the frame reaches
 * its layer (measured at most one frame, 19 ms, later). A removal therefore
 * waits 500 ms and runs only if nothing counts and no encode outside a counted
 * context happened after it was scheduled; such an encode schedules its own.
 */
struct HudRow {
    std::mutex mutex;
    unsigned interpolating = 0;
    std::uint64_t activity = 0;
} hudRow;

// Call with hudRow.mutex held.
void scheduleHudRowRemoval() noexcept {
    const std::uint64_t activity = ++hudRow.activity;
    if (!NSClassFromString(@"MTLHUDService")) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            std::lock_guard lock(hudRow.mutex);
            if (!hudRow.interpolating && hudRow.activity == activity)
                hideHudFrameInterpolator();
        }
    });
}

// Call with the context's executionMutex held; `counted` is State::hudCounted.
void releaseHudRow(bool& counted) noexcept {
    if (!counted) return;
    counted = false;
    std::lock_guard lock(hudRow.mutex);
    if (!--hudRow.interpolating) scheduleHudRowRemoval();
}

class Object {
public:
    Object() noexcept = default;
    explicit Object(id value) noexcept : value_([value retain]) {}
    Object(const Object& other) noexcept : value_([other.value_ retain]) {}
    Object(Object&& other) noexcept : value_(other.value_) { other.value_ = nil; }
    ~Object() { [value_ release]; }
    Object& operator=(Object other) noexcept {
        std::swap(value_, other.value_);
        return *this;
    }
    id get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nil; }
private:
    id value_ = nil;
};

void* pointer(std::uint64_t value) noexcept { return reinterpret_cast<void*>(value); }
struct Command {
    WMTFGNativeCommand value{};
    bool open(void* list) { if (!list) return false; value = *static_cast<WMTFGNativeCommand*>(list); return value.device && value.compiler; }
};
struct Resources { struct Resource { void* texture = nullptr; }; std::array<Resource,5> values{}; };
MTLPixelFormat colorFormat(std::uint32_t format) noexcept { return to_metal_pixel_format(static_cast<WMTPixelFormat>(format)); }
bool textureValid(id<MTLTexture> texture, id<MTLDevice> device,
                  NSUInteger width, NSUInteger height) {
    return texture && texture.device == device &&
           texture.textureType == MTLTextureType2D &&
           texture.sampleCount == 1 && texture.width >= width &&
           texture.height >= height &&
           texture.storageMode != MTLStorageModeMemoryless;
}

Object privateTexture(id<MTLDevice> device, MTLPixelFormat format,
                      NSUInteger width, NSUInteger height, MTLTextureUsage usage) {
    MTLTextureDescriptor* descriptor =
        [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = format;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.arrayLength = 1;
    descriptor.sampleCount = 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = usage;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    [descriptor release];
    Object result(texture);
    [texture release];
    return result;
}

// FG has no public color offset selector; color/UI conversion stays staged.
bool exactDescriptorInput(id<MTLTexture> texture, MTLPixelFormat format,
                          NSUInteger width, NSUInteger height,
                          MTLTextureUsage usage, bool conversion) {
    return !conversion && texture.width == width && texture.height == height &&
           texture.pixelFormat == format && (texture.usage & usage) == usage;
}

// MetalFX retains the per-frame resources assigned to the cached interpolator.
// The execution lease owns them for GPU lifetime, so drop the interpolator's
// references on every exit from encode, including failures and exceptions.
class API_AVAILABLE(macos(26.0)) InterpolatorBindings final {
public:
    explicit InterpolatorBindings(id<MTLFXFrameInterpolatorBase> effect) noexcept
        : effect_(effect) {}
    InterpolatorBindings(const InterpolatorBindings&) = delete;
    InterpolatorBindings& operator=(const InterpolatorBindings&) = delete;
    ~InterpolatorBindings() {
        @try {
            effect_.colorTexture = nil;
            effect_.prevColorTexture = nil;
            effect_.depthTexture = nil;
            effect_.motionTexture = nil;
            effect_.uiTexture = nil;
            effect_.outputTexture = nil;
            if (@available(macOS 27.0, *)) effect_.distortionTexture = nil;
            effect_.fence = nil;
        } @catch (id) {
        }
    }
private:
    id<MTLFXFrameInterpolatorBase> effect_;
};

Object makePipeline(id<MTLDevice> device, NSString* name) {
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kFsrKernelsSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return {};
    id<MTLFunction> function = [library newFunctionWithName:name];
    [library release];
    if (!function) return {};
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    [function release];
    Object result(pipeline);
    [pipeline release];
    return result;
}

struct Configuration {
    Object device;
    Object compiler;
    Object descriptor;
    Object factory;
    MTLPixelFormat color = MTLPixelFormatInvalid;
    MTLPixelFormat ui = MTLPixelFormatInvalid;
    MTLPixelFormat sourceColor = MTLPixelFormatInvalid;
    MTLPixelFormat sourceUi = MTLPixelFormatInvalid;
    std::uint32_t transfer = 0;
    bool cameraPresent = false;
    bool distortionEnabled = false;
    MTLPixelFormat depth = MTLPixelFormatInvalid;
    MTLPixelFormat motion = MTLPixelFormatRG16Float;
    NSUInteger width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    Object decodePipeline;
    Object depthPipeline;
    Object motionPipeline;
    Object encodePipeline;

    Object makeInterpolator() const {
        if (@available(macOS 26.0, *)) {
            auto d = (MTLFXFrameInterpolatorDescriptor*)descriptor.get();
            id instance = (id)[d newFrameInterpolatorWithDevice:(id<MTLDevice>)device.get()
                                               compiler:(id<MTL4Compiler>)compiler.get()];
            Object result(instance);
            [instance release];
            return result;
        }
        return {};
    }
};

struct Snapshot {
    std::shared_ptr<Configuration> configuration;
    WMTFGprepare_packet parameters{};
    Object depth;
    Object motion;
    NSUInteger depthWidth = 0;
    NSUInteger depthHeight = 0;
    NSUInteger motionWidth = 0;
    NSUInteger motionHeight = 0;
    struct Production { bool produced = false; };
    std::shared_ptr<Production> production = std::make_shared<Production>();
    bool hasPreviousJitter = false;
    float previousJitterX = 0;
    float previousJitterY = 0;
};

struct State {
    std::vector<std::shared_ptr<Configuration>> configurations;
    std::mutex mutex;
    std::mutex executionMutex;
    Object device;
    WMTFGcreate_packet creation{};
    bool retired = false;
    // Pending provider selection records its first Prepare before PE replays Configure.
    bool generationEnabled = true;
    // Protected by executionMutex while encoding; configuration also holds mutex.
    std::uint64_t generationEpoch = 0;
    // Protected by executionMutex; set while this context holds a HudRow count.
    bool hudCounted = false;
    bool providerValidated = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<Snapshot>> frames;
    std::shared_ptr<Configuration> historyConfiguration;
    WMTFGdispatch_packet historyDispatch{};
    std::uint64_t historyFrame = 0;
    bool hasPreparedJitter = false;
    std::uint64_t preparedFrame = 0;
    float preparedJitterX = 0;
    float preparedJitterY = 0;
};

std::unordered_map<std::uint64_t, std::shared_ptr<State>> contexts;

std::shared_ptr<State> lookup(std::uint64_t context) {
    std::lock_guard lock(registryMutex);
    auto found = contexts.find(context);
    return found == contexts.end() ? nullptr : found->second;
}

bool supported(id<MTLDevice> device, std::uint64_t compiler) {
    if (@available(macOS 27.0, *)) {
        return device && compiler && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:device];
    }
    return false;
}

std::shared_ptr<Configuration> configure(
    State& state, const Command& command,
    const WMTFGprepare_packet& packet,
    id<MTLTexture> depth, MTLPixelFormat sourceColor, MTLPixelFormat sourceUi,
    NSUInteger outputWidth, NSUInteger outputHeight, std::uint32_t transfer, bool distortionEnabled) {
    if (@available(macOS 26.0, *)) {
        auto device = (id<MTLDevice>)command.value.device;
        if (!supported(device, command.value.compiler)) return {};
        const MTLPixelFormat color = MTLPixelFormatRGBA16Float;
        const MTLPixelFormat ui = sourceUi == MTLPixelFormatInvalid
                                ? MTLPixelFormatInvalid : MTLPixelFormatRGBA16Float;
        const MTLPixelFormat depthFormat = depth.pixelFormat;
        const MTLPixelFormat motion = MTLPixelFormatRG16Float;
        const bool cameraPresent = packet.camera_info_present != 0;
        // MetalFX consumes color, depth, and motion in one origin-zero active domain.
        for (const auto& cached : state.configurations) {
            if (cached->device.get() == device &&
                cached->color == color && cached->ui == ui &&
                cached->sourceColor == sourceColor && cached->sourceUi == sourceUi &&
                cached->transfer == transfer && cached->cameraPresent == cameraPresent &&
                cached->distortionEnabled == distortionEnabled &&
                cached->depth == depthFormat && cached->motion == motion &&
                cached->width == outputWidth && cached->height == outputHeight &&
                cached->outputWidth == outputWidth && cached->outputHeight == outputHeight)
                return cached;
        }

        auto result = std::make_shared<Configuration>();
        result->device = Object(device);
        result->compiler = Object((id)command.value.compiler);
        result->color = color;
        result->ui = ui;
        result->sourceColor = sourceColor;
        result->sourceUi = sourceUi;
        result->transfer = transfer;
        result->cameraPresent = cameraPresent;
        result->distortionEnabled = distortionEnabled;
        result->depth = depthFormat;
        result->motion = motion;
        result->width = outputWidth;
        result->height = outputHeight;
        result->outputWidth = outputWidth;
        result->outputHeight = outputHeight;

        MTLFXFrameInterpolatorDescriptor* descriptor =
            [[MTLFXFrameInterpolatorDescriptor alloc] init];
        result->descriptor = Object(descriptor);
        [descriptor release];
        descriptor.colorTextureFormat = result->color;
        descriptor.outputTextureFormat = result->color;
        descriptor.depthTextureFormat = result->depth;
        descriptor.motionTextureFormat = result->motion;
        descriptor.uiTextureFormat = result->ui;
        descriptor.inputWidth = result->width;
        descriptor.inputHeight = result->height;
        descriptor.outputWidth = result->outputWidth;
        descriptor.outputHeight = result->outputHeight;
        if (@available(macOS 27.0, *)) {
            descriptor.requiresPrevColorTexture = NO;
            descriptor.distortionTextureEnabled = distortionEnabled;
        } else if (distortionEnabled) return {};

        result->factory = result->makeInterpolator();
        if (!result->factory) return {};
        result->decodePipeline = makePipeline(device, @"yaagl_fg_decode_crop");
        result->depthPipeline = makePipeline(device, @"yaagl_fg_resample_depth");
        result->motionPipeline = makePipeline(device, @"yaagl_fg_normalize_motion");
        result->encodePipeline = makePipeline(device, @"yaagl_fg_encode_scatter");
        if (!result->decodePipeline || !result->depthPipeline || !result->motionPipeline ||
            !result->encodePipeline)
            return {};
        constexpr std::size_t configurationCacheLimit = 8;
        if (state.configurations.size() == configurationCacheLimit)
            state.configurations.erase(state.configurations.begin());
        state.configurations.push_back(result);
        return result;
    }
    return {};
}

bool mapOne(Resources& resources, unsigned index, const State& state,
            const Command& command, std::uint64_t address, bool output,
            NSUInteger width, NSUInteger height) {
    auto texture = (id<MTLTexture>)address;
    if (index >= resources.values.size() || !textureValid(texture, (id<MTLDevice>)command.value.device, width, height) ||
        command.value.device != reinterpret_cast<std::uint64_t>(state.device.get())) return false;
    resources.values[index].texture = (void*)texture;
    return true;
}

id<MTLTexture> rootTexture(id<MTLTexture> texture) {
    while (texture.parentTexture) texture = texture.parentTexture;
    return texture;
}

bool mapPair(Resources& resources, const State& state, const Command& command,
             std::uint64_t first, std::uint64_t second, bool output,
             NSUInteger width, NSUInteger height) {
    if (!first || !second || first == second ||
        !mapOne(resources, 0, state, command, first, false, width, height) ||
        !mapOne(resources, 1, state, command, second, output, width, height))
        return false;
    return rootTexture((id<MTLTexture>)resources.values[0].texture) !=
           rootTexture((id<MTLTexture>)resources.values[1].texture);
}

struct CameraPlanes {
    float nearPlane;
    float farPlane;
};

bool normalizeCameraPlanes(const WMTFGprepare_packet& p,
                           std::uint32_t createFlags, CameraPlanes& planes) noexcept {
    if (!std::isfinite(p.camera_near) || p.camera_near <= 0) return false;
    if (createFlags & WMTFG_DEPTH_INFINITE) {
        planes = {p.camera_near, INFINITY};
        return true;
    }
    if (!std::isfinite(p.camera_far) || p.camera_far <= 0 ||
        p.camera_far == p.camera_near)
        return false;
    planes = {std::min(p.camera_near, p.camera_far),
              std::max(p.camera_near, p.camera_far)};
    return true;
}

bool validParameters(const WMTFGprepare_packet& p, std::uint32_t createFlags) {
    CameraPlanes planes{};
    if ((p.flags & ~31u) || !p.render_width || !p.render_height ||
        !std::isfinite(p.jitter_x) || !std::isfinite(p.jitter_y) ||
        !std::isfinite(p.motion_scale_x) || !std::isfinite(p.motion_scale_y) ||
        !std::isfinite(p.frame_time_delta_ms) || p.frame_time_delta_ms <= 0 ||
        !std::isfinite(p.view_space_to_meters) ||
        !normalizeCameraPlanes(p, createFlags, planes) ||
        !std::isfinite(p.camera_fov_vertical_radians) ||
        p.camera_fov_vertical_radians <= 0 ||
        p.camera_fov_vertical_radians >= 3.14159265358979323846f)
        return false;
    if (!p.camera_info_present) return true;
    simd_float3 basis[3]{};
    for (unsigned i = 0; i != 3; ++i) {
        if (!std::isfinite(p.camera_position[i]) || !std::isfinite(p.camera_up[i]) ||
            !std::isfinite(p.camera_right[i]) || !std::isfinite(p.camera_forward[i]))
            return false;
        basis[0][i] = p.camera_up[i];
        basis[1][i] = p.camera_right[i];
        basis[2][i] = p.camera_forward[i];
    }
    constexpr float tolerance = 1.0e-3f;
    for (const auto& axis : basis)
        if (std::abs(simd_length(axis) - 1.0f) > tolerance) return false;
    return std::abs(simd_dot(basis[0], basis[1])) <= tolerance &&
           std::abs(simd_dot(basis[0], basis[2])) <= tolerance &&
           std::abs(simd_dot(basis[1], basis[2])) <= tolerance;
}

bool validTransfer(const WMTFGdispatch_packet& p) {
    if (p.backbuffer_transfer_function > 2 ||
        !std::isfinite(p.min_luminance) || !std::isfinite(p.max_luminance))
        return false;
    if (p.backbuffer_transfer_function == 1)
        return p.max_luminance > 0;
    if (p.backbuffer_transfer_function == 2)
        return p.max_luminance > p.min_luminance;
    return true;
}

bool resolveRect(const WMTFGdispatch_packet& p, NSUInteger displayWidth,
                 NSUInteger displayHeight, std::int32_t& left, std::int32_t& top,
                 NSUInteger& width, NSUInteger& height) {
    const bool defaultRect = p.generation_rect_width == 0 &&
                             p.generation_rect_height == 0;
    if (defaultRect) {
        if (p.generation_rect_left || p.generation_rect_top) return false;
        left = top = 0; width = displayWidth; height = displayHeight;
        return true;
    }
    if (p.generation_rect_left < 0 || p.generation_rect_top < 0 ||
        p.generation_rect_width <= 0 || p.generation_rect_height <= 0)
        return false;
    const std::uint64_t right = std::uint64_t(p.generation_rect_left) +
                                std::uint64_t(p.generation_rect_width);
    const std::uint64_t bottom = std::uint64_t(p.generation_rect_top) +
                                 std::uint64_t(p.generation_rect_height);
    if (right > displayWidth || bottom > displayHeight) return false;
    left = p.generation_rect_left; top = p.generation_rect_top;
    width = p.generation_rect_width; height = p.generation_rect_height;
    return true;
}

struct Copy {
    id<MTLTexture> source;
    id<MTLTexture> destination;
    NSUInteger width, height;
    NSUInteger sourceX = 0, sourceY = 0;
    NSUInteger destinationX = 0, destinationY = 0;
};

bool copyTextures(id buffer, id fence, const Copy* copies,
                  std::size_t count, std::vector<Object>& retained) {
    if (@available(macOS 26.0, *)) {
    id encoder = send<id>(buffer, sel_registerName("computeCommandEncoder"));
    if (!encoder) return false;
    retained.emplace_back(encoder);
    if (fence) {
            [(id<MTL4ComputeCommandEncoder>)encoder waitForFence:(id<MTLFence>)fence
                                             beforeEncoderStages:MTLStageBlit];
    }
    for (std::size_t i = 0; i != count; ++i) {
        const auto& copy = copies[i];
        send<void>(encoder,
            sel_registerName("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:"
                             "sourceSize:toTexture:destinationSlice:destinationLevel:"
                             "destinationOrigin:"),
            copy.source, NSUInteger(0), NSUInteger(0),
            MTLOriginMake(copy.sourceX, copy.sourceY, 0),
            MTLSizeMake(copy.width, copy.height, 1), copy.destination,
            NSUInteger(0), NSUInteger(0),
            MTLOriginMake(copy.destinationX, copy.destinationY, 0));
    }
    if (fence) {
            [(id<MTL4ComputeCommandEncoder>)encoder updateFence:(id<MTLFence>)fence
                                             afterEncoderStages:MTLStageBlit];
    }
    send<void>(encoder, sel_registerName("endEncoding"));
    return true;
    }
    return false;
}

struct alignas(16) FgParams {
    std::int32_t sourceOrigin[2];
    std::uint32_t extent[2];
    std::uint32_t sourceExtent[2];
    std::uint32_t motionTargetExtent[2];
    std::uint32_t outputExtent[2];
    float motionScale[2];
    float jitterCancellation[2];
    float minLuminance;
    float maxLuminance;
    std::uint32_t transfer;
    std::uint32_t debugFlags;
    std::uint32_t reset;
};

Object makeParameterBuffer(id<MTLDevice> device, const FgParams& params) {
    id<MTLBuffer> buffer =
        [device newBufferWithBytes:&params length:sizeof(params)
                           options:MTLResourceStorageModeShared];
    Object result(buffer);
    [buffer release];
    return result;
}

bool computePass(id buffer, id fence, id<MTLDevice> device,
                 id<MTLComputePipelineState> pipeline, id<MTLTexture> source,
                 id<MTLTexture> destination, id<MTLBuffer> parameterBuffer,
                 const FgParams& params, std::vector<Object>& retained,
                 id<MTLTexture> depth = nil, id<MTLTexture> motion = nil) {
    if (!pipeline || !source || !destination || !parameterBuffer) return false;
    if (@available(macOS 26.0, *)) {
        {
            MTL4ArgumentTableDescriptor* descriptor = [MTL4ArgumentTableDescriptor new];
            descriptor.maxBufferBindCount = 1;
            descriptor.maxTextureBindCount = depth ? 4 : 2;
            descriptor.initializeBindings = YES;
            NSError* error = nil;
            id<MTL4ArgumentTable> table =
                [device newArgumentTableWithDescriptor:descriptor error:&error];
            [descriptor release];
            if (!table) return false;
            retained.emplace_back(table);
            [table release];
            [table setAddress:parameterBuffer.gpuAddress atIndex:0];
            [table setTexture:source.gpuResourceID atIndex:0];
            [table setTexture:destination.gpuResourceID atIndex:1];
            if (depth) [table setTexture:depth.gpuResourceID atIndex:2];
            if (motion) [table setTexture:motion.gpuResourceID atIndex:3];
            id<MTL4ComputeCommandEncoder> encoder =
                [(id<MTL4CommandBuffer>)buffer computeCommandEncoder];
            if (!encoder) return false;
            retained.emplace_back(encoder);
            [encoder waitForFence:(id<MTLFence>)fence beforeEncoderStages:MTLStageDispatch];
            [encoder setComputePipelineState:pipeline];
            [encoder setArgumentTable:table];
            [encoder dispatchThreads:MTLSizeMake(params.extent[0], params.extent[1], 1)
                 threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [encoder updateFence:(id<MTLFence>)fence afterEncoderStages:MTLStageDispatch];
            [encoder endEncoding];
            return true;
        }
    }
    return false;
}

bool sameHistoryValues(const WMTFGdispatch_packet& a,
                       const WMTFGdispatch_packet& b) {
    return a.generation_rect_left == b.generation_rect_left &&
           a.generation_rect_top == b.generation_rect_top &&
           a.generation_rect_width == b.generation_rect_width &&
           a.generation_rect_height == b.generation_rect_height &&
           a.backbuffer_transfer_function == b.backbuffer_transfer_function &&
           // SDR decode/encode does not use luminance metadata.
           (a.backbuffer_transfer_function == 0 ||
            (a.min_luminance == b.min_luminance && a.max_luminance == b.max_luminance));
}

} // namespace

struct PreparedFrame::Impl {
    enum class Kind { Snapshot, Generate };
    Kind kind = Kind::Snapshot;
    std::shared_ptr<State> state;
    std::shared_ptr<Snapshot> snapshot;
    Object first;
    Object second;
    Object third;
    Object realOutput;
    Object distortion;
    WMTFGdispatch_packet dispatch{};
    bool reset = false;
    std::uint64_t generationEpoch = 0;
};

struct ExecutionLease::Impl {
    std::shared_ptr<PreparedFrame::Impl> prepared;
    std::vector<Object> objects;
    Object residency;

    ~Impl() {
        if (residency) {
            if (@available(macOS 15.0, *))
                [(id<MTLResidencySet>)residency.get() endResidency];
        }
    }

    bool makeResident(id buffer) {
        if (@available(macOS 26.0, *)) {
            auto device = (id<MTLDevice>)prepared->snapshot->configuration->device.get();
            MTLResidencySetDescriptor* descriptor =
                [[MTLResidencySetDescriptor alloc] init];
            descriptor.initialCapacity = objects.size();
            NSError* error = nil;
            id<MTLResidencySet> set =
                [device newResidencySetWithDescriptor:descriptor error:&error];
            [descriptor release];
            if (!set) return false;
            residency = Object(set);
            [set release];
            for (const auto& object : objects)
                if ([object.get() conformsToProtocol:@protocol(MTLAllocation)])
                    [set addAllocation:(id<MTLAllocation>)object.get()];
            [set commit];
            [set requestResidency];
            [(id<MTL4CommandBuffer>)buffer useResidencySet:set];
            return true;
        }
        return false;
    }
};

PreparedFrame::PreparedFrame(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PreparedFrame::~PreparedFrame() = default;
ExecutionLease::ExecutionLease(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ExecutionLease::~ExecutionLease() = default;

bool PreparedFrame::encode(
    void* commandBuffer, void* fence,
    std::shared_ptr<const ExecutionLease>& lease, std::uint64_t recording) const noexcept {
    lease.reset();
    auto fail = [&](const char* stage) noexcept {
        if (impl_ && impl_->snapshot && impl_->state) {
            const auto& p = impl_->snapshot->parameters;
            logFailure(impl_->kind == Impl::Kind::Snapshot
                           ? WMTFG_PREPARE : WMTFG_DISPATCH,
                       stage, Runtime, impl_->state->creation.flags,
                       p.render_width, p.render_height, p.camera_near, p.camera_far,
                       p.camera_fov_vertical_radians, p.view_space_to_meters);
        } else {
            logFailure(WMTFG_DISPATCH, stage, Runtime);
        }
        return false;
    };
    try {
        @try {
            if (@available(macOS 26.0, *)) {
                if (!commandBuffer || !impl_ || !impl_->snapshot) return fail("encode_arguments");
                id buffer = (id)commandBuffer;
                auto& configuration = *impl_->snapshot->configuration;
                if (![buffer conformsToProtocol:@protocol(MTL4CommandBuffer)])
                    return fail("encode_command_buffer_type");
                auto execution = std::make_shared<ExecutionLease::Impl>();
                execution->prepared = impl_;
                execution->objects.reserve(impl_->kind == Impl::Kind::Snapshot ? 8 : 32);
                execution->objects.emplace_back((id)fence);
                execution->objects.push_back(impl_->first);
                execution->objects.push_back(impl_->second);
                execution->objects.push_back(impl_->third);
                execution->objects.push_back(impl_->realOutput);
                execution->objects.push_back(impl_->distortion);
                execution->objects.push_back(impl_->snapshot->depth);
                execution->objects.push_back(impl_->snapshot->motion);

                auto retainedLease = std::shared_ptr<const ExecutionLease>(
                    new ExecutionLease(execution));
                /* Publish ownership before the first command is encoded. */
                lease = retainedLease;
                retainFGLease(recording, retainedLease);

                std::lock_guard executionLock(impl_->state->executionMutex);
                if (impl_->kind == Impl::Kind::Snapshot) {
                    if (!execution->makeResident(buffer))
                        return fail("snapshot_residency");
                    const std::array<Copy, 2> copies{{
                        {(id<MTLTexture>)impl_->first.get(),
                         (id<MTLTexture>)impl_->snapshot->depth.get(),
                         impl_->snapshot->depthWidth, impl_->snapshot->depthHeight,
                         impl_->snapshot->parameters.depth_subrect_x, impl_->snapshot->parameters.depth_subrect_y},
                        {(id<MTLTexture>)impl_->second.get(),
                         (id<MTLTexture>)impl_->snapshot->motion.get(),
                         impl_->snapshot->motionWidth, impl_->snapshot->motionHeight,
                         impl_->snapshot->parameters.motion_subrect_x, impl_->snapshot->parameters.motion_subrect_y}
                    }};
                    if (!copyTextures(buffer, (id)fence,
                                      copies.data(), copies.size(), execution->objects))
                        return fail("snapshot_copy");
                    impl_->snapshot->production->produced = true;
                    return true;
                }
                if (!impl_->snapshot->production->produced) return fail("generate_before_prepare_execution");

                Object interpolator = configuration.factory;
                if (!interpolator) return fail("encode_interpolator");
                auto effect = (id<MTLFXFrameInterpolatorBase>)interpolator.get();
                auto device = (id<MTLDevice>)configuration.device.get();
                const auto& p = impl_->snapshot->parameters;
                const auto& d = impl_->dispatch;
                const bool displayMotion = (impl_->state->creation.flags &
                    WMTFG_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
                const NSUInteger motionTargetWidth = displayMotion
                    ? impl_->state->creation.display_width : p.render_width;
                const NSUInteger motionTargetHeight = displayMotion
                    ? impl_->state->creation.display_height : p.render_height;
                const bool motionConversion = p.motion_scale_x != 1 || p.motion_scale_y != 1 ||
                    motionTargetWidth != configuration.outputWidth ||
                    motionTargetHeight != configuration.outputHeight ||
                    (impl_->state->creation.flags & WMTFG_MOTION_VECTORS_JITTERED);
                const bool directDepth = exactDescriptorInput(
                    (id<MTLTexture>)impl_->snapshot->depth.get(), configuration.depth,
                    configuration.width, configuration.height, effect.depthTextureUsage, false);
                const bool directMotion = exactDescriptorInput(
                    (id<MTLTexture>)impl_->snapshot->motion.get(), configuration.motion,
                    configuration.width, configuration.height, effect.motionTextureUsage, motionConversion);
                Object current = privateTexture(device, configuration.color,
                    configuration.outputWidth, configuration.outputHeight,
                    effect.colorTextureUsage | MTLTextureUsageShaderWrite);
                Object ui = configuration.ui == MTLPixelFormatInvalid ? Object{} :
                    privateTexture(device, configuration.ui, configuration.outputWidth,
                                   configuration.outputHeight, effect.uiTextureUsage |
                                   MTLTextureUsageShaderWrite);
                Object normalizedDepth = directDepth ? impl_->snapshot->depth : privateTexture(device, configuration.depth,
                    configuration.width, configuration.height,
                    effect.depthTextureUsage | MTLTextureUsageShaderWrite);
                Object normalizedMotion = directMotion ? impl_->snapshot->motion : privateTexture(device, configuration.motion,
                    configuration.width, configuration.height,
                    effect.motionTextureUsage | MTLTextureUsageShaderWrite);
                Object output = privateTexture(device, configuration.color,
                    configuration.outputWidth, configuration.outputHeight,
                    effect.outputTextureUsage | MTLTextureUsageShaderRead);
                auto distortionSource = (id<MTLTexture>)impl_->distortion.get();
                const bool cropDistortion = distortionSource && !exactDescriptorInput(distortionSource,
                    distortionSource.pixelFormat, d.distortion_width, d.distortion_height, MTLTextureUsageShaderRead, false);
                // Native half-resolution atlas offsets differ from a canonical cropped field.
                Object distortion = cropDistortion ? privateTexture(device, distortionSource.pixelFormat,
                    d.distortion_width, d.distortion_height, MTLTextureUsageShaderRead) : impl_->distortion;
                if (!current || !normalizedDepth || !normalizedMotion || !output ||
                    (distortionSource && !distortion) ||
                    (configuration.ui != MTLPixelFormatInvalid && !ui))
                    return fail("encode_intermediate_textures");

                auto& state = *impl_->state;
                const std::uint64_t frame = impl_->dispatch.frame_id;
                bool reset = impl_->reset || impl_->snapshot->parameters.reset ||
                    impl_->snapshot->configuration != state.historyConfiguration ||
                    !sameHistoryValues(impl_->dispatch, state.historyDispatch) ||
                    state.historyFrame + 1 != frame;
                execution->objects.push_back(interpolator);
                execution->objects.push_back(current);
                execution->objects.push_back(ui);
                execution->objects.push_back(normalizedDepth);
                execution->objects.push_back(normalizedMotion);
                execution->objects.push_back(output);
                execution->objects.push_back(distortion);

                id<MTLTexture> present = (id<MTLTexture>)impl_->first.get();
                id<MTLTexture> scene = impl_->third
                    ? (id<MTLTexture>)impl_->third.get() : present;
                auto rawView = [&](id<MTLTexture> texture) -> Object {
                    MTLPixelFormat raw = texture.pixelFormat;
                    if (raw == MTLPixelFormatRGBA8Unorm_sRGB) raw = MTLPixelFormatRGBA8Unorm;
                    else if (raw == MTLPixelFormatBGRA8Unorm_sRGB) raw = MTLPixelFormatBGRA8Unorm;
                    if (raw == texture.pixelFormat) return Object(texture);
                    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:raw];
                    Object result(view);
                    [view release];
                    return result;
                };
                Object sceneView = rawView(scene);
                Object presentView = rawView(present);
                Object outputView = rawView((id<MTLTexture>)impl_->second.get());
                if (!sceneView || !presentView || !outputView)
                    return fail("encode_texture_views");
                execution->objects.push_back(sceneView);
                execution->objects.push_back(presentView);
                execution->objects.push_back(outputView);

                FgParams colorParams{};
                colorParams.sourceOrigin[0] = d.generation_rect_left;
                colorParams.sourceOrigin[1] = d.generation_rect_top;
                colorParams.extent[0] = configuration.outputWidth;
                colorParams.extent[1] = configuration.outputHeight;
                colorParams.sourceExtent[0] = present.width;
                colorParams.sourceExtent[1] = present.height;
                colorParams.motionTargetExtent[0] = state.creation.display_width;
                colorParams.motionTargetExtent[1] = state.creation.display_height;
                colorParams.outputExtent[0] = configuration.outputWidth;
                colorParams.outputExtent[1] = configuration.outputHeight;
                colorParams.minLuminance = d.min_luminance;
                colorParams.maxLuminance = d.max_luminance;
                colorParams.transfer = d.backbuffer_transfer_function;
                colorParams.debugFlags = d.flags | (p.flags & 7u);
                colorParams.reset = reset;

                FgParams depthParams{};
                depthParams.extent[0] = configuration.width;
                depthParams.extent[1] = configuration.height;
                depthParams.sourceExtent[0] = impl_->snapshot->depthWidth;
                depthParams.sourceExtent[1] = impl_->snapshot->depthHeight;

                FgParams motionParams{};
                motionParams.extent[0] = configuration.width;
                motionParams.extent[1] = configuration.height;
                // The generation rectangle is a placement offset, not a source offset.
                // Resample the complete origin-zero motion domain into the active rect.
                motionParams.sourceExtent[0] = impl_->snapshot->motionWidth;
                motionParams.sourceExtent[1] = impl_->snapshot->motionHeight;
                motionParams.motionTargetExtent[0] = displayMotion
                    ? state.creation.display_width : p.render_width;
                motionParams.motionTargetExtent[1] = displayMotion
                    ? state.creation.display_height : p.render_height;
                motionParams.outputExtent[0] = configuration.outputWidth;
                motionParams.outputExtent[1] = configuration.outputHeight;
                motionParams.motionScale[0] = p.motion_scale_x;
                motionParams.motionScale[1] = p.motion_scale_y;
                if ((state.creation.flags & WMTFG_MOTION_VECTORS_JITTERED) &&
                    impl_->snapshot->hasPreviousJitter && !reset) {
                    motionParams.jitterCancellation[0] =
                        impl_->snapshot->previousJitterX - p.jitter_x;
                    motionParams.jitterCancellation[1] =
                        impl_->snapshot->previousJitterY - p.jitter_y;
                }

                Object colorParameters = makeParameterBuffer(device, colorParams);
                Object depthParameters = directDepth ? Object{} : makeParameterBuffer(device, depthParams);
                Object motionParameters = directMotion ? Object{} : makeParameterBuffer(device, motionParams);
                if (!colorParameters || (!directDepth && !depthParameters) ||
                    (!directMotion && !motionParameters))
                    return fail("encode_parameter_buffers");
                execution->objects.push_back(colorParameters);
                if (!directDepth) execution->objects.push_back(depthParameters);
                if (!directMotion) execution->objects.push_back(motionParameters);
                // Metal4 GPU addresses must be admitted before the residency set commits.
                if (!execution->makeResident(buffer))
                    return fail("encode_residency");

                const NSUInteger copyX = d.output_subrect_only ? d.generation_rect_left : 0;
                const NSUInteger copyY = d.output_subrect_only ? d.generation_rect_top : 0;
                const NSUInteger copyWidth = d.output_subrect_only ? configuration.outputWidth : present.width;
                const NSUInteger copyHeight = d.output_subrect_only ? configuration.outputHeight : present.height;
                std::array<Copy, 3> copies{};
                unsigned copyCount = 0;
                copies[copyCount++] = {present, (id<MTLTexture>)impl_->second.get(),
                    copyWidth, copyHeight, copyX, copyY, copyX, copyY};
                if (impl_->realOutput) copies[copyCount++] = {present, (id<MTLTexture>)impl_->realOutput.get(),
                    copyWidth, copyHeight, copyX, copyY, copyX, copyY};
                if (cropDistortion) copies[copyCount++] = {distortionSource, (id<MTLTexture>)distortion.get(),
                    d.distortion_width, d.distortion_height, d.distortion_x, d.distortion_y};
                if (!copyTextures(buffer, (id)fence, copies.data(), copyCount,
                                  execution->objects))
                    return fail("encode_baseline_copy");
                if (!computePass(buffer, (id)fence, device,
                                 (id<MTLComputePipelineState>)configuration.decodePipeline.get(),
                                 (id<MTLTexture>)sceneView.get(),
                                 (id<MTLTexture>)current.get(),
                                 (id<MTLBuffer>)colorParameters.get(), colorParams,
                                 execution->objects))
                    return fail("encode_scene_decode");
                if (ui && !computePass(buffer, (id)fence, device,
                                      (id<MTLComputePipelineState>)configuration.decodePipeline.get(),
                                      (id<MTLTexture>)presentView.get(),
                                      (id<MTLTexture>)ui.get(),
                                      (id<MTLBuffer>)colorParameters.get(), colorParams,
                                      execution->objects))
                    return fail("encode_ui_decode");
                if (!directDepth && !computePass(buffer, (id)fence, device,
                                 (id<MTLComputePipelineState>)configuration.depthPipeline.get(),
                                 (id<MTLTexture>)impl_->snapshot->depth.get(),
                                 (id<MTLTexture>)normalizedDepth.get(),
                                 (id<MTLBuffer>)depthParameters.get(), depthParams,
                                 execution->objects))
                    return fail("encode_depth_normalize");
                if (!directMotion && !computePass(buffer, (id)fence, device,
                                 (id<MTLComputePipelineState>)configuration.motionPipeline.get(),
                                 (id<MTLTexture>)impl_->snapshot->motion.get(),
                                 (id<MTLTexture>)normalizedMotion.get(),
                                 (id<MTLBuffer>)motionParameters.get(), motionParams,
                                 execution->objects))
                    return fail("encode_motion_normalize");

                const InterpolatorBindings bindings(effect);
                effect.colorTexture = (id<MTLTexture>)current.get();
                effect.depthTexture = (id<MTLTexture>)normalizedDepth.get();
                effect.motionTexture = (id<MTLTexture>)normalizedMotion.get();
                effect.uiTexture = (id<MTLTexture>)ui.get();
                effect.uiTextureComposited = bool(ui);
                if (@available(macOS 27.0, *)) {
                    effect.distortionTexture = (id<MTLTexture>)distortion.get();
                    effect.distortionOffsetX = effect.distortionOffsetY = 0;
                    effect.distortionWidth = d.distortion_width; effect.distortionHeight = d.distortion_height;
                }
                effect.outputTexture = (id<MTLTexture>)output.get();
                effect.fence = (id<MTLFence>)fence;
                effect.shouldResetHistory = reset;
                effect.depthReversed =
                    (state.creation.flags & WMTFG_DEPTH_INVERTED) != 0;
                effect.motionVectorScaleX = 1.0f;
                effect.motionVectorScaleY = 1.0f;
                effect.jitterOffsetX = p.jitter_x;
                effect.jitterOffsetY = p.jitter_y;
                effect.deltaTime = p.frame_time_delta_ms * 0.001f;
                effect.aspectRatio = float(p.render_width) / float(p.render_height);

                if (@available(macOS 27.0, *)) {
                    // All interpolator inputs are normalized to the origin-zero
                    // generation-rectangle domain before MetalFX consumes them.
                    effect.contentWidth = configuration.outputWidth;
                    effect.contentHeight = configuration.outputHeight;
                    effect.depthContentOffsetX = 0;
                    effect.depthContentOffsetY = 0;
                    effect.motionContentOffsetX = 0;
                    effect.motionContentOffsetY = 0;
                    effect.outputOffsetX = 0;
                    effect.outputOffsetY = 0;
                }
                CameraPlanes planes{};
                if (!normalizeCameraPlanes(p, state.creation.flags, planes)) {
                    logFailure(WMTFG_DISPATCH, "encode_camera_planes", Parameter,
                               state.creation.flags, p.render_width, p.render_height,
                               p.camera_near, p.camera_far,
                               p.camera_fov_vertical_radians, p.view_space_to_meters);
                    return false;
                }
                const float worldScale = p.view_space_to_meters > 0
                    ? p.view_space_to_meters : 1.0f;
                effect.nearPlane = planes.nearPlane * worldScale;
                effect.farPlane = planes.farPlane * worldScale;
                effect.fieldOfView =
                    p.camera_fov_vertical_radians * 57.295779513082320876f;
                if (p.camera_info_present) {
                    if (@available(macOS 27.0, *)) {
                        simd_float3 position = {p.camera_position[0] * worldScale,
                            p.camera_position[1] * worldScale, p.camera_position[2] * worldScale};
                        simd_float3 right = {p.camera_right[0], p.camera_right[1], p.camera_right[2]};
                        simd_float3 up = {p.camera_up[0], p.camera_up[1], p.camera_up[2]};
                        simd_float3 forward = {p.camera_forward[0], p.camera_forward[1],
                                               p.camera_forward[2]};
                        simd_float4x4 view{};
                        view.columns[0] = {right.x, up.x, forward.x, 0};
                        view.columns[1] = {right.y, up.y, forward.y, 0};
                        view.columns[2] = {right.z, up.z, forward.z, 0};
                        view.columns[3] = {-simd_dot(right, position), -simd_dot(up, position),
                                           -simd_dot(forward, position), 1};
                        float n = effect.nearPlane, f = effect.farPlane;
                        float y = 1.0f / std::tan(p.camera_fov_vertical_radians * 0.5f);
                        bool reversed = effect.depthReversed;
                        float a = std::isinf(f) ? (reversed ? 0.0f : 1.0f)
                            : (reversed ? n / (n - f) : f / (f - n));
                        float b = std::isinf(f) ? (reversed ? n : -n)
                            : (reversed ? n * f / (f - n) : -n * f / (f - n));
                        simd_float4x4 projection{};
                        projection.columns[0] = {y / effect.aspectRatio, 0, 0, 0};
                        projection.columns[1] = {0, y, 0, 0};
                        projection.columns[2] = {0, 0, a, 1};
                        projection.columns[3] = {0, 0, b, 0};
                        effect.worldToViewMatrix = view;
                        effect.viewToClipMatrix = projection;
                    }
                }

                const bool currentGeneration =
                    impl_->generationEpoch == state.generationEpoch && state.generationEnabled;
                // Record the encode before MetalFX can report it to the HUD.
                if (!currentGeneration || !state.hudCounted) {
                    std::lock_guard lock(hudRow.mutex);
                    if (currentGeneration) {
                        state.hudCounted = true;
                        ++hudRow.interpolating;
                    } else {
                        // A command recorded before OFF or DESTROY re-adds the row.
                        scheduleHudRowRemoval();
                    }
                }
                [(id<MTL4FXFrameInterpolator>)interpolator.get()
                    encodeToCommandBuffer:(id<MTL4CommandBuffer>)buffer];

                if (!computePass(buffer, (id)fence, device,
                                 (id<MTLComputePipelineState>)configuration.encodePipeline.get(),
                                 (id<MTLTexture>)output.get(),
                                 (id<MTLTexture>)outputView.get(),
                                 (id<MTLBuffer>)colorParameters.get(), colorParams,
                                 execution->objects, (id<MTLTexture>)normalizedDepth.get(),
                                 (id<MTLTexture>)normalizedMotion.get()))
                    return fail("encode_output_scatter");
                // A queued command may encode after OFF (or after a subsequent ON).
                // Its output remains owned by the command, but must not seed new history.
                if (currentGeneration) {
                    state.historyConfiguration = impl_->snapshot->configuration;
                    state.historyDispatch = impl_->dispatch;
                    state.historyFrame = frame;
                }
                return true;
            }
            return fail("encode_os_unavailable");
        } @catch (NSException* exception) {
            const char* reason = [[exception reason] UTF8String];
            return fail(reason && *reason ? reason : "encode_objc_exception");
        }
    } catch (...) {
        return fail("encode_cpp_exception");
    }
}

bool available() noexcept { if (@available(macOS 26.0, *)) return true; return false; }

std::uint32_t api(std::uint32_t operation, void* arguments, std::shared_ptr<const PreparedFrame>* recorded) noexcept {
    if (!arguments) return Parameter;
    auto& header = *static_cast<WMTFGpacket_header*>(arguments);
    if (header.size < sizeof(header)) return Parameter;
    auto finish = [&](std::uint32_t result, const char* stage = "request",
                      std::uint32_t flags = 0) {
        header.result = result;
        if (result != Ok) {
            if (operation == WMTFG_PREPARE &&
                header.size == sizeof(WMTFGprepare_packet)) {
                const auto& p = *static_cast<const WMTFGprepare_packet*>(arguments);
                logFailure(operation, stage, result, flags, p.render_width, p.render_height,
                           p.camera_near, p.camera_far, p.camera_fov_vertical_radians,
                           p.view_space_to_meters);
            } else {
                logFailure(operation, stage, result, flags);
            }
        }
        return result;
    };
    if (header.operation != operation ||
        header.version != WMTFG_BRIDGE_VERSION)
        return finish(Parameter);
    try {
        @try {
            if (operation == WMTFG_DESTROY) {
                if (header.size != sizeof(WMTFGdestroy_packet))
                    return finish(Parameter);
                std::shared_ptr<State> state;
                {
                    std::lock_guard lock(registryMutex);
                    auto found = contexts.find(header.context);
                    if (found == contexts.end()) return finish(Parameter);
                    state = found->second;
                    contexts.erase(found);
                }
                std::lock_guard lock(state->mutex);
                state->retired = true;
                state->frames.clear();
                // Commands recorded before DESTROY may still encode; mark them stale.
                std::lock_guard executionLock(state->executionMutex);
                ++state->generationEpoch;
                releaseHudRow(state->hudCounted);
                return finish(Ok);
            }
            if (operation == WMTFG_RETIRE_FRAME) {
                if (header.size != sizeof(WMTFGretire_frame_packet))
                    return finish(Parameter);
                auto state = lookup(header.context);
                if (!state) return finish(Parameter);
                std::lock_guard lock(state->mutex);
                if (state->retired) return finish(Parameter);
                auto& retire =
                    *static_cast<WMTFGretire_frame_packet*>(arguments);
                state->frames.erase(retire.frame_id);
                retire.has_pending_frame = !state->frames.empty();
                if (retire.has_pending_frame) {
                    retire.oldest_pending_frame_id = state->frames.begin()->first;
                    for (const auto& pending : state->frames)
                        retire.oldest_pending_frame_id = std::min(
                            retire.oldest_pending_frame_id, pending.first);
                }
                return finish(Ok);
            }
            if (operation == WMTFG_CONFIGURE) {
                if (header.size != sizeof(WMTFGconfigure_packet))
                    return finish(Parameter);
                auto state = lookup(header.context);
                if (!state) return finish(Parameter);
                std::lock_guard lock(state->mutex);
                if (state->retired) return finish(Parameter);
                auto& config = *static_cast<WMTFGconfigure_packet*>(arguments);
                if (config.enabled > 1) return finish(Parameter);
                std::lock_guard executionLock(state->executionMutex);
                state->generationEnabled = config.enabled != 0;
                if (!state->generationEnabled) {
                    releaseHudRow(state->hudCounted);
                    ++state->generationEpoch;
                    state->frames.clear();
                    state->configurations.clear();
                    state->hasPreparedJitter = false;
                    state->historyConfiguration.reset();
                }
                return finish(Ok);
            }
            if (!available()) return finish(Unsupported);

            if (operation == WMTFG_CREATE) {
                if (header.size != sizeof(WMTFGcreate_packet))
                    return finish(Parameter);
                auto& p = *static_cast<WMTFGcreate_packet*>(arguments);
                if (!p.display_width || !p.display_height ||
                    !p.max_render_width || !p.max_render_height ||
                    p.max_render_width > p.display_width ||
                    p.max_render_height > p.display_height)
                    return finish(Parameter);
                if (colorFormat(p.backbuffer_format) == MTLPixelFormatInvalid)
                    return finish(Unsupported);
                auto state = std::make_shared<State>();
                state->device = Object((id)p.device);
                if (!state->device)
                    return finish(Parameter);
                state->creation = p;
                auto context = nextContext.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard lock(registryMutex);
                    contexts.emplace(context, state);
                }
                header.context = context;
                return finish(Ok);
            }

            if (operation != WMTFG_PREPARE &&
                operation != WMTFG_DISPATCH)
                return finish(Unsupported);

            const bool generate = operation == WMTFG_DISPATCH;
            if (header.size != (generate ? sizeof(WMTFGdispatch_packet)
                                         : sizeof(WMTFGprepare_packet)))
                return finish(Parameter);
            auto state = lookup(header.context);
            if (!state) return finish(Parameter);
            std::lock_guard lock(state->mutex);
            if (state->retired) return finish(Parameter);

            auto* p = generate ? nullptr
                : static_cast<WMTFGprepare_packet*>(arguments);
            auto* d = generate
                ? static_cast<WMTFGdispatch_packet*>(arguments) : nullptr;
            void* list = pointer(generate ? d->command_list : p->command_list);
            Command command;
            if (!command.open(list)) return finish(Unsupported);
            if (!supported((id<MTLDevice>)command.value.device,
                           command.value.compiler))
                return finish(Unsupported);
            if (state->providerValidated && state->device.get() != (id)command.value.device)
                return finish(Parameter);

            Resources resources;
            auto implementation = std::make_shared<PreparedFrame::Impl>();
            implementation->state = state;
            std::array<std::uint64_t, 5> addresses{};
            std::uint64_t frame = generate ? d->frame_id : p->frame_id;

            if (!generate) {
                CameraPlanes planes{};
                if (!normalizeCameraPlanes(*p, state->creation.flags, planes))
                    return finish(Parameter, "prepare_camera_planes", state->creation.flags);
                if (!validParameters(*p, state->creation.flags) ||
                    p->render_width > state->creation.max_render_width ||
                    p->render_height > state->creation.max_render_height)
                    return finish(Parameter, "prepare_parameters", state->creation.flags);
                if (state->frames.count(frame)) return finish(Parameter);
                // Prepare may continue while FG is off. Recorded work owns its snapshot,
                // but disabled presentation cannot consume frame-ID metadata.
                if (state->generationEnabled && state->frames.size() >= 64)
                    return finish(Runtime, "prepare_backpressure");
                addresses = {p->depth, p->motion_vectors, 0};
                const bool displayMotion = (state->creation.flags &
                    WMTFG_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
                const NSUInteger motionWidth = displayMotion
                    ? state->creation.display_width : p->render_width;
                const NSUInteger motionHeight = displayMotion
                    ? state->creation.display_height : p->render_height;
                if (!addresses[0] || !addresses[1] || addresses[0] == addresses[1] ||
                    !mapOne(resources, 0, *state, command, addresses[0], false,
                            NSUInteger(p->depth_subrect_x) + p->render_width,
                            NSUInteger(p->depth_subrect_y) + p->render_height) ||
                    !mapOne(resources, 1, *state, command, addresses[1], false,
                            NSUInteger(p->motion_subrect_x) + motionWidth,
                            NSUInteger(p->motion_subrect_y) + motionHeight) ||
                    rootTexture((id<MTLTexture>)resources.values[0].texture) ==
                    rootTexture((id<MTLTexture>)resources.values[1].texture))
                    return finish(Unsupported);
                auto depth = (id<MTLTexture>)resources.values[0].texture;
                auto motion = (id<MTLTexture>)resources.values[1].texture;
                // The first Prepare selects the provider even before the pending
                // configuration is replayed. Validate its MetalFX eligibility,
                // but do not create or record snapshots while generation is off.
                if (!state->generationEnabled && state->providerValidated) return finish(Ok);
                auto configuration = configure(*state, command, *p, depth,
                    MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid,
                    p->render_width, p->render_height,
                    0, false);
                if (!configuration) return finish(Unsupported);
                if (!state->generationEnabled) {
                    state->configurations.clear();
                    state->providerValidated = true;
                    return finish(Ok);
                }
                auto snapshot = std::make_shared<Snapshot>();
                snapshot->configuration = configuration;
                snapshot->parameters = *p;
                snapshot->depthWidth = p->render_width;
                snapshot->depthHeight = p->render_height;
                snapshot->motionWidth = motionWidth;
                snapshot->motionHeight = motionHeight;
                snapshot->hasPreviousJitter = state->hasPreparedJitter &&
                    state->preparedFrame + 1 == frame && !p->reset;
                snapshot->previousJitterX = state->preparedJitterX;
                snapshot->previousJitterY = state->preparedJitterY;
                auto nativeInputs = (id<MTLFXFrameInterpolatorBase>)configuration->factory.get();
                snapshot->depth = privateTexture(
                    (id<MTLDevice>)configuration->device.get(), configuration->depth,
                    p->render_width, p->render_height,
                    nativeInputs.depthTextureUsage | MTLTextureUsageShaderRead);
                snapshot->motion = privateTexture(
                    (id<MTLDevice>)configuration->device.get(), motion.pixelFormat,
                    motionWidth, motionHeight,
                    nativeInputs.motionTextureUsage | MTLTextureUsageShaderRead);
                if (!snapshot->depth || !snapshot->motion) return finish(Unsupported);
                state->hasPreparedJitter = true;
                state->preparedFrame = frame;
                state->preparedJitterX = p->jitter_x;
                state->preparedJitterY = p->jitter_y;
                implementation->snapshot = std::move(snapshot);
            } else {
                if ((d->flags & ~7u) || d->output_subrect_only > 1) return finish(Parameter);
                if (d->num_generated_frames != 1) return finish(Unsupported);
                std::int32_t rectLeft = 0, rectTop = 0;
                NSUInteger rectWidth = 0, rectHeight = 0;
                if (!validTransfer(*d)) return finish(Parameter);
                auto found = state->frames.find(frame);
                if (found == state->frames.end()) return finish(Parameter);
                implementation->kind = PreparedFrame::Impl::Kind::Generate;
                implementation->generationEpoch = state->generationEpoch;
                implementation->reset = d->reset != 0;
                implementation->dispatch = *d;
                addresses = {d->present_color, d->output, d->hudless_color, d->real_output, d->distortion};
                if (!mapPair(resources, *state, command, addresses[0], addresses[1],
                             true, state->creation.display_width,
                             state->creation.display_height))
                    return finish(Unsupported);
                auto present = (id<MTLTexture>)resources.values[0].texture;
                auto output = (id<MTLTexture>)resources.values[1].texture;
                if (output.width != present.width || output.height != present.height ||
                    present.pixelFormat != output.pixelFormat)
                    return finish(Unsupported);
                if (!resolveRect(*d, d->generation_rect_width ? present.width : state->creation.display_width,
                                 d->generation_rect_height ? present.height : state->creation.display_height,
                                 rectLeft, rectTop, rectWidth, rectHeight))
                    return finish(Parameter);
                id<MTLTexture> scene = present;
                MTLPixelFormat uiFormat = MTLPixelFormatInvalid;
                if (d->hudless_color) {
                    if (!mapOne(resources, 2, *state, command, d->hudless_color, false,
                                NSUInteger(rectLeft) + rectWidth, NSUInteger(rectTop) + rectHeight))
                        return finish(Unsupported);
                    scene = (id<MTLTexture>)resources.values[2].texture;
                    if (rootTexture(scene) == rootTexture(output)) return finish(Parameter);
                    uiFormat = present.pixelFormat;
                }
                if (d->real_output) {
                    if (!mapOne(resources, 3, *state, command, d->real_output, true, present.width, present.height))
                        return finish(Parameter);
                    auto real = (id<MTLTexture>)resources.values[3].texture;
                    if (real.width != present.width || real.height != present.height ||
                        real.pixelFormat != present.pixelFormat || rootTexture(real) == rootTexture(present) ||
                        rootTexture(real) == rootTexture(output) || rootTexture(real) == rootTexture(scene))
                        return finish(Parameter);
                }
                if (d->distortion) {
                    if (!d->distortion_width || !d->distortion_height ||
                        !mapOne(resources, 4, *state, command, d->distortion, false,
                            NSUInteger(d->distortion_x) + d->distortion_width,
                            NSUInteger(d->distortion_y) + d->distortion_height))
                        return finish(Parameter);
                    auto distortion = (id<MTLTexture>)resources.values[4].texture;
                    if (rootTexture(distortion) == rootTexture(output) ||
                        (d->real_output && rootTexture(distortion) == rootTexture((id<MTLTexture>)resources.values[3].texture)))
                        return finish(Parameter);
                    if (distortion.pixelFormat != MTLPixelFormatRGBA16Float && distortion.pixelFormat != MTLPixelFormatRGBA32Float)
                        return finish(Unsupported);
                }
                auto dispatchSnapshot = std::make_shared<Snapshot>(*found->second);
                dispatchSnapshot->configuration = configure(*state, command,
                    dispatchSnapshot->parameters,
                    (id<MTLTexture>)dispatchSnapshot->depth.get(), scene.pixelFormat,
                    uiFormat, rectWidth, rectHeight, d->backbuffer_transfer_function, d->distortion != 0);
                if (!dispatchSnapshot->configuration) return finish(Unsupported);
                implementation->snapshot = std::move(dispatchSnapshot);
            }
            implementation->first = Object((id)resources.values[0].texture);
            implementation->second = Object((id)resources.values[1].texture);
            implementation->third = Object((id)resources.values[2].texture);
            implementation->realOutput = Object((id)resources.values[3].texture);
            implementation->distortion = Object((id)resources.values[4].texture);
            auto prepared = std::shared_ptr<const PreparedFrame>(
                new PreparedFrame(implementation));

            /*
             * Insert before recording so allocation failure never leaves an
             * accepted command without its frame-ID registry entry.
             */
            if (!generate && state->generationEnabled)
                state->frames.emplace(frame, implementation->snapshot);
            if (!recorded) { if (!generate) state->frames.erase(frame); return finish(Parameter); }
            *recorded = std::move(prepared);
            if (generate) state->frames.erase(frame);
            state->providerValidated = true;
            return finish(Ok);
        } @catch (NSException*) {
            return finish(Runtime);
        }
    } catch (const std::bad_alloc&) {
        return finish(Memory);
    } catch (...) {
        return finish(Runtime);
    }
}

} // namespace dxmt::framegeneration


@interface DXMTFGHolder : NSObject {
@public
    std::shared_ptr<const dxmt::framegeneration::PreparedFrame> prepared;
    std::shared_ptr<const dxmt::framegeneration::ExecutionLease> lease;
}
@end
@implementation DXMTFGHolder
- (void)dealloc { prepared.reset(); lease.reset(); [super dealloc]; }
@end
static void retainFGLease(std::uint64_t recording, std::shared_ptr<const dxmt::framegeneration::ExecutionLease> lease) {
    DXMTFGHolder* holder = [DXMTFGHolder new];
    if (!holder) throw std::bad_alloc();
    holder->lease = std::move(lease);
    winemetal_metal4_retain_resource(recording, holder);
    [holder release];
}
extern "C" int32_t winemetal_metal4_fg_call(struct WMTMetal4Call* call) {
    @autoreleasepool {
        if (call->operation == WMTMetal4FGRequest) {
            // Allocate the recording owner before mutating frame-ID metadata.
            DXMTFGHolder* holder = nullptr;
            if (call->args[0] == WMTFG_PREPARE || call->args[0] == WMTFG_DISPATCH) {
                @try { holder = [DXMTFGHolder new]; }
                @catch (NSException*) { call->ret[0] = 5; return 0; }
                if (!holder) { call->ret[0] = 5; return 0; }
            }
            std::shared_ptr<const dxmt::framegeneration::PreparedFrame> prepared;
            call->ret[0] = dxmt::framegeneration::api(static_cast<std::uint32_t>(call->args[0]),
                reinterpret_cast<void*>(call->args[1]), &prepared);
            if (prepared) {
                holder->prepared = std::move(prepared);
                call->ret[1] = reinterpret_cast<std::uint64_t>(holder);
            } else [holder release];
            return 0;
        }
        if (call->operation == WMTMetal4FGEncode) {
            auto holder = (DXMTFGHolder*)call->args[1];
            if (!holder || !holder->prepared) { call->ret[0] = 0; return 0; }
            std::shared_ptr<const dxmt::framegeneration::ExecutionLease> lease;
            call->ret[0] = holder->prepared->encode((void*)winemetal_metal4_command_buffer(call->args[0]),
                (void*)call->args[2], lease, call->args[0]);
            return 0;
        }
        return static_cast<int32_t>(0xc0000002);
    }
}
