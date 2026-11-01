// Native GPU probe for the production FG depth/motion normalization kernels in
// src/winemetal/unix/fsr-kernels.metal (passed as argv[1]). No Wine or MetalFX.
// Checks 3x3 nearest-depth dilation (center wins ties, inverted ordering, clamped
// edges), motion read at the winning sample, and round-to-nearest half motion
// stores with finite saturation while NaN/Inf are preserved, and that the fused
// depth+motion kernel is bit-identical to the two separate passes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct alignas(16) FgParams {
    std::int32_t sourceOrigin[2];
    std::uint32_t extent[2], sourceExtent[2], motionTargetExtent[2], outputExtent[2];
    float motionScale[2], jitterCancellation[2];
    float minLuminance, maxLuminance;
    std::uint32_t transfer, debugFlags, reset, disableHint;
    std::uint32_t uiOrigin[2], sceneOrigin[2], presentOrigin[2];
    std::uint32_t uiMode, reconstructCameraMotion;
    float clipToPreviousClip[16];
    float motionVectorsInvalidValue;
    std::uint32_t motionVectorsInvalidValuePresent;
    std::uint32_t depthSourceExtent[2];
    std::uint32_t depthDilation, depthInverted;
};
static_assert(sizeof(FgParams) == 208 && offsetof(FgParams, depthSourceExtent) == 184);

unsigned failures = 0;
void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "OWNED_FG_NORMALIZATION_FAIL reason=%s\n", reason); std::exit(1); }
}
id<MTLTexture> makeTexture(id<MTLDevice> device, MTLPixelFormat format, unsigned w, unsigned h,
                           const void* data, size_t pixel) {
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    require(texture != nil, "texture");
    if (data) [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data bytesPerRow:w * pixel];
    return texture;
}
void run(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pipeline, const FgParams& params,
         std::initializer_list<std::pair<id<MTLTexture>, unsigned>> textures) {
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBytes:&params length:sizeof(params) atIndex:0];
    // Unused declared slots bind the first texture, as production computePass does.
    for (unsigned i = 0; i < 7; ++i) [encoder setTexture:textures.begin()->first atIndex:i];
    for (const auto& [texture, index] : textures) [encoder setTexture:texture atIndex:index];
    [encoder dispatchThreads:MTLSizeMake(params.extent[0], params.extent[1], 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "command");
}
std::array<unsigned, 2> sourcePosition(unsigned x, unsigned y, const unsigned extent[2], const unsigned source[2]) {
    return {std::min(unsigned((x + .5f) * source[0] / extent[0]), source[0] - 1),
            std::min(unsigned((y + .5f) * source[1] / extent[1]), source[1] - 1)};
}
std::array<unsigned, 2> nearest(const std::vector<float>& depth, unsigned w, unsigned h,
                                std::array<unsigned, 2> center, bool inverted) {
    auto best = center;
    float bestDepth = depth[center[1] * w + center[0]];
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const int x = int(center[0]) + dx, y = int(center[1]) + dy;
        if ((!dx && !dy) || x < 0 || y < 0 || x >= int(w) || y >= int(h)) continue;
        const float candidate = depth[y * w + x];
        if (inverted ? candidate > bestDepth : candidate < bestDepth) { best = {unsigned(x), unsigned(y)}; bestDepth = candidate; }
    }
    return best;
}
bool sameHalf(__fp16 actual, float value) {
    const __fp16 expected = __fp16(std::isfinite(value) ? std::clamp(value, -65504.f, 65504.f) : value);
    if (std::isnan(value)) return std::isnan(float(actual));
    return !std::memcmp(&actual, &expected, sizeof(expected));
}
}

