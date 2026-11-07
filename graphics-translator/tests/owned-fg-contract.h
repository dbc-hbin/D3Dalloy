#pragma once
#include <cstdint>

// Binary capture shared only by the NGX test executable and native observer.
constexpr uint32_t fgProbeExtent = 128, fgProbeRow = 1024;
constexpr uint32_t fgProbeHeader = 16384, fgProbePlane = fgProbeRow * fgProbeExtent;
constexpr uint32_t fgProbeBytes = fgProbeHeader + 4 * fgProbePlane;
struct FgProbeFrame {
  uint32_t serial, width, height, contentWidth, contentHeight;
  uint32_t reset, reversed, hasUI, hasDistortion;
  uint64_t effect;
  float jitter[2], motionScale[2], nearPlane, farPlane, aspect, fov, delta;
  float view[16], projection[16];
};
static_assert(sizeof(FgProbeFrame) <= fgProbeHeader);
