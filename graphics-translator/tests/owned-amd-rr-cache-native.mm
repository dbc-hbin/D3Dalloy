// Native AMD RR cache proof using the production provider, not a mock scaler.
#include "../../src/winemetal/unix/metalfx_amd_rr.mm"
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#ifndef EXPECT_RR_PIPELINES
#define EXPECT_RR_PIPELINES 11
#endif

namespace {
using namespace d3dalloy::rr;
void require(bool ok, const char *reason) {
  if (!ok) { std::fprintf(stderr,"AMD_RR_CACHE_FAIL reason=%s\n",reason); std::exit(1); }
}
using Factory = id (*)(id,SEL,id,id);
using Pipeline = id (*)(id,SEL,id,NSError**);
using Encode = void (*)(id,SEL,id);
Factory originalFactory;
Pipeline originalPipeline;
Encode originalEncode;
Method encodeMethod;
unsigned creations = 0, pipelines = 0;
id zeroPipeline = nil;
using BindPipeline = void (*)(id,SEL,id);
BindPipeline originalBind = nullptr;
Method bindMethod = nullptr;
unsigned zeroDispatches = 0;
void observeBind(id self, SEL selector, id pipeline) {
  if (pipeline == zeroPipeline && pipeline) ++zeroDispatches;
  originalBind(self,selector,pipeline);
}
std::vector<id> encoded;
std::vector<bool> resets;
void observeEncode(id self, SEL selector, id command) {
  encoded.push_back(self);
  resets.push_back(((id<MTL4FXTemporalDenoisedScaler>)self).shouldResetHistory);
  originalEncode(self,selector,command);
}
id observeFactory(id self, SEL selector, id device, id compiler) {
  id result = originalFactory(self,selector,device,compiler);
  if (result) {
    ++creations;
    auto method = class_getInstanceMethod(object_getClass(result),@selector(encodeToCommandBuffer:));
    require(method != nullptr,"encode-observer");
    if (!encodeMethod) {
      encodeMethod = method;
      originalEncode = reinterpret_cast<Encode>(method_setImplementation(method,reinterpret_cast<IMP>(observeEncode)));
    } else require(method == encodeMethod,"same-native-scaler-class");
  }
  return result;
}
id observePipeline(id self, SEL selector, id function, NSError **error) {
  if ([[(id<MTLFunction>)function name] hasPrefix:@"rr_"]) ++pipelines;
  id pipeline = originalPipeline(self,selector,function,error);
  if ([[(id<MTLFunction>)function name] isEqualToString:@"rr_zero_albedo"]) zeroPipeline = pipeline;
  return pipeline;
}
struct Segments {
  id<MTLDevice> device;
  id<MTL4CommandAllocator> allocator;
  id<MTLResidencySet> residency;
  std::vector<id<MTL4CommandBuffer>> buffers;
  explicit Segments(id<MTLDevice> d, id<MTLResidencySet> r) : device(d), allocator([d newCommandAllocator]), residency(r) {
    require(allocator != nil,"allocator"); next(this);
  }
  static void *next(void *context) {
    auto &self = *static_cast<Segments*>(context);
    if (!self.buffers.empty()) [self.buffers.back() endCommandBuffer];
    auto buffer = [self.device newCommandBuffer]; require(buffer != nil,"command-buffer");
    self.buffers.push_back(buffer);
    [buffer beginCommandBufferWithAllocator:self.allocator]; [buffer useResidencySet:self.residency];
    return buffer;
  }
  ~Segments() { for (auto buffer : buffers) [buffer release]; [allocator release]; }
};
std::vector<float> run(id<MTLDevice> device, id<MTL4CommandQueue> queue,
    const std::shared_ptr<Feature> &feature, unsigned width, unsigned height, unsigned index) {
  TextureSet inputs{};
  std::vector<Object> owners;
  auto descriptor = [MTLResidencySetDescriptor new]; descriptor.initialCapacity = 8;
  auto residency = [device newResidencySetWithDescriptor:descriptor error:nil]; [descriptor release];
  require(residency != nil,"residency");
  for (unsigned slot : {0u,1u,2u,4u,7u,8u}) {
    bool depth = slot == 0;
    auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        depth ? MTLPixelFormatR32Float : MTLPixelFormatRGBA32Float width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    auto texture = [device newTextureWithDescriptor:td]; require(texture != nil,"texture");
    owners.emplace_back(texture); [texture release]; inputs[slot] = texture;
    const size_t channels = depth ? 1 : 4;
    std::vector<float> pixels(size_t(width)*height*channels);
    for (size_t p = 0; p < size_t(width)*height; ++p) {
      float *value = pixels.data()+p*channels;
      if (depth) { value[0] = 0.5f; continue; }
      if (slot == 2) { value[0] = value[1] = value[2] = 0.5f; value[3] = 1; }
      if (slot == 4) value[0] = value[1] = value[2] = 1;
      if (slot == 7) {
        value[0] = 0.25f + float(p%width%8)*0.005f;
        value[1] = 0.35f; value[2] = 0.45f; value[3] = 0.75f;
      }
      if (slot == 8) value[3] = 0.75f;
    }
    [texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:pixels.data()
        bytesPerRow:width*channels*sizeof(float)];
    [residency addAllocation:texture];
  }
  [residency commit]; [residency requestResidency];
  WMTRRFrame frame{}; frame.width=width; frame.height=height; frame.frame_index=index;
  frame.flags=2; frame.depth_max=100;
  frame.motion_scale[0]=frame.motion_scale[1]=frame.motion_scale[2]=1;
  for (unsigned i=0;i<4;++i) frame.view[5*i]=frame.projection[5*i]=1;
  WMTRRSettings settings{}; settings.normal_strength=1; settings.stability_bias=0.5f;
  settings.max_radiance=65504; settings.radiance_clip_std_k=3; settings.gaussian_relaxation=0.5f;
  settings.disocclusion_threshold=0.01f; settings.debug_depth_max=100; settings.history_rejection_strength=0.5f;
  Error error;
  auto prepared=feature->prepare(frame,inputs,settings,&error);
  if (!prepared) std::fprintf(stderr,"prepare: %s\n",error.message.c_str());
  require(bool(prepared),"prepare");
  Segments commands(device,residency);
  auto fence=[device newFence]; require(fence != nil,"fence");
  auto initial=[commands.buffers.back() computeCommandEncoder];
  if (!bindMethod) {
    bindMethod = class_getInstanceMethod(object_getClass(initial),@selector(setComputePipelineState:));
    require(bindMethod != nullptr,"pipeline-bind-observer");
    originalBind = reinterpret_cast<BindPipeline>(method_setImplementation(bindMethod,reinterpret_cast<IMP>(observeBind)));
  }
  [initial updateFence:fence afterEncoderStages:MTLStageDispatch]; [initial endEncoding];
  std::shared_ptr<const ExecutionLease> lease;
  const bool accepted=prepared->encode(commands.buffers.back(),fence,lease,Segments::next,&commands,&error);
  if (!accepted) std::fprintf(stderr,"encode: %s\n",error.message.c_str());
  require(accepted && lease,"encode");
  [commands.buffers.back() endCommandBuffer];
  auto completed=[device newSharedEvent];
  [queue commit:commands.buffers.data() count:commands.buffers.size()];
  [queue signalEvent:completed value:1];
  require([completed waitUntilSignaledValue:1 timeoutMS:20000],"gpu-completion");
  std::vector<float> output(size_t(width)*height*4);
  [(id<MTLTexture>)inputs[8] getBytes:output.data() bytesPerRow:width*4*sizeof(float)
      fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
  for (float value : output) require(std::isfinite(value),"finite-output");
  require(output[0]>0.1f && output[0]<1,"nonzero-radiance-output");
  if (const char* path = std::getenv("OWNED_RR_CAPTURE")) {
    auto file = std::fopen(path,"ab"); require(file != nullptr,"capture-open");
    const uint32_t header[] = {width,height,index};
    require(std::fwrite(header,sizeof(header),1,file) == 1 &&
            std::fwrite(output.data(),sizeof(float),output.size(),file) == output.size(),"capture-write");
    require(std::fclose(file) == 0,"capture-close");
  }
  lease.reset(); prepared.reset();
  [completed release]; [fence release]; [residency endResidency]; [residency release];
  return output;
}
}
int main() { @autoreleasepool {
  auto device=MTLCreateSystemDefaultDevice(); require(device != nil,"device");
  auto cd=[MTL4CompilerDescriptor new]; auto compiler=[device newCompilerWithDescriptor:cd error:nil]; [cd release];
  auto queue=[device newMTL4CommandQueue]; require(compiler && queue,"compiler-queue");
  auto factoryMethod=class_getInstanceMethod([MTLFXTemporalDenoisedScalerDescriptor class],
      @selector(newTemporalDenoisedScalerWithDevice:compiler:));
  auto pipelineMethod=class_getInstanceMethod(object_getClass(device),@selector(newComputePipelineStateWithFunction:error:));
  require(factoryMethod && pipelineMethod,"observer-methods");
  originalFactory=reinterpret_cast<Factory>(method_setImplementation(factoryMethod,reinterpret_cast<IMP>(observeFactory)));
  originalPipeline=reinterpret_cast<Pipeline>(method_setImplementation(pipelineMethod,reinterpret_cast<IMP>(observePipeline)));
  WMTRRCreateInfo info{}; info.version=0x00402000; info.width=160; info.height=80; info.signals=2;
  Error error; auto feature=Feature::create(device,compiler,info,&error);
  require(bool(feature) && pipelines==EXPECT_RR_PIPELINES,"first-feature-pipelines");
  auto independent=Feature::create(device,compiler,info,&error);
  require(bool(independent) && pipelines==EXPECT_RR_PIPELINES,"second-feature-shares-pipelines");
  auto a=run(device,queue,feature,128,64,0);
  run(device,queue,feature,160,80,1);
  auto restored=run(device,queue,feature,128,64,2);
  run(device,queue,feature,128,64,3);
  require(creations==2 && encoded.size()==4 && encoded[0]==encoded[2] && encoded[0]==encoded[3] &&
      encoded[0]!=encoded[1],"two-models-a-b-a-reuse");
  require(resets==std::vector<bool>({true,true,true,false}),"switch-reset-consecutive-history");
  require(a==restored,"restored-reset-bit-exact");
  auto fresh=run(device,queue,independent,128,64,0);
  require(creations==3 && fresh==a && encoded.back()!=encoded[0],"feature-history-isolation");
  feature->markDormant();
  auto dormant=run(device,queue,feature,128,64,4);
  require(creations==4 && resets.back() && dormant==a,"dormant-evicts-models-and-history");
  require(EXPECT_RR_PIPELINES == 10 ? zeroDispatches == 0 : zeroDispatches == creations,
      "one-gpu-zero-clear-per-generation");
  std::printf("AMD_RR_CACHE_PASS features=2 shared_psos=%u extents=A,B,A,A models_before_dormant=3 switch_resets=1 consecutive_reuse=1 restored_output=bit_exact dormant_fresh=1 gpu_zero_initializations=%u\n",pipelines,zeroDispatches);
  feature.reset(); independent.reset();
  method_setImplementation(factoryMethod,reinterpret_cast<IMP>(originalFactory));
  method_setImplementation(pipelineMethod,reinterpret_cast<IMP>(originalPipeline));
  if (encodeMethod) method_setImplementation(encodeMethod,reinterpret_cast<IMP>(originalEncode));
  if (bindMethod) method_setImplementation(bindMethod,reinterpret_cast<IMP>(originalBind));
  [queue release]; [compiler release]; [device release];
} }