int main(int argc, char** argv) {
    require(argc == 2, "usage-kernel-source");
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "device");
        NSError* error = nil;
        NSString* source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]
                                                     encoding:NSUTF8StringEncoding error:&error];
        require(source != nil, "kernel-source");
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        require(library != nil, "kernel-compile");
        auto pipeline = [&](NSString* name) {
            id<MTLFunction> function = [library newFunctionWithName:name];
            require(function != nil, "kernel-function");
            id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:function error:&error];
            require(state != nil, "kernel-pipeline");
            return state;
        };
        id<MTLComputePipelineState> depthPipeline = pipeline(@"yaagl_fg_resample_depth");
        id<MTLComputePipelineState> motionPipeline = pipeline(@"yaagl_fg_normalize_motion");
        id<MTLComputePipelineState> fusedPipeline = pipeline(@"yaagl_fg_normalize_depth_motion");
        id<MTLCommandQueue> queue = [device newCommandQueue];

        // Half-store rounding: RG16Float float-typed stores may truncate on Apple GPUs.
        unsigned roundingMismatches = 0;
        {
            constexpr unsigned w = 64, h = 32;
            const float controls[]{0.f, 1.0008f, 10.006f, 64.05f, 256.2f, 512.4f, 1024.8f, 2049.6f, 3841.6f,
                1.00048828125f, 1.00146484375f, 65504.f, 65519.f, 65520.f, 65521.f, 65535.f, 65536.f, 1.e6f,
                0x1p120f, INFINITY, NAN};
            std::vector<std::array<float, 2>> values(w * h);
            for (unsigned i = 0; i < values.size(); ++i) {
                const float v = i < std::size(controls) ? controls[i] : std::ldexp(1.f + float(i % 97) / 97.f, int(i % 26) - 10);
                values[i] = {v, -v};
            }
            const float one = .5f;
            std::vector<float> depth(w * h, one);
            auto motion = makeTexture(device, MTLPixelFormatRG32Float, w, h, values.data(), 8);
            auto depthTexture = makeTexture(device, MTLPixelFormatR32Float, w, h, depth.data(), 4);
            auto output = makeTexture(device, MTLPixelFormatRG16Float, w, h, nullptr, 4);
            FgParams p{};
            p.extent[0] = p.sourceExtent[0] = p.outputExtent[0] = p.motionTargetExtent[0] = p.depthSourceExtent[0] = w;
            p.extent[1] = p.sourceExtent[1] = p.outputExtent[1] = p.motionTargetExtent[1] = p.depthSourceExtent[1] = h;
            p.motionScale[0] = p.motionScale[1] = 1;
            run(queue, motionPipeline, p, {{motion, 0}, {output, 1}, {depthTexture, 2}, {depthTexture, 4}});
            std::vector<std::array<__fp16, 2>> actual(w * h);
            [output getBytes:actual.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            for (unsigned i = 0; i < values.size(); ++i)
                for (unsigned c = 0; c < 2; ++c) roundingMismatches += !sameHalf(actual[i][c], values[i][c]);
        }
        std::printf("OWNED_FG_NORMALIZATION_ROUNDING components=%u mismatches=%u\n", 64u * 32u * 2u, roundingMismatches);

        // Dilation: 7x5 render depth/motion upsampled into 17x13; ties include
        // equal neighbors so a non-center winner would differ from the reference.
        unsigned dilationMismatches = 0, cases = 0;
        constexpr unsigned rw = 7, rh = 5, ow = 17, oh = 13;
        std::vector<float> depth(rw * rh);
        for (unsigned i = 0; i < depth.size(); ++i) depth[i] = float((i * 7) % 5) / 8.f + (i % 3 == 0 ? 0.f : .03125f);
        depth[2 * rw + 3] = depth[2 * rw + 4]; // Exact tie with the center sample.
        std::vector<std::array<float, 2>> vectors(rw * rh);
        for (unsigned i = 0; i < vectors.size(); ++i) vectors[i] = {float(i) / 16.f, -float(i % 9) / 8.f};
        auto depthTexture = makeTexture(device, MTLPixelFormatR32Float, rw, rh, depth.data(), 4);
        auto motionTexture = makeTexture(device, MTLPixelFormatRG32Float, rw, rh, vectors.data(), 8);
        for (unsigned inverted = 0; inverted < 2; ++inverted)
            for (unsigned dilate = 0; dilate < 2; ++dilate) {
                auto outDepth = makeTexture(device, MTLPixelFormatR32Float, ow, oh, nullptr, 4);
                auto outMotion = makeTexture(device, MTLPixelFormatRG16Float, ow, oh, nullptr, 4);
                FgParams d{};
                d.extent[0] = ow; d.extent[1] = oh;
                d.sourceExtent[0] = rw; d.sourceExtent[1] = rh;
                d.depthDilation = dilate; d.depthInverted = inverted;
                FgParams m = d;
                m.motionTargetExtent[0] = rw; m.motionTargetExtent[1] = rh;
                m.outputExtent[0] = ow; m.outputExtent[1] = oh;
                m.motionScale[0] = m.motionScale[1] = 1;
                m.depthSourceExtent[0] = rw; m.depthSourceExtent[1] = rh;
                run(queue, depthPipeline, d, {{depthTexture, 0}, {outDepth, 1}});
                run(queue, motionPipeline, m, {{motionTexture, 0}, {outMotion, 1}, {outDepth, 2}, {depthTexture, 4}});
                std::vector<float> actualDepth(ow * oh);
                std::vector<std::array<__fp16, 2>> actualMotion(ow * oh);
                [outDepth getBytes:actualDepth.data() bytesPerRow:ow * 4 fromRegion:MTLRegionMake2D(0, 0, ow, oh) mipmapLevel:0];
                [outMotion getBytes:actualMotion.data() bytesPerRow:ow * 4 fromRegion:MTLRegionMake2D(0, 0, ow, oh) mipmapLevel:0];
                const unsigned extent[2]{ow, oh}, render[2]{rw, rh};
                for (unsigned y = 0; y < oh; ++y) for (unsigned x = 0; x < ow; ++x) {
                    auto center = sourcePosition(x, y, extent, render);
                    auto winner = dilate ? nearest(depth, rw, rh, center, inverted) : center;
                    const unsigned i = winner[1] * rw + winner[0];
                    dilationMismatches += actualDepth[y * ow + x] != depth[i];
                    for (unsigned c = 0; c < 2; ++c)
                        dilationMismatches += !sameHalf(actualMotion[y * ow + x][c], vectors[i][c] * float(extent[c]) / float(render[c]));
                }
                ++cases;
            }
        std::printf("OWNED_FG_NORMALIZATION_DILATION cases=%u pixels=%u mismatches=%u\n", cases, ow * oh, dilationMismatches);

        // Fused depth+motion pass: bit-identical to depth then motion, including
        // camera reconstruction (which reads the depth the separate pass stored).
        // A motion grid larger than depth exercises the winner remap between grids.
        constexpr unsigned mw = 11, mh = 8;
        std::vector<std::array<float, 2>> wideVectors(mw * mh);
        for (unsigned i = 0; i < wideVectors.size(); ++i) wideVectors[i] = {float(i % 13) / 4.f, -float(i) / 32.f};
        auto wideMotion = makeTexture(device, MTLPixelFormatRG32Float, mw, mh, wideVectors.data(), 8);
        unsigned fusedMismatches = 0, fusedCases = 0;
        for (unsigned wide = 0; wide < 2; ++wide)
        for (unsigned inverted = 0; inverted < 2; ++inverted)
            for (unsigned dilate = 0; dilate < 2; ++dilate)
                for (unsigned camera = 0; camera < 2; ++camera) {
                    auto make = [&](MTLPixelFormat format, size_t pixel) {
                        return makeTexture(device, format, ow, oh, nullptr, pixel);
                    };
                    auto separateDepth = make(MTLPixelFormatR32Float, 4), fusedDepth = make(MTLPixelFormatR32Float, 4);
                    auto separateMotion = make(MTLPixelFormatRG16Float, 4), fusedMotion = make(MTLPixelFormatRG16Float, 4);
                    FgParams d{};
                    d.extent[0] = ow; d.extent[1] = oh;
                    d.sourceExtent[0] = rw; d.sourceExtent[1] = rh;
                    d.depthDilation = dilate; d.depthInverted = inverted;
                    FgParams m = d;
                    if (wide) { m.sourceExtent[0] = mw; m.sourceExtent[1] = mh; }
                    id<MTLTexture> motionSource = wide ? wideMotion : motionTexture;
                    m.motionTargetExtent[0] = rw; m.motionTargetExtent[1] = rh;
                    m.outputExtent[0] = ow; m.outputExtent[1] = oh;
                    m.motionScale[0] = 1.5f; m.motionScale[1] = -.75f;
                    m.jitterCancellation[0] = .125f; m.jitterCancellation[1] = -.25f;
                    m.motionVectorsInvalidValue = vectors[3][0];
                    m.motionVectorsInvalidValuePresent = 1;
                    m.depthSourceExtent[0] = rw; m.depthSourceExtent[1] = rh;
                    m.reconstructCameraMotion = camera;
                    const float clip[16]{1.f, .01f, 0, .002f, -.02f, .98f, 0, 0, .03f, -.01f, 1.01f, .001f, .004f, 0, .02f, 1.f};
                    std::memcpy(m.clipToPreviousClip, clip, sizeof(clip));
                    run(queue, depthPipeline, d, {{depthTexture, 0}, {separateDepth, 1}});
                    run(queue, motionPipeline, m, {{motionSource, 0}, {separateMotion, 1}, {separateDepth, 2}, {depthTexture, 4}});
                    run(queue, fusedPipeline, m, {{motionSource, 0}, {fusedMotion, 1}, {fusedDepth, 2}, {depthTexture, 4}});
                    std::vector<std::uint32_t> a(ow * oh), b(ow * oh), c(ow * oh), e(ow * oh);
                    const auto region = MTLRegionMake2D(0, 0, ow, oh);
                    [separateDepth getBytes:a.data() bytesPerRow:ow * 4 fromRegion:region mipmapLevel:0];
                    [fusedDepth getBytes:b.data() bytesPerRow:ow * 4 fromRegion:region mipmapLevel:0];
                    [separateMotion getBytes:c.data() bytesPerRow:ow * 4 fromRegion:region mipmapLevel:0];
                    [fusedMotion getBytes:e.data() bytesPerRow:ow * 4 fromRegion:region mipmapLevel:0];
                    for (unsigned i = 0; i < a.size(); ++i) fusedMismatches += (a[i] != b[i]) + (c[i] != e[i]);
                    ++fusedCases;
                }
        std::printf("OWNED_FG_NORMALIZATION_FUSED cases=%u pixels=%u mismatches=%u\n", fusedCases, ow * oh, fusedMismatches);
        failures = roundingMismatches + dilationMismatches + fusedMismatches;
    }
    if (failures) { std::fprintf(stderr, "OWNED_FG_NORMALIZATION_FAIL mismatches=%u\n", failures); return 1; }
    std::puts("OWNED_FG_NORMALIZATION_PASS");
    return 0;
}
