// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_frame_generation_interfaces.hpp"
#include "com/com_pointer.hpp"
#include <array>
#include <memory>
namespace dxmt {
struct OwnedPhaseRecordingStorage;
struct OwnedPhaseInputs {
  MTL_FRAME_GENERATION_PREPARE_DESC desc{};
  Com<ID3D12Resource> source_depth, source_motion, depth, motion;
};
struct OwnedPhaseBatch {
  MTL_FRAME_GENERATION_GENERATE_DESC desc{};
  std::shared_ptr<OwnedPhaseInputs> inputs;
  std::array<Com<ID3D12Resource>, 4> outputs;
  Com<ID3D12Resource> source_present, source_hudless, present, hudless;
  Com<ID3D12Resource> gpu_count, readback_count;
  std::shared_ptr<OwnedPhaseRecordingStorage> recording;
  HRESULT GetActualGeneratedCount(UINT &) const;
  void ResetRecordedHistory();
};
struct OwnedPhasePublication {
  MTL_FRAME_GENERATION_OUTPUT_DESC desc{};
  std::shared_ptr<OwnedPhaseBatch> batch;
  Com<ID3D12Resource> output, disable, control;
  Com<ID3D12DescriptorHeap> descriptors;
  UINT *mapped_control = nullptr;
  ~OwnedPhasePublication();
  void SetValid(bool);
};
class OwnedPhaseBackend {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  OwnedPhaseBackend();
  ~OwnedPhaseBackend();
  HRESULT Initialize(ID3D12Device *, const MTL_FRAME_GENERATION_CREATE_DESC &);
  HRESULT CreateInputs(const MTL_FRAME_GENERATION_PREPARE_DESC &, std::shared_ptr<OwnedPhaseInputs> &, bool capture = true);
  HRESULT RecordInputs(const std::shared_ptr<OwnedPhaseInputs> &, ID3D12GraphicsCommandList *);
  HRESULT CreateBatch(const MTL_FRAME_GENERATION_GENERATE_DESC &, const std::shared_ptr<OwnedPhaseInputs> &,
      std::shared_ptr<OwnedPhaseBatch> &);
  HRESULT RecordBatch(const std::shared_ptr<OwnedPhaseBatch> &, ID3D12GraphicsCommandList *);
  HRESULT CreatePublication(const std::shared_ptr<OwnedPhaseBatch> &, const MTL_FRAME_GENERATION_OUTPUT_DESC &,
      std::shared_ptr<OwnedPhasePublication> &);
  HRESULT RecordPublication(const std::shared_ptr<OwnedPhasePublication> &, ID3D12GraphicsCommandList *);
};
HRESULT CreateOwnedPhaseBackend(ID3D12Device *, const MTL_FRAME_GENERATION_CREATE_DESC &,
    std::shared_ptr<OwnedPhaseBackend> &);
} // namespace dxmt
