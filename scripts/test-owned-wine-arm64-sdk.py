#!/usr/bin/env python3
"""Exercise actual archived WineMetalView layers in ARM64 and x64 driver dylibs.

No Wine guest, prefix, patched-out driver, or fake D3DMetal class is used. x64
runs under the host's existing Rosetta support; ARM64 uses ordinary CAMetalLayer.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

PROBE = r'''
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>

@interface NSView (WineMetalProbe)
- (id)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device;
- (void)setRetinaMode:(BOOL)mode;
@end

int main(int argc, char **argv)
{
    @autoreleasepool
    {
        if (argc != 2) return 2;
        void *driver = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
        if (!driver) { fprintf(stderr, "%s\n", dlerror()); return 3; }
        Class viewClass = NSClassFromString(@"WineMetalView");
        if (!viewClass) return 4;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 5;
        NSView *view = [[viewClass alloc] initWithFrame:NSMakeRect(0, 0, 32, 32) device:device];
        CAMetalLayer *layer = (CAMetalLayer *)[view makeBackingLayer];
#if defined(__x86_64__)
        const char *expected = "WineMetalLayer";
        Class wineLayer = NSClassFromString(@"WineMetalLayer");
        if (!wineLayer || !class_getInstanceMethod(wineLayer, @selector(nextDrawable))) return 6;
        if (class_getMethodImplementation(wineLayer, @selector(nextDrawable)) ==
            class_getMethodImplementation([CAMetalLayer class], @selector(nextDrawable))) return 7;
#else
        const char *expected = "CAMetalLayer";
        if (NSClassFromString(@"WineMetalLayer")) return 8;
#endif
        if (strcmp(class_getName([layer class]), expected) || layer.device != device ||
            !layer.framebufferOnly || ![layer.magnificationFilter isEqualToString:kCAFilterNearest] ||
            !CGColorEqualToColor(layer.backgroundColor, CGColorGetConstantColor(kCGColorBlack))) return 9;
        [view setRetinaMode:YES];
        if (view.layer.contentsScale != 2.0) return 10;
        [view setRetinaMode:NO];
        if (view.layer.contentsScale != 1.0) return 11;
        layer.drawableSize = CGSizeMake(32, 32);
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable || drawable.texture.width != 32 || drawable.texture.height != 32) return 12;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted || commands.error) return 13;
        printf("layer=%s device=%s drawable=32x32 clear=completed retina=2,1\n", expected, device.name.UTF8String);
        for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        {
            const char *name = _dyld_get_image_name(i);
            if (strstr(name, "winemac") || strstr(name, "ntdll") || strstr(name, "win32u"))
                printf("loaded=%s\n", name);
        }
        [view release];
        [queue release];
        [device release];
    }
    return 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.wine_root.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error("output must be fresh")
    output.mkdir(parents=True)
    source = output / "metal-layer-probe.m"
    source.write_text(PROBE)
    result = {"schema": 1, "scope": "real native Wine driver layers; no Windows/FEX guest",
              "guest_runtime": "not_run", "commands": [], "branches": [], "status": "running"}

    def run(command: list[str], label: str, environment: dict[str, str]) -> str:
        process = subprocess.run(command, env=environment, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = output / (label + ".log")
        log.write_text(process.stdout)
        result["commands"].append({"command": command, "returncode": process.returncode, "log": str(log)})
        if process.returncode:
            raise RuntimeError(f"{label} failed ({process.returncode}): {log}")
        return process.stdout

    try:
        for name, arch in (("hybrid", "arm64"), ("x64-check", "x86_64")):
            build = root / ("wine-build-" + name)
            driver = build / "dlls/winemac.drv/winemac.so"
            executable = output / ("metal-layer-probe-" + arch)
            environment = dict(os.environ)
            run(["/usr/bin/clang", "-arch", arch, "-fblocks", str(source), "-o", str(executable),
                 "-Wl,-rpath," + str(build / "dlls/ntdll"),
                 "-Wl,-rpath," + str(build / "dlls/win32u"),
                 "-framework", "AppKit", "-framework", "Metal", "-framework", "QuartzCore"],
                arch + "-compile", environment)
            architecture = run(["/usr/bin/lipo", "-archs", str(driver)], arch + "-driver-arch", environment).strip()
            if architecture != arch:
                raise RuntimeError("driver architecture mismatch: " + architecture)
            stdout = run(["/usr/bin/arch", "-" + arch, str(executable), str(driver)], arch + "-runtime", environment)
            result["branches"].append({"native_arch": arch, "driver": str(driver),
                                       "driver_sha256": hashlib.sha256(driver.read_bytes()).hexdigest(),
                                       "runtime": stdout, "status": "pass"})
        result["status"] = "pass"
    except (OSError, RuntimeError) as error:
        result["status"] = "blocked"
        result["blocker"] = str(error)
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
