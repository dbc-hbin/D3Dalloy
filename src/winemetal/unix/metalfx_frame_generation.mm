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
#include <cstring>
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
struct Resources { struct Resource { void* texture = nullptr; }; std::array<Resource,7> values{}; };
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

// Recycles FG scratch only after its last owner drops: a recorded frame, the
// Prepare-to-Generate snapshot, or an execution lease retained until GPU
// retirement. Idle capacity is bounded; a destroyed context stops refilling.
class Pool final : public std::enable_shared_from_this<Pool> {
public:
    enum class Kind { Texture, Buffer, Table };
    struct Lent {
        std::shared_ptr<Pool> pool;
        Object object;
        Kind kind;
        ~Lent() { pool->put(kind, std::move(object)); }
    };
    static constexpr NSUInteger kParameterSlot = 256;
    static constexpr NSUInteger kParameterSlots = 6;

    std::shared_ptr<Lent> texture(id<MTLDevice> device, MTLPixelFormat format,
                                  NSUInteger width, NSUInteger height, MTLTextureUsage usage) {
        Object found;
        {
            std::lock_guard lock(mutex_);
            for (auto& slot : textures_) {
                auto texture = (id<MTLTexture>)slot.get();
                if (!texture || texture.device != device || texture.pixelFormat != format ||
                    texture.width != width || texture.height != height || texture.usage != usage)
                    continue;
                textureBytes_ -= texture.allocatedSize;
                found = std::exchange(slot, Object{});
                break;
            }
        }
        if (!found) {
            found = privateTexture(device, format, width, height, usage);
            if (found && !admit(device, found)) return {};
        }
        return lend(std::move(found), Kind::Texture);
    }

    std::shared_ptr<Lent> parameters(id<MTLDevice> device) {
        Object found = take(buffers_, device);
        if (!found) {
            id<MTLBuffer> buffer = [device newBufferWithLength:kParameterSlot * kParameterSlots
                                                      options:MTLResourceStorageModeShared];
            found = Object(buffer);
            [buffer release];
            if (found && !admit(device, found)) return {};
        }
        return lend(std::move(found), Kind::Buffer);
    }

    std::shared_ptr<Lent> table(id<MTLDevice> device) API_AVAILABLE(macos(26.0)) {
        Object found = take(tables_, device);
        if (!found) {
            MTL4ArgumentTableDescriptor* descriptor = [MTL4ArgumentTableDescriptor new];
            descriptor.maxBufferBindCount = 1;
            descriptor.maxTextureBindCount = 7;
            descriptor.initializeBindings = YES;
            NSError* error = nil;
            id<MTL4ArgumentTable> table = [device newArgumentTableWithDescriptor:descriptor error:&error];
            [descriptor release];
            found = Object(table);
            [table release];
        }
        return lend(std::move(found), Kind::Table);
    }

    // Every live pool texture and parameter buffer, idle or lent, is a member.
    // Membership changes only on creation and release, so steady-state frames
    // attach the set without committing it. Application resources are resident
    // through the device-global set attached to every queue.
    id residency() noexcept {
        std::lock_guard lock(mutex_);
        return residency_.get();
    }

    // Generation off: free idle memory but keep accepting completions.
    void trim() noexcept {
        std::lock_guard lock(mutex_);
        clearLocked();
    }

    // Context destroyed: completions after this release instead of refilling.
    void markDormant() noexcept {
        std::lock_guard lock(mutex_);
        dormant_ = true;
        clearLocked();
    }

private:
    static constexpr NSUInteger kMaxIdleTextureBytes = 512u * 1024u * 1024u;

    std::shared_ptr<Lent> lend(Object object, Kind kind) {
        if (!object) return {};
        auto lent = std::make_shared<Lent>();
        lent->pool = shared_from_this();
        lent->object = std::move(object);
        lent->kind = kind;
        return lent;
    }

    template<std::size_t N>
    Object take(std::array<Object, N>& slots, id<MTLDevice> device) {
        std::lock_guard lock(mutex_);
        for (auto& slot : slots)
            if (slot && [(id<MTLResource>)slot.get() device] == device)
                return std::exchange(slot, Object{});
        return {};
    }

    template<std::size_t N>
    static bool putSlot(std::array<Object, N>& slots, Object& object) noexcept {
        for (auto& slot : slots)
            if (!slot) { slot = std::move(object); return true; }
        return false;
    }

    // A new allocation joins the set before its first encode can commit.
    bool admit(id<MTLDevice> device, const Object& object) noexcept {
        if (@available(macOS 15.0, *)) {
            @try {
                std::lock_guard lock(mutex_);
                if (!residency_) {
                    MTLResidencySetDescriptor* descriptor = [[MTLResidencySetDescriptor alloc] init];
                    descriptor.initialCapacity = textures_.size() + buffers_.size();
                    NSError* error = nil;
                    id<MTLResidencySet> set = [device newResidencySetWithDescriptor:descriptor error:&error];
                    [descriptor release];
                    residency_ = Object(set);
                    [set release];
                    if (!residency_) return false;
                }
                auto set = (id<MTLResidencySet>)residency_.get();
                if (set.device != device) return false;
                [set addAllocation:(id<MTLAllocation>)object.get()];
                [set commit];
                [set requestResidency];
                return true;
            } @catch (id) {
            }
        }
        return false;
    }

    // Called with mutex_ held, only for allocations no recorded work still uses.
    void evictLocked(Object& object, bool commit = true) noexcept {
        if (!object) return;
        if (@available(macOS 15.0, *)) {
            @try {
                if (residency_) {
                    auto set = (id<MTLResidencySet>)residency_.get();
                    [set removeAllocation:(id<MTLAllocation>)object.get()];
                    if (commit) [set commit];
                }
            } @catch (id) {
            }
        }
        object = Object{};
    }

    void put(Kind kind, Object object) noexcept {
        if (!object) return;
        std::lock_guard lock(mutex_);
        if (dormant_) { if (kind != Kind::Table) evictLocked(object); return; }
        if (kind == Kind::Buffer) { if (!putSlot(buffers_, object)) evictLocked(object); return; }
        if (kind == Kind::Table) { putSlot(tables_, object); return; }
        const NSUInteger bytes = ((id<MTLTexture>)object.get()).allocatedSize;
        if (!bytes || bytes > kMaxIdleTextureBytes) { evictLocked(object); return; }
        // Evict until the byte cap admits this texture, oldest slot first.
        for (std::size_t i = 0; i != textures_.size() && textureBytes_ > kMaxIdleTextureBytes - bytes; ++i) {
            auto& slot = textures_[(nextTexture_ + i) % textures_.size()];
            if (!slot) continue;
            textureBytes_ -= ((id<MTLTexture>)slot.get()).allocatedSize;
            evictLocked(slot);
        }
        if (!putSlot(textures_, object)) {
            auto& slot = textures_[nextTexture_];
            textureBytes_ -= ((id<MTLTexture>)slot.get()).allocatedSize;
            evictLocked(slot);
            slot = std::move(object);
            nextTexture_ = (nextTexture_ + 1) % textures_.size();
        }
        textureBytes_ += bytes;
    }

