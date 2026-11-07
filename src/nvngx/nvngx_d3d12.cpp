#include "nvngx_d3d12.hpp"

#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "nvngx_parameter.hpp"
#include "../d3d12/d3d12_interfaces.hpp"
#include "../d3d12/d3d12_frame_generation_interfaces.hpp"
#include "../d3d12/owned_trace.hpp"
#include "dxgi_interfaces.h"
#include "Metal4FX.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace d3dalloy {

extern "C" {
unsigned long long owned_ngxd3d12_msvc_parameter_size();
unsigned long long owned_ngxd3d12_msvc_parameter_align();
void *owned_ngxd3d12_msvc_parameter_construct(void *memory, void *storage);
void owned_ngxd3d12_msvc_parameter_destruct(void *object);
}

namespace {

__attribute__((used)) static const char kOwnedNgxD3D12Marker[] = "own-ngxd3d12";

constexpr uint32_t kFeatureSuperSampling = 1;
constexpr uint32_t kFeatureSpatial = 9;
constexpr uint32_t kFeatureFrameGeneration = 11;
constexpr uint32_t kFeatureRayReconstruction = 13;
constexpr uint32_t kRayReconstructionFlag = 256; // Owned transport selector, not a public DLSS flag.
bool supportedFeature(unsigned int feature) {
  return feature == kFeatureSuperSampling || feature == kFeatureSpatial || feature == kFeatureFrameGeneration ||
      feature == kFeatureRayReconstruction;
}
constexpr auto kSupportAdapterUnsupported = static_cast<NVNGX_FEATURE_SUPPORT_RESULT>(4);
constexpr uint32_t kSupportedCreateFlags =
    NVNGX_DLSS_FLAG_HDR | NVNGX_DLSS_FLAG_MV_LOWRES | NVNGX_DLSS_FLAG_DEPTH_INVERTED |
    NVNGX_DLSS_FLAG_MV_JITTERED | NVNGX_DLSS_FLAG_DO_SHARPENING | NVNGX_DLSS_FLAG_AUTO_EXPOSURE | 128;

// Keep the legacy D3D11 shim's historical 0x0bad000x values untouched, but
// return NVIDIA's actual public ABI values from every new D3D12 entry point.
constexpr NVNGX_RESULT kResultSuccess = static_cast<NVNGX_RESULT>(0x00000001u);
constexpr NVNGX_RESULT kResultFail = static_cast<NVNGX_RESULT>(0xBAD00000u);
constexpr NVNGX_RESULT kResultFeatureNotSupported = static_cast<NVNGX_RESULT>(0xBAD00001u);
constexpr NVNGX_RESULT kResultPlatformError = static_cast<NVNGX_RESULT>(0xBAD00002u);
constexpr NVNGX_RESULT kResultFeatureNotFound = static_cast<NVNGX_RESULT>(0xBAD00004u);
constexpr NVNGX_RESULT kResultInvalidParameter = static_cast<NVNGX_RESULT>(0xBAD00005u);
constexpr NVNGX_RESULT kResultNotInitialized = static_cast<NVNGX_RESULT>(0xBAD00007u);
constexpr NVNGX_RESULT kResultMissingInput = static_cast<NVNGX_RESULT>(0xBAD0000Au);
constexpr NVNGX_RESULT kResultUnableToInitializeFeature = static_cast<NVNGX_RESULT>(0xBAD0000Bu);
constexpr NVNGX_RESULT kResultUnsupportedParameter = static_cast<NVNGX_RESULT>(0xBAD00010u);

// Bounded identity for one Init registration. A device COM identity can be
// re-initialized after Shutdown1, so CreateFeature must prove that the exact
// registration it validated is still the one published in the registry.
struct DeviceRegistration {};

struct DeviceEntry {
  Com<IUnknown> identity;
  Com<ID3D12Device> device;
  Com<IMTLD3D12DeviceExt> extension;
  std::shared_ptr<DeviceRegistration> registration;
};

struct FeatureState {
  uint32_t kind = kFeatureSuperSampling;
  Com<IUnknown> device_identity;
  Com<ID3D12Device> device;
  Com<IMTLD3D12TemporalUpscaleState> backend;
  MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC create = {};
  bool sharpening = false;
  Com<IMTLD3D12FrameGenerationState> frame_generation;
  MTL_FRAME_GENERATION_CREATE_DESC fg_create = {};
  std::mutex fg_mutex;
  uint64_t next_frame_id = 0;
  LARGE_INTEGER fg_last_counter = {}, fg_counter_frequency = {};
  UINT roughness_packed = 0, depth_linear = 0;
};

struct ParameterState {
  std::unique_ptr<ParametersImpl> storage;
  void *memory = nullptr;
  NVNGXParameter *facade = nullptr;
  bool managed = false;

  ~ParameterState() {
    if (facade)
      owned_ngxd3d12_msvc_parameter_destruct(facade);
    ::operator delete(memory);
  }
};

std::mutex g_mutex;
std::unordered_map<IUnknown *, DeviceEntry> g_devices;
std::unordered_map<const NVNGXHandle *, std::shared_ptr<FeatureState>> g_features;
std::unordered_map<const NVNGXParameter *, std::shared_ptr<ParameterState>> g_parameters;
// Monotonic opaque token space. The public NVSDK_NGX_Handle payload is never
// dereferenced, so an incrementing integer value is a complete handle: released
// values are never reused and live storage stays bounded. Exhaustion fails the
// feature rather than aliasing an old handle.
std::atomic<uintptr_t> g_next_handle{1};

NVNGX_RESULT mapBackendResult(HRESULT hr, bool creating);

std::shared_ptr<ParameterState>
lookupParameter(const NVNGXParameter *parameter) {
  if (!parameter)
    return {};
  std::lock_guard lock(g_mutex);
  auto it = g_parameters.find(parameter);
  return it == g_parameters.end() ? std::shared_ptr<ParameterState>{} : it->second;
}

NVNGX_RESULT
registerParameter(std::unique_ptr<ParametersImpl> storage, bool managed, NVNGXParameter **out_parameter) {
  if (!out_parameter)
    return kResultInvalidParameter;
  *out_parameter = nullptr;
  if (!storage)
    return kResultInvalidParameter;
  const auto size = owned_ngxd3d12_msvc_parameter_size();
  const auto alignment = owned_ngxd3d12_msvc_parameter_align();
  if (!size || !alignment || alignment > alignof(std::max_align_t))
    return kResultFail;
  void *memory = ::operator new(static_cast<size_t>(size), std::nothrow);
  if (!memory)
    return kResultFail;

  auto state = std::make_shared<ParameterState>();
  state->storage = std::move(storage);
  state->memory = memory;
  state->managed = managed;
  state->facade = reinterpret_cast<NVNGXParameter *>(
      owned_ngxd3d12_msvc_parameter_construct(memory, state->storage.get())
  );
  if (!state->facade)
    return kResultFail;
  {
    // The Init gate and the publication are one critical section, so a
    // concurrent Shutdown1 cannot slip between an external check and this
    // insertion and publish a parameter map for a closed SDK.
    std::lock_guard lock(g_mutex);
    if (g_devices.empty())
      return kResultNotInitialized;
    g_parameters[state->facade] = state;
  }
  *out_parameter = state->facade;
  return kResultSuccess;
}

Com<IUnknown>
identityOf(IUnknown *object) {
  return Com<IUnknown>::queryFrom(object);
}

NVNGXHandle *
allocateFeatureHandle() {
  // Do not recycle: a released handle value must keep resolving to
  // FAIL_FeatureNotFound instead of aliasing a later feature.
  uintptr_t current = g_next_handle.load(std::memory_order_relaxed);
  while (current != UINTPTR_MAX) {
    if (g_next_handle.compare_exchange_weak(
            current, current + 1, std::memory_order_relaxed, std::memory_order_relaxed
        ))
      return reinterpret_cast<NVNGXHandle *>(current);
  }
  return nullptr;
}

bool
deviceMaskValid(ID3D12Device *device, uint32_t mask) {
  if (!device || !mask)
    return false;
  const UINT count = device->GetNodeCount();
  if (!count)
    return false;
  const uint32_t valid = count >= 32 ? ~0u : ((1u << count) - 1u);
  return (mask & ~valid) == 0;
}

bool
findInitializedDevice(ID3D12Device *device, DeviceEntry *entry = nullptr) {
  if (!device)
    return false;
  auto identity = identityOf(device);
  if (!identity)
    return false;
  std::lock_guard lock(g_mutex);
  auto it = g_devices.find(identity.ptr());
  if (it == g_devices.end())
    return false;
  if (entry)
    *entry = it->second;
  return true;
}

NVNGX_RESULT
validateCommandList(
    ID3D12GraphicsCommandList *list, const Com<IUnknown> &expected_device,
    Com<IMTLD3D12GraphicsCommandListExt> &extension, Com<ID3D12Device> &device_out
) {
  if (!list)
    return kResultInvalidParameter;
  const auto type = list->GetType();
  if (type == D3D12_COMMAND_LIST_TYPE_COPY || type == D3D12_COMMAND_LIST_TYPE_BUNDLE)
    return kResultInvalidParameter;

  Com<ID3D12Device> device = nullptr;
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device)
    return kResultInvalidParameter;
  auto identity = identityOf(device.ptr());
  if (!identity)
    return kResultInvalidParameter;
  if (expected_device && identity.ptr() != expected_device.ptr())
    return kResultInvalidParameter;
  if (!findInitializedDevice(device.ptr()))
    return kResultNotInitialized;

