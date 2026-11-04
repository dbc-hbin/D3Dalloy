#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace d3dalloy {
// Owner is copied under the shared map lock. Native commit/residency never
// participates in lookup locking, and an unregister cannot invalidate a pin.
template<class Owner> class OwnedVARegistry {
  // A physical alias cohort preserves the original allocation as its VA
  // resolution and RTAS sidecar identity until its last registration ends.
  // Ordinary addresses need no separate allocation or secondary lookup.
  struct Entry {
    uint64_t length;
    Owner owner;
    std::unique_ptr<std::vector<Owner>> aliases;
    bool original_registered = true;
    Entry(uint64_t length, Owner &&owner) : length(length), owner(std::move(owner)) {}
  };
  struct NoAliases { bool operator()(const Owner &, const Owner &) const { return false; } };
  std::map<uint64_t,Entry> intervals_;
  mutable std::shared_mutex mutex_;
  std::atomic<uint64_t>& generation_;
public:
  struct Pin {Owner owner;uint64_t offset=0,begin=0,end=0,generation=0;};
  struct Removal {
    bool removed = false;
    Owner retired_identity{}; // non-null only after the last registration
    size_t remaining = 0;
  };
  explicit OwnedVARegistry(std::atomic<uint64_t>& generation):generation_(generation) {}
  template<class SamePlacement = NoAliases>
  bool Insert(uint64_t address,uint64_t length,Owner owner,SamePlacement same_placement = {},
              size_t *alias_members = nullptr) {
    if (alias_members) *alias_members = 0;
    if (!owner || !length || length > UINT64_MAX-address) return false;
    std::unique_lock lock(mutex_);
    auto [where, inserted] = intervals_.try_emplace(address, length, std::move(owner));
    if (!inserted) {
      auto &entry = where->second;
      // try_emplace did not move owner on a collision. Equal VA alone is
      // never enough to make two owners interchangeable for the full range.
      if (entry.length != length || entry.owner == owner || !same_placement(entry.owner, owner)) return false;
      if (!entry.aliases) {
        auto aliases = std::make_unique<std::vector<Owner>>();
        aliases->push_back(std::move(owner));
        entry.aliases = std::move(aliases);
      } else {
        if (std::find(entry.aliases->begin(), entry.aliases->end(), owner) != entry.aliases->end()) return false;
        entry.aliases->push_back(std::move(owner));
      }
      if (alias_members) *alias_members = size_t(entry.original_registered) + entry.aliases->size();
    }
    generation_.fetch_add(1,std::memory_order_release);return true;
  }
  Removal Remove(uint64_t address,const Owner &expected) {
    std::unique_lock lock(mutex_);auto it=intervals_.find(address);
    if (it == intervals_.end() || !expected) return {};
    auto &entry = it->second;
    if (expected == entry.owner) {
      if (!entry.original_registered) return {};
      entry.original_registered = false;
    } else {
      if (!entry.aliases) return {};
      auto &aliases = *entry.aliases;
      auto found = std::find(aliases.begin(), aliases.end(), expected);
      if (found == aliases.end()) return {};
      aliases.erase(found);
      if (aliases.empty()) entry.aliases.reset();
    }
    Removal result{true, {}, size_t(entry.original_registered) + (entry.aliases ? entry.aliases->size() : 0)};
    if (!result.remaining) {
      result.retired_identity = std::move(entry.owner);
      intervals_.erase(it);
    }
    generation_.fetch_add(1,std::memory_order_release);return result;
  }
  Pin Lookup(uint64_t address) const {
    std::shared_lock lock(mutex_);Pin result;result.generation=generation_.load(std::memory_order_relaxed);
    auto it=intervals_.upper_bound(address);
    const auto next=it==intervals_.end()?UINT64_MAX:it->first;
    if(it==intervals_.begin())return result;
    --it;const auto offset=address-it->first;
    if(offset>=it->second.length)return result;
    result.owner=it->second.owner;result.offset=offset;result.begin=it->first;
    result.end=it->first+std::min(it->second.length,next-it->first);return result;
  }
  std::vector<Owner> Snapshot() const {
    // Keep the result outside the lock scope: allocation/copy failure releases
    // the registry lock before unwinding any owners that were already pinned.
    std::vector<Owner> owners;
    {
      std::shared_lock lock(mutex_);
      owners.reserve(intervals_.size());
      for (const auto& [address,entry] : intervals_) {
        owners.push_back(entry.owner);
        if (entry.aliases) owners.insert(owners.end(), entry.aliases->begin(), entry.aliases->end());
      }
    }
    return owners;
  }
};
} // namespace d3dalloy
