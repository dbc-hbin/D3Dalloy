// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

// Private cross-DLL ABI between amd_fidelityfx_denoiser_dx12.dll and d3d12.dll.
// Layouts follow AMD RR after legacy normalization; no native handles cross it.
#define MTL_RAY_REGENERATION_SIGNAL_COUNT 7
#define MTL_RAY_REGENERATION_RESOURCE_COUNT (5 + 2 * MTL_RAY_REGENERATION_SIGNAL_COUNT + 2)

struct MTL_RAY_REGENERATION_CREATE_DESC {
  UINT StructSize;
  UINT Version, Width, Height, Signals, CheckerboardSignals, Flags;
};
struct MTL_RAY_REGENERATION_SETTINGS {
  FLOAT NormalStrength, StabilityBias, MaxRadiance, RadianceClipStdK;
  FLOAT GaussianRelaxation, DisocclusionThreshold;
  FLOAT DebugDepthMin, DebugDepthMax;
  FLOAT HistoryRejectionStrength;
};
struct MTL_RAY_REGENERATION_FRAME_DESC {
  UINT StructSize;
  // Depth, motion, normals, specular albedo, diffuse albedo; input/output pairs
  // in AMD signal-bit order; legacy fused albedo; debug output.
  ID3D12Resource *Resources[MTL_RAY_REGENERATION_RESOURCE_COUNT];
  D3D12_RESOURCE_STATES States[MTL_RAY_REGENERATION_RESOURCE_COUNT];
  MTL_RAY_REGENERATION_SETTINGS Settings;
  FLOAT MotionScale[3], Jitter[2], CameraDelta[3];
  FLOAT View[16], Projection[16];
  FLOAT DepthMin, DepthMax;
  UINT Width, Height, FrameIndex, Flags;
  UINT CheckerboardOrigin[MTL_RAY_REGENERATION_SIGNAL_COUNT];
  FLOAT LightDirection[3], LightEmission[3], LightAngularRadius;
  UINT DebugWidth, DebugHeight, DebugMode, DebugViewport;
  UINT LegacyMode; // 0: 1.2 signals; 1: legacy split; 2: fused pair; 3: composite.
};

DEFINE_COM_INTERFACE("6f0d6f42-1c8e-4b2f-9d3e-2a7b5c1e9a30", IMTLD3D12RayRegenerationState) : public IUnknown {
  // Actual live native conversion/correction/history bytes; excludes opaque MetalFX model memory.
  virtual HRESULT STDMETHODCALLTYPE GetMemoryUsage(UINT64 *bytes) = 0;
};
DEFINE_COM_INTERFACE("6f0d6f42-1c8e-4b2f-9d3e-2a7b5c1e9a31", IMTLD3D12RayRegenerationDevice) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateRayRegenerationState(const MTL_RAY_REGENERATION_CREATE_DESC *desc,
      IMTLD3D12RayRegenerationState **state) = 0;
};
DEFINE_COM_INTERFACE("6f0d6f42-1c8e-4b2f-9d3e-2a7b5c1e9a32", IMTLD3D12RayRegenerationCommandList) : public IUnknown {
  // Validates and records a replayable native command; denoising executes at submission.
  virtual HRESULT STDMETHODCALLTYPE DenoiseRayRegeneration(IMTLD3D12RayRegenerationState *state,
      const MTL_RAY_REGENERATION_FRAME_DESC *desc) = 0;
};
