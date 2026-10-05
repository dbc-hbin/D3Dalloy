// Run scripts/test-fp64.sh. CPU arithmetic is independent hardware binary64;
// the same exact integer helper AIR is exercised by real compute and fragment PSOs.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "../../src/airconv/shaders/air_fp64.cpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <limits>

static std::uint64_t bits(double x) { return std::bit_cast<std::uint64_t>(x); }
static double value(std::uint64_t x) { return std::bit_cast<double>(x); }
static bool nan(std::uint64_t x) { return (x & 0x7fffffffffffffffULL)>0x7ff0000000000000ULL; }
static std::uint64_t integer(double x, bool sign, unsigned width) {
  if(std::isnan(x)) return 0;
  if(sign) {
    if(width==32) {
      if(x>=2147483648.0) return 2147483647;
      if(x<=-2147483648.0) return static_cast<std::uint64_t>(INT32_MIN);
      return static_cast<std::uint64_t>(static_cast<std::int32_t>(x));
    }
    if(x>=0x1p63) return INT64_MAX;
    if(x<=-0x1p63) return static_cast<std::uint64_t>(INT64_MIN);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(x));
  }
  if(x<=0) return 0;
  if(width==32) return x>=0x1p32 ? UINT32_MAX : static_cast<std::uint32_t>(x);
  return x>=0x1p64 ? UINT64_MAX : static_cast<std::uint64_t>(x);
}
int main(int argc, char **argv) { @autoreleasepool {
  if(argc!=2) { std::fprintf(stderr,"usage: fp64-oracle library.metallib\n"); return 2; }
  constexpr unsigned count=32768, ops=22;
  std::mt19937_64 random(0xF064);
  const std::uint64_t edges[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,0x000fffffffffffffULL,0x0010000000000000ULL,0x3ff0000000000000ULL,0x3ff0000000000001ULL,0x7fefffffffffffffULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000123ULL,0x7ff0000000000001ULL,0x41e0000000000000ULL,0xc1e0000000000000ULL,0x43e0000000000000ULL,0xc3e0000000000000ULL,0x43f0000000000000ULL};
  constexpr unsigned edgeCount=sizeof(edges)/sizeof(edges[0]);
  std::vector<std::uint64_t> input(count*3), expected(count*ops);
  unsigned cpuChecks=0;
  for(unsigned i=0;i<count;++i) {
    std::uint64_t a=i<edgeCount*edgeCount?edges[i/edgeCount]:random();
    std::uint64_t b=i<edgeCount*edgeCount?edges[i%edgeCount]:random(), c=random();
    input[i*3]=a;input[i*3+1]=b;input[i*3+2]=c;
    double x=value(a),y=value(b),z=value(c);
    float f=static_cast<float>(x), from=std::bit_cast<float>(static_cast<std::uint32_t>(a));
    auto *e=expected.data()+i*ops;
    e[0]=bits(x+y);e[1]=bits(x-y);e[2]=bits(x*y);e[3]=bits(x/y);e[4]=bits(std::fmod(x,y));e[5]=bits(std::fma(x,y,z));e[6]=bits(std::sqrt(x));
    e[7]=bits(std::nearbyint(x));e[8]=bits(std::trunc(x));e[9]=bits(std::floor(x));e[10]=bits(std::ceil(x));
    e[11]=bits(static_cast<double>(std::bit_cast<std::int64_t>(a)));e[12]=bits(static_cast<double>(a));e[13]=std::bit_cast<std::uint32_t>(f);e[14]=bits(static_cast<double>(from));
    e[15]=integer(x,true,64);e[16]=integer(x,false,64);e[17]=integer(x,true,32);e[18]=integer(x,false,32);
    e[19]=bits(std::fmin(x,y));e[20]=bits(std::fmax(x,y));e[21]=x==y;
    // D3D treats signaling and quiet NaNs alike: one NaN returns the number.
    if(std::isnan(x)){e[19]=b;e[20]=b;}else if(std::isnan(y)){e[19]=a;e[20]=a;}
    // Signed zeros in D3D minimumNumber/maximumNumber have deterministic ordering.
    if(x==0&&y==0){e[19]=a|b;e[20]=a&b;}
    const ulong actual[]={fp64_add(a,b),fp64_sub(a,b),fp64_mul(a,b),fp64_div(a,b),fp64_rem(a,b),fp64_fma(a,b,c),fp64_sqrt(a),fp64_round(a,0),fp64_round(a,1),fp64_round(a,2),fp64_round(a,3),fp64_from_i64(std::bit_cast<std::int64_t>(a)),fp64_from_u64(a),fp64_to_f32(a),fp64_from_f32(static_cast<unsigned>(a)),static_cast<ulong>(fp64_to_i64(a)),fp64_to_u64(a),static_cast<ulong>(fp64_to_i32(a)),fp64_to_u32(a),fp64_min(a,b),fp64_max(a,b),fp64_eq(a,b)};
    for(unsigned op=0;op<ops;++op) {
      bool ok=actual[op]==e[op];
      if(op==13) ok|=((actual[op]&0x7fffffff)>0x7f800000 && (e[op]&0x7fffffff)>0x7f800000);
      else if(op<15||op==19||op==20) ok|=nan(actual[op])&&nan(e[op]);
      if(!ok){std::printf("CPU FAIL case=%u op=%u a=%016llx b=%016llx expected=%016llx actual=%016llx\n",i,op,(unsigned long long)a,(unsigned long long)b,(unsigned long long)e[op],(unsigned long long)actual[op]);return 1;}
      ++cpuChecks;
    }
  }
  std::printf("PASS CPU binary64 bit oracle checks=%u\n",cpuChecks);
  id<MTLDevice> device=MTLCreateSystemDefaultDevice();
  if(!device){std::fprintf(stderr,"No Metal device\n");return 2;}
  NSError *error=nil;
  id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
  if(!library){std::fprintf(stderr,"library: %s\n",error.description.UTF8String);return 2;}
  id<MTLComputePipelineState> compute=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"fp64_compute"] error:&error];
  if(!compute){std::fprintf(stderr,"compute: %s\n",error.description.UTF8String);return 2;}
  id<MTLBuffer> inputs=[device newBufferWithBytes:input.data() length:input.size()*8 options:MTLResourceStorageModeShared];
  id<MTLBuffer> output=[device newBufferWithLength:expected.size()*8 options:MTLResourceStorageModeShared];
  std::memset(output.contents,0xcd,expected.size()*8);
  id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> command=[queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];[encoder setComputePipelineState:compute];[encoder setBuffer:inputs offset:0 atIndex:0];[encoder setBuffer:output offset:0 atIndex:1];
  [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[encoder endEncoding];[command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"compute command: %s\n",command.error.description.UTF8String);return 2;}
  const auto *gpu=static_cast<const std::uint64_t *>(output.contents);
  for(unsigned i=0;i<count;++i)for(unsigned op=0;op<ops;++op){auto got=gpu[i*ops+op],want=expected[i*ops+op];bool ok=got==want;
    if(op==13)ok|=((got&0x7fffffff)>0x7f800000&&(want&0x7fffffff)>0x7f800000);
    else if(op<15||op==19||op==20)ok|=nan(got)&&nan(want);
    if(!ok){std::printf("COMPUTE FAIL case=%u op=%u a=%016llx b=%016llx expected=%016llx actual=%016llx\n",i,op,(unsigned long long)input[i*3],(unsigned long long)input[i*3+1],(unsigned long long)want,(unsigned long long)got);return 1;}}
  std::printf("PASS Metal compute device=%s checks=%u\n",device.name.UTF8String,count*ops);
  MTLRenderPipelineDescriptor *pipeline=[MTLRenderPipelineDescriptor new];pipeline.vertexFunction=[library newFunctionWithName:@"fp64_vertex"];pipeline.fragmentFunction=[library newFunctionWithName:@"fp64_fragment"];pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Uint;
  id<MTLRenderPipelineState> graphics=[device newRenderPipelineStateWithDescriptor:pipeline error:&error];
  if(!graphics){std::fprintf(stderr,"graphics: %s\n",error.description.UTF8String);return 2;}
  MTLTextureDescriptor *textureDescription=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:512 height:1 mipmapped:NO];textureDescription.storageMode=MTLStorageModeShared;textureDescription.usage=MTLTextureUsageRenderTarget;
  id<MTLTexture> texture=[device newTextureWithDescriptor:textureDescription];
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  command=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:pass];[render setRenderPipelineState:graphics];[render setFragmentBuffer:inputs offset:0 atIndex:0];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];[command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"graphics command: %s\n",command.error.description.UTF8String);return 2;}
  std::vector<std::uint64_t> pixels(1024);[texture getBytes:pixels.data() bytesPerRow:512*16 fromRegion:MTLRegionMake2D(0,0,512,1) mipmapLevel:0];
  for(unsigned i=0;i<512;++i)for(unsigned op=0;op<2;++op){auto got=pixels[i*2+op],want=expected[i*ops+(op?0:5)];if(got!=want&&!(nan(got)&&nan(want))){std::printf("GRAPHICS FAIL case=%u op=%u expected=%016llx actual=%016llx\n",i,op,(unsigned long long)want,(unsigned long long)got);return 1;}}
  std::printf("PASS Metal graphics bit oracle checks=1024\n");return 0;
}}