  Com<IMTLD3D12GraphicsCommandListExt> ext = nullptr;
  if (FAILED(list->QueryInterface(IID_PPV_ARGS(&ext))) || !ext)
    return kResultFeatureNotSupported;
  const HRESULT recording = ext->ValidateRecording();
  if (FAILED(recording))
    return kResultInvalidParameter;
  extension = std::move(ext);
  device_out = std::move(device);
  return kResultSuccess;
}

NVNGX_RESULT
getRequiredUInt(const ParametersImpl *params, const char *name, uint32_t *value) {
  return params && value && !NVNGX_FAILED(params->Get(name, value)) ? kResultSuccess : kResultMissingInput;
}

NVNGX_RESULT
getRequiredResource(const ParametersImpl *params, const char *name, ID3D12Resource **value) {
  if (!params || !value || NVNGX_FAILED(params->Get(name, value)))
    return kResultMissingInput;
  return *value ? kResultSuccess : kResultInvalidParameter;
}

uint32_t
getOptionalUInt(const ParametersImpl *params, const char *name, uint32_t fallback, bool *present = nullptr) {
  uint32_t value = 0;
  const bool found = params && !NVNGX_FAILED(params->Get(name, &value));
  if (present)
    *present = found;
  return found ? value : fallback;
}

int
getOptionalInt(const ParametersImpl *params, const char *name, int fallback, bool *present = nullptr) {
  int value = 0;
  const bool found = params && !NVNGX_FAILED(params->Get(name, &value));
  if (present)
    *present = found;
  return found ? value : fallback;
}

float
getOptionalFloat(const ParametersImpl *params, const char *name, float fallback, bool *present = nullptr) {
  float value = 0.0f;
  const bool found = params && !NVNGX_FAILED(params->Get(name, &value));
  if (present)
    *present = found;
  return found ? value : fallback;
}

bool invertMatrix(const float input[16], float output[16]) {
  double augmented[4][8]{};
  for (unsigned row = 0; row != 4; ++row) {
    for (unsigned column = 0; column != 4; ++column) {
      if (!std::isfinite(input[row * 4 + column])) return false;
      augmented[row][column] = input[row * 4 + column];
    }
    augmented[row][row + 4] = 1.0;
  }
  for (unsigned column = 0; column != 4; ++column) {
    unsigned pivot = column;
    for (unsigned row = column + 1; row != 4; ++row)
      if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
    if (augmented[pivot][column] == 0) return false;
    if (pivot != column)
      for (unsigned value = 0; value != 8; ++value) std::swap(augmented[pivot][value], augmented[column][value]);
    const double scale = augmented[column][column];
    for (unsigned value = 0; value != 8; ++value) augmented[column][value] /= scale;
    for (unsigned row = 0; row != 4; ++row) {
      if (row == column) continue;
      const double factor = augmented[row][column];
      for (unsigned value = 0; value != 8; ++value) augmented[row][value] -= factor * augmented[column][value];
    }
  }
  for (unsigned row = 0; row != 4; ++row)
    for (unsigned column = 0; column != 4; ++column) {
      output[row * 4 + column] = static_cast<float>(augmented[row][column + 4]);
      if (!std::isfinite(output[row * 4 + column])) return false;
    }
  return true;
}

ID3D12Resource *
getOptionalResource(const ParametersImpl *params, const char *name, bool *present = nullptr) {
  ID3D12Resource *value = nullptr;
  const bool found = params && !NVNGX_FAILED(params->Get(name, &value));
  if (present)
    *present = found;
  return found ? value : nullptr;
}

NVNGX_RESULT
mapBackendResult(HRESULT hr, bool creating) {
  if (SUCCEEDED(hr))
    return kResultSuccess;
  if (hr == E_INVALIDARG || hr == E_POINTER)
    return kResultInvalidParameter;
  if (hr == E_NOINTERFACE || hr == E_NOTIMPL)
    return kResultFeatureNotSupported;
  if (hr == DXGI_ERROR_UNSUPPORTED)
    return kResultFeatureNotSupported;
  return creating ? kResultUnableToInitializeFeature : kResultPlatformError;
}

NVNGX_RESULT
optimalSettingsCallback(NVNGXParameter *params) {
  auto parameter = lookupParameter(params);
  if (!parameter)
    return kResultInvalidParameter;
  auto *storage = parameter->storage.get();
  uint32_t width = 0, height = 0;
  int quality = NVNGX_PERFQUALITY_BALANCED;
  if (NVNGX_FAILED(storage->Get(NVNGX_Parameter_Width, &width)) ||
      NVNGX_FAILED(storage->Get(NVNGX_Parameter_Height, &height)))
    return kResultInvalidParameter;
  storage->Get(NVNGX_Parameter_PerfQualityValue, &quality);

  float scale = 0.58f;
  switch (quality) {
  case NVNGX_PERFQUALITY_ULTRA_PERFORMANCE: scale = 1.0f / 3.0f; break;
  case NVNGX_PERFQUALITY_MAXPERF: scale = 0.5f; break;
  case NVNGX_PERFQUALITY_MAXQUALITY: scale = 1.0f / 1.5f; break;
  case NVNGX_PERFQUALITY_ULTRA_QUALITY: scale = 1.0f / 1.3f; break;
  case NVNGX_PERFQUALITY_DLAA: scale = 1.0f; break;
  default: break;
  }
  const uint32_t out_width = static_cast<uint32_t>(std::ceil(width * scale));
  const uint32_t out_height = static_cast<uint32_t>(std::ceil(height * scale));
  storage->Set(NVNGX_Parameter_Scale, scale);
  storage->Set(NVNGX_Parameter_SuperSampling_ScaleFactor, scale);
  storage->Set(NVNGX_Parameter_OutWidth, out_width);
  storage->Set(NVNGX_Parameter_OutHeight, out_height);
  storage->Set(NVNGX_Parameter_Sharpness, 0.0f);
  storage->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, static_cast<uint32_t>(std::ceil(width / 3.0f)));
  storage->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, static_cast<uint32_t>(std::ceil(height / 3.0f)));
  storage->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, width);
  storage->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, height);
  storage->Set("DLSSMode", 1);
  storage->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_DLAA, 0u);
  storage->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, 0u);
  storage->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Quality, 0u);
  storage->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Balanced, 0u);
  storage->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Performance, 0u);
  storage->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, 0u);
  return kResultSuccess;
}

NVNGX_RESULT
statsCallback(NVNGXParameter *params) {
  auto parameter = lookupParameter(params);
  if (!parameter)
    return kResultInvalidParameter;
  parameter->storage->Set("SizeInBytes", 0ull);
  return kResultSuccess;
}

std::unique_ptr<ParametersImpl>
makeCapabilityParameters() {
  auto out = std::make_unique<ParametersImpl>();
  out->Set("SuperSampling.Available", 1u);
  out->Set(NVSDK_NGX_EParameter_SuperSampling_Available, 1u);
  out->Set("SuperSampling.MinDriverVersionMajor", 0);
  out->Set("SuperSampling.MinDriverVersionMinor", 0);
  out->Set("SuperSampling.NeedsUpdatedDriver", 0);
  out->Set("SuperSampling.FeatureInitResult", 1u);
  out->Set("Snippet.OptLevel", 0);
  out->Set("Snippet.IsDevBranch", 0);
  out->Set(NVNGX_Parameter_DLSSOptimalSettingsCallback, reinterpret_cast<void *>(&optimalSettingsCallback));
  out->Set("DLSSGetStatsCallback", reinterpret_cast<void *>(&statsCallback));
  out->Set(NVNGX_Parameter_Sharpness, 0.0f);
  out->Set(NVNGX_Parameter_MV_Scale_X, 1.0f);
  out->Set(NVNGX_Parameter_MV_Scale_Y, 1.0f);
  out->Set("MV.Offset.X", 0.0f);
  out->Set("MV.Offset.Y", 0.0f);
  out->Set(NVNGX_Parameter_DLSS_Exposure_Scale, 1.0f);
  out->Set(NVNGX_Parameter_PerfQualityValue, static_cast<int>(NVNGX_PERFQUALITY_MAXQUALITY));
  out->Set("CreationNodeMask", 1u);
  out->Set("VisibilityNodeMask", 1u);
  // DLSS.Enable.Output.Subrects is a create-time opt-in that NVIDIA's D3D12
  // helper writes from InEnableOutputSubrects. It is not a capability grant, so
  // the capability map must not pre-enable it for callers that never asked.
  out->Set("DLSS.Enable.Output.Subrects", 0);
  out->Set("RTXValue", 0);
  bool fg = false, rr = false, spatial = false;
  {
    std::lock_guard lock(g_mutex);
    for (const auto &[identity, entry] : g_devices) {
      BOOL supported = FALSE;
      if (SUCCEEDED(entry.extension->SupportsTemporalFeature(512, &supported))) fg |= supported != FALSE;
      supported = FALSE;
      if (SUCCEEDED(entry.extension->SupportsTemporalFeature(kRayReconstructionFlag, &supported))) rr |= supported != FALSE;
      supported = FALSE;
      if (SUCCEEDED(entry.extension->SupportsTemporalFeature(WMTMetal4FXSpatialFlag, &supported))) spatial |= supported != FALSE;
    }
  }
  for (const auto &[name, available] : {std::pair{"FrameGeneration", fg}, std::pair{"SuperSamplingDenoising", rr},
       std::pair{"InPainting", false}, std::pair{"ImageSuperResolution", false},
       std::pair{"SlowMotion", false}, std::pair{"VideoSuperResolution", false},
       std::pair{"ImageSignalProcessing", spatial}, std::pair{"DeepResolve", false}, std::pair{"DeepDVC", false}}) {
    std::string prefix(name);
    out->Set((prefix + ".Available").c_str(), available ? 1u : 0u);
    out->Set((prefix + ".FeatureInitResult").c_str(), static_cast<unsigned int>(available ? kResultSuccess : kResultFeatureNotSupported));
    out->Set((prefix + ".NeedsUpdatedDriver").c_str(), 0u);
    out->Set((prefix + ".MinDriverVersionMajor").c_str(), 0u);
    out->Set((prefix + ".MinDriverVersionMinor").c_str(), 0u);
  }
  out->Set("#\x09", spatial ? 1u : 0u);
  out->Set("DLSSG.MultiFrameCountMax", 1u);
  out->Set("DLSSDOptimalSettingsCallback", reinterpret_cast<void *>(&optimalSettingsCallback));
  out->Set("DLSSDGetStatsCallback", reinterpret_cast<void *>(&statsCallback));
  return out;
}

