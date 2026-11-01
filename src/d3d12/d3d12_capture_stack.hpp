// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace d3dalloy {

// Called only after an opt-in capture has reserved its bounded output slot.
__declspec(noinline) inline std::string
Win32CaptureStackBackTrace() {
  std::array<void *, 48> frames{};
  const USHORT count = CaptureStackBackTrace(1, frames.size(), frames.data(), nullptr);
  std::ostringstream text;
  text << "schema 1\nprocess " << GetCurrentProcessId() << "\nthread " << GetCurrentThreadId()
       << "\nframe_limit " << frames.size() << "\nframes " << count << '\n';
  for (USHORT i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    std::array<char, 4096> path{};
    const bool resolved = GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(frames[i]), &module);
    const DWORD length = resolved ? GetModuleFileNameA(module, path.data(), path.size()) : 0;
    text << "frame " << i << " address 0x" << std::hex << reinterpret_cast<uintptr_t>(frames[i])
         << " module_base 0x" << reinterpret_cast<uintptr_t>(module) << " relative_offset 0x"
         << (resolved ? reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module) : 0)
         << std::dec << " resolved " << resolved << " path_truncated " << (length == path.size())
         << " path " << std::quoted(std::string(path.data(), length < path.size() ? length : path.size())) << '\n';
  }
  return text.str();
}

} // namespace d3dalloy
