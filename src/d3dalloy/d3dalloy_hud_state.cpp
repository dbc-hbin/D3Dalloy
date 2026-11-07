#include "d3dalloy_hud_state.hpp"
#include "Metal.hpp"
#include "Metal4.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>

namespace d3dalloy {

namespace {
constexpr unsigned statistic_count = unsigned(HUDStatistic::Count);
struct StatisticDescriptor {
  const char *name;
  const char *suffix;
  unsigned padding;
};
constexpr std::array<StatisticDescriptor, statistic_count> statistic_descriptors{{
    {"Render Encoders", "re-count", 11}, {"Compute Encoders", "ce-count", 10},
    {"Blit Encoders", "be-count", 13}, {"Acceleration Encoders", "ae-count", 5},
    {"Command Buffers", "cb-count", 11}, {"Dispatch", "disp-count", 18},
    {"Dispatch Mesh", "dispmesh-count", 13}, {"Draw", "fragdraw-count", 22},
    {"Draw(GS)", "geom-count", 18}, {"Draw(TS)", "tess-count", 18}, {"Draw(GSTS)", "geomtess-count", 16},
    {"Clear Resource", "clear-count", 12}, {"Copy Resource", "copy-count", 13},
    {"ExecuteIndirect", "ei-count", 11}, {"ExecuteIndirect ICB", "ei-icb-count", 7},
    {"Encoder Fence Waits", "encoder-fence-waits", 5}, {"Skipped Fence Waits", "skipped-fence-waits", 5},
    {"Dispatch Rays", "dispray-count", 13}, {"Ray Query", "rayquery-count", 17},
    {"Build BVH", "buildbvh-count", 17}, {"Refit BVH", "refitbvh-count", 17}, {"Tile Update", "tile-update-count", 15},
}};
std::array<std::atomic<uint32_t>, statistic_count> statistics{};
std::mutex statistics_mutex;
std::array<std::array<uint32_t, 4>, statistic_count> statistic_history{};
std::array<uint32_t, statistic_count> statistic_sums{};
std::array<WMT::Reference<WMT::String>, statistic_count> statistic_keys;
WMT::DeveloperHUDProperties statistics_hud;
bool statistics_initialized = false, statistics_modern = false;
}

bool
HUDState::statisticsEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("D3DALLOY_SHOW_HUD_STATS");
    return !value || std::atoi(value) != 0;
  }();
  return enabled;
}

void
HUDState::addStatistic(HUDStatistic statistic, uint32_t count) {
  if (statisticsEnabled())
    statistics[unsigned(statistic)].fetch_add(count, std::memory_order_relaxed);
}

void
HUDState::initializeStatistics() {
  if (!statisticsEnabled() || !hud_.handle) return;
  std::lock_guard lock(statistics_mutex);
  if (statistics_initialized) return;
  auto pool = WMT::MakeAutoreleasePool();
  statistics_hud = hud_;
  statistics_modern = WMT::Metal4Invoke(WMTMetal4HUDMetric, hud_.handle, 0, 0, 0, 2).ret[0] != 0;
  WMT::String previous = WMT::String::string("com.github.d3dalloy.version", WMTUTF8StringEncoding);
  for (unsigned i = 0; i < statistic_count; ++i) {
    const bool insight = i == unsigned(HUDStatistic::EncoderFenceWaits) || i == unsigned(HUDStatistic::SkippedFenceWaits);
    if (!statistics_modern && (insight || i == unsigned(HUDStatistic::TileUpdate))) continue;
    const auto &descriptor = statistic_descriptors[i];
    statistic_keys[i] = WMT::MakeString(
        (std::string("com.github.d3dalloy.") + descriptor.suffix).c_str(), WMTUTF8StringEncoding);
    auto name = WMT::String::string(descriptor.name, WMTUTF8StringEncoding);
    WMT::Metal4Invoke(WMTMetal4HUDMetric, hud_.handle, statistic_keys[i].handle, name.handle, 0, 0,
                     insight ? 0x400 : 0x800, previous.handle);
    previous = statistic_keys[i];
  }
  statistics_initialized = true;
}

void
HUDState::sampleStatistics(uint64_t present_count) {
  if (!statisticsEnabled()) return;
  std::lock_guard lock(statistics_mutex);
  if (!statistics_initialized) return;
  auto pool = WMT::MakeAutoreleasePool();
  const auto native = WMT::Metal4Invoke(WMTMetal4HUDCounters);
  constexpr HUDStatistic native_statistics[] = {
      HUDStatistic::RenderEncoders, HUDStatistic::ComputeEncoders, HUDStatistic::BlitEncoders,
      HUDStatistic::AccelerationEncoders, HUDStatistic::CommandBuffers, HUDStatistic::BuildBVH,
      HUDStatistic::RefitBVH, HUDStatistic::EncoderFenceWaits};
  for (unsigned i = 0; i < 8; ++i)
    statistics[unsigned(native_statistics[i])].fetch_add(uint32_t(native.ret[i]), std::memory_order_relaxed);
  const unsigned slot = present_count & 3;
  for (unsigned i = 0; i < statistic_count; ++i) {
    const auto sample = statistics[i].exchange(0, std::memory_order_relaxed);
    statistic_sums[i] += sample - statistic_history[i][slot];
    statistic_history[i][slot] = sample;
    if (!statistic_keys[i]) continue;
    const uint32_t average = statistic_sums[i] / 4;
    const auto &descriptor = statistic_descriptors[i];
    WMT::String text;
    if (!statistics_modern) {
      char buffer[128];
      std::snprintf(buffer, sizeof(buffer), "%s %*u", descriptor.name, int(descriptor.padding), average);
      text = WMT::String::string(buffer, WMTUTF8StringEncoding);
    }
    WMT::Metal4Invoke(WMTMetal4HUDMetric, statistics_hud.handle, statistic_keys[i].handle,
                     text.handle, average, 1);
  }
}

void
HUDState::initialize(const std::string &heading) {
  using namespace WMT;
  auto pool = MakeAutoreleasePool();
  auto str_d3dalloy_version = MakeString("com.github.d3dalloy.version", WMTUTF8StringEncoding);
  hud_.addLabel(str_d3dalloy_version, String::string("com.apple.hud-graph.default", WMTUTF8StringEncoding));
  hud_.updateLabel(str_d3dalloy_version, String::string(heading.c_str(), WMTUTF8StringEncoding));
  line_labels_.push_back(std::move(str_d3dalloy_version));
}

void
HUDState::begin() {
#ifndef D3DALLOY_DEBUG
  return;
#endif
  pool_ = WMT::MakeAutoreleasePool();
  current_line_ = 1;
}

void
HUDState::printLine(const char *c_str) {
#ifndef D3DALLOY_DEBUG
  return;
#endif
  using namespace WMT;
  while (current_line_ >= line_labels_.size()) {
    String prev = line_labels_.back();
    line_labels_.push_back(MakeString(
        ("com.github.d3dalloy.line" + std::to_string(line_labels_.size())).c_str(), WMTUTF8StringEncoding
    ));
    hud_.addLabel(line_labels_.back(), prev);
  }
  hud_.updateLabel(line_labels_[current_line_], String::string(c_str, WMTUTF8StringEncoding));
  current_line_++;
}

void
HUDState::end() {
#ifndef D3DALLOY_DEBUG
  return;
#endif
  while (line_labels_.size() > current_line_) {
    hud_.remove(line_labels_.back());
    line_labels_.pop_back();
  }
  pool_ = nullptr;
}

} // namespace d3dalloy