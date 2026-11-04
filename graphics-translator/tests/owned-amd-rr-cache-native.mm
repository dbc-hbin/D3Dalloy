// Native AMD RR cache proof using the production provider, not a mock scaler.
#ifndef OWNED_RR_PROVIDER_SOURCE
#define OWNED_RR_PROVIDER_SOURCE "../../src/winemetal/unix/metalfx_amd_rr.mm"
#endif
#include OWNED_RR_PROVIDER_SOURCE
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <set>
#ifndef EXPECT_RR_PIPELINES
#define EXPECT_RR_PIPELINES 11
#endif
#ifndef EXPECT_RR_WORKING_SETS
#define EXPECT_RR_WORKING_SETS 1
#endif
#ifndef EXPECT_SCALAR_FUSION
#define EXPECT_SCALAR_FUSION 1
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
id zeroPipeline = nil, scalarWritePipeline = nil, resolveWritePipeline = nil;
using BindPipeline = void (*)(id,SEL,id);
BindPipeline originalBind = nullptr;
Method bindMethod = nullptr;
unsigned zeroDispatches = 0;
unsigned scalarWrites = 0, scalarResolves = 0;
void observeBind(id self, SEL selector, id pipeline) {
  if (pipeline == zeroPipeline && pipeline) ++zeroDispatches;
  if (pipeline == scalarWritePipeline && pipeline) ++scalarWrites;
  if (pipeline == resolveWritePipeline && pipeline) ++scalarResolves;
  originalBind(self,selector,pipeline);
}
std::vector<id> encoded;
std::vector<bool> resets;
struct EncodedScratch {
  uintptr_t color, output, reactive, strength;
  uint64_t bytes;
};
std::vector<EncodedScratch> scratch;
void observeEncode(id self, SEL selector, id command) {
  encoded.push_back(self);
  auto scaler = (id<MTL4FXTemporalDenoisedScaler>)self;
  resets.push_back(scaler.shouldResetHistory);
  scratch.push_back({reinterpret_cast<uintptr_t>(scaler.colorTexture),
      reinterpret_cast<uintptr_t>(scaler.outputTexture),reinterpret_cast<uintptr_t>(scaler.reactiveMaskTexture),
      reinterpret_cast<uintptr_t>(scaler.denoiseStrengthMaskTexture),
      scaler.colorTexture.allocatedSize+scaler.outputTexture.allocatedSize+
          scaler.reactiveMaskTexture.allocatedSize+scaler.denoiseStrengthMaskTexture.allocatedSize});
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
  if ([[(id<MTLFunction>)function name] isEqualToString:@"rr_filter_write"]) scalarWritePipeline = pipeline;
  if ([[(id<MTLFunction>)function name] isEqualToString:@"rr_resolve_write"]) resolveWritePipeline = pipeline;
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

struct PendingFrame {
  TextureSet textures{};
  std::vector<Object> owners;
  Object residency, fence;
  std::unique_ptr<Segments> commands;
  std::shared_ptr<const PreparedFrame> prepared;
  std::shared_ptr<const ExecutionLease> lease;
  size_t scratchStart = 0;
  uint64_t memory = 0;
  double encodeUs = 0;
};
using Clock = std::chrono::steady_clock;
double elapsedUs(Clock::time_point start) {
  return std::chrono::duration<double,std::micro>(Clock::now()-start).count();
}
void complete(id<MTLCommandBuffer> command) {
  [command commit]; [command waitUntilCompleted];
  require(command.status == MTLCommandBufferStatusCompleted,"working-set-copy-completion");
}
std::unique_ptr<PendingFrame> recordWorkingSet(id<MTLDevice> device, id<MTLCommandQueue> uploadQueue,
    const std::shared_ptr<Feature>& feature, unsigned width, unsigned height, unsigned index,
    unsigned checkerboard, bool alias) {
  auto pending = std::make_unique<PendingFrame>();
  auto descriptor = [MTLResidencySetDescriptor new]; descriptor.initialCapacity = WMTRR_RESOURCE_COUNT;
  auto residency = [device newResidencySetWithDescriptor:descriptor error:nil]; [descriptor release];
  require(residency != nil,"working-set-residency");
  pending->residency = Object(residency); [residency release];
  auto upload = [uploadQueue commandBuffer]; auto blit = [upload blitCommandEncoder];
  std::vector<Object> uploads;
  for (unsigned slot=0;slot<WMTRR_FUSED_ALBEDO;++slot) {
    const bool depth = slot == 0, output = slot >= 5 && ((slot-5)&1);
    const unsigned signal = slot >= 5 ? (slot-5)/2 : 0;
    auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        depth ? MTLPixelFormatR32Float : MTLPixelFormatRGBA32Float width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    auto texture = [device newTextureWithDescriptor:td]; require(texture != nil,"working-set-texture");
    pending->owners.emplace_back(texture); [texture release]; pending->textures[slot] = texture;
    [residency addAllocation:texture];
    const size_t channels = depth ? 1 : 4;
    const size_t pitch = (size_t(width)*channels*sizeof(float)+255)&~size_t(255);
    std::vector<float> pixels(pitch/sizeof(float)*height);
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
      float *value = pixels.data()+y*pitch/sizeof(float)+x*channels;
      if (depth) { value[0] = 0.5f; continue; }
      if (slot == 2) {
        value[0] = value[1] = value[2] = 0.5f;
        // On frame 1, checkerboard holes have no same-material traced
        // neighbors. Their alpha must come from the matching prior signal.
        value[3] = checkerboard && index == 1 && ((x&1) == ((y&1)^1u)) ? 1 : 0;
      }
      if (slot == 3 || slot == 4) {
        value[0] = 0.75f; value[1] = 0.8f; value[2] = 0.9f; value[3] = 1;
      }
      if (slot >= 5) {
        value[0] = value[1] = value[2] = output ? 0 :
            signal == 3 ? 65504.f : float(signal+1)/8 + float((x+index)%8)*0.002f;
        value[3] = output ? float(20+signal) : float(2+2*signal)+float(x%9)*0.03f;
      }
    }
    auto buffer = [device newBufferWithBytes:pixels.data() length:pixels.size()*sizeof(float)
        options:MTLResourceStorageModeShared]; require(buffer != nil,"working-set-upload");
    uploads.emplace_back(buffer); [buffer release];
    [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch*height
        sourceSize:MTLSizeMake(width,height,1) toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0,0,0)];
  }
  [blit endEncoding]; complete(upload);
  if (alias) {
    // Output views overwrite normal, motion and both albedos before later
    // radiance prepares. ID also aliases the later IS input. Views exercise
    // root-based detection rather than only equal texture handles.
    for (auto pair : {std::pair{6u,2u},{8u,1u},{10u,3u},{12u,4u},{14u,15u}}) {
      auto parent = (id<MTLTexture>)pending->textures[pair.second];
      auto view = [parent newTextureViewWithPixelFormat:parent.pixelFormat];
      require(view != nil,"working-set-alias-view");
      pending->owners.emplace_back(view); [view release]; pending->textures[pair.first] = view;
      [residency addAllocation:view];
    }
  }
  [residency commit]; [residency requestResidency];
  WMTRRFrame frame{}; frame.width=width; frame.height=height; frame.frame_index=index;
  frame.flags=2; frame.depth_max=100; frame.light_direction[2]=-1;
  frame.light_emission[0]=frame.light_emission[1]=frame.light_emission[2]=1;
  frame.light_angular_radius=0.01f;
  frame.motion_scale[0]=frame.motion_scale[1]=frame.motion_scale[2]=1;
  for (unsigned i=0;i<4;++i) frame.view[5*i]=frame.projection[5*i]=1;
  for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) if (checkerboard&(1u<<s)) frame.checkerboard_origin[s]=index&1;
  auto settings=WMTRRDefaultSettings(); settings.normal_strength=1;
  Error error;
  pending->prepared=feature->prepare(frame,pending->textures,settings,&error);
  if (!pending->prepared) std::fprintf(stderr,"working-set prepare: %s\n",error.message.c_str());
  require(bool(pending->prepared),"working-set-prepare");
  pending->commands=std::make_unique<Segments>(device,residency);
  auto fence=[device newFence]; require(fence != nil,"working-set-fence");
  pending->fence=Object(fence); [fence release];
  auto initial=[pending->commands->buffers.back() computeCommandEncoder];
  if (!bindMethod) {
    bindMethod = class_getInstanceMethod(object_getClass(initial),@selector(setComputePipelineState:));
    require(bindMethod != nullptr,"working-set-pipeline-observer");
    originalBind = reinterpret_cast<BindPipeline>(method_setImplementation(bindMethod,reinterpret_cast<IMP>(observeBind)));
  }
  [initial updateFence:fence afterEncoderStages:MTLStageDispatch]; [initial endEncoding];
  pending->scratchStart=scratch.size();
  const auto start=Clock::now();
  const unsigned writesBefore=scalarWrites, resolvesBefore=scalarResolves;
  const bool accepted=pending->prepared->encode(pending->commands->buffers.back(),fence,pending->lease,
      Segments::next,pending->commands.get(),&error);
  pending->encodeUs=elapsedUs(start); pending->memory=feature->memoryUsage();
  if (!accepted) std::fprintf(stderr,"working-set encode: %s\n",error.message.c_str());
  require(accepted && pending->lease,"working-set-encode");
  const unsigned expectedWrites=EXPECT_SCALAR_FUSION ? (alias ? 1 : 3) : 0;
  require(scalarWrites-writesBefore==expectedWrites && scalarResolves-resolvesBefore==3-expectedWrites,
      "scalar-filter-resolve-selection");
  std::printf("AMD_RR_SCALAR_FUSION frame=%u aliases=%u fused=%u resolves=%u\n",
      index,unsigned(alias),scalarWrites-writesBefore,scalarResolves-resolvesBefore);
  [pending->commands->buffers.back() endCommandBuffer];
  require(scratch.size()==pending->scratchStart+4,"four-independent-neural-signals");
  for (auto member : {&EncodedScratch::color,&EncodedScratch::output,
                     &EncodedScratch::reactive,&EncodedScratch::strength}) {
    std::set<uintptr_t> unique;
    for (size_t i=pending->scratchStart;i<scratch.size();++i) unique.insert(scratch[i].*member);
    require(unique.size()==EXPECT_RR_WORKING_SETS,"expected-signal-transient-sets");
  }
  return pending;
}
std::vector<float> readWorkingSet(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLTexture> texture, unsigned width, unsigned height) {
  const size_t pitch=(size_t(width)*16+255)&~size_t(255);
  auto readback=[device newBufferWithLength:pitch*height options:MTLResourceStorageModeShared];
  require(readback != nil,"working-set-readback");
  auto command=[queue commandBuffer]; auto encoder=[command blitCommandEncoder];
  [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
      sourceSize:MTLSizeMake(width,height,1) toBuffer:readback destinationOffset:0
      destinationBytesPerRow:pitch destinationBytesPerImage:pitch*height];
  [encoder endEncoding]; complete(command);
  std::vector<float> result(size_t(width)*height*4);
  for (unsigned y=0;y<height;++y)
    std::memcpy(result.data()+size_t(y)*width*4,(const char*)readback.contents+y*pitch,width*16);
  [readback release];
  for (float value:result) require(std::isfinite(value),"working-set-finite-output");
  return result;
}
void workingSet(id<MTLDevice> device, id<MTL4Compiler> compiler, id<MTL4CommandQueue> queue) {
  auto uploadQueue=[device newCommandQueue]; require(uploadQueue != nil,"working-set-copy-queue");
  for (unsigned scenario=0;scenario<3;++scenario) {
    constexpr unsigned width=128,height=64;
    const unsigned checkerboard=scenario ? 0x36 : 0;
    const bool alias=scenario==2;
    WMTRRCreateInfo info{}; info.version=0x00402000; info.width=width; info.height=height;
    info.signals=0x7f; info.checkerboard=checkerboard;
    Error error; auto feature=Feature::create(device,compiler,info,&error);
    require(bool(feature),"working-set-feature");
    auto first=recordWorkingSet(device,uploadQueue,feature,width,height,0,checkerboard,alias);
    auto second=recordWorkingSet(device,uploadQueue,feature,width,height,1,checkerboard,alias);
    for (unsigned a=0;a<4;++a) for (unsigned b=0;b<4;++b)
      require(scratch[first->scratchStart+a].color != scratch[second->scratchStart+b].color,
          "unretired-executions-have-distinct-working-sets");
    for (unsigned s=0;s<4;++s)
      require(resets[first->scratchStart+s] && !resets[second->scratchStart+s],"per-signal-consecutive-history");
    const uint64_t retained=feature->memoryUsage();
    feature->markDormant();
    require(feature->memoryUsage()==retained,"dormant-keeps-unretired-working-set");
    std::vector<id<MTL4CommandBuffer>> buffers=first->commands->buffers;
    buffers.insert(buffers.end(),second->commands->buffers.begin(),second->commands->buffers.end());
    auto completed=[device newSharedEvent]; const auto start=Clock::now();
    [queue commit:buffers.data() count:buffers.size()]; [queue signalEvent:completed value:1];
    require([completed waitUntilSignaledValue:1 timeoutMS:20000],"working-set-gpu-completion");
    const double completionUs=elapsedUs(start); [completed release];
    std::array<std::array<std::vector<float>,WMTRR_SIGNAL_COUNT>,2> outputs;
    unsigned index=0;
    for (auto pending : {first.get(),second.get()}) {
      for (unsigned s=0;s<WMTRR_SIGNAL_COUNT;++s) {
        auto& values=outputs[index][s];
        values=readWorkingSet(device,uploadQueue,(id<MTLTexture>)pending->textures[6+2*s],width,height);
        if (!checkerboard && radiance(s))
          require(values[3]==float(20+s),"independent-destination-alpha");
        if (const char* path=std::getenv("OWNED_RR_CAPTURE")) {
          auto file=std::fopen(path,"ab"); require(file != nullptr,"working-set-capture-open");
          const uint32_t header[]={scenario,index,s,width,height};
          require(std::fwrite(header,sizeof(header),1,file)==1 &&
              std::fwrite(values.data(),sizeof(float),values.size(),file)==values.size(),"working-set-capture-write");
          require(std::fclose(file)==0,"working-set-capture-close");
        }
      }
      std::printf("AMD_RR_WORKING_SET_FRAME scenario=%u index=%u checkerboard=%u aliases=%u sets=%u bytes=%llu segments=%zu encode_us=%.3f\n",
          scenario,index,checkerboard,unsigned(alias),unsigned(EXPECT_RR_WORKING_SETS),
          (unsigned long long)pending->memory,pending->commands->buffers.size(),pending->encodeUs);
      ++index;
    }
    if (checkerboard) {
      const size_t center=(size_t(height/2)*width+width/2)*4+3;
      for (unsigned s:{1u,2u})
        require(outputs[1][s][center]==outputs[0][s][center],"checkerboard-history-only-alpha");
      require(outputs[1][1][center]!=outputs[1][2][center],"checkerboard-signal-alpha-isolation");
    }
    first->lease.reset(); second->lease.reset();
    require(feature->memoryUsage()==0,"retirement-releases-dormant-working-sets");
    [(id<MTLResidencySet>)first->residency.get() endResidency];
    [(id<MTLResidencySet>)second->residency.get() endResidency];
    std::printf("AMD_RR_WORKING_SET_CASE scenario=%u submit_to_complete_us=%.3f dormant_retained_bytes=%llu retired_bytes=0\n",
        scenario,completionUs,(unsigned long long)retained);
  }
  [uploadQueue release];
  std::printf("AMD_RR_WORKING_SET_PASS cases=3 frames=6 signals=42 expected_sets=%u unretired_isolation=1 dormant=1 alpha_history=1\n",
      unsigned(EXPECT_RR_WORKING_SETS));
}
}
int main(int argc,char** argv) { @autoreleasepool {
  auto device=MTLCreateSystemDefaultDevice(); require(device != nil,"device");
  auto cd=[MTL4CompilerDescriptor new]; auto compiler=[device newCompilerWithDescriptor:cd error:nil]; [cd release];
  auto queue=[device newMTL4CommandQueue]; require(compiler && queue,"compiler-queue");
  auto factoryMethod=class_getInstanceMethod([MTLFXTemporalDenoisedScalerDescriptor class],
      @selector(newTemporalDenoisedScalerWithDevice:compiler:));
  auto pipelineMethod=class_getInstanceMethod(object_getClass(device),@selector(newComputePipelineStateWithFunction:error:));
  require(factoryMethod && pipelineMethod,"observer-methods");
  originalFactory=reinterpret_cast<Factory>(method_setImplementation(factoryMethod,reinterpret_cast<IMP>(observeFactory)));
  originalPipeline=reinterpret_cast<Pipeline>(method_setImplementation(pipelineMethod,reinterpret_cast<IMP>(observePipeline)));
  if (argc==2) {
    require(!std::strcmp(argv[1],"--working-set"),"known-probe-mode");
    workingSet(device,compiler,queue);
    method_setImplementation(factoryMethod,reinterpret_cast<IMP>(originalFactory));
    method_setImplementation(pipelineMethod,reinterpret_cast<IMP>(originalPipeline));
    if (encodeMethod) method_setImplementation(encodeMethod,reinterpret_cast<IMP>(originalEncode));
    if (bindMethod) method_setImplementation(bindMethod,reinterpret_cast<IMP>(originalBind));
    [queue release]; [compiler release]; [device release];
    return 0;
  }
  require(argc==1,"known-probe-arguments");
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
