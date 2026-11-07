#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <compare>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <tuple>
#include <unordered_map>
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
  bool external_backing = false;
  // Nonzero only for bounded, disjoint indirect-argument reads. end includes the final element.
  uint32_t byte_stride = 0, element_bytes = 0;
  bool SamePattern(const SubmissionAccess &b) const {
    return byte_stride == b.byte_stride && element_bytes == b.element_bytes;
  }
  template<class F> bool AnyByteSpan(F f) const {
    if (!byte_stride) return f(begin, end);
    for (uint64_t pos = begin;; pos += byte_stride) {
      if (f(pos, pos + element_bytes)) return true;
      if (end - pos == element_bytes) return false;
    }
  }
  bool OverlapsBytes(uint64_t lo, uint64_t hi) const {
    lo = std::max(lo, begin);
    hi = std::min(hi, end);
    if (lo >= hi) return false;
    if (!byte_stride) return true;
    const auto phase = (lo - begin) % byte_stride;
    return phase < element_bytes || hi - lo > byte_stride - phase;
  }
  bool CoversBytes(const SubmissionAccess &other) const {
    if (begin > other.begin || end < other.end) return false;
    if (!byte_stride) return true;
    return !other.AnyByteSpan([&](uint64_t lo, uint64_t hi) {
      const auto phase = (lo - begin) % byte_stride;
      return phase >= element_bytes || hi - lo > element_bytes - phase;
    });
  }
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
    return !a.byte_stride ? b.OverlapsBytes(a.begin, a.end) :
           !b.byte_stride ? a.OverlapsBytes(b.begin, b.end) :
           a.AnyByteSpan([&](uint64_t lo, uint64_t hi) { return b.OverlapsBytes(lo, hi); });
  return (a.aspects & b.aspects) && a.mip_begin < b.mip_end && b.mip_begin < a.mip_end &&
      a.slice_begin < b.slice_end && b.slice_begin < a.slice_end;
}
// Track unsynchronized accesses since the most recent Dispatch -> Dispatch
// barrier. A zero mask bit orders the complete preceding group, so start
// tracking again at that dispatch even when the barrier was budget-driven.
struct DispatchDependencyProof {
  uint64_t independent = 0;
  size_t comparisons = 0;
  bool needs_barrier = false;
  bool budget_barrier = false;

