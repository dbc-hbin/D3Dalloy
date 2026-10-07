// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned graphics backend: GPU-authoritative lifetime of acceleration-structure versions.
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace owned_graphics {

/*
 * Which version of an acceleration structure the GPU actually holds, and which versions an
 * in-flight submission may still read.
 *
 * A D3D12 acceleration-structure address is stable, but the native structure behind it changes
 * every time a build executes. The CPU cannot infer when that happens: two queues may encode B
 * before A while A executes first (the application fences B behind A), so the most recently
 * *published* build is not the version the GPU header holds. The only authority is what the GPU
 * itself wrote, which is why the prepass publishes a version token into a shared mirror and the
 * queue reports the mirror value once the submission carrying that write has completed.
 *
 * A version is then one of:
 *   recorded  the most recently published build for the address, while the command list that
 *             recorded it still exists; the target of record-time CPU lookups (refit/copy/emit
 *             source resolution)
 *   live      the version the mirror says the GPU header currently holds
 *   stamped   superseded: waiting for every submission that was in flight when it was
 *             superseded to retire
 *   leased    a recording or a submission can still execute it
 *
 * A version is released only when nothing can read it any more: the GPU does not hold it, no
 * recording still holds a lease on it, and (if it was superseded by an observation) every
 * submission that was in flight at that observation has retired.
 *
 * Retirement is by active-submission low watermark, never by a completion counter: ids are handed
 * out on the CPU in submission order and retired in completion order, so a version may be
 * released only when no submission with an id at or below its stamp is still active. A
 * long-running submission that started before the version was superseded therefore keeps it
 * alive, and an abandoned recording can never release the version the GPU holds, because
 * superseding a version only ever stamps it.
 *
 * Bounded: per address at most one live version, one version per recording that has published but
 * not yet been reset or destroyed, and one version per in-flight submission that superseded one.
 * A recording releases its lease when its command list dies, so an application that repeatedly
 * records and abandons builds at one address without ever submitting them holds at most one
 * version at a time, with no GPU observation involved.
 */
template <typename Key, typename Payload, typename Hash = std::hash<Key>> class RTASLifetime {
public:
  struct Version {
    Payload payload;
    uint32_t token = 0;
    bool live = false;
    bool recorded = false;
    bool stamped = false;
    uint64_t barrier = 0;
    /*
     * Number of recordings that could still execute this version. A recording holds one from the
     * moment it publishes until its command list is reset or destroyed, so a version that an
     * application records over and over without ever submitting it is released as soon as its
     * last recording goes away instead of accumulating.
     */
    uint32_t leases = 0;
  };

  struct Slot {
    std::vector<Version> versions;
  };

  /** A submission id, handed out when a submission starts and retired when it completes. */
  uint64_t
  BeginSubmission() {
    const uint64_t id = next_submission_++;
    active_.insert(id);
    return id;
  }

  void
  RetireSubmission(uint64_t id) {
    active_.erase(id);
  }

  /**
   * Every submission in flight right now. A version superseded at this instant may only be
   * released once no id at or below this value is still active.
   */
  uint64_t
  ObservationBarrier() const {
    return next_submission_ - 1;
  }

  /**
   * Publish a newly recorded build. It becomes the recorded version and gets a fresh token; the
   * version it replaces stops being recorded.
   *
   * Nothing is stamped here: a version may only be superseded once the GPU has actually executed
   * a newer build for the address, which is exactly what an observation reports. Stamping at
   * publish time would release a version whose build was never submitted, or one that a
   * submission issued afterwards still reads because it executes before the newer build does.
   *
   * Always succeeds, and the returned token is never 0: the token is only ever compared against
   * other tokens of the same address, so its 32-bit wraparound cannot confuse two live versions
   * (an address has at most one live version, and a superseded one is released long before the
   * counter could reach it again).
   *
   * The caller receives the recording lease and must give it up when the command list that
   * published the version is reset or destroyed (ReleaseLease).
   */
  uint32_t
  Publish(const Key &key, Payload payload) {
    Slot &slot = slots_[key];
    for (auto &version : slot.versions)
      version.recorded = false;

    Version version;
    version.payload = std::move(payload);
    version.token = NextToken();
    version.recorded = true;
    /* The caller holds the recording lease until its command list dies (see ReleaseLease). */
    version.leases = 1;
    slot.versions.push_back(std::move(version));
    return slot.versions.back().token;
  }

  /**
   * Give up the lease one recording held on a version. Returns the payload when that was the last
   * thing keeping the version alive.
   *
   * This is what bounds repeated abandoned recordings: a build that is recorded and never
   * submitted is released when its command list is reset or destroyed, because it is neither the
   * version the GPU holds nor referenced by any submission, and it can never become either.
   *
   * A version that a submission still has in flight is not released here even though its last
   * recording is gone: it is kept until that submission retires (see Sweep).
   */
  std::vector<Payload>
  ReleaseLease(const Key &key, uint32_t token) {
    std::vector<Payload> released;
    auto found = slots_.find(key);
    if (found == slots_.end())
      return released;
    auto &versions = found->second.versions;
    for (auto version = versions.begin(); version != versions.end();) {
      if (version->token != token) {
        ++version;
        continue;
      }
      if (version->leases)
        --version->leases;
      if (Dead(*version, ActiveLowWatermark())) {
        released.push_back(std::move(version->payload));
        version = versions.erase(version);
      } else {
        ++version;
      }
    }
    return released;
  }

  /** Take one lease on a published version; its recording has been handed to a submission. */
  void
  HoldLease(const Key &key, uint32_t token) {
    auto found = slots_.find(key);
    if (found == slots_.end())
      return;
    for (auto &version : found->second.versions)
      if (version.token == token)
        ++version.leases;
  }

  /**
   * The version a record-time CPU lookup resolves against.
   *
   * The most recently published build while a recording still holds it; otherwise the version the
   * GPU actually holds, which is the native structure the address really refers to. A published
   * build with no recording left can never execute, so it is never the answer.
   */
  const Payload *
  FindRecorded(const Key &key) const {
    auto found = slots_.find(key);
    if (found == slots_.end())
      return nullptr;
    const Payload *live = nullptr;
    for (const auto &version : found->second.versions) {
      if (version.recorded && version.leases)
        return &version.payload;
      if (version.live)
        live = &version.payload;
    }
    return live;
  }

  /**
   * Report the token the GPU header holds for `key`. The matching version becomes live and every
   * superseded version is stamped against the submissions in flight right now.
   *
   * Returns false when the address or the token is not tracked, which can only happen for an
   * address whose backing allocation has already been released.
   */
  bool
  Observe(const Key &key, uint32_t token) {
    auto found = slots_.find(key);
    if (found == slots_.end())
      return false;
    bool matched = false;
    for (auto &version : found->second.versions) {
      version.live = version.token == token;
      matched |= version.live;
    }
    if (!matched)
      return false;
    Stamp(found->second);
    return true;
  }

  /**
   * Versions whose payload must now be released (residency dropped, reference let go).
   *
   * A version is dead when nothing can read it any more: the GPU does not hold it, no recording
   * still holds a lease on it, and if it was superseded by an observation, every submission that
   * was in flight at that observation has retired.
   */
  std::vector<Payload>
  Sweep() {
    std::vector<Payload> released;
    const uint64_t low = ActiveLowWatermark();
    for (auto &entry : slots_) {
      auto &versions = entry.second.versions;
      for (auto version = versions.begin(); version != versions.end();) {
        if (Dead(*version, low)) {
          released.push_back(std::move(version->payload));
          version = versions.erase(version);
        } else {
          ++version;
        }
      }
    }
    return released;
  }

  /** Every version of one address, for the death of the allocation that backs it. */
  std::vector<Payload>
  ReleaseAll(const Key &key) {
    std::vector<Payload> released;
    auto found = slots_.find(key);
    if (found == slots_.end())
      return released;
    for (auto &version : found->second.versions)
      released.push_back(std::move(version.payload));
    slots_.erase(found);
    return released;
  }

  template <typename Fn>
  void
  ForEachKey(Fn &&fn) const {
    for (const auto &entry : slots_)
      fn(entry.first);
  }

private:
  /**
   * Lowest id of any submission still in flight, or the highest possible id when none is. A
   * version stamped against a barrier at or above this value may still be read.
   */
  uint64_t
  ActiveLowWatermark() const {
    return active_.empty() ? UINT64_MAX : *active_.begin();
  }

  /**
   * One rule for every release site: a version is dead when nothing can read it any more. The GPU
   * does not hold it, no recording still holds a lease on it, and every submission that was in
   * flight when it was superseded has retired.
   */
  static bool
  Dead(const Version &version, uint64_t active_low) {
    if (version.live || version.leases)
      return false;
    return !version.stamped || active_low > version.barrier;
  }

  void
  Stamp(Slot &slot) {
    const uint64_t barrier = ObservationBarrier();
    for (auto &version : slot.versions) {
      if (version.live || version.recorded)
        continue;
      version.stamped = true;
      version.barrier = barrier;
    }
  }

  uint32_t
  NextToken() {
    const uint32_t token = next_token_++;
    /* 0 is reserved so a zeroed mirror slot is never a version. */
    return token ? token : next_token_++;
  }

  std::unordered_map<Key, Slot, Hash> slots_;
  std::set<uint64_t> active_;
  uint64_t next_submission_ = 1;
  uint32_t next_token_ = 1;
};

} // namespace owned_graphics
