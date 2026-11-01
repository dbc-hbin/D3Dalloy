#pragma once
#include "d3dalloy_command_queue.hpp"
#include <memory>

namespace d3dalloy {

class Device {
public:
  virtual ~Device() {}

  virtual WMT::Device device() = 0;
  virtual CommandQueue& queue() = 0;
  virtual WMTMetalVersion metalVersion() = 0;
  virtual uint64_t maxObjectThreadgroups() = 0;
};

struct DEVICE_DESC {
  WMT::Device device;
};

std::unique_ptr<Device> CreateD3DalloyDevice(const DEVICE_DESC &desc);

} // namespace d3dalloy