  bool Append(size_t ordinal, std::span<const SubmissionAccess> accesses) {
    if (accesses.size() > 256) return false;
    if (ordinal % 64 == 0) {
      independent = 0;
      window_comparison_start = comparisons;
    }
    bool barrier = accesses.size() > 256 - access_count;
    budget_barrier |= barrier;
    for (const auto &access : accesses) {
      if (barrier) break;
      auto found = history.find(access.Key());
      if (found == history.end()) continue;
      const auto &group = found->second;
      // Read-only accesses never conflict with other readers of this backing.
      if (!access.writes && !group.has_writer) continue;
      for (const auto &earlier : group.accesses) {
        if (!access.writes && !earlier.writes) continue;
        if (comparisons - window_comparison_start == 4096) {
          barrier = budget_barrier = true;
          break;
        }
        ++comparisons;
        if (DispatchOverlaps(access, earlier)) { barrier = true; break; }
      }
    }
    if (barrier) {
      needs_barrier = true;
      history.clear();
      access_count = 0;
    } else if (ordinal) independent |= uint64_t(1) << (ordinal % 64);
    // Simultaneous bindings of one dispatch cannot create an inter-dispatch edge.
    for (const auto &access : accesses) {
      auto &group = history[access.Key()];
      group.has_writer |= access.writes;
      // Byte hazards need only the union of the read ranges and the union
      // of the write ranges. Combine adjacent suballocations or repeat binds
      // without filling a hole or changing the access mode.
      if (!group.accesses.empty() && !access.aspects) {
        auto &last = group.accesses.back();
        if (!last.byte_stride && !access.byte_stride && last.writes == access.writes && !last.aspects &&
            last.end >= access.begin && access.end >= last.begin) {
          last.begin = std::min(last.begin, access.begin);
          last.end = std::max(last.end, access.end);
          continue;
        }
      }
      // Exact repeated texture reads have the same future hazards. The retained
      // allocation is part of the check because placed aliases share a heap key.
      if (access.aspects && !access.writes &&
          std::any_of(group.accesses.begin(), group.accesses.end(), [&](const auto &old) {
            return !old.writes && old.allocation == access.allocation && old.aspects == access.aspects &&
                old.begin == access.begin && old.end == access.end && old.mip_begin == access.mip_begin &&
                old.mip_end == access.mip_end && old.slice_begin == access.slice_begin && old.slice_end == access.slice_end;
          })) continue;
      group.accesses.push_back(access);
      ++access_count;
    }
    return true;
  }
private:
  static bool DispatchOverlaps(const SubmissionAccess &a, const SubmissionAccess &b) {
    // Within one retained placed texture, mip and array ranges are exact. Different
    // views of a packed depth/stencil subresource still share its storage.
    if (a.Key().domain == PhysicalDomain::Heap && a.allocation && a.allocation == b.allocation &&
        a.aspects && b.aspects && a.begin == b.begin && a.end == b.end && a.begin < a.end &&
        a.end != UINT64_MAX && a.mip_begin < a.mip_end && b.mip_begin < b.mip_end &&
        a.slice_begin < a.slice_end && b.slice_begin < b.slice_end)
      return a.mip_begin < b.mip_end && b.mip_begin < a.mip_end &&
          a.slice_begin < b.slice_end && b.slice_begin < a.slice_end;
    return AccessOverlaps(a, b);
  }
  struct Group { std::vector<SubmissionAccess> accesses; bool has_writer = false; };
  std::unordered_map<PhysicalIdentity, Group, PhysicalIdentityHash> history;
  size_t access_count = 0, window_comparison_start = 0;
};
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
  struct StridedRead {
    uint64_t begin, live;
    uint32_t stride, width;
    Value value;

    // One bit per intact element. After a partial overwrite, the surviving
    // ends of that element move to the ordinary byte intervals.
    uint64_t OverlapMask(uint64_t lo, uint64_t hi) const {
      if (hi <= begin || hi <= lo) return 0;
      const uint64_t offset = std::max(lo, begin) - begin;
      const uint64_t first = offset / stride + (offset % stride >= width);
      if (first >= 64) return 0;
      const auto last = std::min<uint64_t>(63, (hi - 1 - begin) / stride);
      return live & (UINT64_MAX << first) & (UINT64_MAX >> (63 - last));
    }
  };
  struct Lane {
    Intervals cells;
    std::vector<StridedRead> strided_reads;
  };
  std::map<std::pair<PhysicalIdentity,uint64_t>,Lane> lanes_;
  template<class F> static void spans(const SubmissionAccess& a,F callback) {
    if (a.Key().domain==PhysicalDomain::Heap || !a.aspects) {
      if (a.begin < a.end) a.AnyByteSpan([&](uint64_t lo, uint64_t hi) {
        callback(0, lo, hi);
        return false;
      });
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
  template<class F> static void updateCells(Intervals& cells, uint64_t begin, uint64_t end, F update) {
    split(cells,begin);split(cells,end);
    auto it=cells.lower_bound(begin);uint64_t at=begin;
    while(at<end) {
      if(it==cells.end() || it->first>at)
        it=cells.emplace_hint(it,at,Cell{it==cells.end()?end:std::min(end,it->first),{}, {}});
      update(it->second);
      at=it->second.end;++it;
    }
  }
  template<class Preserve> static void retireStridedReads(Lane &lane, uint64_t begin, uint64_t end, Preserve &preserve) {
    for (auto &read : lane.strided_reads) {
      const uint64_t hit = read.OverlapMask(begin, end);
      if (!hit || preserve(read.value)) continue;
      auto retain = [&](uint64_t lo, uint64_t hi) {
        updateCells(lane.cells, lo, hi, [&](Cell &cell) { cell.readers.push_back(read.value); });
      };
      const auto first = read.begin + std::countr_zero(hit) * uint64_t(read.stride);
      const auto last = read.begin + (63 - std::countl_zero(hit)) * uint64_t(read.stride);
      if (first < begin) retain(first, begin);
      if (last + read.width > end) retain(end, last + read.width);
      read.live &= ~hit;
    }
    std::erase_if(lane.strided_reads, [](const auto &read) { return !read.live; });
  }
public:
  void Clear() {lanes_.clear();}
  template<class F> void Visit(const SubmissionAccess& access,F callback) const {
    if (access.byte_stride && !access.writes) {
      const auto lane = lanes_.find({access.Key(),0});
      if (lane == lanes_.end() || lane->second.cells.empty()) return;
    }
    spans(access,[&](uint64_t lane,uint64_t begin,uint64_t end){
      auto found=lanes_.find({access.Key(),lane});if(found==lanes_.end())return;
      const auto& storage=found->second;
      if (access.writes) for (const auto &read : storage.strided_reads)
        if (read.OverlapMask(begin,end)) callback(read.value);
      const auto& cells=storage.cells;auto it=cells.upper_bound(begin);
      if(it!=cells.begin()) {auto previous=std::prev(it);if(previous->second.end>begin)it=previous;}
      for(;it!=cells.end() && it->first<end;++it) {
        for(const auto& writer:it->second.writers)callback(writer);
        if(access.writes)for(const auto& reader:it->second.readers)callback(reader);
      }
    });
  }
  template<class Preserve> void PublishPreserving(const SubmissionAccess& access,const Value& value,Preserve preserve) {
    if (!access.writes && access.byte_stride && access.begin < access.end) {
      const auto last = (access.end - 1 - access.begin) / access.byte_stride;
      auto &reads = lanes_[{access.Key(),0}].strided_reads;
      // Keep a dense writer's scan bounded even for adversarial command lists.
      if (last < 64 && reads.size() < 32) {
        reads.push_back({access.begin, UINT64_MAX >> (63 - last), access.byte_stride, access.element_bytes, value});
        return;
      }
    }
    spans(access,[&](uint64_t lane,uint64_t begin,uint64_t end){
      auto& storage=lanes_[{access.Key(),lane}];
      if (access.writes && !storage.strided_reads.empty()) retireStridedReads(storage,begin,end,preserve);
      updateCells(storage.cells,begin,end,[&](Cell &cell) {
        if(access.writes) {
          std::erase_if(cell.writers,[&](const auto& old){return !preserve(old);});
          std::erase_if(cell.readers,[&](const auto& old){return !preserve(old);});
          cell.writers.push_back(value);
        } else cell.readers.push_back(value);
      });
    });
  }
  void Publish(const SubmissionAccess& access,const Value& value,bool replace=true) {
    PublishPreserving(access,value,[&](const Value&){return !replace;});
  }
};
} // namespace d3dalloy
