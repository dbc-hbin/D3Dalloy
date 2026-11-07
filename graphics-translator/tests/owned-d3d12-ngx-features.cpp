// Public NGX FG/RR entry points with the real MSVC parameter consumer.
#define main ownedNgxSupportMain
#include "owned-d3d12-ngx.cpp"
#undef main
#include "owned-rr-pattern.h"
#include <cmath>
#include <limits>

extern "C" {
void ngx_feature_set_uint(NVSDK_NGX_Parameter *, const char *, unsigned int);
void ngx_feature_set_float(NVSDK_NGX_Parameter *, const char *, float);
void ngx_feature_set_resource(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
void ngx_feature_set_pointer(NVSDK_NGX_Parameter *, const char *, void *);
unsigned int ngx_feature_get_uint(NVSDK_NGX_Parameter *, const char *, unsigned int *);
}
#include "owned-d3d12-ngx-present.inc"
#include "owned-fg-contract-guest.inc"
namespace {
void checkCapabilities(const NgxApi &api,Gpu &gpu) {
  const GUID oldFgDevice = {0xb2a31090,0xa1f2,0x4717,{0xbb,0x50,0x61,0x7a,0x47,0xf2,0xa0,0x11}};
  ComPtr<IUnknown> oldFg;
  if (gpu.device->QueryInterface(oldFgDevice, reinterpret_cast<void**>(oldFg.GetAddressOf())) != E_NOINTERFACE || oldFg)
    fail("obsolete FG factory ABI accepted");
  auto get = loadExport<NgxAllocateParameters>(api.module,"NVSDK_NGX_D3D12_GetCapabilityParameters");
  NVSDK_NGX_Parameter *params = nullptr;
  checkNgxResult(get(&params),ngx_fixture_result_success(),"feature capability parameters");
  for (const char *key : {"FrameGeneration.Available","SuperSamplingDenoising.Available","ImageSignalProcessing.Available"}) {
    UINT value = 0;
    checkNgxResult(ngx_feature_get_uint(params,key,&value),ngx_fixture_result_success(),key);
    if (value != 1) fail(key);
  }
  UINT countMax = 0;
  checkNgxResult(ngx_feature_get_uint(params, "DLSSG.MultiFrameCountMax", &countMax), ngx_fixture_result_success(), "FG generated capacity");
  if (countMax != 1) fail("public FG advertised generated capacity");
  for (const char *key : {"InPainting.Available","ImageSuperResolution.Available","SlowMotion.Available",
       "VideoSuperResolution.Available","DeepResolve.Available","DeepDVC.Available"}) {
    UINT value = 1;
    checkNgxResult(ngx_feature_get_uint(params,key,&value),ngx_fixture_result_success(),key);
    if (value) fail("unsupported public NGX feature capability");
  }
  resetAndDestroyParams(api,params);
  params=allocateParams(api);
  auto recording=gpu.makeRecording();
  for(UINT feature : {0u,2u,3u,4u,5u,6u,7u,8u,10u,12u,14u,15u,16u,17u,18u,19u,0xffffffffu}) {
    NVSDK_NGX_Handle *handle=nullptr;
    checkNgxResult(api.create(recording.list.Get(),feature,params,&handle),ngx_fixture_result_feature_not_supported(),"unsupported feature CreateFeature contract");
    if(handle) fail("unsupported feature fabricated a handle");
  }
  check(recording.list->Close(),"unsupported inventory close"); resetAndDestroyParams(api,params);
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=exact_capabilities fg=1 rr=1 isp=1 other=0");
}

void runPublicSpatial(const NgxApi &api, Gpu &gpu) {
  constexpr UINT outWidth = 96, outHeight = 80;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  auto create = gpu.makeRecording();
  auto params = allocateParams(api);
  ngx_feature_set_uint(params, "Width", 64);
  ngx_feature_set_uint(params, "Height", 48);
  ngx_feature_set_uint(params, "OutWidth", outWidth);
  ngx_feature_set_uint(params, "OutHeight", outHeight);
  NVSDK_NGX_Handle *feature = nullptr;
  checkNgxResult(api.create(create.list.Get(), 9, params, &feature), ngx_fixture_result_success(), "ISP create");
  check(create.list->Close(), "ISP close create");
  resetAndDestroyParams(api, params);

  for (UINT atlas = 0; atlas < 2; ++atlas) {
    const UINT left = atlas ? 5 : 0, top = atlas ? 3 : 0;
    const UINT width = outWidth + 2 * left, height = outHeight + 2 * top;
    std::vector<uint8_t> input(width * height * 4);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
      auto *p = &input[(y * width + x) * 4];
      p[0] = uint8_t(x * 13 + y * 7);
      p[1] = uint8_t((x / 6 ^ y / 9) * 37);
      p[2] = uint8_t(x * 3 - y * 11);
      p[3] = 255;
    }
    auto color = createTexture(gpu.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto output = createTexture(gpu.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> sentinel(input.size(), 19);
    uploadTexture(gpu, color.Get(), input.data(), width * 4, read);
    uploadTexture(gpu, output.Get(), sentinel.data(), width * 4, write);
    auto record = gpu.makeRecording();
    params = allocateParams(api);
    ngx_feature_set_resource(params, "Color", color.Get());
    ngx_feature_set_resource(params, "Output", output.Get());
    if (atlas) {
      ngx_feature_set_uint(params, "Rect.X", width);
      checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr),
          ngx_fixture_result_invalid_parameter(), "ISP out-of-bounds rect");
      ngx_feature_set_uint(params, "Rect.X", left);
      ngx_feature_set_uint(params, "Rect.Y", top);
      ngx_feature_set_uint(params, "Rect.W", 48);
      ngx_feature_set_uint(params, "Rect.H", 40);
    }
    checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_success(), "ISP evaluate");
    resetAndDestroyParams(api, params);
    check(record.list->Close(), "ISP close");
    color.Reset();
    if (atlas) checkNgxResult(api.release(feature), ngx_fixture_result_success(), "ISP release before execute");
    gpu.executeAndWait(record.list.Get());
    const auto pixels = ngxPresentPixels(gpu, output.Get(), write);
    bool varied = false;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
      const size_t i = 4 * (size_t(y) * width + x);
      if (x >= left && x < left + outWidth && y >= top && y < top + outHeight)
        varied |= pixels[i] != pixels[(top * width + left) * 4];
      else if (std::memcmp(&pixels[i], &sentinel[i], 4))
        fail("ISP wrote outside viewport");
    }
    if (!varied) fail("ISP output contains no spatial detail");
    gpu.executeAndWait(record.list.Get());
    if (ngxPresentPixels(gpu, output.Get(), write) != pixels) fail("ISP replay differs");
    ngxPresentDump(input, atlas ? "isp-input-atlas.bin" : "isp-input-direct.bin");
    ngxPresentDump(pixels, atlas ? "isp-output-atlas.bin" : "isp-output-direct.bin");
  }
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_isp feature=9 direct=1 atlas=1 invalid_rect=1 release_before_execute=1 replay=1");
}

