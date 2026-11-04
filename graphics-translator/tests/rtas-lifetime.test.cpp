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
using Lease = std::pair<Key, uint32_t>;

static void TestBatchObservationsAndDuplicateLeases() {
  Lifetime life;
  int owner{};
  const std::array<Key, 2> keys{{{&owner, 0}, {&owner, 256}}};
  std::array<uint32_t, 2> live_tokens{}, submitted_tokens{};
  std::array<std::weak_ptr<int>, 2> live_objects{}, submitted_objects{};
  for (size_t i = 0; i < keys.size(); ++i) {
    auto object = std::make_shared<int>(int(i + 1));
    live_objects[i] = object;
    live_tokens[i] = life.Publish(keys[i], std::move(object));
    assert(life.Observe(keys[i], live_tokens[i]));
    assert(life.ReleaseLease(keys[i], live_tokens[i]).empty());
  }
  const auto submission = life.BeginSubmission();
  for (size_t i = 0; i < keys.size(); ++i) {
    auto object = std::make_shared<int>(int(i + 11));
    submitted_objects[i] = object;
    submitted_tokens[i] = life.Publish(keys[i], std::move(object));
    life.HoldLease(keys[i], submitted_tokens[i]);
    if (i == 0)
      life.HoldLease(keys[i], submitted_tokens[i]);
    assert(life.ReleaseLease(keys[i], submitted_tokens[i]).empty());
  }
  const std::array<Lease, 3> leases{{
    {keys[0], submitted_tokens[0]}, {keys[1], submitted_tokens[1]}, {keys[0], submitted_tokens[0]}
  }};
  std::array<bool, 2> observed{};
  auto released = life.CompleteSubmission(submission, leases, [&](const Key &key) {
    // Every submitted version must still be available while the remaining mirrors are read.
    for (size_t i = 0; i < keys.size(); ++i) {
      const auto *recorded = life.FindRecorded(keys[i]);
      assert(recorded && *recorded == submitted_objects[i].lock());
    }
    const size_t i = key == keys[0] ? 0 : 1;
    assert(key == keys[i]);
    observed[i] = true;
    // A different queue may have executed a different token at the same address.
    return live_tokens[i];
  });
  assert(observed[0] && observed[1]);
  assert(released.size() == keys.size());
  std::array<bool, 2> seen{};
  for (const auto &[key, payload] : released) {
    const size_t i = key == keys[0] ? 0 : 1;
    assert(key == keys[i] && !seen[i]);
    seen[i] = true;
    assert(payload == submitted_objects[i].lock());
    const auto *current = life.FindRecorded(key);
    assert(current && *current == live_objects[i].lock());
  }
  released.clear();
  assert(submitted_objects[0].expired() && submitted_objects[1].expired());
  assert(life.Sweep().empty());
}

static void TestBatchReaderAndSharedSidecar() {
  Lifetime life;
  int owner{};
  const std::array<Key, 2> keys{{{&owner, 0}, {&owner, 256}}};
  std::array<uint32_t, 2> live_tokens{};
  std::array<std::weak_ptr<int>, 2> old_objects{}, live_objects{};
  for (size_t i = 0; i < keys.size(); ++i) {
    auto object = std::make_shared<int>(int(i + 1));
    old_objects[i] = object;
    const auto token = life.Publish(keys[i], std::move(object));
    assert(life.Observe(keys[i], token));
    assert(life.ReleaseLease(keys[i], token).empty());
  }
  const auto reader = life.BeginSubmission();
  const auto writer = life.BeginSubmission();
  for (size_t i = 0; i < keys.size(); ++i) {
    auto object = i == 1 ? old_objects[i].lock() : std::make_shared<int>(11);
    live_objects[i] = object;
    live_tokens[i] = life.Publish(keys[i], std::move(object));
    life.HoldLease(keys[i], live_tokens[i]);
    assert(life.ReleaseLease(keys[i], live_tokens[i]).empty());
  }
  const std::array<Lease, 2> leases{{{keys[0], live_tokens[0]}, {keys[1], live_tokens[1]}}};
  assert(life.CompleteSubmission(writer, leases, [&](const Key &key) {
    const size_t i = key == keys[0] ? 0 : 1;
    assert(key == keys[i]);
    return live_tokens[i];
  }).empty());
  assert(!old_objects[0].expired() && !old_objects[1].expired());
  // A submission begun after the observation must not extend the old reader's barrier.
  const auto later = life.BeginSubmission();
  const auto no_mirror = [](const Key &) -> uint32_t { assert(false); return 0; };
  auto released = life.CompleteSubmission(reader, {}, no_mirror);
  assert(released.size() == keys.size());
  std::array<bool, 2> seen{};
  for (const auto &[key, payload] : released) {
    const size_t i = key == keys[0] ? 0 : 1;
    assert(key == keys[i] && !seen[i]);
    seen[i] = true;
    assert(payload == old_objects[i].lock());
    const auto *current = life.FindRecorded(key);
    assert(current && *current == live_objects[i].lock());
    assert((*current == payload) == (i == 1));
  }
  released.clear();
  assert(old_objects[0].expired() && !old_objects[1].expired());
  assert(life.CompleteSubmission(later, {}, no_mirror).empty());
  for (const auto &key : keys)
    life.ReleaseAll(key).clear();
  assert(live_objects[0].expired() && live_objects[1].expired());
}

