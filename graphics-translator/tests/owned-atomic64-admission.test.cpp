#include "../../src/d3d12/owned_gpu_execution_gate.hpp"
#include "../../src/d3d12/owned_atomic64_constants.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>

using namespace d3dalloy;
using namespace std::chrono_literals;

static void Require(bool value, const char *what, GPUExecutionGate *gate = nullptr) {
  if (value) return;
  if (gate) gate->Cancel();
  throw std::runtime_error(what);
}
static GPUExecutionGate::Accesses Range(uintptr_t owner, uint64_t begin = 0, uint64_t end = 64,
    PhysicalDomain domain = PhysicalDomain::Buffer, const void *logical = nullptr) {
  auto accesses = std::make_shared<std::vector<SubmissionAccess>>();
  accesses->push_back({logical, begin, end, 0, 1, 0, 1, 0, true, false, 0, {domain, owner}});
  return accesses;
}
template<class F> static auto Start(F function) {
  std::promise<void> started;
  auto entered = started.get_future();
  auto result = std::async(std::launch::async, [function = std::move(function), started = std::move(started)]() mutable {
    started.set_value();
    return function();
  });
  entered.wait();
  return result;
}
template<class T> static void Ready(std::future<T> &future, GPUExecutionGate &gate, const char *what) {
  Require(future.wait_for(2s) == std::future_status::ready, what, &gate);
}
template<class T> static void Blocked(std::future<T> &future, GPUExecutionGate &gate, const char *what) {
  Require(future.wait_for(25ms) == std::future_status::timeout, what, &gate);
}

static void Admission() {
  {
    GPUExecutionGate gate;
    auto first = gate.AcquireShared(7);
    auto fifo = gate.AcquireKnownShared(Range(1), 7);
    Require(first.OrderedAdmissionSerial() && fifo.OrderedAdmissionSerial() == first.OrderedAdmissionSerial() + 1,
        "same queue preserves order with live tickets");
    auto overlap = gate.AcquireShared(8);
    Require(!overlap.OrderedAdmissionSerial(), "overlapping foreign queue invalidates order");
    first.Reset();
    fifo.Reset();
    overlap.Reset();
    auto producer = gate.AcquireShared(8);
    const auto serial = producer.OrderedAdmissionSerial();
    producer.Reset();
    auto consumer = gate.AcquireShared(7);
    Require(serial && consumer.OrderedAdmissionSerial() == serial + 1, "retired foreign queue preserves order");
    consumer.Reset();
    auto auxiliary = gate.AcquireShared();
    Require(!auxiliary.OrderedAdmissionSerial(), "anonymous admission cannot carry proof");
    auxiliary.Reset();
    Require(gate.AcquireShared(7).OrderedAdmissionSerial() == serial + 3, "auxiliary admission leaves a serial gap");
  }
  {
    GPUExecutionGate gate;
    auto first = gate.AcquireAtomic(Range(11), true);
    auto other = Start([&] { return gate.AcquireAtomic(Range(12), true); });
    Ready(other, gate, "independent private domains can coexist");
    auto second = other.get();
    Require(second.AtomicPeers() == 1 && first.AtomicPeers() == 0, "private phase peer count", &gate);
    auto conflict = Start([&] { return gate.AcquireAtomic(Range(11, 48, 96), true); });
    Blocked(conflict, gate, "overlap with non-head phase must wait");
    first.Reset();
    Ready(conflict, gate, "remaining disjoint phase must not delay the successor");
    auto third = conflict.get();
    Require(third.AtomicPeers() == 1, "successor retains independent peer", &gate);
    auto shared = Start([&] { return gate.AcquireKnownShared(Range(12)); });
    Blocked(shared, gate, "ordinary participant checks every phase");
    second.Reset();
    Ready(shared, gate, "ordinary resumes when actual conflicting phase retires");
    auto ordinary = shared.get();
    auto legacy = Start([&] { return gate.AcquireAtomic(Range(13)); });
    Blocked(legacy, gate, "non-private atomic waits for every other phase");
    third.Reset();
    Ready(legacy, gate, "legacy atomic resumes after private retirement");
    auto serial = legacy.get();
    auto later = Start([&] { return gate.AcquireAtomic(Range(14), true); });
    Blocked(later, gate, "private domain cannot overlap legacy atomic in reverse");
    serial.Reset();
    Ready(later, gate, "private successor resumes after serial retirement");
  }
  {
    GPUExecutionGate gate;
    auto ordinary = gate.AcquireKnownShared(Range(1));
    auto pending = Start([&] { return gate.AcquireAtomic(Range(2)); });
    Ready(pending, gate, "disjoint ordinary must not block Atomic64");
    auto atomic = pending.get();
    Require(atomic && atomic.IndependentPeers() == 1, "Atomic64 retains one independent peer", &gate);
    auto peer = Start([&] { return gate.AcquireKnownShared(Range(3)); });
    Ready(peer, gate, "disjoint ordinary must enter an active Atomic64 phase");
    auto reverse = peer.get();
    Require(reverse && reverse.IndependentPeers() == 1, "reverse admission observes the active SC owner", &gate);
    auto next = Start([&] { return gate.AcquireAtomic(Range(4)); });
    Blocked(next, gate, "disjoint Atomic64 phases must retain one SC domain");
    atomic.Reset();
    Ready(next, gate, "SC successor must advance at retirement");
    auto successor = next.get();
    Require(successor && successor.IndependentPeers() == 2, "ordinary owners survive Atomic64 handoff", &gate);
  }
  {
    GPUExecutionGate gate;
    auto ordinary = gate.AcquireKnownShared(Range(5, 8, 24));
    auto pending = Start([&] { return gate.AcquireAtomic(Range(5, 16, 32)); });
    Blocked(pending, gate, "overlapping physical range must wait");
    auto retirement_owner = std::move(ordinary);
    Require(!ordinary && retirement_owner, "moving a ticket transfers retirement ownership", &gate);
    Blocked(pending, gate, "commit-side ticket move must not release the range");
    retirement_owner.Reset();
    Ready(pending, gate, "overlapping Atomic64 must resume after retirement");
    auto atomic = pending.get();
    auto ordinary_pending = Start([&] { return gate.AcquireKnownShared(Range(5, 0, 24)); });
    Blocked(ordinary_pending, gate, "ordinary physical aliases must wait for export retirement");
    atomic.Reset();
    Ready(ordinary_pending, gate, "ordinary alias must resume after export retirement");
    Require(bool(ordinary_pending.get()), "ordinary alias receives a live ticket", &gate);
  }
  {
    GPUExecutionGate gate;
    auto ordinary = gate.AcquireShared();
    auto pending = Start([&] { return gate.AcquireAtomic(Range(7)); });
    Blocked(pending, gate, "unresolved ordinary access excludes every Atomic64 phase");
    ordinary.NarrowShared(Range(6));
    Ready(pending, gate, "a complete disjoint proof may narrow a live ordinary ticket");
    auto atomic = pending.get();
    Require(ordinary.IsKnown() && atomic.IndependentPeers() == 1, "narrowing keeps the original owner live", &gate);
    auto legacy = Start([&] { return gate.AcquireExclusive(); });
    Blocked(legacy, gate, "legacy stages exclude a scoped Atomic64 owner");
    atomic.Reset();
    Blocked(legacy, gate, "legacy stages also exclude known ordinary owners");
    ordinary.Reset();
    Ready(legacy, gate, "legacy exclusion resumes after all owners retire");
    Require(bool(legacy.get()), "legacy receives an exclusive ticket", &gate);
  }
  {
    GPUExecutionGate gate;
    int first = 0, alias = 0;
    auto ordinary = gate.AcquireKnownShared(Range(8, 0, 4096, PhysicalDomain::Heap, &first));
    auto pending = Start([&] { return gate.AcquireAtomic(Range(8, 2048, 6144, PhysicalDomain::Heap, &alias)); });
    Blocked(pending, gate, "different logical resources still conflict in one physical heap span");
    ordinary.Reset();
    Ready(pending, gate, "heap alias must resume after its physical owner retires");
    Require(bool(pending.get()), "heap overlap receives a ticket after retirement", &gate);
  }
  {
    GPUExecutionGate gate;
    auto ordinary = gate.AcquireShared();
    auto pending = Start([&] { return gate.AcquireAtomic(Range(9)); });
    Blocked(pending, gate, "cancel probe starts with an unresolved owner");
    gate.Cancel();
    Ready(pending, gate, "device removal must wake an Atomic64 waiter");
    Require(!pending.get(), "device removal cannot grant a new Atomic64 owner", &gate);
    Require(!gate.AcquireKnownShared(Range(10)), "device removal cannot grant ordinary admission", &gate);
    ordinary.Reset();
  }
}

