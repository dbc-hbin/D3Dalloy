#include "nvngx_d3d12.hpp"

#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "nvngx_parameter.hpp"
#include "../d3d12/d3d12_interfaces.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace dxmt {

extern "C" {
unsigned long long owned_ngxd3d12_msvc_parameter_size();
unsigned long long owned_ngxd3d12_msvc_parameter_align();
void *owned_ngxd3d12_msvc_parameter_construct(void *memory, void *storage);
void owned_ngxd3d12_msvc_parameter_destruct(void *object);
}

namespace {

__attribute__((used)) static const char kOwnedNgxD3D12Marker[] = "own-ngxd3d12";

constexpr uint32_t kFeatureSuperSampling = 1;
constexpr uint32_t kSupportedCreateFlags =
    NVNGX_DLSS_FLAG_HDR | NVNGX_DLSS_FLAG_MV_LOWRES | NVNGX_DLSS_FLAG_DEPTH_INVERTED |
    NVNGX_DLSS_FLAG_DO_SHARPENING | NVNGX_DLSS_FLAG_AUTO_EXPOSURE;

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
  Com<IUnknown> device_identity;
  Com<ID3D12Device> device;
  Com<IMTLD3D12TemporalUpscaleState> backend;
  MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC create = {};
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
  return out;
}

NVNGX_RESULT
parseCreate(const ParametersImpl *params, ID3D12Device *device, MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc) {
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

  const int flags = getOptionalInt(params, NVNGX_Parameter_DLSS_Feature_Create_Flags, 0);
  if (flags < 0)
    return kResultUnsupportedParameter;
  desc->FeatureFlags = static_cast<uint32_t>(flags);
  if ((desc->FeatureFlags & ~kSupportedCreateFlags) != 0 ||
      (desc->FeatureFlags & NVNGX_DLSS_FLAG_MV_LOWRES) == 0)
    return kResultUnsupportedParameter;

  const int subrects = getOptionalInt(params, "DLSS.Enable.Output.Subrects", 0);
  if (subrects != 0 && subrects != 1)
    return kResultInvalidParameter;
  desc->OutputSubrectsEnabled = subrects ? TRUE : FALSE;
  desc->CreationNodeMask = getOptionalUInt(params, "CreationNodeMask", 1u);
  desc->VisibilityNodeMask = getOptionalUInt(params, "VisibilityNodeMask", 1u);
  if (!deviceMaskValid(device, desc->CreationNodeMask) || !deviceMaskValid(device, desc->VisibilityNodeMask))
    return kResultInvalidParameter;
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
  if (auto result = getRequiredResource(params, NVNGX_Parameter_Depth, &desc->Depth); result != kResultSuccess)
    return result;
  if (auto result = getRequiredResource(params, NVNGX_Parameter_MotionVectors, &desc->MotionVector); result != kResultSuccess)
    return result;

  desc->ExposureTexture = getOptionalResource(params, NVNGX_Parameter_ExposureTexture);
  desc->BiasCurrentColorMask = getOptionalResource(params, NVNGX_Parameter_BiasCurrentColorMask);
  desc->TransparencyMask = getOptionalResource(params, NVNGX_Parameter_TransparencyMask);
  if (desc->BiasCurrentColorMask || desc->TransparencyMask)
    return kResultUnsupportedParameter;

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
  };
  for (const auto &offset : offsets) {
    *offset.value = getOptionalUInt(params, offset.name, 0);
    if (*offset.value)
      return kResultUnsupportedParameter;
  }
  // Output base is only meaningful for a feature created with output subrects
  // enabled. When it is enabled the backend owns logical-output-vs-backing
  // bounds validation; the frontend must not guess or clamp the rectangle.
  desc->OutputBaseX = getOptionalUInt(params, NVNGX_Parameter_DLSS_Output_Base_X, 0);
  desc->OutputBaseY = getOptionalUInt(params, NVNGX_Parameter_DLSS_Output_Base_Y, 0);
  if ((desc->OutputBaseX || desc->OutputBaseY) && !feature.create.OutputSubrectsEnabled)
    return kResultUnsupportedParameter;
  if (getOptionalUInt(params, NVNGX_Parameter_DLSS_Translucency_Base_X, 0) ||
      getOptionalUInt(params, NVNGX_Parameter_DLSS_Translucency_Base_Y, 0))
    return kResultUnsupportedParameter;

  desc->JitterOffsetX = getOptionalFloat(params, NVNGX_Parameter_Jitter_Offset_X, 0.0f);
  desc->JitterOffsetY = getOptionalFloat(params, NVNGX_Parameter_Jitter_Offset_Y, 0.0f);
  desc->MotionVectorScaleX = getOptionalFloat(params, NVNGX_Parameter_MV_Scale_X, 1.0f);
  desc->MotionVectorScaleY = getOptionalFloat(params, NVNGX_Parameter_MV_Scale_Y, 1.0f);
  const float raw_pre_exposure = getOptionalFloat(params, NVNGX_Parameter_DLSS_Pre_Exposure, 0.0f);
  const float raw_exposure_scale = getOptionalFloat(params, NVNGX_Parameter_DLSS_Exposure_Scale, 0.0f);
  const float sharpness = getOptionalFloat(params, NVNGX_Parameter_Sharpness, 0.0f);
  if (!std::isfinite(desc->JitterOffsetX) || !std::isfinite(desc->JitterOffsetY) ||
      !std::isfinite(desc->MotionVectorScaleX) || !std::isfinite(desc->MotionVectorScaleY) ||
      !std::isfinite(raw_pre_exposure) || !std::isfinite(raw_exposure_scale) || !std::isfinite(sharpness))
    return kResultInvalidParameter;
  if (desc->MotionVectorScaleX == 0.0f || desc->MotionVectorScaleY == 0.0f || raw_pre_exposure < 0.0f ||
      raw_exposure_scale < 0.0f)
    return kResultInvalidParameter;

  desc->PreExposure = raw_pre_exposure == 0.0f ? 1.0f : raw_pre_exposure;
  desc->ExposureScale = raw_exposure_scale == 0.0f ? 1.0f : raw_exposure_scale;
  if (desc->ExposureScale != 1.0f || sharpness != 0.0f)
    return kResultUnsupportedParameter;
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
  if (feature != kFeatureSuperSampling)
    return kResultFeatureNotSupported;
  *out_size = 0;
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList *list, unsigned int feature, NVNGXParameter *params, NVNGXHandle **out_handle
) {
  if (!list || !params || !out_handle)
    return kResultInvalidParameter;
  *out_handle = nullptr;
  if (feature != kFeatureSuperSampling)
    return kResultFeatureNotSupported;

  Com<IMTLD3D12GraphicsCommandListExt> list_ext = nullptr;
  Com<ID3D12Device> device = nullptr;
  auto result = validateCommandList(list, nullptr, list_ext, device);
  if (result != kResultSuccess)
    return result;
  DeviceEntry device_entry;
  if (!findInitializedDevice(device.ptr(), &device_entry))
    return kResultNotInitialized;

  MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC create = {};
  auto parameter = lookupParameter(params);
  if (!parameter)
    return kResultInvalidParameter;
  result = parseCreate(parameter->storage.get(), device.ptr(), &create);
  if (result != kResultSuccess)
    return result;

  Com<IMTLD3D12TemporalUpscaleState> backend = nullptr;
  const HRESULT hr = device_entry.extension->CreateTemporalUpscaleState(&create, &backend);
  if (FAILED(hr) || !backend)
    return mapBackendResult(FAILED(hr) ? hr : E_FAIL, true);

  auto state = std::make_shared<FeatureState>();
  state->device_identity = device_entry.identity;
  state->device = device_entry.device;
  state->backend = std::move(backend);
  state->create = create;

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
  return kResultSuccess;
}