NVNGX_RESULT
parseCreate(const ParametersImpl *params, ID3D12Device *device, MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc, bool spatial) {
  if (!params || !device || !desc)
    return kResultInvalidParameter;
  *desc = {};
  desc->StructSize = sizeof(*desc);
  if (auto result = getRequiredUInt(params, NVNGX_Parameter_Width, &desc->InputWidth); result != kResultSuccess)
    return result;
  if (auto result = getRequiredUInt(params, NVNGX_Parameter_Height, &desc->InputHeight); result != kResultSuccess)
    return result;
  if (auto result = getRequiredUInt(params, NVNGX_Parameter_OutWidth, &desc->OutputWidth); result != kResultSuccess)
    return result;
  if (auto result = getRequiredUInt(params, NVNGX_Parameter_OutHeight, &desc->OutputHeight); result != kResultSuccess)
    return result;
  if (!desc->InputWidth || !desc->InputHeight || !desc->OutputWidth || !desc->OutputHeight)
    return kResultInvalidParameter;
  desc->CreationNodeMask = getOptionalUInt(params, "CreationNodeMask", 1u);
  desc->VisibilityNodeMask = getOptionalUInt(params, "VisibilityNodeMask", 1u);
  if (!deviceMaskValid(device, desc->CreationNodeMask) || !deviceMaskValid(device, desc->VisibilityNodeMask))
    return kResultInvalidParameter;
  if (spatial) {
    desc->FeatureFlags = WMTMetal4FXSpatialFlag;
    return kResultSuccess;
  }

  const int flags = getOptionalInt(params, NVNGX_Parameter_DLSS_Feature_Create_Flags, 0);
  if (flags < 0)
    return kResultUnsupportedParameter;
  desc->FeatureFlags = static_cast<uint32_t>(flags);
  if ((desc->FeatureFlags & ~kSupportedCreateFlags) != 0)
    return kResultUnsupportedParameter;
  // Sharpening is a ported post-operation, not a MetalFX descriptor feature.
  desc->FeatureFlags &= ~NVNGX_DLSS_FLAG_DO_SHARPENING;

  const int subrects = getOptionalInt(params, "DLSS.Enable.Output.Subrects", 0);
  if (subrects != 0 && subrects != 1)
    return kResultInvalidParameter;
  desc->OutputSubrectsEnabled = subrects ? TRUE : FALSE;
  return kResultSuccess;
}

NVNGX_RESULT
parseFrame(
    const ParametersImpl *params, const FeatureState &feature, MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc
) {
  if (!params || !desc)
    return kResultInvalidParameter;
  *desc = {};
  desc->StructSize = sizeof(*desc);
  if (auto result = getRequiredResource(params, NVNGX_Parameter_Color, &desc->Color); result != kResultSuccess)
    return result;
  if (auto result = getRequiredResource(params, NVNGX_Parameter_Output, &desc->Output); result != kResultSuccess)
    return result;
  if (feature.kind == kFeatureSpatial) {
    desc->InputContentWidth = getOptionalUInt(params, "Rect.W", feature.create.InputWidth);
    desc->InputContentHeight = getOptionalUInt(params, "Rect.H", feature.create.InputHeight);
    desc->ColorBaseX = getOptionalUInt(params, "Rect.X", 0);
    desc->ColorBaseY = getOptionalUInt(params, "Rect.Y", 0);
    return kResultSuccess;
  }
  if (auto result = getRequiredResource(params, NVNGX_Parameter_Depth, &desc->Depth); result != kResultSuccess)
    return result;
  if (auto result = getRequiredResource(params, NVNGX_Parameter_MotionVectors, &desc->MotionVector); result != kResultSuccess)
    return result;

  desc->ExposureTexture = getOptionalResource(params, NVNGX_Parameter_ExposureTexture);
  desc->BiasCurrentColorMask = getOptionalResource(params, NVNGX_Parameter_BiasCurrentColorMask);
  desc->TransparencyMask = getOptionalResource(params, NVNGX_Parameter_TransparencyMask);
  desc->CombineCompositionMask = desc->TransparencyMask != nullptr;

  bool has_width = false, has_height = false;
  desc->InputContentWidth = getOptionalUInt(
      params, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, feature.create.InputWidth, &has_width
  );
  desc->InputContentHeight = getOptionalUInt(
      params, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, feature.create.InputHeight, &has_height
  );
  if (has_width != has_height)
    return kResultMissingInput;
  if (!desc->InputContentWidth || !desc->InputContentHeight)
    return kResultInvalidParameter;

  struct Offset {
    const char *name;
    UINT *value;
  };
  Offset offsets[] = {
      {NVNGX_Parameter_DLSS_Color_Base_X, &desc->ColorBaseX},
      {NVNGX_Parameter_DLSS_Color_Base_Y, &desc->ColorBaseY},
      {NVNGX_Parameter_DLSS_Depth_Base_X, &desc->DepthBaseX},
      {NVNGX_Parameter_DLSS_Depth_Base_Y, &desc->DepthBaseY},
      {NVNGX_Parameter_DLSS_Motion_Base_X, &desc->MotionBaseX},
      {NVNGX_Parameter_DLSS_Motion_Base_Y, &desc->MotionBaseY},
      {NVNGX_Parameter_DLSS_Bias_Base_X, &desc->BiasBaseX},
      {NVNGX_Parameter_DLSS_Bias_Base_Y, &desc->BiasBaseY},
      {NVNGX_Parameter_DLSS_Translucency_Base_X, &desc->CompositionBaseX},
      {NVNGX_Parameter_DLSS_Translucency_Base_Y, &desc->CompositionBaseY},
  };
  for (const auto &offset : offsets)
    *offset.value = getOptionalUInt(params, offset.name, 0);
  // Output base is only meaningful for a feature created with output subrects
  // enabled. When it is enabled the backend owns logical-output-vs-backing
  // bounds validation; the frontend must not guess or clamp the rectangle.
  desc->OutputBaseX = getOptionalUInt(params, NVNGX_Parameter_DLSS_Output_Base_X, 0);
  desc->OutputBaseY = getOptionalUInt(params, NVNGX_Parameter_DLSS_Output_Base_Y, 0);
  if ((desc->OutputBaseX || desc->OutputBaseY) && !feature.create.OutputSubrectsEnabled)
    return kResultUnsupportedParameter;

  desc->JitterOffsetX = getOptionalFloat(params, NVNGX_Parameter_Jitter_Offset_X, 0.0f);
  desc->JitterOffsetY = getOptionalFloat(params, NVNGX_Parameter_Jitter_Offset_Y, 0.0f);
  desc->MotionVectorScaleX = getOptionalFloat(params, NVNGX_Parameter_MV_Scale_X, 1.0f);
  desc->MotionVectorScaleY = getOptionalFloat(params, NVNGX_Parameter_MV_Scale_Y, 1.0f);
  const float raw_pre_exposure = getOptionalFloat(params, NVNGX_Parameter_DLSS_Pre_Exposure, 0.0f);
  const float raw_exposure_scale = getOptionalFloat(params, NVNGX_Parameter_DLSS_Exposure_Scale, 0.0f);
  const float sharpness = getOptionalFloat(params, NVNGX_Parameter_Sharpness, 0.0f);
  const float motion_offset_x = getOptionalFloat(params, "MV.Offset.X", 0.0f);
  const float motion_offset_y = getOptionalFloat(params, "MV.Offset.Y", 0.0f);
  if (!std::isfinite(desc->JitterOffsetX) || !std::isfinite(desc->JitterOffsetY) ||
      !std::isfinite(desc->MotionVectorScaleX) || !std::isfinite(desc->MotionVectorScaleY) ||
      !std::isfinite(raw_pre_exposure) || !std::isfinite(raw_exposure_scale) || !std::isfinite(sharpness) ||
      !std::isfinite(motion_offset_x) || !std::isfinite(motion_offset_y))
    return kResultInvalidParameter;
  if (desc->MotionVectorScaleX == 0.0f || desc->MotionVectorScaleY == 0.0f || raw_pre_exposure < 0.0f ||
      raw_exposure_scale < 0.0f || sharpness < 0.0f || sharpness > 1.0f)
    return kResultInvalidParameter;

  desc->PreExposure = raw_pre_exposure == 0.0f ? 1.0f : raw_pre_exposure;
  // GPTK 4.0b2 does not apply Exposure.Scale or MV.Offset to GPU inputs.
  // Accept these validated compatibility parameters without inventing effects.
  if (!feature.sharpening && sharpness != 0.0f)
    return kResultUnsupportedParameter;
  desc->EnableSharpening = feature.sharpening ? TRUE : FALSE;
  desc->Sharpness = sharpness;
  desc->ResetHistory = getOptionalInt(params, NVNGX_Parameter_Reset, 0) ? TRUE : FALSE;
  return kResultSuccess;
}