void runPublicFg(const NgxApi &api, Gpu &gpu) {
  constexpr UINT extent = ngxPresentExtent;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const std::vector<uint8_t> sentinel(extent * extent * 4, 19);
  auto feature = ngxPresentFeature(api, gpu);
  for (UINT frame = 0; frame < 9; ++frame) {
    auto depth = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const std::vector<float> depths(extent * extent, .75f);
    uploadTexture(gpu, depth.Get(), depths.data(), extent * 4, read);
    auto motion = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> vectors(extent * extent * 2, 0);
    for (size_t i = 0; i < vectors.size(); i += 2) vectors[i] = 0x4400; // Translating checkerboard: current-to-previous +4 pixels.
    uploadTexture(gpu, motion.Get(), vectors.data(), extent * 4, read);
    auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto pixels = ngxPresentScene(frame, false);
    uploadTexture(gpu, color.Get(), pixels.data(), extent * 4, read);
    auto output = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    uploadTexture(gpu, output.Get(), sentinel.data(), extent * 4, write);
    if (frame == 0) {
      auto params = allocateParams(api);
      ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
      ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
      ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
      ngx_feature_set_resource(params, "DLSSG.MVecs", motion.Get());
      ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", 1);
      auto rejected = gpu.makeRecording();
      for (UINT count = 2; count <= 4; ++count) {
        ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", count);
        checkNgxResult(api.evaluate(rejected.list.Get(), feature, params, nullptr), ngx_fixture_result_unsupported_parameter(), "FG native unsupported count");
        std::printf("OWNED_D3D12_NGX_REJECT case=count count=%u result=unsupported_parameter\n", count);
      }
      ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", 0);
      checkNgxResult(api.evaluate(rejected.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(), "FG zero count");
      ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", 1);
      ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", 0);
      checkNgxResult(api.evaluate(rejected.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(), "FG zero index");
      for (UINT index = 2; index <= 4; ++index) {
        ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", index);
        checkNgxResult(api.evaluate(rejected.list.Get(), feature, params, nullptr), ngx_fixture_result_unsupported_parameter(), "FG native unsupported index");
        std::printf("OWNED_D3D12_NGX_REJECT case=index index=%u result=unsupported_parameter\n", index);
      }
      ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", 1);
      auto plain = createBuffer(gpu.device.Get(), 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
      auto small = ngxPresentDisableBuffer(gpu, 3);
      Gpu foreign; auto foreignFlag = ngxPresentDisableBuffer(foreign);
      ID3D12Resource* invalidFlags[] = {output.Get(), plain.Get(), small.Get(), foreignFlag.Get()};
      for (auto invalid : invalidFlags) {
        ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", invalid);
        checkNgxResult(api.evaluate(rejected.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(), "FG invalid disable buffer");
      }
      resetAndDestroyParams(api, params);
      check(rejected.list->Close(), "FG rejected close"); gpu.executeAndWait(rejected.list.Get());
      if (ngxPresentPixels(gpu, output.Get(), write) != sentinel) fail("rejected FG modified output");
      std::puts("OWNED_D3D12_NGX_REJECT case=disable_buffer result=invalid_parameter texture=1 non_uav=1 too_small=1 cross_device=1 output_untouched=1 zero_count_index=invalid_parameter");
    }
    const bool reset = frame == 0 || frame == 5;
    const UINT frameId = frame < 7 ? frame : 9; // Gap, followed by repeated ID.
    const UINT flagBytes = frame & 1 ? 8 : 4;
    auto flag = ngxPresentDisableBuffer(gpu, flagBytes);
    auto flagUpload = createBuffer(gpu.device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto flagReadback = createBuffer(gpu.device.Get(), 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT* mappedFlag = nullptr;
    check(flagUpload->Map(0, nullptr, reinterpret_cast<void**>(&mappedFlag)), "FG flag upload map");
    mappedFlag[0] = 0x76543210; mappedFlag[1] = 0x12345678; flagUpload->Unmap(0, nullptr);
    auto initialize = gpu.makeRecording();
    transition(initialize.list.Get(), flag.Get(), write, D3D12_RESOURCE_STATE_COPY_DEST);
    initialize.list->CopyBufferRegion(flag.Get(), 0, flagUpload.Get(), 0, flagBytes);
    transition(initialize.list.Get(), flag.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    initialize.list->CopyBufferRegion(flagReadback.Get(), 0, flagUpload.Get(), 0, 8);
    initialize.list->CopyBufferRegion(flagReadback.Get(), 0, flag.Get(), 0, flagBytes);
    transition(initialize.list.Get(), flag.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
    check(initialize.list->Close(), "FG flag initialize close"); gpu.executeAndWait(initialize.list.Get());
    auto record = ngxPresentEvaluate(api, gpu, feature, frameId, reset, color.Get(), nullptr, output.Get(), depth.Get(), motion.Get(), false, frame == 6, flag.Get());
    auto beforeExecute = gpu.makeRecording();
    transition(beforeExecute.list.Get(), flag.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
    beforeExecute.list->CopyBufferRegion(flagReadback.Get(), 0, flag.Get(), 0, flagBytes);
    transition(beforeExecute.list.Get(), flag.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
    check(beforeExecute.list->Close(), "FG before execute close"); gpu.executeAndWait(beforeExecute.list.Get());
    check(flagReadback->Map(0, nullptr, reinterpret_cast<void**>(&mappedFlag)), "FG flag before execute map");
    if (mappedFlag[0] != 0x76543210 || mappedFlag[1] != 0x12345678) fail("FG flag changed before execute");
    flagReadback->Unmap(0, nullptr);
    transition(record.list.Get(), flag.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
    record.list->CopyBufferRegion(flagReadback.Get(), 0, flag.Get(), 0, flagBytes);
    transition(record.list.Get(), flag.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
    auto readback = createTextureReadback(gpu, output.Get());
    transition(record.list.Get(), output.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(record.list.Get(), output.Get(), readback);
    transition(record.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
    check(record.list->Close(), "FG lifetime close");
    color.Reset(); depth.Reset(); motion.Reset(); output.Reset(); flag.Reset();
    if (frame == 8) checkNgxResult(api.release(feature), ngx_fixture_result_success(), "FG release before execute");
    for (UINT replay = 0; replay <= UINT(frame == 0 || frame == 7 || frame == 8); ++replay) {
      gpu.executeAndWait(record.list.Get());
      check(flagReadback->Map(0, nullptr, reinterpret_cast<void**>(&mappedFlag)), "FG flag after fence map");
      const UINT disabled = *mappedFlag;
      if (mappedFlag[1] != 0x12345678) fail("FG flag wrote beyond uint32 hint");
      flagReadback->Unmap(0, nullptr);
      const UINT expectedDisabled = frame == 0 || frame >= 5 || replay != 0;
      if (disabled != expectedDisabled) fail("FG disable hint history contract");
      std::printf("OWNED_D3D12_NGX_DISABLE_HINT frame=%u replay=%u value=%u before_execute_untouched=1 after_fence=1 tail_untouched=1 history_hint=1\n", frame, replay, disabled);
      uint8_t* bytes = nullptr;
      check(readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&bytes)), "FG lifetime readback");
      std::vector<uint8_t> image(extent * extent * 4);
      for (UINT y = 0; y < extent; ++y) std::memcpy(image.data() + size_t(y) * extent * 4,
          bytes + readback.footprint.Offset + size_t(y) * readback.footprint.Footprint.RowPitch, extent * 4);
      readback.buffer->Unmap(0, nullptr);
      // Replayed bootstrap/reset can extend native warm-up; established history must interpolate.
      if (image == sentinel || ((frame == 3 || frame == 4) && !replay && image == pixels)) fail("FG native empty/current generated output");
      std::printf("OWNED_D3D12_NGX_NATIVE frame=%u reset=%u replay=%u output_written=1 different_current=%u resources_released=1 consumer=gpu_readback frame_id=%u not_rendering=%u\n", frame, UINT(reset), replay, UINT(image != pixels), frameId, UINT(frame == 6));
    }
  }
  checkNgxResult(api.release(feature), ngx_fixture_result_feature_not_found(), "FG stale handle");
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=1 unsupported_mfg=1 disable_buffer=gpu_history_hint reset=native_history release_before_execute=1 replay=native_encode");
}
void runPublicFgCameraMotion(const NgxApi &api, Gpu &gpu, bool orthographic = false) {
  constexpr UINT extent = ngxPresentExtent;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  NVSDK_NGX_Handle* features[3]{};
  uint64_t effects[3]{};
  for (auto& feature : features) feature = ngxPresentFeature(api, gpu, orthographic);
  auto depth = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<float> depths(extent * extent, .75f);
  if (orthographic) for (UINT y = 0; y != extent; ++y) for (UINT x = 0; x != extent; ++x)
    depths[y * extent + x] = .2f + .6f * float(x + y) / float(2 * (extent - 1));
  uploadTexture(gpu, depth.Get(), depths.data(), extent * 4, read);
  std::array<ComPtr<ID3D12Resource>, 2> motions;
  for (UINT mode = 0; mode != 2; ++mode) {
    motions[mode] = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> vectors(extent * extent * 2, 0);
    if (!mode) for (size_t i = 0; i < vectors.size(); i += 2) {
      vectors[i] = 0x4400; vectors[i + 1] = 0x4000; // +4,+2 pixels, both exactly representable.
    }
    uploadTexture(gpu, motions[mode].Get(), vectors.data(), extent * 4, read);
  }
  const std::vector<uint8_t> sentinel(extent * extent * 4, 19);
  UINT effectiveFrames = 0;
  for (UINT frame = 0; frame != 6; ++frame) {
    auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto scene = ngxPresentScene(frame, false);
    uploadTexture(gpu, color.Get(), scene.data(), extent * 4, read);
    std::array<std::vector<uint8_t>, 3> images;
    for (UINT mode = 0; mode != 3; ++mode) {
      auto output = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      uploadTexture(gpu, output.Get(), sentinel.data(), extent * 4, write);
      // Row-vector multiplication: last row supplies +4,+2 output-pixel motion.
      float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 8.f/extent,-4.f/extent,0,1};
      float viewToClip[16] = {.25f,0,0,0, 0,.25f,0,0, 0,0,-1.f/999.9f,0, 0,0,1000.f/999.9f,1};
      if (!frame && mode == 1) {
        auto real = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadTexture(gpu, real.Get(), sentinel.data(), extent * 4, write);
        auto params = allocateParams(api);
        ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
        ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
        ngx_feature_set_resource(params, "DLSSG.OutputReal", real.Get());
        ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
        ngx_feature_set_resource(params, "DLSSG.MVecs", motions[1].Get());
        ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
        ngx_feature_set_float(params, "DLSSG.CameraFar", 1000);
        ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
        ngx_feature_set_float(params, "FrameTimeDeltaInMsec", 1000.f / 30);
        ngx_feature_set_float(params, "DLSSG.MvecScaleX", 1.f / extent);
        ngx_feature_set_float(params, "DLSSG.MvecScaleY", 1.f / extent);
        ngx_feature_set_uint(params, "DLSSG.CameraMotionIncluded", 0);
        auto rejected = gpu.makeRecording();
        if (orthographic) {
          ngx_feature_set_uint(params, "DLSSG.CameraMotionIncluded", 1);
          ngx_feature_set_uint(params, "DLSSG.OrthoProjection", 1);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
              ngx_fixture_result_missing_input(), "FG ortho missing view-to-clip");
          ngx_feature_set_pointer(params, "DLSSG.CameraViewToClip", viewToClip);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
              ngx_fixture_result_missing_input(), "FG ortho missing basis");
          const char* keys[4][3] = {{"DLSSG.CameraPosX","DLSSG.CameraPosY","DLSSG.CameraPosZ"},
              {"DLSSG.CameraUpX","DLSSG.CameraUpY","DLSSG.CameraUpZ"},
              {"DLSSG.CameraRightX","DLSSG.CameraRightY","DLSSG.CameraRightZ"},
              {"DLSSG.CameraFwdX","DLSSG.CameraFwdY","DLSSG.CameraFwdZ"}};
          const float basis[4][3] = {{0,0,0},{0,1,0},{1,0,0},{0,0,1}};
          for (UINT vector = 0; vector != 4; ++vector) for (UINT axis = 0; axis != 3; ++axis)
            ngx_feature_set_float(params, keys[vector][axis], basis[vector][axis]);
          const float scale = viewToClip[0];
          for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.f}) {
            viewToClip[0] = invalid;
            checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
                ngx_fixture_result_invalid_parameter(), "FG invalid ortho view-to-clip");
          }
          viewToClip[0] = scale;
          ngx_feature_set_uint(params, "DLSSG.CameraMotionIncluded", 0);
        }
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_missing_input(), "FG missing clip-to-prev");
        ngx_feature_set_pointer(params, "DLSSG.ClipToPrevClip", matrix);
        matrix[12] = std::numeric_limits<float>::quiet_NaN();
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG NaN clip-to-prev");
        matrix[12] = 8.f/extent; matrix[5] = std::numeric_limits<float>::infinity();
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG infinite clip-to-prev");
        matrix[5] = 1;
        matrix[15] = 0;
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG singular clip-to-prev");
        matrix[15] = 1;
        resetAndDestroyParams(api, params);
        check(rejected.list->Close(), "FG camera rejects close"); gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu, output.Get(), write) != sentinel ||
            ngxPresentPixels(gpu, real.Get(), write) != sentinel) fail("FG rejected camera modified output");
        std::printf("OWNED_D3D12_NGX_REJECT case=camera_matrix ortho=%u missing=missing_input nan=invalid_parameter infinity=invalid_parameter singular=invalid_parameter output_untouched=1\n", UINT(orthographic));
        if (orthographic) std::puts("OWNED_D3D12_NGX_REJECT case=ortho_camera missing_matrix=missing_input missing_basis=missing_input nan=invalid_parameter infinity=invalid_parameter singular=invalid_parameter outputs_untouched=1");
      }
      armFgCapture();
      auto record = ngxPresentEvaluate(api, gpu, features[mode], frame, !frame, color.Get(), nullptr,
          output.Get(), depth.Get(), motions[mode ? 1 : 0].Get(), true, false, nullptr,
          mode == 1 ? matrix : nullptr, mode != 1, orthographic ? viewToClip : nullptr);
      std::fill_n(matrix, 16, std::numeric_limits<float>::quiet_NaN()); // Evaluate must snapshot caller memory.
      std::fill_n(viewToClip, 16, std::numeric_limits<float>::quiet_NaN());
      if (frame == 5) checkNgxResult(api.release(features[mode]), ngx_fixture_result_success(), "FG camera release before execute");
      gpu.executeAndWait(record.list.Get());
      images[mode] = ngxPresentPixels(gpu, output.Get(), write);
      if (images[mode] == sentinel) fail("FG camera output unwritten");
      checkFgObservation(FgProbeCase::cameraMotion,orthographic,frame,mode,scene,images[mode],effects);
    }
    int maxDelta = 0;
    for (size_t i = 0; i != images[0].size(); ++i)
      maxDelta = std::max(maxDelta, std::abs(int(images[0][i]) - int(images[1][i])));
    if (!fgCaptureEnabled && maxDelta > 1) fail("FG camera reconstruction differs from equivalent supplied motion");
    const bool effective = frame >= 2 && images[0] != images[2];
    effectiveFrames += UINT(effective);
    std::printf("OWNED_D3D12_NGX_CAMERA_MOTION ortho=%u frame=%u max_delta=%d different_zero=%u row_major=1 translated_last_row=1 matrix_snapshotted=1\n", UINT(orthographic), frame, maxDelta, UINT(effective));
  }
  if (!effectiveFrames) fail("FG camera comparison did not exercise nonzero effective motion");
  std::printf("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg_camera_motion ortho=%u frames=6 independent_features=3 equivalent_motion=1 nonzero_effect=1 release_before_execute=1\n", UINT(orthographic));
}
void runPublicFgCameraInputs(const NgxApi &api, Gpu &gpu, bool infiniteFar) {
  constexpr UINT extent = ngxPresentExtent;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  NVSDK_NGX_Handle* features[4]{};
  uint64_t effects[4]{};
  for (auto& feature : features) feature = ngxPresentFeature(api, gpu);
  auto depth = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<float> depths(extent * extent);
  for (UINT y = 0; y != extent; ++y) for (UINT x = 0; x != extent; ++x)
    depths[y * extent + x] = .2f + .6f * float(x + y) / float(2 * (extent - 1));
  uploadTexture(gpu, depth.Get(), depths.data(), extent * 4, read);
  std::array<ComPtr<ID3D12Resource>, 2> motions;
  for (UINT mode = 0; mode != 2; ++mode) {
    motions[mode] = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> vectors(extent * extent * 2, 0);
    if (!mode) for (size_t i = 0; i != vectors.size(); i += 2) {
      vectors[i] = 0x4400; vectors[i + 1] = 0x4000;
    }
    uploadTexture(gpu, motions[mode].Get(), vectors.data(), extent * 4, read);
  }
  const std::vector<uint8_t> sentinel(extent * extent * 4, 19);
  UINT effectiveFrames = 0, generatedFrames = 0;
  for (UINT frame = 0; frame != 6; ++frame) {
    const auto scene = ngxPresentScene(frame, false);
    auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    uploadTexture(gpu, color.Get(), scene.data(), extent * 4, read);
    std::array<std::vector<uint8_t>, 4> images;
    for (UINT mode = 0; mode != 4; ++mode) {
      auto output = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      auto real = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      uploadTexture(gpu, output.Get(), sentinel.data(), extent * 4, write);
      uploadTexture(gpu, real.Get(), sentinel.data(), extent * 4, write);
      auto params = allocateParams(api);
      ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
      ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
      ngx_feature_set_resource(params, "DLSSG.OutputReal", real.Get());
      ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
      ngx_feature_set_resource(params, "DLSSG.MVecs", motions[mode == 2].Get());
      ngx_feature_set_uint(params, "DLSSG.BackbufferFrameID", frame);
      ngx_feature_set_uint(params, "DLSSG.Reset", !frame);
      ngx_feature_set_float(params, "FrameTimeDeltaInMsec", 1000.f / 30);
      ngx_feature_set_float(params, "DLSSG.MvecScaleX", 1.f / extent);
      ngx_feature_set_float(params, "DLSSG.MvecScaleY", 1.f / extent);
      ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
      // A distant finite oracle avoids feeding +infinity into both comparison paths.
      const float farPlane = infiniteFar ? (mode == 1 ? std::numeric_limits<float>::infinity() : 1e8f) : 1000.f;
      ngx_feature_set_float(params, "DLSSG.CameraFar", farPlane);
      ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
      const char* keys[4][3] = {{"DLSSG.CameraPosX","DLSSG.CameraPosY","DLSSG.CameraPosZ"},
          {"DLSSG.CameraUpX","DLSSG.CameraUpY","DLSSG.CameraUpZ"},
          {"DLSSG.CameraRightX","DLSSG.CameraRightY","DLSSG.CameraRightZ"},
          {"DLSSG.CameraFwdX","DLSSG.CameraFwdY","DLSSG.CameraFwdZ"}};
      const float basis[4][3] = {{0,0,0},{0,1,0},{1,0,0},{0,0,1}};
      for (UINT vector = 0; vector != 4; ++vector) for (UINT axis = 0; axis != 3; ++axis)
        ngx_feature_set_float(params, keys[vector][axis], basis[vector][axis]);
      const float cot = 1.f / std::tan(.5f);
      float projection[16] = {cot / 2,0,0,0, 0,cot,0,0, 0,0,infiniteFar ? 0.f : -.1f / 999.9f,1,
          0,0,infiniteFar ? .1f : .1f * 1000.f / 999.9f,0};
      float inverseProjection[16] = {2 / cot,0,0,0, 0,1 / cot,0,0,
          0,0,0,1 / projection[14], 0,0,1,-projection[10] / projection[14]};
      float lens[16] = {2,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
      if (mode == 3) ngx_feature_set_pointer(params, "DLSSG.ClipToCameraView", inverseProjection);
      else if (mode != 1) ngx_feature_set_pointer(params, "DLSSG.CameraViewToClip", projection);
      ngx_feature_set_float(params, "DLSSG.CameraAspectRatio", 2);
      if (!frame && mode == 1) {
        auto rejected = gpu.makeRecording();
        for (float invalid : {0.f, -2.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
          ngx_feature_set_float(params, "DLSSG.CameraAspectRatio", invalid);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG invalid CameraAspectRatio");
        }
        ngx_feature_set_float(params, "DLSSG.CameraAspectRatio", 2);
        for (float invalid : {-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
          ngx_feature_set_float(params, "DLSSG.CameraFar", invalid);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG invalid CameraFar");
        }
        ngx_feature_set_float(params, "DLSSG.CameraFar", farPlane);
        ngx_feature_set_resource(params, "DLSSG.NoWarp", depth.Get());
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_unsupported_parameter(), "FG NoWarp has no native contract");
        ngx_feature_set_resource(params, "DLSSG.NoWarp", nullptr);
        ngx_feature_set_float(params, "DLSSG.CameraPinholeOffsetX", .25f);
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_unsupported_parameter(), "FG nonzero pinhole is not silently ignored");
        ngx_feature_set_float(params, "DLSSG.CameraPinholeOffsetX", 0);
        ngx_feature_set_pointer(params, "DLSSG.ClipToLensClip", lens);
        checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
            ngx_fixture_result_unsupported_parameter(), "FG lens transform is not silently ignored");
        lens[0] = 1;
        check(rejected.list->Close(), "FG camera inputs rejected close");
        gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu, output.Get(), write) != sentinel ||
            ngxPresentPixels(gpu, real.Get(), write) != sentinel) fail("FG invalid camera inputs modified outputs");
        std::printf("OWNED_D3D12_NGX_INPUT_REJECT case=camera infinite_far=%u aspect_zero_negative_nan_infinity=invalid_parameter far_negative_infinity_nan=invalid_parameter outputs_untouched=1\n", UINT(infiniteFar));
      }
      if (!frame && mode == 3) {
        auto rejected = gpu.makeRecording();
        const float first = inverseProjection[0];
        for (float invalid : {0.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
          inverseProjection[0] = invalid;
          checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG invalid inverse projection");
        }
        inverseProjection[0] = first;
        check(rejected.list->Close(), "FG inverse projection rejection close"); gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu, output.Get(), write) != sentinel) fail("FG invalid inverse projection modified output");
      }
      armFgCapture();
      auto record = gpu.makeRecording();
      checkNgxResult(api.evaluate(record.list.Get(), features[mode], params, nullptr),
          ngx_fixture_result_success(), "FG camera inputs evaluate");
      resetAndDestroyParams(api, params);
      std::fill_n(projection, 16, std::numeric_limits<float>::quiet_NaN());
      std::fill_n(inverseProjection, 16, std::numeric_limits<float>::quiet_NaN());
      // Keep the explicit forward-projection oracle alive through execution.
      if (frame == 5 && mode != 0) checkNgxResult(api.release(features[mode]), ngx_fixture_result_success(), "FG camera inputs release before execute");
      check(record.list->Close(), "FG camera inputs close"); gpu.executeAndWait(record.list.Get());
      if (frame == 5 && mode == 0) checkNgxResult(api.release(features[mode]), ngx_fixture_result_success(), "FG camera inputs release after execute");
      images[mode] = ngxPresentPixels(gpu, output.Get(), write);
      if (images[mode] == sentinel) fail("FG camera inputs output unwritten");
      checkFgObservation(FgProbeCase::cameraInputs,infiniteFar,frame,mode,scene,images[mode],effects);
    }
    int maxDelta = 0, inverseDelta = 0;
    for (size_t i = 0; i != images[0].size(); ++i)
      maxDelta = std::max(maxDelta, std::abs(int(images[0][i]) - int(images[1][i])));
    for (size_t i = 0; i != images[0].size(); ++i)
      inverseDelta = std::max(inverseDelta, std::abs(int(images[0][i]) - int(images[3][i])));
    if (!fgCaptureEnabled && inverseDelta > 1) fail("FG inverse projection differs from forward projection");
    if (!fgCaptureEnabled && maxDelta > 1) fail("FG camera inputs differ from equivalent explicit projection");
    effectiveFrames += UINT(frame >= 2 && images[0] != images[2]);
    generatedFrames += UINT(frame >= 2 && images[1] != scene);
  }
  if (!effectiveFrames || !generatedFrames) fail("FG camera inputs never generated nontrivial motion beyond warmup");
  std::printf("OWNED_D3D12_NGX_INPUT_PASS case=camera infinite_far=%u frames=6 independent_features=4 inverse_projection=1 invalid_untouched=1 native_boundaries=1 explicit_projection=1 complete_basis=1 aspect=2 generated_beyond_warmup=1 release_before_execute=1 snapshot=1\n", UINT(infiniteFar));
}
void runPublicFgMotionInputs(const NgxApi &api, Gpu &gpu, bool invalidSentinel) {
  constexpr UINT extent = ngxPresentExtent, render = extent / 2;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  NVSDK_NGX_Handle* features[5]{};
  uint64_t effects[5]{};
  for (auto& feature : features) feature = ngxPresentFeature(api, gpu, false, !invalidSentinel);
  const std::vector<uint8_t> sentinel(extent * extent * 4, 19);
  UINT effectiveFrames = 0, generatedFrames = 0;
  for (UINT frame = 0; frame != 6; ++frame) {
    const auto scene = ngxPresentScene(frame, false);
    auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    uploadTexture(gpu, color.Get(), scene.data(), extent * 4, read);
    std::array<std::vector<uint8_t>, 5> images;
    for (UINT mode = 0; mode != 5; ++mode) {
      const UINT depthExtent = invalidSentinel ? extent : render;
      const UINT motionExtent = invalidSentinel || mode == 1 || mode == 2 ? extent : render;
      const bool atlas = mode == 2 || mode == 4;
      const UINT dx = atlas ? 3 : 0, dy = atlas ? 5 : 0;
      const UINT mx = atlas ? 11 : 0, my = atlas ? 13 : 0;
      const UINT dw = depthExtent + (atlas ? 16 : 0), dh = depthExtent + (atlas ? 20 : 0);
      const UINT mw = motionExtent + (atlas ? 32 : 0), mh = motionExtent + (atlas ? 40 : 0);
      std::vector<float> depths(dw * dh, .01f);
      std::vector<uint16_t> vectors(mw * mh * 2, 0x4c00);
      for (UINT y = 0; y != depthExtent; ++y) for (UINT x = 0; x != depthExtent; ++x)
        depths[(y + dy) * dw + x + dx] = .75f;
      for (UINT y = 0; y != motionExtent; ++y) for (UINT x = 0; x != motionExtent; ++x) {
        const UINT i = ((y + my) * mw + x + mx) * 2;
        // -32 is a finite invalid marker, never valid object displacement.
        vectors[i] = mode == 3 ? 0 : invalidSentinel && (mode == 1 || mode == 2) ? (x % 2 ? 0x4000 : 0xd000) :
            mode == 4 ? (motionExtent == extent ? 0x4800 : 0x4400) : motionExtent == extent ? 0x4400 : 0x4000;
        vectors[i + 1] = mode == 3 ? 0 : invalidSentinel && (mode == 1 || mode == 2) ? (x % 2 ? 0xd000 : 0x4000) :
            mode == 4 ? (motionExtent == extent ? 0x4400 : 0x4000) : motionExtent == extent ? 0x4000 : 0x3c00;
      }
      auto depth = createTexture(gpu.device.Get(), dw, dh, DXGI_FORMAT_R32_FLOAT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      auto motion = createTexture(gpu.device.Get(), mw, mh, DXGI_FORMAT_R16G16_FLOAT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      const UINT ow = extent + (atlas ? 24 : 0), oh = extent + (atlas ? 16 : 0);
      const UINT ox = atlas ? 7 : 0, oy = atlas ? 5 : 0;
      const std::vector<uint8_t> outputSentinel(ow * oh * 4, 19);
      auto output = createTexture(gpu.device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      const UINT rw = extent + 32, rh = extent + 24, rx = 9, ry = 7;
      const std::vector<uint8_t> realSentinel(rw * rh * 4, 19);
      auto real = createTexture(gpu.device.Get(), rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      uploadTexture(gpu, depth.Get(), depths.data(), dw * 4, read);
      uploadTexture(gpu, motion.Get(), vectors.data(), mw * 4, read);
      uploadTexture(gpu, output.Get(), outputSentinel.data(), ow * 4, write);
      uploadTexture(gpu, real.Get(), realSentinel.data(), rw * 4, write);
      auto params = allocateParams(api);
      ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
      ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
      if (atlas) {
        ngx_feature_set_uint(params, "DLSSG.EvalFlags", 1);
        ngx_feature_set_uint(params, "DLSSG.OutputInterpolatedSubrectBaseX", ox);
        ngx_feature_set_uint(params, "DLSSG.OutputInterpolatedSubrectBaseY", oy);
        ngx_feature_set_resource(params, "DLSSG.OutputReal", real.Get());
        ngx_feature_set_uint(params, "DLSSG.OutputRealSubrectBaseX", rx);
        ngx_feature_set_uint(params, "DLSSG.OutputRealSubrectBaseY", ry);
      }
      ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
      ngx_feature_set_resource(params, "DLSSG.MVecs", motion.Get());
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectBaseX", dx);
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectBaseY", dy);
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectWidth", depthExtent);
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectHeight", depthExtent);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectBaseX", mx);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectBaseY", my);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectWidth", motionExtent);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectHeight", motionExtent);
      ngx_feature_set_uint(params, "DLSSG.BackbufferFrameID", frame);
      ngx_feature_set_uint(params, "DLSSG.Reset", !frame);
      ngx_feature_set_float(params, "FrameTimeDeltaInMsec", 1000.f / 30);
      // Mode 4 doubles the encoded displacement and halves its scale on the
      // same render grid; its independent padded origins must not change output.
      ngx_feature_set_float(params, "DLSSG.MvecScaleX", (mode == 4 ? .5f : 1.f) / motionExtent);
      ngx_feature_set_float(params, "DLSSG.MvecScaleY", (mode == 4 ? .5f : 1.f) / motionExtent);
      ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
      ngx_feature_set_float(params, "DLSSG.CameraFar", 1000);
      ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
      if (!invalidSentinel) {
        ngx_feature_set_float(params, "DLSSG.JitterOffsetX", float(frame % 2) / render);
        ngx_feature_set_float(params, "DLSSG.JitterOffsetY", -float(frame % 2) / render);
      }
      float clipToPrev[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 8.f/extent,-4.f/extent,0,1};
      if (mode == 2) { clipToPrev[12] *= -1; clipToPrev[13] *= -1; }
      if (invalidSentinel && (mode == 1 || mode == 2)) {
        ngx_feature_set_float(params, "DLSSG.MvecInvalidValue", -32);
        ngx_feature_set_uint(params, "DLSSG.CameraMotionIncluded", 0);
        ngx_feature_set_pointer(params, mode == 2 ? "DLSSG.PrevClipToClip" : "DLSSG.ClipToPrevClip", clipToPrev);
      }
      if (!frame && invalidSentinel && mode == 2) {
        auto rejected = gpu.makeRecording();
        for (float invalid : {0.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
          clipToPrev[0] = invalid;
          checkNgxResult(api.evaluate(rejected.list.Get(), features[mode], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG invalid inverse camera motion");
        }
        clipToPrev[0] = 1;
        check(rejected.list->Close(), "FG inverse camera motion rejected close"); gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu, output.Get(), write) != outputSentinel) fail("FG inverse camera motion modified output");
      }
      armFgCapture();
      auto record = gpu.makeRecording();
      checkNgxResult(api.evaluate(record.list.Get(), features[mode], params, nullptr),
          ngx_fixture_result_success(), "FG motion inputs evaluate");
      resetAndDestroyParams(api, params);
      std::fill_n(clipToPrev, 16, std::numeric_limits<float>::quiet_NaN());
      if (frame == 5) checkNgxResult(api.release(features[mode]), ngx_fixture_result_success(), "FG motion inputs release before execute");
      check(record.list->Close(), "FG motion inputs close"); gpu.executeAndWait(record.list.Get());
      const auto backing = ngxPresentPixels(gpu, output.Get(), write);
      images[mode].resize(extent * extent * 4);
      for (UINT y = 0; y != oh; ++y) for (UINT x = 0; x != ow; ++x) for (UINT c = 0; c != 4; ++c) {
        const auto value = backing[(y * ow + x) * 4 + c];
        if (x >= ox && x < ox + extent && y >= oy && y < oy + extent)
          images[mode][((y - oy) * extent + x - ox) * 4 + c] = value;
        else if (value != 19) fail("FG changed output outside independent subrect");
      }
      const auto realPixels = ngxPresentPixels(gpu, real.Get(), write);
      for (UINT y = 0; y != rh; ++y) for (UINT x = 0; x != rw; ++x) for (UINT c = 0; c != 4; ++c) {
        const auto expected = atlas && x >= rx && x < rx + extent && y >= ry && y < ry + extent
            ? scene[((y - ry) * extent + x - rx) * 4 + c] : 19;
        if (realPixels[(y * rw + x) * 4 + c] != expected) fail("FG independent real-output crop or sentinel");
      }
      if (images[mode] == sentinel) fail("FG motion inputs output unwritten");
      checkFgObservation(FgProbeCase::motionInputs,invalidSentinel,frame,mode,scene,images[mode],effects);
    }
    // Different guide resolutions legitimately change MetalFX interpolation.
    // Compare equivalent encodings within each grid, not render vs display:
    // 0/4 covers render-grid scale and padding; 1/2 covers mixed-grid/inverse motion.
    for (const auto pair : {std::pair{0u, 4u}, std::pair{1u, 2u}}) {
      int maxDelta = 0;
      for (size_t i = 0; i != images[pair.first].size(); ++i)
        maxDelta = std::max(maxDelta, std::abs(int(images[pair.first][i]) - int(images[pair.second][i])));
      if (!fgCaptureEnabled && maxDelta > 1) {
        std::fprintf(stderr, "FG motion comparison frame=%u invalid_sentinel=%u modes=%u/%u max_delta=%d\n",
            frame, UINT(invalidSentinel), pair.first, pair.second, maxDelta);
        fail("FG motion inputs differ from equivalent normalized motion");
      }
    }
    // Sentinel cases share one grid: camera-reconstructed motion must also
    // match the independently supplied valid displacement, not only its inverse.
    if (invalidSentinel) {
      int maxDelta = 0;
      for (size_t i = 0; i != images[0].size(); ++i)
        maxDelta = std::max(maxDelta, std::abs(int(images[0][i]) - int(images[1][i])));
      if (!fgCaptureEnabled && maxDelta > 1) fail("FG invalid motion reconstruction differs from valid displacement");
    }
    effectiveFrames += UINT(frame >= 2 && images[0] != images[3]);
    generatedFrames += UINT(frame >= 2 && images[1] != scene);
  }
  if (!effectiveFrames || !generatedFrames) fail("FG motion inputs never generated nontrivial motion beyond warmup");
  std::printf("OWNED_D3D12_NGX_INPUT_PASS case=motion invalid_sentinel=%u frames=6 independent_features=5 equivalent_motion=1 poisoned_padding=1 output_real_rects=1 jitter_or_inverse=1 generated_beyond_warmup=1 release_before_execute=1 snapshot=1\n", UINT(invalidSentinel));
}
void runPublicFgUi(const NgxApi &api, Gpu &gpu) {
  constexpr UINT extent = ngxPresentExtent, atlas = extent + 8, uiX = 3, uiY = 5;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  NVSDK_NGX_Handle* features[3]{};
  for (UINT mode = 0; mode != 3; ++mode) {
    auto params = allocateParams(api);
    ngx_feature_set_uint(params,"DLSSG.Width",extent);
    ngx_feature_set_uint(params,"DLSSG.Height",extent);
    ngx_feature_set_uint(params,"DLSSG.InternalWidth",extent);
    ngx_feature_set_uint(params,"DLSSG.InternalHeight",extent);
    ngx_feature_set_uint(params,"DLSSG.BackbufferFormat",DXGI_FORMAT_R8G8B8A8_UNORM);
    ngx_feature_set_uint(params,"DLSSG.DepthInverted",1);
    auto record = gpu.makeRecording();
    if (!mode) {
      ngx_feature_set_uint(params,"DLSSG.UserInterfaceRecompositionEnabled",2);
      checkNgxResult(api.create(record.list.Get(),11,params,&features[mode]),ngx_fixture_result_invalid_parameter(),"FG invalid UIR boolean");
      if (features[mode]) fail("invalid UIR created feature");
    }
    ngx_feature_set_uint(params,"DLSSG.UserInterfaceRecompositionEnabled",mode != 2);
    checkNgxResult(api.create(record.list.Get(),11,params,&features[mode]),ngx_fixture_result_success(),"FG UI create");
    check(record.list->Close(),"FG UI create close"); resetAndDestroyParams(api,params);
  }
  for (UINT frame = 0; frame != 5; ++frame) {
    auto scene = ngxPresentScene(frame,false), composite = scene;
    std::vector<float> overlay(atlas * atlas * 4,1.f), alpha(atlas * atlas,1.f);
    for (UINT y = 0; y != extent; ++y) for (UINT x = 0; x != extent; ++x) {
      const float a = x < 32 ? 1.f : x < 64 ? .5f : 0.f;
      const UINT i = (y * extent + x) * 4, u = ((y + uiY) * atlas + x + uiX) * 4;
      alpha[u / 4] = a; overlay[u + 3] = a;
      for (UINT c = 0; c != 3; ++c) {
        overlay[u + c] = a * (.5f / (1u << c));
        composite[i + c] = uint8_t(std::lround(255.f * overlay[u + c] + (1.f - a) * scene[i + c]));
      }
    }
    auto depth = createTexture(gpu.device.Get(),extent,extent,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto motion = createTexture(gpu.device.Get(),extent,extent,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto hudless = createTexture(gpu.device.Get(),extent,extent,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto color = createTexture(gpu.device.Get(),extent,extent,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto ui = createTexture(gpu.device.Get(),atlas,atlas,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto uiAlpha = createTexture(gpu.device.Get(),atlas,atlas,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> depths(extent * extent,.75f);
    std::vector<uint16_t> vectors(extent * extent * 2,0);
    for (size_t i = 0; i < vectors.size(); i += 2) vectors[i] = 0x4400;
    uploadTexture(gpu,depth.Get(),depths.data(),extent * 4,read);
    uploadTexture(gpu,motion.Get(),vectors.data(),extent * 4,read);
    uploadTexture(gpu,hudless.Get(),scene.data(),extent * 4,read);
    uploadTexture(gpu,color.Get(),composite.data(),extent * 4,read);
    uploadTexture(gpu,ui.Get(),overlay.data(),atlas * 16,read);
    uploadTexture(gpu,uiAlpha.Get(),alpha.data(),atlas * 4,read);
    std::array<Recording,3> records;
    std::array<TextureReadback,3> readbacks;
    for (UINT mode = 0; mode != 3; ++mode) {
      auto output = createTexture(gpu.device.Get(),extent,extent,DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
      const std::vector<uint8_t> sentinel(extent * extent * 4,19);
      uploadTexture(gpu,output.Get(),sentinel.data(),extent * 4,write);
      auto params = allocateParams(api);
      ngx_feature_set_resource(params,"DLSSG.Backbuffer",mode == 2 ? hudless.Get() : color.Get());
      ngx_feature_set_resource(params,"DLSSG.HUDLess",mode == 2 ? nullptr : hudless.Get());
      ngx_feature_set_resource(params,"DLSSG.UI",mode == 0 ? ui.Get() : nullptr);
      ngx_feature_set_resource(params,"DLSSG.UIAlpha",mode == 1 ? uiAlpha.Get() : nullptr);
      ngx_feature_set_resource(params,"DLSSG.OutputInterpolated",output.Get());
      ngx_feature_set_resource(params,"DLSSG.Depth",depth.Get());
      ngx_feature_set_resource(params,"DLSSG.MVecs",motion.Get());
      ngx_feature_set_uint(params,"DLSSG.UISubrectBaseX",uiX); ngx_feature_set_uint(params,"DLSSG.UISubrectBaseY",uiY);
      ngx_feature_set_uint(params,"DLSSG.UIAlphaSubrectBaseX",uiX); ngx_feature_set_uint(params,"DLSSG.UIAlphaSubrectBaseY",uiY);
      ngx_feature_set_uint(params,"DLSSG.BackbufferFrameID",frame);
      ngx_feature_set_uint(params,"DLSSG.Reset",frame == 0);
      ngx_feature_set_float(params,"FrameTimeDeltaInMsec",1000.f / 30);
      ngx_feature_set_float(params,"DLSSG.MvecScaleX",1.f / extent);
      ngx_feature_set_float(params,"DLSSG.MvecScaleY",1.f / extent);
      ngx_feature_set_float(params,"DLSSG.CameraNear",.1f);
      ngx_feature_set_float(params,"DLSSG.CameraFar",1000);
      ngx_feature_set_float(params,"DLSSG.CameraFOV",1);
      records[mode] = gpu.makeRecording();
      if (!frame && !mode) {
        auto rejected = gpu.makeRecording();
        ngx_feature_set_resource(params,"DLSSG.HUDLess",nullptr);
        checkNgxResult(api.evaluate(rejected.list.Get(),features[mode],params,nullptr),ngx_fixture_result_missing_input(),"FG UI missing HUDLess");
        ngx_feature_set_resource(params,"DLSSG.HUDLess",hudless.Get());
        ngx_feature_set_resource(params,"DLSSG.UI",nullptr);
        checkNgxResult(api.evaluate(rejected.list.Get(),features[mode],params,nullptr),ngx_fixture_result_missing_input(),"FG UIR missing UI");
        ngx_feature_set_resource(params,"DLSSG.UI",ui.Get());
        ngx_feature_set_uint(params,"DLSSG.UISubrectBaseX",9);
        checkNgxResult(api.evaluate(rejected.list.Get(),features[mode],params,nullptr),ngx_fixture_result_invalid_parameter(),"FG UI invalid origin");
        ngx_feature_set_uint(params,"DLSSG.UISubrectBaseX",uiX);
        ngx_feature_set_resource(params,"DLSSG.UIAlpha",uiAlpha.Get());
        checkNgxResult(api.evaluate(rejected.list.Get(),features[mode],params,nullptr),ngx_fixture_result_unsupported_parameter(),"FG ambiguous dual alpha");
        ngx_feature_set_resource(params,"DLSSG.UIAlpha",nullptr);
        ngx_feature_set_resource(params,"DLSSG.UI",output.Get());
        checkNgxResult(api.evaluate(rejected.list.Get(),features[mode],params,nullptr),ngx_fixture_result_invalid_parameter(),"FG UI output alias");
        ngx_feature_set_resource(params,"DLSSG.UI",ui.Get());
        check(rejected.list->Close(),"FG rejected UI close"); gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu,output.Get(),write) != sentinel) fail("rejected UI modified output");
      }
      if (!frame && mode == 1) {
        ngx_feature_set_uint(params,"DLSSG.UIAlphaSubrectBaseY",9);
        checkNgxResult(api.evaluate(records[mode].list.Get(),features[mode],params,nullptr),ngx_fixture_result_invalid_parameter(),"FG UIAlpha invalid origin");
        ngx_feature_set_uint(params,"DLSSG.UIAlphaSubrectBaseY",uiY);
        ngx_feature_set_resource(params,"DLSSG.UIAlpha",ui.Get());
        checkNgxResult(api.evaluate(records[mode].list.Get(),features[mode],params,nullptr),ngx_fixture_result_unsupported_parameter(),"FG UIAlpha invalid format");
        ngx_feature_set_resource(params,"DLSSG.UIAlpha",uiAlpha.Get());
      }
      checkNgxResult(api.evaluate(records[mode].list.Get(),features[mode],params,nullptr),ngx_fixture_result_success(),"FG UI evaluate");
      resetAndDestroyParams(api,params);
      readbacks[mode] = createTextureReadback(gpu,output.Get());
      transition(records[mode].list.Get(),output.Get(),write,D3D12_RESOURCE_STATE_COPY_SOURCE);
      copyTextureToReadback(records[mode].list.Get(),output.Get(),readbacks[mode]);
      transition(records[mode].list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,write);
      check(records[mode].list->Close(),"FG UI close");
    }
    depth.Reset(); motion.Reset(); hudless.Reset(); color.Reset(); ui.Reset(); uiAlpha.Reset();
    if (frame == 4) for (auto feature : features) checkNgxResult(api.release(feature),ngx_fixture_result_success(),"FG UI release before execute");
    for (UINT replay = 0; replay <= UINT(frame == 4); ++replay) {
      std::array<std::vector<uint8_t>,3> images;
      for (UINT mode = 0; mode != 3; ++mode) {
        gpu.executeAndWait(records[mode].list.Get());
        uint8_t* bytes = nullptr;
        check(readbacks[mode].buffer->Map(0,nullptr,reinterpret_cast<void**>(&bytes)),"FG UI map");
        images[mode].resize(extent * extent * 4);
        for (UINT y = 0; y != extent; ++y) std::memcpy(images[mode].data() + y * extent * 4,
            bytes + readbacks[mode].footprint.Offset + y * readbacks[mode].footprint.Footprint.RowPitch,extent * 4);
        readbacks[mode].buffer->Unmap(0,nullptr);
      }
      if (frame == 3 && images[2] == scene) fail("FG UI control did not interpolate");
      for (UINT y = 0; y != extent; ++y) for (UINT x = 0; x != extent; ++x) for (UINT c = 0; c != 3; ++c) {
        const UINT i = (y * extent + x) * 4 + c, u = ((y + uiY) * atlas + x + uiX) * 4;
        const float a = alpha[u / 4];
        const int expected = int(std::lround(255.f * overlay[u + c] + (1.f - a) * images[2][i]));
        for (UINT mode = 0; mode != 2; ++mode)
          if (std::abs(int(images[mode][i]) - expected) > 2) fail("FG premultiplied UI/UIAlpha composition");
      }
    }
  }
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg_ui premultiplied=1 alpha_only=1 independent_ui_origin=1 invalid_inputs=1 release_before_execute=1 replay=1");
}
void runPublicFgSubrect(const NgxApi &api, Gpu &gpu) {
  constexpr UINT extent = ngxPresentExtent;
  constexpr UINT render = 96;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  NVSDK_NGX_Handle* features[8]{};
  for (auto& feature : features) feature = ngxPresentFeature(api, gpu);
  unsigned distortionFrames = 0;
  for (UINT frame = 0; frame != 8; ++frame) {
    std::vector<uint8_t> reference, floatReference, precisionReference, control;
    for (UINT variant = 0; variant != 8; ++variant) {
      const UINT atlas = variant == 4 || variant == 5 || variant == 7 ? 2 : variant == 2 ? 1 : 0;
      const bool fieldEnabled = variant != 0 && frame != 6;
      const UINT colorWidth = extent + (atlas == 2 ? 32 : 0), colorHeight = extent + (atlas == 2 ? 40 : 0);
      const UINT left = atlas == 2 ? 11 : 0, top = atlas == 2 ? 13 : 0;
      const UINT hx = atlas == 2 ? 3 : left, hy = atlas == 2 ? 5 : top;
      const UINT ox = atlas == 2 ? 7 : left, oy = atlas == 2 ? 9 : top;
      const UINT rx = atlas == 2 ? 17 : left, ry = atlas == 2 ? 19 : top;
      const std::vector<uint8_t> sentinel(colorWidth * colorHeight * 4, 19);
      const UINT depthWidth = render + atlas * 8, depthHeight = render + atlas * 12;
      const UINT motionWidth = render + atlas * 16, motionHeight = render + atlas * 20;
      const UINT dx = atlas * 3, dy = atlas * 5, mx = atlas * 7, my = atlas * 9;
      // Padding differs from every active texel, so ignoring either origin changes native inputs.
      std::vector<float> depths(depthWidth * depthHeight, .01f);
      std::vector<uint16_t> vectors(motionWidth * motionHeight * 2, 0x4c00);
      for (UINT y = 0; y != render; ++y) for (UINT x = 0; x != render; ++x) {
        depths[(y + dy) * depthWidth + x + dx] = x > 40 && x < 88 && y > 32 && y < 96 ? .9f : .75f;
        vectors[((y + my) * motionWidth + x + mx) * 2] = 0x4200;
        vectors[((y + my) * motionWidth + x + mx) * 2 + 1] = 0;
      }
      auto depth = createTexture(gpu.device.Get(), depthWidth, depthHeight, DXGI_FORMAT_R32_FLOAT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      auto motion = createTexture(gpu.device.Get(), motionWidth, motionHeight, DXGI_FORMAT_R16G16_FLOAT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      auto color = createTexture(gpu.device.Get(), colorWidth, colorHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      auto output = createTexture(gpu.device.Get(), colorWidth, colorHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      auto real = createTexture(gpu.device.Get(), colorWidth, colorHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      auto hudless = createTexture(gpu.device.Get(), colorWidth, colorHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      constexpr UINT fieldExtent = 64;
      const UINT fieldWidth = fieldExtent + atlas * 8, fieldHeight = fieldExtent + atlas * 12;
      const UINT fieldX = atlas * 3, fieldY = atlas * 5;
      const bool float32 = variant >= 3;
      const bool lowPrecision = variant == 5 || variant == 7;
      auto field = createTexture(gpu.device.Get(), fieldWidth, fieldHeight,
          variant == 7 ? DXGI_FORMAT_R8G8B8A8_UNORM :
          float32 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      std::vector<uint16_t> fieldHalf(fieldWidth * fieldHeight * 4, 0x3600);
      std::vector<float> fieldFloat(fieldWidth * fieldHeight * 4, .375f);
      std::vector<uint8_t> fieldBytes(fieldWidth * fieldHeight * 4, 255);
      for (UINT y = 0; y != fieldExtent; ++y) for (UINT x = 0; x != fieldExtent; ++x) {
        const float u = (x + .5f) / fieldExtent - .5f;
        // Centered 1.25x horizontal map and its inverse, in normalized UV units.
        const float offsets[] = {.25f * u, 0, -.2f * u, 0};
        for (UINT c = 0; c != 4; ++c) {
          const _Float16 value = static_cast<_Float16>(offsets[c]);
          const UINT index = ((y + fieldY) * fieldWidth + x + fieldX) * 4 + c;
          std::memcpy(&fieldHalf[index], &value, sizeof(value));
          fieldFloat[index] = static_cast<float>(value);
          if (variant == 5) fieldFloat[index] = (fieldFloat[index] + 1.f) * 2.f;
          if (variant >= 6) {
            fieldBytes[index] = uint8_t(64 + (x * 3 + c * 37) % 127);
            fieldFloat[index] = float(fieldBytes[index]) / 255.f * .25f - .125f;
          }
        }
      }
      uploadTexture(gpu, field.Get(), variant == 7 ? static_cast<const void*>(fieldBytes.data()) :
          float32 ? static_cast<const void*>(fieldFloat.data()) : fieldHalf.data(),
          fieldWidth * (variant == 7 ? 4 : float32 ? 16 : 8), read);
      const auto scene = ngxPresentScene(frame, false);
      std::vector<uint8_t> pixels(colorWidth * colorHeight * 4, 203);
      for (UINT y = 0; y != extent; ++y)
        std::memcpy(pixels.data() + ((y + hy) * colorWidth + hx) * 4, scene.data() + y * extent * 4, extent * 4);
      uploadTexture(gpu, hudless.Get(), pixels.data(), colorWidth * 4, read);
      std::fill(pixels.begin(), pixels.end(), 203);
      const auto composited = ngxPresentScene(frame, true);
      for (UINT y = 0; y != extent; ++y)
        std::memcpy(pixels.data() + ((y + top) * colorWidth + left) * 4, composited.data() + y * extent * 4, extent * 4);
      uploadTexture(gpu, depth.Get(), depths.data(), depthWidth * 4, read);
      uploadTexture(gpu, motion.Get(), vectors.data(), motionWidth * 4, read);
      uploadTexture(gpu, color.Get(), pixels.data(), colorWidth * 4, read);
      uploadTexture(gpu, output.Get(), sentinel.data(), colorWidth * 4, write);
      uploadTexture(gpu, real.Get(), sentinel.data(), colorWidth * 4, write);
      const bool realEnabled = frame % 2 == 0 || frame == 7;
      const bool rectOnly = atlas == 2 && frame >= 4;
      auto params = allocateParams(api);
      ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
      ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
      ngx_feature_set_resource(params, "DLSSG.OutputReal", realEnabled ? real.Get() : nullptr);
      ngx_feature_set_resource(params, "DLSSG.HUDLess", hudless.Get());
      ngx_feature_set_uint(params, "DLSSG.EvalFlags", rectOnly);
      ngx_feature_set_resource(params, "DLSSG.BidirectionalDistortionField", fieldEnabled ? field.Get() : nullptr);
      ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldSubrectBaseX", fieldX);
      ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldSubrectBaseY", fieldY);
      ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldSubrectWidth", fieldExtent);
      ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldSubrectHeight", fieldExtent);
      ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision", lowPrecision);
      ngx_feature_set_float(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.Scale", variant == 7 ? .25f : .5f);
      ngx_feature_set_float(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.Bias", variant == 7 ? -.125f : -1.f);
      ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
      ngx_feature_set_resource(params, "DLSSG.MVecs", motion.Get());
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectBaseX", dx);
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectBaseY", dy);
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectWidth", render);
      ngx_feature_set_uint(params, "DLSSG.DepthSubrectHeight", render);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectWidth", render);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectHeight", render);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectBaseX", mx);
      ngx_feature_set_uint(params, "DLSSG.MVecsSubrectBaseY", my);
      ngx_feature_set_uint(params, "DLSSG.BackbufferSubrectBaseX", left);
      ngx_feature_set_uint(params, "DLSSG.BackbufferSubrectBaseY", top);
      if (atlas == 2) {
        // Explicit resource origins override the deprecated shared origin.
        ngx_feature_set_uint(params, "DLSSG.InputBackbufferSubrectBaseX", left);
        ngx_feature_set_uint(params, "DLSSG.InputBackbufferSubrectBaseY", top);
        ngx_feature_set_uint(params, "DLSSG.HUDLessSubrectBaseX", hx);
        ngx_feature_set_uint(params, "DLSSG.HUDLessSubrectBaseY", hy);
        ngx_feature_set_uint(params, "DLSSG.OutputInterpolatedSubrectBaseX", ox);
        ngx_feature_set_uint(params, "DLSSG.OutputInterpolatedSubrectBaseY", oy);
        ngx_feature_set_uint(params, "DLSSG.OutputRealSubrectBaseX", rx);
        ngx_feature_set_uint(params, "DLSSG.OutputRealSubrectBaseY", ry);
      }
      ngx_feature_set_uint(params, "DLSSG.BackbufferFrameID", frame);
      ngx_feature_set_uint(params, "DLSSG.Reset", frame == 0 || frame == 5);
      ngx_feature_set_float(params, "FrameTimeDeltaInMsec", 1000.f / 30);
      ngx_feature_set_float(params, "DLSSG.MvecScaleX", 1.f / render);
      ngx_feature_set_float(params, "DLSSG.MvecScaleY", 1.f / render);
      ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
      ngx_feature_set_float(params, "DLSSG.CameraFar", 1000);
      ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
      if (atlas == 1 && frame == 0) {
        auto rejected = gpu.makeRecording();
        for (const auto& invalid : {std::pair{"DLSSG.DepthSubrectBaseX", depthWidth - render + 1},
             std::pair{"DLSSG.DepthSubrectBaseY", UINT_MAX},
             std::pair{"DLSSG.MVecsSubrectBaseX", UINT_MAX},
             std::pair{"DLSSG.MVecsSubrectBaseY", motionHeight - render + 1},
             std::pair{"DLSSG.BackbufferSubrectBaseX", UINT_MAX},
             std::pair{"DLSSG.BackbufferSubrectBaseY", 1u}}) {
          ngx_feature_set_uint(params, invalid.first, invalid.second);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG subrect overflow rejected");
          ngx_feature_set_uint(params, "DLSSG.DepthSubrectBaseX", dx);
          ngx_feature_set_uint(params, "DLSSG.DepthSubrectBaseY", dy);
          ngx_feature_set_uint(params, "DLSSG.MVecsSubrectBaseX", mx);
          ngx_feature_set_uint(params, "DLSSG.MVecsSubrectBaseY", my);
          ngx_feature_set_uint(params, "DLSSG.BackbufferSubrectBaseX", left);
          ngx_feature_set_uint(params, "DLSSG.BackbufferSubrectBaseY", top);
        }
        for (auto alias : {color.Get(), output.Get(), hudless.Get(), depth.Get(), motion.Get()}) {
          ngx_feature_set_resource(params, "DLSSG.OutputReal", alias);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG real-output alias rejected");
        }
        ngx_feature_set_resource(params, "DLSSG.OutputReal", real.Get());
        ngx_feature_set_uint(params, "DLSSG.EvalFlags", 2);
        checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
            ngx_fixture_result_unsupported_parameter(), "FG unknown evaluate flags rejected");
        ngx_feature_set_uint(params, "DLSSG.EvalFlags", 0);
        ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldSubrectBaseX", UINT_MAX);
        checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG distortion overflow rejected");
        ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldSubrectBaseX", fieldX);
        ngx_feature_set_resource(params, "DLSSG.BidirectionalDistortionField", output.Get());
        checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG distortion/output alias rejected");
        ngx_feature_set_resource(params, "DLSSG.BidirectionalDistortionField", field.Get());
        ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision", 2);
        checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG invalid precision flag rejected");
        ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision", 0);
        ngx_feature_set_resource(params, "DLSSG.BidirectionalDistortionField", motion.Get());
        checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
            ngx_fixture_result_unsupported_parameter(), "FG two-channel field rejected");
        ngx_feature_set_resource(params, "DLSSG.BidirectionalDistortionField", field.Get());
        check(rejected.list->Close(), "FG subrect rejected close");
        gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu, output.Get(), write) != sentinel) fail("FG invalid subrect modified output");
        if (ngxPresentPixels(gpu, real.Get(), write) != sentinel) fail("FG invalid input modified real output");
      }
      if (variant == 4 && frame == 0) {
        auto rejected = gpu.makeRecording();
        for (const auto& origin : {std::pair{"DLSSG.InputBackbufferSubrectBaseX", left},
             std::pair{"DLSSG.HUDLessSubrectBaseY", hy},
             std::pair{"DLSSG.OutputInterpolatedSubrectBaseX", ox},
             std::pair{"DLSSG.OutputRealSubrectBaseY", ry}}) {
          ngx_feature_set_uint(params, origin.first, UINT_MAX);
          checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
              ngx_fixture_result_invalid_parameter(), "FG independent color origin overflow");
          ngx_feature_set_uint(params, origin.first, origin.second);
        }
        ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision", 1);
        ngx_feature_set_float(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.Scale", INFINITY);
        checkNgxResult(api.evaluate(rejected.list.Get(), features[variant], params, nullptr),
            ngx_fixture_result_invalid_parameter(), "FG nonfinite precision scale");
        ngx_feature_set_uint(params, "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision", 0);
        check(rejected.list->Close(), "FG color origin rejection close");
        gpu.executeAndWait(rejected.list.Get());
        if (ngxPresentPixels(gpu, output.Get(), write) != sentinel ||
            ngxPresentPixels(gpu, real.Get(), write) != sentinel) fail("FG rejected color origin modified output");
      }
      auto record = gpu.makeRecording();
      checkNgxResult(api.evaluate(record.list.Get(), features[variant], params, nullptr),
          ngx_fixture_result_success(), "FG independent depth/motion subrect");
      resetAndDestroyParams(api, params);
      auto readback = createTextureReadback(gpu, output.Get());
      auto realReadback = createTextureReadback(gpu, real.Get());
      transition(record.list.Get(), output.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
      copyTextureToReadback(record.list.Get(), output.Get(), readback);
      transition(record.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
      transition(record.list.Get(), real.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
      copyTextureToReadback(record.list.Get(), real.Get(), realReadback);
      transition(record.list.Get(), real.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
      check(record.list->Close(), "FG subrect close");
      color.Reset(); depth.Reset(); motion.Reset(); output.Reset(); real.Reset(); hudless.Reset(); field.Reset();
      if (frame == 7) checkNgxResult(api.release(features[variant]), ngx_fixture_result_success(), "FG subrect release before execute");
      gpu.executeAndWait(record.list.Get());
      if (frame == 7) gpu.executeAndWait(record.list.Get());
      uint8_t* mapped = nullptr;
      check(realReadback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "FG real output map");
      auto expectedReal = realEnabled ? pixels : sentinel;
      if (realEnabled)
        for (UINT y = 0; y != extent; ++y)
          std::memcpy(expectedReal.data() + ((y + ry) * colorWidth + rx) * 4,
              pixels.data() + ((y + top) * colorWidth + left) * 4, extent * 4);
      if (realEnabled && rectOnly)
        for (UINT y = 0; y != colorHeight; ++y) for (UINT x = 0; x != colorWidth; ++x) {
          if (x >= rx && x < rx + extent && y >= ry && y < ry + extent) continue;
          std::memcpy(expectedReal.data() + (y * colorWidth + x) * 4, sentinel.data(), 4);
        }
      for (UINT y = 0; y != colorHeight; ++y)
        if (std::memcmp(mapped + realReadback.footprint.Offset + y * realReadback.footprint.Footprint.RowPitch,
                        expectedReal.data() + y * colorWidth * 4, colorWidth * 4))
          fail("FG real output did not preserve original composited backbuffer or unbound sentinel");
      realReadback.buffer->Unmap(0, nullptr);
      check(readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "FG subrect map");
      std::vector<uint8_t> image(extent * extent * 4);
      for (UINT y = 0; y != extent; ++y)
        std::memcpy(image.data() + y * extent * 4, mapped + readback.footprint.Offset +
            (y + oy) * readback.footprint.Footprint.RowPitch + ox * 4, extent * 4);
      for (UINT y = 0; y != colorHeight; ++y) for (UINT x = 0; x != colorWidth; ++x) {
        if (x >= ox && x < ox + extent && y >= oy && y < oy + extent) continue;
        if (std::memcmp(mapped + readback.footprint.Offset + y * readback.footprint.Footprint.RowPitch + x * 4,
                        (rectOnly ? sentinel : pixels).data() + (y * colorWidth + x) * 4, 4))
          fail("FG outside viewport copy/preserve flag mismatch");
      }
      readback.buffer->Unmap(0, nullptr);
      if (std::all_of(image.begin(), image.end(), [](uint8_t value) { return value == 19; }))
        fail("FG subrect output unwritten");
      if (!variant) control = std::move(image);
      else if (variant == 1) {
        if (frame > 1 && frame != 5 && image != control) ++distortionFrames;
        reference = std::move(image);
      } else if (variant == 3) floatReference = std::move(image);
      else if (variant == 6) precisionReference = std::move(image);
      else if (image != (variant == 7 ? precisionReference : float32 ? floatReference : reference)) {
        const auto& expected = variant == 7 ? precisionReference : float32 ? floatReference : reference;
        unsigned maximum = 0, changed = 0;
        for (size_t i = 0; i != image.size(); ++i) {
          maximum = std::max(maximum, unsigned(std::abs(int(image[i]) - int(expected[i]))));
          changed += image[i] != expected[i];
        }
        std::printf("OWNED_D3D12_NGX_FG_DISTORTION_MISMATCH frame=%u variant=%u maximum=%u changed=%u\n",
            frame, variant, maximum, changed);
        fail("FG atlas differs from independently cropped same-format reference");
      }
    }
  }
  if (!distortionFrames) fail("FG distortion field had no observable effect");
}

std::vector<uint16_t> runPublicRr(const NgxApi &api, Gpu &gpu, unsigned variant=0) {
  const char *variantName=variant==1?"linear-":variant==2?"packed-":variant==3?"mirror-":
      variant==4?"responsivity-zero-":variant==5?"responsivity-one-":
      variant==6?"responsivity-zero-tight-":variant==7?"responsivity-one-tight-":variant==8?"overlay-":"";
  std::vector<uint16_t> historyPixels;
  auto params = allocateParams(api);
  ngx_fixture_set_create_flags74(params,owned_rr::Width,owned_rr::Height,owned_rr::Width,owned_rr::Height);
  ngx_fixture_set_create_options(params,2|8,0);
  ngx_feature_set_uint(params,"DLSS.Denoise.Mode",1);
  ngx_feature_set_uint(params,"DLSS.Roughness.Mode",variant==2?1:0);
  ngx_feature_set_uint(params,"DLSS.Use.HW.Depth",variant==1?0:1);
  auto create=gpu.makeRecording(); NVSDK_NGX_Handle *feature=nullptr;
  checkNgxResult(api.create(create.list.Get(),13,params,&feature),ngx_fixture_result_success(),"public RR create13");
  check(create.list->Close(),"RR create close"); resetAndDestroyParams(api,params);
  constexpr const char *keys[]={"Color","Depth","MotionVectors","DLSS.Input.DiffuseAlbedo",
      "DLSS.Input.SpecularAlbedo","GBuffer.Normals","GBuffer.Roughness","DLSSD.SpecularHitDistance"};
  constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,
      DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT};
  for(UINT index=1;index<=owned_rr::Frames;++index) {
    auto pattern=owned_rr::makeFrame(index);
    if(variant==1) for(auto &depth:pattern.depth) depth=.1f/depth;
    if(variant==3) {
      std::fill(pattern.specular.begin(),pattern.specular.end(),0x3c00);
      std::fill(pattern.roughness.begin(),pattern.roughness.end(),0.0f);
    }
    const void *data[]={pattern.color.data(),pattern.depth.data(),pattern.motion.data(),pattern.diffuse.data(),
        pattern.specular.data(),pattern.normals.data(),pattern.roughness.data(),pattern.hit.data()};
    constexpr UINT strides[]={8,4,4,8,8,8,4,4};
    std::array<ComPtr<ID3D12Resource>,8> textures;
    params=allocateParams(api);
    for(size_t i=0;i<textures.size();++i) {
      textures[i]=createTexture(gpu.device.Get(),owned_rr::Width,owned_rr::Height,formats[i],D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
      uploadTexture(gpu,textures[i].Get(),data[i],owned_rr::Width*strides[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      if(variant!=2 || i!=6) ngx_feature_set_resource(params,keys[i],textures[i].Get());
    }
    auto output=createTexture(gpu.device.Get(),owned_rr::Width,owned_rr::Height,DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback=createTextureReadback(gpu,output.Get());
    ngx_feature_set_resource(params,"Output",output.Get());
    ngx_feature_set_pointer(params,"WorldToViewMatrix",pattern.world);
    ngx_feature_set_pointer(params,"ViewToClipMatrix",pattern.projection);
    ngx_feature_set_float(params,"Jitter.Offset.X",pattern.jitterX); ngx_feature_set_float(params,"Jitter.Offset.Y",pattern.jitterY);
    ngx_feature_set_uint(params,"Reset",pattern.reset);
    auto record=gpu.makeRecording();
    ComPtr<ID3D12Resource> responsivity, overlay;
    if (variant==8) {
      const UINT w=owned_rr::Width+2,h=owned_rr::Height+1;
      std::vector<uint32_t> rgba(w*h);
      for(UINT y=1;y<h;++y) std::fill_n(rgba.data()+y*w+2,owned_rr::Width,0xff0000ffu);
      overlay=createTexture(gpu.device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
      uploadTexture(gpu,overlay.Get(),rgba.data(),w*4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      ngx_feature_set_resource(params,"DLSS.TransparencyLayer",overlay.Get());
      ngx_feature_set_uint(params,"DLSS.TransparencyLayer.Subrect.Base.X",2);
      ngx_feature_set_uint(params,"DLSS.TransparencyLayer.Subrect.Base.Y",1);
    } else if (variant>=4) {
      const UINT left=variant>=6?0:2, top=variant>=6?0:3;
      const UINT atlasWidth=owned_rr::Width+(variant>=6?0:3), atlasHeight=owned_rr::Height+(variant>=6?0:5);
      // Opposite-valued margins catch accidentally ignoring the public origin.
      const bool zero=(variant%2)==0;
      std::vector<uint8_t> mask(size_t(atlasWidth)*atlasHeight,zero?255:0);
      for(UINT y=0;y<owned_rr::Height;++y)
        std::fill_n(mask.data()+size_t(y+top)*atlasWidth+left,owned_rr::Width,zero?0:255);
      responsivity=createTexture(gpu.device.Get(),atlasWidth,atlasHeight,DXGI_FORMAT_R8_UNORM,
          D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
      uploadTexture(gpu,responsivity.Get(),mask.data(),atlasWidth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      ngx_feature_set_resource(params,"DLSSD.ResponsivityMask",responsivity.Get());
      ngx_feature_set_uint(params,"DLSSD.ResponsivityMask.Subrect.Base.X",left);
      ngx_feature_set_uint(params,"DLSSD.ResponsivityMask.Subrect.Base.Y",top);
      if(index==1 && variant==4) {
        ngx_feature_set_uint(params,"DLSSD.ResponsivityMask.Subrect.Base.X",UINT_MAX);
        checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_invalid_parameter(),"RR responsivity overflowing subrect rejected");
        ngx_feature_set_uint(params,"DLSSD.ResponsivityMask.Subrect.Base.X",2);
        ngx_feature_set_resource(params,"DLSSD.ResponsivityMask",textures[2].Get());
        checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_invalid_parameter(),"RR responsivity vector format rejected");
        ngx_feature_set_resource(params,"DLSSD.ResponsivityMask",output.Get());
        checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_invalid_parameter(),"RR responsivity output alias rejected");
        ngx_feature_set_resource(params,"DLSSD.ResponsivityMask",nullptr);
        checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_invalid_parameter(),"RR absent responsivity origin rejected");
        ngx_feature_set_resource(params,"DLSSD.ResponsivityMask",responsivity.Get());
        for(const char *key:{"DLSSD.SpecularRayDirectionHitDistance","DLSSD.Alpha","DLSSD.ColorBeforeTransparency",
                             "DLSSD.ColorBeforeParticles","DLSS.TransparencyLayerOpacity"}) {
          ngx_feature_set_resource(params,key,textures[3].Get());
          checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_unsupported_parameter(),"RR guide without native consumer rejected");
          ngx_feature_set_resource(params,key,nullptr);
        }
      }
    }
    if (index==1) {
      ngx_feature_set_resource(params,"DLSS.Input.DiffuseAlbedo",nullptr);
      checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_invalid_parameter(),"RR missing guide rejected");
      ngx_feature_set_resource(params,"DLSS.Input.DiffuseAlbedo",textures[3].Get());
    }
    checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_success(),"public RR evaluate13");
    if ((variant==0 || variant==3) && !owned_rr::saveInputs(".",variant==3?"public-rr-mirror-input":"public-rr-input",
        index,pattern,pattern.world,pattern.projection)) fail("RR input parity artifact");
    std::fill_n(pattern.world,16,0.0f); std::fill_n(pattern.projection,16,0.0f);
    resetAndDestroyParams(api,params);
    for(auto &texture:textures) texture.Reset();
    responsivity.Reset();
    overlay.Reset();
    if(index==owned_rr::Frames) checkNgxResult(api.release(feature),ngx_fixture_result_success(),"RR release before execute");
    transition(record.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(record.list.Get(),output.Get(),readback);
    transition(record.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    check(record.list->Close(),"public RR close");
    gpu.executeAndWait(record.list.Get());
    uint8_t *mapped=nullptr; check(readback.buffer->Map(0,nullptr,reinterpret_cast<void **>(&mapped)),"RR map");
    std::vector<uint16_t> pixels(size_t(owned_rr::Width)*owned_rr::Height*4);
    for(UINT y=0;y<owned_rr::Height;++y) std::memcpy(pixels.data()+size_t(y)*owned_rr::Width*4,
        mapped+readback.footprint.Offset+size_t(y)*readback.footprint.Footprint.RowPitch,owned_rr::Width*8);
    readback.buffer->Unmap(0,nullptr);
    UINT nonzero=0; for(size_t pixel=0;pixel<pixels.size();++pixel) {
      if((pixels[pixel]&0x7c00)==0x7c00) fail("RR nonfinite output");
      if(pixel%4!=3 && pixels[pixel]) ++nonzero;
    }
    if(nonzero<owned_rr::Width*owned_rr::Height) fail("RR empty denoising output");
    if(variant==8) {
      for(size_t i=0;i<pixels.size();i+=4)
        if(pixels[i]!=0x3c00 || pixels[i+1] || pixels[i+2]) fail("RR opaque red transparency overlay");
    } else if(variant>=4 && index>1) historyPixels.insert(historyPixels.end(),pixels.begin(),pixels.end());
    char filename[64]; std::snprintf(filename,sizeof(filename),"public-rr-%sframe-%u.bin",variantName,index);
    FILE *file=std::fopen(filename,"wb"); if(!file) fail("RR oracle artifact open");
    if(std::fwrite(pixels.data(),sizeof(uint16_t),pixels.size(),file)!=pixels.size()) fail("RR oracle artifact write");
    if(std::fclose(file)) fail("RR oracle artifact close");
    if(index==owned_rr::Frames) {
      gpu.executeAndWait(record.list.Get());
      check(readback.buffer->Map(0,nullptr,reinterpret_cast<void **>(&mapped)),"RR replay map");
      for(UINT y=0;y<owned_rr::Height;++y) std::memcpy(pixels.data()+size_t(y)*owned_rr::Width*4,
          mapped+readback.footprint.Offset+size_t(y)*readback.footprint.Footprint.RowPitch,owned_rr::Width*8);
      readback.buffer->Unmap(0,nullptr);
      for(auto pixel:pixels) if((pixel&0x7c00)==0x7c00) fail("RR replay nonfinite");
      std::snprintf(filename,sizeof(filename),"public-rr-%sreplay.bin",variantName);
      file=std::fopen(filename,"wb"); if(!file) fail("RR replay artifact open");
      if(std::fwrite(pixels.data(),sizeof(uint16_t),pixels.size(),file)!=pixels.size()) fail("RR replay artifact write");
      if(std::fclose(file)) fail("RR replay artifact close");
      checkNgxResult(api.release(feature),ngx_fixture_result_feature_not_found(),"RR stale handle");
    }
  }
  std::printf("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_rr variant=%u frames=8 handle13=1 denoising_guides=1 matrices_snapshotted=1 release_before_execute=1 replay=1 oracle_artifacts=8\n",variant);
  return historyPixels;
}
void runPublicRrResponsivity(const NgxApi &api,Gpu &gpu) {
  const auto zero=runPublicRr(api,gpu,4);
  const auto one=runPublicRr(api,gpu,5);
  if(zero!=runPublicRr(api,gpu,6) || one!=runPublicRr(api,gpu,7))
    fail("RR responsivity atlas origin changed numeric output");
  if(zero.size()!=one.size()) fail("RR responsivity image extent mismatch");
  size_t changed=0;
  for(size_t i=0;i<zero.size();++i) if(i%4!=3 && zero[i]!=one[i]) ++changed;
  if(!changed) fail("RR responsivity mask did not change numeric RGB output");
  std::printf("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_rr_responsivity changed_rgb=%zu origins=1 invalid_resource_subrect=1 release_before_execute=1 replay=1\n",changed);
}
void runPublicAlpha(const NgxApi &api,Gpu &gpu) {
  auto params=allocateParams(api); ngx_fixture_set_create_flags74(params,64,64,128,128);
  ngx_fixture_set_create_options(params,74|128,0);
  auto create=gpu.makeRecording(); NVSDK_NGX_Handle *feature=nullptr;
  checkNgxResult(api.create(create.list.Get(),1,params,&feature),ngx_fixture_result_success(),"public SR alpha128 create");
  check(create.list->Close(),"alpha create close"); resetAndDestroyParams(api,params);
  auto color=createTexture(gpu.device.Get(),64,64,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<uint8_t> pixels(64*64*4);
  for(UINT y=0;y<64;++y) for(UINT x=0;x<64;++x) {
    size_t i=(y*64+x)*4; pixels[i]=112; pixels[i+1]=96; pixels[i+2]=80; pixels[i+3]=x<32?48:192;
  }
  uploadTexture(gpu,color.Get(),pixels.data(),64*4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  auto depth=makeDepth(gpu); auto motion=makeMotion(gpu);
  auto output=createTexture(gpu.device.Get(),128,128,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback=createTextureReadback(gpu,output.Get());
  params=allocateParams(api); ngx_fixture_set_frame_flags74(params,color.Get(),output.Get(),depth.Get(),motion.Get(),1);
  auto record=gpu.makeRecording();
  checkNgxResult(api.evaluate(record.list.Get(),feature,params,nullptr),ngx_fixture_result_success(),"public alpha evaluate");
  resetAndDestroyParams(api,params); checkNgxResult(api.release(feature),ngx_fixture_result_success(),"alpha release before execute");
  transition(record.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  copyTextureToReadback(record.list.Get(),output.Get(),readback);
  transition(record.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  check(record.list->Close(),"public alpha close");
  for (UINT replay=0; replay<2; ++replay) {
    gpu.executeAndWait(record.list.Get());
    uint8_t *mapped=nullptr; check(readback.buffer->Map(0,nullptr,reinterpret_cast<void **>(&mapped)),"alpha map");
    const auto *row=mapped+readback.footprint.Offset+64*readback.footprint.Footprint.RowPitch;
    const auto *left=row+16*4, *right=row+112*4;
    if(left[3]>96 || right[3]<144 || right[3]>240) fail("public reconstructed alpha contrast");
    int rgbError=0;
    const int expected[] = {112,96,80};
    for (UINT c=0; c<3; ++c) rgbError=std::max({rgbError,std::abs(int(left[c])-expected[c]),std::abs(int(right[c])-expected[c])});
    if(rgbError>8) fail("alpha disturbed RGB");
    std::printf("OWNED_D3D12_NGX_ALPHA_PIXELS replay=%u left=%u,%u,%u,%u right=%u,%u,%u,%u rgb_max_error=%d release_before_execute=1\n",
                replay,left[0],left[1],left[2],left[3],right[0],right[1],right[2],right[3],rgbError);
    readback.buffer->Unmap(0,nullptr);
  }
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_alpha flag128=1 nonopaque_contrast=1 rgb_preserved=1");
}
}
namespace {
void runPublicFgSceneChange(const NgxApi& api, Gpu& gpu) {
  constexpr UINT extent = ngxPresentExtent;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  auto depth = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const std::vector<float> depths(extent * extent, .01f);
  uploadTexture(gpu, depth.Get(), depths.data(), extent * sizeof(float), read);
  auto motion = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R16G16_FLOAT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const std::vector<uint16_t> vectors(extent * extent * 2, 0);
  uploadTexture(gpu, motion.Get(), vectors.data(), extent * 4, read);
  const std::vector<uint8_t> sentinel(extent * extent * 4, 19);
  std::vector<uint8_t> pixels(extent * extent * 4, 0);
  for (size_t pixel = 3; pixel < pixels.size(); pixel += 4) pixels[pixel] = 255;
  auto feature = ngxPresentFeature(api, gpu);
  for (UINT frame = 0; frame < 14; ++frame) {
    auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (frame == 13) std::fill(pixels.begin(), pixels.end(), 255);
    uploadTexture(gpu, color.Get(), pixels.data(), extent * 4, read);
    auto output = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    uploadTexture(gpu, output.Get(), sentinel.data(), extent * 4, write);
    auto record = ngxPresentEvaluate(api, gpu, feature, frame, frame == 13, color.Get(), nullptr, output.Get(), depth.Get(), motion.Get());
    gpu.executeAndWait(record.list.Get());
    if (ngxPresentPixels(gpu, output.Get(), write) == sentinel) fail("FG scene/reset native output unwritten");
  }
  checkNgxResult(api.release(feature), ngx_fixture_result_success(), "public FG scene release");
  std::puts("OWNED_D3D12_NGX_SCENE frames=14 reset=1 output_written=1 actual_count=unavailable consumer=gpu_readback");
}
}
int main(int argc, char** argv) {
  const bool present = (argc == 3 || argc == 5) && std::strcmp(argv[1],"--mode") == 0 && std::strcmp(argv[2],"present") == 0;
  const char* presentControl = nullptr;
  if (present && argc == 5) {
    if (std::strcmp(argv[3], "--present-control") != 0 || !argv[4][0])
      fail("expected --mode present --present-control <path>");
    presentControl = argv[4];
  }
  const bool alphaOnly = argc == 2 && std::strcmp(argv[1],"--alpha-only") == 0;
  const bool fgOnly = argc == 2 && std::strcmp(argv[1],"--fg-only") == 0;
  const bool rrOnly = argc == 2 && std::strcmp(argv[1],"--rr-only") == 0;
  const bool rrGuidesOnly = argc == 2 && std::strcmp(argv[1],"--rr-guides-only") == 0;
  if (argc != 1 && !present && !alphaOnly && !fgOnly && !rrOnly && !rrGuidesOnly)
    fail("expected no arguments, --mode present [--present-control <path>], --alpha-only, --fg-only, --rr-only or --rr-guides-only");
  requireOwnedBackend();
  NgxApi api; Gpu gpu;
  checkNgxResult(api.init(0x4e47584645415455ull,L"C:\\",gpu.device.Get(),nullptr,ngx_fixture_sdk_version()),ngx_fixture_result_success(),"public feature init");
  if (present) {
    runPublicFgPresent(api,gpu,presentControl);
    checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public present shutdown");
    return 0;
  }
  if (alphaOnly) {
    runPublicAlpha(api,gpu);
    checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public alpha shutdown");
    std::puts("OWNED_D3D12_NGX_ALPHA_PASS cases=1 release_before_execute=1 replay=1 rgb_alpha=gpu_readback");
    return 0;
  }
  if (rrOnly || rrGuidesOnly) {
    if(rrOnly) {
      runPublicRr(api,gpu); runPublicRr(api,gpu,1); runPublicRr(api,gpu,2); runPublicRr(api,gpu,3);
    }
    runPublicRrResponsivity(api,gpu);
    runPublicRr(api,gpu,8);
    checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public RR shutdown");
    std::puts("OWNED_D3D12_NGX_RR_PASS numeric_responsivity=1 invalid_resource_subrect=1 release_before_execute=1 replay=1");
    return 0;
  }
  checkCapabilities(api,gpu);
  runPublicFg(api,gpu);
  runPublicFgCameraMotion(api,gpu);
  runPublicFgCameraMotion(api,gpu,true);
  runPublicFgCameraInputs(api,gpu,false);
  runPublicFgCameraInputs(api,gpu,true);
  runPublicFgMotionInputs(api,gpu,false);
  runPublicFgMotionInputs(api,gpu,true);
  if (fgCaptureEnabled) {
    if (fgCaptureSerial!=144) fail("FG contract observation count");
    std::puts("OWNED_FG_CONTRACT_PASS evaluations=144 native_inputs=1 same_encode_scatter=1");
  }
  runPublicFgUi(api,gpu);
  runPublicFgSceneChange(api,gpu);
  runPublicFgSubrect(api,gpu);
  if (fgOnly) {
    checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public FG shutdown");
    std::puts("OWNED_D3D12_NGX_FG_PASS capacity=1 native_output=1 unsupported_mfg=1");
    return 0;
  }
  runPublicSpatial(api,gpu);
  runPublicRr(api,gpu);
  runPublicRr(api,gpu,1);
  runPublicRr(api,gpu,2);
  runPublicRr(api,gpu,3);
  runPublicRrResponsivity(api,gpu);
  runPublicAlpha(api,gpu);
  checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public feature shutdown");
  std::puts("OWNED_D3D12_NGX_FEATURES_PASS");
}