    void clearLocked() noexcept {
        for (auto& slot : textures_) evictLocked(slot, false);
        for (auto& slot : buffers_) evictLocked(slot, false);
        for (auto& slot : tables_) slot = Object{};
        if (@available(macOS 15.0, *)) {
            @try {
                if (residency_) [(id<MTLResidencySet>)residency_.get() commit];
            } @catch (id) {
            }
        }
        textureBytes_ = 0;
    }

    std::mutex mutex_;
    bool dormant_ = false;
    std::array<Object, 24> textures_{};
    NSUInteger textureBytes_ = 0;
    std::size_t nextTexture_ = 0;
    std::array<Object, 8> buffers_{};
    std::array<Object, 8> tables_{};
    Object residency_;
};
using Lent = std::shared_ptr<Pool::Lent>;

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

Object makePipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString* name) {
    NSError* error = nil;
    id<MTLFunction> function = [library newFunctionWithName:name];
    Object functionOwner(function);
    [function release];
    if (!function) return {};
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    Object result(pipeline);
    [pipeline release];
    return result;
}

// The kernel library and its PSOs depend only on the device. Configurations
// share them; the weak cache never extends their lifetime past the last user.
struct Kernels {
    Object device;
    Object decodePipeline;
    Object depthPipeline;
    Object motionPipeline;
    Object encodePipeline;
    Object distortionPipeline;
};

std::mutex kernelMutex;
std::vector<std::weak_ptr<const Kernels>> kernelCache;

std::shared_ptr<const Kernels> deviceKernels(id<MTLDevice> device) {
    std::lock_guard lock(kernelMutex);
    std::shared_ptr<const Kernels> found;
    std::erase_if(kernelCache, [&](const std::weak_ptr<const Kernels>& entry) {
        auto kernels = entry.lock();
        if (kernels && kernels->device.get() == device) found = kernels;
        return !kernels;
    });
    if (found) return found;
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kFsrKernelsSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    Object libraryOwner(library);
    [library release];
    if (!library) return {};
    auto result = std::make_shared<Kernels>();
    result->device = Object(device);
    result->decodePipeline = makePipeline(device, library, @"yaagl_fg_decode_crop");
    result->depthPipeline = makePipeline(device, library, @"yaagl_fg_resample_depth");
    result->motionPipeline = makePipeline(device, library, @"yaagl_fg_normalize_motion");
    result->encodePipeline = makePipeline(device, library, @"yaagl_fg_encode_scatter");
    result->distortionPipeline = makePipeline(device, library, @"yaagl_fg_decode_distortion");
    if (!result->decodePipeline || !result->depthPipeline || !result->motionPipeline ||
        !result->encodePipeline || !result->distortionPipeline)
        return {};
    kernelCache.push_back(result);
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
    std::shared_ptr<const Kernels> kernels;

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
    WMTFGprepare_packet parameters{};
    // Pooled copies stay immutable until every holder of this snapshot drops.
    Lent depthScratch;
    Lent motionScratch;
    Object depth;
    Object motion;
    NSUInteger depthWidth = 0;
    NSUInteger depthHeight = 0;
    NSUInteger motionWidth = 0;
    NSUInteger motionHeight = 0;
    // Set by the Prepare encode, read by Generate encodes sharing this snapshot.
    std::atomic<bool> produced{false};
    bool hasPreviousJitter = false;
    float previousJitterX = 0;
    float previousJitterY = 0;
};

// Pending frame-ID metadata. Prepare admits at most kCapacity entries while
// generation is enabled and none while it is disabled; fixed storage avoids
// per-frame node allocation.
class FrameRegistry {
public:
    static constexpr std::size_t kCapacity = 64;
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return !count_; }
    std::shared_ptr<Snapshot>* find(std::uint64_t frame) noexcept {
        for (std::size_t i = 0; i != count_; ++i)
            if (entries_[i].first == frame) return &entries_[i].second;
        return nullptr;
    }
    bool insert(std::uint64_t frame, std::shared_ptr<Snapshot> snapshot) noexcept {
        if (count_ == kCapacity) return false;
        entries_[count_++] = {frame, std::move(snapshot)};
        return true;
    }
    void erase(std::uint64_t frame) noexcept {
        for (std::size_t i = 0; i != count_; ++i)
            if (entries_[i].first == frame) {
                entries_[i] = std::move(entries_[--count_]);
                entries_[count_].second.reset();
                return;
            }
    }
    void clear() noexcept {
        for (std::size_t i = 0; i != count_; ++i) entries_[i].second.reset();
        count_ = 0;
    }
    std::uint64_t oldest() const noexcept {
        std::uint64_t result = entries_[0].first;
        for (std::size_t i = 1; i < count_; ++i) result = std::min(result, entries_[i].first);
        return result;
    }
private:
    std::array<std::pair<std::uint64_t, std::shared_ptr<Snapshot>>, kCapacity> entries_{};
    std::size_t count_ = 0;
};

