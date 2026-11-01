#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run: python3 graphics-translator/tests/dxgi-simplification.test.py
# Compile the production function bodies and SDK declarations unchanged, without
# the surrounding Wine/Metal translation unit dependencies.
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract(path, pattern):
    return re.search(pattern, (ROOT / path).read_text(), re.S).group(0)


source = """
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>
#include <vector>
using UINT = uint32_t;
using WINBOOL = int32_t;
"""
for header, kind, name in (
    ("dxgicommon.h", "struct", "DXGI_RATIONAL"),
    ("dxgiformat.h", "enum", "DXGI_FORMAT"),
    ("dxgitype.h", "enum", "DXGI_MODE_SCANLINE_ORDER"),
    ("dxgitype.h", "enum", "DXGI_MODE_SCALING"),
    ("dxgi1_2.h", "struct", "DXGI_MODE_DESC1"),
):
    source += extract(f"include/native/directx/{header}",
                      rf"typedef {kind} {name} \{{.*?\}} {name};") + "\n"
source += extract("src/dxgi/dxgi_options.cpp", r"static int32_t parsePciId\(.*?\n\}")
source += extract("src/dxgi/dxgi_output.cpp", r"void FilterModesByDesc\(.*?\n\}")
source += r'''
std::vector<DXGI_FORMAT> format_warnings;
namespace str {
  DXGI_FORMAT format(const char *, DXGI_FORMAT value) { return value; }
}
struct Logger {
  static void warn(DXGI_FORMAT value) { format_warnings.push_back(value); }
};
'''
source += extract("src/d3dalloy/d3dalloy_monitor_format.hpp",
                  r"static inline uint32_t\nGetMonitorFormatBpp\(.*?\n\}")
source += r'''
int main() {
  for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                             DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                             DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,
                             DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM})
    assert(GetMonitorFormatBpp(format) == 32);
  assert(GetMonitorFormatBpp(DXGI_FORMAT_R16G16B16A16_FLOAT) == 64);
  assert(format_warnings.empty());
  for (DXGI_FORMAT format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8_UNORM,
                             DXGI_FORMAT_R16G16B16A16_UNORM, static_cast<DXGI_FORMAT>(0x1234)}) {
    const auto warnings_before = format_warnings.size();
    assert(GetMonitorFormatBpp(format) == 32);
    assert(format_warnings.size() == warnings_before + 1 && format_warnings.back() == format);
  }

  for (uint32_t id = 0; id <= 0xffff; ++id) {
    char hex[5];
    std::snprintf(hex, sizeof(hex), "%04x", id);
    assert(parsePciId(hex) == int32_t(id));
    std::snprintf(hex, sizeof(hex), "%04X", id);
    assert(parsePciId(hex) == int32_t(id));
  }
  assert(parsePciId("aBcD") == 0xabcd);
  for (const char *invalid : {"", "123", "12345", "-001", "+001", " 001", "001 ",
                              "0x01", "001g", "g001", "00\t1", "0.01"})
    assert(parsePciId(invalid) == -1);
  assert(parsePciId(std::string("001\0", 4)) == -1);

  DXGI_MODE_DESC1 target{1000, 1000, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_CENTERED, 0};
  auto low = target, high = target, far = target, stereo = target;
  low.Width = 900;
  high.Width = 1100;
  far.Width = 1200;
  low.RefreshRate = {5994, 100}; // Integer conversion yields 59, tied with 61.
  high.RefreshRate = {61, 1};
  stereo.Stereo = 1;
  std::vector<DXGI_MODE_DESC1> modes{far, high, stereo, low};
  FilterModesByDesc(modes, target);
  assert(modes.size() == 2 && modes[0].Width == 1100 && modes[1].Width == 900);
  // The second default-mode pass must keep the first surviving tie winner.
  auto defaults = target;
  defaults.Width = defaults.Height = 0;
  defaults.RefreshRate = {0, 0};
  FilterModesByDesc(modes, defaults);
  assert(modes.size() == 2 && modes[0].Width == 1100 && modes[1].Width == 900);

  auto fallback = target;
  fallback.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  fallback.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST;
  fallback.Scaling = DXGI_MODE_SCALING_STRETCHED;
  modes = {fallback}; // Unavailable preferences must not remove fallback modes.
  FilterModesByDesc(modes, target);
  assert(modes.size() == 1 && modes[0].Format == fallback.Format);
  modes = {fallback, stereo}; // Discover preferences before rejecting stereo.
  FilterModesByDesc(modes, target);
  assert(modes.empty());
  FilterModesByDesc(modes, target);
  assert(modes.empty());
  std::puts("DXGI_SIMPLIFICATION_PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="dxgi-simplification-") as temporary:
    binary = str(Path(temporary) / "probe")
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
                    "-Werror", "-x", "c++", "-", "-o", binary], input=source, text=True, check=True)
    subprocess.run([binary], check=True)