std::shared_ptr<FeatureState>
lookupFeature(const NVNGXHandle *handle) {
  if (!handle)
    return {};
  std::lock_guard lock(g_mutex);
  auto it = g_features.find(handle);
  return it == g_features.end() ? std::shared_ptr<FeatureState>{} : it->second;
}

NVNGX_RESULT
parseRayReconstructionFrame(const ParametersImpl *params, const FeatureState &feature,
    MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC &frame) {
  auto result = parseFrame(params, feature, &frame);
  if (result != kResultSuccess) return result;
  frame.RoughnessPacked = feature.roughness_packed; frame.DepthLinear = feature.depth_linear;
  frame.ExposureTexture = nullptr; frame.PreExposure = 1;
  // RR slot 6 carries a premultiplied RGBA overlay. A transparency mask or
  // independent per-channel opacity cannot use the native overlay's alpha.
  if (frame.TransparencyMask || getOptionalResource(params, "DLSS.TransparencyLayerOpacity"))
    return kResultUnsupportedParameter;
  if (frame.CompositionBaseX || frame.CompositionBaseY) return kResultInvalidParameter;
  frame.TransparencyMask = getOptionalResource(params, "DLSS.TransparencyLayer");
  frame.CompositionBaseX = getOptionalUInt(params, "DLSS.TransparencyLayer.Subrect.Base.X", 0);
  frame.CompositionBaseY = getOptionalUInt(params, "DLSS.TransparencyLayer.Subrect.Base.Y", 0);
  if (!frame.TransparencyMask && (frame.CompositionBaseX || frame.CompositionBaseY)) return kResultInvalidParameter;
  auto responsivity = getOptionalResource(params, "DLSSD.ResponsivityMask");
  const UINT responsivity_x = getOptionalUInt(params, "DLSSD.ResponsivityMask.Subrect.Base.X", 0);
  const UINT responsivity_y = getOptionalUInt(params, "DLSSD.ResponsivityMask.Subrect.Base.Y", 0);
  if (!responsivity && (responsivity_x || responsivity_y)) return kResultInvalidParameter;
  if (responsivity) {
    // Two independently supplied history masks have no defined combination.
    if (frame.BiasCurrentColorMask || frame.BiasBaseX || frame.BiasBaseY) return kResultUnsupportedParameter;
    frame.BiasCurrentColorMask = responsivity;
    frame.BiasBaseX = responsivity_x; frame.BiasBaseY = responsivity_y;
  }
  struct Guide { const char *key; ID3D12Resource **resource; const char *x, *y; UINT *base_x, *base_y; };
  Guide guides[] = {
    {"DLSS.Input.DiffuseAlbedo", &frame.DiffuseAlbedo, "DLSS.Input.DiffuseAlbedo.Subrect.Base.X", "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y", &frame.DiffuseAlbedoBaseX, &frame.DiffuseAlbedoBaseY},
    {"DLSS.Input.SpecularAlbedo", &frame.SpecularAlbedo, "DLSS.Input.SpecularAlbedo.Subrect.Base.X", "DLSS.Input.SpecularAlbedo.Subrect.Base.Y", &frame.SpecularAlbedoBaseX, &frame.SpecularAlbedoBaseY},
    {"GBuffer.Normals", &frame.Normals, "DLSS.Input.Normals.Subrect.Base.X", "DLSS.Input.Normals.Subrect.Base.Y", &frame.NormalsBaseX, &frame.NormalsBaseY},
    {"GBuffer.Roughness", &frame.Roughness, "DLSS.Input.Roughness.Subrect.Base.X", "DLSS.Input.Roughness.Subrect.Base.Y", &frame.RoughnessBaseX, &frame.RoughnessBaseY},
    {"DLSSD.SpecularHitDistance", &frame.SpecularHitDistance, "DLSSD.SpecularHitDistance.Subrect.Base.X", "DLSSD.SpecularHitDistance.Subrect.Base.Y", &frame.SpecularHitDistanceBaseX, &frame.SpecularHitDistanceBaseY},
  };
  for (size_t i = 0; i < std::size(guides); ++i) {
    const auto &guide = guides[i];
    if (i == 4 || (i == 3 && frame.RoughnessPacked)) *guide.resource = getOptionalResource(params, guide.key);
    else if (auto status = getRequiredResource(params, guide.key, guide.resource); status != kResultSuccess) return status;
    *guide.base_x = getOptionalUInt(params, guide.x, 0); *guide.base_y = getOptionalUInt(params, guide.y, 0);
  }
  for (const auto &[key, destination] : {std::pair{"WorldToViewMatrix", frame.WorldToViewMatrix},
      std::pair{"ViewToClipMatrix", frame.ViewToClipMatrix}}) {
    void *matrix = nullptr;
    if (NVNGX_FAILED(params->Get(key, &matrix))) return kResultMissingInput;
    if (!matrix) return kResultInvalidParameter;
    std::memcpy(destination, matrix, sizeof(float) * 16);
    for (size_t i = 0; i < 16; ++i) if (!std::isfinite(destination[i])) return kResultInvalidParameter;
  }
  // These optional RR2 guides need separate native denoiser inputs; never silently ignore them.
  for (const char *key : {"DLSSD.DiffuseHitDistance", "DLSSD.DiffuseRayDirection", "DLSSD.SpecularRayDirection",
       "DLSSD.DiffuseRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance",
       "DLSSD.ReflectedAlbedo", "DLSSD.Alpha", "DLSSD.OutputAlpha",
       "DLSSD.DepthOfFieldGuide", "DLSSD.ScreenSpaceRefractionGuide", "DLSSD.ScreenSpaceSubsurfaceScatteringGuide",
       "MotionVectorsReflection", "DLSSD.ColorBeforeTransparency", "DLSSD.ColorBeforeParticles", "DLSSD.ColorAfterParticles",
       "DLSSD.ColorAfterTransparency", "DLSSD.ColorBeforeFog", "DLSSD.ColorAfterFog",
       "DLSSD.ColorBeforeDepthOfField", "DLSSD.ColorAfterDepthOfField",
       "DLSSD.ColorBeforeScreenSpaceRefraction", "DLSSD.ColorAfterScreenSpaceRefraction",
       "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering", "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering"})
    if (getOptionalResource(params, key)) return kResultUnsupportedParameter;
  return kResultSuccess;
}

NVNGX_RESULT
parseFrameGenerationCreate(const ParametersImpl *params, ID3D12Device *device, MTL_FRAME_GENERATION_CREATE_DESC &desc) {
  desc = {}; desc.StructSize = sizeof(desc);
  desc.display_width = getOptionalUInt(params, "DLSSG.Width", getOptionalUInt(params, "Width", 0));
  desc.display_height = getOptionalUInt(params, "DLSSG.Height", getOptionalUInt(params, "Height", 0));
  desc.max_render_width = getOptionalUInt(params, "DLSSG.InternalWidth", desc.display_width);
  desc.max_render_height = getOptionalUInt(params, "DLSSG.InternalHeight", desc.display_height);
  desc.backbuffer_format = static_cast<DXGI_FORMAT>(getOptionalUInt(params, "DLSSG.BackbufferFormat", 0));
  if (!desc.display_width || !desc.display_height || !desc.max_render_width || !desc.max_render_height) return kResultMissingInput;
  if (!deviceMaskValid(device,getOptionalUInt(params,"CreationNodeMask",1)) ||
      !deviceMaskValid(device,getOptionalUInt(params,"VisibilityNodeMask",1))) return kResultInvalidParameter;
  desc.user_interface_recomposition = getOptionalUInt(params,"DLSSG.UserInterfaceRecompositionEnabled",0);
  desc.orthographic_projection = getOptionalUInt(params,"DLSSG.OrthoProjection",0);
  if (desc.orthographic_projection > 1) return kResultInvalidParameter;
  if (desc.user_interface_recomposition > 1) return kResultInvalidParameter;
  const UINT inverted = getOptionalUInt(params,"DLSSG.DepthInverted",0);
  const UINT jittered = getOptionalUInt(params,"DLSSG.MvecJittered",0);
  if (inverted > 1 || jittered > 1) return kResultInvalidParameter;
  desc.flags = inverted | (jittered ? 8u : 0u);
  return kResultSuccess;
}