static void Constants() {
  struct Relocation { uint64_t Offset, TargetOffset; };
  constexpr uint64_t context_target = ~uint64_t(1), lock_target = ~uint64_t(0);
  const std::array<Relocation, 5> relocations{{{0, 16}, {8, lock_target}, {16, context_target},
      {24, context_target}, {41, 64}}};
  std::array<uint8_t, 64> original{}, first{}, second{};
  for (size_t index = 0; index < original.size(); ++index) original[index] = uint8_t(index * 17 + 3);
  const auto snapshot = original;
  auto copy = [&](auto &destination, uint64_t base, uint64_t context) {
    return CopyAtomic64Constants(destination.data(), original.data(), original.size(), base, context,
        std::span<const Relocation>(relocations), context_target, lock_target);
  };
  Require(copy(first, 0x10000, 0x20000) && copy(second, 0x30000, 0x40000), "independent constant snapshots");
  auto word = [](const auto &bytes, size_t offset) {
    uint64_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  };
  Require(original == snapshot, "a private snapshot cannot modify the immutable PSO");
  Require(word(first, 0) == 0x10010 && word(second, 0) == 0x30010, "pool-relative references must use their own base");
  Require(word(first, 8) == word(original, 8) && word(second, 8) == word(original, 8), "absolute lock pointers retain exact bits");
  Require(word(first, 16) == 0x20000 && word(first, 24) == 0x20000 &&
      word(second, 16) == 0x40000 && word(second, 24) == 0x40000, "every context relocation must be execution-local");
  Require(word(first, 41) == 0x10040 && word(second, 41) == 0x30040, "unaligned and one-past relocations preserve their arithmetic");
  for (size_t index = 32; index < original.size(); ++index)
    if (index < 41 || index >= 49)
      Require(first[index] == original[index] && second[index] == original[index], "unrelocated payload bytes remain identical");
  const auto before_failure = first;
  const std::array<Relocation, 2> invalid{{{16, context_target}, {57, 0}}};
  Require(!CopyAtomic64Constants(first.data(), original.data(), original.size(), 0x10000, 0x20000,
      std::span<const Relocation>(invalid), context_target, lock_target) && first == before_failure,
      "a truncated relocation must fail before writing any destination bytes");
}

int main() try {
  Admission();
  Constants();
  std::puts("OWNED_ATOMIC64_ADMISSION_PASS bidirectional=2 overlap=2 sc_domain=1 unknown=1 retirement=1 heap_alias=1 cancel=1 constants=2 private_phase_pair=1 non_head_conflict=1");
  return 0;
} catch (const std::exception &error) {
  std::fprintf(stderr, "OWNED_ATOMIC64_ADMISSION_FAIL %s\n", error.what());
  return 1;
}
