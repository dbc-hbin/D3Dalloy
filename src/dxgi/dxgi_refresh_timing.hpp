#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace dxmt {
// CoreVideo output timestamps may predict a future refresh. Native drawable
// presentation need not be vsync-aligned: select the interval containing time,
// never reject its phase or round it forward into a refresh not yet reached.
inline int64_t RefreshTickOffset(double time, double anchor, double period) {
  return static_cast<int64_t>(std::floor((time - anchor) / period));
}

// Inline per statistics owner, protected by that owner's existing mutex. A
// refresh keeps one QPC timestamp even if a later CV callback predicts it again.
struct RefreshClockBasis {
  uint64_t generation = 0, host_frequency = 0;
  uint32_t display_id = 0;
  int32_t video_time_scale = 0;
  double calibration_host_seconds = 0;
  int64_t calibration_qpc = 0, qpc_frequency = 0;
  int64_t last_video_time = 0, last_qpc = 0;
  double last_host_seconds = 0;

  bool SetEpoch(uint64_t new_generation, uint32_t new_display_id,
                uint64_t new_host_frequency, int32_t new_video_time_scale) {
    if (generation == new_generation && display_id == new_display_id &&
        host_frequency == new_host_frequency && video_time_scale == new_video_time_scale)
      return false;
    *this = {};
    generation = new_generation;
    display_id = new_display_id;
    host_frequency = new_host_frequency;
    video_time_scale = new_video_time_scale;
    return true;
  }

  bool Project(int64_t reached_video_time, double reached_host_seconds, int64_t &qpc) {
    if (qpc_frequency <= 0 || reached_video_time <= 0 ||
        !std::isfinite(reached_host_seconds) || reached_host_seconds <= 0 ||
        reached_video_time < last_video_time)
      return false;
    if (reached_video_time == last_video_time) {
      qpc = last_qpc;
      return true;
    }
    const double ticks = (reached_host_seconds - calibration_host_seconds) * qpc_frequency;
    constexpr auto min = std::numeric_limits<int64_t>::min();
    constexpr auto max = std::numeric_limits<int64_t>::max();
    if (!std::isfinite(ticks) || ticks <= static_cast<double>(min) || ticks >= static_cast<double>(max))
      return false;
    const int64_t delta = std::llround(ticks);
    if ((delta > 0 && calibration_qpc > max - delta) ||
        (delta < 0 && calibration_qpc < min - delta))
      return false;
    const int64_t projected = calibration_qpc + delta;
    if (last_video_time && (reached_host_seconds < last_host_seconds || projected < last_qpc))
      return false;
    last_video_time = reached_video_time;
    last_host_seconds = reached_host_seconds;
    last_qpc = projected;
    qpc = projected;
    return true;
  }
};
} // namespace dxmt
