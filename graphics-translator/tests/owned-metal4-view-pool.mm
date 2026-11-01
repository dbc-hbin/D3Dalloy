#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void require(bool condition, const char *what, NSError *error = nil) {
  if (!condition) {
    std::fprintf(stderr, "OWNED_METAL4_VIEW_POOL_FAIL %s %s\n", what,
                 error ? error.localizedDescription.UTF8String : "");
    std::exit(1);
  }
}

#ifdef OWNED_VIEW_POOL_CPU_ONLY
// The bounded CPU build injects the actual native pool class and allocation path.
#include OWNED_VIEW_POOL_NATIVE_MEMBERSHIP
static unsigned cpuTextureRetirements;
static unsigned cpuViewRetirements;
static uint64_t cpuResourceID=100;
@interface OwnedViewPoolCPUTexture : NSObject
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type
    levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle;
@end
@interface OwnedViewPoolCPUView : NSObject { @public id parent; NSRange levels,slices; uint64_t resource; }
- (MTLResourceID)gpuResourceID;
@end
@implementation OwnedViewPoolCPUView
- (MTLResourceID)gpuResourceID { return (MTLResourceID){resource}; }
- (void)dealloc { ++cpuViewRetirements; [parent release]; [super dealloc]; }
@end
@implementation OwnedViewPoolCPUTexture
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type
    levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swizzle {
  require(format==MTLPixelFormatR32Uint && type==MTLTextureType2DArray,"frozen-view-format-type");
  (void)swizzle;
  auto view=[OwnedViewPoolCPUView new]; view->parent=[self retain];
  view->levels=levels; view->slices=slices; view->resource=++cpuResourceID;
  return (id<MTLTexture>)view;
}
- (void)dealloc { ++cpuTextureRetirements; [super dealloc]; }
@end
@interface OwnedViewPoolCPUPool : NSObject { @public id lastTexture; NSRange levels,slices; }
- (MTLResourceID)setTextureView:(id<MTLTexture>)texture descriptor:(MTLTextureViewDescriptor *)descriptor atIndex:(NSUInteger)index;
- (MTLResourceID)copyResourceViewsFromPool:(id<MTLResourceViewPool>)source sourceRange:(NSRange)range destinationIndex:(NSUInteger)index;
@end
@implementation OwnedViewPoolCPUPool
- (MTLResourceID)setTextureView:(id<MTLTexture>)texture descriptor:(MTLTextureViewDescriptor *)descriptor atIndex:(NSUInteger)index {
  (void)index; lastTexture=texture; levels=descriptor.levelRange; slices=descriptor.sliceRange;
  return (MTLResourceID){++cpuResourceID};
}
- (MTLResourceID)copyResourceViewsFromPool:(id<MTLResourceViewPool>)source sourceRange:(NSRange)range destinationIndex:(NSUInteger)index {
  (void)source; (void)index; require(range.length==1,"copy-count"); return (MTLResourceID){++cpuResourceID};
}
@end
@interface OwnedViewPoolCPUDevice : NSObject { @public BOOL failFactory; }
- (id<MTLTextureViewPool>)newTextureViewPoolWithDescriptor:(MTLResourceViewPoolDescriptor *)descriptor error:(NSError **)error;
@end
@implementation OwnedViewPoolCPUDevice
- (id<MTLTextureViewPool>)newTextureViewPoolWithDescriptor:(MTLResourceViewPoolDescriptor *)descriptor error:(NSError **)error {
  (void)error; require(descriptor.resourceViewCount==4,"factory-capacity");
  return failFactory ? nil : (id<MTLTextureViewPool>)[OwnedViewPoolCPUPool new];
}
@end
template<typename... Args> static uint64_t cpuPoolOperation(uint32_t operation,Args... args) {
  struct WMTMetal4Call call{operation,0,{uint64_t(args)...},{}};
  require(m4_pool_operation(&call)==0,"native-pool-operation");
  return call.ret[0];
}
@interface OwnedViewPoolCPUResidency : NSObject { @public NSUInteger additions; }
- (void)addAllocation:(id<MTLAllocation>)allocation;
@end
@implementation OwnedViewPoolCPUResidency
- (void)addAllocation:(id<MTLAllocation>)allocation { require(allocation!=nil,"residency-allocation"); ++additions; }
@end
int main() {
  @autoreleasepool {
    auto views=[D3DALLOYM4ViewPool new];
    views->textures=[[NSMutableArray alloc] initWithCapacity:1000000];
    views->parents=[NSCountedSet new]; views->trace=YES;
    for (NSUInteger i=0;i<1000000;++i) [views->textures addObject:[NSNull null]];
    auto other=[D3DALLOYM4ViewPool new];
    other->textures=[@[[NSNull null],[NSNull null]] mutableCopy];
    other->parents=[NSCountedSet new];
    auto first=[OwnedViewPoolCPUTexture new], second=[OwnedViewPoolCPUTexture new];
    [views replaceTexture:first atIndex:0];
    [views replaceTexture:first atIndex:300000];
    [views replaceTexture:first atIndex:999999];
    [views replaceTexture:second atIndex:17];
    [views replaceTexture:views->textures[0] atIndex:1];
    [other replaceTexture:views->textures[0] atIndex:0];
    require([views->parents countForObject:first]==4 && [other->parents countForObject:first]==1,"copy-refcounts");
    [views replaceTexture:second atIndex:0];
    [views replaceTexture:[NSNull null] atIndex:300000];
    [views replaceTexture:[NSNull null] atIndex:999999];
    require([views->parents countForObject:first]==1 && [views->parents countForObject:second]==2,"overwrite-refcounts");
    auto buffer=[NSObject new];
    objc_setAssociatedObject(buffer,&m4_view_pool_key,views,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    auto recording=[D3DALLOYM4Recording new];
    recording->objects=[NSMutableSet new]; recording->allocations=[NSMutableSet new];
    recording->residency=(id<MTLResidencySet>)[OwnedViewPoolCPUResidency new]; recording->trace=YES;
    m4_allocation(recording,(id<MTLAllocation>)buffer);
    m4_allocation(recording,(id<MTLAllocation>)buffer);
    require(views->residencyVisits==2 && recording->allocations.count==3,"sparse-unique-recording-visits");
    require(((OwnedViewPoolCPUResidency *)recording->residency)->additions==3,"residency-deduplication");
    [first release]; [second release];
    [views replaceTexture:[NSNull null] atIndex:1];
    [views replaceTexture:[NSNull null] atIndex:0];
    [views replaceTexture:[NSNull null] atIndex:17];
    [other replaceTexture:[NSNull null] atIndex:0];
    require(views->parents.count==0 && other->parents.count==0 && cpuTextureRetirements==0,"immutable-recording-retention");
    require([recording->objects containsObject:first] && [recording->objects containsObject:second],"recorded-parent-snapshot");
    [recording release];
    require(cpuTextureRetirements==2,"retirement-refcounts");
    views->trace=NO; [buffer release]; [views release]; [other release];
    std::puts("OWNED_METAL4_VIEW_POOL_CPU_PASS slots=1000000 unique_parent_visits=2 repeated_recording_visits=0 copy_refcounts=1 overwrite_null=1 retired_parents=2");
    auto device=[OwnedViewPoolCPUDevice new]; device->failFactory=YES;
    auto fallbackBuffer=[NSObject new];
    auto fallback=(D3DALLOYM4ViewPool *)(uintptr_t)cpuPoolOperation(WMTMetal4NewTextureViewPool,(uintptr_t)device,4,(uintptr_t)fallbackBuffer);
    auto fallback2=(D3DALLOYM4ViewPool *)(uintptr_t)cpuPoolOperation(WMTMetal4NewTextureViewPool,(uintptr_t)device,4,0);
    require(fallback && !fallback->pool && objc_getAssociatedObject(fallbackBuffer,&m4_view_pool_key)==fallback,"factory-nil-preserves-lease");
    device->failFactory=NO;
    auto pooled=(D3DALLOYM4ViewPool *)(uintptr_t)cpuPoolOperation(WMTMetal4NewTextureViewPool,(uintptr_t)device,4,0);
    require(pooled->pool!=nil,"factory-success");
    fallback->trace=YES; fallback2->trace=YES; pooled->trace=YES;
    auto oldParent=[OwnedViewPoolCPUTexture new], newParent=[OwnedViewPoolCPUTexture new];
    WMTTextureViewPoolInfo info{WMTPixelFormatR32Uint,MTLTextureType2DArray,1,2,2,1};
    auto set=[&](D3DALLOYM4ViewPool *heap,unsigned slot,id parent) { return cpuPoolOperation(WMTMetal4SetTextureView,(uintptr_t)heap,(uintptr_t)parent,(uintptr_t)&info,slot); };
    auto copy=[&](D3DALLOYM4ViewPool *dst,unsigned to,D3DALLOYM4ViewPool *src,unsigned from,id parent) { return cpuPoolOperation(WMTMetal4CopyTextureViews,(uintptr_t)dst,(uintptr_t)src,from,to,(uintptr_t)parent,(uintptr_t)&info); };
    const auto pooledID=set(pooled,0,oldParent);
    const auto ordinaryID=copy(fallback,0,pooled,0,oldParent);
    require(ordinaryID && ordinaryID!=pooledID,"pooled-to-fallback-id");
    auto frozen=(OwnedViewPoolCPUView *)fallback->ordinaryViews[@0];
    require(frozen->parent==oldParent && frozen->levels.location==1 && frozen->levels.length==2 && frozen->slices.location==2 && frozen->slices.length==1,"frozen-view-metadata");
    set(pooled,0,newParent);
    require(copy(pooled,1,fallback,0,oldParent)!=ordinaryID,"fallback-to-pooled-id");
    auto native=(OwnedViewPoolCPUPool *)pooled->pool;
    require(native->lastTexture==oldParent && native->levels.location==1 && native->slices.location==2,"fallback-to-pooled-frozen-backing");
    require(copy(fallback2,1,fallback,0,oldParent)==ordinaryID,"fallback-to-fallback-view-reuse");
    require(copy(fallback,2,fallback,0,oldParent)==ordinaryID && [fallback->ordinaryOwners countForObject:frozen]==2,"same-fallback-copy-refcounts");
    auto lease=[D3DALLOYM4Recording new]; lease->objects=[NSMutableSet new]; lease->allocations=[NSMutableSet new];
    lease->residency=(id<MTLResidencySet>)[OwnedViewPoolCPUResidency new];
    m4_allocation(lease,(id<MTLAllocation>)fallbackBuffer);
    const auto beforeViews=cpuViewRetirements;
    set(fallback,0,newParent);
    cpuPoolOperation(WMTMetal4ClearTextureView,(uintptr_t)fallback,2);
    cpuPoolOperation(WMTMetal4ClearTextureView,(uintptr_t)fallback2,1);
    require([lease->objects containsObject:frozen] && cpuViewRetirements==beforeViews,"fallback-recorded-view-retention");
    cpuPoolOperation(WMTMetal4ClearTextureView,(uintptr_t)fallback,0);
    require(fallback->parents.count==0 && fallback->ordinaryOwners.count==0,"fallback-null-membership");
    [lease release];
    require(cpuViewRetirements==beforeViews+2,"fallback-view-retirement");
    require(pooled->setCount==3 && pooled->copyCount==0 && fallback->ordinaryNew==2 && fallback->ordinaryReuse==1 && fallback2->ordinaryReuse==1,"fallback-native-operation-counts");
    require(fallback->residencyVisits==1,"fallback-unique-parent-visits");
    fallback->trace=NO; fallback2->trace=NO; pooled->trace=NO;
    [fallbackBuffer release]; [fallback release]; [fallback2 release]; [pooled release];
    [oldParent release]; [newParent release]; [device release];
    std::puts("OWNED_METAL4_VIEW_POOL_FALLBACK_CPU_PASS factory_nil=1 pooled_to_fallback=1 fallback_to_pooled=1 fallback_reuse=1 frozen_metadata=1 overwrite_null=1 recording_retirement=1");
  }
  return 0;
}
#else
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
#include OWNED_VIEW_POOL_NATIVE_MEMBERSHIP
@interface OwnedViewPoolDeviceProxy : NSObject { @public id<MTLDevice> target; BOOL failFactory; }
- (id<MTLTextureViewPool>)newTextureViewPoolWithDescriptor:(MTLResourceViewPoolDescriptor *)descriptor error:(NSError **)error;
@end
@implementation OwnedViewPoolDeviceProxy
- (id<MTLTextureViewPool>)newTextureViewPoolWithDescriptor:(MTLResourceViewPoolDescriptor *)descriptor error:(NSError **)error {
  return failFactory ? nil : [target newTextureViewPoolWithDescriptor:descriptor error:error];
}
@end
template<typename... Args> static uint64_t bridgePoolOperation(uint32_t operation,Args... args) {
  struct WMTMetal4Call call{operation,0,{uint64_t(args)...},{}};
  require(m4_pool_operation(&call)==0,"native-pool-operation");
  return call.ret[0];
}
#endif
int main(int argc,char **argv) {
  (void)argc; (void)argv;
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    require(device != nil, "device");
    NSError *error = nil;
    auto compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
    auto library = [device newLibraryWithSource:@R"(
#include <metal_stdlib>
using namespace metal;
kernel void read_view(texture2d<uint, access::read> input [[texture(0)]], device uint *output [[buffer(0)]]) {
  output[0] = input.read(uint2(0)).x;
}
)" options:nil error:&error];
    require(compiler && library, "compiler-library", error);
    auto function = [MTL4LibraryFunctionDescriptor new];
    function.library = library;
    function.name = @"read_view";
    auto descriptor = [MTL4ComputePipelineDescriptor new];
    descriptor.computeFunctionDescriptor = function;
    auto pipeline = [compiler newComputePipelineStateWithDescriptor:descriptor dynamicLinkingDescriptor:nil
                                               compilerTaskOptions:nil error:&error];
    require(pipeline != nil, "pipeline", error);
#ifndef OWNED_VIEW_POOL_BRIDGE_PROBE
    auto pool_descriptor = [MTLResourceViewPoolDescriptor new];
    pool_descriptor.resourceViewCount = 4;
    auto source = [device newTextureViewPoolWithDescriptor:pool_descriptor error:&error];
    require(source != nil, "source-pool", error);
    auto destination = [device newTextureViewPoolWithDescriptor:pool_descriptor error:&error];
    require(destination != nil, "destination-pool", error);
    require(source.resourceViewCount >= 4 && destination.resourceViewCount >= 4, "pool-size");
#endif
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
    require(argc==2,"bridge-mode-required");
    const bool sourceFallback=!std::strcmp(argv[1],"fallback-to-pooled") || !std::strcmp(argv[1],"fallback-to-fallback");
    const bool destinationFallback=!std::strcmp(argv[1],"pooled-to-fallback") || !std::strcmp(argv[1],"fallback-to-fallback");
    require(sourceFallback || destinationFallback,"bridge-mode");
    auto proxy=[OwnedViewPoolDeviceProxy new]; proxy->target=device; proxy->failFactory=sourceFallback;
    auto sourceBuffer=[device newBufferWithLength:32 options:MTLResourceStorageModeShared];
    auto destinationBuffer=[device newBufferWithLength:32 options:MTLResourceStorageModeShared];
    auto sourceWrapper=(D3DALLOYM4ViewPool *)(uintptr_t)bridgePoolOperation(WMTMetal4NewTextureViewPool,(uintptr_t)proxy,4,(uintptr_t)sourceBuffer);
    proxy->failFactory=destinationFallback;
    auto destinationWrapper=(D3DALLOYM4ViewPool *)(uintptr_t)bridgePoolOperation(WMTMetal4NewTextureViewPool,(uintptr_t)proxy,4,(uintptr_t)destinationBuffer);
    require(bool(sourceWrapper->pool)==!sourceFallback && bool(destinationWrapper->pool)==!destinationFallback,"controlled-factory-failure");
    WMTTextureViewPoolInfo viewInfo{WMTPixelFormatR32Uint,MTLTextureType2D,0,1,0,1};
    auto setView=[&](D3DALLOYM4ViewPool *heap,id texture,unsigned slot) { return MTLResourceID{bridgePoolOperation(WMTMetal4SetTextureView,(uintptr_t)heap,(uintptr_t)texture,(uintptr_t)&viewInfo,slot)}; };
    auto copyView=[&](D3DALLOYM4ViewPool *dst,D3DALLOYM4ViewPool *src,unsigned from,unsigned to,id parent) { return MTLResourceID{bridgePoolOperation(WMTMetal4CopyTextureViews,(uintptr_t)dst,(uintptr_t)src,from,to,(uintptr_t)parent,(uintptr_t)&viewInfo)}; };
#endif
    auto texture_descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                              width:1 height:1 mipmapped:NO];
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = MTLTextureUsageShaderRead;
    auto first = [device newTextureWithDescriptor:texture_descriptor];
    auto second = [device newTextureWithDescriptor:texture_descriptor];
    auto output = [device newBufferWithLength:6 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    auto residency = [device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    auto queue = [device newMTL4CommandQueue];
    auto allocator = [device newCommandAllocator];
    require(first && second && output && residency && queue && allocator, "execution-resources", error);
    [residency addAllocation:first];
    [residency addAllocation:second];
    [residency addAllocation:output];
    [residency commit];
    auto table_descriptor = [MTL4ArgumentTableDescriptor new];
    table_descriptor.maxBufferBindCount = 1;
    table_descriptor.maxTextureBindCount = 1;
    table_descriptor.initializeBindings = YES;
    auto table = [device newArgumentTableWithDescriptor:table_descriptor error:&error];
    require(table != nil, "argument-table", error);
    for (unsigned execution = 0; execution < 2; ++execution) {
      const uint32_t a = 11 + execution * 100, b = 29 + execution * 100;
      [first replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&a bytesPerRow:sizeof(a)];
      [second replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&b bytesPerRow:sizeof(b)];
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
      const auto original=setView(sourceWrapper,first,0);
      const auto source_second=setView(sourceWrapper,second,1);
      const auto copied=copyView(destinationWrapper,sourceWrapper,0,2,first);
      const auto overwritten_source=setView(sourceWrapper,second,0);
      const auto copied_again=copyView(destinationWrapper,destinationWrapper,2,0,first);
      const auto overwritten_destination=setView(destinationWrapper,second,2);
      auto lease=[D3DALLOYM4Recording new]; lease->objects=[NSMutableSet new]; lease->allocations=[NSMutableSet new];
      lease->residency=[device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
      require(lease->residency!=nil,"bridge-recording-residency",error);
      m4_allocation(lease,sourceBuffer); m4_allocation(lease,destinationBuffer);
      [lease->residency addAllocation:output]; [lease->residency commit];
#else
      const auto original = [source setTextureView:first atIndex:0];
      const auto source_second = [source setTextureView:second atIndex:1];
      const auto copied = [destination copyResourceViewsFromPool:source sourceRange:NSMakeRange(0, 1)
                                               destinationIndex:2];
      const auto overwritten_source = [source setTextureView:second atIndex:0];
      const auto copied_again = [destination copyResourceViewsFromPool:destination sourceRange:NSMakeRange(2, 1)
                                                     destinationIndex:0];
      const auto overwritten_destination = [destination setTextureView:second atIndex:2];
#endif
      const std::array<MTLResourceID, 4> ids = {
        overwritten_source, source_second, copied_again, overwritten_destination
      };
      std::printf("OWNED_METAL4_VIEW_POOL_IDS execution=%u source=%llu copied=%llu source_after=%llu destination_after=%llu\n",
                  execution, (unsigned long long)original._impl, (unsigned long long)copied._impl,
                  (unsigned long long)overwritten_source._impl, (unsigned long long)overwritten_destination._impl);
      auto words = static_cast<uint32_t *>(output.contents);
      for (unsigned i = 0; i < 6; ++i) words[i] = 0x6d6d6d6d;
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
      for (id view in sourceWrapper->ordinaryOwners) require([lease->objects containsObject:view],"source-frozen-view-lease");
      for (id view in destinationWrapper->ordinaryOwners) require([lease->objects containsObject:view],"destination-frozen-view-lease");
      bridgePoolOperation(WMTMetal4ClearTextureView,(uintptr_t)sourceWrapper,0);
      bridgePoolOperation(WMTMetal4ClearTextureView,(uintptr_t)sourceWrapper,1);
      bridgePoolOperation(WMTMetal4ClearTextureView,(uintptr_t)destinationWrapper,0);
      bridgePoolOperation(WMTMetal4ClearTextureView,(uintptr_t)destinationWrapper,2);
      require(sourceWrapper->parents.count==0 && destinationWrapper->parents.count==0 && sourceWrapper->ordinaryOwners.count==0 && destinationWrapper->ordinaryOwners.count==0,"precommit-null-preserves-recording");
#endif
      auto command = [device newCommandBuffer];
      require(command != nil, "command-buffer");
      [command beginCommandBufferWithAllocator:allocator];
      [command useResidencySet:residency];
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
      [command useResidencySet:lease->residency];
#endif
      auto encoder = [command computeCommandEncoder];
      require(encoder != nil, "compute-encoder");
      [encoder setComputePipelineState:pipeline];
      [encoder setArgumentTable:table];
      for (unsigned i = 0; i < ids.size(); ++i) {
        [table setAddress:output.gpuAddress + (i + 1) * sizeof(uint32_t) atIndex:0];
        [table setTexture:ids[i] atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
      }
      [encoder endEncoding];
      [command endCommandBuffer];
      auto completed = dispatch_semaphore_create(0);
      __block NSError *gpu_error = nil;
      auto options = [MTL4CommitOptions new];
      [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
        gpu_error = feedback.error;
        dispatch_semaphore_signal(completed);
      }];
      id<MTL4CommandBuffer> commands[] = {command};
      [queue commit:commands count:1 options:options];
      require(dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) == 0,
              "completion-timeout");
      require(gpu_error == nil, "gpu-completion", gpu_error);
      const std::array<uint32_t, 6> expected = {0x6d6d6d6d, b, b, a, b, 0x6d6d6d6d};
      std::printf("OWNED_METAL4_VIEW_POOL_VALUES execution=%u words=", execution);
      for (unsigned i = 0; i < expected.size(); ++i) {
        std::printf("%s%u", i ? "," : "", words[i]);
        if (words[i] != expected[i]) {
          std::fprintf(stderr, "\nOWNED_METAL4_VIEW_POOL_FAIL execution=%u word=%u actual=%u expected=%u\n",
                       execution, i, words[i], expected[i]);
          return 1;
        }
      }
      std::printf("\n");
      [allocator reset];
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
      [command release];
      [lease release];
#endif
    }
    std::printf("OWNED_METAL4_VIEW_POOL_PASS executions=2 samples=8 guards=4 source_overwrite=1 destination_overwrite=1 same_pool_copy=1\n");
#ifdef OWNED_VIEW_POOL_BRIDGE_PROBE
    for (auto wrapper : {sourceWrapper,destinationWrapper})
      std::printf("OWNED_METAL4_VIEW_POOL_BRIDGE_COUNTS mode=%s pool_new=%u pool_fallback=%u slot_set=%llu slot_copy=%llu slot_clear=%llu unique_parent_visits=%llu ordinary_view_new=%llu ordinary_view_reuse=%llu\n",argv[1],wrapper->pool?1:0,wrapper->pool?0:1,(unsigned long long)wrapper->setCount,(unsigned long long)wrapper->copyCount,(unsigned long long)wrapper->clearCount,(unsigned long long)wrapper->residencyVisits,(unsigned long long)wrapper->ordinaryNew,(unsigned long long)wrapper->ordinaryReuse);
    [sourceBuffer release]; [destinationBuffer release]; [sourceWrapper release]; [destinationWrapper release]; [proxy release];
    std::printf("OWNED_METAL4_VIEW_POOL_FALLBACK_GPU_PASS mode=%s executions=2 samples=8 guards=4 precommit_null=1 frozen_view_leases=1 retired_recordings=2\n",argv[1]);
#endif
  }
  return 0;
}
#endif