NVNGX_RESULT
evaluateFrameGeneration(ID3D12GraphicsCommandList *list, const ParametersImpl *params, FeatureState &feature) {
  Com<IMTLD3D12FrameGenerationCommandList> extension = Com<IMTLD3D12FrameGenerationCommandList>::queryFrom(list);
  if (!extension) return kResultFeatureNotSupported;
  MTL_FRAME_GENERATION_PREPARE_DESC prepare{}; prepare.StructSize = sizeof(prepare);
  MTL_FRAME_GENERATION_GENERATE_DESC generate{}; generate.StructSize = sizeof(generate);
  ID3D12Resource *output = nullptr;
  for (const auto &[key, resource] : {std::pair{"DLSSG.Backbuffer", &generate.present_color},
       std::pair{"DLSSG.OutputInterpolated", &output}, std::pair{"DLSSG.Depth", &prepare.depth},
       std::pair{"DLSSG.MVecs", &prepare.motion_vectors}})
    if (auto status = getRequiredResource(params,key,resource); status != kResultSuccess) return status;
  generate.hudless_color = getOptionalResource(params,"DLSSG.HUDLess");
  generate.real_output = getOptionalResource(params,"DLSSG.OutputReal");
  generate.distortion = getOptionalResource(params,"DLSSG.BidirectionalDistortionField");
  generate.ui = getOptionalResource(params,"DLSSG.UI");
  generate.ui_alpha = getOptionalResource(params,"DLSSG.UIAlpha");
  if ((feature.fg_create.user_interface_recomposition || generate.ui || generate.ui_alpha) &&
      (!generate.hudless_color || (!generate.ui && !generate.ui_alpha))) return kResultMissingInput;
  // Two independently supplied alpha signals have no specified precedence.
  if (generate.ui && generate.ui_alpha) return kResultUnsupportedParameter;
  for (auto resource : {generate.ui, generate.ui_alpha})
    if (resource && (resource == output || resource == generate.real_output)) return kResultInvalidParameter;
  if (generate.ui) {
    const auto format = generate.ui->GetDesc().Format;
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_R32G32B32A32_FLOAT)
      return kResultUnsupportedParameter;
  }
  if (generate.ui_alpha) {
    const auto format = generate.ui_alpha->GetDesc().Format;
    if (format != DXGI_FORMAT_R8_UNORM && format != DXGI_FORMAT_R16_UNORM &&
        format != DXGI_FORMAT_R16_FLOAT && format != DXGI_FORMAT_R32_FLOAT) return kResultUnsupportedParameter;
  }
  const UINT count = getOptionalUInt(params, "DLSSG.MultiFrameCount", 1);
  const UINT index = getOptionalUInt(params, "DLSSG.MultiFrameIndex", 1);
  if (!count || !index) return kResultInvalidParameter;
  if (count > 1 || index > 1) return kResultUnsupportedParameter;
  if (output == generate.present_color || output == generate.hudless_color ||
      output == prepare.depth || output == prepare.motion_vectors) return kResultInvalidParameter;
  if (generate.real_output && (generate.real_output == output || generate.real_output == generate.present_color ||
      generate.real_output == generate.hudless_color || generate.real_output == prepare.depth ||
      generate.real_output == prepare.motion_vectors)) return kResultInvalidParameter;
  const UINT eval_flags = getOptionalUInt(params,"DLSSG.EvalFlags",0);
  if (eval_flags & ~1u) return kResultUnsupportedParameter;
  generate.output_subrect_only = eval_flags & 1u;
  if (generate.distortion && (generate.distortion == output || generate.distortion == generate.real_output))
    return kResultInvalidParameter;
  prepare.reconstruct_camera_motion = !getOptionalUInt(params,"DLSSG.CameraMotionIncluded",1);
  const auto orthographic = getOptionalUInt(params,"DLSSG.OrthoProjection",feature.fg_create.orthographic_projection);
  if (orthographic > 1) return kResultInvalidParameter;
  void *projection = nullptr;
  const bool has_projection = !NVNGX_FAILED(params->Get("DLSSG.CameraViewToClip", &projection));
  if (has_projection) {
    if (!projection) return kResultInvalidParameter;
    std::memcpy(prepare.view_to_clip, projection, sizeof(prepare.view_to_clip));
    for (float value : prepare.view_to_clip)
      if (!std::isfinite(value)) return kResultInvalidParameter;
    prepare.projection_matrix_present = 1;
  } else {
    void *clip_to_camera = nullptr;
    const bool has_clip_to_camera = !NVNGX_FAILED(params->Get("DLSSG.ClipToCameraView", &clip_to_camera));
    if (has_clip_to_camera) {
      if (!clip_to_camera || !invertMatrix(static_cast<const float *>(clip_to_camera), prepare.view_to_clip))
        return kResultInvalidParameter;
      prepare.projection_matrix_present = 1;
    }
  }
  if (orthographic && !prepare.projection_matrix_present) return kResultMissingInput;
  if (orthographic && (prepare.view_to_clip[3] != 0 || prepare.view_to_clip[7] != 0 ||
      prepare.view_to_clip[11] != 0 || prepare.view_to_clip[15] == 0)) return kResultInvalidParameter;
  if (prepare.reconstruct_camera_motion) {
    void *matrix = nullptr;
    if (NVNGX_FAILED(params->Get("DLSSG.ClipToPrevClip", &matrix))) {
      void *inverse = nullptr;
      if (NVNGX_FAILED(params->Get("DLSSG.PrevClipToClip", &inverse))) return kResultMissingInput;
      if (!inverse || !invertMatrix(static_cast<const float *>(inverse), prepare.clip_to_previous_clip))
        return kResultInvalidParameter;
    } else {
      if (!matrix) return kResultInvalidParameter;
      std::memcpy(prepare.clip_to_previous_clip, matrix, sizeof(prepare.clip_to_previous_clip));
    }
    for (float value : prepare.clip_to_previous_clip)
      if (!std::isfinite(value)) return kResultInvalidParameter;
  }
  const auto present_desc = generate.present_color->GetDesc();
  for (auto resource : {output, generate.real_output}) {
    if (!resource) continue;
    Com<ID3D12Device> output_device;
    const auto desc = resource->GetDesc();
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&output_device))) ||
        identityOf(output_device.ptr()).ptr() != feature.device_identity.ptr() ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        (!generate.output_subrect_only && (desc.Width != present_desc.Width || desc.Height != present_desc.Height)) ||
        desc.Format != feature.fg_create.backbuffer_format ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return kResultInvalidParameter;
  }
  generate.disable_interpolation = getOptionalResource(params, "DLSSG.OutputDisableInterpolation");
  if (generate.disable_interpolation) {
    Com<ID3D12Device> owner;
    const auto desc = generate.disable_interpolation->GetDesc();
    if (FAILED(generate.disable_interpolation->GetDevice(IID_PPV_ARGS(&owner))) ||
        identityOf(owner.ptr()).ptr() != feature.device_identity.ptr() ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width < sizeof(UINT) ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return kResultInvalidParameter;
  }
  prepare.render_width = getOptionalUInt(params,"DLSSG.InternalWidth",feature.fg_create.max_render_width);
  prepare.render_height = getOptionalUInt(params,"DLSSG.InternalHeight",feature.fg_create.max_render_height);
  if (auto width = getOptionalUInt(params,"DLSSG.DepthSubrectWidth",0)) prepare.render_width = width;
  if (auto height = getOptionalUInt(params,"DLSSG.DepthSubrectHeight",0)) prepare.render_height = height;
  prepare.depth_subrect_x = getOptionalUInt(params,"DLSSG.DepthSubrectBaseX",0);
  prepare.depth_subrect_y = getOptionalUInt(params,"DLSSG.DepthSubrectBaseY",0);
  prepare.motion_subrect_x = getOptionalUInt(params,"DLSSG.MVecsSubrectBaseX",0);
  prepare.motion_subrect_y = getOptionalUInt(params,"DLSSG.MVecsSubrectBaseY",0);
  const UINT left = getOptionalUInt(params,"DLSSG.BackbufferSubrectBaseX",0);
  const UINT top = getOptionalUInt(params,"DLSSG.BackbufferSubrectBaseY",0);
  generate.independent_subrects = 1;
  generate.present_x = getOptionalUInt(params,"DLSSG.InputBackbufferSubrectBaseX",left);
  generate.present_y = getOptionalUInt(params,"DLSSG.InputBackbufferSubrectBaseY",top);
  generate.hudless_x = getOptionalUInt(params,"DLSSG.HUDLessSubrectBaseX",left);
  generate.hudless_y = getOptionalUInt(params,"DLSSG.HUDLessSubrectBaseY",top);
  generate.ui_x = getOptionalUInt(params,"DLSSG.UISubrectBaseX",left);
  generate.ui_y = getOptionalUInt(params,"DLSSG.UISubrectBaseY",top);
  generate.ui_alpha_x = getOptionalUInt(params,"DLSSG.UIAlphaSubrectBaseX",left);
  generate.ui_alpha_y = getOptionalUInt(params,"DLSSG.UIAlphaSubrectBaseY",top);
  generate.output_x = getOptionalUInt(params,"DLSSG.OutputInterpolatedSubrectBaseX",left);
  generate.output_y = getOptionalUInt(params,"DLSSG.OutputInterpolatedSubrectBaseY",top);
  generate.real_x = getOptionalUInt(params,"DLSSG.OutputRealSubrectBaseX",left);
  generate.real_y = getOptionalUInt(params,"DLSSG.OutputRealSubrectBaseY",top);
  const UINT default_mw = (feature.fg_create.flags & 4) ? feature.fg_create.display_width : prepare.render_width;
  const UINT default_mh = (feature.fg_create.flags & 4) ? feature.fg_create.display_height : prepare.render_height;
  const UINT mw = getOptionalUInt(params, "DLSSG.MVecsSubrectWidth", 0) ?: default_mw;
  const UINT mh = getOptionalUInt(params, "DLSSG.MVecsSubrectHeight", 0) ?: default_mh;
  prepare.motion_width = mw;
  prepare.motion_height = mh;
  auto validSubrect = [&](ID3D12Resource *resource, UINT x, UINT y, UINT width, UINT height) {
    Com<ID3D12Device> owner;
    const auto desc = resource->GetDesc();
    return SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&owner))) &&
        identityOf(owner.ptr()).ptr() == feature.device_identity.ptr() &&
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && width && height && x <= desc.Width && y <= desc.Height &&
        width <= desc.Width - x && height <= desc.Height - y;
  };
  if (generate.output_x > INT_MAX || generate.output_y > INT_MAX ||
      !validSubrect(prepare.depth, prepare.depth_subrect_x, prepare.depth_subrect_y, prepare.render_width, prepare.render_height) ||
      !validSubrect(prepare.motion_vectors, prepare.motion_subrect_x, prepare.motion_subrect_y, mw, mh) ||
      !validSubrect(generate.present_color, generate.present_x, generate.present_y,
          feature.fg_create.display_width, feature.fg_create.display_height) ||
      !validSubrect(output, generate.output_x, generate.output_y,
          feature.fg_create.display_width, feature.fg_create.display_height) ||
      (generate.hudless_color && !validSubrect(generate.hudless_color, generate.hudless_x, generate.hudless_y,
          feature.fg_create.display_width, feature.fg_create.display_height)) ||
      (generate.real_output && !validSubrect(generate.real_output, generate.real_x, generate.real_y,
          feature.fg_create.display_width, feature.fg_create.display_height))) return kResultInvalidParameter;
  if ((generate.ui && !validSubrect(generate.ui,generate.ui_x,generate.ui_y,
          feature.fg_create.display_width,feature.fg_create.display_height)) ||
      (generate.ui_alpha && !validSubrect(generate.ui_alpha,generate.ui_alpha_x,generate.ui_alpha_y,
          feature.fg_create.display_width,feature.fg_create.display_height))) return kResultInvalidParameter;
  generate.generation_rect_left = generate.output_x; generate.generation_rect_top = generate.output_y;
  if (generate.distortion) {
    generate.distortion_low_precision = getOptionalUInt(params,"DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision",0);
    if (generate.distortion_low_precision > 1) return kResultInvalidParameter;
    if (generate.distortion_low_precision) {
      if (NVNGX_FAILED(params->Get("DLSSG.BidirectionalDistortionFieldLowPrecision.Scale", &generate.distortion_scale)) ||
          NVNGX_FAILED(params->Get("DLSSG.BidirectionalDistortionFieldLowPrecision.Bias", &generate.distortion_bias)))
        return kResultMissingInput;
      if (!std::isfinite(generate.distortion_scale) || !std::isfinite(generate.distortion_bias))
        return kResultInvalidParameter;
    }
    const auto format = generate.distortion->GetDesc().Format;
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
        !(generate.distortion_low_precision && (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
          format == DXGI_FORMAT_R16G16B16A16_UNORM || format == DXGI_FORMAT_R16G16B16A16_SNORM)))
      return kResultUnsupportedParameter;
    generate.distortion_x = getOptionalUInt(params,"DLSSG.BidirectionalDistortionFieldSubrectBaseX",0);
    generate.distortion_y = getOptionalUInt(params,"DLSSG.BidirectionalDistortionFieldSubrectBaseY",0);
    generate.distortion_width = getOptionalUInt(params,"DLSSG.BidirectionalDistortionFieldSubrectWidth",0);
    generate.distortion_height = getOptionalUInt(params,"DLSSG.BidirectionalDistortionFieldSubrectHeight",0);
    if (!generate.distortion_width) generate.distortion_width = prepare.render_width;
    if (!generate.distortion_height) generate.distortion_height = prepare.render_height;
    if (!validSubrect(generate.distortion, generate.distortion_x, generate.distortion_y,
        generate.distortion_width, generate.distortion_height)) return kResultInvalidParameter;
    generate.distortion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  }
  if (getOptionalUInt(params,"DLSSG.DepthInverted",feature.fg_create.flags & 1) != (feature.fg_create.flags & 1) ||
      getOptionalUInt(params,"DLSSG.MvecJittered",(feature.fg_create.flags >> 3) & 1) != ((feature.fg_create.flags >> 3) & 1))
    return kResultUnsupportedParameter;
  for (const auto &[key, expected] : {
       std::pair{"DLSSG.DepthSubrectWidth",prepare.render_width},std::pair{"DLSSG.DepthSubrectHeight",prepare.render_height},
       std::pair{"DLSSG.InputBackbufferSubrectWidth",feature.fg_create.display_width},
       std::pair{"DLSSG.InputBackbufferSubrectHeight",feature.fg_create.display_height},
       std::pair{"DLSSG.OutputInterpolatedSubrectWidth",feature.fg_create.display_width},
       std::pair{"DLSSG.OutputInterpolatedSubrectHeight",feature.fg_create.display_height},
       std::pair{"DLSSG.OutputRealSubrectWidth",feature.fg_create.display_width},
       std::pair{"DLSSG.OutputRealSubrectHeight",feature.fg_create.display_height},
       std::pair{"DLSSG.HUDLessSubrectWidth",feature.fg_create.display_width},
       std::pair{"DLSSG.HUDLessSubrectHeight",feature.fg_create.display_height},
       std::pair{"DLSSG.UISubrectWidth",feature.fg_create.display_width},
       std::pair{"DLSSG.UISubrectHeight",feature.fg_create.display_height},
       std::pair{"DLSSG.UIAlphaSubrectWidth",feature.fg_create.display_width},
       std::pair{"DLSSG.UIAlphaSubrectHeight",feature.fg_create.display_height}}) {
    const auto extent = getOptionalUInt(params,key,0);
    if (extent && extent != expected) return kResultUnsupportedParameter;
  }
  prepare.depth_state = prepare.motion_vectors_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  generate.present_color_state = generate.hudless_color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  generate.ui_state = generate.ui_alpha_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  // NGX clip-space (+Y up) jitter becomes render pixels (+Y down).
  prepare.jitter_x = getOptionalFloat(params,"DLSSG.JitterOffsetX",0) * prepare.render_width * .5f;
  prepare.jitter_y = getOptionalFloat(params,"DLSSG.JitterOffsetY",0) * prepare.render_height * -.5f;
  // Public NGX scales normalize to [-1,1]; private transport multiplies into pixels.
  prepare.motion_scale_x = getOptionalFloat(params,"DLSSG.MvecScaleX",1) * mw;
  prepare.motion_scale_y = getOptionalFloat(params,"DLSSG.MvecScaleY",1) * mh;
  if (getOptionalUInt(params,"DLSSG.InvertXAxis",0)) prepare.motion_scale_x *= -1;
  if (getOptionalUInt(params,"DLSSG.InvertYAxis",0)) prepare.motion_scale_y *= -1;
  prepare.camera_near = getOptionalFloat(params,"DLSSG.CameraNear",0);
  prepare.camera_far = getOptionalFloat(params,"DLSSG.CameraFar",0);
  prepare.camera_fov_vertical_radians = getOptionalFloat(params,"DLSSG.CameraFOV",0);
  if (!prepare.camera_near || !prepare.camera_far || !prepare.camera_fov_vertical_radians) return kResultMissingInput;
  bool has_aspect = false;
  prepare.camera_aspect_ratio = getOptionalFloat(params, "DLSSG.CameraAspectRatio", 0, &has_aspect);
  if (has_aspect && (!std::isfinite(prepare.camera_aspect_ratio) || prepare.camera_aspect_ratio <= 0))
    return kResultInvalidParameter;
  bool has_invalid_motion = false;
  prepare.motion_vectors_invalid_value = getOptionalFloat(params, "DLSSG.MvecInvalidValue", 0, &has_invalid_motion);
  if (has_invalid_motion && prepare.reconstruct_camera_motion) {
    if (!std::isfinite(prepare.motion_vectors_invalid_value)) return kResultUnsupportedParameter;
    prepare.motion_vectors_invalid_value_present = 1;
  }
  if (getOptionalResource(params, "DLSSG.NoWarp")) return kResultUnsupportedParameter;
  // No public convention maps an extra pinhole/lens transform onto MetalFX's projection or distortion field.
  for (const char *key : {"DLSSG.CameraPinholeOffsetX", "DLSSG.CameraPinholeOffsetY"}) {
    const float offset = getOptionalFloat(params, key, 0);
    if (!std::isfinite(offset)) return kResultInvalidParameter;
    if (offset != 0) return kResultUnsupportedParameter;
  }
  void *lens = nullptr;
  if (!NVNGX_FAILED(params->Get("DLSSG.ClipToLensClip", &lens))) {
    if (!lens) return kResultInvalidParameter;
    const auto values = static_cast<const float *>(lens);
    bool identity = true;
    for (unsigned i = 0; i != 16; ++i) {
      if (!std::isfinite(values[i])) return kResultInvalidParameter;
      identity &= values[i] == (i % 5 == 0 ? 1.f : 0.f);
    }
    if (!identity) return kResultUnsupportedParameter;
  }
  const char *camera_keys[4][3] = {{"DLSSG.CameraPosX","DLSSG.CameraPosY","DLSSG.CameraPosZ"},
      {"DLSSG.CameraUpX","DLSSG.CameraUpY","DLSSG.CameraUpZ"},
      {"DLSSG.CameraRightX","DLSSG.CameraRightY","DLSSG.CameraRightZ"},
      {"DLSSG.CameraFwdX","DLSSG.CameraFwdY","DLSSG.CameraFwdZ"}};
  float *camera_vectors[] = {prepare.camera_position,prepare.camera_up,prepare.camera_right,prepare.camera_forward};
  bool any_camera = false, all_camera = true;
  for (size_t vector = 0; vector < 4; ++vector)
    for (size_t axis = 0; axis < 3; ++axis) {
      bool present = false;
      camera_vectors[vector][axis] = getOptionalFloat(params,camera_keys[vector][axis],0,&present);
      any_camera |= present; all_camera &= present;
    }
  if (any_camera && !all_camera) return kResultMissingInput;
  if (prepare.projection_matrix_present && !all_camera) return kResultMissingInput;
  prepare.camera_info_present = all_camera;
  prepare.view_space_to_meters = 1;
  prepare.reset = generate.reset = getOptionalUInt(params,"DLSSG.Reset",0) ||
      getOptionalUInt(params,"DLSSG.AutomodeOverrideReset",0) || getOptionalUInt(params,"DLSSG.NotRenderingGameFrames",0);
  generate.num_generated_frames = count;
  generate.generation_rect_width = feature.fg_create.display_width; generate.generation_rect_height = feature.fg_create.display_height;
  // Public DLSS-FG requires display-ready PQ for HDR; scRGB/scene-linear input is excluded.
  generate.backbuffer_transfer_function = getOptionalUInt(params,"DLSSG.ColorBuffersHDR",0) ? 1 : 0;
  generate.min_luminance = 0; generate.max_luminance = generate.backbuffer_transfer_function ? 10000 : 1;
  std::lock_guard lock(feature.fg_mutex);
  unsigned long long frame_id = feature.next_frame_id;
  params->Get("DLSSG.BackbufferFrameID", &frame_id);
  if (frame_id == UINT64_MAX) return kResultInvalidParameter;
  prepare.frame_id = generate.frame_id = frame_id;
  LARGE_INTEGER now{};
  if (!QueryPerformanceCounter(&now) || now.QuadPart <= feature.fg_last_counter.QuadPart ||
      feature.fg_counter_frequency.QuadPart <= 0) return kResultPlatformError;
  prepare.frame_time_delta_ms = getOptionalFloat(params, "FrameTimeDeltaInMsec", static_cast<float>(1000.0 *
      (now.QuadPart - feature.fg_last_counter.QuadPart) / feature.fg_counter_frequency.QuadPart));
  generate.outputs[0] = output;
  generate.output_states[0] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  generate.real_output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Generate is recorded next on this list, so Prepare need not snapshot depth/motion.
  prepare.flags |= MTL_FRAME_GENERATION_PREPARE_DIRECT_INPUTS;
  auto hr = extension->PrepareFrameGeneration(feature.frame_generation.ptr(), &prepare);
  if (FAILED(hr)) return mapBackendResult(hr, false);
  hr = extension->GenerateFrames(feature.frame_generation.ptr(), &generate);
  // Retirement affects metadata only; the recorded packet owns all GPU resources.
  UINT64 oldest = 0; BOOL pending = FALSE;
  const auto retired = feature.frame_generation->RetireFrame(frame_id, &oldest, &pending);
  if (FAILED(hr)) return mapBackendResult(hr, false);
  if (FAILED(retired)) return mapBackendResult(retired, false);
  if (frame_id >= feature.next_frame_id) feature.next_frame_id = frame_id + 1;
  feature.fg_last_counter = now;
  return kResultSuccess;
}

} // namespace