static void TestPreviouslyLiveRecordedVersionWaitsForReader() {
  Lifetime life;
  int owner{};
  const Key key{&owner, 0};
  auto earlier_object = std::make_shared<int>(1);
  std::weak_ptr<int> earlier = earlier_object;
  const auto earlier_token = life.Publish(key, std::move(earlier_object));
  auto latest_object = std::make_shared<int>(2);
  std::weak_ptr<int> latest = latest_object;
  const auto latest_token = life.Publish(key, std::move(latest_object));
  assert(life.Observe(key, latest_token));
  const auto reader = life.BeginSubmission();
  const auto writer = life.BeginSubmission();
  life.HoldLease(key, latest_token);
  assert(life.ReleaseLease(key, latest_token).empty());
  const std::array<Lease, 1> leases{{{key, latest_token}}};
  // Regression: GPU execution order can differ from CPU publication order. The latest
  // recorded version was live, so losing live status must stamp it even though it is recorded.
  assert(life.CompleteSubmission(writer, leases, [&](const Key &observed_key) {
    assert(observed_key == key);
    return earlier_token;
  }).empty());
  assert(!latest.expired());
  assert(life.ReleaseLease(key, earlier_token).empty());
  const auto *current = life.FindRecorded(key);
  assert(current && *current == earlier.lock());
  auto released = life.CompleteSubmission(reader, {}, [](const Key &) -> uint32_t { assert(false); return 0; });
  assert(released.size() == 1 && released[0].first == key && released[0].second == latest.lock());
  released.clear();
  assert(latest.expired() && !earlier.expired());
}

static void TestBatchZeroObservation() {
  Lifetime life;
  int owner{};
  const Key key{&owner, 0};
  auto live_object = std::make_shared<int>(1);
  std::weak_ptr<int> live = live_object;
  const auto live_token = life.Publish(key, std::move(live_object));
  assert(life.Observe(key, live_token));
  assert(life.ReleaseLease(key, live_token).empty());
  const auto failed_submission = life.BeginSubmission();
  auto failed_object = std::make_shared<int>(2);
  std::weak_ptr<int> failed = failed_object;
  const auto failed_token = life.Publish(key, std::move(failed_object));
  life.HoldLease(key, failed_token);
  assert(life.ReleaseLease(key, failed_token).empty());
  const std::array<Lease, 1> leases{{{key, failed_token}}};
  auto released = life.CompleteSubmission(failed_submission, leases, [&](const Key &observed_key) -> uint32_t {
    assert(observed_key == key);
    return 0; // A zero mirror or Metal error must not replace the known live version.
  });
  assert(released.size() == 1 && released[0].first == key && released[0].second == failed.lock());
  released.clear();
  assert(failed.expired() && !live.expired());
  const auto *current = life.FindRecorded(key);
  assert(current && *current == live.lock());
  assert(life.CompleteSubmission(0, {}, [](const Key &) -> uint32_t { assert(false); return 0; }).empty());
}

