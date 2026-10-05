// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_reflex_interfaces.hpp"
#include "dxgi.h"
#include "winemetal.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace dxmt {

class ReflexState {
  struct Frame {
    DXMT_REFLEX_REPORT report{};
    UINT markers = 0;
    UINT pending = 0;
    bool valid = false, failed = false, presented = false;
  };
  std::mutex mutex_;
  std::condition_variable cv_;
  std::array<Frame, 128> frames_{};
  UINT64 latest_ = 0;
  bool have_frame_ = false, removed_ = false, low_latency_ = false, enabled_ = false;
  UINT interval_us_ = 0, sleep_us_ = 0, pending_ = 0;
  UINT64 sleep_generation_ = 0, present_generation_ = 0;
  bool game_sleep_ = false;
  std::chrono::steady_clock::time_point last_wake_{};

public:
  static UINT64 NowUs() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return UINT64(counter.QuadPart / frequency.QuadPart) * 1000000 +
           UINT64(counter.QuadPart % frequency.QuadPart) * 1000000 / UINT64(frequency.QuadPart);
  }

  struct Work {
    ReflexState *state = nullptr;
    UINT64 frame = 0;
    bool tracked = false, present = false;
    ~Work() {
      std::lock_guard lock(state->mutex_);
      --state->pending_;
      if (tracked) {
        auto &slot = state->frames_[frame % state->frames_.size()];
        if (slot.valid && slot.report.frameID == frame) --slot.pending;
      }
      state->cv_.notify_all();
    }
  };

  std::shared_ptr<Work> Capture(bool present) {
    std::lock_guard lock(mutex_);
    if (!enabled_) return {};
    auto work = std::make_shared<Work>();
    work->state = this; work->frame = latest_; work->tracked = have_frame_; work->present = present;
    ++pending_;
    if (have_frame_) ++frames_[latest_ % frames_.size()].pending;
    if (present) ++present_generation_;
    return work;
  }

  void Remove() {
    std::lock_guard lock(mutex_);
    removed_ = true;
    cv_.notify_all();
  }

  HRESULT SetMode(BOOL low, UINT interval, BOOL markers) {
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    low_latency_ = low; interval_us_ = interval;
    enabled_ = have_frame_ || low || interval;
    (void)markers; // Queue retirement is authoritative even without marker-based prediction.
    last_wake_ = {};
    cv_.notify_all();
    return S_OK;
  }

  HRESULT Status(BOOL *low, UINT *sleep, BOOL *game) {
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    *low = low_latency_; *sleep = sleep_us_;
    *game = game_sleep_ && present_generation_ - sleep_generation_ < 3;
    return S_OK;
  }

  HRESULT Sleep(bool fallback = false, bool do_not_wait = false) {
    std::unique_lock lock(mutex_);
    if (fallback && !enabled_) return S_OK;
    if (fallback && game_sleep_ && present_generation_ - sleep_generation_ < 3) return S_OK;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
      const auto now = std::chrono::steady_clock::now();
      const auto deadline = last_wake_ + std::chrono::microseconds(interval_us_);
      // ponytail: completion-gated admission bounds queued frames; predictive overlap needs a native deadline model.
      const bool gpu_busy = low_latency_ && pending_ != 0;
      const bool limited = interval_us_ && last_wake_ != std::chrono::steady_clock::time_point{} && now < deadline;
      if (!gpu_busy && !limited) break;
      if (do_not_wait) return DXGI_ERROR_WAS_STILL_DRAWING;
      if (limited) cv_.wait_until(lock, deadline);
      else cv_.wait(lock);
    }
    last_wake_ = std::chrono::steady_clock::now();
    sleep_us_ = UINT(std::min<int64_t>(UINT32_MAX,
        std::chrono::duration_cast<std::chrono::microseconds>(last_wake_ - start).count()));
    if (!fallback) { game_sleep_ = true; sleep_generation_ = present_generation_; }
    return S_OK;
  }

  HRESULT Marker(UINT64 id, UINT marker) {
    if (marker > 8 || marker == 7) return E_NOTIMPL; // Flash rendering is not available.
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    if (marker == 0) {
      if (have_frame_ && id <= latest_) return E_INVALIDARG;
      auto &slot = frames_[id % frames_.size()];
      if (slot.pending) return DXGI_ERROR_WAS_STILL_DRAWING;
      slot = {}; slot.valid = true; slot.report.frameID = id;
      latest_ = id; have_frame_ = true; enabled_ = true;
    }
    if (!have_frame_) return E_INVALIDARG;
    auto &slot = frames_[id % frames_.size()];
    if (!slot.valid || slot.report.frameID != id || (slot.markers & (1u << marker))) return E_INVALIDARG;
    auto &r = slot.report;
    const auto now = NowUs();
    switch (marker) {
    case 0: r.simStartTime = now; break;
    case 1: if (!r.simStartTime) return E_INVALIDARG; r.simEndTime = now; break;
    case 2: if (!r.simEndTime) return E_INVALIDARG; r.renderSubmitStartTime = now; break;
    case 3: if (!r.renderSubmitStartTime) return E_INVALIDARG; r.renderSubmitEndTime = now; break;
    case 4: if (!r.renderSubmitEndTime) return E_INVALIDARG; r.presentStartTime = now; break;
    case 5: if (!r.presentStartTime) return E_INVALIDARG; r.presentEndTime = now; break;
    case 6: if (r.simEndTime) return E_INVALIDARG; r.inputSampleTime = now; break;
    case 8: break; // Ping has no field in the V1 frame report.
    }
    slot.markers |= 1u << marker;
    return S_OK;
  }

  // Native feedback timestamps are host-clock nanoseconds, not GPU counter ticks.
  // Bracket CACurrentMediaTime in Wine QPC space; never substitute CPU completion time for GPU time.
  void Complete(const std::shared_ptr<Work> &work, UINT64 driver_start, UINT64 driver_end,
                UINT64 queue_start, UINT64 gpu_start_ns, UINT64 gpu_end_ns, bool success) {
    if (!work || !work->tracked) return;
    UINT64 before = NowUs();
    double host = WMTClockSampleHostSeconds();
    UINT64 after = NowUs();
    const double offset = double(before + (after - before) / 2) - host * 1000000.0;
    const auto start = static_cast<int64_t>(double(gpu_start_ns) / 1000.0 + offset);
    const auto end = static_cast<int64_t>(double(gpu_end_ns) / 1000.0 + offset);
    std::lock_guard lock(mutex_);
    auto &slot = frames_[work->frame % frames_.size()];
    if (!slot.valid || slot.report.frameID != work->frame) return;
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

  HRESULT Reports(DXMT_REFLEX_REPORT *reports) {
    std::lock_guard lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    std::array<const Frame *, 128> complete{};
    size_t count = 0;
    for (const auto &slot : frames_)
      if (slot.valid && !slot.failed && !slot.pending && slot.presented &&
          (slot.markers & 0x3f) == 0x3f && slot.report.gpuRenderEndTime)
        complete[count++] = &slot;
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
} // namespace dxmt
