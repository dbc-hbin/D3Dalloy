// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned PE recording and validated-retirement lifecycle for the concrete phase backend.
#include "d3d12_frame_generation.hpp"
#include "d3d12_frame_generation_backend.hpp"
#include "d3d12_device.hpp"
#include "dxmt_queue_order.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include <condition_variable>
#include <map>
#include <mutex>
#include <new>

namespace dxmt {
namespace {
enum class FrameKind { Prepare, Generate, Publish };
struct FrameRecord {
  FrameKind kind = FrameKind::Prepare;
  UINT64 frame_id = 0, epoch = 0, queue = 0, submission = 0;
  UINT requested = 0, actual = 0;
  bool started = false, committed = false, completed = false, suppressed = false;
  bool retire_requested = false, retain_results = false;
  bool reset = false;
  HRESULT status = S_FALSE;
  std::shared_ptr<FrameRecord> producer;
  std::shared_ptr<OwnedPhaseInputs> inputs;
  std::shared_ptr<OwnedPhaseBatch> batch;
  std::shared_ptr<OwnedPhasePublication> publication;
};
struct Core {
  Com<MTLD3D12Device> device;
  std::shared_ptr<OwnedPhaseBackend> backend;
  std::shared_ptr<owned_graphics::FenceOrderEpoch> removal_order;
  std::mutex mutex, recording_mutex;
  std::condition_variable completion;
  std::map<UINT64, std::shared_ptr<FrameRecord>> preparations, generations;
  std::shared_ptr<FrameRecord> active;
  UINT64 epoch = 1, last_real = 0, recorded_real = 0;
  bool enabled = false, destroyed = false, have_history = false, have_recorded_real = false;
  bool reset_after_recording_failure = false;
  HRESULT failure = S_OK;
};
constexpr GUID kInternalStateId = {0xb2a31090,0xa1f2,0x4717,{0xbb,0x50,0x61,0x7a,0x47,0xf2,0xa0,0x13}};
struct IOwnedFrameGenerationStateInternal : IUnknown {
  virtual HRESULT Prepare(MTLD3D12Device *, const MTL_FRAME_GENERATION_PREPARE_DESC &,
      std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
  virtual HRESULT Generate(MTLD3D12Device *, const MTL_FRAME_GENERATION_GENERATE_DESC &,
      std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
  virtual HRESULT Publish(MTLD3D12Device *, const MTL_FRAME_GENERATION_OUTPUT_DESC &,
      std::shared_ptr<OwnedFrameGenerationFrame> &) = 0;
};
}
struct OwnedFrameGenerationFrame {
  std::shared_ptr<Core> core;
  std::shared_ptr<FrameRecord> record;
};
namespace {
class OwnedFrameGenerationState final : public ComObject<IMTLD3D12FrameGenerationState, IOwnedFrameGenerationStateInternal> {
  std::shared_ptr<Core> core_;
public:
  explicit OwnedFrameGenerationState(std::shared_ptr<Core> core) : core_(std::move(core)) {}
  ~OwnedFrameGenerationState() {
    {
      std::lock_guard lock(core_->mutex);
      core_->destroyed = true;
      core_->enabled = false;
      ++core_->epoch;
      core_->have_history = false;
      core_->preparations.clear();
      core_->generations.clear();
    }
    core_->completion.notify_all();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) final {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMTLD3D12FrameGenerationState))
      *object = static_cast<IMTLD3D12FrameGenerationState *>(this);
    else if (iid == kInternalStateId) *object = static_cast<IOwnedFrameGenerationStateInternal *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Configure(BOOL enabled) final {
    if (enabled != FALSE && enabled != TRUE) return E_INVALIDARG;
    {
      std::lock_guard lock(core_->mutex);
      if (core_->destroyed) return E_FAIL;
      if (core_->enabled != bool(enabled)) {
        // Epoch changes cancel packets not yet admitted by Begin, not already
        // submitted GPU writes. Retained old payloads finish before arena reuse.
        core_->enabled = bool(enabled);
        ++core_->epoch;
        core_->have_history = false;
        core_->have_recorded_real = false;
        core_->preparations.clear();
      }
    }
    core_->completion.notify_all();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RetireFrame(UINT64 frame_id, BOOL retain_results, UINT64 *oldest, BOOL *pending) final {
    if (!oldest || !pending) return E_POINTER;
    *oldest = 0; *pending = FALSE;
    if (retain_results != FALSE && retain_results != TRUE) return E_INVALIDARG;
    std::lock_guard lock(core_->mutex);
    // Payloads retain their producers independently of this metadata GC.
    core_->preparations.erase(core_->preparations.begin(), core_->preparations.upper_bound(frame_id));
    for (auto it = core_->generations.begin(); it != core_->generations.end();) {
      auto &record = *it->second;
      if (it->first <= frame_id) {
        record.retire_requested = true;
        record.retain_results = bool(retain_results);
      }
      if (record.retire_requested && record.completed && !record.retain_results)
        it = core_->generations.erase(it);
      else ++it;
    }
    if (!core_->preparations.empty()) {
      *oldest = core_->preparations.begin()->first;
      *pending = TRUE;
    }
    if (!core_->generations.empty() && (!*pending || core_->generations.begin()->first < *oldest)) {
      *oldest = core_->generations.begin()->first;
      *pending = TRUE;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFrameGenerationResult(UINT64 frame_id, MTL_FRAME_GENERATION_RESULT *result) final {
    if (!result) return E_POINTER;
    if (result->StructSize != sizeof(*result)) return E_INVALIDARG;
    *result = {sizeof(*result), frame_id, 0, {}};
    std::lock_guard lock(core_->mutex);
    const auto found = core_->generations.find(frame_id);
    if (found == core_->generations.end()) return S_FALSE;
    auto &record = *found->second;
    if (!record.completed) {
      if (FAILED(core_->failure)) return core_->failure;
      const auto removed = core_->device->RemovedReason();
      return FAILED(removed) ? removed : S_FALSE;
    }
    const auto status = record.status;
    if (SUCCEEDED(status) && record.epoch == core_->epoch && core_->enabled && !core_->destroyed) {
      result->generated_count = record.actual;
      for (UINT i = 0; i < record.actual; ++i)
        result->phases[i] = FLOAT(i + 1) / FLOAT(record.requested + 1);
    }
    return status;
  }
  HRESULT Prepare(MTLD3D12Device *device, const MTL_FRAME_GENERATION_PREPARE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr()) return E_INVALIDARG;
    auto record = std::make_shared<FrameRecord>();
    record->kind = FrameKind::Prepare;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>(OwnedFrameGenerationFrame{core_, record});
    record->frame_id = desc.frame_id;
    record->reset = desc.reset != 0;
    bool capture;
    {
      std::lock_guard lock(core_->mutex);
      record->epoch = core_->epoch;
      if (core_->destroyed) return E_FAIL;
      capture = core_->enabled;
    }
    const auto hr = core_->backend->CreateInputs(desc, record->inputs, capture);
    if (FAILED(hr)) return hr;
    {
      std::lock_guard lock(core_->mutex);
      const auto found = core_->preparations.find(desc.frame_id);
      if (found != core_->preparations.end() && found->second->epoch == record->epoch) return E_INVALIDARG;
      if (core_->preparations.size() >= 64 && found == core_->preparations.end()) return E_OUTOFMEMORY;
      record->suppressed = !capture || !core_->enabled || core_->destroyed || record->epoch != core_->epoch;
      core_->preparations.insert_or_assign(desc.frame_id, record);
    }
    result = std::move(frame);
    return S_OK;
  }
  HRESULT Generate(MTLD3D12Device *device, const MTL_FRAME_GENERATION_GENERATE_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr() || desc.num_generated_frames < 1 || desc.num_generated_frames > 4)
      return E_INVALIDARG;
    auto record = std::make_shared<FrameRecord>();
    record->kind = FrameKind::Generate;
    auto frame = std::make_shared<OwnedFrameGenerationFrame>(OwnedFrameGenerationFrame{core_, record});
    record->frame_id = desc.frame_id;
    record->requested = desc.num_generated_frames;
    record->reset = desc.reset != 0;
    {
      std::lock_guard lock(core_->mutex);
      if (core_->destroyed) return E_FAIL;
      record->epoch = core_->epoch;
      const auto found = core_->preparations.find(desc.frame_id);
      if (found != core_->preparations.end() && found->second->epoch == record->epoch) {
        record->producer = found->second;
        record->inputs = found->second->inputs;
        record->reset |= found->second->reset;
      }
      // An enabled request needs its exact current-epoch prepare. Missing input
      // is a caller error, not a legitimate retired zero-count/bootstrap batch.
      if (core_->enabled && !record->producer) return E_INVALIDARG;
      record->suppressed = !core_->enabled || !record->producer || record->producer->suppressed ||
          (core_->have_recorded_real && desc.frame_id <= core_->recorded_real);
    }
    const auto hr = core_->backend->CreateBatch(desc, record->suppressed ? nullptr : record->inputs, record->batch);
    if (FAILED(hr)) return hr;
    {
      std::lock_guard lock(core_->mutex);
      record->suppressed |= record->epoch != core_->epoch || !core_->enabled || core_->destroyed;
      const auto found = core_->generations.find(desc.frame_id);
      if (found == core_->generations.end() || found->second->epoch != record->epoch) {
        core_->generations.insert_or_assign(desc.frame_id, record);
      } else {
        // A repeated frame-ID cannot replace the genuine retained batch/outcome.
        record->suppressed = true;
      }
    }
    result = std::move(frame);
    return S_OK;
  }
  HRESULT Publish(MTLD3D12Device *device, const MTL_FRAME_GENERATION_OUTPUT_DESC &desc,
      std::shared_ptr<OwnedFrameGenerationFrame> &result) final {
    if (device != core_->device.ptr() || desc.output_index < 1 || desc.output_index > 4) return E_INVALIDARG;
    auto record = std::make_shared<FrameRecord>();
    record->kind = FrameKind::Publish;
    record->frame_id = desc.frame_id;
    {
      std::lock_guard lock(core_->mutex);
      const auto found = core_->generations.find(desc.frame_id);
      if (found == core_->generations.end() || desc.output_index > found->second->requested) return E_INVALIDARG;
      record->producer = found->second;
      record->epoch = core_->epoch;
    }
    const auto hr = core_->backend->CreatePublication(record->producer->batch, desc, record->publication);
    if (FAILED(hr)) return hr;
    result = std::make_shared<OwnedFrameGenerationFrame>(OwnedFrameGenerationFrame{core_, std::move(record)});
    return S_OK;
  }
};
}
HRESULT CreateOwnedFrameGenerationState(MTLD3D12Device *device, const MTL_FRAME_GENERATION_CREATE_DESC *desc,
    IMTLD3D12FrameGenerationState **state) {
  if (!state) return E_POINTER;
  *state = nullptr;
  if (!device || !desc || desc->StructSize != sizeof(*desc) || !desc->display_width || !desc->display_height ||
      !desc->max_render_width || !desc->max_render_height || desc->display_width > 16384 ||
      desc->display_height > 16384 || desc->max_render_width > 16384 || desc->max_render_height > 16384 ||
      (desc->flags & ~15u)) return E_INVALIDARG;
  try {
    auto core = std::make_shared<Core>();
    core->device = device;
    const auto hr = CreateOwnedPhaseBackend(device, *desc, core->backend);
    if (FAILED(hr)) return hr;
    // Reuse the device's removal wait registry, including unrelated queue faults.
    core->removal_order = std::make_shared<owned_graphics::FenceOrderEpoch>(0);
    if (!core->removal_order->addCompletionWaiter(UINT64_MAX, [weak = std::weak_ptr(core)] {
          if (auto retained = weak.lock()) {
            {
              std::lock_guard lock(retained->mutex);
              retained->failure = retained->device->RemovedReason();
            }
            retained->completion.notify_all();
          }
        }) || !device->RegisterFenceOrder(core->removal_order)) return E_OUTOFMEMORY;
    const auto removed = device->RemovedReason();
    if (FAILED(removed)) return removed;
    Com<OwnedFrameGenerationState> implementation = new OwnedFrameGenerationState(std::move(core));
    return implementation->QueryInterface(__uuidof(IMTLD3D12FrameGenerationState), reinterpret_cast<void **>(state));
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT PrepareOwnedFrameGenerationFrame(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_PREPARE_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Prepare(device, *desc, frame); }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT GenerateOwnedFrameGenerationFrame(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_GENERATE_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Generate(device, *desc, frame); }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT PrepareOwnedFrameGenerationOutput(MTLD3D12Device *device, IMTLD3D12FrameGenerationState *state,
    const MTL_FRAME_GENERATION_OUTPUT_DESC *desc, std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  frame.reset();
  if (!state || !desc || desc->StructSize != sizeof(*desc)) return E_INVALIDARG;
  Com<IOwnedFrameGenerationStateInternal> internal;
  if (FAILED(state->QueryInterface(kInternalStateId, reinterpret_cast<void **>(&internal)))) return E_INVALIDARG;
  try { return internal->Publish(device, *desc, frame); }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT RecordOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame,
    ID3D12GraphicsCommandList *list) {
  if (!frame || !list) return E_INVALIDARG;
  auto &core = *frame->core;
  auto &record = *frame->record;
  std::lock_guard recording_lock(core.recording_mutex);
  {
    std::lock_guard lock(core.mutex);
    record.suppressed |= record.epoch != core.epoch || !core.enabled || core.destroyed;
    if (record.kind == FrameKind::Generate && !record.suppressed) {
      if (core.have_recorded_real && record.frame_id <= core.recorded_real) record.suppressed = true;
    }
  }
  if (record.kind != FrameKind::Publish && record.suppressed) return S_OK;
  HRESULT status = E_INVALIDARG;
  try {
    switch (record.kind) {
    case FrameKind::Prepare: status = core.backend->RecordInputs(record.inputs, list); break;
    case FrameKind::Generate: status = core.backend->RecordBatch(record.batch, list); break;
    case FrameKind::Publish: status = core.backend->RecordPublication(record.publication, list); break;
    }
  } catch (const std::bad_alloc &) { status = E_OUTOFMEMORY; }
  if (FAILED(status)) {
    // A poisoned command list cannot produce this packet. Do not retain an
    // unreachable pending outcome or allow a dependent packet to wait for it.
    std::lock_guard lock(core.mutex);
    record.status = status;
    record.completed = true;
    const auto prepared = core.preparations.find(record.frame_id);
    if (prepared != core.preparations.end() && prepared->second == frame->record)
      core.preparations.erase(prepared);
    const auto generated = core.generations.find(record.frame_id);
    if (generated != core.generations.end() && generated->second == frame->record)
      core.generations.erase(generated);
    if (record.kind == FrameKind::Generate && record.epoch == core.epoch)
      core.reset_after_recording_failure = true;
  } else if (record.kind == FrameKind::Generate) {
    std::lock_guard lock(core.mutex);
    if (record.epoch == core.epoch) {
      core.recorded_real = record.frame_id;
      core.have_recorded_real = true;
    }
  }
  return status;
}
bool HasOwnedFrameGenerationFrameStarted(const std::shared_ptr<OwnedFrameGenerationFrame> &frame) {
  std::lock_guard lock(frame->core->mutex);
  return frame->record->started;
}
bool BeginOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame,
    uint64_t queue, uint64_t preceding_submission) {
  auto &core = *frame->core;
  auto &record = *frame->record;
  std::unique_lock lock(core.mutex);
  const auto await_producer = [&](std::shared_ptr<FrameRecord> producer) {
    if (!producer || !producer->started) return false;
    // A recorded API packet is not a producer. Wait only an admitted recording
    // and require its real submission commit, then validated GPU retirement.
    core.completion.wait(lock, [&] { return producer->committed || producer->completed || FAILED(core.failure) ||
        FAILED(core.device->RemovedReason()); });
    core.completion.wait(lock, [&] { return producer->completed || FAILED(core.failure) ||
        FAILED(core.device->RemovedReason()); });
    return producer->completed && SUCCEEDED(producer->status);
  };
  for (;;) {
    if (record.started) {
      await_producer(frame->record);
      return false; // Replay never overwrites the earlier batch, history or outcome.
    }
    if (!core.active || !await_producer(core.active)) break;
  }
  bool valid = !record.suppressed && SUCCEEDED(core.failure) && SUCCEEDED(core.device->RemovedReason());
  if (record.kind != FrameKind::Prepare) valid &= await_producer(record.producer);
  valid &= !core.destroyed && core.enabled && record.epoch == core.epoch;
  if (record.kind == FrameKind::Generate && core.have_history && record.frame_id <= core.last_real) valid = false;
  // Admission linearizes with Configure under the Core lock. Subsequent epoch
  // changes discard this outcome/history, without racing CPU writes against
  // GPU-visible constants. Reset/publication patches below precede first submit.
  record.started = true;
  record.queue = queue;
  record.submission = preceding_submission;
  if (record.kind == FrameKind::Publish) {
    valid &= record.producer->epoch == core.epoch;
    record.publication->SetValid(valid);
    // Even invalid publication must execute its GPU suppression-flag writer.
    core.active = frame->record;
    return true;
  }
  record.suppressed |= !valid;
  if (!valid) return false;
  if (record.kind == FrameKind::Generate && (record.reset || core.reset_after_recording_failure || !core.have_history ||
      record.frame_id - core.last_real != 1)) {
    record.batch->ResetRecordedHistory();
    core.reset_after_recording_failure = false;
  }
  core.active = frame->record;
  return true;
}
void CommitOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame,
    uint64_t queue, uint64_t submission) {
  auto &core = *frame->core;
  {
    std::lock_guard lock(core.mutex);
    auto &record = *frame->record;
    if (record.completed) return;
    record.committed = true;
    record.queue = queue;
    record.submission = submission;
  }
  core.completion.notify_all();
}
void CompleteOwnedFrameGenerationFrame(const std::shared_ptr<OwnedFrameGenerationFrame> &frame, HRESULT status) {
  auto &core = *frame->core;
  {
    std::lock_guard lock(core.mutex);
    auto &record = *frame->record;
    if (record.completed) return;
    const auto removed = core.device->RemovedReason();
    if (SUCCEEDED(status) && FAILED(removed)) status = removed;
    if (SUCCEEDED(status) && FAILED(core.failure)) status = core.failure;
    if (SUCCEEDED(status) && record.kind == FrameKind::Generate && !record.suppressed) {
      status = record.batch->GetActualGeneratedCount(record.actual);
      if (SUCCEEDED(status) && record.actual > record.requested) status = DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
    const bool current = core.enabled && !core.destroyed && record.epoch == core.epoch;
    if (!current || FAILED(status) || record.suppressed) record.actual = 0;
    record.status = status;
    record.completed = true;
    if (FAILED(status) && SUCCEEDED(core.failure)) core.failure = status;
    if (SUCCEEDED(status) && current && !record.suppressed && record.kind == FrameKind::Generate) {
      core.have_history = true;
      core.last_real = record.frame_id;
    }
    if (core.active == frame->record) core.active.reset();
    if (record.kind == FrameKind::Generate && record.retire_requested && !record.retain_results) {
      const auto found = core.generations.find(record.frame_id);
      if (found != core.generations.end() && found->second == frame->record)
        core.generations.erase(found);
    }
  }
  core.completion.notify_all();
  if (FAILED(status)) core.device->ReportRemoval(status);
}
} // namespace dxmt