extern "C" {

void owned_ngxd3d12_param_set_ull(void *storage, const char *name, unsigned long long value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_float(void *storage, const char *name, float value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_double(void *storage, const char *name, double value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_uint(void *storage, const char *name, unsigned int value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_int(void *storage, const char *name, int value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_d3d11(void *storage, const char *name, ID3D11Resource *value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_d3d12(void *storage, const char *name, ID3D12Resource *value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}
void owned_ngxd3d12_param_set_pointer(void *storage, const char *name, void *value) {
  static_cast<ParametersImpl *>(storage)->Set(name, value);
}

#define OWNED_NGX_PARAMETER_GET(name, type)                                                                                 \
  unsigned int name(void *storage, const char *key, type value) {                                                           \
    if (!storage || !key || !value)                                                                                         \
      return static_cast<unsigned int>(kResultInvalidParameter);                                                            \
    return static_cast<ParametersImpl *>(storage)->Get(key, value) == NVNGX_RESULT_OK                                      \
               ? static_cast<unsigned int>(kResultSuccess)                                                                  \
               : static_cast<unsigned int>(kResultFail);                                                                    \
  }

OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_ull, unsigned long long *)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_float, float *)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_double, double *)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_uint, unsigned int *)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_int, int *)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_d3d11, ID3D11Resource **)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_d3d12, ID3D12Resource **)
OWNED_NGX_PARAMETER_GET(owned_ngxd3d12_param_get_pointer, void **)

