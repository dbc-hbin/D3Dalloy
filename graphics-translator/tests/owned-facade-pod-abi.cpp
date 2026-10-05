// Compare the official FFX headers under application MSVC and facade MinGW
// targets. Constants are read from compiler IR, never from source spelling.
#include "../../src/fsr/include/upscalers/include/ffx_upscale.h"
#include "../../src/fsr/include/framegeneration/include/ffx_framegeneration.h"

#define LAYOUT(T) sizeof(T), alignof(T)
#define OFFSET(T, M) __builtin_offsetof(T, M)
extern "C" const unsigned int owned_facade_pod_layout[] = {
  sizeof(void *), sizeof(ffxReturnCode_t),
  LAYOUT(ffxApiHeader), OFFSET(ffxApiHeader, pNext),
  LAYOUT(ffxAllocationCallbacks),
  LAYOUT(ffxConfigureDescGlobalDebug1), OFFSET(ffxConfigureDescGlobalDebug1, fpMessage),
  LAYOUT(FfxApiResource), OFFSET(FfxApiResource, description), OFFSET(FfxApiResource, state),
  LAYOUT(ffxCreateContextDescUpscale), OFFSET(ffxCreateContextDescUpscale, fpMessage),
  LAYOUT(ffxDispatchDescUpscale), OFFSET(ffxDispatchDescUpscale, color),
  OFFSET(ffxDispatchDescUpscale, output), OFFSET(ffxDispatchDescUpscale, reset),
  LAYOUT(ffxCreateContextDescFrameGeneration),
  LAYOUT(ffxDispatchDescFrameGeneration), OFFSET(ffxDispatchDescFrameGeneration, outputs),
  OFFSET(ffxDispatchDescFrameGeneration, frameID),
  LAYOUT(ffxConfigureDescFrameGeneration),
  OFFSET(ffxConfigureDescFrameGeneration, presentCallback),
  OFFSET(ffxConfigureDescFrameGeneration, frameGenerationCallback),
  OFFSET(ffxConfigureDescFrameGeneration, HUDLessColor),
  OFFSET(ffxConfigureDescFrameGeneration, frameID),
};
