#pragma once

#include "Metal.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace d3dalloy {

enum class HUDStatistic : unsigned {
  RenderEncoders, ComputeEncoders, BlitEncoders, AccelerationEncoders, CommandBuffers,
  Dispatch, DispatchMesh, Draw, DrawGS, DrawTS, DrawGSTS, ClearResource, CopyResource,
  ExecuteIndirect, ExecuteIndirectICB, EncoderFenceWaits, SkippedFenceWaits,
  DispatchRays, RayQuery, BuildBVH, RefitBVH, TileUpdate, Count
};

class HUDState {
public:
  void initialize(const std::string &heading);
  void initializeStatistics();
  static bool statisticsEnabled();
  static void addStatistic(HUDStatistic statistic, uint32_t count = 1);
  static void sampleStatistics(uint64_t present_count);

  void begin();

  void
  printLine(const std::string &str) {
    printLine(str.c_str());
  }

  void printLine(const char *c_str);

  void end();

  HUDState(WMT::DeveloperHUDProperties hud) : hud_(hud){};

private:
  WMT::DeveloperHUDProperties hud_;
  std::vector<WMT::Reference<WMT::String>> line_labels_;
  WMT::Reference<WMT::Object> pool_;
  unsigned current_line_;
};

} // namespace d3dalloy