#undef OWNED_NGX_PARAMETER_GET

void owned_ngxd3d12_param_reset(void *storage) {
  if (storage)
    static_cast<ParametersImpl *>(storage)->Reset();
}

} // extern "C"

NVNGX_RESULT
NVNGXD3D12InitDevice(ID3D12Device *device) {
  owned_graphics::Trace("ngx_d3d12_init", ",\"device_present\":", device != nullptr);
  if (!device)
    return kResultInvalidParameter;
  auto identity = identityOf(device);
  if (!identity)
    return kResultInvalidParameter;
  Com<IMTLD3D12DeviceExt> extension = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&extension))) || !extension)
    return kResultFeatureNotSupported;
  BOOL supported = FALSE;
  if (FAILED(extension->SupportsTemporalUpscale(&supported)))
    return kResultPlatformError;
  if (!supported)
    return kResultFeatureNotSupported;

  DeviceEntry entry{identity, Com<ID3D12Device>(device), extension, std::make_shared<DeviceRegistration>()};
  std::lock_guard lock(g_mutex);
  g_devices[identity.ptr()] = std::move(entry);
  owned_graphics::Trace("ngx_d3d12_init_ready", ",\"result\":", static_cast<unsigned>(kResultSuccess));
  return kResultSuccess;
}

void
NVNGXD3D12ShutdownAll() {
  std::lock_guard lock(g_mutex);
  g_features.clear();
  g_devices.clear();
  g_parameters.clear();
}

