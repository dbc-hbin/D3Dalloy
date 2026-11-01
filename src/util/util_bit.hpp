/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#if (defined(__x86_64__) && !defined(__arm64ec__)) || (defined(_M_X64) && !defined(_M_ARM64EC)) \
    || defined(__i386__) ||  defined(_M_IX86)
#define D3DALLOY_ARCH_X86
#if defined(__x86_64__) || defined(_M_X64)
#define D3DALLOY_ARCH_X86_64
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#define D3DALLOY_ARCH_ARM64
#else
#error "Unknown CPU Architecture"
#endif

#include <stddef.h>
#include "util_math.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace d3dalloy::bit {

inline uint32_t popcnt(uint32_t n) {
  return std::popcount(n);
}

inline uint32_t tzcnt(uint32_t n) {
  return std::countr_zero(n);
}

inline uint32_t tzcnt(uint64_t n) {
  return std::countr_zero(n);
}

inline uint32_t lzcnt(uint32_t n) {
  return std::countl_zero(n);
}


/** Extracts bit range from bit field */
template<typename T>
T bextract(T op, uint32_t first, uint32_t count) {
  if (!count)
    return 0;

  T mask = (T(2) << (count - 1)) - T(1);
  return (op >> first) & mask;
}

template <size_t Bits> class bitset {
  static constexpr size_t Qwords = align(Bits, 64) / 64;

public:
  constexpr bitset() : m_qwords() {}

  constexpr bool get(uint64_t idx) const {
    uint64_t qword = 0;
    uint64_t bit = idx;

    // Compiler doesn't remove this otherwise.
    if constexpr (Qwords > 1) {
      qword = idx / 64;
      bit = idx % 64;
    }

    return m_qwords[qword] & (1uLL << bit);
  }

  constexpr void set(uint64_t idx, bool value) {
    uint64_t qword = 0;
    uint64_t bit = idx;

    // Compiler doesn't remove this otherwise.
    if constexpr (Qwords > 1) {
      qword = idx / 64;
      bit = idx % 64;
    }

    if (value)
      m_qwords[qword] |= 1uLL << bit;
    else
      m_qwords[qword] &= ~(1uLL << bit);
  }

  constexpr bool exchange(uint64_t idx, bool value) {
    bool oldValue = get(idx);
    set(idx, value);
    return oldValue;
  }

  constexpr void flip(uint64_t idx) {
    uint64_t dword = 0;
    uint64_t bit = idx;

    // Compiler doesn't remove this otherwise.
    if constexpr (Qwords > 1) {
      dword = idx / 64;
      bit = idx % 64;
    }

    m_qwords[dword] ^= 1uLL << bit;
  }

  constexpr void setAll() {
    if constexpr (Bits % 64 == 0) {
      for (size_t i = 0; i < Qwords; i++)
        m_qwords[i] = std::numeric_limits<uint64_t>::max();
    } else {
      for (size_t i = 0; i < Qwords - 1; i++)
        m_qwords[i] = std::numeric_limits<uint64_t>::max();

      m_qwords[Qwords - 1] = (1uLL << (Bits % 64)) - 1;
    }
  }

  constexpr void clearAll() {
    for (size_t i = 0; i < Qwords; i++)
      m_qwords[i] = 0;
  }

  constexpr void clearMask(uint32_t mask) {
    m_qwords[0] &= ~((uint64_t)mask);
  }

  constexpr bool any() const {
    for (size_t i = 0; i < Qwords; i++) {
      if (m_qwords[i] != 0)
        return true;
    }

    return false;
  }

  constexpr uint64_t &qword(uint64_t idx) { return m_qwords[idx]; }

  constexpr uint64_t qword(uint64_t idx) const { return m_qwords[idx]; }

  constexpr size_t bitCount() { return Bits; }

  constexpr size_t qwordCount() { return Qwords; }

  constexpr bool operator[](uint64_t idx) const { return get(idx); }

  constexpr void setN(uint64_t bits) {
    uint64_t fullqwords = bits / 64;
    uint64_t offset = bits % 64;

    for (size_t i = 0; i < fullqwords; i++)
      m_qwords[i] = std::numeric_limits<uint64_t>::max();

    if (offset > 0)
      m_qwords[fullqwords] = (1uLL << offset) - 1;
  }

private:
  uint64_t m_qwords[Qwords];
};

} // namespace d3dalloy::bit
