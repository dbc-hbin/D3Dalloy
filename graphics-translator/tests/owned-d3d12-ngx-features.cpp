// Public NGX FG/RR entry points with the real MSVC parameter consumer.
#define main ownedNgxSupportMain
#include "owned-d3d12-ngx.cpp"
#undef main
#include "owned-rr-pattern.h"
#include <cmath>

extern "C" {
void ngx_feature_set_uint(NVSDK_NGX_Parameter *, const char *, unsigned int);
void ngx_feature_set_float(NVSDK_NGX_Parameter *, const char *, float);
void ngx_feature_set_resource(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
void ngx_feature_set_pointer(NVSDK_NGX_Parameter *, const char *, void *);
unsigned int ngx_feature_get_uint(NVSDK_NGX_Parameter *, const char *, unsigned int *);
}
namespace {
void checkCapabilities(const NgxApi &api,Gpu &gpu) {
  auto get = loadExport<NgxAllocateParameters>(api.module,"NVSDK_NGX_D3D12_GetCapabilityParameters");
  NVSDK_NGX_Parameter *params = nullptr;
  checkNgxResult(get(&params),ngx_fixture_result_success(),"feature capability parameters");
  for (const char *key : {"FrameGeneration.Available","SuperSamplingDenoising.Available"}) {
    UINT value = 0;
    checkNgxResult(ngx_feature_get_uint(params,key,&value),ngx_fixture_result_success(),key);
    if (value != 1) fail(key);
  }
  UINT countMax = 0;
  checkNgxResult(ngx_feature_get_uint(params, "DLSSG.MultiFrameCountMax", &countMax), ngx_fixture_result_success(), "FG generated capacity");
  if (countMax != 4) fail("public FG advertised generated capacity");
  for (const char *key : {"InPainting.Available","ImageSuperResolution.Available","SlowMotion.Available",
       "VideoSuperResolution.Available","ImageSignalProcessing.Available","DeepResolve.Available","DeepDVC.Available"}) {
    UINT value = 1;
    checkNgxResult(ngx_feature_get_uint(params,key,&value),ngx_fixture_result_success(),key);
    if (value) fail("unsupported public NGX feature capability");
  }
  resetAndDestroyParams(api,params);
  params=allocateParams(api);
  auto recording=gpu.makeRecording();
  for(UINT feature : {0u,2u,3u,4u,5u,6u,7u,8u,9u,10u,12u,14u,15u,16u,17u,18u,19u,0xffffffffu}) {
    NVSDK_NGX_Handle *handle=nullptr;
    checkNgxResult(api.create(recording.list.Get(),feature,params,&handle),ngx_fixture_result_feature_not_supported(),"unsupported feature CreateFeature contract");
    if(handle) fail("unsupported feature fabricated a handle");
  }
  check(recording.list->Close(),"unsupported inventory close"); resetAndDestroyParams(api,params);
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=exact_capabilities fg=1 rr=1 non_sr_other=0");
}

void runPublicFg(const NgxApi &api, Gpu &gpu, bool grouped = false) {
  constexpr UINT extent = 128;
  constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const std::vector<uint8_t> sentinel(extent * extent * 4, 19);
  auto depth = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const std::vector<float> depths(extent * extent, .75f);
  uploadTexture(gpu, depth.Get(), depths.data(), extent * sizeof(float), read);
  auto motion = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R16G16_FLOAT,
      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<uint16_t> vectors(extent * extent * 2, 0);
  for (size_t pixel = 0; pixel < vectors.size(); pixel += 2) vectors[pixel] = 0xc400; // Current-to-previous: -4 pixels.
  uploadTexture(gpu, motion.Get(), vectors.data(), extent * 4, read);
  for (UINT count = 1; count <= 4; ++count) {
    auto params = allocateParams(api);
    ngx_feature_set_uint(params, "DLSSG.Width", extent);
    ngx_feature_set_uint(params, "DLSSG.Height", extent);
    ngx_feature_set_uint(params, "DLSSG.InternalWidth", extent);
    ngx_feature_set_uint(params, "DLSSG.InternalHeight", extent);
    ngx_feature_set_uint(params, "DLSSG.BackbufferFormat", DXGI_FORMAT_R8G8B8A8_UNORM);
    ngx_feature_set_uint(params, "DLSSG.DepthInverted", 1);
    auto create = gpu.makeRecording(); NVSDK_NGX_Handle *feature = nullptr;
    checkNgxResult(api.create(create.list.Get(), 11, params, &feature), ngx_fixture_result_success(), "public FG create11");
    check(create.list->Close(), "FG create close"); resetAndDestroyParams(api, params);
    for (UINT frame = 0; frame < 7; ++frame) {
      auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      std::vector<uint8_t> pixels(extent * extent * 4);
      for (UINT y = 0; y < extent; ++y) for (UINT x = 0; x < extent; ++x) {
        const size_t pixel = (size_t(y) * extent + x) * 4;
        pixels[pixel] = uint8_t(x < 48 + frame * 4 ? 32 + frame * 8 : 200);
        pixels[pixel + 1] = 96; pixels[pixel + 2] = 160; pixels[pixel + 3] = 255;
      }
      uploadTexture(gpu, color.Get(), pixels.data(), extent * 4, read);
      std::vector<std::vector<uint8_t>> phases;
      TextureReadback imageReadbacks[4];
      ComPtr<ID3D12Resource> flagReadbacks[4];
      auto groupRecord = grouped ? gpu.makeRecording() : Recording{};
      for (UINT index = 1; index <= count; ++index) {
        auto output = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadTexture(gpu, output.Get(), sentinel.data(), extent * 4, write);
        auto flagDesc = bufferDescription(4); flagDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> flag;
        check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &flagDesc, write,
            nullptr, IID_PPV_ARGS(&flag)), "FG disable buffer");
        auto flagReadback = createBuffer(gpu.device.Get(), 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto readback = createTextureReadback(gpu, output.Get());
        params = allocateParams(api);
        ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
        ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
        ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", flag.Get());
        ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
        ngx_feature_set_resource(params, "DLSSG.MVecs", motion.Get());
        ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", count);
        ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", index);
        if (frame != 1) ngx_feature_set_uint(params, "DLSSG.BackbufferFrameID", frame == 6 ? 5 : frame);
        ngx_feature_set_uint(params, "DLSSG.DepthInverted", 1);
        ngx_feature_set_float(params, "DLSSG.MvecScaleX", 1.0f / extent);
        ngx_feature_set_float(params, "DLSSG.MvecScaleY", 1.0f / extent);
        ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
        ngx_feature_set_float(params, "DLSSG.CameraFar", 1000);
        ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
        ngx_feature_set_uint(params, "DLSSG.Reset", frame == 0 || frame == 2);
        ngx_feature_set_uint(params, "DLSSG.NotRenderingGameFrames", frame == 4);
        auto record = grouped ? groupRecord : gpu.makeRecording();
        if (frame == 0 && index == 1) {
          ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", 5);
          checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_unsupported_parameter(), "public FG capacity bound");
          ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", count);
          ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", 0);
          checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(), "public FG zero index");
          ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", index);
          ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", output.Get());
          checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(), "public FG flag buffer shape");
          ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", flag.Get());
        }
        if (frame == 1 && index > 1) {
          ngx_feature_set_float(params, "DLSSG.CameraNear", .2f);
          checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(), "public FG same pair immutable guides");
          ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
        }
        checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_success(), "public FG per-index evaluate11");
        resetAndDestroyParams(api, params);
        if (frame == 6 && index == count)
          checkNgxResult(api.release(feature), ngx_fixture_result_success(), "public FG release before execute");
        transition(record.list.Get(), output.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copyTextureToReadback(record.list.Get(), output.Get(), readback);
        transition(record.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
        transition(record.list.Get(), flag.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
        record.list->CopyBufferRegion(flagReadback.Get(), 0, flag.Get(), 0, 4);
        transition(record.list.Get(), flag.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
        imageReadbacks[index - 1] = readback;
        flagReadbacks[index - 1] = flagReadback;
        if (grouped && index < count) continue;
        check(record.list->Close(), "public FG close");
        gpu.executeAndWait(record.list.Get());
        for (UINT observed = grouped ? 1 : index; observed <= index; ++observed) {
        const auto &readback = imageReadbacks[observed - 1];
        const auto &flagReadback = flagReadbacks[observed - 1];
        uint8_t *flagBytes = nullptr;
        check(flagReadback->Map(0, nullptr, reinterpret_cast<void **>(&flagBytes)), "FG flag readback");
        const bool disabled = flagBytes[0] != 0;
        flagReadback->Unmap(0, nullptr);
        const bool expectedDisabled = frame == 0 || frame == 2 || frame == 4 || frame == 6;
        if (disabled != expectedDisabled) fail("public FG actual suppression flag");
        uint8_t *bytes = nullptr;
        check(readback.buffer->Map(0, nullptr, reinterpret_cast<void **>(&bytes)), "FG pixels readback");
        std::vector<uint8_t> image(extent * extent * 4);
        for (UINT y = 0; y < extent; ++y) std::memcpy(image.data() + size_t(y) * extent * 4,
            bytes + readback.footprint.Offset + size_t(y) * readback.footprint.Footprint.RowPitch, extent * 4);
        readback.buffer->Unmap(0, nullptr);
        if (disabled && image != sentinel) fail("suppressed public FG modified output");
        if (!disabled && (image == sentinel || image == pixels)) fail("public FG fake/current output");
        if (!disabled && count > 1) for (const auto &prior : phases)
          if (image == prior) fail("public FG repeated phase output");
        const size_t probe = (size_t(extent / 2) * extent + 24) * 4;
        const double phase = double(observed) / (count + 1);
        const double expectedRed = disabled ? 19 : 32 + (frame - 1) * 8 + phase * 8;
        if (std::abs(double(image[probe]) - expectedRed) > 1 || (!disabled &&
            (std::abs(int(image[probe + 1]) - 96) > 1 || std::abs(int(image[probe + 2]) - 160) > 1 || image[probe + 3] != 255))) {
          std::fprintf(stderr, "OWNED_D3D12_NGX_COLOR_MISMATCH frame=%u count=%u index=%u grouped=%u phase=%g output_flag=%u rgba=%u,%u,%u,%u expected_red=%g probe=24,64\n",
              frame, count, observed, UINT(grouped), phase, UINT(disabled), UINT(image[probe]),
              UINT(image[probe + 1]), UINT(image[probe + 2]), UINT(image[probe + 3]), expectedRed);
          fail("public FG phase-consistent temporal color");
        }
        const UINT probeRed = image[probe];
        phases.push_back(std::move(image));
        std::printf("OWNED_D3D12_NGX_PHASE frame=%u count=%u index=%u phase=%g actual=%u grouped=%u actual_count=%u output_flag=%u untouched=%u probe_red=%u consumer=gpu_readback\n",
            frame, count, observed, phase, disabled ? 0u : 1u, UINT(grouped), disabled ? 0u : count, UINT(disabled), UINT(disabled), probeRed);
        }
        if ((frame == 0 || frame == 5 || frame == 6) && index == count) {
          gpu.executeAndWait(record.list.Get());
          for (UINT observed = grouped ? 1 : index; observed <= index; ++observed) {
            uint8_t *bytes = nullptr;
            check(flagReadbacks[observed - 1]->Map(0, nullptr, reinterpret_cast<void **>(&bytes)), "FG replay flag map");
            const UINT replayFlag = bytes[0];
            flagReadbacks[observed - 1]->Unmap(0, nullptr);
            const auto &readback = imageReadbacks[observed - 1];
            check(readback.buffer->Map(0, nullptr, reinterpret_cast<void **>(&bytes)), "FG replay pixels map");
            // A closed-list replay is a no-op for every already-started FG
            // packet. It preserves the original GPU outcome, including valid
            // flag0 and bootstrap/stale flag1, without promoting history twice.
            const UINT expectedFlag = frame == 5 ? 0u : 1u;
            if (replayFlag != expectedFlag) {
              const auto *pixel = bytes + readback.footprint.Offset + size_t(64) * readback.footprint.Footprint.RowPitch + 24 * 4;
              const auto *original = phases[observed - 1].data() + (size_t(64) * extent + 24) * 4;
              std::fprintf(stderr, "OWNED_D3D12_NGX_REPLAY_MISMATCH frame=%u count=%u index=%u grouped=%u output_flag=%u expected_flag=%u rgba=%u,%u,%u,%u original_rgba=%u,%u,%u,%u probe=24,64\n",
                  frame, count, observed, UINT(grouped), replayFlag, expectedFlag,
                  pixel[0], pixel[1], pixel[2], pixel[3], original[0], original[1], original[2], original[3]);
              fail("public FG replay retained GPU outcome");
            }
            for (UINT y = 0; y < extent; ++y)
              if (std::memcmp(phases[observed - 1].data() + size_t(y) * extent * 4,
                  bytes + readback.footprint.Offset + size_t(y) * readback.footprint.Footprint.RowPitch, extent * 4))
                fail("public FG immutable replay/actual0 output changed");
            readback.buffer->Unmap(0, nullptr);
          }
        }
        if (frame == 6 && index == count) {
          checkNgxResult(api.release(feature), ngx_fixture_result_feature_not_found(), "public FG stale handle");
        }
      }
    }
  }
  if (grouped) std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=4 per_index_output=1 grouped_recording=1 actual0_untouched=1 immutable_params=1 release_before_execute=1 replay=1");
}
void runPublicRr(const NgxApi &api, Gpu &gpu, unsigned variant=0) {
  const char *variantName=variant==1?"linear-":variant==2?"packed-":variant==3?"mirror-":"";
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
#include "owned-d3d12-ngx-present.inc"
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
  for (UINT count = 1; count <= 4; ++count) {
    auto feature = ngxPresentFeature(api, gpu);
    auto flag = ngxPresentDisableBuffer(gpu);
    for (UINT frame = 0; frame < 14; ++frame) {
      auto color = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
      if (frame == 13) std::fill(pixels.begin(), pixels.end(), 255);
      uploadTexture(gpu, color.Get(), pixels.data(), extent * 4, read);
      for (UINT index = 1; index <= (frame == 13 ? count : 1); ++index) {
        auto output = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadTexture(gpu, output.Get(), sentinel.data(), extent * 4, write);
        auto params = allocateParams(api);
        ngx_feature_set_resource(params, "DLSSG.Backbuffer", color.Get());
        ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output.Get());
        ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", flag.Get());
        ngx_feature_set_resource(params, "DLSSG.Depth", depth.Get());
        ngx_feature_set_resource(params, "DLSSG.MVecs", motion.Get());
        ngx_feature_set_uint(params, "DLSSG.MultiFrameCount", count);
        ngx_feature_set_uint(params, "DLSSG.MultiFrameIndex", index);
        ngx_feature_set_uint(params, "DLSSG.BackbufferFrameID", frame);
        ngx_feature_set_uint(params, "DLSSG.DepthInverted", 1);
        ngx_feature_set_float(params, "DLSSG.MvecScaleX", 1.0f / extent);
        ngx_feature_set_float(params, "DLSSG.MvecScaleY", 1.0f / extent);
        ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
        ngx_feature_set_float(params, "DLSSG.CameraFar", 1000);
        ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
        ngx_feature_set_uint(params, "DLSSG.Reset", 0);
        auto record = gpu.makeRecording();
        checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_success(), "public FG scene evaluate");
        resetAndDestroyParams(api, params);
        check(record.list->Close(), "public FG scene close");
        gpu.executeAndWait(record.list.Get());
        if (frame == 13 && (!ngxPresentDisabled(gpu, flag.Get()) || ngxPresentPixels(gpu, output.Get(), write) != sentinel))
          fail("public FG SDK scene change actual0 output untouched");
      }
    }
    checkNgxResult(api.release(feature), ngx_fixture_result_success(), "public FG scene release");
    std::printf("OWNED_D3D12_NGX_SCENE count=%u actual=0 output_flag=1 untouched=1 phases=%u reset=0 consumer=gpu_readback\n", count, count);
    std::fill(pixels.begin(), pixels.end(), 0);
    for (size_t pixel = 3; pixel < pixels.size(); pixel += 4) pixels[pixel] = 255;
  }
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
  if (argc != 1 && !present && !alphaOnly && !fgOnly) fail("expected no arguments, --mode present [--present-control <path>], --alpha-only or --fg-only");
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
  checkCapabilities(api,gpu);
  runPublicFg(api,gpu);
  runPublicFg(api,gpu,true);
  runPublicFgSceneChange(api,gpu);
  if (fgOnly) {
    checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public FG shutdown");
    std::puts("OWNED_D3D12_NGX_FG_PASS capacity=4 independent=1 grouped=1");
    return 0;
  }
  runPublicRr(api,gpu);
  runPublicRr(api,gpu,1);
  runPublicRr(api,gpu,2);
  runPublicRr(api,gpu,3);
  runPublicAlpha(api,gpu);
  checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public feature shutdown");
  std::puts("OWNED_D3D12_NGX_FEATURES_PASS");
}
