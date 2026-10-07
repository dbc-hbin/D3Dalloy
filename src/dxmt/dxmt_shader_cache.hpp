#pragma once
#include "Metal.hpp"
#include "thread.hpp"
#include "airconv_public.h"

namespace dxmt {

constexpr int kDXMTShaderCacheVersion = AIRCONV_VERSION;

class ShaderCache {
public:
  static ShaderCache &getInstance(WMTMetalVersion version);

  template <typename K>
  void
  set(const K &key, WMT::DispatchData data) {
    if (!scache_writer_)
      return;
    std::lock_guard<dxmt::mutex> lock(scache_writer_mutex_);
    scache_writer_.set(key, data);
  }

  template <typename K>
  WMT::Reference<WMT::DispatchData>
  get(const K &key) {
    if (!scache_reader_)
      return {};
    std::lock_guard<dxmt::mutex> lock(scache_reader_mutex_);
    return scache_reader_.get(key);
  }

  ShaderCache(WMTMetalVersion metal_version);
  ShaderCache(const ShaderCache &copy) = delete;

private:
  WMT::Reference<WMT::CacheWriter> scache_writer_;
  dxmt::mutex scache_writer_mutex_;

  WMT::Reference<WMT::CacheReader> scache_reader_;
  dxmt::mutex scache_reader_mutex_;
};

} // namespace dxmt
