// SPDX-License-Identifier: LGPL-2.1-or-later
#include "include/upscalers/include/ffx_upscale.h"
#include "include/api/include/dx12/ffx_api_dx12.h"
#include "../d3d12/d3d12_interfaces.hpp"
#include "com/com_pointer.hpp"
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

using dxmt::Com;
namespace {
constexpr uint64_t kProvider = 0x4d46580000000001ull;
constexpr char kProviderName[] = "MetalFX (DXMT owned temporal upscaler; dxmt-owned-fsr-metalfx-v1)";
constexpr uint32_t kCreateFlags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
    FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
    FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
    FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE |
    FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
// Color-space/visualization create flags permit per-frame requests; dispatch
// selects the actual transfer function and diagnostic output.
// A binary marker, not another public export or an AMD model/version claim.
constexpr wchar_t kMarker[] = L"dxmt-owned-fsr-metalfx-v1: unsupported MetalFX operation";
struct Context {
  Context *next = nullptr;
  ffxContext handle = nullptr;
  LONG refs = 1;
  ffxAllocationCallbacks allocation{};
  Com<IUnknown> identity;
  Com<IMTLD3D12TemporalUpscaleState> backend;
  FfxApiDimensions2D render{}, output{};
  uint32_t flags = 0;
  ffxApiMessage message = nullptr;
  uint32_t debug = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Context *g_contexts = nullptr;
uintptr_t g_nextHandle = 1;
ffxApiMessage g_message = nullptr;
uint32_t g_debug = 0;

Context *lookup(ffxContext *handle) {
  if (!handle || !*handle) return nullptr;
  AcquireSRWLockShared(&g_lock);
  Context *found = g_contexts;
  while (found && found->handle != *handle) found = found->next;
  if (found) InterlockedIncrement(&found->refs);
  ReleaseSRWLockShared(&g_lock);
  return found;
}
void release(Context *context) {
  if (!context || InterlockedDecrement(&context->refs)) return;
  const auto allocation = context->allocation;
  context->~Context();
  if (allocation.dealloc) allocation.dealloc(allocation.pUserData, context);
  else std::free(context);
}
struct Lease {
  Context *context;
  ~Lease() { release(context); }
};
ffxReturnCode_t unsupportedType(ffxStructType_t type) {
  return (type & FFX_API_EFFECT_MASK) == FFX_API_EFFECT_ID_UPSCALE
      ? FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE : FFX_API_RETURN_NO_PROVIDER;
}
ffxReturnCode_t backendResult(HRESULT result) {
  if (SUCCEEDED(result)) return FFX_API_RETURN_OK;
  if (result == E_OUTOFMEMORY) return FFX_API_RETURN_ERROR_MEMORY;
  if (result == E_INVALIDARG || result == E_POINTER) return FFX_API_RETURN_ERROR_PARAMETER;
  if (result == E_NOTIMPL || result == E_NOINTERFACE || result == DXGI_ERROR_UNSUPPORTED)
    return FFX_API_RETURN_NO_PROVIDER;
  return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
}
void report(Context *context, ffxReturnCode_t code) {
  if (!code) return;
  AcquireSRWLockShared(&g_lock);
  const auto message = context && context->message ? context->message : g_message;
  const auto level = context && context->message ? context->debug : g_debug;
  ReleaseSRWLockShared(&g_lock);
  if (message && (level & FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS))
    message(FFX_API_MESSAGE_TYPE_ERROR, code == FFX_API_RETURN_NO_PROVIDER ? kMarker : L"DXMT MetalFX: invalid parameter or runtime failure");
}
bool validChain(const ffxApiHeader *head) {
  auto slow = head, fast = head;
  while (fast && fast->pNext) {
    slow = slow->pNext;
    fast = fast->pNext->pNext;
    if (slow == fast) return false;
  }
  return true;
}
// FFX states are not D3D12 bits. The native temporal command preserves the
// caller's logical D3D12 state and fences its Metal reads/writes independently.
bool resourceState(uint32_t state, D3D12_RESOURCE_STATES &mapped) {
  constexpr uint32_t reads = FFX_API_RESOURCE_STATE_COMPUTE_READ | FFX_API_RESOURCE_STATE_PIXEL_READ |
      FFX_API_RESOURCE_STATE_COPY_SRC | FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT;
  constexpr uint32_t exclusive = FFX_API_RESOURCE_STATE_COMMON | FFX_API_RESOURCE_STATE_UNORDERED_ACCESS |
      FFX_API_RESOURCE_STATE_COPY_DEST | FFX_API_RESOURCE_STATE_PRESENT |
      FFX_API_RESOURCE_STATE_RENDER_TARGET | FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
  if (!state || (state & ~(reads | exclusive)) ||
      ((state & (state - 1)) && state != FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ &&
       state != FFX_API_RESOURCE_STATE_GENERIC_READ)) return false;
  uint32_t bits = 0;
  if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) bits |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (state & FFX_API_RESOURCE_STATE_COMPUTE_READ) bits |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (state & FFX_API_RESOURCE_STATE_PIXEL_READ) bits |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if (state & FFX_API_RESOURCE_STATE_COPY_SRC) bits |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (state & FFX_API_RESOURCE_STATE_COPY_DEST) bits |= D3D12_RESOURCE_STATE_COPY_DEST;
  if (state & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) bits |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) bits |= D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (state & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT) bits |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
  mapped = static_cast<D3D12_RESOURCE_STATES>(bits); // COMMON and PRESENT both map to zero.
  return true;
}
ffxReturnCode_t resource(const FfxApiResource &api, Context &context, bool output, ID3D12Resource **result) {
  auto object = static_cast<ID3D12Resource *>(api.resource);
  if (!object) return FFX_API_RETURN_ERROR_PARAMETER;
  D3D12_RESOURCE_STATES state;
  if (!resourceState(api.state, state)) return FFX_API_RETURN_ERROR_PARAMETER;
  Com<ID3D12Device> device;
  Com<IUnknown> identity;
  if (FAILED(object->GetDevice(IID_PPV_ARGS(&device))) || !device ||
      FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || identity.ptr() != context.identity.ptr())
    return FFX_API_RETURN_ERROR_PARAMETER;
  const auto desc = object->GetDesc();
  const auto &metadata = api.description;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count != 1 || metadata.type != FFX_API_RESOURCE_TYPE_TEXTURE2D ||
      metadata.width != desc.Width || metadata.height != desc.Height || metadata.depth != desc.DepthOrArraySize ||
      (metadata.mipCount && metadata.mipCount != desc.MipLevels) || metadata.format != ffxApiGetSurfaceFormatDX12(desc.Format))
    return FFX_API_RETURN_ERROR_PARAMETER;
  if (metadata.flags & ~(FFX_API_RESOURCE_FLAGS_ALIASABLE)) return FFX_API_RETURN_ERROR_PARAMETER;
  if (output && (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
                 !(metadata.usage & FFX_API_RESOURCE_USAGE_UAV))) return FFX_API_RETURN_ERROR_PARAMETER;
  if (!output && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return FFX_API_RETURN_ERROR_PARAMETER;
  if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
    return FFX_API_RETURN_ERROR_PARAMETER;
  if ((state & D3D12_RESOURCE_STATE_DEPTH_WRITE) && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    return FFX_API_RETURN_ERROR_PARAMETER;
  if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    return FFX_API_RETURN_ERROR_PARAMETER;
  *result = object;
  return FFX_API_RETURN_OK;
}
ffxReturnCode_t dispatch(Context &context, const ffxDispatchDescUpscale &desc) {
  if (!desc.commandList || !desc.renderSize.width || !desc.renderSize.height ||
      desc.renderSize.width > context.render.width || desc.renderSize.height > context.render.height)
    return FFX_API_RETURN_ERROR_PARAMETER;
  for (float scalar : {desc.jitterOffset.x, desc.jitterOffset.y, desc.motionVectorScale.x,
      desc.motionVectorScale.y, desc.preExposure, desc.sharpness, desc.frameTimeDelta,
      desc.cameraNear, desc.cameraFar, desc.cameraFovAngleVertical, desc.viewSpaceToMetersFactor})
    if (!std::isfinite(scalar)) return FFX_API_RETURN_ERROR_PARAMETER;
  if (!desc.motionVectorScale.x || !desc.motionVectorScale.y || desc.preExposure <= 0 ||
      desc.sharpness < 0 || desc.sharpness > 1 || desc.frameTimeDelta < 0) return FFX_API_RETURN_ERROR_PARAMETER;
  if (desc.flags & ~(FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW | FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB |
                     FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ)) return FFX_API_RETURN_ERROR_PARAMETER;
  if ((desc.flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) &&
      (desc.flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ)) return FFX_API_RETURN_ERROR_PARAMETER;
  if ((desc.flags & FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW) &&
      !(context.flags & FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION)) return FFX_API_RETURN_ERROR_PARAMETER;
  if ((!desc.upscaleSize.width != !desc.upscaleSize.height) ||
      desc.upscaleSize.width > context.output.width || desc.upscaleSize.height > context.output.height)
    return FFX_API_RETURN_ERROR_PARAMETER;
  auto list = static_cast<ID3D12GraphicsCommandList *>(desc.commandList);
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
    return FFX_API_RETURN_ERROR_PARAMETER;
  Com<ID3D12Device> device;
  Com<IUnknown> identity;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device ||
      FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || identity.ptr() != context.identity.ptr())
    return FFX_API_RETURN_ERROR_PARAMETER;
  Com<IMTLD3D12GraphicsCommandListExt> extension;
  if (FAILED(list->QueryInterface(IID_PPV_ARGS(&extension))) || !extension) return FFX_API_RETURN_NO_PROVIDER;
  if (FAILED(extension->ValidateRecording())) return FFX_API_RETURN_ERROR_PARAMETER;
  MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC frame{};
  frame.StructSize = sizeof(frame);
  ffxReturnCode_t code;
  if ((code = resource(desc.color, context, false, &frame.Color)) ||
      (code = resource(desc.depth, context, false, &frame.Depth)) ||
      (code = resource(desc.motionVectors, context, false, &frame.MotionVector)) ||
      (code = resource(desc.output, context, true, &frame.Output))) return code;
  if (desc.reactive.resource && (code = resource(desc.reactive, context, false, &frame.BiasCurrentColorMask)))
    return code;
  if (desc.transparencyAndComposition.resource &&
      (code = resource(desc.transparencyAndComposition, context, false, &frame.TransparencyMask))) return code;
  if (desc.exposure.resource && !(context.flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE)) {
    if ((code = resource(desc.exposure, context, false, &frame.ExposureTexture))) return code;
    const auto exposure = frame.ExposureTexture->GetDesc();
    if (exposure.Width != 1 || exposure.Height != 1) return FFX_API_RETURN_ERROR_PARAMETER;
  }
  frame.InputContentWidth = desc.renderSize.width;
  frame.InputContentHeight = desc.renderSize.height;
  frame.OutputWidth = desc.upscaleSize.width ? desc.upscaleSize.width : context.output.width;
  frame.OutputHeight = desc.upscaleSize.height ? desc.upscaleSize.height : context.output.height;
  frame.ColorTransfer = (desc.flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) ? 1u :
      (desc.flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ) ? 2u : 0u;
  frame.CombineCompositionMask = desc.transparencyAndComposition.resource != nullptr;
  frame.EnableSharpening = desc.enableSharpening;
  frame.Sharpness = desc.sharpness;
  // Reference FSR deliberately centers/caps output above the device temporal scale limit.
  frame.CapOutputToTemporalMaxScale = TRUE;
  frame.DrawDebugView = (desc.flags & FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW) != 0;
  frame.JitterOffsetX = desc.jitterOffset.x;
  frame.JitterOffsetY = desc.jitterOffset.y;
  frame.MotionVectorScaleX = desc.motionVectorScale.x;
  frame.MotionVectorScaleY = desc.motionVectorScale.y;
  frame.PreExposure = desc.preExposure;
  frame.ResetHistory = desc.reset;
  return backendResult(extension->TemporalUpscale(context.backend.ptr(), &frame));
}
} // namespace

