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

#include <atomic>
#include "unknwn.h"
#include "../util_likely.hpp"

namespace d3dalloy {

template <typename T> class NoWrapper : public T {

public:
  virtual ~NoWrapper() {}
};

/**
 * \brief Reference-counted COM object
 *
 * This can serve as a templated base class for most
 * COM objects. It implements AddRef and Release from
 * \c IUnknown, and provides methods to increment and
 * decrement private references which are not visible
 * to the application.
 *
 * Having two reference counters is sadly necessary
 * in order to not break games which steal internal
 * references if the refefence count of an object is
 + greater than they expect. DXVK sometimes requires
 * holding on to objects which the application wants
 * to delete.
 */
template <typename... Base> class ComObject : public Base... {

public:
  virtual ~ComObject() {}

  ULONG STDMETHODCALLTYPE AddRef() {
    uint32_t refCount = m_refCount++;
    if (unlikely(!refCount))
      AddRefPrivate();
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() {
    uint32_t refCount = --m_refCount;
    if (unlikely(!refCount))
      ReleasePrivate();
    return refCount;
  }

  void AddRefPrivate() { ++m_refPrivate; }

  bool TryAddRefPrivate() {
    uint32_t count = m_refPrivate.load(std::memory_order_acquire);
    while (count && count < 0x7fffffff) {
      if (m_refPrivate.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel,
                                             std::memory_order_acquire))
        return true;
    }
    return false;
  }

  void ReleasePrivate() {
    uint32_t refPrivate = --m_refPrivate;
    if (unlikely(!refPrivate)) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }

  ULONG GetPrivateRefCount() { return m_refPrivate.load(); }

protected:
  std::atomic<uint32_t> m_refCount = {0ul};
  std::atomic<uint32_t> m_refPrivate = {0ul};
};

template <typename... Base> class ComObjectWithInitialRef : public Base... {

public:
  virtual ~ComObjectWithInitialRef() {}

  ULONG STDMETHODCALLTYPE AddRef() {
    uint32_t refCount = m_refCount++;
    if (unlikely(!refCount))
      AddRefPrivate();
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() {
    uint32_t refCount = --m_refCount;
    if (unlikely(!refCount))
      ReleasePrivate();
    return refCount;
  }

  void AddRefPrivate() { ++m_refPrivate; }

  void ReleasePrivate() {
    uint32_t refPrivate = --m_refPrivate;
    if (unlikely(!refPrivate)) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }

protected:
  std::atomic<uint32_t> m_refCount = {1ul};
  std::atomic<uint32_t> m_refPrivate = {1ul};
};
template <typename T> inline void InitReturnPtr(T **ptr) {
  if (ptr != nullptr)
    *ptr = nullptr;
}

} // namespace d3dalloy
