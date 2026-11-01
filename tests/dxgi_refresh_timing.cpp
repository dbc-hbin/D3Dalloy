// Host-only: clang++ -std=c++20 -Wall -Wextra -Werror tests/dxgi_refresh_timing.cpp -o /tmp/dxgi-refresh-timing && /tmp/dxgi-refresh-timing
#include "../src/dxgi/dxgi_refresh_timing.hpp"
#include <cassert>
#include <initializer_list>
#include <limits>

int main() {
  using d3dalloy::RefreshTickOffset;
  constexpr double anchor = 100, period = 1;
  // Both sides of the former +/-0.4 phase cutoff are valid presentations.
  for (double fraction : {0.39, 0.41, 0.49, 0.5, 0.51, 0.59, 0.61}) {
    assert(RefreshTickOffset(anchor + fraction, anchor, period) == 0);
    assert(RefreshTickOffset(anchor - fraction, anchor, period) == -1);
  }
  assert(RefreshTickOffset(anchor, anchor, period) == 0);
  assert(RefreshTickOffset(std::nextafter(anchor, -std::numeric_limits<double>::infinity()), anchor, period) == -1);
  assert(RefreshTickOffset(std::nextafter(anchor, std::numeric_limits<double>::infinity()), anchor, period) == 0);
  assert(RefreshTickOffset(anchor + 1, anchor, period) == 1);
  assert(RefreshTickOffset(anchor - 1, anchor, period) == -1);
  assert(RefreshTickOffset(std::nextafter(anchor - 1, -std::numeric_limits<double>::infinity()), anchor, period) == -2);

  // Runtime03 raw native/CV samples: the output callback predicts a refresh
  // more than one tick in the future. Statistics must not publish that tick.
  constexpr double native_time = 396494.481719375;
  constexpr double event_anchor = 396494.498068193;
  constexpr double sample_anchor = 396494.514734859;
  constexpr double observed_time = 396494.493151000;
  constexpr double native_period = 16666666.0 / 1000000000.0;
  const auto present_delta = RefreshTickOffset(native_time, event_anchor, native_period);
  const auto sync_delta = RefreshTickOffset(observed_time, sample_anchor, native_period);
  assert(present_delta == -1 && sync_delta == -2);
  assert(22 + present_delta == 21 && 23 + sync_delta == 21);
  const double sync_time = sample_anchor + sync_delta * native_period;
  assert(sync_time <= observed_time && observed_time - sync_time < native_period);

  // Root33 WoW64 raw clock inputs: callbacks33 and34 predict the SAME reached
  // native refresh, but fresh query brackets regress its QPC by two ticks.
  constexpr int64_t reached33 = 473302747734556LL - 16666666;
  constexpr int64_t reached34 = 473302764401222LL - 2 * 16666666;
  constexpr double host33 = 473302749241602.0 / 1000000000.0 - native_period;
  constexpr double host34 = 473302765908268.0 / 1000000000.0 - 2 * native_period;
  constexpr double sampled5 = 473302.73742558406, sampled6 = 473302.74483220902;
  constexpr int64_t midpoint5 = 4825489582809LL + (4825489582819LL - 4825489582809LL) / 2;
  constexpr int64_t midpoint6 = 4825489656872LL + (4825489656886LL - 4825489656872LL) / 2;
  constexpr int64_t frequency = 10000000;
  assert(reached33 == reached34 && host33 == host34);
  const int64_t old5 = midpoint5 + std::llround((host33 - sampled5) * frequency);
  const int64_t old6 = midpoint6 + std::llround((host34 - sampled6) * frequency);
  assert(old5 == 4825489534308LL && old6 == old5 - 2);

  d3dalloy::RefreshClockBasis clock;
  assert(clock.SetEpoch(1, 1, 1000000000, 1000000000));
  clock.calibration_host_seconds = sampled5;
  clock.calibration_qpc = midpoint5;
  clock.qpc_frequency = frequency;
  int64_t projected = 0;
  assert(clock.Project(reached33, host33, projected) && projected == old5);
  assert(!clock.SetEpoch(1, 1, 1000000000, 1000000000));
  assert(clock.Project(reached34, host34, projected) && projected == old5);
  // A revised prediction for that physical token must not change its timestamp.
  assert(clock.Project(reached34, std::nextafter(host34, 0.0), projected) && projected == old5);
  assert(clock.Project(reached34 + 16666666, host34 + native_period, projected));
  const int64_t advanced = projected;
  assert(advanced > old5);
  assert(!clock.Project(reached34, host34, projected) && projected == advanced);
  assert(!clock.Project(reached34 + 2 * 16666666, host34 - native_period, projected));
  assert(projected == advanced);

  // An epoch boundary clears both the old calibration and token, so a new
  // actual clock origin is allowed only with an explicit disjoint notification.
  assert(clock.SetEpoch(2, 1, 1000000000, 1000000000));
  assert(!clock.qpc_frequency && !clock.last_video_time);
  assert(!clock.Project(reached34, host34, projected));
  clock.calibration_host_seconds = host34;
  clock.calibration_qpc = old5 - 10;
  clock.qpc_frequency = frequency;
  assert(clock.Project(reached34, host34, projected) && projected == old5 - 10);
  assert(clock.SetEpoch(2, 2, 1000000000, 1000000000));
  assert(!clock.qpc_frequency && !clock.last_video_time);
  assert(clock.SetEpoch(2, 2, 2000000000, 1000000000));
  assert(clock.SetEpoch(2, 2, 2000000000, 500000000));
}
