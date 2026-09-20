// SPDX-License-Identifier: LGPL-2.1-or-later
// Queue-local close-and-drain FIFO used by the owned D3D12 submission worker.
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace owned_graphics {

template <typename Task>
class QueueTaskFifo {
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> tasks_;
  bool closed_ = false;

public:
  bool push(Task task) {
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return false;
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  bool waitPop(Task &task) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return closed_ || !tasks_.empty(); });
    if (tasks_.empty())
      return false;
    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
  }

  void close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }
};

} // namespace owned_graphics
