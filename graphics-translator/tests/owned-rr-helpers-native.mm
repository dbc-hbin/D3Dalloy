// Differential RR helper oracle against an independently frozen kernel source.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct RrParams {
  uint32_t width, height, signal, checkerboard, origin, flags, hasHistory, legacyMode;
  float normalStrength, stability, maxRadiance, clipK, relaxation, depthThreshold, depthMin, depthMax;
  float motionScale[3], jitter[2], previousJitter[2], cameraDelta[3];
  float view[16], inverseProjection[16], inverseView[16];
  float lightDirection[3], lightEmission[3], lightRadius, historyStrength;
  uint32_t apiVersion;
  float projection[16], previousView[16];
};
static_assert(sizeof(RrParams) == 460);
struct Pixel { float x,y,z,w; };
static id<MTLDevice> gpu;
static id<MTLCommandQueue> commandQueue;
static id<MTLLibrary> libraries[2];
static id<MTLComputePipelineState> geometryPSO, unpackPSO, filterPSO[2], preparePSO[2], resolvePSO, fusedPSO;
static id<MTLComputePipelineState> commonPSO[2];
static uint64_t checkedComponents = 0, nonfiniteComponents = 0, failures = 0, checkedImages = 0;
static size_t pixelBytes(MTLPixelFormat format) {
  switch (format) {
    case MTLPixelFormatR8Unorm: return 1;
    case MTLPixelFormatR16Float: return 2;
    case MTLPixelFormatR32Float: return 4;
    case MTLPixelFormatRGBA16Float: return 8;
    default: return 16;
  }
}
static id<MTLTexture> texture(unsigned width,unsigned height,MTLPixelFormat format = MTLPixelFormatRGBA32Float) {
  auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
  d.storageMode=MTLStorageModeShared; d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
  auto t=[gpu newTextureWithDescriptor:d]; if(!t) std::abort();
  std::vector<uint8_t> initial(width*height*pixelBytes(format),0x23);
  [t replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:initial.data() bytesPerRow:width*pixelBytes(format)];
  return t;
}
static void upload(id<MTLTexture> t,const std::vector<Pixel>& data) {
  [t replaceRegion:MTLRegionMake2D(0,0,t.width,t.height) mipmapLevel:0 withBytes:data.data() bytesPerRow:t.width*sizeof(Pixel)];
}
static std::vector<uint8_t> read(id<MTLTexture> t) {
  std::vector<uint8_t> bytes(t.width*t.height*pixelBytes(t.pixelFormat));
  [t getBytes:bytes.data() bytesPerRow:t.width*pixelBytes(t.pixelFormat) fromRegion:MTLRegionMake2D(0,0,t.width,t.height) mipmapLevel:0];
  return bytes;
}
static void compare(const char *name,id<MTLTexture> a,id<MTLTexture> b,const RrParams& p) {
  ++checkedImages;
  auto x=read(a),y=read(b); size_t step=a.pixelFormat==MTLPixelFormatR8Unorm?1:
      (a.pixelFormat==MTLPixelFormatR16Float||a.pixelFormat==MTLPixelFormatRGBA16Float)?2:4;
  size_t bad=0;
  for(size_t i=0;i<x.size();i+=step) {
    ++checkedComponents;
    uint32_t v=0,w=0; std::memcpy(&v,x.data()+i,step); std::memcpy(&w,y.data()+i,step);
    uint32_t exponent=step==4?0x7f800000u:0x7c00u, mantissa=step==4?0x7fffffu:0x3ffu;
    bool nv=step!=1&&(v&exponent)==exponent&&(v&mantissa), nw=step!=1&&(w&exponent)==exponent&&(w&mantissa);
    if(nv&&nw){++nonfiniteComponents;continue;}
    if(step!=1&&(v&exponent)==exponent)++nonfiniteComponents;
    if(v!=w) {if(!bad)std::printf("RR_DIFF %s %ux%u signal=%u checker=%u history=%u offset=%zu before=%08x after=%08x\n",
        name,p.width,p.height,p.signal,p.checkerboard,p.hasHistory,i,v,w);++bad;}
  }
  failures+=bad;
}
static id<MTLComputePipelineState> pso(unsigned which,NSString *name) {
  NSError *error=nil; auto result=[gpu newComputePipelineStateWithFunction:[libraries[which] newFunctionWithName:name] error:&error];
  if(!result){std::fprintf(stderr,"%s\n",error.description.UTF8String);std::abort();}return result;
}
static void encode(id<MTLCommandBuffer> cb,id<MTLComputePipelineState> state,NSArray<id<MTLTexture>>* textures,const RrParams& p) {
  auto e=[cb computeCommandEncoder];[e setComputePipelineState:state];[e setBytes:&p length:sizeof(p) atIndex:0];
  for(NSUInteger i=0;i<textures.count;++i)[e setTexture:textures[i] atIndex:i];
  [e dispatchThreadgroups:MTLSizeMake((p.width+7)/8,(p.height+7)/8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
  [e endEncoding];
}
static void finish(id<MTLCommandBuffer> cb) {
  [cb commit];[cb waitUntilCompleted];if(cb.status==MTLCommandBufferStatusError){
    std::fprintf(stderr,"GPU error: %s\n",cb.error.description.UTF8String);std::abort();}
}
static void run(unsigned width,unsigned height,unsigned signal,unsigned variant,unsigned formatIndex,bool timing=false) {
  RrParams p{};p.width=width;p.height=height;p.signal=signal;p.checkerboard=variant&1;
  p.hasHistory=(variant>>1)&1;p.origin=variant&1;p.flags=variant&2;p.legacyMode=variant==3?3:0;
  p.normalStrength=0.37f;p.stability=0.61f;p.maxRadiance=1024;p.clipK=1.75f;
  p.relaxation=0.3f;p.depthThreshold=0.2f;p.depthMin=0.1f;p.depthMax=12;
  p.motionScale[0]=p.motionScale[1]=p.motionScale[2]=1;
  p.jitter[0]=0.25f;p.jitter[1]=-0.125f;p.previousJitter[0]=-0.125f;p.previousJitter[1]=0.25f;
  p.cameraDelta[0]=0.01f;p.lightDirection[0]=0.2f;p.lightDirection[1]=0.1f;p.lightDirection[2]=1;
  p.lightEmission[0]=2;p.lightEmission[1]=4;p.lightEmission[2]=1;p.lightRadius=0.1f;
  p.historyStrength=0.5f;p.apiVersion=variant&1?0x00400000:0x00400200;
  for(unsigned i=0;i<4;++i)p.view[5*i]=p.inverseProjection[5*i]=p.inverseView[5*i]=p.projection[5*i]=p.previousView[5*i]=1;
  const unsigned tw=width+3,th=height+2;
  std::array<id<MTLTexture>,11> input;
  for(auto& t:input)t=texture(tw,th);
  // 0 raw signal,1 depth,2 normals,3 motion,4 specular,5 diffuse,6 fused,
  // 7 history,8 previous depth,9 previous normal,10 previous alpha.
  std::array<std::vector<Pixel>,11> pixels;
  for(auto& data:pixels)data.resize(size_t(tw)*th);
  for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x){
    size_t i=size_t(y)*tw+x;unsigned k=x+17*y;
    float depth=0.5f+float(k%13)*0.03f;
    pixels[0][i]={signal==3?(k%7?0.01f*float(k%13):65504.0f):float(k%19)/13.0f,float(k%11)/7.0f,float(k%29)/9.0f,k%5?float(k%17)*0.1f:-1.0f};
    pixels[1][i]={depth,0,0,0};pixels[2][i]={0.43f+0.01f*(k%9),0.52f-0.01f*(k%7),0.1f+0.1f*(k%9),float((x/5+y/3)%4)/3};
    pixels[3][i]={0.17f/width,-0.23f/height,k%3?0:0.03f,0};
    pixels[4][i]={float(k%7)/9,float(k%5)/7,float(k%11)/13,0.75f};
    pixels[5][i]={float(k%11)/13,float(k%13)/15,float(k%7)/9,float(k%5)/4};
    pixels[6][i]={0.2f+float(k%7)/11,0.3f,0.7f,0.2f};
    pixels[7][i]={float(k%17)/19,float(k%19)/21,float(k%23)/27,k%11?-0.0f:-1};
    pixels[8][i]={depth+(k%3?0:0.01f),0,0,0};pixels[9][i]=pixels[2][i];pixels[10][i]={0,0,0,float(k%23)/5};
    if(!timing){
      if(k%31==0)pixels[1][i].x=-0.0f;
      if(k%43==0)pixels[1][i].x=INFINITY;
      if(k%47==0)pixels[1][i].x=std::bit_cast<float>(0x7fc00001u);
      if(k%53==0)pixels[0][i].x=-0.0f;
      if(k%59==0)pixels[0][i].y=-INFINITY;
      if(k%61==0)pixels[0][i].z=std::bit_cast<float>(0x7fc00041u);
    }
  }
  for(unsigned i=0;i<input.size();++i)upload(input[i],pixels[i]);
  auto source=texture(tw,th),depth=texture(tw,th),normals=texture(tw,th),guide=texture(tw,th),decoded=texture(tw,th);
  auto prep=[commandQueue commandBuffer];
  encode(prep,geometryPSO,@[input[1],input[2],depth,normals,guide,decoded],p);
  encode(prep,unpackPSO,@[input[0],source],p);finish(prep);
  if(signal==5&&!timing) {
    // The candidate produces both the snapshots/geometry and the neural common guides.
    std::array<id<MTLTexture>,9> before,after;
    before[0]=depth;before[1]=normals;before[2]=guide;before[3]=decoded;
    for(unsigned i=0;i<before.size();++i) {
      if(i>=4)before[i]=texture(tw,th);
      after[i]=texture(tw,th);
    }
    auto common=[commandQueue commandBuffer];
    encode(common,commonPSO[0],@[depth,input[3],normals,input[4],input[5],before[4],before[5],source,
                                 before[6],before[7],before[8]],p);
    encode(common,commonPSO[1],@[input[1],input[3],input[2],input[4],input[5],after[4],after[5],source,
                                 after[6],after[7],after[8],after[0],after[1],after[2],after[3]],p);
    finish(common);
    for(unsigned i=0;i<before.size();++i)compare("geometry-common",before[i],after[i],p);
  }
  auto first=texture(tw,th),second=texture(tw,th),fusedHistory=texture(tw,th);
  const MTLPixelFormat formats[]={MTLPixelFormatRGBA32Float,MTLPixelFormatR32Float,MTLPixelFormatR16Float,MTLPixelFormatRGBA16Float,MTLPixelFormatR8Unorm};
  auto callerBefore=texture(tw,th,formats[formatIndex%5]),callerAfter=texture(tw,th,formats[formatIndex%5]);
  NSArray<id<MTLTexture>>* beforeBindings=@[source,first,depth,input[3],normals,input[4],input[5],input[7],input[8],input[9],input[6],guide,decoded];
  NSArray<id<MTLTexture>>* afterBindings=@[source,second,depth,input[3],normals,input[4],input[5],input[7],input[8],input[9],input[6],guide,decoded];
  NSArray<id<MTLTexture>>* fusedBindings=@[source,fusedHistory,depth,input[3],normals,input[4],input[5],input[7],input[8],input[9],input[6],guide,decoded,callerAfter];
  bool scalar=signal==0||signal==3||signal==6;
  auto cb=[commandQueue commandBuffer];encode(cb,filterPSO[0],beforeBindings,p);encode(cb,filterPSO[1],afterBindings,p);
  if(scalar){encode(cb,resolvePSO,@[first,callerBefore,source,source],p);encode(cb,fusedPSO,fusedBindings,p);}
  std::array<std::array<id<MTLTexture>,7>,2> outputs;
  if(!scalar&&!timing)for(unsigned version=0;version<2;++version){
    for(auto& t:outputs[version])t=texture(tw,th);
    auto& o=outputs[version];
    encode(cb,preparePSO[version],@[source,depth,input[3],normals,input[4],input[5],input[6],input[8],input[9],input[7],
        o[0],o[1],o[2],o[3],o[4],o[5],o[6],input[10],guide,decoded],p);
  }
  finish(cb);compare("filter",first,second,p);
  if(scalar){compare("fused-history",first,fusedHistory,p);compare("fused-caller",callerBefore,callerAfter,p);}
  else if(!timing)for(unsigned i=0;i<7;++i)compare("neural-prepare",outputs[0][i],outputs[1][i],p);
  if(timing){
    std::array<std::vector<double>,2> samples;
    for(unsigned sample=0;sample<12;++sample)for(unsigned turn=0;turn<2;++turn){
      unsigned which=(sample+turn)&1;auto batch=[commandQueue commandBuffer];
      for(unsigned repeat=0;repeat<3;++repeat){
        if(which)encode(batch,fusedPSO,fusedBindings,p);
        else {encode(batch,filterPSO[0],beforeBindings,p);encode(batch,resolvePSO,@[first,callerBefore,source,source],p);}
      }
      finish(batch);double ms=(batch.GPUEndTime-batch.GPUStartTime)*1000/3;if(sample>=3)samples[which].push_back(ms);
      std::printf("RR_SAMPLE sample=%u version=%u gpu_ms=%.9f\n",sample,which,ms);
    }
    for(auto& s:samples)std::sort(s.begin(),s.end());
    std::printf("RR_TIMING width=%u height=%u signal=%u baseline_ms=%.9f candidate_ms=%.9f ratio=%.9f\n",
        width,height,signal,samples[0][4],samples[1][4],samples[1][4]/samples[0][4]);
  }
}
int main(int argc,char** argv){@autoreleasepool{
  if(argc<3)return 2;gpu=MTLCreateSystemDefaultDevice();commandQueue=[gpu newCommandQueue];
  for(unsigned i=0;i<2;++i){
    NSError* error=nil;NSString* source=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[i+1]] encoding:NSUTF8StringEncoding error:&error];
    // Match deviceKernelLibrary in the provider, including its default math mode.
    libraries[i]=[gpu newLibraryWithSource:source options:nil error:&error];
    if(!libraries[i]){std::fprintf(stderr,"%s\n",error.description.UTF8String);return 2;}
    filterPSO[i]=pso(i,@"rr_filter");preparePSO[i]=pso(i,@"rr_neural_prepare");commonPSO[i]=pso(i,@"rr_neural_common");
  }
  geometryPSO=pso(0,@"rr_geometry");unpackPSO=pso(0,@"rr_unpack");resolvePSO=pso(0,@"rr_resolve_write");fusedPSO=pso(1,@"rr_filter_write");
  bool timing=argc>3&&!std::strcmp(argv[3],"--timing");
  if(timing)run(1920,1080,6,2,2,true);
  else {unsigned index=0;for(auto extent:std::array<std::array<unsigned,2>,5>{{{1,1},{1,17},{17,1},{19,13},{31,29}}})
    for(unsigned signal=0;signal<7;++signal)for(unsigned variant=0;variant<4;++variant)run(extent[0],extent[1],signal,variant,index++%5);}
  std::printf("RR_HELPER_RESULT images=%llu components=%llu nonfinite=%llu failures=%llu\n",checkedImages,checkedComponents,nonfiniteComponents,failures);
  return failures?1:0;
}}
