// SPDX-License-Identifier: LGPL-2.1-or-later
// clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined
//   -fno-sanitize-recover=all -Isrc/d3d12 graphics-translator/tests/rtas-size-cache.test.cpp
//   -o build/rtas-size-cache.test && build/rtas-size-cache.test
#include "owned_rtas_size_cache.hpp"

#include <cassert>
#include <cstdio>

using owned_graphics::RTASSizeCache;
using owned_graphics::RTASSizeKey;
using owned_graphics::RTASSizes;

int main() {
  RTASSizeCache cache;
  RTASSizeKey triangles{1, 1, 1, 0,
      {{0, 1, 32, 12, 30, 42, false, true, true, false}, {0, 1, 48, 12, 30, 42, false, true, true, false}}};
  const RTASSizes expected{4096, 2048, 0};
  RTASSizes result{99, 99, 99};
  assert(!cache.Find(triangles, result));
  assert((result == RTASSizes{99, 99, 99}));
  assert(cache.Insert(triangles, expected));
  assert(cache.Find(triangles, result) && result == expected);

  // Equal totals are insufficient: geometry order, flags, formats, stride,
  // transform presence and primitive counts are distinct native descriptors.
  auto miss_after = [&](auto change) {
    auto changed = triangles;
    change(changed);
    assert(!cache.Find(changed, result));
  };
  miss_after([](auto &key) { ++key.type; });
  miss_after([](auto &key) { ++key.flags; });
  miss_after([](auto &key) { ++key.metalUsage; });
  miss_after([](auto &key) { ++key.instanceCount; });
  miss_after([](auto &key) { ++key.geometries[0].type; });
  miss_after([](auto &key) { ++key.geometries[0].flags; });
  miss_after([](auto &key) { ++key.geometries[0].primitiveCount; --key.geometries[1].primitiveCount; });
  miss_after([](auto &key) { key.geometries[0].primitiveCount += uint64_t(1) << 32; });
  miss_after([](auto &key) { ++key.geometries[0].stride; });
  miss_after([](auto &key) { ++key.geometries[0].vertexFormat; });
  miss_after([](auto &key) { ++key.geometries[0].indexFormat; });
  miss_after([](auto &key) { key.geometries[0].hasTransform = true; });
  // A prebuild query with unresolved bindings must not supply sizes for a
  // descriptor with native index/transform buffers merely because formats match.
  miss_after([](auto &key) { key.geometries[0].hasVertexBuffer = false; });
  miss_after([](auto &key) { key.geometries[0].hasIndexBuffer = false; });
  miss_after([](auto &key) { key.geometries[0].hasTransformBuffer = true; });
  miss_after([](auto &key) { std::swap(key.geometries[0], key.geometries[1]); });
  miss_after([](auto &key) { key.geometries.pop_back(); });

  // Cached keys are owned snapshots, not aliases of a caller's reusable key.
  auto original = triangles;
  triangles.geometries.clear();
  assert(cache.Find(original, result) && result == expected);
  assert(!cache.Find(triangles, result));

  RTASSizeKey empty_tlas{}, empty_blas{1, 0, 0, 0, {}};
  assert(cache.Insert(empty_tlas, {256, 256, 0}));
  assert(cache.Find(empty_tlas, result) && (result == RTASSizes{256, 256, 0}));
  assert(!cache.Find(empty_blas, result));

  // A failed native query is not cached. Zero refit scratch remains a valid hit.
  assert(!cache.Insert(empty_blas, {0, 256, 0}));
  assert(!cache.Insert(empty_blas, {256, 0, 0}));
  assert(!cache.Find(empty_blas, result));
  assert(cache.Insert(empty_blas, {512, 256, 0}));
  assert(cache.Find(empty_blas, result) && (result == RTASSizes{512, 256, 0}));

  auto large = original;
  large.geometries.resize(RTASSizeCache::kMaxGeometries);
  auto at_limit = large;
  assert(cache.Insert(std::move(large), expected));
  assert(cache.Find(at_limit, result) && result == expected);
  large = at_limit;
  large.geometries.push_back({});
  assert(!cache.Insert(large, expected));
  assert(!cache.Find(large, result));
  large = original;
  large.geometries.reserve(RTASSizeCache::kMaxGeometries + 1);
  assert(!cache.Insert(std::move(large), expected));

  // Capacity is bounded and eviction changes only the need for a new query.
  RTASSizeCache bounded;
  for (uint32_t i = 0; i <= RTASSizeCache::kCapacity; ++i) {
    RTASSizeKey key{0, 0, 0, i, {}};
    assert(bounded.Insert(std::move(key), {1024 + i, 512 + i, 0}));
  }
  assert(!bounded.Find(RTASSizeKey{}, result));
  for (uint32_t i = 1; i <= RTASSizeCache::kCapacity; ++i) {
    assert(bounded.Find(RTASSizeKey{0, 0, 0, i, {}}, result));
    assert((result == RTASSizes{1024 + i, 512 + i, 0}));
  }
  RTASSizeCache other_device;
  assert(!other_device.Find(original, result));
  std::puts("PASS: exact RTAS shapes, owned keys, zero refit scratch, retryable failures, bounded eviction and device isolation");
}
