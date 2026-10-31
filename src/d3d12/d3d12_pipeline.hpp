/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once
#include "airconv_public.h"
#include <cstring>

namespace dxmt {

/* A compiler-owned pointer as seen by this image, or null when it lies outside a 32-bit PE's address space. */
inline const void *
SM50AccessiblePointer(sm50_ptr64_t ptr) {
#if defined(__LP64__) || defined(_WIN64)
  return ptr;
#else
  return ptr.impl >> 32 ? nullptr : reinterpret_cast<const void *>(uintptr_t(ptr.impl));
#endif
}

/**
 * Copies one compiled-bitcode part into caller memory. A directly addressable source (every 64-bit
 * image, or a cache blob) is copied in place; a WoW64 image asks the compiler to copy a part whose
 * native address is outside its address space, which needs the owning bitcode handle.
 */
inline bool
SM50ReadCompiledBitcode(
    sm50_bitcode_t bitcode, SM50_COMPILED_BITCODE_PART part, sm50_ptr64_t source, void *destination, uint64_t size
) {
  if (!size)
    return true;
  if (const void *direct = SM50AccessiblePointer(source)) {
    std::memcpy(destination, direct, size);
    return true;
  }
  return bitcode && !SM50CopyCompiledBitcode(bitcode, part, destination, size);
}

class SM50Shader {
  sm50_shader_t sm50_shader_{};

public:
  sm50_shader_t *
  operator&() {
    return &sm50_shader_;
  }

  operator sm50_shader_t() {
    return sm50_shader_;
  }

  ~SM50Shader() {
    if (sm50_shader_)
      SM50Destroy(sm50_shader_);
  }
};

class SM50ShaderBitcode {
  sm50_bitcode_t sm50_bitcode_{};

public:
  sm50_bitcode_t *
  operator&() {
    return &sm50_bitcode_;
  }

  operator sm50_bitcode_t() {
    return sm50_bitcode_;
  }

  ~SM50ShaderBitcode() {
    if (sm50_bitcode_)
      SM50DestroyBitcode(sm50_bitcode_);
  }
};

class SM50Error {
  sm50_error_t sm50_error_{};

public:
  sm50_error_t *
  operator&() {
    return &sm50_error_;
  }

  operator sm50_error_t() const {
    return sm50_error_;
  }

  ~SM50Error() {
    if (sm50_error_)
      SM50FreeError(sm50_error_);
  }
};

} // namespace dxmt