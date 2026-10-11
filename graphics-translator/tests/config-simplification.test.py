#!/usr/bin/env python3
# Run: python3 graphics-translator/tests/config-simplification.test.py
# Compile the production integer parser unchanged and exercise its boundaries.
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = """
#include <cstdint>
#include <limits>
#include <string>
class Config {
public:
  static bool parseOptionValue(const std::string& value, int32_t& result);
};
"""
implementation = (ROOT / "src/util/config/config.cpp").read_text()
source += re.search(
    r"bool Config::parseOptionValue\(\n    const std::string&  value,\n          int32_t&      result\) \{.*?\n  \}",
    implementation, re.S).group(0)
source += r'''
int main() {
  int32_t result = 0;
  if (!Config::parseOptionValue("0", result) || result != 0) return 1;
  if (!Config::parseOptionValue("-0", result) || result != 0) return 1;
  if (!Config::parseOptionValue("2147483647", result) || result != 2147483647) return 1;
  if (!Config::parseOptionValue("-2147483648", result) || result != INT32_MIN) return 1;

  for (const char* value : {"2147483648", "-2147483649", "999999999999999999", "-"})
    if (Config::parseOptionValue(value, result)) return 1;
}
'''
with tempfile.TemporaryDirectory(prefix="config-simplification-") as temporary:
    binary = str(Path(temporary) / "probe")
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
                    "-Werror", "-x", "c++", "-", "-o", binary], input=source, text=True, check=True)
    subprocess.run([binary], check=True)
