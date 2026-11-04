#pragma once
#include "owned_submission_access.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace d3dalloy {

// A phase pair may overlap only with independent private contexts and complete,
// disjoint physical work. Their observable histories can then be ordered either
// way; any phase that could observe both waits for their export retirement.
// Tickets are released by GPU retirement, not by native command-buffer commit.
class GPUExecutionGate {
public:
  using Accesses = std::shared_ptr<const std::vector<SubmissionAccess>>;

private:
  struct KnownAdmission {
    Accesses accesses;
    bool private_domain = false;
    KnownAdmission *previous = nullptr, *next = nullptr;
    explicit KnownAdmission(Accesses value) : accesses(std::move(value)) {}
  };
  enum class Kind { Unknown, Known, Exclusive, Atomic };
  std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t readers_ = 0, writers_waiting_ = 0, atomic_waiters_ = 0;
  size_t atomic_count_ = 0;
  uint64_t next_atomic_ = 0, serving_atomic_ = 0;
  KnownAdmission *known_ = nullptr, *atomic_ = nullptr;
  bool exclusive_ = false, cancelled_ = false;
  // Every admission consumes a serial. Only uninterrupted FIFO work or a
  // handoff after all preceding tickets retired may inherit the prior proof.
  uint64_t ordered_queue_ = 0, admission_serial_ = 0;
  uint64_t ObserveQueue(uint64_t queue) {
    if (admission_serial_ == UINT64_MAX) return 0;
    ++admission_serial_;
    if (queue != ordered_queue_ && (exclusive_ || atomic_ || readers_ || known_)) queue = 0;
    ordered_queue_ = queue;
    return queue ? admission_serial_ : 0;
  }

  static bool Conflicts(const Accesses &a, const Accesses &b) {
    for (const auto &first : *a)
      for (const auto &second : *b)
        if ((first.writes || second.writes) && AccessOverlaps(first, second)) return true;
    return false;
  }
  static bool ConflictsWith(KnownAdmission *head, const Accesses &accesses) {
    for (auto *entry = head; entry; entry = entry->next)
      if (Conflicts(accesses, entry->accesses)) return true;
    return false;
  }
  static void Link(KnownAdmission *&head, KnownAdmission *entry) {
    entry->next = head;
    if (head) head->previous = entry;
    head = entry;
  }
  static void Unlink(KnownAdmission *&head, KnownAdmission *entry) {
    if (entry->previous) entry->previous->next = entry->next;
    else head = entry->next;
    if (entry->next) entry->next->previous = entry->previous;
  }

public:
  class EncodingAdmission {
    GPUExecutionGate *gate_;
    EncodingAdmission *previous_;
    inline static thread_local EncodingAdmission *current_ = nullptr;
    friend class GPUExecutionGate;
  public:
    explicit EncodingAdmission(GPUExecutionGate &gate) : gate_(&gate), previous_(current_) { current_ = this; }
    EncodingAdmission(const EncodingAdmission &) = delete;
    EncodingAdmission &operator=(const EncodingAdmission &) = delete;
    ~EncodingAdmission() { current_ = previous_; }
  };
  bool CurrentThreadAdmitted() const {
    for (auto *scope = EncodingAdmission::current_; scope; scope = scope->previous_)
      if (scope->gate_ == this) return true;
    return false;
  }
  class Ticket {
    GPUExecutionGate *gate_ = nullptr;
    Kind kind_ = Kind::Unknown;
    std::unique_ptr<KnownAdmission> known_;
    size_t independent_peers_ = 0;
    uint64_t ordered_era_ = 0;
    size_t atomic_peers_ = 0;
    friend class GPUExecutionGate;
    Ticket(GPUExecutionGate *gate, Kind kind, std::unique_ptr<KnownAdmission> known = {}, size_t peers = 0,
           uint64_t ordered_era = 0, size_t atomic_peers = 0) :
        gate_(gate), kind_(kind), known_(std::move(known)), independent_peers_(peers), ordered_era_(ordered_era),
        atomic_peers_(atomic_peers) {}

