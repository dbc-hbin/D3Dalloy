// SPDX-License-Identifier: LGPL-2.1-or-later
// clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all
//   -fno-omit-frame-pointer -Isrc/d3d12 graphics-translator/tests/rtas-lifetime.test.cpp
//   -o build/rtas-lifetime.test && build/rtas-lifetime.test
#include "owned_rtas_lifetime.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <memory>

struct Key {
  const void *allocationIdentity;
  uint64_t offset;
  bool operator==(const Key &) const = default;
};
struct Hash {
  size_t operator()(const Key &key) const noexcept {
    return std::hash<const void *>{}(key.allocationIdentity) ^ (std::hash<uint64_t>{}(key.offset) << 1);
  }
};
using Payload = std::shared_ptr<int>;
using Lifetime = owned_graphics::RTASLifetime<Key, Payload, Hash>;

int main() {
  Lifetime life;
  int owners[2]{};
  const std::array<Key, 3> keys{{{&owners[0], 0}, {&owners[0], 256}, {&owners[1], 0}}};
  std::array<uint32_t, 3> old_tokens{}, live_tokens{};
  std::array<std::weak_ptr<int>, 3> old_objects{}, live_objects{};
  for (size_t i = 0; i < keys.size(); ++i) {
    auto object = std::make_shared<int>(int(i + 1));
    old_objects[i] = object;
    old_tokens[i] = life.Publish(keys[i], std::move(object));
    assert(life.Observe(keys[i], old_tokens[i]));
    assert(life.ReleaseLease(keys[i], old_tokens[i]).empty());
  }

  const auto slow = life.BeginSubmission();
  const auto fast = life.BeginSubmission();
  for (size_t i = 0; i < keys.size(); ++i) {
    // Only the same-address refit shares its sidecar; other keys have distinct objects.
    auto object = i == 0 ? old_objects[i].lock() : std::make_shared<int>(int(i + 11));
    live_objects[i] = object;
    live_tokens[i] = life.Publish(keys[i], std::move(object));
    life.HoldLease(keys[i], live_tokens[i]);
    assert(life.Observe(keys[i], live_tokens[i]));
    assert(life.ReleaseLease(keys[i], live_tokens[i]).empty());
    assert(life.ReleaseLease(keys[i], live_tokens[i]).empty());
  }
  life.RetireSubmission(fast);
  assert(life.Sweep().empty());
  for (const auto &object : old_objects)
    assert(!object.expired());
  life.RetireSubmission(slow);
  auto released = life.Sweep();
  assert(released.size() == keys.size());
  std::array<bool, 3> seen{};
  for (const auto &[key, payload] : released) {
    size_t i = 0;
    while (i < keys.size() && !(key == keys[i]))
      ++i;
    assert(i < keys.size() && !seen[i]);
    seen[i] = true;
    assert(payload == old_objects[i].lock() && *payload == int(i + 1));
    const auto *current = life.FindRecorded(key);
    assert(current && *current == live_objects[i].lock());
    // The keyed result keeps the same-address refit resident without consulting other keys.
    assert((*current == payload) == (i == 0));
  }
  released.clear();
  assert(!old_objects[0].expired());
  assert(old_objects[1].expired() && old_objects[2].expired());
  assert(life.Sweep().empty());

  auto abandoned_object = std::make_shared<int>(99);
  std::weak_ptr<int> abandoned_weak = abandoned_object;
  const auto abandoned = life.Publish(keys[0], std::move(abandoned_object));
  std::vector<Payload> lease_released = life.ReleaseLease(keys[0], abandoned);
  assert(lease_released.size() == 1 && *lease_released[0] == 99);
  lease_released.clear();
  assert(abandoned_weak.expired());
  assert(*life.FindRecorded(keys[0]) == live_objects[0].lock());

  const auto abandoned_refit = life.Publish(keys[0], live_objects[0].lock());
  lease_released = life.ReleaseLease(keys[0], abandoned_refit);
  assert(lease_released.size() == 1 && lease_released[0] == *life.FindRecorded(keys[0]));
  lease_released.clear();
  assert(!live_objects[0].expired());

  std::vector<Payload> all = life.ReleaseAll(keys[0]);
  assert(all.size() == 1 && !life.FindRecorded(keys[0]));
  assert(*life.FindRecorded(keys[1]) == live_objects[1].lock());
  assert(*life.FindRecorded(keys[2]) == live_objects[2].lock());
  all.clear();
  assert(live_objects[0].expired());
  assert(!live_objects[1].expired() && !live_objects[2].expired());
  life.ReleaseAll(keys[1]).clear();
  assert(live_objects[1].expired() && !live_objects[2].expired());
  life.ReleaseAll(keys[2]).clear();
  assert(live_objects[2].expired());
  std::puts("PASS: keyed RTAS release, multiple offsets/owners, refit/live preservation, abandoned recordings, out-of-order retirement");
}
