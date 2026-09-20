// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "Metal.hpp"
#include <memory>
#include <mutex>
#include <vector>

namespace owned_graphics {

// The tested Apple Metal driver needs the timestamp producer to complete before
// resolveCounters is encoded. Keep each pending element's producer independently
// so unrelated query slots and other queues never introduce a false dependency.
class QueryTimestampProducers {
  std::mutex mutex_;
  std::vector<WMT::Reference<WMT::CommandBuffer>> producers_;

public:
  explicit QueryTimestampProducers(size_t count) : producers_(count) {}

  void record(size_t index, const WMT::Reference<WMT::CommandBuffer> &producer) {
    std::lock_guard lock(mutex_);
    producers_[index] = producer;
  }

  void completed(size_t index, WMT::CommandBuffer producer) {
    std::lock_guard lock(mutex_);
    if (producers_[index] == producer)
      producers_[index] = nullptr;
  }

  std::vector<WMT::Reference<WMT::CommandBuffer>> snapshot(size_t first, size_t count) {
    std::lock_guard lock(mutex_);
    return {producers_.begin() + first, producers_.begin() + first + count};
  }
};

} // namespace owned_graphics
