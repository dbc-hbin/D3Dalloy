#pragma once

#include "../airconv/airconv_public.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

namespace d3dalloy::atomic64 {

// Shader lookup needs disjoint intervals. When descriptor ranges overlap,
// keep the original first matching map, including its sparse-hole entries.
inline void IndexRawRanges(std::vector<SM50_ATOMIC64_RAW_RANGE> &ranges) {
  using Event = std::pair<uint64_t, size_t>;
  std::vector<Event> events;
  events.reserve(2 * ranges.size());
  for (size_t i = 0; i < ranges.size(); ++i) {
    const auto &r = ranges[i];
    if (!r.byteLength) continue;
    if (r.byteLength > UINT64_MAX - r.gpuAddressBegin)
      throw std::length_error("atomic64 raw address overflow");
    events.emplace_back(r.gpuAddressBegin, 2 * i);
    events.emplace_back(r.gpuAddressBegin + r.byteLength, 2 * i + 1);
  }
  std::sort(events.begin(), events.end());
  std::set<size_t> active;
  std::vector<SM50_ATOMIC64_RAW_RANGE> indexed;
  size_t previous = ranges.size();
  for (size_t i = 0; i < events.size();) {
    const uint64_t begin = events[i].first;
    do {
      const auto tag = events[i++].second;
      if (tag & 1) active.erase(tag / 2);
      else active.insert(tag / 2);
    } while (i < events.size() && events[i].first == begin);
    if (i == events.size()) break;
    if (active.empty()) { previous = ranges.size(); continue; }
    const auto winner = *active.begin();
    const uint64_t length = events[i].first - begin;
    if (winner == previous) indexed.back().byteLength += length;
    else {
      const auto &r = ranges[winner];
      indexed.push_back({begin, length, r.cellMapAddr + ((begin - r.gpuAddressBegin) / 8) * sizeof(uint32_t)});
    }
    previous = winner;
  }
  ranges.swap(indexed);
}

} // namespace d3dalloy::atomic64
