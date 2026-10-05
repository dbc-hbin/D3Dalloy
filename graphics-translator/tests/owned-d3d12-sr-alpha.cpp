// Independent scalar-in-RGB temporal oracle: alpha must be reconstructed, not copied.
#define main ownedTemporalFixtureMain
#include "owned-d3d12-temporal.cpp"
#undef main

namespace {
std::vector<uint16_t> alphaImage(const OutputResource& output, UINT width, UINT height) {
    uint8_t* mapped = nullptr;
    check(output.readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "alpha Map");
    std::vector<uint16_t> pixels(size_t(width) * height * 4);
    for (UINT y = 0; y < kOutputBacking; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(mapped + output.readback.footprint.Offset +
            y * output.readback.footprint.Footprint.RowPitch);
        for (UINT x = 0; x < kOutputBacking; ++x) {
            if (x >= 3 && x < 3 + width && y >= 5 && y < 5 + height)
                std::copy_n(row + x * 4, 4, pixels.data() + ((y - 5) * width + x - 3) * 4);
            else for (UINT c = 0; c < 4; ++c) if (row[x * 4 + c] != 0x3555) fail("alpha outside sentinel");
        }
    }
    output.readback.buffer->Unmap(0, nullptr);
    return pixels;
}
OutputResource alphaOutput(Gpu& gpu) {
    OutputResource output;
    output.resource = createTexture(gpu.device.Get(), kOutputBacking, kOutputBacking,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> sentinel(size_t(kOutputBacking) * kOutputBacking * 4, 0x3555);
    uploadTexture(gpu, output.resource.Get(), sentinel.data(), kOutputBacking * 8, D3D12_RESOURCE_STATE_COPY_SOURCE);
    output.readback = createTextureReadback(gpu, output.resource.Get());
    return output;
}
}
int main() {
    Gpu gpu;
    requireOwnedBackend();
    ComPtr<IMTLD3D12DeviceExt> ext;
    check(gpu.device->QueryInterface(kIID_IMTLD3D12DeviceExt, reinterpret_cast<void**>(ext.GetAddressOf())), "alpha device QI");
    auto create = makeCreateDesc(TRUE);
    create.DynamicOutputEnabled = TRUE;
    create.FeatureFlags = kFlagMvLowRes | kFlagDepthInverted | 128;
    ComPtr<IMTLD3D12TemporalUpscaleState> alphaState, scalarState;
    check(ext->CreateTemporalUpscaleState(&create, &alphaState), "alpha create");
    create.FeatureFlags &= ~128u;
    check(ext->CreateTemporalUpscaleState(&create, &scalarState), "scalar create");
    auto depth = makeDepth(gpu);
    auto motion = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> vectors(64 * 64 * 2, 0);
    for (size_t i = 0; i < vectors.size(); i += 2) vectors[i] = 0x3c00; // one pixel
    uploadTexture(gpu, motion.Get(), vectors.data(), 64 * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    uint64_t historyDelta = 0;
    std::vector<uint16_t> previous;
    for (UINT frame = 0; frame < 6; ++frame) {
        const UINT input = frame == 3 ? 48 : 64;
        const UINT outputExtent = frame == 3 ? 96 : 128;
        auto color = createTexture(gpu.device.Get(), 68, 68, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto scalar = createTexture(gpu.device.Get(), 68, 68, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint16_t> a(68 * 68 * 4, 0), b(a.size(), 0);
        for (UINT y = 0; y < 68; ++y) for (UINT x = 0; x < 68; ++x) {
            const size_t i = (y * 68 + x) * 4;
            const uint16_t coverage = ((x / 5 + y / 7 + frame) & 1) ? 0x3400 : 0x3a00; // .25 / .75
            a[i] = 0x3800; a[i + 1] = 0x3400; a[i + 2] = 0x3a00; a[i + 3] = coverage;
            b[i] = b[i + 1] = b[i + 2] = coverage; b[i + 3] = 0x3c00;
        }
        uploadTexture(gpu, color.Get(), a.data(), 68 * 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploadTexture(gpu, scalar.Get(), b.data(), 68 * 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto ao = alphaOutput(gpu), so = alphaOutput(gpu);
        auto ad = makeFrameDesc(color.Get(), ao.resource.Get(), depth.Get(), motion.Get(), frame == 0 || frame == 4);
        ad.InputContentWidth = ad.InputContentHeight = input;
        ad.OutputWidth = ad.OutputHeight = outputExtent;
        ad.ColorBaseX = 2; ad.ColorBaseY = 1; ad.OutputBaseX = 3; ad.OutputBaseY = 5;
        ad.JitterOffsetX = frame & 1 ? 0.25f : -0.25f;
        ad.JitterOffsetY = frame & 1 ? -0.125f : 0.125f;
        ad.PreExposure = 2; // Must never affect alpha.
        auto sd = ad; sd.Color = scalar.Get(); sd.Output = so.resource.Get(); sd.PreExposure = 1;
        auto ar = recordTemporal(gpu, alphaState.Get(), ad, ao);
        auto sr = recordTemporal(gpu, scalarState.Get(), sd, so);
        if (frame == 5) { alphaState.Reset(); scalarState.Reset(); }
        const UINT replays = frame == 5 ? 2 : 1;
        for (UINT replay = 0; replay < replays; ++replay) {
            gpu.executeAndWait(ar.list.Get()); gpu.executeAndWait(sr.list.Get());
            auto actual = alphaImage(ao, outputExtent, outputExtent);
            auto reference = alphaImage(so, outputExtent, outputExtent);
            UINT maxUlp = 0; uint16_t lo = 0xffff, hi = 0;
            for (size_t i = 0; i < actual.size(); i += 4) {
                maxUlp = std::max(maxUlp, UINT(std::abs(int(actual[i + 3]) - int(reference[i]))));
                lo = std::min(lo, actual[i + 3]); hi = std::max(hi, actual[i + 3]);
                if (previous.size() == actual.size()) historyDelta += std::abs(int(actual[i + 3]) - int(previous[i + 3]));
            }
            if (maxUlp > 4 || lo >= 0x3800 || hi <= 0x3800 || hi > 0x3c00) fail("independent temporal alpha oracle");
            previous = actual;
            std::printf("OWNED_D3D12_SR_ALPHA_CASE_PASS frame=%u replay=%u scalar_oracle_max_half_ulp=%u subrect=1 dynamic=%u retired=%u\n",
                        frame, replay, maxUlp, frame == 3, frame == 5);
        }
    }
    if (!historyDelta) fail("alpha history stimulus unchanged");
    std::printf("OWNED_D3D12_SR_ALPHA_PASS frames=6 replay_after_release=1 independent_scalar_oracle=1 history_delta=%llu\n", static_cast<unsigned long long>(historyDelta));
    return 0;
}
