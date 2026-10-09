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
#include "owned-d3d12-ngx-present.inc"
namespace {
void checkCapabilities(const NgxApi &api,Gpu &gpu) {
  const GUID oldFgDevice = {0xb2a31090,0xa1f2,0x4717,{0xbb,0x50,0x61,0x7a,0x47,0xf2,0xa0,0x11}};
  ComPtr<IUnknown> oldFg;
  if (gpu.device->QueryInterface(oldFgDevice, reinterpret_cast<void**>(oldFg.GetAddressOf())) != E_NOINTERFACE || oldFg)
    fail("obsolete FG factory ABI accepted");
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
  if (countMax != 1) fail("public FG advertised generated capacity");
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
      auto flag = ngxPresentDisableBuffer(gpu);
      ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", flag.Get());
      checkNgxResult(api.evaluate(rejected.list.Get(), feature, params, nullptr), ngx_fixture_result_unsupported_parameter(), "FG native unsupported disable buffer");
      resetAndDestroyParams(api, params);
      check(rejected.list->Close(), "FG rejected close"); gpu.executeAndWait(rejected.list.Get());
      if (ngxPresentPixels(gpu, output.Get(), write) != sentinel) fail("rejected FG modified output");
      std::puts("OWNED_D3D12_NGX_REJECT case=disable_buffer result=unsupported_parameter output_untouched=1 before_valid_dispatch=1 zero_count_index=invalid_parameter");
    }
    const bool reset = frame == 0 || frame == 5;
    const UINT frameId = frame == 8 ? 7 : frame;
    auto record = ngxPresentEvaluate(api, gpu, feature, frameId, reset, color.Get(), nullptr, output.Get(), depth.Get(), motion.Get(), false, frame == 6);
    auto readback = createTextureReadback(gpu, output.Get());
    transition(record.list.Get(), output.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(record.list.Get(), output.Get(), readback);
    transition(record.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, write);
    check(record.list->Close(), "FG lifetime close");
    color.Reset(); depth.Reset(); motion.Reset(); output.Reset();
    if (frame == 8) checkNgxResult(api.release(feature), ngx_fixture_result_success(), "FG release before execute");
    for (UINT replay = 0; replay <= UINT(frame == 0 || frame == 7 || frame == 8); ++replay) {
      gpu.executeAndWait(record.list.Get());
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
  std::puts("OWNED_D3D12_NGX_FEATURE_CASE_PASS case=public_fg count_max=1 unsupported_mfg=1 disable_buffer=unsupported reset=native_history release_before_execute=1 replay=native_encode");
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
  runPublicFgSceneChange(api,gpu);
  if (fgOnly) {
    checkNgxResult(api.shutdown1(gpu.device.Get()),ngx_fixture_result_success(),"public FG shutdown");
    std::puts("OWNED_D3D12_NGX_FG_PASS capacity=1 native_output=1 unsupported_mfg=1");
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
