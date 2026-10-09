#include "dxgi_options.hpp"
#include <charconv>

namespace dxmt {

static int32_t parsePciId(const std::string &str) {
  if (str.size() != 4)
    return -1;

  uint32_t id = 0;
  const auto end = str.data() + str.size();
  const auto result = std::from_chars(str.data(), end, id, 16);
  return result.ec == std::errc{} && result.ptr == end ? int32_t(id) : -1;
}

DxgiOptions::DxgiOptions(const Config &config) {
  // Fetch these as a string representing a hexadecimal number and parse it.
  this->customVendorId =
      parsePciId(config.getOption<std::string>("dxgi.customVendorId"));
  this->customDeviceId =
      parsePciId(config.getOption<std::string>("dxgi.customDeviceId"));
  this->customDeviceDesc =
      config.getOption<std::string>("dxgi.customDeviceDesc", "");
  this->forceSDR = config.getOption<bool>("dxgi.forceSDR", false);
}

} // namespace dxmt