struct State {
    std::shared_ptr<Pool> pool = std::make_shared<Pool>();
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
    Object supportDevice;
    bool supportResult = false;
    // Protected by executionMutex: raw (non-sRGB) views of recent color sources,
    // e.g. the swapchain ring. Dropped when generation turns off or the context dies.
    std::array<std::pair<Object, Object>, 8> rawViews{};
    std::size_t nextRawView = 0;
    FrameRegistry frames;
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

bool supported(State& state, id<MTLDevice> device, std::uint64_t compiler) {
    if (@available(macOS 27.0, *)) {
        if (!device || !compiler) return false;
        // Protected by State::mutex; device support is immutable.
        if (state.supportDevice.get() != device) {
            state.supportResult = [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:device];
            state.supportDevice = Object(device);
        }
        return state.supportResult;
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
        if (!supported(state, device, command.value.compiler)) return {};
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

        // Source interpretation (source formats, transfer, camera) is history
        // continuity, not native descriptor state: identical descriptors share one
        // interpolator. Resets still follow the Configuration pointer.
        for (const auto& cached : state.configurations) {
            if (cached->device.get() == device && cached->color == result->color && cached->ui == result->ui &&
                cached->depth == result->depth && cached->motion == result->motion &&
                cached->width == result->width && cached->height == result->height &&
                cached->outputWidth == result->outputWidth && cached->outputHeight == result->outputHeight &&
                cached->distortionEnabled == result->distortionEnabled) {
                result->factory = cached->factory;
                break;
            }
        }
        if (!result->factory) result->factory = result->makeInterpolator();
        if (!result->factory) return {};
        result->kernels = deviceKernels(device);
        if (!result->kernels) return {};
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
    if (std::isnan(p.camera_far) || (std::isinf(p.camera_far) && p.camera_far < 0)) return false;
    if ((createFlags & WMTFG_DEPTH_INFINITE) || std::isinf(p.camera_far)) {
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
    if (p.reconstruct_camera_motion > 1) return false;
    if (p.camera_aspect_ratio < 0 || !std::isfinite(p.camera_aspect_ratio) ||
        p.motion_vectors_invalid_value_present > 1 ||
        (p.motion_vectors_invalid_value_present && !std::isfinite(p.motion_vectors_invalid_value))) return false;
    if (p.projection_matrix_present > 1 || (p.projection_matrix_present && !p.camera_info_present)) return false;
    if (p.projection_matrix_present) {
        for (float value : p.view_to_clip)
            if (!std::isfinite(value)) return false;
        simd_float4x4 projection;
        std::memcpy(&projection, p.view_to_clip, sizeof(projection));
        const float determinant = simd_determinant(projection);
        if (!std::isfinite(determinant) || determinant == 0) return false;
    }
    if (p.reconstruct_camera_motion)
        for (float value : p.clip_to_previous_clip)
            if (!std::isfinite(value)) return false;
    if (p.reconstruct_camera_motion) {
        simd_float4x4 transform;
        std::memcpy(&transform, p.clip_to_previous_clip, sizeof(transform));
        const float determinant = simd_determinant(transform);
        if (!std::isfinite(determinant) || determinant == 0) return false;
    }
    if ((p.flags & ~(31u | WMTFG_PREPARE_DIRECT_INPUTS)) || !p.render_width || !p.render_height || !p.motion_width || !p.motion_height ||
        !std::isfinite(p.jitter_x) || !std::isfinite(p.jitter_y) ||
        !std::isfinite(p.motion_scale_x) || !std::isfinite(p.motion_scale_y) ||
        !std::isfinite(p.frame_time_delta_ms) || p.frame_time_delta_ms < 0 ||
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
    std::uint32_t disableHint;
    std::uint32_t uiOrigin[2], sceneOrigin[2], presentOrigin[2];
    std::uint32_t uiMode;
    std::uint32_t reconstructCameraMotion;
    float clipToPreviousClip[16];
    float motionVectorsInvalidValue;
    std::uint32_t motionVectorsInvalidValuePresent;
    std::uint32_t depthSourceExtent[2];
    std::uint32_t depthDilation;
    std::uint32_t depthInverted;
};
static_assert(sizeof(FgParams) == 208 && offsetof(FgParams, depthSourceExtent) == 184 &&
              sizeof(FgParams) <= Pool::kParameterSlot);

// Metal snapshots argument-table bindings at each dispatch, so one table and
// one parameter buffer (one slot per pass) serve every pass of an execution.
enum ParameterSlot : NSUInteger { ColorSlot, SceneSlot, UiSlot, DistortionSlot, DepthSlot, MotionSlot };

// Encodes one dispatch into an open encoder; bindings are snapshotted at dispatch.
bool dispatchPass(id encoderObject, id table, id<MTLComputePipelineState> pipeline,
                  id<MTLTexture> source, id<MTLTexture> destination, id<MTLBuffer> parameterBuffer,
                  ParameterSlot slot, const FgParams& params,
                  id<MTLTexture> depth = nil, id<MTLTexture> motion = nil,
                  id<MTLTexture> ui = nil, id<MTLTexture> scene = nil, id<MTLTexture> present = nil) {
    if (!pipeline || !source || !destination || !parameterBuffer || !table || !encoderObject) return false;
    if (@available(macOS 26.0, *)) {
        auto arguments = (id<MTL4ArgumentTable>)table;
        auto encoder = (id<MTL4ComputeCommandEncoder>)encoderObject;
        const NSUInteger offset = slot * Pool::kParameterSlot;
        std::memcpy(static_cast<char*>(parameterBuffer.contents) + offset, &params, sizeof(params));
        [arguments setAddress:parameterBuffer.gpuAddress + offset atIndex:0];
        [arguments setTexture:source.gpuResourceID atIndex:0];
        [arguments setTexture:destination.gpuResourceID atIndex:1];
        // Metal validates every declared texture, including inactive uiMode
        // branches. Rebind unused slots so no stale binding outlives its pass.
        [arguments setTexture:(depth ? depth : source).gpuResourceID atIndex:2];
        [arguments setTexture:(motion ? motion : source).gpuResourceID atIndex:3];
        [arguments setTexture:(ui ? ui : source).gpuResourceID atIndex:4];
        [arguments setTexture:(scene ? scene : source).gpuResourceID atIndex:5];
        [arguments setTexture:(present ? present : source).gpuResourceID atIndex:6];
        [encoder setComputePipelineState:pipeline];
        [encoder setArgumentTable:arguments];
        [encoder dispatchThreads:MTLSizeMake(params.extent[0], params.extent[1], 1)
             threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        return true;
    }
    return false;
}

bool sameHistoryValues(const WMTFGdispatch_packet& a,
                       const WMTFGdispatch_packet& b) {
    return a.generation_rect_left == b.generation_rect_left &&
           a.generation_rect_top == b.generation_rect_top &&
           a.generation_rect_width == b.generation_rect_width &&
           a.generation_rect_height == b.generation_rect_height &&
           a.present_x == b.present_x && a.present_y == b.present_y &&
           a.hudless_x == b.hudless_x && a.hudless_y == b.hudless_y &&
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
    // Shared with the frame registry and every Generate of this frame; immutable
    // after Prepare except its produced flag.
    std::shared_ptr<Snapshot> snapshot;
    // Prepare: input requirements. Generate: the selected native configuration.
    std::shared_ptr<Configuration> configuration;
    Object first;
    Object second;
    Object third;
    Object realOutput;
    Object distortion;
    Object ui, uiAlpha;
    Object disableInterpolation;
    WMTFGdispatch_packet dispatch{};
    bool reset = false;
    std::uint64_t generationEpoch = 0;
};

struct ExecutionLease::Impl {
    std::shared_ptr<PreparedFrame::Impl> prepared;
    std::shared_ptr<Pool> pool;
    // Dropped after GPU retirement, when this lease leaves the recording.
    std::vector<Lent> scratch;
    std::vector<Object> objects;

    Object scratchTexture(MTLPixelFormat format, NSUInteger width, NSUInteger height, MTLTextureUsage usage) {
        auto device = (id<MTLDevice>)prepared->configuration->device.get();
        auto lent = pool->texture(device, format, width, height, usage);
        if (!lent) return {};
        scratch.push_back(lent);
        return lent->object;
    }

    // Pool scratch lives in the pool's persistent set; application resources in
    // the device-global set attached to the queue.
    bool makeResident(id buffer) {
        if (@available(macOS 26.0, *)) {
            auto set = (id<MTLResidencySet>)pool->residency();
            if (!set) return false;
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
                auto& configuration = *impl_->configuration;
                if (![buffer conformsToProtocol:@protocol(MTL4CommandBuffer)])
                    return fail("encode_command_buffer_type");
                auto execution = std::make_shared<ExecutionLease::Impl>();
                execution->prepared = impl_;
                execution->pool = impl_->state->pool;
                execution->objects.reserve(impl_->kind == Impl::Kind::Snapshot ? 8 : 32);
                execution->objects.emplace_back((id)fence);
                execution->objects.push_back(impl_->first);
                execution->objects.push_back(impl_->second);
                execution->objects.push_back(impl_->third);
                execution->objects.push_back(impl_->realOutput);
                execution->objects.push_back(impl_->distortion);
                execution->objects.push_back(impl_->ui);
                execution->objects.push_back(impl_->uiAlpha);
                execution->objects.push_back(impl_->disableInterpolation);
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
                    impl_->snapshot->produced.store(true, std::memory_order_release);
                    return true;
                }
                if (!impl_->snapshot->produced.load(std::memory_order_acquire)) return fail("generate_before_prepare_execution");

                Object interpolator = configuration.factory;
                if (!interpolator) return fail("encode_interpolator");
                auto effect = (id<MTLFXFrameInterpolatorBase>)interpolator.get();
                auto device = (id<MTLDevice>)configuration.device.get();
                const auto& p = impl_->snapshot->parameters;
                const auto& d = impl_->dispatch;
                const NSUInteger motionTargetWidth = p.motion_width;
                const NSUInteger motionTargetHeight = p.motion_height;
                // Display-resolution motion already carries color-grid silhouettes; dilate
                // only a render grid smaller than its sampling grid. A public (FSR)
                // generation rect keeps the full display's sampling density.
                const NSUInteger samplingWidth = impl_->dispatch.independent_subrects
                    ? configuration.width : impl_->state->creation.display_width;
                const NSUInteger samplingHeight = impl_->dispatch.independent_subrects
                    ? configuration.height : impl_->state->creation.display_height;
                const bool depthDilation =
                    !(impl_->state->creation.flags & WMTFG_DISPLAY_RESOLUTION_MOTION_VECTORS) &&
                    (impl_->snapshot->depthWidth < samplingWidth || impl_->snapshot->depthHeight < samplingHeight);
                const bool motionConversion = depthDilation || p.reconstruct_camera_motion || p.motion_scale_x != 1 || p.motion_scale_y != 1 ||
                    p.motion_vectors_invalid_value_present ||
                    motionTargetWidth != configuration.outputWidth ||
                    motionTargetHeight != configuration.outputHeight ||
                    (impl_->state->creation.flags & WMTFG_MOTION_VECTORS_JITTERED);
                const bool directDepth = exactDescriptorInput(
                    (id<MTLTexture>)impl_->snapshot->depth.get(), configuration.depth,
                    configuration.width, configuration.height, effect.depthTextureUsage, depthDilation);
                const bool directMotion = exactDescriptorInput(
                    (id<MTLTexture>)impl_->snapshot->motion.get(), configuration.motion,
                    configuration.width, configuration.height, effect.motionTextureUsage, motionConversion);
                Object current = execution->scratchTexture(configuration.color,
                    configuration.outputWidth, configuration.outputHeight,
                    effect.colorTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
                Object ui = configuration.ui == MTLPixelFormatInvalid ? Object{} :
                    execution->scratchTexture(configuration.ui, configuration.outputWidth,
                                   configuration.outputHeight, effect.uiTextureUsage |
                                   MTLTextureUsageShaderWrite);
                Object normalizedDepth = directDepth ? impl_->snapshot->depth : execution->scratchTexture(configuration.depth,
                    configuration.width, configuration.height,
                    effect.depthTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
                Object normalizedMotion = directMotion ? impl_->snapshot->motion : execution->scratchTexture(configuration.motion,
                    configuration.width, configuration.height,
                    effect.motionTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
                Object output = execution->scratchTexture(configuration.color,
                    configuration.outputWidth, configuration.outputHeight,
                    effect.outputTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
                auto distortionSource = (id<MTLTexture>)impl_->distortion.get();
                const bool decodeDistortion = distortionSource && d.distortion_low_precision;
                const bool cropDistortion = distortionSource && !exactDescriptorInput(distortionSource,
                    distortionSource.pixelFormat, d.distortion_width, d.distortion_height, MTLTextureUsageShaderRead, false);
                // Native half-resolution atlas offsets differ from a canonical cropped field.
                Object distortion = cropDistortion || decodeDistortion ? execution->scratchTexture(
                    decodeDistortion ? MTLPixelFormatRGBA32Float : distortionSource.pixelFormat,
                    d.distortion_width, d.distortion_height, MTLTextureUsageShaderRead |
                    (decodeDistortion ? MTLTextureUsageShaderWrite : 0)) : impl_->distortion;
                if (!current || !normalizedDepth || !normalizedMotion || !output ||
                    (distortionSource && !distortion) ||
                    (configuration.ui != MTLPixelFormatInvalid && !ui))
                    return fail("encode_intermediate_textures");

                auto& state = *impl_->state;
                const std::uint64_t frame = impl_->dispatch.frame_id;
                const bool currentGeneration =
                    impl_->generationEpoch == state.generationEpoch && state.generationEnabled;
                bool reset = impl_->reset || impl_->snapshot->parameters.reset ||
                    impl_->configuration != state.historyConfiguration ||
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
                    // The entry retains its source, so a cached pointer is never reused.
                    for (const auto& entry : state.rawViews)
                        if (entry.first.get() == texture) return entry.second;
                    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:raw];
                    Object result(view);
                    [view release];
                    if (result) {
                        state.rawViews[state.nextRawView] = {Object(texture), result};
                        state.nextRawView = (state.nextRawView + 1) % state.rawViews.size();
                    }
                    return result;
                };
                Object sceneView = rawView(scene);
                Object presentView = scene == present ? sceneView : rawView(present);
                Object outputView = rawView((id<MTLTexture>)impl_->second.get());
                Object explicitUiView = impl_->ui ? rawView((id<MTLTexture>)impl_->ui.get()) : impl_->uiAlpha;
                if (!sceneView || !presentView || !outputView)
                    return fail("encode_texture_views");
                execution->objects.push_back(sceneView);
                execution->objects.push_back(presentView);
                execution->objects.push_back(outputView);
                execution->objects.push_back(explicitUiView);

                FgParams colorParams{};
                colorParams.sourceOrigin[0] = d.output_x;
                colorParams.sourceOrigin[1] = d.output_y;
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
                colorParams.disableHint = reset || !currentGeneration;
                colorParams.uiMode = impl_->ui ? 1 : impl_->uiAlpha ? 2 : 0;
                colorParams.uiOrigin[0] = impl_->ui ? d.ui_x : d.ui_alpha_x;
                colorParams.uiOrigin[1] = impl_->ui ? d.ui_y : d.ui_alpha_y;
                colorParams.sceneOrigin[0] = d.hudless_x; colorParams.sceneOrigin[1] = d.hudless_y;
                colorParams.presentOrigin[0] = d.present_x; colorParams.presentOrigin[1] = d.present_y;
                FgParams sceneParams = colorParams, uiParams = colorParams;
                sceneParams.sourceOrigin[0] = impl_->third ? d.hudless_x : d.present_x;
                sceneParams.sourceOrigin[1] = impl_->third ? d.hudless_y : d.present_y;
                uiParams.sourceOrigin[0] = d.present_x;
                uiParams.sourceOrigin[1] = d.present_y;
                FgParams distortionParams{};
                distortionParams.sourceOrigin[0] = d.distortion_x;
                distortionParams.sourceOrigin[1] = d.distortion_y;
                distortionParams.extent[0] = d.distortion_width;
                distortionParams.extent[1] = d.distortion_height;
                distortionParams.motionScale[0] = d.distortion_scale;
                distortionParams.motionScale[1] = d.distortion_bias;

                FgParams depthParams{};
                depthParams.extent[0] = configuration.width;
                depthParams.extent[1] = configuration.height;
                depthParams.sourceExtent[0] = impl_->snapshot->depthWidth;
                depthParams.sourceExtent[1] = impl_->snapshot->depthHeight;
                depthParams.depthDilation = depthDilation;
                depthParams.depthInverted = (state.creation.flags & WMTFG_DEPTH_INVERTED) != 0;

                FgParams motionParams{};
                motionParams.extent[0] = configuration.width;
                motionParams.extent[1] = configuration.height;
                // The generation rectangle is a placement offset, not a source offset.
                // Resample the complete origin-zero motion domain into the active rect.
                motionParams.sourceExtent[0] = impl_->snapshot->motionWidth;
                motionParams.sourceExtent[1] = impl_->snapshot->motionHeight;
                motionParams.motionTargetExtent[0] = p.motion_width;
                motionParams.motionTargetExtent[1] = p.motion_height;
                motionParams.outputExtent[0] = configuration.outputWidth;
                motionParams.outputExtent[1] = configuration.outputHeight;
                motionParams.motionScale[0] = p.motion_scale_x;
                motionParams.motionScale[1] = p.motion_scale_y;
                motionParams.reconstructCameraMotion = p.reconstruct_camera_motion;
                motionParams.motionVectorsInvalidValue = p.motion_vectors_invalid_value;
                motionParams.motionVectorsInvalidValuePresent = p.motion_vectors_invalid_value_present;
                motionParams.depthDilation = depthDilation;
                motionParams.depthInverted = depthParams.depthInverted;
                motionParams.depthSourceExtent[0] = impl_->snapshot->depthWidth;
                motionParams.depthSourceExtent[1] = impl_->snapshot->depthHeight;
                std::memcpy(motionParams.clipToPreviousClip, p.clip_to_previous_clip, sizeof(p.clip_to_previous_clip));
                if ((state.creation.flags & WMTFG_MOTION_VECTORS_JITTERED) &&
                    impl_->snapshot->hasPreviousJitter && !reset) {
                    motionParams.jitterCancellation[0] =
                        (impl_->snapshot->previousJitterX - p.jitter_x) * p.motion_width / p.render_width;
                    motionParams.jitterCancellation[1] =
                        (impl_->snapshot->previousJitterY - p.jitter_y) * p.motion_height / p.render_height;
                }

                auto parameterLease = impl_->state->pool->parameters(device);
                auto tableLease = impl_->state->pool->table(device);
                if (!parameterLease || !tableLease) return fail("encode_parameter_buffers");
                execution->scratch.push_back(parameterLease);
                execution->scratch.push_back(tableLease);
                auto parameters = (id<MTLBuffer>)parameterLease->object.get();
                id table = tableLease->object.get();
                execution->objects.push_back(parameterLease->object);
                // Metal4 GPU addresses must be admitted before the residency set commits.
                if (!execution->makeResident(buffer))
                    return fail("encode_residency");

                const NSUInteger copyX = d.output_subrect_only ? d.present_x : 0;
                const NSUInteger copyY = d.output_subrect_only ? d.present_y : 0;
                const NSUInteger copyWidth = d.output_subrect_only ? configuration.outputWidth : present.width;
                const NSUInteger copyHeight = d.output_subrect_only ? configuration.outputHeight : present.height;
                std::array<Copy, 8> copies{};
                unsigned copyCount = 0;
                // The output scatter writes every generation-rect pixel. A full
                // output keeps only the present color outside that rect; a
                // subrect-only output has no baseline left to copy.
                if (!d.output_subrect_only) {
                    auto generated = (id<MTLTexture>)impl_->second.get();
                    const NSUInteger left = d.output_x, top = d.output_y;
                    const NSUInteger right = left + configuration.outputWidth;
                    const NSUInteger bottom = top + configuration.outputHeight;
                    const std::array<Copy, 4> bands{{
                        {present, generated, present.width, top, 0, 0, 0, 0},
                        {present, generated, present.width, present.height - bottom, 0, bottom, 0, bottom},
                        {present, generated, left, configuration.outputHeight, 0, top, 0, top},
                        {present, generated, present.width - right, configuration.outputHeight, right, top, right, top},
                    }};
                    for (const auto& band : bands)
                        if (band.width && band.height) copies[copyCount++] = band;
                }
                if (impl_->realOutput) copies[copyCount++] = {present, (id<MTLTexture>)impl_->realOutput.get(),
                    copyWidth, copyHeight, copyX, copyY,
                    d.output_subrect_only ? d.real_x : 0, d.output_subrect_only ? d.real_y : 0};
                if (impl_->realOutput && !d.output_subrect_only &&
                    (d.real_x != d.present_x || d.real_y != d.present_y))
                    copies[copyCount++] = {present, (id<MTLTexture>)impl_->realOutput.get(),
                        configuration.outputWidth, configuration.outputHeight, d.present_x, d.present_y, d.real_x, d.real_y};
                if (cropDistortion && !decodeDistortion) copies[copyCount++] = {distortionSource, (id<MTLTexture>)distortion.get(),
                    d.distortion_width, d.distortion_height, d.distortion_x, d.distortion_y};
                // One prepass encoder: baseline copies, then independent decode and
                // depth passes, then motion (camera reconstruction reads normalized
                // depth). Stage barriers replace per-pass fence boundaries.
                id<MTL4ComputeCommandEncoder> prepass = [(id<MTL4CommandBuffer>)buffer computeCommandEncoder];
                if (!prepass) return fail("encode_prepass_encoder");
                execution->objects.emplace_back(prepass);
                struct EndEncoding {
                    id<MTL4ComputeCommandEncoder> encoder;
                    ~EndEncoding() { if (encoder) [encoder endEncoding]; }
                } prepassScope{prepass};
                [prepass waitForFence:(id<MTLFence>)fence beforeEncoderStages:MTLStageBlit | MTLStageDispatch];
                for (unsigned i = 0; i != copyCount; ++i) {
                    const auto& copy = copies[i];
                    [prepass copyFromTexture:copy.source sourceSlice:0 sourceLevel:0
                                sourceOrigin:MTLOriginMake(copy.sourceX, copy.sourceY, 0)
                                  sourceSize:MTLSizeMake(copy.width, copy.height, 1)
                                   toTexture:copy.destination destinationSlice:0 destinationLevel:0
                           destinationOrigin:MTLOriginMake(copy.destinationX, copy.destinationY, 0)];
                }
                if (copyCount)
                    [prepass barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch
                                     visibilityOptions:MTL4VisibilityOptionDevice];
                if (!dispatchPass(prepass, table, (id<MTLComputePipelineState>)configuration.kernels->decodePipeline.get(),
                                  (id<MTLTexture>)sceneView.get(), (id<MTLTexture>)current.get(),
                                  parameters, SceneSlot, sceneParams))
                    return fail("encode_scene_decode");
                if (ui && !dispatchPass(prepass, table, (id<MTLComputePipelineState>)configuration.kernels->decodePipeline.get(),
                                        (id<MTLTexture>)presentView.get(), (id<MTLTexture>)ui.get(),
                                        parameters, UiSlot, uiParams))
                    return fail("encode_ui_decode");
                if (decodeDistortion && !dispatchPass(prepass, table,
                                        (id<MTLComputePipelineState>)configuration.kernels->distortionPipeline.get(),
                                        distortionSource, (id<MTLTexture>)distortion.get(),
                                        parameters, DistortionSlot, distortionParams))
                    return fail("encode_distortion_decode");
                if (!directDepth && !dispatchPass(prepass, table,
                                  (id<MTLComputePipelineState>)configuration.kernels->depthPipeline.get(),
                                  (id<MTLTexture>)impl_->snapshot->depth.get(), (id<MTLTexture>)normalizedDepth.get(),
                                  parameters, DepthSlot, depthParams))
                    return fail("encode_depth_normalize");
                if (!directMotion) {
                    if (!directDepth && p.reconstruct_camera_motion)
                        [prepass barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch
                                         visibilityOptions:MTL4VisibilityOptionDevice];
                    if (!dispatchPass(prepass, table, (id<MTLComputePipelineState>)configuration.kernels->motionPipeline.get(),
                                      (id<MTLTexture>)impl_->snapshot->motion.get(), (id<MTLTexture>)normalizedMotion.get(),
                                      parameters, MotionSlot, motionParams, (id<MTLTexture>)normalizedDepth.get(), nil,
                                      (id<MTLTexture>)impl_->snapshot->depth.get()))
                        return fail("encode_motion_normalize");
                }
                [prepass updateFence:(id<MTLFence>)fence afterEncoderStages:MTLStageBlit | MTLStageDispatch];
                [prepass endEncoding];
                prepassScope.encoder = nil;

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
                effect.aspectRatio = p.camera_aspect_ratio > 0
                    ? p.camera_aspect_ratio
                    : float(p.render_width) / float(p.render_height);

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
                if (!p.camera_info_present) {
                    // A shared interpolator must not keep another configuration's camera.
                    if (@available(macOS 27.0, *)) {
                        effect.worldToViewMatrix = matrix_identity_float4x4;
                        effect.viewToClipMatrix = matrix_identity_float4x4;
                    }
                } else {
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
                        // An explicit NGX matrix also represents orthographic/off-axis
                        // cameras. Preserve its hardware-depth space for native unprojection.
                        if (p.projection_matrix_present)
                            std::memcpy(&projection, p.view_to_clip, sizeof(projection));
                        effect.worldToViewMatrix = view;
                        effect.viewToClipMatrix = projection;
                    }
                }

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

                id<MTL4ComputeCommandEncoder> scatter = [(id<MTL4CommandBuffer>)buffer computeCommandEncoder];
                if (!scatter) return fail("encode_output_scatter");
                execution->objects.emplace_back(scatter);
                struct EndScatter {
                    id<MTL4ComputeCommandEncoder> encoder;
                    ~EndScatter() { if (encoder) [encoder endEncoding]; }
                } scatterScope{scatter};
                [scatter waitForFence:(id<MTLFence>)fence beforeEncoderStages:MTLStageDispatch | MTLStageBlit];
                if (!dispatchPass(scatter, table, (id<MTLComputePipelineState>)configuration.kernels->encodePipeline.get(),
                                  (id<MTLTexture>)output.get(), (id<MTLTexture>)outputView.get(),
                                  parameters, ColorSlot, colorParams, (id<MTLTexture>)normalizedDepth.get(),
                                  (id<MTLTexture>)normalizedMotion.get(), (id<MTLTexture>)explicitUiView.get(),
                                  explicitUiView ? (id<MTLTexture>)sceneView.get() : nil,
                                  explicitUiView ? (id<MTLTexture>)presentView.get() : nil))
                    return fail("encode_output_scatter");
                if (impl_->disableInterpolation) {
                    // NGX defines a presentation hint, not a native quality/count query.
                    // A reset or detached recording has no continuous pair to present.
                    // The CPU-written parameter word is independent of the scatter dispatch.
                    [scatter copyFromBuffer:parameters
                               sourceOffset:ColorSlot * Pool::kParameterSlot + offsetof(FgParams, disableHint)
                                   toBuffer:(id<MTLBuffer>)impl_->disableInterpolation.get()
                          destinationOffset:d.disable_interpolation_offset size:sizeof(std::uint32_t)];
                }
                [scatter updateFence:(id<MTLFence>)fence afterEncoderStages:MTLStageDispatch | MTLStageBlit];
                [scatter endEncoding];
                scatterScope.encoder = nil;
                // A queued command may encode after OFF (or after a subsequent ON).
                // Its output remains owned by the command, but must not seed new history.
                if (currentGeneration) {
                    state.historyConfiguration = impl_->configuration;
                    state.historyDispatch = impl_->dispatch;
                    state.historyFrame = frame;
                } else if (state.historyConfiguration &&
                           state.historyConfiguration->factory.get() == interpolator.get()) {
                    // A stale encode advanced the shared native history without
                    // recording it; the next current frame must reset.
                    state.historyConfiguration.reset();
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
                state->rawViews = {};
                state->pool->markDormant();
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
                if (retire.has_pending_frame)
                    retire.oldest_pending_frame_id = state->frames.oldest();
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
                // Repeated ON changes nothing that encoding observes; skip the
                // execution lock so per-frame Configure never waits for an encode.
                if (config.enabled && state->generationEnabled) return finish(Ok);
                std::lock_guard executionLock(state->executionMutex);
                state->generationEnabled = config.enabled != 0;
                if (!state->generationEnabled) {
                    releaseHudRow(state->hudCounted);
                    ++state->generationEpoch;
                    state->frames.clear();
                    state->configurations.clear();
                    state->hasPreparedJitter = false;
                    state->historyConfiguration.reset();
                    state->rawViews = {};
                    state->pool->trim();
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
            if (!supported(*state, (id<MTLDevice>)command.value.device,
                           command.value.compiler))
                return finish(Unsupported);
            if (state->providerValidated && state->device.get() != (id)command.value.device)
                return finish(Parameter);

            Resources resources;
            auto implementation = std::make_shared<PreparedFrame::Impl>();
            implementation->state = state;
            std::array<std::uint64_t, 7> addresses{};
            std::uint64_t frame = generate ? d->frame_id : p->frame_id;

            if (!generate) {
                CameraPlanes planes{};
                if (!normalizeCameraPlanes(*p, state->creation.flags, planes))
                    return finish(Parameter, "prepare_camera_planes", state->creation.flags);
                if (!validParameters(*p, state->creation.flags) ||
                    p->render_width > state->creation.max_render_width ||
                    p->render_height > state->creation.max_render_height)
                    return finish(Parameter, "prepare_parameters", state->creation.flags);
                // Cleared unless this Prepare is accepted without a snapshot command.
                const bool requestDirect = (p->flags & WMTFG_PREPARE_DIRECT_INPUTS) != 0;
                p->flags &= ~WMTFG_PREPARE_DIRECT_INPUTS;
                if (state->frames.find(frame)) return finish(Parameter);
                // Prepare may continue while FG is off. Recorded work owns its snapshot,
                // but disabled presentation cannot consume frame-ID metadata.
                if (state->generationEnabled && state->frames.size() >= FrameRegistry::kCapacity)
                    return finish(Runtime, "prepare_backpressure");
                addresses = {p->depth, p->motion_vectors, 0};
                const NSUInteger motionWidth = p->motion_width;
                const NSUInteger motionHeight = p->motion_height;
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
                // Prepare only copies snapshots; it needs the native input usages, not
                // a render-size interpolator. Reuse any configuration with this depth
                // format, else the default full-display SDR one Generate commonly selects.
                std::shared_ptr<Configuration> configuration;
                for (const auto& cached : state->configurations)
                    if (cached->device.get() == (id)command.value.device && cached->depth == depth.pixelFormat) {
                        configuration = cached;
                        break;
                    }
                if (!configuration)
                    configuration = configure(*state, command, *p, depth,
                        MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid,
                        state->creation.display_width, state->creation.display_height, 0, false);
                if (!configuration) return finish(Unsupported);
                if (!state->generationEnabled) {
                    state->configurations.clear();
                    state->providerValidated = true;
                    return finish(Ok);
                }
                auto snapshot = std::make_shared<Snapshot>();
                implementation->configuration = configuration;
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
                // A Generate recorded next on the same list reads the inputs before the
                // application can change them. Every Generate pass samples only the
                // origin-zero snapshot extent, and MetalFX consumes an input directly
                // only at its exact descriptor size, so origin-zero inputs are the snapshot.
                const bool direct = requestDirect &&
                    !p->depth_subrect_x && !p->depth_subrect_y && !p->motion_subrect_x && !p->motion_subrect_y &&
                    (depth.usage & MTLTextureUsageShaderRead) && (motion.usage & MTLTextureUsageShaderRead);
                if (direct) {
                    snapshot->depth = Object(depth);
                    snapshot->motion = Object(motion);
                    snapshot->produced.store(true, std::memory_order_relaxed);
                } else {
                    snapshot->depthScratch = state->pool->texture(
                        (id<MTLDevice>)configuration->device.get(), configuration->depth,
                        p->render_width, p->render_height,
                        nativeInputs.depthTextureUsage | MTLTextureUsageShaderRead);
                    snapshot->motionScratch = state->pool->texture(
                        (id<MTLDevice>)configuration->device.get(), motion.pixelFormat,
                        motionWidth, motionHeight,
                        nativeInputs.motionTextureUsage | MTLTextureUsageShaderRead);
                    if (!snapshot->depthScratch || !snapshot->motionScratch) return finish(Unsupported);
                    snapshot->depth = snapshot->depthScratch->object;
                    snapshot->motion = snapshot->motionScratch->object;
                }
                state->hasPreparedJitter = true;
                state->preparedFrame = frame;
                state->preparedJitterX = p->jitter_x;
                state->preparedJitterY = p->jitter_y;
                implementation->snapshot = std::move(snapshot);
                if (direct) {
                    // No Prepare command: register the frame and record nothing.
                    if (!state->frames.insert(frame, implementation->snapshot)) return finish(Runtime);
                    p->flags |= WMTFG_PREPARE_DIRECT_INPUTS;
                    state->providerValidated = true;
                    return finish(Ok);
                }
            } else {
                if ((d->flags & ~7u) || d->output_subrect_only > 1 || d->independent_subrects > 1 ||
                    d->distortion_low_precision > 1 || (d->distortion_low_precision &&
                    (!std::isfinite(d->distortion_scale) || !std::isfinite(d->distortion_bias)))) return finish(Parameter);
                if (d->num_generated_frames != 1) return finish(Unsupported);
                std::int32_t rectLeft = 0, rectTop = 0;
                NSUInteger rectWidth = 0, rectHeight = 0;
                if (!validTransfer(*d)) return finish(Parameter);
                auto found = state->frames.find(frame);
                if (!found) return finish(Parameter);
                implementation->kind = PreparedFrame::Impl::Kind::Generate;
                implementation->generationEpoch = state->generationEpoch;
                implementation->reset = d->reset != 0;
                implementation->dispatch = *d;
                if (d->disable_interpolation) {
                    auto status = (id<MTLBuffer>)d->disable_interpolation;
                    if (![status conformsToProtocol:@protocol(MTLBuffer)] ||
                        status.device != (id<MTLDevice>)command.value.device ||
                        d->disable_interpolation_offset > status.length ||
                        sizeof(std::uint32_t) > status.length - d->disable_interpolation_offset)
                        return finish(Parameter);
                    implementation->disableInterpolation = Object(status);
                }
                addresses = {d->present_color, d->output, d->hudless_color, d->real_output, d->distortion};
                if (!mapPair(resources, *state, command, addresses[0], addresses[1],
                             true, state->creation.display_width,
                             state->creation.display_height))
                    return finish(Unsupported);
                auto present = (id<MTLTexture>)resources.values[0].texture;
                auto output = (id<MTLTexture>)resources.values[1].texture;
                if ((!d->output_subrect_only && (output.width != present.width || output.height != present.height)) ||
                    present.pixelFormat != output.pixelFormat)
                    return finish(Unsupported);
                if (!resolveRect(*d, d->generation_rect_width ? output.width : state->creation.display_width,
                                 d->generation_rect_height ? output.height : state->creation.display_height,
                                 rectLeft, rectTop, rectWidth, rectHeight))
                    return finish(Parameter);
                auto& dispatch = implementation->dispatch;
                if (!d->independent_subrects) {
                    dispatch.present_x = dispatch.hudless_x = dispatch.output_x = dispatch.real_x = rectLeft;
                    dispatch.present_y = dispatch.hudless_y = dispatch.output_y = dispatch.real_y = rectTop;
                }
                if (NSUInteger(dispatch.present_x) + rectWidth > present.width ||
                    NSUInteger(dispatch.present_y) + rectHeight > present.height ||
                    NSUInteger(dispatch.output_x) + rectWidth > output.width ||
                    NSUInteger(dispatch.output_y) + rectHeight > output.height) return finish(Parameter);
                id<MTLTexture> scene = present;
                MTLPixelFormat uiFormat = MTLPixelFormatInvalid;
                if (d->hudless_color) {
                    if (!mapOne(resources, 2, *state, command, d->hudless_color, false,
                                NSUInteger(dispatch.hudless_x) + rectWidth, NSUInteger(dispatch.hudless_y) + rectHeight))
                        return finish(Unsupported);
                    scene = (id<MTLTexture>)resources.values[2].texture;
                    if (rootTexture(scene) == rootTexture(output)) return finish(Parameter);
                    if (!d->ui && !d->ui_alpha) uiFormat = present.pixelFormat;
                }
                if (d->real_output) {
                    if (!mapOne(resources, 3, *state, command, d->real_output, true,
                                d->output_subrect_only ? NSUInteger(dispatch.real_x) + rectWidth : present.width,
                                d->output_subrect_only ? NSUInteger(dispatch.real_y) + rectHeight : present.height))
                        return finish(Parameter);
                    auto real = (id<MTLTexture>)resources.values[3].texture;
                    if ((!d->output_subrect_only && (real.width != present.width || real.height != present.height)) ||
                        NSUInteger(dispatch.real_x) + rectWidth > real.width ||
                        NSUInteger(dispatch.real_y) + rectHeight > real.height ||
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
                    if (distortion.pixelFormat != MTLPixelFormatRGBA16Float && distortion.pixelFormat != MTLPixelFormatRGBA32Float &&
                        !(d->distortion_low_precision && (distortion.pixelFormat == MTLPixelFormatRGBA8Unorm ||
                          distortion.pixelFormat == MTLPixelFormatRGBA16Unorm || distortion.pixelFormat == MTLPixelFormatRGBA16Snorm)))
                        return finish(Unsupported);
                }
                if ((d->ui || d->ui_alpha) && (!d->hudless_color || (d->ui && d->ui_alpha)))
                    return finish(Parameter);
                for (unsigned i = 5; i != 7; ++i) {
                    const auto address = i == 5 ? d->ui : d->ui_alpha;
                    if (!address) continue;
                    const auto x = i == 5 ? d->ui_x : d->ui_alpha_x;
                    const auto y = i == 5 ? d->ui_y : d->ui_alpha_y;
                    if (!mapOne(resources,i,*state,command,address,false,NSUInteger(x) + rectWidth,NSUInteger(y) + rectHeight))
                        return finish(Parameter);
                    auto texture = (id<MTLTexture>)resources.values[i].texture;
                    if (rootTexture(texture) == rootTexture(output) ||
                        (d->real_output && rootTexture(texture) == rootTexture((id<MTLTexture>)resources.values[3].texture)))
                        return finish(Parameter);
                    const auto format = texture.pixelFormat;
                    if (i == 5 ? (format != MTLPixelFormatRGBA8Unorm && format != MTLPixelFormatBGRA8Unorm &&
                                  format != MTLPixelFormatRGBA16Float && format != MTLPixelFormatRGBA32Float)
                               : (format != MTLPixelFormatR8Unorm && format != MTLPixelFormatR16Unorm &&
                                  format != MTLPixelFormatR16Float && format != MTLPixelFormatR32Float))
                        return finish(Unsupported);
                }
                implementation->configuration = configure(*state, command,
                    (*found)->parameters,
                    (id<MTLTexture>)(*found)->depth.get(), scene.pixelFormat,
                    uiFormat, rectWidth, rectHeight, d->backbuffer_transfer_function, d->distortion != 0);
                if (!implementation->configuration) return finish(Unsupported);
                implementation->snapshot = *found;
            }
            implementation->first = Object((id)resources.values[0].texture);
            implementation->second = Object((id)resources.values[1].texture);
            implementation->third = Object((id)resources.values[2].texture);
            implementation->realOutput = Object((id)resources.values[3].texture);
            implementation->distortion = Object((id)resources.values[4].texture);
            implementation->ui = Object((id)resources.values[5].texture);
            implementation->uiAlpha = Object((id)resources.values[6].texture);
            auto prepared = std::shared_ptr<const PreparedFrame>(
                new PreparedFrame(implementation));

            /*
             * Insert before recording so allocation failure never leaves an
             * accepted command without its frame-ID registry entry.
             */
            if (!generate && state->generationEnabled)
                state->frames.insert(frame, implementation->snapshot);
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