NVNGX_API NVNGX_RESULT
NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList *list, const NVNGXHandle *handle, NVNGXParameter *params, void *
) {
  if (!list || !handle || !params)
    return kResultInvalidParameter;
  auto feature = lookupFeature(handle);
  if (!feature)
    return kResultFeatureNotFound;

  Com<IMTLD3D12GraphicsCommandListExt> extension = nullptr;
  Com<ID3D12Device> device = nullptr;
  auto result = validateCommandList(list, feature->device_identity, extension, device);
  if (result != kResultSuccess)
    return result;

  MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC frame = {};
  auto parameter = lookupParameter(params);
  if (!parameter)
    return kResultInvalidParameter;
  result = parseFrame(parameter->storage.get(), *feature, &frame);
  if (result != kResultSuccess)
    return result;
  return mapBackendResult(extension->TemporalUpscale(feature->backend.ptr(), &frame), false);
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
  if (discovery_info->FeatureID != NVNGX_FEATURE_SUPERSAMPLING) {
    requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_UNSUPPORTED;
    requirement->MinHWArchitecture = 0;
    requirement->MinOSVersion[0] = 0;
    return kResultFeatureNotSupported;
  }
  requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_SUPPORTED;
  requirement->MinHWArchitecture = 0;
  strcpy_s(requirement->MinOSVersion, "10.0.16299.0");
  return kResultSuccess;
}

} // namespace dxmt
