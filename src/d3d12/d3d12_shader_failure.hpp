// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "airconv_public.h"
#include "d3d12_pipeline.hpp"
#include "d3d12_capture_stack.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace d3dalloy {

// Only the initial VS/PS/CS SM50Compile calls use this recorder. The bytecode
// retains the input signature; IA elements below are the exact extracted Metal inputs.
inline bool
RecordShaderCompileFailure(const char *stage, D3D12_SHADER_BYTECODE shader,
                           const SM50_SHADER_ROOT_SIGNATURE_DATA &root,
                           const SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                           const char *entry, const std::string &error, bool explicit_root,
                           const Sha1Digest *stage_key = nullptr) noexcept {
  // Function-local statics in this inline function are shared across translation units.
  static std::mutex mutex;
  static std::array<Sha1Digest, 64> identities{};
  static size_t count = 0;
  try {
    static const std::string directory = env::getEnvVar("D3DALLOY_SHADER_FAILURE_PATH");
    // ponytail: serialize at most 64 failed pairs; a queue only helps if failure recording becomes hot.
    std::lock_guard<std::mutex> lock(mutex);
    if (count == identities.size()) return false;
    Sha1HashState hash;
    hash.update(stage, std::strlen(stage));
    if (directory.empty()) {
      // Production does not need a replay capture. Use the existing stage key so
      // deduplication does not hash full shader/root blobs on every failed PSO.
      if (stage_key) hash.update(*stage_key);
    } else {
      hash.update(shader.BytecodeLength);
      hash.update(shader.pShaderBytecode, shader.BytecodeLength);
      hash.update(root.bytecode_length);
      hash.update(root.bytecode, root.bytecode_length);
      if (stage_key) hash.update(*stage_key);
    }
    const auto identity = hash.final();
    for (size_t i = 0; i < count; ++i)
      if (identities[i] == identity) return false;
    // Failed saves also consume a slot: broken paths must not cause unbounded retries/logs.
    identities[count++] = identity;
    if (directory.empty()) return true;

    std::ostringstream metadata;
    metadata << "schema 1\nstage " << stage << "\nentry " << std::quoted(entry)
             << "\nroot_source " << (explicit_root ? "explicit" : "embedded_shader")
             << "\nshader_bytes " << shader.BytecodeLength << "\nroot_bytes " << root.bytecode_length
             << "\nshader_sha1 " << Sha1HashState::compute(shader.pShaderBytecode, shader.BytecodeLength).string()
             << "\nroot_sha1 " << Sha1HashState::compute(root.bytecode, root.bytecode_length).string();
    if (stage_key) metadata << "\nstage_key_sha1 " << stage_key->string();
    metadata << '\n';
    // Each argument line is in original next-chain order. Values are decimal,
    // named after the public SM50 structs; quoted strings use std::quoted escaping.
    for (auto *arg = args; arg; arg = static_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(arg->next)) {
      metadata << "argument " << uint32_t(arg->type) << '\n';
      switch (arg->type) {
      case SM50_SHADER_ROOT_SIGNATURE:
        break; // Exact bytes are in rootsig.bin (including the container for embedded roots).
      case SM50_SHADER_COMMON: {
        const auto &data = *reinterpret_cast<const SM50_SHADER_COMMON_DATA *>(arg);
        metadata << "metal_version " << uint32_t(data.metal_version) << "\nflags " << uint32_t(data.flags) << '\n';
        break;
      }
      case SM50_SHADER_IA_INPUT_LAYOUT: {
        const auto &data = *reinterpret_cast<const SM50_SHADER_IA_INPUT_LAYOUT_DATA *>(arg);
        metadata << "index_buffer_format " << uint32_t(data.index_buffer_format) << "\nslot_mask " << data.slot_mask
                 << "\nnum_elements " << data.num_elements << '\n';
        for (uint32_t i = 0; i < data.num_elements; ++i) {
          const auto &element = data.elements[i];
          metadata << "element " << element.reg << ' ' << element.slot << ' ' << element.aligned_byte_offset << ' '
                   << element.format << ' ' << element.step_function << ' ' << element.step_rate << '\n';
        }
        break;
      }
      case SM50_SHADER_PSO_PIXEL_SHADER: {
        const auto &data = *reinterpret_cast<const SM50_SHADER_PSO_PIXEL_SHADER_DATA *>(arg);
        metadata << "sample_mask " << data.sample_mask << "\ndual_source_blending " << data.dual_source_blending
                 << "\ndisable_depth_output " << data.disable_depth_output << "\nunorm_output_reg_mask "
                 << data.unorm_output_reg_mask << "\nnative_sample_count " << data.native_sample_count << "\npixel_formats";
        for (auto format : data.pixel_formats) metadata << ' ' << format;
        metadata << '\n';
        break;
      }
      case SM50_SHADER_GS_PASS_THROUGH: {
        const auto &data = *reinterpret_cast<const SM50_SHADER_GS_PASS_THROUGH_DATA *>(arg);
        metadata << "DataEncoded " << data.DataEncoded << "\nRasterizationDisabled " << data.RasterizationDisabled << '\n';
        break;
      }
      case SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE: {
        const auto &data = *reinterpret_cast<const SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *>(arg);
        metadata << "record_dwords " << data.record_dwords << "\nnum_elements " << data.num_elements << '\n';
        for (uint32_t i = 0; i < data.num_elements; ++i) {
          const auto &element = data.elements[i];
          metadata << "capture_element " << std::quoted(element.semantic_name) << ' ' << element.semantic_index << ' '
                   << element.start_component << ' ' << element.component_count << ' ' << element.dword_offset << '\n';
        }
        break;
      }
      case SM50_SHADER_D3D12_LOGIC_OP: {
        const auto &data = *reinterpret_cast<const SM50_SHADER_D3D12_LOGIC_OP_DATA *>(arg);
        metadata << "op " << data.op << "\nrt_mask " << data.rt_mask << "\ncomponent_bits";
        for (const auto &target : data.component_bits)
          for (auto bits : target) metadata << ' ' << unsigned(bits);
        metadata << '\n';
        break;
      }
      case SM50_SHADER_D3D12_ATOMIC64_PHASE:
        break; // Header-only argument.
      default:
        metadata << "unsupported_argument 1\n"; // Never claim an incomplete capture is replay-equivalent.
        break;
      }
    }
    const auto text = metadata.str();
    const std::string base = directory + "\\" + std::to_string(GetCurrentProcessId()) + "-" + stage + "-" + identity.string();
    const std::string temporary = base + ".tmp";
    const std::string destination = base + ".failure";
    const auto stack = Win32CaptureStackBackTrace();
    const std::array<std::string, 5> paths = {temporary + "\\shader.cso", temporary + "\\rootsig.bin",
                                              temporary + "\\metadata.txt", temporary + "\\error.txt",
                                              temporary + "\\stack.txt"};
    if (!CreateDirectoryA(temporary.c_str(), nullptr)) {
      WARN("D3D12 shader failure capture directory creation failed: error=", GetLastError());
      return true;
    }
    // Publish the entire directory only after every file closes successfully.
    const void *data[] = {shader.pShaderBytecode, root.bytecode, text.data(), error.data(), stack.data()};
    const size_t sizes[] = {shader.BytecodeLength, root.bytecode_length, text.size(), error.size(), stack.size()};
    bool saved = true;
    DWORD save_error = ERROR_SUCCESS;
    for (size_t i = 0; saved && i < paths.size(); ++i) {
      HANDLE file = CreateFileA(paths[i].c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        saved = false;
        save_error = GetLastError();
        break;
      }
      size_t offset = 0;
      while (saved && offset < sizes[i]) {
        const DWORD bytes = DWORD(std::min<size_t>(sizes[i] - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, static_cast<const char *>(data[i]) + offset, bytes, &written, nullptr) || written != bytes) {
          saved = false;
          save_error = GetLastError();
          if (save_error == ERROR_SUCCESS) save_error = ERROR_WRITE_FAULT;
        }
        offset += written;
      }
      if (saved && !FlushFileBuffers(file)) { saved = false; save_error = GetLastError(); }
      if (!CloseHandle(file) && saved) { saved = false; save_error = GetLastError(); }
    }
    if (saved && !MoveFileExA(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
      saved = false;
      save_error = GetLastError();
    }
    if (!saved) {
      for (const auto &path : paths) DeleteFileA(path.c_str());
      RemoveDirectoryA(temporary.c_str());
      WARN("D3D12 shader failure capture save failed: error=", save_error);
    } else {
      Logger::info(str::format("D3D12 shader failure capture saved: stage=", stage, " pair=", identity.string()));
    }
  } catch (...) {
    // Observation must not alter the compiler's original failure result.
    static std::atomic_uint32_t exceptions{0};
    if (exceptions.fetch_add(1, std::memory_order_relaxed) < 64)
      WARN("D3D12 shader failure capture failed: exception");
  }
  return true;
}

} // namespace d3dalloy