extern "C" ffxReturnCode_t ffxCreateContext(ffxContext *output, ffxCreateContextDescHeader *desc,
    const ffxAllocationCallbacks *callbacks) {
  if (!output) return FFX_API_RETURN_ERROR_PARAMETER;
  *output = nullptr;
  if (!desc || (callbacks && (!!callbacks->alloc != !!callbacks->dealloc)))
    return FFX_API_RETURN_ERROR_PARAMETER;
  if (!validChain(desc)) return FFX_API_RETURN_ERROR_PARAMETER;
  const ffxCreateContextDescUpscale *upscale = nullptr;
  const ffxCreateBackendDX12Desc *backend = nullptr;
  bool version = false, overrideVersion = false;
  for (auto entry = desc; entry; entry = entry->pNext) {
    switch (entry->type) {
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE:
      if (upscale) return FFX_API_RETURN_ERROR_PARAMETER;
      upscale = reinterpret_cast<const ffxCreateContextDescUpscale *>(entry); break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12:
      if (backend) return FFX_API_RETURN_ERROR_PARAMETER;
      backend = reinterpret_cast<const ffxCreateBackendDX12Desc *>(entry); break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION:
      if (version) return FFX_API_RETURN_ERROR_PARAMETER;
      version = true;
      if (reinterpret_cast<const ffxCreateContextDescUpscaleVersion *>(entry)->version != FFX_UPSCALER_VERSION)
        return FFX_API_RETURN_ERROR_PARAMETER;
      break;
    case FFX_API_DESC_TYPE_OVERRIDE_VERSION:
      if (overrideVersion) return FFX_API_RETURN_ERROR_PARAMETER;
      overrideVersion = true;
      if (reinterpret_cast<const ffxOverrideVersion *>(entry)->versionId != kProvider) return FFX_API_RETURN_NO_PROVIDER;
      break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS:
      return FFX_API_RETURN_NO_PROVIDER; // Native Metal allocations cannot honor D3D12 allocation callbacks.
    default: return unsupportedType(entry->type);
    }
  }
  if (!upscale || !backend || !backend->device || !upscale->maxRenderSize.width || !upscale->maxRenderSize.height ||
      !upscale->maxUpscaleSize.width || !upscale->maxUpscaleSize.height ||
      upscale->maxRenderSize.width > upscale->maxUpscaleSize.width ||
      upscale->maxRenderSize.height > upscale->maxUpscaleSize.height ||
      upscale->maxUpscaleSize.width > 16384 || upscale->maxUpscaleSize.height > 16384)
    return FFX_API_RETURN_ERROR_PARAMETER;
  if (upscale->flags & ~0x3ffu) return FFX_API_RETURN_ERROR_PARAMETER;
  if (upscale->flags & ~kCreateFlags) return FFX_API_RETURN_NO_PROVIDER;
  Com<IMTLD3D12DeviceExt> extension;
  Com<IUnknown> identity;
  if (FAILED(backend->device->QueryInterface(IID_PPV_ARGS(&identity)))) return FFX_API_RETURN_ERROR_PARAMETER;
  if (FAILED(backend->device->QueryInterface(IID_PPV_ARGS(&extension))) || !extension) return FFX_API_RETURN_NO_PROVIDER;
  BOOL supported = FALSE;
  const auto supportResult = extension->SupportsTemporalUpscale(&supported);
  if (FAILED(supportResult)) return backendResult(supportResult);
  if (!supported) return FFX_API_RETURN_NO_PROVIDER;
  MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC create{};
  create.StructSize = sizeof(create);
  create.InputWidth = upscale->maxRenderSize.width;
  create.InputHeight = upscale->maxRenderSize.height;
  create.OutputWidth = upscale->maxUpscaleSize.width;
  create.OutputHeight = upscale->maxUpscaleSize.height;
  // The COM ABI uses NGX semantic bits, not FFX bits.
  if (!(upscale->flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)) create.FeatureFlags |= 1u << 1;
  if (upscale->flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) create.FeatureFlags |= 1u << 2;
  if (upscale->flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) create.FeatureFlags |= 1u << 0;
  if (upscale->flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) create.FeatureFlags |= 1u << 3;
  if (upscale->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) create.FeatureFlags |= 1u << 6;
  create.DynamicOutputEnabled = TRUE;
  Com<IMTLD3D12TemporalUpscaleState> state;
  const auto result = extension->CreateTemporalUpscaleState(&create, &state);
  if (FAILED(result) || !state) return backendResult(FAILED(result) ? result : E_FAIL);
  void *memory = callbacks && callbacks->alloc ? callbacks->alloc(callbacks->pUserData, sizeof(Context)) : std::malloc(sizeof(Context));
  if (!memory) return FFX_API_RETURN_ERROR_MEMORY;
  auto context = new (memory) Context;
  if (callbacks) context->allocation = *callbacks;
  context->identity = std::move(identity);
  context->backend = std::move(state);
  context->render = upscale->maxRenderSize;
  context->output = upscale->maxUpscaleSize;
  context->flags = upscale->flags;
  context->message = upscale->fpMessage;
  AcquireSRWLockExclusive(&g_lock);
  // Never reuse opaque IDs: freed callback allocations must not revive stale handles.
  if (!g_nextHandle) {
    ReleaseSRWLockExclusive(&g_lock);
    release(context);
    return FFX_API_RETURN_ERROR_MEMORY;
  }
  context->handle = reinterpret_cast<ffxContext>(g_nextHandle++);
  context->next = g_contexts;
  g_contexts = context;
  *output = context->handle;
  ReleaseSRWLockExclusive(&g_lock);
  return FFX_API_RETURN_OK;
}
extern "C" ffxReturnCode_t ffxDestroyContext(ffxContext *handle, const ffxAllocationCallbacks *callbacks) {
  if (!handle || !*handle) return FFX_API_RETURN_ERROR_PARAMETER;
  AcquireSRWLockExclusive(&g_lock);
  auto link = &g_contexts;
  while (*link && (*link)->handle != *handle) link = &(*link)->next;
  auto context = *link;
  const ffxAllocationCallbacks empty{};
  const auto &allocation = callbacks ? *callbacks : empty;
  if (!context || context->allocation.alloc != allocation.alloc || context->allocation.dealloc != allocation.dealloc ||
      context->allocation.pUserData != allocation.pUserData) {
    ReleaseSRWLockExclusive(&g_lock);
    return FFX_API_RETURN_ERROR_PARAMETER;
  }
  *link = context->next;
  *handle = nullptr;
  ReleaseSRWLockExclusive(&g_lock);
  release(context);
  return FFX_API_RETURN_OK;
}
extern "C" ffxReturnCode_t ffxConfigure(ffxContext *handle, const ffxConfigureDescHeader *desc) {
  if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
  Lease lease{handle ? lookup(handle) : nullptr};
  if (handle && !lease.context) return FFX_API_RETURN_ERROR_PARAMETER;
  if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE) return FFX_API_RETURN_NO_PROVIDER;
  if (desc->type != FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1) return unsupportedType(desc->type);
  if (desc->pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  auto &debug = *reinterpret_cast<const ffxConfigureDescGlobalDebug1 *>(desc);
  if (debug.debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE &&
      debug.debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS &&
      debug.debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS &&
      debug.debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE) return FFX_API_RETURN_ERROR_PARAMETER;
  AcquireSRWLockExclusive(&g_lock);
  if (lease.context) { lease.context->message = debug.fpMessage; lease.context->debug = debug.debugLevel; }
  else { g_message = debug.fpMessage; g_debug = debug.debugLevel; }
  ReleaseSRWLockExclusive(&g_lock);
  return FFX_API_RETURN_OK;
}
extern "C" ffxReturnCode_t ffxDispatch(ffxContext *handle, const ffxDispatchDescHeader *desc) {
  Lease lease{lookup(handle)};
  if (!lease.context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
  ffxReturnCode_t result;
  if (desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK) result = FFX_API_RETURN_NO_PROVIDER;
  else if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) result = unsupportedType(desc->type);
  else if (desc->pNext) result = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  else result = dispatch(*lease.context, *reinterpret_cast<const ffxDispatchDescUpscale *>(desc));
  report(lease.context, result);
  return result;
}
extern "C" ffxReturnCode_t ffxQuery(ffxContext *handle, ffxQueryDescHeader *desc) {
  if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
  Lease lease{handle ? lookup(handle) : nullptr};
  if (handle && !lease.context) return FFX_API_RETURN_ERROR_PARAMETER;
  if (desc->pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  switch (desc->type) {
  case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
    auto &query = *reinterpret_cast<ffxQueryDescGetVersions *>(desc);
    if (query.createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) return FFX_API_RETURN_NO_PROVIDER;
    if (!query.outputCount) return FFX_API_RETURN_ERROR_PARAMETER;
    uint64_t count = 1;
    if (query.device) {
      Com<IMTLD3D12DeviceExt> extension;
      BOOL supported = FALSE;
      auto device = static_cast<ID3D12Device *>(query.device);
      if (FAILED(device->QueryInterface(IID_PPV_ARGS(&extension))) || !extension) count = 0;
      else {
        auto result = extension->SupportsTemporalUpscale(&supported);
        if (FAILED(result)) return backendResult(result);
        if (!supported) count = 0;
      }
    }
    const auto capacity = *query.outputCount;
    *query.outputCount = count;
    if (capacity && count) {
      if (query.versionIds) query.versionIds[0] = kProvider;
      if (query.versionNames) query.versionNames[0] = kProviderName;
    }
    return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
    if (!lease.context) return FFX_API_RETURN_ERROR_PARAMETER;
    auto &query = *reinterpret_cast<ffxQueryGetProviderVersion *>(desc);
    query.versionId = kProvider; query.versionName = kProviderName;
    return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode *>(desc);
    constexpr float ratios[] = {1, 1.5f, 1.7f, 2, 3};
    if (query.qualityMode > 4 || !query.pOutUpscaleRatio) return FFX_API_RETURN_ERROR_PARAMETER;
    *query.pOutUpscaleRatio = ratios[query.qualityMode]; return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode *>(desc);
    constexpr double ratios[] = {1, 1.5, 1.7, 2, 3};
    if (query.qualityMode > 4 || !query.displayWidth || !query.displayHeight ||
        !query.pOutRenderWidth || !query.pOutRenderHeight) return FFX_API_RETURN_ERROR_PARAMETER;
    const auto width = uint32_t(query.displayWidth / ratios[query.qualityMode]);
    const auto height = uint32_t(query.displayHeight / ratios[query.qualityMode]);
    if (!width || !height) return FFX_API_RETURN_ERROR_PARAMETER;
    *query.pOutRenderWidth = width; *query.pOutRenderHeight = height; return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetJitterPhaseCount *>(desc);
    if (!query.renderWidth || !query.displayWidth || !query.pOutPhaseCount) return FFX_API_RETURN_ERROR_PARAMETER;
    const double ratio = double(query.displayWidth) / query.renderWidth;
    const double phases = 8 * ratio * ratio;
    if (phases < 1 || phases > std::numeric_limits<int32_t>::max()) return FFX_API_RETURN_ERROR_PARAMETER;
    *query.pOutPhaseCount = int32_t(phases); return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetJitterOffset *>(desc);
    if (query.phaseCount <= 0 || !query.pOutX || !query.pOutY) return FFX_API_RETURN_ERROR_PARAMETER;
    const uint32_t index = uint32_t((int64_t(query.index) % query.phaseCount + query.phaseCount) % query.phaseCount) + 1;
    float offsets[2]{};
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const uint32_t base = axis + 2;
      float fraction = 1.0f / base;
      for (uint32_t sample = index; sample; sample /= base, fraction /= base)
        offsets[axis] += (sample % base) * fraction;
    }
    *query.pOutX = offsets[0] - 0.5f; *query.pOutY = offsets[1] - 0.5f; return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetResourceRequirements *>(desc);
    query.required_resources = FFX_API_QUERY_RESOURCE_INPUT_COLOR | FFX_API_QUERY_RESOURCE_INPUT_DEPTH | FFX_API_QUERY_RESOURCE_INPUT_MV;
    query.optional_resources = FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK |
        FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION;
    if (!lease.context || !(lease.context->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE))
      query.optional_resources |= FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE;
    return FFX_API_RETURN_OK;
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsage *>(desc);
    if (!lease.context || !query.gpuMemoryUsageUpscaler) return FFX_API_RETURN_ERROR_PARAMETER;
    return FFX_API_RETURN_NO_PROVIDER; // MetalFX exposes no measured allocation footprint.
  }
  case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2: {
    auto &query = *reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsageV2 *>(desc);
    if (!query.gpuMemoryUsageUpscaler || !query.device || !query.maxRenderSize.width || !query.maxRenderSize.height ||
        !query.maxUpscaleSize.width || !query.maxUpscaleSize.height ||
        query.maxRenderSize.width > query.maxUpscaleSize.width || query.maxRenderSize.height > query.maxUpscaleSize.height)
      return FFX_API_RETURN_ERROR_PARAMETER;
    return FFX_API_RETURN_NO_PROVIDER;
  }
  default: return unsupportedType(desc->type);
  }
}
