// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned graphics backend: deferred native graphics pipeline creation (opt-in
// d3d12.asyncPipelineCompile) and the render-target recurrence that decides
// when a draw may be omitted while its pipeline is still compiling.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace owned_graphics {

// One deferred compile. Exactly one caller (a worker or a synchronous consumer)
// runs the work; every other caller waits for it. Cancel() retires unstarted
// work without running it, so a callback that arrives after the owner is gone
// finds Done and never touches the owner.
class AsyncCompileJob {
public:
  enum Status : int { Pending, Ready, Failed };

  explicit AsyncCompileJob(std::function<bool()> work) : work_(std::move(work)) {}

  Status status() const { return Status(status_.load(std::memory_order_acquire)); }

  // Returns true if this call ran the work (false: already claimed or done).
  bool Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == State::Queued) {
      state_ = State::Running;
      auto work = std::move(work_);
      lock.unlock();
      bool ok = false;
      try {
        ok = work();
      } catch (...) {
        ok = false;
      }
      lock.lock();
      Finish(ok ? Ready : Failed);
      return true;
    }
    cv_.wait(lock, [&] { return state_ == State::Done; });
    return false;
  }

  // Ready after this returns true. Runs unstarted work on the caller.
  bool Wait() {
    if (status() == Pending)
      Run();
    return status() == Ready;
  }

  // Returns true if the work never ran.
  bool Cancel() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == State::Queued) {
      work_ = nullptr;
      Finish(Failed);
      return true;
    }
    cv_.wait(lock, [&] { return state_ == State::Done; });
    return false;
  }

private:
  enum class State { Queued, Running, Done };

  void Finish(Status status) {
    state_ = State::Done;
    status_.store(status, std::memory_order_release);
    cv_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  State state_ = State::Queued;
  std::function<bool()> work_;
  std::atomic<int> status_{Pending};
};

// FIFO of jobs drained by worker threads that the owner creates and joins.
class AsyncCompileQueue {
public:
  void Submit(std::shared_ptr<AsyncCompileJob> job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  // Worker body; returns after Stop().
  void RunWorker() {
    for (;;) {
      std::shared_ptr<AsyncCompileJob> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
        if (stop_)
          return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job->Run();
    }
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      jobs_.clear();
    }
    cv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<AsyncCompileJob>> jobs_;
  bool stop_ = false;
};

// Attachment recurrence across presentation epochs. Only emitted, nonempty
// draws are recorded; omitted draws never extend a target's history, so a
// target drawn only by omitted draws expires and the next draw waits.
class RenderTargetHistory {
public:
  // Consecutive completed (presented) epochs a target must be drawn before omission is allowed.
  static constexpr uint64_t kRecurringEpochs = 5;

  // `extent` separates views that share a TextureView but cover different ranges (3D RTV WSize).
  struct Key {
    uint64_t resource;
    uint64_t view;
    uint32_t plane;
    uint32_t extent;
    bool operator<(const Key &other) const {
      return std::tie(resource, view, plane, extent) <
             std::tie(other.resource, other.view, other.plane, other.extent);
    }
  };

  void Record(const Key &key, uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch != pruned_epoch_ && entries_.size() >= kPruneThreshold) {
      pruned_epoch_ = epoch;
      for (auto it = entries_.begin(); it != entries_.end();)
        it = it->second.last + 1 < epoch ? entries_.erase(it) : std::next(it);
    }
    auto &entry = entries_[key];
    if (entry.streak && entry.last == epoch)
      return;
    entry.streak = entry.streak && entry.last + 1 == epoch ? entry.streak + 1 : 1;
    entry.last = epoch;
  }

  // The current epoch has not been presented yet, so a draw in it never counts toward the streak.
  bool Recurring(const Key &key, uint64_t epoch) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
      return false;
    const auto &entry = it->second;
    const uint64_t completed = entry.last == epoch ? entry.streak - 1 : entry.last + 1 == epoch ? entry.streak : 0;
    return completed >= kRecurringEpochs;
  }

private:
  // Stale entries cannot become eligible again without a fresh streak.
  static constexpr size_t kPruneThreshold = 256;
  struct Entry {
    uint64_t last = 0;
    uint64_t streak = 0;
  };
  mutable std::mutex mutex_;
  std::map<Key, Entry> entries_;
  uint64_t pruned_epoch_ = 0;
};

} // namespace owned_graphics
