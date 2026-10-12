// SPDX-License-Identifier: LGPL-2.1-or-later
// Build: xcrun clang++ -std=c++17 -fblocks -fno-objc-arc -arch arm64 -mmacosx-version-min=26.0 -framework Foundation -framework Metal -framework QuartzCore graphics-translator/tests/owned-metal4-present-order.mm -o /tmp/owned-metal4-present-order
// Run: /tmp/owned-metal4-present-order <fresh-native-winemetal.so>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <initializer_list>
#include <unistd.h>
#include "../../src/winemetal/winemetal_metal4_fx.h"

using UnixCall = int32_t (*)(void *);
using FeedbackHandler = void (^)(id<MTL4CommitFeedback>);
static UnixCall bridge;
static char feedbackKey;
static void require(bool ok, const char *reason) {
  if (!ok) { std::fprintf(stderr,"METAL4_PRESENT_ORDER_FAIL %s\n",reason); std::exit(1); }
}
static uint64_t handle(id object) { return reinterpret_cast<uintptr_t>(object); }
static uint64_t call(uint32_t operation, std::initializer_list<uint64_t> args) {
  struct WMTMetal4Call c{}; c.operation=operation;
  unsigned i=0; for (uint64_t arg : args) c.args[i++]=arg;
  int32_t status=bridge(&c);
  if (status) { std::fprintf(stderr,"METAL4_PRESENT_ORDER_FAIL operation=%u status=%08x\n",operation,uint32_t(status)); std::exit(1); }
  return c.ret[0];
}
static id member(id object, const char *name) {
  Ivar ivar=class_getInstanceVariable(object_getClass(object),name);
  require(ivar!=nullptr,"missing production object ivar");
  return object_getIvar(object,ivar);
}
// Private oracle: capture the production block at the SDK registration boundary,
// not a copy of the bridge's ordering logic. No real queue uses these options.
static void captureFeedback(id options, SEL, FeedbackHandler handler) {
  require(!objc_getAssociatedObject(options,&feedbackKey),"one feedback registration");
  objc_setAssociatedObject(options,&feedbackKey,handler,OBJC_ASSOCIATION_COPY_NONATOMIC);
}
@interface NSObject (PresentTicketOracle)
- (void)complete:(BOOL)success;
@end

@interface OrderDrawable : NSObject {
@public
  unsigned signals, presents;
  BOOL commitReturned;
}
@end
@implementation OrderDrawable
- (void)present {
  require(commitReturned,"present before native commit returned");
  require(signals==1,"present must follow exactly one signalDrawable");
  require(presents++==0,"drawable presented twice");
}
@end

@interface OrderFeedback : NSObject {
@public NSError *failure;
}
@end
@implementation OrderFeedback
- (NSError *)error { return failure; }
- (CFTimeInterval)GPUStartTime { return 1; }
- (CFTimeInterval)GPUEndTime { return 2; }
- (void)dealloc { [failure release]; [super dealloc]; }
@end

@interface OrderQueue : NSObject {
@public
  OrderDrawable *drawable;
  OrderFeedback *feedback;
  id<MTLResidencySet> residency;
  FeedbackHandler handler;
  BOOL early, returned;
  unsigned commits, deliveries;
}
- (void)deliver;
@end
@implementation OrderQueue
- (void)deliver {
  require(handler!=nil && deliveries++==0,"exactly one native feedback delivery");
  handler((id<MTL4CommitFeedback>)feedback);
}
- (void)commit:(id<MTL4CommandBuffer> const *)commands count:(NSUInteger)count options:(MTL4CommitOptions *)options {
  require(commits++==0 && count==1 && commands[0]!=nil,"native command submission");
  require(residency.allocationCount==1,"production recording owns allocation at commit");
  handler=[(FeedbackHandler)objc_getAssociatedObject(options,&feedbackKey) copy];
  require(handler!=nil,"captured production feedback");
  if (early) {
    [self deliver];
    require(drawable->signals==0 && drawable->presents==0,"early feedback cannot signal or present during commit");
    require(residency.allocationCount==1,"early feedback cannot retire native residency during commit");
  }
  returned=YES; drawable->commitReturned=YES;
}
- (void)signalDrawable:(id)nativeDrawable {
  require(returned,"signalDrawable before native commit returned");
  require(nativeDrawable==drawable,"signal real ticket drawable");
  require(drawable->presents==0,"signalDrawable after present");
  require(drawable->signals++==0,"drawable signaled twice");
}
- (void)dealloc { [handler release]; [super dealloc]; }
@end

