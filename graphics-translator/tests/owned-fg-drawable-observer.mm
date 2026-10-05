// Test-only observer. Public Objective-C selectors, no renderer offsets or
// private layouts. Present is intercepted after the owned GPU completion.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crt_externs.h>
#include <mutex>
#include <unordered_map>

namespace {
std::mutex hookMutex, fileMutex;
std::unordered_map<Class, IMP> presents, timedPresents, atPresents;
std::atomic<unsigned long long> nextSequence{0};
IMP originalNext;
char capturedKey;
char acquisitionLayerKey;
bool windowMetadata = false;
const char* capturePrefix = "OWNED_D3D12_FSR_FG";
thread_local bool capturing = false;
struct ViewMatch {
    NSView* view = nil;
    NSUInteger ancestor = 64, visited = 0;
    bool truncated = false;
};
void matchView(NSView* view, CALayer* const* ancestors, NSUInteger count, ViewMatch& match) {
    if (!view) return;
    if (match.visited >= 256) { match.truncated = true; return; }
    ++match.visited;
    CALayer* root = view.layer;
    for (NSUInteger i = 0; root && i < count; ++i)
        if (root == ancestors[i] && i < match.ancestor) { match.view = view; match.ancestor = i; }
    for (NSView* child in view.subviews) matchView(child, ancestors, count, match);
}
void snapshotWindow(CAMetalLayer* layer, const char* stage, unsigned long long sequence,
                    unsigned long long drawable, double presentedTime) {
    // AppKit sampling is asynchronous, not an observation of scanout. Never
    // infer detachment merely from a nil/non-NSView delegate or no tree match.
    const BOOL main = NSThread.isMainThread;
    if (!main) return;
    CALayer* ancestors[64]{};
    NSUInteger count = 0;
    for (CALayer* node = layer; node && count < 64; node = node.superlayer) ancestors[count++] = node;
    const double sampled = CACurrentMediaTime();
    std::lock_guard<std::mutex> lock(fileMutex);
    fprintf(stderr, "%s_WINDOW_METADATA stage=%s sequence=%llu drawable=%llu layer=%p available=%u appkit_main=1 sampled_time=%.9f presented_time=%.9f ancestors=%lu ancestry_truncated=%u\n",
            capturePrefix, stage, sequence, drawable, layer, unsigned(layer != nil), sampled, presentedTime, count,
            unsigned(count == 64 && ancestors[63].superlayer != nil));
    for (NSUInteger i = 0; i < count; ++i) {
        CALayer* node = ancestors[i];
        const CGRect frame = node.frame, bounds = node.bounds;
        id delegate = node.delegate;
        fprintf(stderr, "%s_LAYER_METADATA stage=%s sequence=%llu drawable=%llu index=%lu layer=%p class=%s superlayer=%p delegate=%p delegate_class=%s hidden=%u opacity=%g frame=%g,%g,%g,%g bounds=%g,%g,%g,%g\n",
                capturePrefix, stage, sequence, drawable, i, node, object_getClassName(node), node.superlayer,
                delegate, delegate ? object_getClassName(delegate) : "nil", unsigned(node.hidden), node.opacity,
                frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
                bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
    }
    NSUInteger windows = 0, matches = 0;
    // Resolve the already-loaded public AppKit class at runtime so the normal
    // observer keeps its existing Foundation/Metal/QuartzCore link recipe.
    NSApplication* application = [objc_lookUpClass("NSApplication") sharedApplication];
    for (NSWindow* window in application.windows) {
        if (windows++ >= 32) break;
        ViewMatch match;
        matchView(window.contentView, ancestors, count, match);
        const NSRect frame = window.frame;
        const NSWindowOcclusionState state = window.occlusionState;
        NSScreen* screen = window.screen;
        const NSRect screenFrame = screen ? screen.frame : NSZeroRect;
        const CGDirectDisplayID display = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
        matches += match.view != nil;
        fprintf(stderr, "%s_NS_WINDOW_METADATA stage=%s sequence=%llu drawable=%llu layer=%p window=%p window_number=%ld visible=%u occlusion_state=%lu occlusion_visible=%u screen=%p display=%u screen_frame=%g,%g,%g,%g backing_scale=%g frame=%g,%g,%g,%g content_view=%p content_layer=%p matched_view=%p matched_layer=%p matched_ancestor=%lu visited=%lu tree_truncated=%u\n",
                capturePrefix, stage, sequence, drawable, layer, window, window.windowNumber, unsigned(window.visible),
                state, unsigned(bool(state & NSWindowOcclusionStateVisible)), screen, display,
                screenFrame.origin.x, screenFrame.origin.y, screenFrame.size.width, screenFrame.size.height, window.backingScaleFactor,
                frame.origin.x, frame.origin.y, frame.size.width, frame.size.height, window.contentView, window.contentView.layer,
                match.view, match.view.layer, match.ancestor, match.visited, unsigned(match.truncated));
    }
    fprintf(stderr, "%s_WINDOW_METADATA_END stage=%s sequence=%llu drawable=%llu matched_windows=%lu windows=%lu windows_truncated=%u\n",
            capturePrefix, stage, sequence, drawable, matches, windows > 32 ? NSUInteger(32) : windows, unsigned(windows > 32));
    fflush(stderr);
}
void requestWindowMetadata(NSHashTable* weakLayer, const char* stage, unsigned long long sequence,
                           unsigned long long drawable, double presentedTime) {
    if (!windowMetadata) return;
    // Weak association does not keep the layer alive until presentation. Only
    // the short queued snapshot retains a still-live layer, avoiding a stale
    // raw-object message or a synchronous AppKit/GPU callback deadlock.
    CAMetalLayer* layer = [[weakLayer anyObject] retain];
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        fprintf(stderr, "%s_WINDOW_METADATA_REQUEST stage=%s sequence=%llu drawable=%llu layer=%p\n",
                capturePrefix, stage, sequence, drawable, layer);
        fflush(stderr);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool { snapshotWindow(layer, stage, sequence, drawable, presentedTime); }
    });
    [layer release];
}
void capture(id<CAMetalDrawable> drawable) {
    const char* directory = getenv("FG_CAPTURE_DIR");
    if (!directory || capturing || objc_getAssociatedObject(drawable, &capturedKey)) return;
    objc_setAssociatedObject(drawable, &capturedKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    capturing = true;
    const auto sequence = ++nextSequence;
    id<MTLTexture> texture = drawable.texture;
    const NSUInteger width = texture.width, height = texture.height;
    // Drawable textures are RGBA/BGRA8 or RGBA16F in the owned presenter.
    const bool half = texture.pixelFormat == MTLPixelFormatRGBA16Float;
    const NSUInteger row = ((width * (half ? 8 : 4) + 255) / 256) * 256;
    id<MTLBuffer> buffer = [texture.device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [texture.device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row * height];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    char path[4096];
    snprintf(path, sizeof(path), "%s/drawable-%06llu.bin", directory, sequence);
    FILE* file = fopen(path, "wb");
    if (file && command.status == MTLCommandBufferStatusCompleted) {
        for (NSUInteger y = 0; y < height; ++y)
            fwrite(static_cast<const unsigned char*>(buffer.contents) + y * row, width * (half ? 8 : 4), 1, file);
        fclose(file);
        std::lock_guard<std::mutex> lock(fileMutex);
        fprintf(stderr, "%s_DRAWABLE sequence=%llu drawable=%llu width=%lu height=%lu format=%lu path=%s\n",
                capturePrefix, sequence, static_cast<unsigned long long>(drawable.drawableID), width, height, texture.pixelFormat, path);
        fflush(stderr);
    } else {
        if (file) fclose(file);
        fprintf(stderr, "%s_OBSERVER_FAIL sequence=%llu status=%lu\n", capturePrefix, sequence, command.status);
    }
    NSHashTable* weakLayer = windowMetadata ? objc_getAssociatedObject(drawable, &acquisitionLayerKey) : nil;
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            fprintf(stderr, "%s_PRESENTED sequence=%llu drawable=%llu time=%.9f\n",
                    capturePrefix, sequence, static_cast<unsigned long long>(presented.drawableID), presented.presentedTime);
            fflush(stderr);
        }
        requestWindowMetadata(weakLayer, "presented", sequence, presented.drawableID, presented.presentedTime);
    }];
    [buffer release]; [queue release];
    capturing = false;
}
void plain(id self, SEL selector) {
    capture(self);
    IMP original;
    { std::lock_guard<std::mutex> lock(hookMutex); original = presents.at(object_getClass(self)); }
    reinterpret_cast<void(*)(id, SEL)>(original)(self, selector);
}
void timed(id self, SEL selector, double duration) {
    capture(self);
    IMP original;
    { std::lock_guard<std::mutex> lock(hookMutex); original = timedPresents.at(object_getClass(self)); }
    reinterpret_cast<void(*)(id, SEL, double)>(original)(self, selector, duration);
}
void atTime(id self, SEL selector, double time) {
    capture(self);
    IMP original;
    { std::lock_guard<std::mutex> lock(hookMutex); original = atPresents.at(object_getClass(self)); }
    reinterpret_cast<void(*)(id, SEL, double)>(original)(self, selector, time);
}
id nextDrawable(id self, SEL selector) {
    [(CAMetalLayer*)self setFramebufferOnly:NO];
    id result = reinterpret_cast<id(*)(id, SEL)>(originalNext)(self, selector);
    if (result) {
        objc_setAssociatedObject(result, &capturedKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        if (windowMetadata) {
            NSHashTable* weakLayer = [NSHashTable weakObjectsHashTable];
            [weakLayer addObject:self];
            objc_setAssociatedObject(result, &acquisitionLayerKey, weakLayer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            requestWindowMetadata(weakLayer, "acquire", 0, [(id<MTLDrawable>)result drawableID], 0);
        }
        Class cls = object_getClass(result);
        std::lock_guard<std::mutex> lock(hookMutex);
        if (!presents.contains(cls)) {
            struct Hook { SEL selector; IMP replacement; std::unordered_map<Class, IMP>* originals; };
            const Hook hooks[] = {{@selector(present), reinterpret_cast<IMP>(plain), &presents},
                                  {@selector(presentAfterMinimumDuration:), reinterpret_cast<IMP>(timed), &timedPresents},
                                  {@selector(presentAtTime:), reinterpret_cast<IMP>(atTime), &atPresents}};
            for (const auto& hook : hooks) {
                Method method = class_getInstanceMethod(cls, hook.selector);
                if (!method) continue;
                hook.originals->emplace(cls, method_getImplementation(method));
                // Add an override on the actual drawable class, not its parent.
                if (!class_addMethod(cls, hook.selector, hook.replacement, method_getTypeEncoding(method)))
                    method_setImplementation(class_getInstanceMethod(cls, hook.selector), hook.replacement);
            }
        }
    }
    return result;
}
__attribute__((constructor)) void install() {
    if (!getenv("FG_CAPTURE_DIR")) return;
    // DYLD injection is inherited by Wine services, including ARM64 wineserver.
    // Ship both native slices but observe only this public consumer process.
    bool consumer = false;
    const char* name = getenv("FG_CAPTURE_CONSUMER");
    if (!name || !*name) name = "owned-d3d12-fsr-fg.exe";
    for (int index = 0; index < *_NSGetArgc(); ++index)
        consumer |= std::strstr((*_NSGetArgv())[index], name) != nullptr;
    if (!consumer) return;
    const char* metadata = getenv("FG_CAPTURE_WINDOW_METADATA");
    windowMetadata = metadata && std::strcmp(metadata, "1") == 0;
    const char* suite = getenv("FG_CAPTURE_SUITE");
    if (suite && std::strcmp(suite, "ngx-fg-present") == 0) capturePrefix = "OWNED_D3D12_NGX_PRESENT";
    @autoreleasepool {
        Method method = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
        originalNext = method_setImplementation(method, reinterpret_cast<IMP>(nextDrawable));
        fprintf(stderr, "%s_OBSERVER_READY public_selectors=1\n", capturePrefix);
    }
}
}
