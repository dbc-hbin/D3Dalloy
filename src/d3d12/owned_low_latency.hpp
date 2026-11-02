// SPDX-License-Identifier: Apache-2.0
// The predictive controller is adapted from LatencyFleX, Copyright 2021 Tatsuyuki Ishi.
// https://github.com/ishitatsuyuki/LatencyFleX/blob/17c061c32594f24be225e69a2aeadf16f959a6fb/latencyflex.h
// Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0).
// Distributed on an AS IS BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
// Avoid winternl.h's NT object types, also declared by util_d3dkmt.h.
extern "C" LONG NTAPI NtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
#endif

namespace owned_graphics {

// One monotonic clock, nanoseconds, externally synchronized. GPU feedback is calibrated
// into this clock; CPU retirement still controls ownership. Signed projections cannot wrap.
class LowLatencyController {
public:
  struct Average {
    double alpha, value = 0, weight = 0;
    void Update(double sample) {
      value = (1 - alpha) * value + alpha * sample;
      weight = (1 - alpha) * weight + alpha;
    }
    double Get() const { return weight ? value / weight : 0; }
  };
private:
  static constexpr uint64_t none = UINT64_MAX;
  struct Slot { uint64_t id = none; int64_t begin = 0, projected = 0, compensation = 0; };
  std::array<Slot, 16> slots_{};
  uint64_t previous_begin_ = 0, previous_end_ = none;
  int64_t previous_end_time_ = 0, base_ = 0, previous_error_ = 0;
  Average latency_{0.3}, throughput_{0.3}, correction_{0.5, 0, 1};
public:
  uint64_t Target(uint64_t frame) {
    if (previous_end_ == none) return 0;
    auto &slot = slots_[frame % slots_.size()];
    int64_t compensation = 0;
    if (!base_) base_ = previous_end_time_;
    else {
      auto &end = slots_[previous_end_ % slots_.size()];
      const auto error = previous_end_time_ - base_ - end.projected;
      correction_.Update(double(std::max<int64_t>(0, error) -
                               std::max<int64_t>(0, previous_error_ - end.compensation)));
      previous_error_ = error;
      compensation = int64_t(std::llround(correction_.Get()));
    }
    slot.compensation = compensation;
    const double elapsed = double(frame - previous_begin_);
    const double up = frame % 2 == 0 ? 1.0 / 1.10 : 1.0;
    const auto previous = slots_[previous_begin_ % slots_.size()].projected;
    const auto target = base_ + previous + compensation + int64_t(std::llround(
        (elapsed + up - 1) * throughput_.Get() / 0.985 - latency_.Get()));
    slot.projected = previous + compensation + int64_t(std::llround(elapsed * throughput_.Get() / 0.985));
    return uint64_t(std::max<int64_t>(0, target));
  }
  void Begin(uint64_t frame, uint64_t target, uint64_t timestamp) {
    auto &slot = slots_[frame % slots_.size()];
    slot.id = frame;
    slot.begin = int64_t(timestamp);
    previous_begin_ = frame;
    if (target) {
      const auto forced = int64_t(timestamp) - int64_t(target);
      slot.projected += forced;
      slot.compensation += forced;
      previous_error_ += forced;
    }
  }
  void End(uint64_t frame, uint64_t timestamp) {
    auto &slot = slots_[frame % slots_.size()];
    if (slot.id != frame) return;
    slot.id = none;
    // Independent queues can finish later application frames first. Retirement
    // order must not turn that GPU timestamp into a negative throughput sample.
    if (previous_end_ != none && int64_t(timestamp) < previous_end_time_) return;
    if (frame % 2) latency_.Update(double(int64_t(timestamp) - slot.begin));
    if (previous_end_ != none && frame > previous_end_) {
      const auto duration = (int64_t(timestamp) - previous_end_time_) / int64_t(frame - previous_end_);
      if (frame % 2 == 0) throughput_.Update(double(std::clamp<int64_t>(duration, 1000000, 50000000)));
    }
    previous_end_ = frame;
    previous_end_time_ = int64_t(timestamp);
  }
};

class LowLatencyScheduler {
public:
  enum class Source : uint32_t { Forced, Reflex, AntiLag };
  enum class Result { Ready, WouldBlock, Removed, Failed };
  enum class EventKind { Mode, Wait, Admit, Close, Complete };
  struct Event {
    EventKind kind;
    Source source;
    uint64_t frame, now, target;
    size_t pending_frames;
    bool capacity, success;
  };
  using Observer = void (*)(void *, const Event &);
  struct Frame {
    uint64_t id = 0, begin = 0, closed_at = 0, completed_at = 0, gpu_completed_at = 0;
    uint64_t cpu_ended_at = 0, submitted_at = 0, first_submitted_at = 0, gpu_started_at = 0;
    size_t pending = 0;
    uint32_t sleep_sources = 0;
    bool begun = false, admitted = false, cooperative = false, closed = false, failed = false, cpu_hint = false;
    bool render_ended = false;
    bool simulation_marked = false;
    bool timing_valid = true;
  };
  // One token per API task, shared by every native fragment of that task. Its last release
  // is authoritative: all fragments have retired, or the unsubmitted task was canceled.
  struct Work {
    LowLatencyScheduler *owner;
    std::shared_ptr<Frame> frame;
    bool completed = false;
    bool present = false;
    ~Work() { owner->Release(*this); }
  };
  class PresentReservation {
    friend class LowLatencyScheduler;
    LowLatencyScheduler *owner_ = nullptr;
  public:
    PresentReservation() = default;
    PresentReservation(const PresentReservation &) = delete;
    PresentReservation &operator=(const PresentReservation &) = delete;
    ~PresentReservation() { if (owner_) owner_->ReleasePresent(); }
  };
private:
  using Clock = std::chrono::steady_clock;
  std::mutex mutex_;
  size_t preparing_presents_ = 0;
  std::condition_variable cv_;
#ifdef _WIN32
  struct TimedWaiter {
    HANDLE event;
    TimedWaiter *next;
  };
  TimedWaiter *timed_waiters_ = nullptr;
#endif
  LowLatencyController controller_;
  std::shared_ptr<Frame> open_;
  std::deque<std::shared_ptr<Frame>> frames_;
  struct Binding { std::weak_ptr<Frame> frame; bool out_of_band = false; };
  std::unordered_map<std::thread::id, Binding> bindings_;
  uint64_t sequence_ = 1, last_wake_ = 0, cpu_ns_ = 0, close_to_complete_ = 0;
  LowLatencyController::Average cpu_time_{0.3}, submit_time_{0.3}, completion_time_{0.3};
  LowLatencyController::Average wake_delay_{0.3};
  uint64_t previous_gpu_end_ = 0;
  double wake_advance_ = 0;
  uint64_t last_sleep_frame_ = 0;
  uint32_t sleep_us_ = 0, interval_sources_ = 0;
  bool force_, removed_ = false;
  std::array<bool, 2> low_{};
  std::array<uint32_t, 2> interval_{};
  Observer observer_;
  void *context_;