static void TestRetainedPayloadBehindNewRecording() {
  Lifetime life;
  int owner{};
  const Key key{&owner, 0};
  auto object_a = std::make_shared<int>(1);
  std::weak_ptr<int> sidecar_a = object_a;
  const auto a1 = life.Publish(key, std::move(object_a));
  assert(life.Observe(key, a1));
  assert(life.ReleaseLease(key, a1).empty());
  const auto refit_submission = life.BeginSubmission();
  const auto a2 = life.Publish(key, sidecar_a.lock());
  life.HoldLease(key, a2);
  assert(life.ReleaseLease(key, a2).empty());
  auto object_b = std::make_shared<int>(2);
  std::weak_ptr<int> sidecar_b = object_b;
  const auto b = life.Publish(key, std::move(object_b));
  const std::array<Lease, 1> refit_leases{{{key, a2}}};
  auto released = life.CompleteSubmission(refit_submission, refit_leases, [&](const Key &observed_key) {
    assert(observed_key == key);
    return a2;
  });
  assert(released.size() == 1 && released[0].first == key && released[0].second == sidecar_a.lock());
  const auto *recorded = life.FindRecorded(key);
  assert(recorded && *recorded == sidecar_b.lock());
  // The newer CPU-only recording hides A2 from FindRecorded, but A2 still holds sidecar A live.
  const auto matches_a = [&](const Payload &payload) { return payload == sidecar_a.lock(); };
  assert(life.HasRetainedPayload(key, matches_a));
  released.clear();
  assert(!sidecar_a.expired());

  const auto build_submission = life.BeginSubmission();
  life.HoldLease(key, b);
  assert(life.ReleaseLease(key, b).empty());
  const std::array<Lease, 1> build_leases{{{key, b}}};
  released = life.CompleteSubmission(build_submission, build_leases, [&](const Key &observed_key) {
    assert(observed_key == key);
    return b;
  });
  assert(released.size() == 1 && released[0].first == key && released[0].second == sidecar_a.lock());
  // The returned payload still owns A here; only the tracked versions determine residency retention.
  assert(!life.HasRetainedPayload(key, matches_a));
  assert(life.HasRetainedPayload(key, [&](const Payload &payload) { return payload == sidecar_b.lock(); }));
  released.clear();
  assert(sidecar_a.expired() && !sidecar_b.expired());
}

static void TestEncodingProofDoesNotPublishOrComplete() {
  Lifetime life;
  int owner{};
  const Key key{&owner, 0};
  auto first_object = std::make_shared<int>(1);
  std::weak_ptr<int> first = first_object;
  const auto first_token = life.Publish(key, std::move(first_object));
  const auto first_submission = life.BeginSubmission();
  life.HoldLease(key, first_token);
  assert(life.ReleaseLease(key, first_token).empty());
  const auto *encoded = life.RecordEncoding(key, first_token, 101);
  assert(encoded && *encoded == first.lock());

  auto second_object = std::make_shared<int>(2);
  std::weak_ptr<int> second = second_object;
  const auto second_token = life.Publish(key, std::move(second_object));
  const auto *recorded = life.FindRecorded(key);
  assert(recorded && *recorded == second.lock());
  encoded = life.FindEncoded(key, 101);
  assert(encoded && *encoded == first.lock()); // CPU publication did not execute the new version.
  const auto second_submission = life.BeginSubmission();
  life.HoldLease(key, second_token);
  assert(life.ReleaseLease(key, second_token).empty());
  encoded = life.RecordEncoding(key, second_token, 101);
  assert(encoded && *encoded == second.lock());

  const std::array<Lease, 1> first_leases{{{key, first_token}}};
  assert(life.CompleteSubmission(first_submission, first_leases, [&](const Key &observed_key) {
    assert(observed_key == key);
    return first_token;
  }).empty());
  // Completion of an earlier GPU writer must not replace the later FIFO encoding proof.
  encoded = life.FindEncoded(key, 101);
  assert(encoded && *encoded == second.lock());
  const std::array<Lease, 1> second_leases{{{key, second_token}}};
  auto released = life.CompleteSubmission(second_submission, second_leases, [](const Key &) -> uint32_t {
    return 0;
  });
  // Encoding alone never made the second version GPU-live, so failed execution can release it.
  assert(released.size() == 1 && released[0].first == key && released[0].second == second.lock());
  assert(!life.FindEncoded(key, 101));
  recorded = life.FindRecorded(key);
  assert(recorded && *recorded == first.lock());
  released.clear();
  assert(second.expired() && !first.expired());
}

