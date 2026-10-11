// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

// Versioned, project-owned boundary between nvngx.dll and d3d12.dll. No Metal,
// D3DMetal, native object pointers, or borrowed implementation layouts cross it.
struct MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC {
  UINT StructSize;
  UINT InputWidth, InputHeight;
  UINT OutputWidth, OutputHeight;
  UINT FeatureFlags; // HDR1, low-res MV2, jittered MV4, reversed depth8, auto exposure64, alpha128, RR256.
  BOOL OutputSubrectsEnabled;
  UINT CreationNodeMask, VisibilityNodeMask;
  BOOL DynamicOutputEnabled;
};

struct MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC {
  UINT StructSize;
  ID3D12Resource *Color, *Output, *Depth, *MotionVector;
  // RR responsivity uses BiasCurrentColorMask: a scalar [0,1] history-rejection mask.
  ID3D12Resource *ExposureTexture, *BiasCurrentColorMask, *TransparencyMask;
  UINT InputContentWidth, InputContentHeight;
  UINT ColorBaseX, ColorBaseY, DepthBaseX, DepthBaseY;
  UINT MotionBaseX, MotionBaseY, BiasBaseX, BiasBaseY;
  UINT OutputBaseX, OutputBaseY;
  FLOAT JitterOffsetX, JitterOffsetY;
  FLOAT MotionVectorScaleX, MotionVectorScaleY;
  FLOAT PreExposure;
  BOOL ResetHistory;
  UINT OutputWidth, OutputHeight; // Zero selects the create-time output extent.
  UINT MotionContentWidth, MotionContentHeight; // Zero derives the flagged MV extent.
  UINT CompositionBaseX, CompositionBaseY;
  UINT ColorTransfer; // 0: linear, 1: sRGB, 2: PQ.
  BOOL CombineCompositionMask, EnableSharpening;
  FLOAT Sharpness;
  BOOL CapOutputToTemporalMaxScale; // FSR centered opaque-black-margin policy.
  BOOL DrawDebugView; // SR color / depth / pixel-motion diagnostic panels.
  ID3D12Resource *DiffuseAlbedo, *SpecularAlbedo, *Normals, *Roughness, *SpecularHitDistance;
  FLOAT WorldToViewMatrix[16], ViewToClipMatrix[16];
  UINT RoughnessPacked, DepthLinear;
  UINT DiffuseAlbedoBaseX, DiffuseAlbedoBaseY, SpecularAlbedoBaseX, SpecularAlbedoBaseY;
  UINT NormalsBaseX, NormalsBaseY, RoughnessBaseX, RoughnessBaseY;
  UINT SpecularHitDistanceBaseX, SpecularHitDistanceBaseY;
};

DEFINE_COM_INTERFACE("c8a9134e-f27c-4e75-909f-f14439b918e1", IMTLD3D12TemporalUpscaleState)
    : public IUnknown {};

DEFINE_COM_INTERFACE("ed85c4e9-e8fd-491b-9595-d9a369ec12b3", IMTLD3D12DeviceExt)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SupportsTemporalUpscale(BOOL *supported) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateTemporalUpscaleState(
      const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC *desc,
      IMTLD3D12TemporalUpscaleState **state) = 0;
  virtual HRESULT STDMETHODCALLTYPE SupportsTemporalFeature(UINT feature_flags, BOOL *supported) = 0;
};

DEFINE_COM_INTERFACE("d7bc6ed0-6a8a-4a0e-8408-5a26c0b06d0f", IMTLD3D12GraphicsCommandListExt)
    : public IUnknown {
  // Validate and retain a replayable command while the list is open. Actual
  // scaler execution, history reset, and fence sequencing happen at submission.
  virtual HRESULT STDMETHODCALLTYPE TemporalUpscale(
      IMTLD3D12TemporalUpscaleState *state,
      const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *desc) = 0;
  // No side effects: lets NGX CreateFeature validate the required open list.
  virtual HRESULT STDMETHODCALLTYPE ValidateRecording() = 0;
};
