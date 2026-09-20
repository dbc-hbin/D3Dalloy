// SPDX-License-Identifier: LGPL-2.1-or-later
// Owned graphics backend: GPU-published acceleration-structure version slots.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace owned_graphics {

/*
 * Fixed-size pages of 32-bit version slots that the GPU writes and the CPU reads.
 *
 * The prepass kernel publishes the version token of the build it is about to execute into one
 * of these slots. A page is created once, never resized, never copied and never freed while the
 * device lives, so a GPU write in flight always targets a buffer that is still there; that is
 * why slots are not packed into one growing buffer.
 *
 * A slot is a `std::atomic<uint32_t>` and the host reads it with a single 32-bit acquire load.
 * The device is not a C++ thread and its store is not a C++ atomic store, so what this buys is
 * exactly what the hardware offers: both sides access one aligned 32-bit word, so the value can
 * never tear and the host never observes a partially written token. Tokens are 32-bit for that
 * reason alone: the 64-bit resource id is not readable this way without a wider access than the
 * hardware guarantees.
 *
 * The host load establishes no ordering against the GPU's own writes. What makes the token
 * authoritative is the command buffer's completion: the queue only reads a slot after the
 * submission that wrote it reported completed, and completion is the barrier that publishes the
 * kernel's header, instance and token writes to the CPU. The acquire here is what keeps a later
 * host read of that header from being reordered before the load of the token that announced it.
 *
 * Slots are recycled. An address keeps its slot for its whole life, and the device returns the
 * slot when the allocation that backs the address is released, which D3D12 only permits once no
 * in-flight submission can still read it. A recycled slot is cleared, so it can never carry its
 * previous occupant's token into the new address's first observation, and pages are never freed,
 * so the page count is bounded by the peak number of addresses alive at once rather than by how
 * many acceleration structures the application ever creates.
 */
class RTASMirror {
public:
  static constexpr uint32_t kSlotsPerPage = 4096;

  using Slot = std::atomic<uint32_t>;

  /** Creates one page of kSlotsPerPage slots and returns a CPU-visible pointer to it. */
  using CreatePage = std::function<Slot *(uint64_t slot_count)>;

  explicit RTASMirror(CreatePage create) : create_(std::move(create)) {}

  /**
   * Reserve one slot. Returns false only when a page could not be created (real out-of-memory) or
   * when the 32-bit index space is exhausted, which the caller reports as a failed build rather
   * than as an untracked version.
   *
   * A slot returned by Free is handed out again before a new one is taken, so the page count
   * tracks how many addresses are alive at once and not how many were ever created.
   */
  bool Allocate(uint32_t *slot) {
    if (!free_slots_.empty()) {
      *slot = free_slots_.back();
      free_slots_.pop_back();
      /* A recycled slot must read as "no build has executed" for its new address. */
      Pointer(*slot)->store(0, std::memory_order_release);
      return true;
    }
    if (next_slot_ == pages_.size() * kSlotsPerPage) {
      if (pages_.size() >= kMaxPages)
        return false;
      Slot *page = create_(kSlotsPerPage);
      if (!page)
        return false;
      pages_.push_back(page);
    }
    *slot = static_cast<uint32_t>(next_slot_++);
    return true;
  }

  /**
   * Return a slot for reuse, when the address that owned it is gone.
   *
   * The caller must be certain no GPU work can still publish into this slot: the only slot this
   * backend ever frees is the one belonging to an allocation D3D12 has already let the
   * application release, which requires every submission that could read it to have completed.
   */
  void Free(uint32_t slot) { free_slots_.push_back(slot); }

  /** Slots currently handed out. */
  uint64_t AllocatedCount() const { return next_slot_ - free_slots_.size(); }

  /**
   * Token the GPU last published for a slot; 0 until a build executes.
   *
   * The load is atomic so the host cannot observe a torn token while the device stores one. The
   * device's store is not a C++ atomic operation and the acquire here does not order the CPU
   * against it: what makes the value the GPU's final one is that the queue reads a slot only
   * after the submission that wrote it reported completion, and command completion is the barrier
   * that publishes the kernel's header, instance and token writes to the host. The acquire is
   * what keeps a later host read of that header from being reordered before this load.
   */
  uint32_t Load(uint32_t slot) const {
    return base(slot)[slot % kSlotsPerPage].load(std::memory_order_acquire);
  }

  /** Host-visible address of a slot, for the GPU to write. */
  Slot *Pointer(uint32_t slot) const {
    return &base(slot)[slot % kSlotsPerPage];
  }

  uint64_t PageCount() const { return pages_.size(); }

private:
  Slot *base(uint32_t slot) const { return pages_[slot / kSlotsPerPage]; }

  /**
   * The public contract is a 32-bit slot index, so the whole index space is 2^32 slots and that
   * is the only cap. Nothing beyond it is assumed: pages are created lazily and a failure to
   * create one is the real out-of-memory condition, while reaching the last index is exhaustion
   * of the index space itself. Both are reported by Allocate returning false.
   */
  static constexpr size_t kMaxPages = (uint64_t(UINT32_MAX) + 1) / kSlotsPerPage;
  static_assert(
      kMaxPages * kSlotsPerPage == uint64_t(UINT32_MAX) + 1,
      "the page budget must be exactly the 32-bit slot index space"
  );
  static_assert(kSlotsPerPage * uint64_t(sizeof(Slot)) <= UINT32_MAX, "a page length must fit uint32");

  CreatePage create_;
  std::vector<Slot *> pages_;
  /* Slots of released addresses, reusable by Allocate. Bounded by kMaxPages * kSlotsPerPage. */
  std::vector<uint32_t> free_slots_;
  size_t next_slot_ = 0;
};

} // namespace owned_graphics
