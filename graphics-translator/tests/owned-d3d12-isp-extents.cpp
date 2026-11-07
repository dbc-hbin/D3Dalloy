// Public NGX feature 9. Generate a serial oracle on the same device with a
// fresh feature for each evaluation; compare a sealed list and its replay.
#define main ownedNgxFixtureMain
#include "owned-d3d12-ngx.cpp"
#undef main

extern "C" {
void ngx_feature_set_uint(NVSDK_NGX_Parameter *, const char *, unsigned);
void ngx_feature_set_resource(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
}

namespace {
constexpr UINT outputWidth = 96, outputHeight = 80, left = 3, top = 2;
constexpr UINT width = outputWidth + 2 * left, height = outputHeight + 2 * top;
constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

NVSDK_NGX_Handle *createIsp(const NgxApi &api, ID3D12GraphicsCommandList *list) {
  auto *params = allocateParams(api);
  ngx_feature_set_uint(params, "Width", 64);
  ngx_feature_set_uint(params, "Height", 48);
  ngx_feature_set_uint(params, "OutWidth", outputWidth);
  ngx_feature_set_uint(params, "OutHeight", outputHeight);
  NVSDK_NGX_Handle *feature = nullptr;
  checkNgxResult(api.create(list, 9, params, &feature), ngx_fixture_result_success(), "ISP create");
  resetAndDestroyParams(api, params);
  return feature;
}

void checkPixels(FILE *file, bool reference, std::vector<TextureReadback> &results) {
  std::array<uint8_t, width * 4> expected{};
  for (auto &result : results) {
    uint8_t *pixels = nullptr;
    check(result.buffer->Map(0, nullptr, reinterpret_cast<void **>(&pixels)), "ISP readback");
    for (UINT y = 0; y < height; ++y) {
      const auto *row = pixels + result.footprint.Offset + y * result.footprint.Footprint.RowPitch;
      for (UINT x = 0; x < width; ++x)
        if (x < left || y < top || x >= left + outputWidth || y >= top + outputHeight)
          for (UINT c = 0; c < 4; ++c) if (row[4 * x + c] != 19) fail("ISP atlas margin");
      if (reference) {
        if (std::fwrite(row, 1, expected.size(), file) != expected.size()) fail("write ISP oracle");
      } else if (std::fread(expected.data(), 1, expected.size(), file) != expected.size() ||
                 std::memcmp(row, expected.data(), expected.size())) {
        fail("ISP differs from serial oracle");
      }
    }
    result.buffer->Unmap(0, nullptr);
  }
  if (!reference && std::fgetc(file) != EOF) fail("ISP oracle length");
}
}

int main(int argc, char **argv) {
  if (argc != 3) fail("expected --reference|--check|--many-features <oracle.rgba>");
  const bool reference = !std::strcmp(argv[1], "--reference");
  const bool manyFeatures = !std::strcmp(argv[1], "--many-features");
  if (!reference && !manyFeatures && std::strcmp(argv[1], "--check")) fail("unknown ISP mode");
  requireOwnedBackend();
  NgxApi api;
  Gpu gpu;
  checkNgxResult(api.init(0x49535076, L"C:\\", gpu.device.Get(), nullptr, ngx_fixture_sdk_version()),
                 ngx_fixture_result_success(), "NGX init");
  auto texture = [&](D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    return createTexture(gpu.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, flags, state);
  };
  auto color = texture(D3D12_RESOURCE_FLAG_NONE, read);
  auto dst = texture(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<uint8_t> pixels(width * height * 4, 19);
  uploadTexture(gpu, dst.Get(), pixels.data(), width * 4, write);
  ComPtr<ID3D12Resource> seeds[2];
  for (UINT parity = 0; parity < 2; ++parity) {
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
      auto *p = &pixels[(y * width + x) * 4];
      p[0] = uint8_t(13 * x + 7 * y + 33 * parity);
      p[1] = uint8_t((x / 6 ^ y / 9) * 37 + 89 * parity);
      p[2] = uint8_t(3 * x - 11 * y + 179 * parity);
      p[3] = 255;
    }
    seeds[parity] = texture(D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    uploadTexture(gpu, seeds[parity].Get(), pixels.data(), width * 4, D3D12_RESOURCE_STATE_COPY_SOURCE);
  }

  auto rec = gpu.makeRecording();
  NVSDK_NGX_Handle *feature = nullptr;
  std::vector<TextureReadback> results;
  constexpr UINT extents[][2] = {{48, 40}, {64, 48}, {96, 80}, {36, 42}, {84, 57}, {56, 76}};
  for (UINT i = 0; i < 42; ++i) {
    // Exceed the 16-slot constant ring first, then the four-generation cache.
    const auto &extent = extents[i < 18 ? 0 : (i - 18) % 6];
    if (!feature) feature = createIsp(api, rec.list.Get());
    transition(rec.list.Get(), color.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
    rec.list->CopyResource(color.Get(), seeds[i % 2].Get());
    transition(rec.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    auto *params = allocateParams(api);
    ngx_feature_set_resource(params, "Color", color.Get());
    ngx_feature_set_resource(params, "Output", dst.Get());
    ngx_feature_set_uint(params, "Rect.X", left);
    ngx_feature_set_uint(params, "Rect.Y", top);
    ngx_feature_set_uint(params, "Rect.W", extent[0]);
    ngx_feature_set_uint(params, "Rect.H", extent[1]);
    checkNgxResult(api.evaluate(rec.list.Get(), feature, params, nullptr), ngx_fixture_result_success(), "ISP evaluate");
    resetAndDestroyParams(api, params);
    transition(rec.list.Get(), dst.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
    results.push_back(createTextureReadback(gpu, dst.Get()));
    copyTextureToReadback(rec.list.Get(), dst.Get(), results.back());
    transition(rec.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
    if (reference || manyFeatures || i == 41) {
      checkNgxResult(api.release(feature), ngx_fixture_result_success(), "ISP release before submit");
      feature = nullptr;
    }
    if (reference) {
      check(rec.list->Close(), "close serial ISP");
      gpu.executeAndWait(rec.list.Get());
      rec = gpu.makeRecording();
    }
  }
  check(rec.list->Close(), "close final ISP");
  color.Reset();
  dst.Reset();
  FILE *file = std::fopen(argv[2], reference ? "wb" : "rb");
  if (!file) fail("open ISP oracle");
  for (UINT replay = 0; replay < (reference ? 1u : 2u); ++replay) {
    if (replay) gpu.queue = createDirectQueue(gpu.device.Get(), "second ISP queue");
    if (!reference) gpu.executeAndWait(rec.list.Get());
    std::rewind(file);
    checkPixels(file, reference, results);
  }
  if (std::fclose(file)) fail("close ISP oracle");
  checkNgxResult(api.shutdown1(gpu.device.Get()), ngx_fixture_result_success(), "NGX shutdown");
  std::printf("OWNED_ISP_EXTENTS_PASS frames=42 extents=6 same=18 mode=%s\n", argv[1]);
}
