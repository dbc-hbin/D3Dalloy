// Direct AS-helper byte oracle. Run with Metal API and Shader Validation enabled.
// The original helper source is supplied independently as argv[1].
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../src/winemetal/unix/rt_acceleration_structure.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Metadata {
  uint32_t magic, type, flags, count;
  uint64_t shadowSize, currentSize;
  uint32_t compacted, reserved;
  uint64_t deserializedSize;
};
struct Job {
  uint64_t source, offset, sourceStride;
  uint32_t bytes, stride, count, mode, flags;
};
struct Params { uint64_t asAddress, metadataSource; uint32_t count, mode; Metadata metadata; };
static_assert(sizeof(Metadata) == 48 && sizeof(Job) == 48 && sizeof(Params) == 72);
static_assert(sizeof(d3dalloy_as_prepass_params) == 96);
static int failures = 0, cases = 0;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLComputePipelineState> shadowPipeline[2], prepass[2], postbuild[2];
static id<MTLBuffer> buffer(size_t bytes) {
  id<MTLBuffer> out = [device newBufferWithLength:std::max<size_t>(bytes, 256) options:MTLResourceStorageModeShared];
  if (!out) std::abort();
  std::memset(out.contents, 0xa7, out.length);
  return out;
}
static id<MTLComputePipelineState> pipeline(id<MTLLibrary> lib, NSString *name) {
  NSError *error = nil;
  auto out = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&error];
  if (!out) { std::fprintf(stderr, "%s\n", error.description.UTF8String); std::abort(); }
  return out;
}
static void finish(id<MTLCommandBuffer> cb) {
  [cb commit]; [cb waitUntilCompleted];
  if (cb.status == MTLCommandBufferStatusError) {
    std::fprintf(stderr, "GPU error: %s\n", cb.error.description.UTF8String); std::abort();
  }
}
static void equal(const char *name, const void *actual, const void *expected, size_t bytes) {
  ++cases;
  if (!std::memcmp(actual, expected, bytes)) return;
  auto *a = static_cast<const uint8_t *>(actual), *b = static_cast<const uint8_t *>(expected);
  for (size_t i = 0; i < bytes; ++i) if (a[i] != b[i]) {
    std::printf("FAIL %s offset=%zu expected=%02x actual=%02x\n", name, i, b[i], a[i]); break;
  }
  ++failures;
}
static id<MTLCommandBuffer> encodeShadow(unsigned which, id<MTLBuffer> destination,
    Params params, id<MTLBuffer> geometries, id<MTLBuffer> jobs, NSArray<id<MTLBuffer>> *sources,
    unsigned repetitions = 1) {
  auto cb = [queue commandBuffer];
  auto e = [cb computeCommandEncoder];
  [e setComputePipelineState:shadowPipeline[which]];
  params.asAddress = destination.gpuAddress;
  [e setBytes:&params length:sizeof(params) atIndex:0];
  [e setBuffer:geometries offset:0 atIndex:1];
  [e setBuffer:jobs offset:0 atIndex:2];
  [e useResource:destination usage:MTLResourceUsageRead | MTLResourceUsageWrite];
  for (id<MTLBuffer> source in sources) [e useResource:source usage:MTLResourceUsageRead];
  for (unsigned i = 0; i < repetitions; ++i) {
    const unsigned rows = which && !params.mode ? std::clamp(params.count, 1u, 64u) : 1;
    const unsigned lanes = which && !params.mode && repetitions == 1 ? 37 : 4096;
    [e dispatchThreads:MTLSizeMake(lanes,rows,1) threadsPerThreadgroup:MTLSizeMake(std::min(lanes,256u),1,1)];
    if (i + 1 != repetitions) [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
  }
  [e endEncoding]; finish(cb); return cb;
}
static void shadowCases(bool timing) {
  const size_t sourceBytes = timing ? 32 * 1024 * 1024 : 2 * 1024 * 1024;
  auto source = buffer(sourceBytes), pointers = buffer(256), geometries = buffer(288);
  auto *data = static_cast<uint8_t *>(source.contents);
  for (size_t i = 0; i < sourceBytes; ++i) data[i] = uint8_t(i * 29 + i / 79 + 7);
  for (unsigned i = 0; i < 288; ++i) static_cast<uint8_t *>(geometries.contents)[i] = uint8_t(i * 37 + 3);
  std::vector<Job> jobs;
  std::vector<uint8_t> expected(64 + 288, 0xa7);
  std::memcpy(expected.data() + 64, geometries.contents, 288);
  auto append = [&](size_t sourceOffset, uint64_t sourceStride, uint32_t bytes, uint32_t stride, uint32_t count, bool pointer = false) {
    size_t offset = (expected.size() + 15) & ~size_t(15);
    expected.resize(offset + size_t(stride) * count, 0xa7);
    jobs.push_back({pointer ? pointers.gpuAddress : source.gpuAddress + sourceOffset, offset, sourceStride, bytes, stride, count, pointer ? 1u : 0u, 0});
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *src = data + sourceOffset + i * sourceStride;
      if (pointer) src = i == 2 ? nullptr : data + 64 * (i + 1);
      for (uint32_t b = 0; b < stride; ++b) expected[offset + size_t(i) * stride + b] = src && b < bytes ? src[b] : 0;
    }
  };
  if (timing) {
    append(0, 12, 12, 12, 699050);
    append(8 * 1024 * 1024, 20, 12, 12, 349525);
    append(16 * 1024 * 1024 + 2, 2, 2, 4, 349525);
  } else {
    append(0, 16, 16, 16, 8193); append(4, 12, 12, 12, 4099);
    append(8, 20, 12, 12, 4099); append(2, 2, 2, 4, 8193);
    append(2, 10, 6, 8, 4099); append(0, 4097, 4097, 4100, 1);
    append(1, 3, 3, 4, 1); append(0, 1, 1, 4, 1);
    append(4, 12, 8, 8, 17); append(0, 12, 12, 12, 0);
    for (unsigned i = 0; i < 5; ++i)
      static_cast<uint64_t *>(pointers.contents)[i] = i == 2 ? 0 : source.gpuAddress + 64 * (i + 1);
    append(0, 8, 64, 64, 5, true);
    for (unsigned i = 0; i < 65; ++i) append(i, 12, 7, 8, i % 9 + 1);
  }
  const size_t payload = expected.size() - 64;
  expected.resize(expected.size() + 64, 0xa7);
  Metadata metadata{0x53415844,1,0x15,3,payload,0x123456789ull,0x12345,0,0x234567890ull};
  std::memcpy(expected.data() + 16, &metadata, 48);
  auto jobBuffer = buffer(jobs.size() * sizeof(Job)); std::memcpy(jobBuffer.contents, jobs.data(), jobs.size() * sizeof(Job));
  auto before = buffer(expected.size()), after = buffer(expected.size());
  Params params{0,0,uint32_t(jobs.size()),0,metadata};
  encodeShadow(0,before,params,geometries,jobBuffer,@[source,pointers]);
  encodeShadow(1,after,params,geometries,jobBuffer,@[source,pointers]);
  equal("shadow-before-cpu",before.contents,expected.data(),expected.size());
  equal("shadow-after-cpu",after.contents,expected.data(),expected.size());
  if (!timing) {
    for (unsigned version = 0; version < 2; ++version) {
      auto clone = buffer(expected.size()); params.mode = 1; params.metadataSource = before.gpuAddress;
      encodeShadow(version,clone,params,geometries,jobBuffer,@[before]);
      equal("shadow-clone",clone.contents,expected.data(),expected.size());
    }
  } else {
    std::array<std::vector<double>, 2> samples;
    for (unsigned sample = 0; sample < 12; ++sample) for (unsigned turn = 0; turn < 2; ++turn) {
      unsigned which = (sample + turn) & 1;
      auto cb = encodeShadow(which,which ? after : before,params,geometries,jobBuffer,@[source,pointers],8);
      double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000 / 8;
      if (sample >= 3) samples[which].push_back(ms);
      std::printf("AS_SHADOW_SAMPLE sample=%u version=%u gpu_ms=%.9f\n",sample,which,ms);
    }
    for (auto &sample : samples) std::sort(sample.begin(),sample.end());
    std::printf("AS_SHADOW_TIMING baseline_ms=%.9f candidate_ms=%.9f ratio=%.9f output_bytes=%zu\n",
        samples[0][4],samples[1][4],samples[1][4]/samples[0][4],payload);
  }
}
static void tlasCase(unsigned count, bool pointerLayout) {
  auto source = buffer(size_t(count) * 96 + 256), pointers = buffer(size_t(count) * 8);
  auto blas = buffer(size_t(count) * 32 + 256), geometries = buffer(288), jobs = buffer(sizeof(Job));
  std::vector<uint8_t> instances(size_t(count) * 64), native(size_t(count) * 72), contributions(size_t(count) * 4);
  for (unsigned i = 0; i < count; ++i) {
    uint32_t words[16];
    for (unsigned j = 0; j < 12; ++j) words[j] = 0x3f000000 + i * 19 + j * 73;
    words[12] = ((i * 7) & 0xffffff) | ((i & 255) << 24);
    words[13] = ((i * 13) & 0xffffff) | ((i & 0xff) << 24);
    uint64_t address = i % 4 ? blas.gpuAddress + i * 32 : 0, id = 0x100000000ull + i;
    std::memcpy(words + 14,&address,8); std::memcpy(static_cast<uint8_t *>(blas.contents) + i * 32,&id,8);
    std::memcpy(instances.data() + i * 64,words,64);
    std::memcpy(static_cast<uint8_t *>(source.contents) + i * (pointerLayout ? 96 : 64),words,64);
    static_cast<uint64_t *>(pointers.contents)[i] = source.gpuAddress + i * 96;
    auto *out = reinterpret_cast<uint32_t *>(native.data() + i * 72);
    for (unsigned column = 0; column < 4; ++column) for (unsigned row = 0; row < 3; ++row)
      out[column * 3 + row] = words[row * 4 + column];
    out[12] = (words[13] >> 24) & 15; out[13] = words[12] >> 24; out[14] = 0; out[15] = words[12] & 0xffffff;
    uint64_t actualID = address ? id : 0; std::memcpy(out + 16,&actualID,8);
    uint32_t contribution = words[13] & 0xffffff; std::memcpy(contributions.data() + i * 4,&contribution,4);
  }
  Metadata metadata{0x53415844,0,0x15,count,uint64_t(count)*64,0x987654321ull,0x12345,0,0x123456789ull};
  std::vector<uint8_t> expected(64 + size_t(count)*64 + 64,0xa7);
  uint64_t id = 0x100000012ull, address = 0x234567800ull;
  std::memcpy(expected.data(),&id,8); std::memcpy(expected.data()+8,&address,8);
  std::memcpy(expected.data()+16,&metadata,48);
  if (count) std::memcpy(expected.data()+64,instances.data(),instances.size());
  Job job{pointerLayout?pointers.gpuAddress:source.gpuAddress,64,pointerLayout?8u:64u,64,64,count,pointerLayout?1u:0u,0};
  std::memcpy(jobs.contents,&job,sizeof(job));
  for (unsigned which = 0; which < 2; ++which) {
    auto header = buffer(expected.size()), converted = buffer(size_t(count)*72), contribution = buffer(size_t(count)*4), mirror = buffer(256);
    if (!which) encodeShadow(0,header,{0,0,count?1u:0u,0,metadata},geometries,jobs,@[source,pointers]);
    d3dalloy_as_prepass_params params{id,address,0,64,72,count,count?(pointerLayout?2u:1u):0u,0x12345678,3,which?1u:0u,{}};
    std::memcpy(params.shadow_metadata,&metadata,48);
    auto cb = [queue commandBuffer]; auto e = [cb computeCommandEncoder];
    [e setComputePipelineState:prepass[which]];
    [e setBuffer:pointerLayout?pointers:source offset:0 atIndex:0];
    [e setBuffer:converted offset:0 atIndex:1]; [e setBuffer:header offset:0 atIndex:2];
    [e setBuffer:contribution offset:0 atIndex:3]; [e setBytes:&params length:which?96:48 atIndex:4];
    [e setBuffer:mirror offset:0 atIndex:5]; [e useResource:source usage:MTLResourceUsageRead];
    [e useResource:blas usage:MTLResourceUsageRead];
    [e dispatchThreads:MTLSizeMake(std::max(count,1u),1,1) threadsPerThreadgroup:MTLSizeMake(std::min(std::max(count,1u),256u),1,1)];
    [e endEncoding]; finish(cb);
    equal("tlas-header-shadow",header.contents,expected.data(),expected.size());
    equal("tlas-native",converted.contents,native.data(),native.size());
    equal("tlas-contributions",contribution.contents,contributions.data(),contributions.size());
    uint32_t token = 0x12345678;
    equal("tlas-version",static_cast<uint32_t *>(mirror.contents)+3,&token,4);
  }
}
static void scalarCases() {
  auto header = buffer(512), output = buffer(256);
  auto *h = static_cast<uint64_t *>(header.contents);
  h[0]=1; h[1]=2; Metadata m{0x53415844,1,0x15,0,96,512,100,0,768}; std::memcpy(h+2,&m,48);
  for (unsigned operation = 0; operation < 4; ++operation) for (unsigned which = 0; which < 2; ++which) {
    struct { uint64_t source,destination; uint32_t operation,reserved; } params{header.gpuAddress,output.gpuAddress,operation,0};
    auto cb=[queue commandBuffer]; auto e=[cb computeCommandEncoder]; [e setComputePipelineState:postbuild[which]];
    [e setBytes:&params length:sizeof(params) atIndex:0]; [e useResource:header usage:MTLResourceUsageRead];
    [e useResource:output usage:MTLResourceUsageWrite];
    [e dispatchThreads:MTLSizeMake(which?1:4096,1,1) threadsPerThreadgroup:MTLSizeMake(which?1:256,1,1)];
    [e endEncoding]; finish(cb);
    uint64_t expected[2] = {operation==0?512ull:operation==1?104ull:operation==2?216ull:512ull,0};
    equal("scalar-query",output.contents,expected,operation==2?16:8);
  }
  // The native size write at byte 48 has cleared the marker at byte 52.
  // Exercise known markers and a relocated shadow with two surviving pointers.
  for (uint32_t marker : {0u, 1u, 3u}) for (bool compact : {false, true}) {
    constexpr uint32_t geometryCount = 4;
    Metadata metadata{0x53415844,1,0,geometryCount,96*geometryCount,2048,100,0,2048};
    std::memset(header.contents, 0xa7, header.length);
    auto *bytes = static_cast<uint8_t *>(header.contents);
    std::memcpy(bytes + 16, &metadata, sizeof(metadata));
    for (unsigned i = 0; i < geometryCount; ++i) {
      uint64_t address = marker != 1 && (i == 0 || i == 2) ? 0x1000u + i : 0;
      std::memcpy(bytes + 64 + 96*i + 64, &address, sizeof(address));
    }
    std::vector<uint8_t> expected(bytes, bytes + header.length);
    metadata.reserved = marker ? marker : 3;
    if (compact) metadata.currentSize = 768;
    std::memcpy(expected.data() + 16, &metadata, sizeof(metadata));
    struct { uint64_t source,marker; uint32_t op,count; } params{header.gpuAddress,marker,compact?0x300u:0x200u,0};
    auto cb=[queue commandBuffer]; auto e=[cb computeCommandEncoder]; [e setComputePipelineState:postbuild[1]];
    [e setBytes:&params length:sizeof(params) atIndex:0]; [e useResource:header usage:MTLResourceUsageRead|MTLResourceUsageWrite];
    [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [e endEncoding]; finish(cb);
    equal("postbuild-header",bytes,expected.data(),expected.size());
  }
}
int main(int argc,char **argv) { @autoreleasepool {
  if (argc < 3) return 2;
  device=MTLCreateSystemDefaultDevice(); queue=[device newCommandQueue];
  for (unsigned i=0;i<2;++i) {
    NSError *error=nil;
    NSString *source=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[i+1]] encoding:NSUTF8StringEncoding error:&error];
    MTLCompileOptions *options=[MTLCompileOptions new]; options.languageVersion=MTLLanguageVersion3_1;
    auto lib=[device newLibraryWithSource:source options:options error:&error];
    if (!lib) { std::fprintf(stderr,"%s\n",error.description.UTF8String); return 2; }
    shadowPipeline[i]=pipeline(lib,@"d3dalloy_as_shadow"); prepass[i]=pipeline(lib,@"d3dalloy_as_prepass");
    postbuild[i]=pipeline(lib,@"d3dalloy_as_postbuild");
  }
  bool timing=argc>3 && !std::strcmp(argv[3],"--timing");
  shadowCases(timing);
  if (!timing) { for (unsigned count:{0u,1u,5u,4097u}) for (bool pointers:{false,true}) tlasCase(count,pointers); scalarCases(); }
  std::printf("AS_HELPER_RESULT cases=%d failures=%d\n",cases,failures);
  return failures?1:0;
} }