static void TestEncodingProofReplayAndInvalidation() {
  Lifetime life;
  int owner{};
  const Key key{&owner, 0}, other_key{&owner, 256};
  const auto token = life.Publish(key, std::make_shared<int>(1));
  assert(life.Observe(key, token));
  const auto *encoded = life.RecordEncoding(key, token, 101);
  assert(encoded && **encoded == 1);
  const auto unsubmitted = life.Publish(key, std::make_shared<int>(2));
  assert(**life.FindRecorded(key) == 2);
  assert(**life.FindEncoded(key, 101) == 1);

  // Replaying the exact same token on another queue replaces the queue identity too.
  encoded = life.RecordEncoding(key, token, 202);
  assert(encoded && **encoded == 1);
  assert(!life.FindEncoded(key, 101) && !life.FindEncoded(key, 0));
  assert(**life.FindEncoded(key, 202) == 1);
  assert(life.Observe(key, token));
  assert(**life.FindEncoded(key, 202) == 1);
  assert(!life.RecordEncoding(other_key, token, 202));
  assert(**life.FindEncoded(key, 202) == 1); // Unknown addresses cannot affect this address.

  assert(!life.RecordEncoding(key, UINT32_MAX, 202));
  assert(!life.FindEncoded(key, 101) && !life.FindEncoded(key, 202));
  assert(**life.FindRecorded(key) == 2);
  assert(life.RecordEncoding(key, token, 101));
  assert(!life.RecordEncoding(key, 0, 101));
  assert(!life.FindEncoded(key, 101));
  assert(life.RecordEncoding(key, token, 101));
  assert(!life.RecordEncoding(key, token, 0));
  assert(!life.FindEncoded(key, 101) && !life.FindEncoded(key, 0));
  auto released = life.ReleaseLease(key, unsubmitted);
  assert(released.size() == 1 && *released[0] == 2);
  assert(**life.FindRecorded(key) == 1); // Proof invalidation did not change GPU authority.
}

static void TestEncodingProofRetirementAndSlotReuse() {
  Lifetime life;
  int owner{};
  const Key key{&owner, 0};
  const auto abandoned = life.Publish(key, std::make_shared<int>(1));
  assert(life.RecordEncoding(key, abandoned, 101));
  auto released_leases = life.ReleaseLease(key, abandoned);
  assert(released_leases.size() == 1 && *released_leases[0] == 1);
  assert(!life.FindEncoded(key, 101)); // Encoding does not take an ownership lease.

  const auto earlier = life.Publish(key, std::make_shared<int>(2));
  const auto latest = life.Publish(key, std::make_shared<int>(3));
  assert(life.Observe(key, latest));
  assert(life.RecordEncoding(key, latest, 101));
  const auto reader = life.BeginSubmission();
  assert(life.Observe(key, earlier));
  assert(life.ReleaseLease(key, latest).empty());
  assert(**life.FindEncoded(key, 101) == 3); // A stamped version is still retained for the reader.
  life.RetireSubmission(reader);
  auto swept = life.Sweep();
  assert(swept.size() == 1 && swept[0].first == key && *swept[0].second == 3);
  assert(!life.FindEncoded(key, 101));
  assert(life.ReleaseLease(key, earlier).empty());

  assert(life.RecordEncoding(key, earlier, 101));
  auto all = life.ReleaseAll(key);
  assert(all.size() == 1 && *all[0] == 2);
  assert(!life.FindEncoded(key, 101));
  const auto replacement = life.Publish(key, std::make_shared<int>(4));
  assert(!life.FindEncoded(key, 101) && !life.FindEncoded(key, 202));
  assert(life.RecordEncoding(key, replacement, 202));
  assert(**life.FindEncoded(key, 202) == 4);
  assert(!life.RecordEncoding(key, earlier, 101));
  assert(!life.FindEncoded(key, 202)); // A reused address cannot revive a released version's proof.
  assert(**life.FindRecorded(key) == 4);
}

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
  TestBatchObservationsAndDuplicateLeases();
  TestBatchReaderAndSharedSidecar();
  TestPreviouslyLiveRecordedVersionWaitsForReader();
  TestBatchZeroObservation();
  TestRetainedPayloadBehindNewRecording();
  TestEncodingProofDoesNotPublishOrComplete();
  TestEncodingProofReplayAndInvalidation();
  TestEncodingProofRetirementAndSlotReuse();
  std::puts("PASS: keyed RTAS release, multiple offsets/owners, refit/live preservation, abandoned recordings, "
            "batch observations/duplicate leases, authoritative tokens, zero/error observations, "
            "out-of-order retirement, previously live recorded versions, hidden live sidecars and "
            "queue-specific execution proof publication/replay/invalidation/retirement");
}
