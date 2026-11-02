#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <algorithm>

namespace d3dalloy {
// Owner is copied under the shared map lock. Native commit/residency never
// participates in lookup locking, and an unregister cannot invalidate a pin.
template<class Owner> class OwnedVARegistry {
  struct Entry {uint64_t length; Owner owner;};
  std::map<uint64_t,Entry> intervals_;
  mutable std::shared_mutex mutex_;
  std::atomic<uint64_t>& generation_;
public:
  struct Pin {Owner owner;uint64_t offset=0,begin=0,end=0,generation=0;};
  explicit OwnedVARegistry(std::atomic<uint64_t>& generation):generation_(generation) {}
  bool Insert(uint64_t address,uint64_t length,Owner owner) {
    if (!owner || !length || length > UINT64_MAX-address) return false;
    std::unique_lock lock(mutex_);
    if (!intervals_.emplace(address,Entry{length,std::move(owner)}).second) return false;
    generation_.fetch_add(1,std::memory_order_release);return true;
  }
  Owner Remove(uint64_t address) {
    std::unique_lock lock(mutex_);auto entry=intervals_.find(address);
    if(entry==intervals_.end())return {};
    Owner owner=std::move(entry->second.owner);intervals_.erase(entry);
    generation_.fetch_add(1,std::memory_order_release);return owner;
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
};
} // namespace d3dalloy
