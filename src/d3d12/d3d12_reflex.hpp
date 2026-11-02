// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_reflex_interfaces.hpp"
#include "owned_low_latency.hpp"
#include "owned_trace.hpp"
#include "dxgi.h"
#include "winemetal.h"
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace d3dalloy {

class ReflexState {
  using Scheduler = owned_graphics::LowLatencyScheduler;
  struct Frame {
    D3DALLOY_REFLEX_REPORT report{};
    std::shared_ptr<Scheduler::Frame> admission;
    UINT markers = 0, pending = 0;
    bool failed = false, presented = false;
  };
  std::mutex mutex_;
  std::array<std::shared_ptr<Frame>, 128> frames_{};
  std::unordered_map<uint64_t, std::weak_ptr<Frame>> admission_reports_;
  std::unordered_map<std::thread::id, std::weak_ptr<Frame>> render_bindings_;
  std::unordered_map<std::thread::id, std::weak_ptr<Frame>> present_bindings_;
  struct PresentHint { bool interpolated; };
  std::unordered_map<std::thread::id, PresentHint> present_hints_;
  UINT64 latest_ = 0;
  bool have_frame_ = false, removed_ = false;
  const uint64_t owned_id_ = owned_graphics::NewObjectId();
  Scheduler scheduler_;

  static void Observe(void *context, const Scheduler::Event &event) {
    const char *name = "low_latency_mode";
    switch (event.kind) {
    case Scheduler::EventKind::Mode: break;
    case Scheduler::EventKind::Wait: name = "low_latency_wait"; break;
    case Scheduler::EventKind::Admit: name = "low_latency_admit"; break;
    case Scheduler::EventKind::Close: name = "low_latency_close"; break;
    case Scheduler::EventKind::Complete: name = "low_latency_complete"; break;
    }
    owned_graphics::Trace(name, ",\"scheduler\":", static_cast<ReflexState *>(context)->owned_id_,
        ",\"frame\":", event.frame, ",\"source\":", uint32_t(event.source), ",\"now_ns\":", event.now,
        ",\"target_ns\":", event.target, ",\"pending_frames\":", event.pending_frames,
        ",\"capacity\":", unsigned(event.capacity), ",\"success\":", unsigned(event.success));
  }
  std::shared_ptr<Scheduler::Frame> TakePresentBinding() {
    auto binding = present_bindings_.find(std::this_thread::get_id());
    if (binding == present_bindings_.end()) return {};
    auto report = binding->second.lock();
    present_bindings_.erase(binding);
    return report ? report->admission : std::shared_ptr<Scheduler::Frame>{};
  }
public:
  using PresentReservation = Scheduler::PresentReservation;
  explicit ReflexState(bool force = false) : scheduler_(force, Observe, this) {}
  static HRESULT Result(Scheduler::Result result) {
    switch (result) {
    case Scheduler::Result::Ready: return S_OK;
    case Scheduler::Result::WouldBlock: return DXGI_ERROR_WAS_STILL_DRAWING;
    case Scheduler::Result::Removed: return DXGI_ERROR_DEVICE_REMOVED;
    case Scheduler::Result::Failed: return E_FAIL;
    }
    return E_FAIL;
  }
  static UINT64 NowUs() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return UINT64(counter.QuadPart / frequency.QuadPart) * 1000000 +
           UINT64(counter.QuadPart % frequency.QuadPart) * 1000000 / UINT64(frequency.QuadPart);
  }

  struct Work {
    ReflexState *state = nullptr;
    std::shared_ptr<Frame> report;
    std::shared_ptr<Scheduler::Work> admission;
    bool present = false, completed = false;
    ~Work() {
      if (report) {
        std::lock_guard lock(state->mutex_);
        report->failed |= !completed;
        --report->pending;
      }
    }
  };
  struct PresentCapture {
    std::shared_ptr<Work> work;
    std::shared_ptr<Scheduler::Frame> frame;
  };

  std::shared_ptr<Work> Capture(bool present, bool closes_frame = false) {
    std::lock_guard lock(mutex_);
    auto identified = present ? TakePresentBinding() : std::shared_ptr<Scheduler::Frame>{};
    auto admission = scheduler_.Capture(present, have_frame_, std::move(identified));
    if (!admission && !have_frame_) return {};
    auto work = std::make_shared<Work>();
    work->state = this;
    work->admission = std::move(admission);
    work->present = closes_frame;
    if (have_frame_) {
      // Presentation may run several simulation frames behind the marker thread.
      // Match its FIFO admission token, never the globally newest marker ID.
      if (work->admission) {
        auto report = admission_reports_.find(work->admission->frame->id);
        if (report != admission_reports_.end()) work->report = report->second.lock();
      }
      if (!present && !work->report) {
        auto binding = render_bindings_.find(std::this_thread::get_id());
        if (binding != render_bindings_.end()) work->report = binding->second.lock();
      }
      if (work->report) ++work->report->pending;
    }
    // Selection and closure must share the reporting lock. Another queue may
    // prepare concurrently, but cannot capture this real Present's open frame.
    if (closes_frame) scheduler_.Close(work->admission);
    return work;
  }
  HRESULT ReservePresent(bool do_not_wait, PresentReservation &reservation) {
    return Result(scheduler_.ReservePresent(do_not_wait, reservation));
  }
  PresentCapture CapturePresent(bool closes_frame) {
    // The caller retains its capacity reservation through queue publication.
    auto work = Capture(true, closes_frame);
    auto frame = closes_frame && work && work->admission
        ? work->admission->frame : std::shared_ptr<Scheduler::Frame>{};
    return {std::move(work), std::move(frame)};
  }
  HRESULT PresentReturn(const std::shared_ptr<Scheduler::Frame> &frame, bool do_not_wait) {
    return Result(scheduler_.PresentReturn(frame, do_not_wait));
  }
  void SkipPresent() {
    std::lock_guard state_lock(mutex_);
    scheduler_.SkipPresent(TakePresentBinding());
  }
  void Remove() {
    { std::lock_guard lock(mutex_); removed_ = true; }
    scheduler_.Remove();
  }
  HRESULT SetMode(BOOL low, UINT interval, BOOL markers) {
    (void)markers;
    return Result(scheduler_.SetMode(Scheduler::Source::Reflex, low != FALSE, interval));
  }
  HRESULT SetAntiLagMode(BOOL low, UINT interval) {
    return Result(scheduler_.SetMode(Scheduler::Source::AntiLag, low != FALSE, interval));
  }
  HRESULT Status(BOOL *low, UINT *sleep, BOOL *game) {
    bool current_low = false, current_game = false;
    uint32_t current_sleep = 0;
    auto hr = Result(scheduler_.Status(current_low, current_sleep, current_game));
    if (SUCCEEDED(hr)) { *low = current_low; *sleep = current_sleep; *game = current_game; }
    return hr;
  }
  HRESULT Sleep() { return Result(scheduler_.Sleep()); }
  HRESULT AntiLagSleep() { return Result(scheduler_.Sleep(Scheduler::Source::AntiLag)); }
  HRESULT EndFrame() { return Result(scheduler_.EndFrame()); }
  HRESULT FrameType(BOOL interpolated) {
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    present_hints_.insert_or_assign(std::this_thread::get_id(), PresentHint{interpolated != FALSE});
    return S_OK;
  }
  // Consume once even for TEST/rejected Present. Explicit internal FG membership wins;
  // a hint cannot leak from an internal generated member into an unrelated chain.
  bool PresentClosesFrame(bool closes_frame, bool explicit_group) {
    std::lock_guard lock(mutex_);
    auto hint = present_hints_.find(std::this_thread::get_id());
    if (hint == present_hints_.end()) return closes_frame;
    const bool generated = hint->second.interpolated;
    present_hints_.erase(hint);
    return explicit_group ? closes_frame : closes_frame && !generated;
  }

  HRESULT Marker(UINT64 id, UINT marker) {
    if (marker > 12 || marker == 7) return E_NOTIMPL;
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    if (marker == 0) {
      if (have_frame_ && id <= latest_) return E_INVALIDARG;
      auto &slot = frames_[id % frames_.size()];
      if (slot && slot->pending) return DXGI_ERROR_WAS_STILL_DRAWING;
      auto frame = std::make_shared<Frame>();
      frame->report.frameID = id;
      frame->admission = scheduler_.BeginSimulation();
      if (slot) admission_reports_.erase(slot->admission->id);
      admission_reports_.insert_or_assign(frame->admission->id, frame);
      slot = std::move(frame);
      latest_ = id;
      have_frame_ = true;
    }
    if (!have_frame_) return E_INVALIDARG;
    auto &frame = frames_[id % frames_.size()];
    if (!frame || frame->report.frameID != id || (frame->markers & (1u << marker))) return E_INVALIDARG;
    auto &r = frame->report;
    const auto now = NowUs();
    switch (marker) {
    case 0: r.simStartTime = now; break;
    case 1: if (!r.simStartTime) return E_INVALIDARG; r.simEndTime = now; break;
    case 2:
      if (!r.simEndTime) return E_INVALIDARG;
      r.renderSubmitStartTime = now;
      scheduler_.Bind(frame->admission);
      render_bindings_[std::this_thread::get_id()] = frame;
      break;
    case 3:
      if (!r.renderSubmitStartTime) return E_INVALIDARG;
      r.renderSubmitEndTime = now;
      scheduler_.EndFrame(frame->admission);
      break;
    case 4:
      if (!r.renderSubmitEndTime) return E_INVALIDARG;
      r.presentStartTime = now;
      present_bindings_[std::this_thread::get_id()] = frame;
      break;
    case 5:
      if (!r.presentStartTime) return E_INVALIDARG;
      r.presentEndTime = now;
      present_bindings_.erase(std::this_thread::get_id());
      break;
    case 6: if (r.simEndTime) return E_INVALIDARG; r.inputSampleTime = now; break;
    case 8: break;
    // Out-of-band render/present markers belong to this existing application frame.
    // They have no V1 report fields and must not create a new simulation admission.
    case 9:
      scheduler_.Bind(frame->admission, true);
      render_bindings_[std::this_thread::get_id()] = frame;
      break;
    case 10: if (!(frame->markers & (1u << 9))) return E_INVALIDARG; break;
    case 11:
      present_bindings_[std::this_thread::get_id()] = frame;
      present_hints_.insert_or_assign(std::this_thread::get_id(), PresentHint{true});
      break;
    case 12:
      if (!(frame->markers & (1u << 11))) return E_INVALIDARG;
      present_bindings_.erase(std::this_thread::get_id());
      present_hints_.erase(std::this_thread::get_id());
      break;
    }
    frame->markers |= 1u << marker;
    return S_OK;
  }

  // Native feedback timestamps are host-clock nanoseconds, not GPU counter ticks.
  // Bracket CACurrentMediaTime in Wine QPC space; never substitute CPU completion time for GPU time.
  void Submitted(const std::shared_ptr<Work> &work, uint64_t timestamp) {
    if (work) scheduler_.Submitted(work->admission, timestamp);
  }
  void Complete(const std::shared_ptr<Work> &work, UINT64 driver_start, UINT64 driver_end,
                UINT64 queue_start, UINT64 gpu_start_ns, UINT64 gpu_end_ns, bool success) {
    if (!work) return;
    const auto before_scheduler = Scheduler::Now();
    UINT64 before = NowUs();
    double host = WMTClockSampleHostSeconds();
    UINT64 after = NowUs();
    const auto after_scheduler = Scheduler::Now();
    const auto scheduler_offset = int64_t(before_scheduler + (after_scheduler - before_scheduler) / 2) -
                                  int64_t(host * 1000000000.0);
    const auto scheduler_start = int64_t(gpu_start_ns) + scheduler_offset;
    const auto scheduler_end = int64_t(gpu_end_ns) + scheduler_offset;
    const bool timing_valid = host > 0 && gpu_start_ns && gpu_end_ns >= gpu_start_ns && scheduler_start > 0;
    scheduler_.Complete(work->admission, success, timing_valid ? uint64_t(scheduler_end) : 0,
                        timing_valid ? uint64_t(scheduler_start) : 0);
    if (!work->report) return;
    const double offset = double(before + (after - before) / 2) - host * 1000000.0;
    const auto start = static_cast<int64_t>(double(gpu_start_ns) / 1000.0 + offset);
    const auto end = static_cast<int64_t>(double(gpu_end_ns) / 1000.0 + offset);
    std::lock_guard lock(mutex_);
    work->completed = true;
    auto &slot = *work->report;
    if (!success || !gpu_start_ns || gpu_end_ns < gpu_start_ns || !host || start <= 0 || end < start) {
      slot.failed = true; return;
    }
    auto &r = slot.report;
    if (!r.driverStartTime || driver_start < r.driverStartTime) r.driverStartTime = driver_start;
    r.driverEndTime = std::max(r.driverEndTime, driver_end);
    if (!r.osRenderQueueStartTime || queue_start < r.osRenderQueueStartTime) r.osRenderQueueStartTime = queue_start;
    r.osRenderQueueEndTime = std::max(r.osRenderQueueEndTime, UINT64(start));
    if (!r.gpuRenderStartTime || UINT64(start) < r.gpuRenderStartTime) r.gpuRenderStartTime = UINT64(start);
    r.gpuRenderEndTime = std::max(r.gpuRenderEndTime, UINT64(end));
    slot.presented |= work->present;
    // Metal feedback exposes the span, not active cycles excluding GPU idle time.
    // gpuActiveRenderTimeUs remains unavailable (0); no invented utilization/scanout time.
  }

  HRESULT Reports(D3DALLOY_REFLEX_REPORT *reports) {
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    std::array<const Frame *, 128> complete{};
    size_t count = 0;
    for (const auto &slot : frames_)
      if (slot && !slot->failed && !slot->pending && slot->presented &&
          (slot->markers & 0x3f) == 0x3f && slot->report.gpuRenderEndTime)
        complete[count++] = slot.get();
    if (count < 64) return S_FALSE;
    std::sort(complete.begin(), complete.begin() + count, [](auto a, auto b) {
      return a->report.frameID < b->report.frameID;
    });
    for (size_t i = 0; i < 64; ++i) {
      const size_t index = count - 64 + i;
      reports[i] = complete[index]->report;
      if (index && reports[i].gpuRenderEndTime >= complete[index - 1]->report.gpuRenderEndTime)
        reports[i].gpuFrameTimeUs = UINT(std::min<UINT64>(UINT32_MAX,
            reports[i].gpuRenderEndTime - complete[index - 1]->report.gpuRenderEndTime));
    }
    return S_OK;
  }
};
} // namespace d3dalloy