static void exercise(id<MTLDevice> device, BOOL early, unsigned outcome) {
  // outcome: successful, restart epoch stale, or native feedback error.
  auto drawable=[OrderDrawable new];
  auto queue=[OrderQueue new]; queue->drawable=drawable; queue->early=early;
  auto feedback=[OrderFeedback new]; queue->feedback=feedback;
  if (outcome==2) feedback->failure=[[NSError errorWithDomain:MTL4CommandQueueErrorDomain
      code:MTL4CommandQueueErrorInternal userInfo:nil] retain];
  id<MTLSharedEvent> restart=[device newSharedEvent]; restart.signaledValue=7;
  uint64_t allocator=call(WMTMetal4NewAllocator,{handle(device)});
  uint64_t buffer=call(WMTMetal4NewCommandBuffer,{handle(device)});
  require(allocator && buffer && restart,"native bridge resources");
  call(WMTMetal4BeginCommandBuffer,{buffer,allocator});
  id<MTLBuffer> allocation=[device newBufferWithLength:16 options:MTLResourceStorageModeShared];
  require(allocation!=nil,"native residency allocation");
  call(WMTMetal4UseAllocation,{buffer,handle(allocation)});
  require(call(WMTMetal4FXReservePresent,{buffer,handle(drawable),0,0,0,0,handle(restart),7})==1,"production present reservation");
  id recording=member(reinterpret_cast<id>(buffer),"recording");
  id ticket=[[member(recording,"presentTickets") firstObject] retain];
  require(ticket!=nil,"production present ticket");
  queue->residency=(id<MTLResidencySet>)member(recording,"residency");
  call(WMTMetal4EndCommandBuffer,{buffer});
  if (outcome==1) restart.signaledValue=8;
  uint64_t submission=call(WMTMetal4Commit,{handle(queue),reinterpret_cast<uintptr_t>(&buffer),1});
  require(submission!=0 && queue->commits==1 && drawable->signals==1,"commit signals reserved drawable exactly once");
  require(member(reinterpret_cast<id>(buffer),"recording")==nil,"submitted recording detached");
  WMTMetal4SubmissionInfo info{};
  call(WMTMetal4GetSubmissionInfo,{submission,reinterpret_cast<uintptr_t>(&info)});
  if (!early) {
    require(!info.completed && !drawable->presents,"commit return alone cannot present or complete");
    require(queue->residency.allocationCount==1,"commit return alone cannot retire residency");
    [queue deliver];
  }
  call(WMTMetal4WaitSubmission,{submission});
  call(WMTMetal4GetSubmissionInfo,{submission,reinterpret_cast<uintptr_t>(&info)});
  require(info.completed && bool(info.error_handle)==(outcome==2),"completed submission feedback error");
  require(info.gpu_start_ns==1000000000 && info.gpu_end_ns==2000000000,"production feedback timestamps");
  require(drawable->presents==(outcome==0 ? 1u : 0u),"stale/error tickets skip presentation");
  require(queue->residency.allocationCount==0,"production retirement clears residency");
  // Ticket completion must remain idempotent, including failed/stale tickets.
  [ticket complete:YES];
  require(drawable->presents==(outcome==0 ? 1u : 0u) && drawable->signals==1,"ticket completion exactly once");
  // The production inFlight counter must permit a fresh recording after retirement.
  call(WMTMetal4BeginCommandBuffer,{buffer,allocator});
  call(WMTMetal4EndCommandBuffer,{buffer});
  std::printf("METAL4_PRESENT_ORDER_PASS feedback=%s outcome=%s signals=%u presents=%u retired=1\n",
      early ? "before-return" : "after-return",outcome==0 ? "success" : outcome==1 ? "stale" : "error",
      drawable->signals,drawable->presents);
  [ticket release]; [reinterpret_cast<id>(submission) release];
  [reinterpret_cast<id>(buffer) release]; [reinterpret_cast<id>(allocator) release];
  [allocation release]; [restart release]; [queue release]; [feedback release]; [drawable release];
}

int main(int argc, char **argv) {
  require(argc==2,"usage: owned-metal4-present-order <fresh-native-winemetal.so>");
  alarm(30); // Bound production completion wait; feedback is explicitly delivered, never polled.
  void *plugin=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
  if (!plugin) { std::fprintf(stderr,"METAL4_PRESENT_ORDER_FAIL dlopen=%s\n",dlerror()); return 1; }
  auto calls=reinterpret_cast<UnixCall *>(dlsym(plugin,"__wine_unix_call_funcs"));
  require(calls!=nullptr,"real Unix-plugin dispatch table"); bridge=calls[314];
  @autoreleasepool {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    require(device && [device supportsFamily:MTLGPUFamilyMetal4],"Metal4 device");
    Method method=class_getInstanceMethod(MTL4CommitOptions.class,@selector(addFeedbackHandler:));
    require(method!=nullptr,"SDK feedback registration method");
    IMP original=method_setImplementation(method,reinterpret_cast<IMP>(captureFeedback));
    for (BOOL early : {YES,NO}) for (unsigned outcome=0;outcome<3;++outcome) exercise(device,early,outcome);
    method_setImplementation(method,original);
    [device release];
  }
  return 0;
}
