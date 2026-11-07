// Test-only MetalFX observer. The public guest arms one capture, submits the
// evaluation, then waits for its D3D fence before reading the GPU-written file.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <objc/runtime.h>
#include <crt_externs.h>
#include "owned-fg-contract.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
IMP originalCreate, originalEncode;
Class observedClass;
const char* directory;
id<MTLBuffer> previousBuffer;
id<MTLResidencySet> previousSet;

void failure(const char* step) {
  std::fprintf(stderr, "OWNED_FG_CONTRACT_OBSERVER_FAIL step=%s\n", step);
  std::abort();
}

void encode(id<MTL4FXFrameInterpolator> effect, SEL selector, id<MTL4CommandBuffer> command) {
  auto original = reinterpret_cast<void(*)(id,SEL,id)>(originalEncode);
  const auto armed = std::string(directory) + "/fg-contract.arm";
  FILE* marker = std::fopen(armed.c_str(),"rb");
  if (!marker) { original(effect,selector,command); return; }
  uint32_t serial = 0;
  const bool valid = std::fread(&serial,sizeof(serial),1,marker) == 1 && serial;
  std::fclose(marker);
  if (!valid || unlink(armed.c_str())) failure("request");
  // Each new request follows completion of the previous captured evaluation.
  [previousBuffer release]; [previousSet release];
  const auto path = std::string(directory) + "/fg-contract-" + std::to_string(serial) + ".bin";
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0 || ftruncate(fd,fgProbeBytes)) failure("create_file");
  auto ptr = mmap(nullptr,fgProbeBytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  close(fd);
  if (ptr == MAP_FAILED) failure("map");
  auto device = command.device;
  previousBuffer = [device newBufferWithBytesNoCopy:ptr length:fgProbeBytes options:MTLResourceStorageModeShared
      deallocator:^(void* p, NSUInteger bytes) { munmap(p,bytes); }];
  if (!previousBuffer) failure("shared_buffer");
  MTLResidencySetDescriptor* desc = [MTLResidencySetDescriptor new];
  previousSet = [device newResidencySetWithDescriptor:desc error:nil]; [desc release];
  if (!previousSet || !effect.fence) failure("residency_or_fence");
  [previousSet addAllocation:previousBuffer]; [previousSet commit]; [command useResidencySet:previousSet];
  auto& h = *static_cast<FgProbeFrame*>(ptr);
  h.serial=serial; h.effect=reinterpret_cast<uintptr_t>(effect); h.width=effect.depthTexture.width;
  h.height=effect.depthTexture.height; h.contentWidth=effect.contentWidth; h.contentHeight=effect.contentHeight;
  h.reset=effect.shouldResetHistory; h.reversed=effect.depthReversed; h.hasUI=effect.uiTexture!=nil;
  h.hasDistortion=effect.distortionTexture!=nil;
  h.jitter[0]=effect.jitterOffsetX; h.jitter[1]=effect.jitterOffsetY;
  h.motionScale[0]=effect.motionVectorScaleX; h.motionScale[1]=effect.motionVectorScaleY;
  h.nearPlane=effect.nearPlane; h.farPlane=effect.farPlane; h.aspect=effect.aspectRatio;
  h.fov=effect.fieldOfView; h.delta=effect.deltaTime;
  auto view=effect.worldToViewMatrix, projection=effect.viewToClipMatrix;
  std::memcpy(h.view,&view,sizeof(view)); std::memcpy(h.projection,&projection,sizeof(projection));
  if (effect.colorTexture.pixelFormat!=MTLPixelFormatRGBA16Float || effect.outputTexture.pixelFormat!=MTLPixelFormatRGBA16Float ||
      effect.depthTexture.pixelFormat!=MTLPixelFormatR32Float || effect.motionTexture.pixelFormat!=MTLPixelFormatRG16Float)
    failure("formats");
  original(effect,selector,command);
  auto pass = [command computeCommandEncoder];
  [pass waitForFence:effect.fence beforeEncoderStages:MTLStageBlit];
  id<MTLTexture> planes[] = {effect.colorTexture,effect.depthTexture,effect.motionTexture,effect.outputTexture};
  for (unsigned i=0;i<4;++i) {
    auto tex=planes[i];
    if (!tex || tex.width>fgProbeExtent || tex.height>fgProbeExtent ||
        ((i==0||i==3) && (tex.width!=fgProbeExtent || tex.height!=fgProbeExtent)) ||
        (i==2 && (tex.width!=h.width || tex.height!=h.height))) failure("extent");
    [pass copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(tex.width,tex.height,1) toBuffer:previousBuffer destinationOffset:fgProbeHeader+i*fgProbePlane
        destinationBytesPerRow:fgProbeRow destinationBytesPerImage:fgProbePlane];
  }
  [pass updateFence:effect.fence afterEncoderStages:MTLStageBlit]; [pass endEncoding];
}

id create(id descriptor, SEL selector, id device, id compiler) {
  id effect = reinterpret_cast<id(*)(id,SEL,id,id)>(originalCreate)(descriptor,selector,device,compiler);
  if (!effect) return effect;
  @synchronized([MTLFXFrameInterpolatorDescriptor class]) {
    Class cls=object_getClass(effect);
    if (observedClass && observedClass!=cls) failure("interpolator_class");
    if (!observedClass) {
      Method method=class_getInstanceMethod(cls,@selector(encodeToCommandBuffer:));
      if (!method) failure("encode_selector");
      originalEncode=method_getImplementation(method);
      class_replaceMethod(cls,@selector(encodeToCommandBuffer:),(IMP)encode,method_getTypeEncoding(method));
      observedClass=cls;
    }
  }
  return effect;
}

__attribute__((constructor)) void install() {
  directory=getenv("OWNED_FG_CONTRACT_DIR");
  if (!directory || !getenv("OWNED_FG_CONTRACT_PROBE")) return;
  const char* name=getenv("OWNED_FG_CONTRACT_CONSUMER");
  if (name) {
    bool matched=false;
    for (int i=0; i<*_NSGetArgc(); ++i) {
      const char* arg=(*_NSGetArgv())[i];
      const char* slash=std::strrchr(arg,'/');
      matched |= std::strcmp(slash ? slash+1 : arg,name)==0;
    }
    if (!matched) return;
  }
  @autoreleasepool {
    Method method=class_getInstanceMethod([MTLFXFrameInterpolatorDescriptor class],@selector(newFrameInterpolatorWithDevice:compiler:));
    if (!method) failure("factory_selector");
    originalCreate=method_setImplementation(method,(IMP)create);
    std::fprintf(stderr,"OWNED_FG_CONTRACT_OBSERVER_READY\n");
  }
}
}