  public:
    Ticket() = default;
    Ticket(const Ticket &) = delete;
    Ticket &operator=(const Ticket &) = delete;
    Ticket(Ticket &&other) noexcept : gate_(std::exchange(other.gate_, nullptr)), kind_(other.kind_),
        known_(std::move(other.known_)), independent_peers_(other.independent_peers_), ordered_era_(other.ordered_era_),
        atomic_peers_(other.atomic_peers_) {}
    Ticket &operator=(Ticket &&other) noexcept {
      if (this != &other) {
        Reset();
        gate_ = std::exchange(other.gate_, nullptr);
        kind_ = other.kind_;
        known_ = std::move(other.known_);
        independent_peers_ = other.independent_peers_;
        ordered_era_ = other.ordered_era_;
        atomic_peers_ = other.atomic_peers_;
      }
      return *this;
    }
    ~Ticket() { Reset(); }
    explicit operator bool() const { return gate_ != nullptr; }
    bool IsKnown() const { return gate_ && kind_ == Kind::Known; }
    size_t IndependentPeers() const { return independent_peers_; }
    size_t AtomicPeers() const { return atomic_peers_; }
    uint64_t OrderedAdmissionSerial() const { return gate_ ? ordered_era_ : 0; }
    // The initial conservative ticket already excludes Atomic64. Publish the
    // completed, retained Execute proof before any of its GPU work is encoded.
    void NarrowShared(Accesses accesses) {
      if (!gate_ || kind_ != Kind::Unknown || !accesses) return;
      auto known = std::make_unique<KnownAdmission>(std::move(accesses));
      {
        std::lock_guard lock(gate_->mutex_);
        --gate_->readers_;
        Link(gate_->known_, known.get());
        known_ = std::move(known);
        kind_ = Kind::Known;
      }
      gate_->changed_.notify_all();
    }
    void Reset() {
      auto *gate = std::exchange(gate_, nullptr);
      if (!gate) return;
      {
        std::lock_guard lock(gate->mutex_);
        switch (kind_) {
        case Kind::Unknown: --gate->readers_; break;
        case Kind::Known: Unlink(gate->known_, known_.get()); break;
        case Kind::Exclusive: gate->exclusive_ = false; break;
        case Kind::Atomic: Unlink(gate->atomic_, known_.get()); --gate->atomic_count_; break;
        }
      }
      known_.reset();
      gate->changed_.notify_all();
    }
  };

  Ticket AcquireShared(uint64_t queue = 0) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return cancelled_ || (!exclusive_ && !atomic_ && !writers_waiting_); });
    if (cancelled_) return {};
    const auto era = ObserveQueue(queue);
    ++readers_;
    return Ticket(this, Kind::Unknown, {}, 0, era);
  }
  Ticket AcquireKnownShared(Accesses accesses, uint64_t queue = 0) {
    if (!accesses) return AcquireShared(queue);
    auto known = std::make_unique<KnownAdmission>(std::move(accesses));
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] {
      return cancelled_ || (!exclusive_ && !writers_waiting_ &&
          !ConflictsWith(atomic_, known->accesses));
    });
    if (cancelled_) return {};
    const size_t peers = atomic_count_;
    const auto era = ObserveQueue(queue);
    Link(known_, known.get());
    return Ticket(this, Kind::Known, std::move(known), peers, era);
  }
  Ticket AcquireExclusive() {
    std::unique_lock lock(mutex_);
    ++writers_waiting_;
    changed_.wait(lock, [&] { return cancelled_ || (!exclusive_ && !atomic_ && !readers_ && !known_); });
    --writers_waiting_;
    if (cancelled_) return {};
    ObserveQueue(0);
    exclusive_ = true;
    return Ticket(this, Kind::Exclusive);
  }
  Ticket AcquireAtomic(Accesses accesses, bool private_domain = false) {
    if (!accesses) return AcquireExclusive();
    auto known = std::make_unique<KnownAdmission>(std::move(accesses));
    known->private_domain = private_domain;
    std::unique_lock lock(mutex_);
    ++writers_waiting_;
    ++atomic_waiters_;
    const auto turn = next_atomic_++;
    changed_.wait(lock, [&] {
      return cancelled_ || (turn == serving_atomic_ && writers_waiting_ == atomic_waiters_ &&
          !exclusive_ && !readers_ && (!atomic_ || (private_domain && atomic_->private_domain &&
          atomic_count_ < 8 && !ConflictsWith(atomic_, known->accesses))) && !ConflictsWith(known_, known->accesses));
    });
    --atomic_waiters_;
    --writers_waiting_;
    if (cancelled_) return {};
    ++serving_atomic_;
    size_t peers = 0;
    for (auto *entry = known_; entry; entry = entry->next) ++peers;
    ObserveQueue(0);
    const auto atomic_peers = atomic_count_++;
    Link(atomic_, known.get());
    // Wake the next phase and ordinary waiters after publishing the new domain.
    changed_.notify_all();
    return Ticket(this, Kind::Atomic, std::move(known), peers + atomic_peers, 0, atomic_peers);
  }
  bool WantsKnownAccess() {
    std::lock_guard lock(mutex_);
    return atomic_ || atomic_waiters_;
  }
  void Cancel() {
    { std::lock_guard lock(mutex_); cancelled_ = true; }
    changed_.notify_all();
  }
};

} // namespace d3dalloy
