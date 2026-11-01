// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

namespace owned_graphics {

inline std::uint64_t NewObjectId() {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline bool TraceEnabled() {
  static const bool enabled = d3dalloy::env::getEnvVar("OWNED_GRAPHICS_TRACE") == "1";
  return enabled;
}

inline std::uint64_t TraceLimit() {
  static const std::uint64_t limit = [] {
    auto value = d3dalloy::env::getEnvVar("OWNED_GRAPHICS_TRACE_LIMIT");
    if (value.empty())
      return 8192ULL;
    char *end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (end != value.c_str() + value.size() || !parsed || value[0] == '-')
      return 8192ULL;
    return std::min(parsed, 10000000ULL);
  }();
  return limit;
}

// Shared across event/template shapes so the limit bounds the whole DLL trace.
inline std::atomic<std::uint64_t> trace_event_counter{0};

// Event names and JSON keys are source literals; values are numeric IDs. Do not
// pass application annotation strings, paths, shader text, or arbitrary blobs.
// IDs are process-local and never derived from reusable COM object addresses.
template <typename... Fields>
inline void Trace(const char *event, Fields &&...fields) noexcept try {
  if (!TraceEnabled())
    return;
  static_assert(sizeof...(Fields) > 0);
  const auto sequence = trace_event_counter.fetch_add(1, std::memory_order_relaxed);
  if (sequence >= TraceLimit())
    return;
  d3dalloy::Logger::info(d3dalloy::str::format(
      "OWNED_GRAPHICS_TRACE {\"event\":\"", event, "\",\"event_id\":", sequence + 1,
      ",\"timestamp_ns\":", NowNs(),
      std::forward<Fields>(fields)..., "}"));
} catch (const std::bad_alloc &) {
  // Optional diagnostics must not throw out of recording cleanup or retirement.
}

} // namespace owned_graphics
