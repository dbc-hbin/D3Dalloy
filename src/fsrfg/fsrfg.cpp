// SPDX-License-Identifier: LGPL-2.1-or-later
// Lifecycle/callback rules ported from the Wine MetalFX FG builtin; the owned
// presentation implementation is the pinned MIT FidelityFX SDK, not a DLL.
#include "sdk/FrameInterpolationSwapchainDX12.h"
#include "../d3d12/d3d12_frame_generation_interfaces.hpp"
#include "com/com_pointer.hpp"
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

using d3dalloy::Com;
namespace {
constexpr uint64_t kProvider = 0x4d46584647000001ull;
constexpr char kName[] = "MetalFX frame generation (d3dalloy-owned-fsr-framegeneration-metalfx-v1)";
constexpr wchar_t kMarker[] = L"d3dalloy-owned-fsr-framegeneration-metalfx-v1: unsupported or invalid frame generation operation";
constexpr UINT kUnsupportedFlags = FFX_FRAMEGENERATION_FLAG_RESERVED_1 | FFX_FRAMEGENERATION_FLAG_RESERVED_2;
constexpr UINT kKnownFlags = kUnsupportedFlags | FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY |
    FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES |
    FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
struct Context;
struct Binding {
  Context* context;
  FfxApiPresentCallbackFunc present;
  void* presentUser;
  FfxApiFrameGenerationDispatchFunc generate;
  void* generateUser;
};
struct Snapshot {
  Snapshot* next;
  UINT64 frame;
  FfxApiResource hudless;
  UINT flags;
};
struct Context {
  Context* next = nullptr;
  ffxContext handle = nullptr;
  ffxAllocationCallbacks allocation{};
  LONG refs = 1;
  bool closing = false;
  bool swapchainContext = false;
  Com<IDXGISwapChain4> proxy;
  Com<IFrameInterpolationSwapChainDX12> attached;
  Com<IMTLD3D12FrameGenerationState> backend;
  Com<IUnknown> deviceIdentity;
  SRWLOCK configureLock = SRWLOCK_INIT;
  CRITICAL_SECTION dispatchLock{};
  Binding* binding = nullptr;
  Snapshot* snapshots = nullptr;
  UINT width = 0, height = 0, renderWidth = 0, renderHeight = 0;
  UINT flags = 0, hudlessFormat = 0, version = FFX_FRAMEGENERATION_MAKE_VERSION(3,1,5);
  UINT64 lastPrepare = 0;
  bool havePrepare = false, enabled = false;
  ffxApiMessage message = nullptr;
  UINT debug = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
  Context() { InitializeCriticalSection(&dispatchLock); }
  ~Context() { DeleteCriticalSection(&dispatchLock); }
};
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_changed = CONDITION_VARIABLE_INIT;
Context* g_contexts = nullptr;
uintptr_t g_next = 1;
ffxApiMessage g_message = nullptr;
UINT g_debug = 0;
struct Scope {
  static thread_local Scope* current;
  Scope* previous;
  Context* context;
  explicit Scope(Context* c) : previous(current), context(c) { current = this; }
  ~Scope() { current = previous; }
};
thread_local Scope* Scope::current = nullptr;
Context* lookup(ffxContext* handle) {
  if (!handle || !*handle) return nullptr;
  AcquireSRWLockExclusive(&g_lock);
  auto c = g_contexts;
  while (c && c->handle != *handle) c = c->next;
  if (c && !c->closing) ++c->refs; else c = nullptr;
  ReleaseSRWLockExclusive(&g_lock);
  return c;
}
struct Lease {
  Context* context;
  ~Lease() {
    if (!context) return;
    AcquireSRWLockExclusive(&g_lock);
    --context->refs;
    WakeAllConditionVariable(&g_changed);
    ReleaseSRWLockExclusive(&g_lock);
  }
};
struct DispatchLock {
  Context& context;
  explicit DispatchLock(Context& c) : context(c) { EnterCriticalSection(&context.dispatchLock); }
  ~DispatchLock() { LeaveCriticalSection(&context.dispatchLock); }
};
void* allocate(Context& c, size_t size) {
  return c.allocation.alloc ? c.allocation.alloc(c.allocation.pUserData, size) : std::malloc(size);
}
void deallocate(Context& c, void* ptr) {
  if (!ptr) return;
  if (c.allocation.dealloc) c.allocation.dealloc(c.allocation.pUserData, ptr); else std::free(ptr);
}
void releaseResource(FfxApiResource& r) {
  if (r.resource) static_cast<IUnknown*>(r.resource)->Release();
  r = {};
}
void clearSnapshots(Context& c) {
  while (c.snapshots) {
    auto old = c.snapshots;
    c.snapshots = old->next;
    releaseResource(old->hudless);
    deallocate(c, old);
  }
  c.havePrepare = false;
}
void freeContext(Context* c) {
  clearSnapshots(*c);
  deallocate(*c, c->binding);
  const auto allocation = c->allocation;
  c->~Context();
  if (allocation.dealloc) allocation.dealloc(allocation.pUserData, c); else std::free(c);
}
bool validChain(const ffxApiHeader* header) {
  unsigned count = 0;
  for (; header; header = header->pNext) if (++count > 64) return false;
  return true;
}
ffxReturnCode_t result(HRESULT hr) {
  if (SUCCEEDED(hr)) return FFX_API_RETURN_OK;
  if (hr == E_OUTOFMEMORY) return FFX_API_RETURN_ERROR_MEMORY;
  if (hr == E_INVALIDARG || hr == E_POINTER) return FFX_API_RETURN_ERROR_PARAMETER;
  if (hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == DXGI_ERROR_UNSUPPORTED) return FFX_API_RETURN_NO_PROVIDER;
  return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
}
ffxReturnCode_t sdkResult(FfxErrorCode code) {
  if (!code) return FFX_API_RETURN_OK;
  if (code == static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY)) return FFX_API_RETURN_ERROR_MEMORY;
  if (code == static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR)) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  return FFX_API_RETURN_ERROR_PARAMETER;
}
void report(Context* c, ffxReturnCode_t code) {
  if (!code) return;
  AcquireSRWLockShared(&g_lock);
  const auto callback = c && c->message ? c->message : g_message;
  const auto level = c && c->message ? c->debug : g_debug;
  ReleaseSRWLockShared(&g_lock);
  if (callback && (level & FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS)) callback(FFX_API_MESSAGE_TYPE_ERROR, kMarker);
}
bool validRect(Context& c, const FfxApiRect2D& r) {
  return (!r.width && !r.height) || (r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 &&
      static_cast<int64_t>(r.left) + r.width <= c.width && static_cast<int64_t>(r.top) + r.height <= c.height);
}
ffxReturnCode_t resource(Context& c, const FfxApiResource& r, ID3D12Resource** out, D3D12_RESOURCE_STATES* state) {
  *out = static_cast<ID3D12Resource*>(r.resource);
  if (!*out) return FFX_API_RETURN_ERROR_PARAMETER;
  constexpr UINT known = FFX_API_RESOURCE_STATE_COMMON | FFX_API_RESOURCE_STATE_UNORDERED_ACCESS |
      FFX_API_RESOURCE_STATE_COMPUTE_READ | FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COPY_SRC |
      FFX_API_RESOURCE_STATE_COPY_DEST | FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT | FFX_API_RESOURCE_STATE_PRESENT |
      FFX_API_RESOURCE_STATE_RENDER_TARGET | FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
  constexpr UINT reads = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
  if (!r.state || (r.state & ~known) || ((r.state & (r.state - 1)) && r.state != reads && r.state != FFX_API_RESOURCE_STATE_GENERIC_READ))
    return FFX_API_RETURN_ERROR_PARAMETER;
  Com<IUnknown> identity;
  if (FAILED((*out)->GetDevice(IID_PPV_ARGS(&identity))) || identity.ptr() != c.deviceIdentity.ptr())
    return FFX_API_RETURN_ERROR_PARAMETER;
  const auto desc = (*out)->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
      r.description.type != FFX_API_RESOURCE_TYPE_TEXTURE2D || r.description.width != desc.Width ||
      r.description.height != desc.Height || r.description.depth != desc.DepthOrArraySize ||
      (r.description.mipCount && r.description.mipCount != desc.MipLevels) ||
      r.description.format != ffxApiGetSurfaceFormatDX12(desc.Format)) return FFX_API_RETURN_ERROR_PARAMETER;
  *state = ffxGetDX12StateFromResourceState(static_cast<FfxApiResourceState>(r.state));
  return FFX_API_RETURN_OK;
}
Snapshot* snapshotFor(Context& c, UINT64 frame) {
  Snapshot* best = nullptr;
  for (auto s = c.snapshots; s; s = s->next) if (s->frame <= frame && (!best || best->frame < s->frame)) best = s;
  return best;
}
void retire(Context& c, UINT64 frame) {
  DispatchLock lock(c);
  UINT64 oldest = 0;
  BOOL pending = FALSE;
  if (FAILED(c.backend->RetireFrame(frame, &oldest, &pending))) return;
  const auto boundary = pending ? std::min(frame, oldest) : frame;
  auto floor = snapshotFor(c, boundary);
  for (auto link = &c.snapshots; *link;) {
    auto s = *link;
    if (s != floor && s->frame < boundary) {
      *link = s->next; releaseResource(s->hudless); deallocate(c, s);
    } else link = &s->next;
  }
}
template<class T>
ffxReturnCode_t prepare(Context& c, const T& desc, bool v2) {
  MTL_FRAME_GENERATION_PREPARE_DESC p{};
  p.StructSize = sizeof(p);
  p.frame_id = desc.frameID; p.flags = desc.flags;
  p.render_width = desc.renderSize.width; p.render_height = desc.renderSize.height;
  p.motion_width = (c.flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? c.width : p.render_width;
  p.motion_height = (c.flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? c.height : p.render_height;
  p.jitter_x = desc.jitterOffset.x; p.jitter_y = desc.jitterOffset.y;
  p.motion_scale_x = desc.motionVectorScale.x; p.motion_scale_y = desc.motionVectorScale.y;
  p.frame_time_delta_ms = desc.frameTimeDelta; p.camera_near = desc.cameraNear; p.camera_far = desc.cameraFar;
  p.camera_fov_vertical_radians = desc.cameraFovAngleVertical; p.view_space_to_meters = desc.viewSpaceToMetersFactor;
  if constexpr (std::is_same_v<T, ffxDispatchDescFrameGenerationPrepareV2>) {
    if (desc.header.pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    p.reset = desc.reset; p.camera_info_present = TRUE;
    std::memcpy(p.camera_position, desc.cameraPosition, sizeof(p.camera_position));
    std::memcpy(p.camera_up, desc.cameraUp, sizeof(p.camera_up));
    std::memcpy(p.camera_right, desc.cameraRight, sizeof(p.camera_right));
    std::memcpy(p.camera_forward, desc.cameraForward, sizeof(p.camera_forward));
  } else {
    // The public V1 unused_reset is deliberately ignored, as in the pinned provider.
    for (auto h = desc.header.pNext; h; h = h->pNext) {
      if (h->type != FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
      if (p.camera_info_present) return FFX_API_RETURN_ERROR_PARAMETER;
      auto camera = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareCameraInfo*>(h);
      std::memcpy(p.camera_position, camera->cameraPosition, sizeof(p.camera_position));
      std::memcpy(p.camera_up, camera->cameraUp, sizeof(p.camera_up));
      std::memcpy(p.camera_right, camera->cameraRight, sizeof(p.camera_right));
      std::memcpy(p.camera_forward, camera->cameraForward, sizeof(p.camera_forward));
      p.camera_info_present = TRUE;
    }
  }
  if (!desc.commandList || !p.render_width || !p.render_height || p.render_width > c.renderWidth || p.render_height > c.renderHeight ||
      !std::isfinite(p.jitter_x) || !std::isfinite(p.jitter_y) || !std::isfinite(p.motion_scale_x) || !std::isfinite(p.motion_scale_y) ||
      !std::isfinite(p.frame_time_delta_ms) || p.frame_time_delta_ms < 0 || !std::isfinite(p.camera_near) ||
      (!(c.flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) && !std::isfinite(p.camera_far)) ||
      !std::isfinite(p.camera_fov_vertical_radians) || p.camera_fov_vertical_radians <= 0 || p.camera_fov_vertical_radians >= 3.141592654f ||
      !std::isfinite(p.view_space_to_meters) || desc.depth.resource == desc.motionVectors.resource || (p.flags & ~kKnownFlags))
    return FFX_API_RETURN_ERROR_PARAMETER;
  if (p.flags & kUnsupportedFlags) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  // Only immutable algorithm/debug bits cross the renderer Prepare contract.
  p.flags &= 7u;
  if (v2 && c.version < FFX_FRAMEGENERATION_VERSION) return FFX_API_RETURN_ERROR_PARAMETER;
  if (p.camera_info_present) for (UINT i = 0; i < 3; ++i)
    if (!std::isfinite(p.camera_position[i]) || !std::isfinite(p.camera_up[i]) || !std::isfinite(p.camera_right[i]) || !std::isfinite(p.camera_forward[i]))
      return FFX_API_RETURN_ERROR_PARAMETER;
  auto code = resource(c, desc.depth, &p.depth, &p.depth_state);
  if (!code) code = resource(c, desc.motionVectors, &p.motion_vectors, &p.motion_vectors_state);
  if (code) return code;
  Com<IMTLD3D12FrameGenerationCommandList> list;
  auto hr = static_cast<IUnknown*>(desc.commandList)->QueryInterface(IID_PPV_ARGS(&list));
  if (FAILED(hr)) return result(hr);
  p.reset |= c.havePrepare && desc.frameID != c.lastPrepare + 1;
  hr = list->PrepareFrameGeneration(c.backend.ptr(), &p);
  if (SUCCEEDED(hr)) { c.lastPrepare = desc.frameID; c.havePrepare = true; }
  return result(hr);
}
ffxReturnCode_t generate(Context& c, const ffxDispatchDescFrameGeneration& desc, bool* submitted) {
  if (desc.header.pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  if (!desc.commandList || desc.numGeneratedFrames < 1 || desc.numGeneratedFrames > 4 ||
      desc.backbufferTransferFunction > FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB || !validRect(c, desc.generationRect) ||
      !std::isfinite(desc.minMaxLuminance[0]) || !std::isfinite(desc.minMaxLuminance[1]) || desc.minMaxLuminance[0] > desc.minMaxLuminance[1] ||
      (desc.backbufferTransferFunction == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ && desc.minMaxLuminance[1] <= 0) ||
      (desc.backbufferTransferFunction == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB && desc.minMaxLuminance[1] <= desc.minMaxLuminance[0]))
    return FFX_API_RETURN_ERROR_PARAMETER;
  if (desc.numGeneratedFrames != 1) return FFX_API_RETURN_NO_PROVIDER;
  MTL_FRAME_GENERATION_GENERATE_DESC p{};
  p.StructSize = sizeof(p); p.frame_id = desc.frameID; p.num_generated_frames = desc.numGeneratedFrames;
  // Core selects this frame's immutable Prepare, not the newest recorded one.
  p.reset = desc.reset;
  p.backbuffer_transfer_function = desc.backbufferTransferFunction;
  p.generation_rect_left = desc.generationRect.left; p.generation_rect_top = desc.generationRect.top;
  p.generation_rect_width = desc.generationRect.width; p.generation_rect_height = desc.generationRect.height;
  p.min_luminance = desc.minMaxLuminance[0]; p.max_luminance = desc.minMaxLuminance[1];
  auto code = resource(c, desc.presentColor, &p.present_color, &p.present_color_state);
  auto config = snapshotFor(c, desc.frameID);
  if (config) p.flags = config->flags & 7u;
  if (!code && config && config->hudless.resource) code = resource(c, config->hudless, &p.hudless_color, &p.hudless_color_state);
  if (code) return code;
  const auto color = p.present_color->GetDesc();
  if (color.Width != c.width || color.Height != c.height) return FFX_API_RETURN_ERROR_PARAMETER;
  code = resource(c, desc.outputs[0], &p.outputs[0], &p.output_states[0]);
  if (code) return code;
  const auto output = p.outputs[0]->GetDesc();
  if (p.outputs[0] == p.present_color || p.outputs[0] == p.hudless_color ||
      output.Width != color.Width || output.Height != color.Height || output.Format != color.Format ||
      output.MipLevels != 1 || !(output.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    return FFX_API_RETURN_ERROR_PARAMETER;
  Com<IMTLD3D12FrameGenerationCommandList> list;
  auto hr = static_cast<IUnknown*>(desc.commandList)->QueryInterface(IID_PPV_ARGS(&list));
  if (FAILED(hr)) return result(hr);
  hr = list->GenerateFrames(c.backend.ptr(), &p);
  if (submitted) *submitted = SUCCEEDED(hr);
  return result(hr);
}
ffxReturnCode_t dispatch(Context& c, const ffxDispatchDescHeader* desc, bool* submitted = nullptr) {
  if (!validChain(desc)) return FFX_API_RETURN_ERROR_PARAMETER;
  DispatchLock lock(c);
  switch (desc->type) {
  case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE:
    return prepare(c, *reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare*>(desc), false);
  case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2:
    return prepare(c, *reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2*>(desc), true);
  case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION:
    return generate(c, *reinterpret_cast<const ffxDispatchDescFrameGeneration*>(desc), submitted);
  default: return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  }
}
ffxReturnCode_t generationCallback(ffxDispatchDescFrameGeneration* desc, void* user) {
  auto b = static_cast<Binding*>(user);
  if (!desc || !b) return FFX_API_RETURN_ERROR_PARAMETER;
  Scope scope(b->context);
  const auto originalFrame = desc->frameID;
  if (!b->generate) desc->numGeneratedFrames = 1;
  auto code = b->generate ? b->generate(desc, b->generateUser) : dispatch(*b->context, &desc->header);
  if (code) desc->numGeneratedFrames = 0;
  if (desc->frameID != originalFrame || desc->numGeneratedFrames > 1) {
    desc->numGeneratedFrames = 0; return FFX_API_RETURN_ERROR_PARAMETER;
  }
  retire(*b->context, originalFrame);
  return code;
}
ffxReturnCode_t presentCallback(ffxCallbackDescFrameGenerationPresent* desc, void* user) {
  auto b = static_cast<Binding*>(user);
  Scope scope(b->context);
  return b->present(desc, b->presentUser);
}
ffxReturnCode_t detach(Context& c) {
  if (!c.attached) return FFX_API_RETURN_OK;
  FfxFrameGenerationConfig off{};
  off.header.type = FFX_API_FRAME_GENERATION_CONFIG;
  off.swapChain = c.attached.ptr();
  c.attached->setFrameGenerationConfig(&off);
  if (!c.attached->waitForPresents()) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  AcquireSRWLockExclusive(&g_lock);
  c.attached = nullptr;
  ReleaseSRWLockExclusive(&g_lock);
  deallocate(c, c.binding); c.binding = nullptr;
  return FFX_API_RETURN_OK;
}
ffxReturnCode_t configureGeneration(Context& c, const ffxConfigureDescFrameGeneration& desc) {
  if (desc.header.pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  if ((desc.flags & ~kKnownFlags) || !validRect(c, desc.generationRect)) return FFX_API_RETURN_ERROR_PARAMETER;
  if (desc.flags & kUnsupportedFlags) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  if (desc.HUDLessColor.resource && desc.HUDLessColor.description.format != c.hudlessFormat) return FFX_API_RETURN_ERROR_PARAMETER;
  const bool notify = !(desc.flags & FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY);
  Com<IFrameInterpolationSwapChainDX12> sc;
  if (notify) {
    if (!desc.swapChain || FAILED(static_cast<IUnknown*>(desc.swapChain)->QueryInterface(IID_IFrameInterpolationSwapChainDX12, reinterpret_cast<void**>(&sc))))
      return FFX_API_RETURN_ERROR_PARAMETER;
    if (c.attached && c.attached.ptr() != sc.ptr()) {
      if (c.enabled) return FFX_API_RETURN_ERROR_PARAMETER;
      auto code = detach(c); if (code) return code;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (auto other = g_contexts; other; other = other->next) if (other != &c && other->attached.ptr() == sc.ptr()) {
      ReleaseSRWLockExclusive(&g_lock); return FFX_API_RETURN_ERROR_PARAMETER;
    }
    ReleaseSRWLockExclusive(&g_lock);
  }
  Binding* binding = c.binding;
  const bool changed = notify && (!binding || binding->present != desc.presentCallback || binding->presentUser != desc.presentCallbackUserContext ||
      binding->generate != desc.frameGenerationCallback || binding->generateUser != desc.frameGenerationCallbackUserContext);
  if (changed) {
    binding = static_cast<Binding*>(allocate(c, sizeof(Binding)));
    if (!binding) return FFX_API_RETURN_ERROR_MEMORY;
    *binding = {&c, desc.presentCallback, desc.presentCallbackUserContext, desc.frameGenerationCallback, desc.frameGenerationCallbackUserContext};
  }
  Snapshot* snapshot = nullptr;
  bool redundant = false;
  if (desc.frameGenerationEnabled) {
    {
      DispatchLock lock(c);
      // A repeated Configure for a recorded frame with identical values replaces
      // nothing. Distinct frame IDs still count toward the 64-snapshot backpressure.
      auto current = snapshotFor(c, desc.frameID);
      redundant = current && current->frame == desc.frameID && current->flags == desc.flags &&
          current->hudless.resource == desc.HUDLessColor.resource && current->hudless.state == desc.HUDLessColor.state &&
          !std::memcmp(&current->hudless.description, &desc.HUDLessColor.description, sizeof(desc.HUDLessColor.description));
      UINT count = 0; bool replacing = false;
      for (auto s = c.snapshots; s; s = s->next) { ++count; replacing |= s->frame == desc.frameID; }
      if (!redundant && count >= 64 && !replacing) { if (changed) deallocate(c, binding); return FFX_API_RETURN_ERROR_RUNTIME_ERROR; }
    }
  }
  if (desc.frameGenerationEnabled && !redundant) {
    snapshot = static_cast<Snapshot*>(allocate(c, sizeof(Snapshot)));
    if (!snapshot) { if (changed) deallocate(c, binding); return FFX_API_RETURN_ERROR_MEMORY; }
    *snapshot = {nullptr, desc.frameID, desc.HUDLessColor, desc.flags};
    if (snapshot->hudless.resource) static_cast<IUnknown*>(snapshot->hudless.resource)->AddRef();
  }
  bool claimed = false;
  if (notify && !c.attached) {
    AcquireSRWLockExclusive(&g_lock);
    for (auto other = g_contexts; other; other = other->next) if (other != &c && other->attached.ptr() == sc.ptr()) {
      ReleaseSRWLockExclusive(&g_lock);
      if (snapshot) { releaseResource(snapshot->hudless); deallocate(c, snapshot); }
      if (changed) deallocate(c, binding);
      return FFX_API_RETURN_ERROR_PARAMETER;
    }
    c.attached = sc; claimed = true;
    ReleaseSRWLockExclusive(&g_lock);
  }
  // Configure(FALSE) runs only through this context, so an enabled context is already ON.
  if (desc.frameGenerationEnabled && !c.enabled) {
    auto code = result(c.backend->Configure(TRUE));
    if (code) {
      if (claimed) { AcquireSRWLockExclusive(&g_lock); c.attached = nullptr; ReleaseSRWLockExclusive(&g_lock); }
      if (snapshot) { releaseResource(snapshot->hudless); deallocate(c, snapshot); }
      if (changed) deallocate(c, binding); return code;
    }
  }
  if (snapshot) {
    DispatchLock lock(c);
    // Immutable history selects the configuration at/before each Prepare frame.
    auto link = &c.snapshots;
    while (*link && (*link)->frame != snapshot->frame) link = &(*link)->next;
    if (*link) {
      auto old = *link; snapshot->next = old->next; *link = snapshot;
      releaseResource(old->hudless); deallocate(c, old);
    } else { snapshot->next = c.snapshots; c.snapshots = snapshot; }
  }
  if (notify) {
    FfxFrameGenerationConfig config{};
    config.header.type = FFX_API_FRAME_GENERATION_CONFIG; config.swapChain = sc.ptr();
    config.presentCallback = binding->present ? presentCallback : nullptr;
    config.presentCallbackContext = binding->present ? binding : nullptr;
    config.frameGenerationCallback = generationCallback; config.frameGenerationCallbackContext = binding;
    config.frameGenerationEnabled = desc.frameGenerationEnabled; config.allowAsyncWorkloads = desc.allowAsyncWorkloads;
    config.HUDLessColor = desc.HUDLessColor; config.flags = desc.flags;
    config.onlyPresentInterpolated = desc.onlyPresentGenerated || (desc.flags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW);
    config.interpolationRect = desc.generationRect; config.frameID = desc.frameID;
    config.drawDebugPacingLines = (desc.flags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES) != 0;
    // No registry or dispatch lock may be held while the proxy drains callbacks.
    sc->setFrameGenerationConfig(&config);
    if (changed) deallocate(c, c.binding);
    c.binding = binding;
    AcquireSRWLockExclusive(&g_lock);
    c.attached = std::move(sc);
    ReleaseSRWLockExclusive(&g_lock);
  }
  c.enabled = desc.frameGenerationEnabled;
  if (!c.enabled) {
    const bool drained = !c.attached || c.attached->waitForPresents();
    DispatchLock lock(c);
    const auto code = result(c.backend->Configure(FALSE));
    if (!code) clearSnapshots(c);
    return code ? code : drained ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  }
  return FFX_API_RETURN_OK;
}
// Adapted from ffx_provider_fsr3framegenerationswapchain_dx12.cpp at the pinned
// MIT revision: direct public descriptors avoid a second provider registry.
ffxReturnCode_t createSwapchain(Context& c, const ffxApiHeader* h) {
  FfxSwapchain output = nullptr;
  IDXGISwapChain4** caller = nullptr;
  Com<IMTLD3D12FrameGenerationDevice> owned;
  FfxErrorCode code;
  switch (h->type) {
  case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: {
    auto d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12*>(h);
    if (!d->swapchain || !*d->swapchain || !d->gameQueue) return FFX_API_RETURN_ERROR_PARAMETER;
    if (FAILED(d->gameQueue->GetDevice(IID_PPV_ARGS(&owned)))) return FFX_API_RETURN_NO_PROVIDER;
    caller = d->swapchain; output = *caller;
    code = ffxReplaceSwapchainForFrameinterpolationDX12(d->gameQueue, output);
    *caller = static_cast<IDXGISwapChain4*>(output); break;
  }
  case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12: {
    auto d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12*>(h);
    if (!d->swapchain || !d->desc || !d->dxgiFactory || !d->gameQueue) return FFX_API_RETURN_ERROR_PARAMETER;
    if (FAILED(d->gameQueue->GetDevice(IID_PPV_ARGS(&owned)))) return FFX_API_RETURN_NO_PROVIDER;
    caller = d->swapchain;
    code = ffxCreateFrameinterpolationSwapchainDX12(d->desc, d->gameQueue, d->dxgiFactory, output); break;
  }
  case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12: {
    auto d = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*>(h);
    if (!d->swapchain || !d->desc || !d->dxgiFactory || !d->gameQueue || !d->hwnd) return FFX_API_RETURN_ERROR_PARAMETER;
    if (FAILED(d->gameQueue->GetDevice(IID_PPV_ARGS(&owned)))) return FFX_API_RETURN_NO_PROVIDER;
    caller = d->swapchain;
    code = ffxCreateFrameinterpolationSwapchainForHwndDX12(d->hwnd, d->desc, d->fullscreenDesc, d->gameQueue, d->dxgiFactory, output); break;
  }
  default: return FFX_API_RETURN_NO_PROVIDER;
  }
  if (code) return sdkResult(code);
  *caller = static_cast<IDXGISwapChain4*>(output);
  c.proxy = *caller; // Context owns an additional reference; caller owns creation reference.
  c.swapchainContext = true;
  return FFX_API_RETURN_OK;
}
} // namespace

extern "C" ffxReturnCode_t ffxCreateContext(ffxContext* output, ffxCreateContextDescHeader* header, const ffxAllocationCallbacks* allocation) {
  if (!output) return FFX_API_RETURN_ERROR_PARAMETER;
  *output = nullptr;
  if (!header || !validChain(header) || (allocation && (!!allocation->alloc != !!allocation->dealloc))) return FFX_API_RETURN_ERROR_PARAMETER;
  const ffxCreateContextDescFrameGeneration* fg = nullptr;
  const ffxCreateBackendDX12Desc* backend = nullptr;
  const ffxApiHeader* sc = nullptr;
  UINT version = FFX_FRAMEGENERATION_MAKE_VERSION(3,1,5), scVersion = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(3,1,5), hudless = 0;
  for (auto h = header; h; h = h->pNext) {
    switch (h->type) {
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION:
      if (fg) return FFX_API_RETURN_ERROR_PARAMETER;
      fg = reinterpret_cast<const ffxCreateContextDescFrameGeneration*>(h); break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12:
      if (backend) return FFX_API_RETURN_ERROR_PARAMETER;
      backend = reinterpret_cast<const ffxCreateBackendDX12Desc*>(h); break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12:
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12:
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12:
      if (sc) return FFX_API_RETURN_ERROR_PARAMETER; sc = h; break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION:
      version = reinterpret_cast<const ffxCreateContextDescFrameGenerationVersion*>(h)->version; break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12:
      scVersion = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainVersionDX12*>(h)->version; break;
    case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS:
      hudless = reinterpret_cast<const ffxCreateContextDescFrameGenerationHudless*>(h)->hudlessBackBufferFormat; break;
    case FFX_API_DESC_TYPE_OVERRIDE_VERSION:
      if (reinterpret_cast<const ffxOverrideVersion*>(h)->versionId != kProvider) return FFX_API_RETURN_NO_PROVIDER; break;
    default: return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
  }
  if (!!fg == !!sc || (fg && (!backend || !backend->device))) return FFX_API_RETURN_ERROR_PARAMETER;
  if (version < FFX_FRAMEGENERATION_MAKE_VERSION(3,1,5) || version > FFX_FRAMEGENERATION_VERSION ||
      scVersion < FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(3,1,5) || scVersion > FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION)
    return FFX_API_RETURN_ERROR_PARAMETER;
  void* memory = allocation && allocation->alloc ? allocation->alloc(allocation->pUserData, sizeof(Context)) : std::malloc(sizeof(Context));
  if (!memory) return FFX_API_RETURN_ERROR_MEMORY;
  auto c = new (memory) Context;
  if (allocation) c->allocation = *allocation;
  c->version = fg ? version : scVersion;
  ffxReturnCode_t code;
  if (fg) {
    constexpr UINT known = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
        FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION | FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
        FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE | FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    if (!fg->displaySize.width || !fg->displaySize.height || !fg->maxRenderSize.width || !fg->maxRenderSize.height ||
        fg->maxRenderSize.width > fg->displaySize.width || fg->maxRenderSize.height > fg->displaySize.height || (fg->flags & ~known)) {
      freeContext(c); return FFX_API_RETURN_ERROR_PARAMETER;
    }
    c->width = fg->displaySize.width; c->height = fg->displaySize.height;
    c->renderWidth = fg->maxRenderSize.width; c->renderHeight = fg->maxRenderSize.height;
    c->flags = fg->flags; c->hudlessFormat = hudless ? hudless : fg->backBufferFormat;
    MTL_FRAME_GENERATION_CREATE_DESC desc{};
    desc.StructSize = sizeof(desc); desc.display_width = c->width; desc.display_height = c->height;
    desc.max_render_width = c->renderWidth; desc.max_render_height = c->renderHeight;
    desc.backbuffer_format = ffxGetDX12FormatFromSurfaceFormat(static_cast<FfxApiSurfaceFormat>(fg->backBufferFormat));
    if (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) desc.flags |= 1;
    if (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) desc.flags |= 2;
    if (fg->flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) desc.flags |= 4;
    if (fg->flags & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) desc.flags |= 8;
    Com<IMTLD3D12FrameGenerationDevice> extension;
    auto device = static_cast<IUnknown*>(backend->device);
    auto hr = device->QueryInterface(IID_PPV_ARGS(&extension));
    if (SUCCEEDED(hr)) hr = device->QueryInterface(IID_PPV_ARGS(&c->deviceIdentity));
    if (SUCCEEDED(hr)) hr = extension->CreateFrameGenerationState(&desc, &c->backend);
    code = result(hr);
    if (!code && !c->backend) code = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  } else code = createSwapchain(*c, sc);
  if (code) { freeContext(c); return code; }
  AcquireSRWLockExclusive(&g_lock);
  if (!g_next) { ReleaseSRWLockExclusive(&g_lock); freeContext(c); return FFX_API_RETURN_ERROR_MEMORY; }
  c->handle = reinterpret_cast<ffxContext>(g_next++);
  c->next = g_contexts; g_contexts = c; *output = c->handle;
  ReleaseSRWLockExclusive(&g_lock);
  return FFX_API_RETURN_OK;
}
extern "C" ffxReturnCode_t ffxDestroyContext(ffxContext* handle, const ffxAllocationCallbacks* allocation) {
  if (!handle || !*handle || Scope::current) return FFX_API_RETURN_ERROR_PARAMETER;
  AcquireSRWLockExclusive(&g_lock);
  auto c = g_contexts;
  while (c && c->handle != *handle) c = c->next;
  const ffxAllocationCallbacks empty{};
  const auto& a = allocation ? *allocation : empty;
  if (!c || c->closing || c->allocation.alloc != a.alloc || c->allocation.dealloc != a.dealloc || c->allocation.pUserData != a.pUserData) {
    ReleaseSRWLockExclusive(&g_lock); return FFX_API_RETURN_ERROR_PARAMETER;
  }
  c->closing = true;
  ReleaseSRWLockExclusive(&g_lock);
  AcquireSRWLockExclusive(&c->configureLock);
  auto code = detach(*c);
  ReleaseSRWLockExclusive(&c->configureLock);
  AcquireSRWLockExclusive(&g_lock);
  if (code) { c->closing = false; ReleaseSRWLockExclusive(&g_lock); return code; }
  while (c->refs > 1) SleepConditionVariableSRW(&g_changed, &g_lock, INFINITE, 0);
  auto link = &g_contexts;
  while (*link != c) link = &(*link)->next;
  *link = c->next; *handle = nullptr;
  ReleaseSRWLockExclusive(&g_lock);
  freeContext(c);
  return FFX_API_RETURN_OK;
}
extern "C" ffxReturnCode_t ffxConfigure(ffxContext* handle, const ffxConfigureDescHeader* header) {
  if (!header || !validChain(header) || Scope::current) return FFX_API_RETURN_ERROR_PARAMETER;
  Lease lease{lookup(handle)};
  auto c = lease.context;
  if (handle && !c) return FFX_API_RETURN_ERROR_PARAMETER;
  if (header->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1) {
    auto d = reinterpret_cast<const ffxConfigureDescGlobalDebug1*>(header);
    if (header->pNext || (d->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE && d->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS &&
        d->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS && d->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE)) return FFX_API_RETURN_ERROR_PARAMETER;
    AcquireSRWLockExclusive(&g_lock);
    if (c) { c->message = d->fpMessage; c->debug = d->debugLevel; } else { g_message = d->fpMessage; g_debug = d->debugLevel; }
    ReleaseSRWLockExclusive(&g_lock); return FFX_API_RETURN_OK;
  }
  if (!c) return FFX_API_RETURN_NO_PROVIDER;
  AcquireSRWLockExclusive(&c->configureLock);
  AcquireSRWLockShared(&g_lock); const bool closing = c->closing; ReleaseSRWLockShared(&g_lock);
  ffxReturnCode_t code = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  if (closing) code = FFX_API_RETURN_ERROR_PARAMETER;
  else if (c->swapchainContext) {
    if (header->pNext) code = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    else if (header->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
      auto d = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*>(header);
      if (d->flags & ~(FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA | FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)) code = FFX_API_RETURN_ERROR_PARAMETER;
      else code = sdkResult(ffxRegisterFrameinterpolationUiResourceDX12(c->proxy.ptr(), d->uiResource, d->flags));
    } else if (header->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12) {
      auto d = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainKeyValueDX12*>(header);
      code = sdkResult(ffxConfigureFrameInterpolationSwapchainDX12(c->proxy.ptr(), static_cast<FfxFrameInterpolationSwapchainConfigureKey>(d->key), d->ptr));
    } else code = FFX_API_RETURN_ERROR_PARAMETER;
  } else if (header->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION)
    code = configureGeneration(*c, *reinterpret_cast<const ffxConfigureDescFrameGeneration*>(header));
  ReleaseSRWLockExclusive(&c->configureLock);
  report(c, code); return code;
}
extern "C" ffxReturnCode_t ffxDispatch(ffxContext* handle, const ffxDispatchDescHeader* header) {
  if (!header || !validChain(header)) return FFX_API_RETURN_ERROR_PARAMETER;
  Lease lease{lookup(handle)};
  auto c = lease.context;
  if (!c) return FFX_API_RETURN_ERROR_PARAMETER;
  ffxReturnCode_t code;
  if (c->swapchainContext) {
    if (header->type != FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12 || header->pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    code = sdkResult(ffxWaitForPresents(c->proxy.ptr()));
  } else {
    bool submitted = false;
    code = dispatch(*c, header, &submitted);
    if (submitted && !Scope::current)
      retire(*c, reinterpret_cast<const ffxDispatchDescFrameGeneration*>(header)->frameID);
  }
  report(c, code); return code;
}
extern "C" ffxReturnCode_t ffxQuery(ffxContext* handle, ffxQueryDescHeader* header) {
  if (!header) return FFX_API_RETURN_ERROR_PARAMETER;
  FfxApiEffectMemoryUsage* memory = nullptr;
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE) memory = reinterpret_cast<ffxQueryDescFrameGenerationGetGPUMemoryUsage*>(header)->gpuMemoryUsageFrameGeneration;
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2) memory = reinterpret_cast<ffxQueryDescFrameGenerationGetGPUMemoryUsageV2*>(header)->gpuMemoryUsageFrameGeneration;
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12) memory = reinterpret_cast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12*>(header)->gpuMemoryUsageFrameGenerationSwapchain;
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2) memory = reinterpret_cast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2*>(header)->gpuMemoryUsageFrameGenerationSwapchain;
  if (memory) *memory = {};
  if (!validChain(header) || header->pNext) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
  Lease lease{lookup(handle)};
  auto c = lease.context;
  if (handle && !c) return FFX_API_RETURN_ERROR_PARAMETER;
  if (header->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
    auto d = reinterpret_cast<ffxQueryDescGetVersions*>(header);
    if ((d->createDescType & FFX_API_EFFECT_MASK) != FFX_API_EFFECT_ID_FRAMEGENERATION &&
        (d->createDescType & FFX_API_EFFECT_MASK) != FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN) return FFX_API_RETURN_NO_PROVIDER;
    if (d->outputCount) { auto capacity = *d->outputCount; *d->outputCount = 1;
      if (capacity) { if (d->versionIds) d->versionIds[0] = kProvider; if (d->versionNames) d->versionNames[0] = kName; } }
    return FFX_API_RETURN_OK;
  }
  if (header->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) {
    if (!c) return FFX_API_RETURN_ERROR_PARAMETER;
    auto d = reinterpret_cast<ffxQueryGetProviderVersion*>(header);
    d->versionId = kProvider;
    d->versionName = kName;
    return FFX_API_RETURN_OK;
  }
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE || header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2) {
    if (!memory) return FFX_API_RETURN_ERROR_PARAMETER;
    if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE && (!c || c->swapchainContext)) return FFX_API_RETURN_ERROR_PARAMETER;
    if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2) {
      auto d = reinterpret_cast<ffxQueryDescFrameGenerationGetGPUMemoryUsageV2*>(header);
      if (!d->device || !d->maxRenderSize.width || !d->maxRenderSize.height || !d->displaySize.width || !d->displaySize.height ||
          d->maxRenderSize.width > d->displaySize.width || d->maxRenderSize.height > d->displaySize.height) return FFX_API_RETURN_ERROR_PARAMETER;
    }
    // The reference MetalFX provider explicitly reports zero/unreported usage,
    // not an estimate of private MetalFX allocations.
    return FFX_API_RETURN_OK;
  }
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2) {
    if (!memory) return FFX_API_RETURN_ERROR_PARAMETER;
    auto d = reinterpret_cast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2*>(header);
    return sdkResult(ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12V2(d->device, &d->displaySize, static_cast<FfxApiSurfaceFormat>(d->backBufferFormat), d->backBufferCount, &d->uiResourceSize, static_cast<FfxApiSurfaceFormat>(d->uiResourceFormat), d->flags, memory));
  }
  if (!c || !c->swapchainContext) return FFX_API_RETURN_ERROR_PARAMETER;
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12)
    return memory ? sdkResult(ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12(c->proxy.ptr(), memory)) : FFX_API_RETURN_ERROR_PARAMETER;
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12) {
    auto d = reinterpret_cast<ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12*>(header);
    if (!d->pOutCommandList) return FFX_API_RETURN_ERROR_PARAMETER;
    return sdkResult(ffxGetFrameinterpolationCommandlistDX12(c->proxy.ptr(), *d->pOutCommandList));
  }
  if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12) {
    auto d = reinterpret_cast<ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12*>(header);
    if (!d->pOutTexture) return FFX_API_RETURN_ERROR_PARAMETER;
    *d->pOutTexture = ffxGetFrameinterpolationTextureDX12(c->proxy.ptr());
    return FFX_API_RETURN_OK;
  }
  return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}