NVNGX_RESULT
NVNGXD3D12ShutdownDevice(ID3D12Device *device) {
  if (!device)
    return kResultInvalidParameter;
  auto identity = identityOf(device);
  if (!identity)
    return kResultInvalidParameter;
  std::lock_guard lock(g_mutex);
  auto it = g_devices.find(identity.ptr());
  if (it == g_devices.end())
    return kResultNotInitialized;
  for (auto feature = g_features.begin(); feature != g_features.end();) {
    if (feature->second->device_identity.ptr() == identity.ptr())
      feature = g_features.erase(feature);
    else
      ++feature;
  }
  g_devices.erase(it);
  if (g_devices.empty())
    g_parameters.clear();
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init(
    unsigned long long, const wchar_t *, ID3D12Device *device, const void *, unsigned int
) {
  return NVNGXD3D12InitDevice(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long, const wchar_t *, ID3D12Device *device, unsigned int, const void *
) {
  return NVNGXD3D12InitDevice(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_ProjectID(
    const char *, unsigned int, const char *, const wchar_t *, ID3D12Device *device, unsigned int, const void *
) {
  return NVNGXD3D12InitDevice(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char *, unsigned int, const char *, const wchar_t *, ID3D12Device *device, const void *, unsigned int
) {
  return NVNGXD3D12InitDevice(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Shutdown() {
  NVNGXD3D12ShutdownAll();
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *device) {
  // Official nvsdk_ngx.h: a null device shuts down every SDK instance.
  if (!device) {
    NVNGXD3D12ShutdownAll();
    return kResultSuccess;
  }
  return NVNGXD3D12ShutdownDevice(device);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_AllocateParameters(NVNGXParameter **out_params) {
  if (!out_params)
    return kResultInvalidParameter;
  return registerParameter(std::make_unique<ParametersImpl>(), false, out_params);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_DestroyParameters(NVNGXParameter *params) {
  if (!params)
    return kResultInvalidParameter;
  std::shared_ptr<ParameterState> state;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_parameters.find(params);
    if (it == g_parameters.end() || it->second->managed)
      return kResultInvalidParameter;
    state = std::move(it->second);
    g_parameters.erase(it);
  }
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetCapabilityParameters(NVNGXParameter **out_params) {
  if (!out_params)
    return kResultInvalidParameter;
  auto params = makeCapabilityParameters();
  if (!params)
    return kResultFail;
  return registerParameter(std::move(params), false, out_params);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetParameters(NVNGXParameter **out_params) {
  if (!out_params)
    return kResultInvalidParameter;
  auto params = makeCapabilityParameters();
  if (!params)
    return kResultFail;
  return registerParameter(std::move(params), true, out_params);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetScratchBufferSize(unsigned int feature, const NVNGXParameter *, size_t *out_size) {
  if (!out_size)
    return kResultInvalidParameter;
  *out_size = 0;
  if (!supportedFeature(feature))
    return kResultFeatureNotSupported;
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList *list, unsigned int feature, NVNGXParameter *params, NVNGXHandle **out_handle
) {
  owned_graphics::Trace("ngx_d3d12_create_begin", ",\"feature\":", feature);
  if (!out_handle)
    return kResultInvalidParameter;
  *out_handle = nullptr;
  if (!list || !params)
    return kResultInvalidParameter;
  if (!supportedFeature(feature))
    return kResultFeatureNotSupported;

  Com<IMTLD3D12GraphicsCommandListExt> list_ext = nullptr;
  Com<ID3D12Device> device = nullptr;
  auto result = validateCommandList(list, nullptr, list_ext, device);
  owned_graphics::Trace("ngx_d3d12_create_list", ",\"result\":", static_cast<unsigned>(result));
  if (result != kResultSuccess)
    return result;
  DeviceEntry device_entry;
  const bool initialized = findInitializedDevice(device.ptr(), &device_entry);
  owned_graphics::Trace("ngx_d3d12_create_device", ",\"initialized\":", initialized);
  if (!initialized)
    return kResultNotInitialized;

  auto parameter = lookupParameter(params);
  owned_graphics::Trace("ngx_d3d12_create_parameters", ",\"found\":", parameter != nullptr);
  if (!parameter)
    return kResultInvalidParameter;
  auto state = std::make_shared<FeatureState>();
  state->kind = feature;
  state->device_identity = device_entry.identity;
  state->device = device_entry.device;
  if (feature == kFeatureFrameGeneration) {
    result = parseFrameGenerationCreate(parameter->storage.get(),device.ptr(),state->fg_create);
    if (result != kResultSuccess) return result;
    auto fg_device = Com<IMTLD3D12FrameGenerationDevice>::queryFrom(device.ptr());
    if (!fg_device) return kResultFeatureNotSupported;
    auto hr = fg_device->CreateFrameGenerationState(&state->fg_create,&state->frame_generation);
    if (FAILED(hr) || !state->frame_generation) return mapBackendResult(FAILED(hr) ? hr : E_FAIL,true);
    hr = state->frame_generation->Configure(TRUE);
    if (FAILED(hr)) return mapBackendResult(hr,true);
    if (!QueryPerformanceFrequency(&state->fg_counter_frequency) ||
        !QueryPerformanceCounter(&state->fg_last_counter)) return kResultPlatformError;
  } else {
    result = parseCreate(parameter->storage.get(),device.ptr(),&state->create,feature == kFeatureSpatial);
    owned_graphics::Trace("ngx_d3d12_create_parse", ",\"result\":", static_cast<unsigned>(result),
        ",\"input_width\":", state->create.InputWidth, ",\"input_height\":", state->create.InputHeight,
        ",\"output_width\":", state->create.OutputWidth, ",\"output_height\":", state->create.OutputHeight,
        ",\"flags\":", state->create.FeatureFlags);
    if (result != kResultSuccess) return result;
    state->sharpening = (getOptionalInt(parameter->storage.get(),NVNGX_Parameter_DLSS_Feature_Create_Flags,0) & NVNGX_DLSS_FLAG_DO_SHARPENING) != 0;
    if (feature == kFeatureRayReconstruction) {
      if (getOptionalInt(parameter->storage.get(),"DLSS.Denoise.Mode",1) != 1) return kResultUnsupportedParameter;
      state->roughness_packed = getOptionalUInt(parameter->storage.get(),"DLSS.Roughness.Mode",0);
      const UINT hardware_depth = getOptionalUInt(parameter->storage.get(),"DLSS.Use.HW.Depth",0);
      if (state->roughness_packed > 1 || hardware_depth > 1) return kResultInvalidParameter;
      state->depth_linear = !hardware_depth;
      if (state->sharpening || (state->create.FeatureFlags & 128)) return kResultUnsupportedParameter;
      // NGX RR guide §3.7 excludes SR exposure/auto-exposure. Native RR receives neutral exposure.
      state->create.FeatureFlags &= ~NVNGX_DLSS_FLAG_AUTO_EXPOSURE;
      state->create.FeatureFlags |= kRayReconstructionFlag;
    }
    const auto hr = device_entry.extension->CreateTemporalUpscaleState(&state->create,&state->backend);
    owned_graphics::Trace("ngx_d3d12_create_backend", ",\"hr\":", static_cast<unsigned>(hr));
    if (FAILED(hr) || !state->backend) return mapBackendResult(FAILED(hr) ? hr : E_FAIL,true);
  }

  auto *public_handle = allocateFeatureHandle();
  if (!public_handle)
    return kResultFail;
  {
    std::lock_guard lock(g_mutex);
    // The backend state above was built outside the lock. If the device was
    // shut down (or shut down and re-inited) meanwhile, publishing this handle
    // would attach a feature to a registration the caller no longer holds.
    auto current = g_devices.find(device_entry.identity.ptr());
    if (current == g_devices.end() || current->second.registration != device_entry.registration)
      return kResultNotInitialized;
    g_features[public_handle] = std::move(state);
  }
  *out_handle = public_handle;
  owned_graphics::Trace("ngx_d3d12_create_ready", ",\"feature\":", feature);
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList *list, const NVNGXHandle *handle, NVNGXParameter *params, void *
) {
  owned_graphics::Trace("ngx_d3d12_evaluate_begin", ",\"list_present\":", list != nullptr,
      ",\"handle_present\":", handle != nullptr, ",\"params_present\":", params != nullptr);
  if (!list || !handle || !params)
    return kResultInvalidParameter;
  auto feature = lookupFeature(handle);
  owned_graphics::Trace("ngx_d3d12_evaluate_handle", ",\"found\":", feature != nullptr);
  if (!feature)
    return kResultFeatureNotFound;

  Com<IMTLD3D12GraphicsCommandListExt> extension = nullptr;
  Com<ID3D12Device> device = nullptr;
  auto result = validateCommandList(list, feature->device_identity, extension, device);
  owned_graphics::Trace("ngx_d3d12_evaluate_list", ",\"result\":", static_cast<unsigned>(result));
  if (result != kResultSuccess)
    return result;

  MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC frame = {};
  auto parameter = lookupParameter(params);
  if (!parameter)
    return kResultInvalidParameter;
  if (feature->kind == kFeatureFrameGeneration)
    return evaluateFrameGeneration(list,parameter->storage.get(),*feature);
  result = feature->kind == kFeatureRayReconstruction
      ? parseRayReconstructionFrame(parameter->storage.get(),*feature,frame)
      : parseFrame(parameter->storage.get(), *feature, &frame);
  owned_graphics::Trace("ngx_d3d12_evaluate_parse", ",\"result\":", static_cast<unsigned>(result),
      ",\"input_width\":", frame.InputContentWidth, ",\"input_height\":", frame.InputContentHeight,
      ",\"pre_exposure\":", frame.PreExposure, ",\"sharpening\":", frame.EnableSharpening);
  if (result != kResultSuccess)
    return result;
  const auto hr = extension->TemporalUpscale(feature->backend.ptr(), &frame);
  owned_graphics::Trace("ngx_d3d12_evaluate_backend", ",\"hr\":", static_cast<unsigned>(hr));
  return mapBackendResult(hr, false);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_EvaluateFeature_C(
    ID3D12GraphicsCommandList *list, const NVNGXHandle *handle, NVNGXParameter *params, void *callback
) {
  return NVSDK_NGX_D3D12_EvaluateFeature(list, handle, params, callback);
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_ReleaseFeature(NVNGXHandle *handle) {
  if (!handle)
    return kResultInvalidParameter;
  std::lock_guard lock(g_mutex);
  auto it = g_features.find(handle);
  if (it == g_features.end())
    return kResultFeatureNotFound;
  g_features.erase(it);
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter *adapter, const NVNGX_FeatureDiscoveryInfo *discovery_info, NVNGX_FeatureRequirement *requirement
) {
  if (!adapter || !discovery_info || !requirement)
    return kResultInvalidParameter;
  requirement->FeatureSupported = kSupportAdapterUnsupported;
  requirement->MinHWArchitecture = 0;
  requirement->MinOSVersion[0] = 0;
  const UINT feature = static_cast<UINT>(discovery_info->FeatureID);
  if (!supportedFeature(feature)) {
    return kResultFeatureNotSupported;
  }
  const auto owned_adapter = Com<IMTLDXGIAdapter>::queryFrom(adapter);
  const UINT flags = feature == kFeatureSpatial ? WMTMetal4FXSpatialFlag :
      feature == kFeatureRayReconstruction ? kRayReconstructionFlag : feature == kFeatureFrameGeneration ? 512u : 0u;
  if (!owned_adapter || !WMT4::TemporalFeature::Supports(owned_adapter->GetMTLDevice(),flags))
    return kResultSuccess;
  requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_SUPPORTED;
  strcpy_s(requirement->MinOSVersion, "10.0.16299.0");
  return kResultSuccess;
}

} // namespace d3dalloy
