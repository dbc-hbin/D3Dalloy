/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once
#include "d3d12_device_child.hpp"
#include "Metal.hpp"
#include <algorithm>
#include <limits>
#include <vector>

namespace d3dalloy {

/*
 * Residency is an explicit property of the pageable object, not of each COM
 * interface which happens to derive from ID3D12Pageable.  The device serializes
 * access to this state with its residency lock, so validation and a whole batch
 * transition are atomic with respect to other residency calls.
 */
class MTLD3D12PageableResidency {
  bool supported_ = false;
  uint32_t reference_count_ = 0;
  uint64_t allocation_bytes_ = 0;
  std::vector<WMT::Allocation> allocations_;

public:
  virtual ~MTLD3D12PageableResidency() = default;

  bool
  SupportsResidency() const {
    return supported_;
  }

  uint32_t
  ResidencyReferenceCount() const {
    return reference_count_;
  }

  bool
  CanAddResidencyReference(uint32_t count = 1) const {
    return count <= std::numeric_limits<uint32_t>::max() - reference_count_;
  }

  void
  AddResidencyReference(uint32_t count = 1) {
    reference_count_ += count;
  }

  bool
  CanRemoveResidencyReference(uint32_t count = 1) const {
    return count <= reference_count_;
  }

  void
  RemoveResidencyReference(uint32_t count = 1) {
    reference_count_ -= count;
  }

  const std::vector<WMT::Allocation> &
  ResidencyAllocations() const {
    return allocations_;
  }

  uint64_t
  ResidencyAllocationBytes() const {
    return allocation_bytes_;
  }

  void
  InitializeResidency(bool initially_resident) {
    supported_ = true;
    reference_count_ = initially_resident ? 1 : 0;
  }

  bool
  AddResidencyAllocation(WMT::Allocation allocation) {
    const uint64_t bytes = allocation.allocatedSize();
    if (bytes > std::numeric_limits<uint64_t>::max() - allocation_bytes_)
      return false;
    allocations_.push_back(allocation);
    allocation_bytes_ += bytes;
    return true;
  }

  bool
  RemoveResidencyAllocation(WMT::Allocation allocation) {
    auto it = std::find(allocations_.begin(), allocations_.end(), allocation);
    if (it == allocations_.end())
      return false;
    allocation_bytes_ -= it->allocatedSize();
    allocations_.erase(it);
    return true;
  }
};

template <typename... Base>
class MTLD3D12Pageable : public MTLD3D12DeviceChild<Base...>, public MTLD3D12PageableResidency {
public:
  MTLD3D12Pageable(MTLD3D12Device *pDevice) : MTLD3D12DeviceChild<Base...>(pDevice) {}
};

using MTLD3D12PageableCommon = MTLD3D12Pageable<ID3D12Pageable>;

}; // namespace d3dalloy
