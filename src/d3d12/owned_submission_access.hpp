#pragma once
#include <algorithm>
#include <cstdint>
#include <compare>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>
#include <vector>

namespace d3dalloy {
enum class PhysicalDomain : uint8_t { Legacy, Buffer, Texture, Heap, History };
struct PhysicalIdentity {
  PhysicalDomain domain = PhysicalDomain::Legacy;
  uintptr_t owner = 0;
  auto operator<=>(const PhysicalIdentity&) const = default;
};
struct PhysicalIdentityHash {
  size_t operator()(const PhysicalIdentity& identity) const {
    return std::hash<uintptr_t>{}(identity.owner) ^ (size_t(identity.domain) * 0x9e3779b9u);
  }
};
// Logical identity remains available for legal attachment unions. Physical
// identity is backed by retained allocations/native objects through retirement.
struct SubmissionAccess {
  const void *allocation;
  uint64_t begin = 0, end = 0;
  uint32_t mip_begin = 0, mip_end = 1, slice_begin = 0, slice_end = 1;
  uint8_t aspects = 0;
  bool writes = false, attachment = false;
  uint64_t stages = 0;
  PhysicalIdentity physical{};
  PhysicalIdentity Key() const {
    return physical.owner ? physical : PhysicalIdentity{PhysicalDomain::Legacy,reinterpret_cast<uintptr_t>(allocation)};
  }
};
// Native placement size, never a texel-derived estimate. Unknown extents retain
// the whole-heap alias domain; both accesses and scoped boundaries use this.
inline void SetHeapAccessExtent(SubmissionAccess &access, uint64_t offset, uint64_t size) {
  const bool known = size && size <= UINT64_MAX - offset;
  access.begin = known ? offset : 0;
  access.end = known ? offset + size : UINT64_MAX;
}
inline bool AccessOverlaps(const SubmissionAccess& a,const SubmissionAccess& b) {
  if (a.Key()!=b.Key()) return false;
  if (a.Key().domain==PhysicalDomain::Heap || !a.aspects || !b.aspects)
    return a.begin < b.end && b.begin < a.end;
  return (a.aspects & b.aspects) && a.mip_begin < b.mip_end && b.mip_begin < a.mip_end &&
      a.slice_begin < b.slice_end && b.slice_begin < a.slice_end;
}
// UINT64_MAX is the planner's unknown whole-heap endpoint. Keep it outside
// the attachment merge family even when an arithmetic extent ends there.
inline bool HasFiniteHeapExtent(uint64_t offset, uint64_t size) {
  return size && offset < UINT64_MAX - size;
}
// Heap aliases normally require native resource-alias visibility. The narrow
// exception is the same retained texture/subresource used as an attachment:
// native primitive ordering already orders it, including the depth/stencil
// aspect pair that the physical heap frontier deliberately treats as one span.
// View compatibility and disjoint attachment unions are checked separately.
inline bool AttachmentDependencyOrdered(const SubmissionAccess &a, const SubmissionAccess &b) {
  if (!a.attachment || !b.attachment || a.Key() != b.Key()) return false;
  if (a.Key().domain != PhysicalDomain::Heap) return true;
  return a.allocation && a.allocation == b.allocation && a.begin < a.end &&
      HasFiniteHeapExtent(a.begin, a.end - a.begin) && a.begin == b.begin && a.end == b.end &&
      a.mip_begin == b.mip_begin && a.mip_end == b.mip_end &&
      a.slice_begin == b.slice_begin && a.slice_end == b.slice_end;
}
// Interval subdivision is exact: a partial write removes only covered history;
// readers of untouched portions remain. Texture aspects/mips are separate lanes
// with slice intervals; heap alias domains deliberately use byte intervals.
template<class Value> class OwnedAccessFrontier {
  struct Cell {uint64_t end; std::vector<Value> writers,readers;};
  using Intervals=std::map<uint64_t,Cell>;
  std::map<std::pair<PhysicalIdentity,uint64_t>,Intervals> lanes_;
  template<class F> static void spans(const SubmissionAccess& a,F callback) {
    if (a.Key().domain==PhysicalDomain::Heap || !a.aspects) {
      if (a.begin<a.end) callback(0,a.begin,a.end);
      return;
    }
    for(uint32_t aspect=1;aspect<=4;aspect<<=1) if(a.aspects&aspect)
      for(uint32_t mip=a.mip_begin;mip<a.mip_end;++mip)
        if(a.slice_begin<a.slice_end)callback((uint64_t(aspect)<<32)|mip,a.slice_begin,a.slice_end);
  }
  static void split(Intervals& cells,uint64_t position) {
    auto after=cells.upper_bound(position);if(after==cells.begin())return;auto before=std::prev(after);
    if(before->first==position || before->second.end<=position)return;
    auto copy=before->second;
    cells.emplace_hint(after,position,std::move(copy));before->second.end=position;
  }
public:
  void Clear() {lanes_.clear();}
  template<class F> void Visit(const SubmissionAccess& access,F callback) const {
    spans(access,[&](uint64_t lane,uint64_t begin,uint64_t end){
      auto found=lanes_.find({access.Key(),lane});if(found==lanes_.end())return;
      const auto& cells=found->second;auto it=cells.upper_bound(begin);
      if(it!=cells.begin()) {auto previous=std::prev(it);if(previous->second.end>begin)it=previous;}
      for(;it!=cells.end() && it->first<end;++it) {
        for(const auto& writer:it->second.writers)callback(writer);
        if(access.writes)for(const auto& reader:it->second.readers)callback(reader);
      }
    });
  }
  template<class Preserve> void PublishPreserving(const SubmissionAccess& access,const Value& value,Preserve preserve) {
    spans(access,[&](uint64_t lane,uint64_t begin,uint64_t end){
      auto& cells=lanes_[{access.Key(),lane}];split(cells,begin);split(cells,end);
      auto it=cells.lower_bound(begin);uint64_t at=begin;
      while(at<end) {
        if(it==cells.end() || it->first>at)
          it=cells.emplace_hint(it,at,Cell{it==cells.end()?end:std::min(end,it->first),{}, {}});
        auto& cell=it->second;
        if(access.writes) {
          std::erase_if(cell.writers,[&](const auto& old){return !preserve(old);});
          std::erase_if(cell.readers,[&](const auto& old){return !preserve(old);});
          cell.writers.push_back(value);
        } else cell.readers.push_back(value);
        at=cell.end;++it;
      }
    });
  }
  void Publish(const SubmissionAccess& access,const Value& value,bool replace=true) {
    PublishPreserving(access,value,[&](const Value&){return !replace;});
  }
};
} // namespace d3dalloy
