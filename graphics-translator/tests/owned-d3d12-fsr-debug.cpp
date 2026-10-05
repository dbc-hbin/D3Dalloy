// Public FFX dispatch diagnostic image contract, with numerical depth/motion oracles.
#define OWNED_FSR_FIXTURE_ENTRY ownedFsrFixtureMain
#include "owned-d3d12-fsr.cpp"
#undef OWNED_FSR_FIXTURE_ENTRY

int main() {
    FsrApi api;
    Gpu gpu;
    requireOwnedBackend();
    FsrCreation creation(gpu.device.Get());
    creation.desc.flags = (kFsrFlags & ~FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "debug_create");
    auto color = makeColor(gpu, 112);
    auto depth = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> depths(64 * 64);
    for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x) depths[y * 64 + x] = (x + 0.5f) / 64;
    uploadTexture(gpu, depth.Get(), depths.data(), 64 * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto motion = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> vectors(64 * 64 * 2);
    for (size_t i = 0; i < vectors.size(); i += 2) { vectors[i] = 0x3400; vectors[i + 1] = 0xb000; } // .25,-.125
    uploadTexture(gpu, motion.Get(), vectors.data(), 64 * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (UINT extent : {128u, 96u}) {
        auto output = makeOutput(gpu, 1700 + extent);
        auto recording = gpu.makeRecording();
        auto desc = fsrFrame(recording.list.Get(), color.Get(), depth.Get(), motion.Get(), output.resource.Get());
        desc.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
        desc.upscaleSize = {extent, extent};
        desc.renderSize = extent == 96 ? FfxApiDimensions2D{48, 48} : FfxApiDimensions2D{64, 64};
        desc.reset = true;
        fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "debug_dispatch");
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "debug Close");
        if (extent == 96) fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "debug_destroy_before_replay");
        for (UINT replay = 0; replay < 2; ++replay) {
            gpu.executeAndWait(recording.list.Get());
            auto image = fsrPixels(output, extent, extent);
            UINT maxError = 0;
            for (UINT y = 4; y < extent - 4; ++y) for (UINT x = 0; x < extent; ++x) {
                const auto* p = image.data() + (y * extent + x) * 4;
                const UINT panel = std::min(x * 3 / extent, 2u);
                const UINT start = (panel * extent + 2) / 3;
                const UINT end = ((panel + 1) * extent + 2) / 3;
                UINT expected[3]{};
                if (panel == 0) expected[0] = expected[1] = expected[2] = 112;
                else if (panel == 1) {
                    const UINT dx = std::min(UINT((x - start + 0.5f) * desc.renderSize.width / (end - start)), desc.renderSize.width - 1);
                    expected[0] = expected[1] = expected[2] = UINT(std::lround((1.0f - (dx + 0.5f) / 64) * 255));
                } else {
                    expected[0] = 191; expected[1] = 96;
                    expected[2] = UINT(std::lround(std::sqrt(16.0 * 16.0 + 8.0 * 8.0) / 32.0 * 255));
                }
                for (UINT c = 0; c < 3; ++c) maxError = std::max(maxError, UINT(std::abs(int(p[c]) - int(expected[c]))));
                if (p[3] != 255) fail("debug opaque panel");
            }
            if (maxError > 2) fail("debug data numerical oracle");
            std::printf("OWNED_D3D12_FSR_DEBUG_CASE_PASS extent=%u replay=%u panel_max_byte_error=%u sentinel=1 retired=%u\n", extent, replay, maxError, extent == 96);
        }
    }
    // Create-time permission is meaningful; flag validation happens before recording.
    creation.desc.flags &= ~FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "debug_permission_create");
    auto output = makeOutput(gpu, 1800);
    auto recording = gpu.makeRecording();
    auto desc = fsrFrame(recording.list.Get(), color.Get(), depth.Get(), motion.Get(), output.resource.Get());
    desc.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_ERROR_PARAMETER, "debug_permission_required");
    fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "debug_permission_destroy");
    std::printf("OWNED_D3D12_FSR_DEBUG_PASS public_dispatch=1 depth_gradient=1 signed_motion=1 color=1 dynamic=1 retirement_replay=1\n");
    return 0;
}
