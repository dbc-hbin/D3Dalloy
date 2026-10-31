// SPDX-License-Identifier: LGPL-2.1-or-later
// AMD RR frontend packets -> owned D3D12 Ray Regeneration recording -> native MetalFX provider.
#include "bridge.h"
#include "../fsr/include/denoisers/include/ffx_denoiser.h"
#include "../d3d12/d3d12_ray_regeneration_interfaces.hpp"
#include "../winemetal/winemetal_metal4_rr.h"
#include "com/com_pointer.hpp"
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>

using dxmt::Com;
namespace {
constexpr uint32_t kOk = FFX_API_RETURN_OK, kRuntime = FFX_API_RETURN_ERROR_RUNTIME_ERROR,
    kNoProvider = FFX_API_RETURN_NO_PROVIDER, kMemory = FFX_API_RETURN_ERROR_MEMORY,
    kParameter = FFX_API_RETURN_ERROR_PARAMETER;
static_assert(RR_BRIDGE_RESOURCE_COUNT == MTL_RAY_REGENERATION_RESOURCE_COUNT);
static_assert(sizeof(rr_bridge_settings) == sizeof(MTL_RAY_REGENERATION_SETTINGS));
struct State {
  std::mutex mutex;
  rr_bridge_create_packet create{};
  WMTRRSettings settings = WMTRRDefaultSettings();
  Com<IUnknown> identity;
  Com<IMTLD3D12RayRegenerationState> backend;
};
std::mutex g_mutex;
std::map<uint64_t, std::shared_ptr<State>> g_contexts;
uint64_t g_next = 1;
std::shared_ptr<State> lookup(uint64_t id) {
  std::lock_guard lock(g_mutex);
  auto it = g_contexts.find(id);
  return it == g_contexts.end() ? nullptr : it->second;
}
uint32_t backendResult(HRESULT hr) {
  if (SUCCEEDED(hr)) return kOk;
  if (hr == E_OUTOFMEMORY) return kMemory;
  if (hr == E_INVALIDARG || hr == E_POINTER) return kParameter;
  if (hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == DXGI_ERROR_UNSUPPORTED) return kNoProvider;
  return kRuntime;
}
bool validSettings(const WMTRRSettings &s) {
  return std::isfinite(s.normal_strength) && s.normal_strength >= 0 && s.normal_strength <= 1 &&
      std::isfinite(s.stability_bias) && s.stability_bias >= 0 && s.stability_bias <= 1 &&
      std::isfinite(s.max_radiance) && s.max_radiance >= 0 && s.max_radiance <= 65504 &&
      std::isfinite(s.radiance_clip_std_k) && s.radiance_clip_std_k >= 0 &&
      std::isfinite(s.gaussian_relaxation) && s.gaussian_relaxation >= 0 && s.gaussian_relaxation <= 1 &&
      std::isfinite(s.disocclusion_threshold) && s.disocclusion_threshold >= 0 &&
      std::isfinite(s.debug_depth_min) && std::isfinite(s.debug_depth_max) &&
      s.debug_depth_min >= 0 && s.debug_depth_max > s.debug_depth_min &&
      std::isfinite(s.history_rejection_strength) && s.history_rejection_strength >= 0 &&
      s.history_rejection_strength <= 1;
}
float *setting(WMTRRSettings &values, uint64_t key) {
  switch (key) {
  case 1: return &values.normal_strength;
  case 2: return &values.stability_bias;
  case 3: return &values.max_radiance;
  case 4: return &values.radiance_clip_std_k;
  case 5: return &values.gaussian_relaxation;
  case 6: return &values.disocclusion_threshold;
  default: return nullptr;
  }
}
// FFX states are not D3D12 bits; the frontend already rejected unknown bits and write/read mixtures.
D3D12_RESOURCE_STATES resourceState(uint32_t state) {
  uint32_t bits = 0;
  if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) bits |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (state & FFX_API_RESOURCE_STATE_COMPUTE_READ) bits |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (state & FFX_API_RESOURCE_STATE_PIXEL_READ) bits |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if (state & FFX_API_RESOURCE_STATE_COPY_SRC) bits |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (state & FFX_API_RESOURCE_STATE_COPY_DEST) bits |= D3D12_RESOURCE_STATE_COPY_DEST;
  if (state & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) bits |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) bits |= D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (state & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT) bits |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
  return static_cast<D3D12_RESOURCE_STATES>(bits); // COMMON and PRESENT both map to zero.
}
uint32_t create(rr_bridge_create_packet &packet) {
  if (packet.header.size != sizeof(packet) || !packet.device) return kParameter;
  auto device = reinterpret_cast<IUnknown *>(packet.device);
  Com<IMTLD3D12RayRegenerationDevice> extension;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&extension))) || !extension) return kNoProvider;
  auto state = std::make_shared<State>();
  // Device proxies can have a different IUnknown from the lists and resources of the implementation.
  if (FAILED(extension->QueryInterface(IID_PPV_ARGS(&state->identity)))) return kParameter;
  MTL_RAY_REGENERATION_CREATE_DESC desc{sizeof(desc), packet.version, packet.width, packet.height,
      packet.signals, packet.checkerboard, packet.flags};
  if (auto hr = extension->CreateRayRegenerationState(&desc, &state->backend); FAILED(hr) || !state->backend)
    return backendResult(FAILED(hr) ? hr : E_FAIL);
  state->create = packet;
  std::lock_guard lock(g_mutex);
  packet.header.context = g_next++;
  g_contexts.emplace(packet.header.context, std::move(state));
  return kOk;
}
uint32_t destroy(rr_packet_header &packet) {
  std::lock_guard lock(g_mutex);
  // Recorded commands retain the native feature until GPU retirement.
  return g_contexts.erase(packet.context) ? kOk : kParameter;
}
uint32_t configure(rr_bridge_configure_packet &packet) {
  if (packet.header.size != sizeof(packet)) return kParameter;
  if (packet.key == 7) return kNoProvider; // AMD debug views are unavailable.
  auto state = lookup(packet.header.context);
  if (!state) return kParameter;
  std::lock_guard lock(state->mutex);
  auto updated = state->settings;
  if (packet.key == 8) {
    if (state->create.version != 0x00400000) return kParameter;
    updated.history_rejection_strength = packet.legacy_values[0];
    updated.normal_strength = packet.legacy_values[1];
    updated.stability_bias = packet.legacy_values[2];
    updated.max_radiance = packet.legacy_values[3];
    updated.radiance_clip_std_k = packet.legacy_values[4];
    updated.gaussian_relaxation = packet.legacy_values[5];
  } else if (auto field = setting(updated, packet.key)) *field = packet.value[0];
  else return kParameter;
  if (!validSettings(updated)) return kParameter;
  state->settings = updated;
  return kOk;
}
uint32_t query(rr_bridge_query_packet &packet) {
  if (packet.header.size != sizeof(packet)) return kParameter;
  // Context-free expected memory would require modeling opaque MetalFX allocations.
  auto state = lookup(packet.header.context);
  if (!state) return packet.header.context ? kParameter : kNoProvider;
  if (!packet.key) {
    UINT64 bytes = 0;
    if (auto hr = state->backend->GetMemoryUsage(&bytes); FAILED(hr)) return backendResult(hr);
    packet.total_bytes = bytes; packet.aliasable_bytes = 0;
    return kOk;
  }
  if (packet.key == 7) return kNoProvider;
  auto defaults = WMTRRDefaultSettings();
  auto field = setting(defaults, packet.key);
  if (!field) return kParameter;
  packet.value[0] = *field;
  return kOk;
}
bool sameIdentity(ID3D12DeviceChild *child, const Com<IUnknown> &identity) {
  Com<ID3D12Device> device;
  Com<IUnknown> unknown;
  return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&device))) && device &&
      SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&unknown))) && unknown.ptr() == identity.ptr();
}
uint32_t dispatch(rr_bridge_dispatch_packet &packet) {
  if (packet.header.size != sizeof(packet) || !packet.command_list) return kParameter;
  auto state = lookup(packet.header.context);
  if (!state) return kParameter;
  std::lock_guard lock(state->mutex);
  auto list = reinterpret_cast<ID3D12GraphicsCommandList *>(packet.command_list);
  if ((list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
      !sameIdentity(list, state->identity)) return kParameter;
  Com<IMTLD3D12RayRegenerationCommandList> extension;
  if (FAILED(list->QueryInterface(IID_PPV_ARGS(&extension))) || !extension) return kNoProvider;
  MTL_RAY_REGENERATION_FRAME_DESC desc{};
  desc.StructSize = sizeof(desc);
  for (size_t i = 0; i < RR_BRIDGE_RESOURCE_COUNT; ++i) {
    if (!packet.resources[i].object) continue;
    auto resource = reinterpret_cast<ID3D12Resource *>(packet.resources[i].object);
    if (!sameIdentity(resource, state->identity)) return kParameter;
    desc.Resources[i] = resource;
    desc.States[i] = resourceState(packet.resources[i].state);
  }
  std::memcpy(&desc.Settings, &state->settings, sizeof(desc.Settings));
  std::memcpy(desc.MotionScale, packet.motion_scale, sizeof(desc.MotionScale));
  std::memcpy(desc.Jitter, packet.jitter, sizeof(desc.Jitter));
  std::memcpy(desc.CameraDelta, packet.camera_delta, sizeof(desc.CameraDelta));
  std::memcpy(desc.View, packet.view, sizeof(desc.View));
  std::memcpy(desc.Projection, packet.projection, sizeof(desc.Projection));
  desc.DepthMin = packet.depth_min; desc.DepthMax = packet.depth_max;
  desc.Width = packet.width; desc.Height = packet.height;
  desc.FrameIndex = packet.frame_index; desc.Flags = packet.flags;
  std::memcpy(desc.CheckerboardOrigin, packet.checkerboard_origin, sizeof(desc.CheckerboardOrigin));
  std::memcpy(desc.LightDirection, packet.light_direction, sizeof(desc.LightDirection));
  std::memcpy(desc.LightEmission, packet.light_emission, sizeof(desc.LightEmission));
  desc.LightAngularRadius = packet.light_angular_radius;
  desc.DebugWidth = packet.debug_width; desc.DebugHeight = packet.debug_height;
  desc.DebugMode = packet.debug_mode; desc.DebugViewport = packet.debug_viewport;
  desc.LegacyMode = packet.legacy_mode;
  return backendResult(extension->DenoiseRayRegeneration(state->backend.ptr(), &desc));
}
} // namespace

extern "C" uint32_t rr_backend_call(void *arguments) {
  if (!arguments) return kParameter;
  auto &header = *static_cast<rr_packet_header *>(arguments);
  if (header.size < sizeof(header)) return kParameter;
  try {
    switch (header.operation) {
    case RR_BRIDGE_CREATE: return create(*static_cast<rr_bridge_create_packet *>(arguments));
    case RR_BRIDGE_DESTROY: return header.size == sizeof(header) ? destroy(header) : kParameter;
    case RR_BRIDGE_CONFIGURE: return configure(*static_cast<rr_bridge_configure_packet *>(arguments));
    case RR_BRIDGE_QUERY: return query(*static_cast<rr_bridge_query_packet *>(arguments));
    case RR_BRIDGE_DISPATCH: return dispatch(*static_cast<rr_bridge_dispatch_packet *>(arguments));
    default: return kParameter;
    }
  } catch (const std::bad_alloc &) { return kMemory; }
}
