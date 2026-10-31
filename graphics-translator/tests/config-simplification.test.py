#!/usr/bin/env python3
# Run: python3 graphics-translator/tests/config-simplification.test.py
# Exercise production numeric parsers through the real Config::getOption template.
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = r'''
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include "src/util/config/config.hpp"
namespace dxmt {
'''
implementation = (ROOT / "src/util/config/config.cpp").read_text()
for pattern in (
    r"Config::Config\(\) \{ \}",
    r"Config::~Config\(\) \{ \}",
    r"void Config::setOption\(.*?\n  \}",
    r"std::string Config::getOptionValue\(.*?\n  \}",
    r"bool Config::parseOptionValue\(\n    const std::string&  value,\n          (?:int32_t|float)& +result\) \{.*?\n  \}",
):
    source += "\n".join(match.group(0) for match in re.finditer(pattern, implementation, re.S)) + "\n"
source += r'''
}

void check_integer(dxmt::Config &config, const std::string &value, int32_t expected) {
  config.setOption("integer", value);
  assert(config.getOption<int32_t>("integer", 42) == expected);
}

void check_float(dxmt::Config &config, const std::string &value, float expected) {
  config.setOption("float", value);
  const float result = config.getOption<float>("float", -123.5f);
  assert(result == expected);
  assert(std::signbit(result) == std::signbit(expected));
}

int main() {
  dxmt::Config config;
  assert(config.getOption<int32_t>("missing", 42) == 42);
  assert(config.getOption<float>("missing", -123.5f) == -123.5f);
  check_integer(config, "0", 0);
  check_integer(config, "-0", 0);
  check_integer(config, "2147483647", INT32_MAX);
  check_integer(config, "-2147483648", INT32_MIN);
  for (const char* value : {"2147483648", "-2147483649", "999999999999999999", "-"})
    check_integer(config, value, 42);

  check_float(config, "0", 0.0f);
  check_float(config, "-0", -0.0f);
  check_float(config, "-0.000", -0.0f);
  check_float(config, "1", 1.0f);
  check_float(config, "-12.5", -12.5f);
  check_float(config, "0.1", 0.1f);
  check_float(config, "1.23456789", 1.23456789f);
  check_float(config, "18446744073709551616", 0x1p64f);
  check_float(config, "123456789012345678901234567890", 1.23456789012345678901234567890e29f);
  check_float(config, std::string(512, '0') + "12.5", 12.5f);
  check_float(config, "1." + std::string(512, '0'), 1.0f);
  check_float(config, "0." + std::string(512, '0'), 0.0f);
  // A significant digit beyond double precision must still round the float up.
  check_float(config, "1.000000059604644775390625" + std::string(200, '0') + "1",
              std::nextafter(1.0f, 2.0f));

  const float maximum = std::numeric_limits<float>::max();
  check_float(config, "340282346638528859811704183484516925440", maximum);
  check_float(config, "-340282346638528859811704183484516925440", -maximum);
  // This is just below the rounding boundary between FLT_MAX and infinity.
  check_float(config, "340282356779733661637539395458142568447", maximum);
  check_float(config, "-340282356779733661637539395458142568447", -maximum);
  check_float(config, "340282346638528859811704183484516925440." + std::string(512, '0'), maximum);
  check_float(config, "0.00000000000000000000000000000000000000000000140129846432481707092372958328991613128026194187651577175706828388979108268586060148663818836212158203125",
              std::numeric_limits<float>::denorm_min());
  check_float(config, "0." + std::string(512, '0') + "1", 0.0f);
  check_float(config, "-0." + std::string(512, '0') + "1", -0.0f);

  for (const char *value : {
      "", "-", "+0", "+1", ".1", "-.1", "1.", "-1.", "1e0", "1E-1", "1.0f",
      "NaN", "nan", "INF", "infinity", "0x1", "1..0", "--1", "1.2.3", "1-2",
      " 1", "1 ", "1\n", "1,5", "340282356779733661637539395458142568448",
      "-340282356779733661637539395458142568448", "9999999999999999999999999999999999999999999"})
    check_float(config, value, -123.5f);
  check_float(config, std::string(512, '9'), -123.5f);
  check_float(config, "0." + std::string(512, '0') + "x", -123.5f);
  check_float(config, std::string("1\0.5", 5), -123.5f);
  config.setOption("float", "invalid");
  const float zero_fallback = config.getOption<float>("float", -0.0f);
  assert(zero_fallback == 0 && std::signbit(zero_fallback));
}
'''
with tempfile.TemporaryDirectory(prefix="config-simplification-") as temporary:
    binary = str(Path(temporary) / "probe")
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
                    "-Werror", "-I", str(ROOT), "-x", "c++", "-", "-o", binary],
                   input=source, text=True, check=True)
    subprocess.run([binary], check=True)