  // Called with mutex_ held. Each timed waiter owns its event, so a later waiter
  // cannot consume a wake intended for one that has not entered the kernel yet.
  void Notify() {
    cv_.notify_all();
#ifdef _WIN32
    for (auto *waiter = timed_waiters_; waiter; waiter = waiter->next)
      SetEvent(waiter->event);
#endif
  }
  bool WaitUntil(std::unique_lock<std::mutex> &lock, uint64_t deadline) {
    const auto started = Now();
    if (deadline <= started) return true;
    bool timed_out;
#ifdef _WIN32
    // libc++'s Win32 condition-variable timeout is millisecond based. NT relative
    // timeouts retain 100ns precision without spinning or changing global timers.
    TimedWaiter waiter{CreateEventW(nullptr, FALSE, FALSE, nullptr), timed_waiters_};
    if (!waiter.event) return false;
    const auto now = Now();
    if (deadline <= now) {
      CloseHandle(waiter.event);
      return true;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -static_cast<LONGLONG>((deadline - now + 99) / 100);
    timed_waiters_ = &waiter;
    lock.unlock();
    const auto status = NtWaitForSingleObject(waiter.event, FALSE, &timeout);
    lock.lock();
    auto **entry = &timed_waiters_;
    while (*entry != &waiter) entry = &(*entry)->next;
    *entry = waiter.next;
    CloseHandle(waiter.event);
    if (status < 0) return false;
    timed_out = status == STATUS_TIMEOUT;
#else
    timed_out = cv_.wait_until(lock, Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds(deadline)))) == std::cv_status::timeout;
#endif
    if (timed_out) {
      const auto resumed = Now();
      // Include kernel wake and lock reacquisition, but do not learn an
      // unbounded advance from suspension/debugger pauses.
      wake_delay_.Update(double(resumed > deadline ? std::min(resumed - deadline, deadline - started) : 0));
    }
    return true;
  }
  bool Active() const { return force_ || low_[0] || low_[1] || interval_[0] || interval_[1]; }
  size_t PendingFrames() const {
    return std::count_if(frames_.begin(), frames_.end(), [](const auto &frame) { return frame->begun; });
  }
  void Emit(EventKind kind, Source source, uint64_t frame, uint64_t now, uint64_t target = 0, bool cap = false,
            bool success = true) {
    if (observer_) observer_(context_, {kind, source, frame, now, target, PendingFrames(), cap, success});
  }
  std::shared_ptr<Frame> Open() {
    if (!open_) {
      auto frame = std::make_shared<Frame>();
      frame->id = sequence_++;
      frames_.push_back(frame);
      open_ = std::move(frame);
    }
    return open_;
  }
  std::shared_ptr<Frame> Rendering() {
    auto binding = bindings_.find(std::this_thread::get_id());
    if (binding != bindings_.end())
      if (auto frame = binding->second.frame.lock();
          frame && !frame->closed && (!frame->render_ended || binding->second.out_of_band))
        return frame;
    for (const auto &frame : frames_)
      if (!frame->closed && !frame->render_ended) return frame;
    if (open_ && open_->render_ended) open_.reset();
    return Open();
  }
  void Begin(Frame &frame, uint64_t target, uint64_t now) {
    if (frame.begun) return;
    frame.begin = now;
    frame.begun = true;
    controller_.Begin(frame.id, target, now);
  }
  bool Retire() {
    bool retired = false;
    while (!frames_.empty() && frames_.front()->closed && !frames_.front()->pending) {
      const auto &frame = *frames_.front();
      if (!frame.failed && frame.timing_valid && frame.gpu_completed_at >= frame.closed_at) {
        controller_.End(frame.id, frame.gpu_completed_at);
        const auto cpu = frame.cpu_ended_at - frame.begin;
        const auto tail = frame.submitted_at > frame.cpu_ended_at ? frame.submitted_at - frame.cpu_ended_at : 0;
        cpu_time_.Update(double(cpu));
        submit_time_.Update(double(tail));
        completion_time_.Update(double(frame.gpu_completed_at - frame.closed_at));
        // Respond immediately to a longer preparation phase and decay toward
        // shorter samples. Native encode/submit is not part of CPU recording.
        cpu_ns_ = uint64_t(std::max(double(cpu), cpu_time_.Get()) +
                           std::max(double(tail), submit_time_.Get()));
        close_to_complete_ = uint64_t(completion_time_.Get());
        if (previous_gpu_end_ && frame.gpu_started_at && frame.first_submitted_at) {
          const auto idle = frame.gpu_started_at > previous_gpu_end_ ? frame.gpu_started_at - previous_gpu_end_ : 0;
          const auto queued = frame.gpu_started_at > frame.first_submitted_at
              ? frame.gpu_started_at - frame.first_submitted_at : 0;
          // Wake lateness is only an upper bound. Advance when the GPU ran dry,
          // and withdraw that advance when it merely creates more queued work.
          wake_advance_ = std::clamp(wake_advance_ + wake_delay_.alpha * (double(idle) - double(queued)),
                                     0.0, wake_delay_.Get());
        }
        previous_gpu_end_ = std::max(previous_gpu_end_, frame.gpu_completed_at);
      } else {
        // Canceled/restarted frames provide no prediction sample; old projections are invalid.
        controller_ = {};
        cpu_ns_ = close_to_complete_ = 0;
        cpu_time_ = submit_time_ = completion_time_ = {0.3};
        previous_gpu_end_ = 0;
        wake_advance_ = 0;
      }
      Emit(EventKind::Complete, Source::Forced, frame.id, frame.completed_at ? frame.completed_at : Now(), 0, false,
           !frame.failed);
      frames_.pop_front();
      retired = true;
    }
    return retired;
  }
  void Release(Work &work) {
    std::lock_guard lock(mutex_);
    auto &frame = *work.frame;
    frame.failed |= !work.completed;
    --frame.pending;
    if (Retire()) Notify();
  }
  void ReleasePresent() {
    std::lock_guard lock(mutex_);
    --preparing_presents_;
    Notify();
  }
  Result Admit(Source source) {
    std::unique_lock lock(mutex_);
    const auto start = Now();
    if (removed_) return Result::Removed;
    const auto index = source == Source::AntiLag ? 1 : 0;
    if (source != Source::Forced && !low_[index] && !interval_[index]) return Result::Ready;
    if (!Active()) return Result::Ready;
    const uint32_t source_bit = 1u << uint32_t(source);
    if (source != Source::Forced && !force_ && !low_[0] && !low_[1]) {
      // An interval-only limiter is useful without any swapchain. It must not
      // reserve simulation frames that an offscreen caller can never Present.
      if (open_ && open_->admitted && !open_->closed && !(open_->sleep_sources & source_bit)) {
        open_->cooperative = true;
        open_->sleep_sources |= source_bit;
        last_sleep_frame_ = open_->id;
        sleep_us_ = 0;
        return Result::Ready;
      }
      if (interval_sources_ && !(interval_sources_ & source_bit)) {
        interval_sources_ |= source_bit;
        sleep_us_ = 0;
        return Result::Ready;
      }
      for (;;) {
        if (removed_) return Result::Removed;
        const auto now = Now();
        const auto deadline = last_wake_ + uint64_t(interval_[index]) * 1000;
        if (!last_wake_ || !interval_[index] || now >= deadline) {
          last_wake_ = now;
          last_sleep_frame_ = sequence_;
          interval_sources_ = source_bit;
          sleep_us_ = uint32_t(std::min<uint64_t>(UINT32_MAX, (now - start) / 1000));
          return Result::Ready;
        }
        Emit(EventKind::Wait, source, 0, now, deadline);
        if (!WaitUntil(lock, deadline)) return Result::Failed;
      }
    }
    // A rendering thread can get ahead of the asynchronous FG presenter. Repeated
    // Sleep calls from the same SDK reserve successive CPU frames; a second SDK
    // calling Sleep for the same CPU frame shares the existing reservation.
    if (source != Source::Forced && open_ && (open_->render_ended || (open_->sleep_sources & source_bit)))
      open_.reset();
    auto frame = Open();
    // Both SDKs may be present, or force may already have admitted this same frame.
    // Reuse the reservation, including its interval wait, instead of sleeping twice.
    for (;;) {
      if (removed_) return Result::Removed;
      if (!Active() || (source != Source::Forced && !low_[index] && !interval_[index])) return Result::Ready;
      // A different presenter may consume this reservation while the condition
      // wait releases the mutex. Never discount or admit a retired frame.
      if (frame->closed) frame = Open();
      if (frame->admitted) {
        if (source != Source::Forced) {
          frame->cooperative = true;
          frame->sleep_sources |= source_bit;
          last_sleep_frame_ = frame->id;
          bindings_[std::this_thread::get_id()] = {frame, false};
          sleep_us_ = 0;
        }
        return Result::Ready;
      }
      const auto now = Now();
      auto candidate = controller_;
      const bool adaptive = force_ || low_[0] || low_[1];
      const auto target = frame->begun || !adaptive ? 0 : candidate.Target(frame->id);
      // Never predict past draining the newest closed frame minus the next CPU phase.
      // This preserves CPU/GPU overlap even when workload/cadence changes abruptly.
      uint64_t latest_close = 0;
      for (const auto &previous : frames_)
        if (previous->closed) latest_close = previous->closed_at;
      const auto drain = !latest_close || !close_to_complete_ ? 0 : latest_close + close_to_complete_;
      const auto overlap = drain > cpu_ns_ ? drain - cpu_ns_ : 0;
      auto deadline = std::min(target, overlap);
      const auto advance = uint64_t(wake_advance_);
      deadline = deadline > advance ? deadline - advance : 0;
      const uint64_t interval = uint64_t(source == Source::Forced
          ? std::max(interval_[0], interval_[1]) : interval_[index]) * 1000;
      // Adaptive prediction may wake early to compensate for observed lateness;
      // the game's minimum interval remains an uncompensated hard lower bound.
      if (last_wake_ && interval) deadline = std::max(deadline, last_wake_ + interval);
      const bool capacity = adaptive && PendingFrames() - size_t(frame->begun) >= 2;
      if (!capacity && deadline <= now) {
        controller_ = candidate;
        Begin(*frame, target, now);
        frame->admitted = true;
        frame->cooperative = source != Source::Forced;
        if (frame->cooperative) frame->sleep_sources |= source_bit;
        last_wake_ = now;
        if (frame->cooperative) last_sleep_frame_ = frame->id;
        bindings_[std::this_thread::get_id()] = {frame, false};
        sleep_us_ = uint32_t(std::min<uint64_t>(UINT32_MAX, (now - start) / 1000));
        Emit(EventKind::Admit, source, frame->id, now, target);
        return Result::Ready;
      }
      Emit(EventKind::Wait, source, frame->id, now, deadline, capacity);
      if (capacity) cv_.wait(lock);
      else if (!WaitUntil(lock, deadline)) return Result::Failed;
    }
  }
public:
  explicit LowLatencyScheduler(bool force = false, Observer observer = nullptr, void *context = nullptr) :
      force_(force), observer_(observer), context_(context) {}
  static uint64_t Now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
  }
  Result SetMode(Source source, bool low, uint32_t interval) {
    std::lock_guard lock(mutex_);
    if (removed_) return Result::Removed;
    const auto index = source == Source::AntiLag ? 1 : 0;
    low_[index] = low;
    interval_[index] = interval;
    if (!Active()) {
      // Release CPU reservations that can no longer be presented by this mode.
      // Pending task tokens still gate retirement; disabling is not GPU completion.
      for (const auto &frame : frames_)
        if (!frame->closed) { frame->closed = true; frame->failed = true; }
      open_.reset();
      Retire();
      last_sleep_frame_ = 0;
      interval_sources_ = 0;
    }
    Emit(EventKind::Mode, source, open_ ? open_->id : 0, Now());
    Notify();
    return Result::Ready;
  }
  Result Sleep(Source source = Source::Reflex) { return Admit(source); }
  // Reserve capacity before taking a queue lock. Preparation can drain a layer's
  // GPU work without blocking other queues or letting them consume this credit.
  // Release after capture/publication (or any early return); closed frames then
  // retain their own capacity until actual retirement.
  Result ReservePresent(bool do_not_wait, PresentReservation &reservation) {
    std::unique_lock lock(mutex_);
    while (!removed_ && (force_ || low_[0] || low_[1]) &&
           preparing_presents_ +
             std::count_if(frames_.begin(), frames_.end(), [](const auto &frame) { return frame->closed; }) >= 2) {
      if (do_not_wait) return Result::WouldBlock;
      Emit(EventKind::Wait, Source::Forced, open_ ? open_->id : 0, Now(), 0, true);
      cv_.wait(lock);
    }
    if (removed_) return Result::Removed;
    ++preparing_presents_;
    reservation.owner_ = this;
    return Result::Ready;
  }
  std::shared_ptr<Work> Capture(bool present = false, bool reporting = false,
                                std::shared_ptr<Frame> frame = {}) {
    std::lock_guard lock(mutex_);
    if ((!Active() && !reporting) || removed_) return {};
    if (present && !frame)
      for (const auto &candidate : frames_)
        if (!candidate->closed) { frame = candidate; break; }
    if (!frame) frame = present ? Open() : Rendering();
    if (!present) {
      auto binding = bindings_.find(std::this_thread::get_id());
      const bool oob = binding != bindings_.end() && binding->second.out_of_band &&
                       binding->second.frame.lock() == frame;
      bindings_[std::this_thread::get_id()] = {frame, oob};
    }
    auto work = std::make_shared<Work>();
    work->owner = this;
    work->frame = frame;
    work->present = present;
    Begin(*frame, 0, Now());
    ++frame->pending;
    return work;
  }
  std::shared_ptr<Frame> BeginSimulation() {
    std::lock_guard lock(mutex_);
    auto frame = Rendering();
    if (frame->simulation_marked) {
      open_.reset();
      frame = Open();
    }
    frame->simulation_marked = true;
    bindings_[std::this_thread::get_id()] = {frame, false};
    return frame;
  }
  void Bind(const std::shared_ptr<Frame> &frame, bool out_of_band = false) {
    std::lock_guard lock(mutex_);
    if (frame) bindings_[std::this_thread::get_id()] = {frame, out_of_band};
  }
  void Close(const std::shared_ptr<Work> &work) {
    if (!work) return;
    std::lock_guard lock(mutex_);
    auto frame = work->frame;
    if (frame->closed) return;
    frame->closed_at = Now();
    frame->closed = true;
    if (!frame->cpu_hint) frame->cpu_ended_at = frame->closed_at;
    if (open_ == frame) open_.reset();
    Emit(EventKind::Close, Source::Forced, frame->id, frame->closed_at);
  }
  void Submitted(const std::shared_ptr<Work> &work, uint64_t timestamp) {
    if (!work || work->present) return;
    std::lock_guard lock(mutex_);
    if (!work->frame->first_submitted_at || timestamp < work->frame->first_submitted_at)
      work->frame->first_submitted_at = timestamp;
    work->frame->submitted_at = std::max(work->frame->submitted_at, timestamp);
  }
  void Complete(const std::shared_ptr<Work> &work, bool success, uint64_t gpu_end = 0, uint64_t gpu_start = 0) {
    if (!work) return;
    std::lock_guard lock(mutex_);
    work->completed = true;
    work->frame->failed |= !success;
    const auto received = Now();
    work->frame->completed_at = std::max(work->frame->completed_at, received);
    if (gpu_end >= work->frame->begin && gpu_end <= received)
      work->frame->gpu_completed_at = std::max(work->frame->gpu_completed_at, gpu_end);
    else
      work->frame->timing_valid = false;
    if (!work->present && gpu_start >= work->frame->begin && gpu_start <= gpu_end &&
        (!work->frame->gpu_started_at || gpu_start < work->frame->gpu_started_at))
      work->frame->gpu_started_at = gpu_start;
  }
  void SkipPresent(std::shared_ptr<Frame> frame = {}) {
    std::lock_guard lock(mutex_);
    if (!frame)
      for (const auto &candidate : frames_)
        if (!candidate->closed) { frame = candidate; break; }
    if (frame && !frame->closed) {
      frame->closed = frame->failed = true;
      if (open_ == frame) open_.reset();
    }
    Retire();
    Notify();
  }
  Result PresentReturn(const std::shared_ptr<Frame> &frame, bool do_not_wait) {
    {
      std::lock_guard lock(mutex_);
      if (removed_) return Result::Removed;
      if (!Active() || !frame || !frame->closed || frame->cooperative || do_not_wait) return Result::Ready;
      if (interval_sources_ && frame->id == last_sleep_frame_) return Result::Ready;
    }
    return Admit(Source::Forced);
  }
  Result EndFrame(std::shared_ptr<Frame> frame = {}) {
    std::lock_guard lock(mutex_);
    if (removed_) return Result::Removed;
    if (frames_.empty()) return Result::Ready;
    if (!frame) {
      auto binding = bindings_.find(std::this_thread::get_id());
      if (binding != bindings_.end()) frame = binding->second.frame.lock();
      if (!frame || frame->closed) {
        for (const auto &candidate : frames_)
          if (!candidate->closed && !candidate->render_ended) { frame = candidate; break; }
      }
    }
    if (!frame || frame->closed) return Result::Ready;
    if (frame->begun && !frame->cpu_hint) {
      frame->cpu_ended_at = Now();
      frame->cpu_hint = true;
    }
    frame->render_ended = true;
    return Result::Ready;
  }
  Result Status(bool &low, uint32_t &sleep, bool &game) {
    std::lock_guard lock(mutex_);
    if (removed_) return Result::Removed;
    low = low_[0];
    sleep = sleep_us_;
    game = last_sleep_frame_ && (open_ ? open_->id <= last_sleep_frame_ : sequence_ <= last_sleep_frame_ + 1);
    return Result::Ready;
  }
  void Remove() {
    std::lock_guard lock(mutex_);
    removed_ = true;
    Notify();
  }
};
} // namespace owned_graphics
