// Test-only observer. Public Objective-C selectors, no renderer offsets or
// private pointers. Present is intercepted after the owned GPU completion.
#import <Foundation/Foundation.h>
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
thread_local bool capturing = false;
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
        fprintf(stderr, "OWNED_D3D12_FSR_FG_DRAWABLE sequence=%llu drawable=%llu width=%lu height=%lu format=%lu path=%s\n",
                sequence, static_cast<unsigned long long>(drawable.drawableID), width, height, texture.pixelFormat, path);
        fflush(stderr);
    } else {
        if (file) fclose(file);
        fprintf(stderr, "OWNED_D3D12_FSR_FG_OBSERVER_FAIL sequence=%llu status=%lu\n", sequence, command.status);
    }
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        std::lock_guard<std::mutex> lock(fileMutex);
        fprintf(stderr, "OWNED_D3D12_FSR_FG_PRESENTED sequence=%llu drawable=%llu time=%.9f\n",
                sequence, static_cast<unsigned long long>(presented.drawableID), presented.presentedTime);
        fflush(stderr);
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
    for (int index = 0; index < *_NSGetArgc(); ++index)
        consumer |= std::strstr((*_NSGetArgv())[index], "owned-d3d12-fsr-fg.exe") != nullptr;
    if (!consumer) return;
    @autoreleasepool {
        Method method = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
        originalNext = method_setImplementation(method, reinterpret_cast<IMP>(nextDrawable));
        fprintf(stderr, "OWNED_D3D12_FSR_FG_OBSERVER_READY public_selectors=1\n");
    }
}
}
