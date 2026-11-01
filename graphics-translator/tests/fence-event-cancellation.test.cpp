#include "d3dalloy_fence_event.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <new>
#include <thread>

namespace {
thread_local int fail_after = -1;
thread_local bool forbid_allocation = false;
}

void *operator new(std::size_t size) {
  if (forbid_allocation || fail_after == 0)
    throw std::bad_alloc();
  if (fail_after > 0)
    --fail_after;
  if (void *memory = std::malloc(size ? size : 1))
    return memory;
  throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace owned_graphics;
using namespace std::chrono_literals;

void require(bool value, const char *what) {
  if (!value) {
    std::fprintf(stderr, "FENCE_EVENT_CANCELLATION_FAIL %s\n", what);
    std::exit(1);
  }
}

template <typename T> void await(std::future<T> &result) {
  require(result.wait_for(5s) == std::future_status::ready, "bounded event wait");
  result.get();
}

void testDispatchAndDestruction() {
  auto order = std::make_shared<FenceOrderEpoch>(1);
  uint64_t loser = 0;
  unsigned fired = 0;
  require(order->addCompletionWaiter(1, [&] {
    require(order->completedValue() == 1, "reentrant value");
    order->removeCompletionWaiter(loser);
    ++fired;
  }), "register reentrant");
  require(order->addCompletionWaiter(2, [] {}, &loser), "register loser");
  for (unsigned i = 0; i < 4096; ++i)
    require(order->addCompletionWaiter(1, [&] { ++fired; }), "register batch");
  forbid_allocation = true;
  order->signalCpu(1);
  forbid_allocation = false;
  require(fired == 4097, "allocation-free batch");
  require(order->addCompletionWaiter(1, [&] { order->signalCpu(0); ++fired; }), "immediate reentrant");
  require(order->completedValue() == 0 && fired == 4098, "atomic immediate crossing");

  struct ReenterOnDestroy {
    FenceOrderEpoch *order;
    ~ReenterOnDestroy() { order->completedValue(); }
  };
  auto owner = std::make_shared<ReenterOnDestroy>();
  owner->order = order.get();
  uint64_t id = 0;
  require(order->addCompletionWaiter(10, [owner] {}, &id), "register destructor");
  owner.reset();
  order->removeCompletionWaiter(id);
  std::puts("FENCE_DISPATCH_PASS batch=4096 allocation_free=1 reentrant=1 destructor=1");
}

void testImmediateAndRollback() {
  auto done = std::make_shared<FenceOrderEpoch>(2, 1);
  auto never = std::make_shared<FenceOrderEpoch>(3);
  unsigned fired = 0;
  for (unsigned i = 0; i < 256; ++i) {
    FenceWaitValue values[] = {FenceWaitValue::prepare(done, 1), FenceWaitValue::prepare(never, 1)};
    std::weak_ptr<FenceCompletionLatch> losing = values[1].latch;
    require(FenceEventRegistration::set(values, 2, false, [&] { ++fired; }), "immediate ANY");
    values[1].latch.reset();
    require(losing.expired(), "ANY retained losing subscription");
  }
  require(fired == 256, "ANY one-shot");
  auto single = FenceWaitValue::prepare(never, 2);
  require(FenceEventRegistration::set(&single, 1, true, [&] { ++fired; }), "single asynchronous");
  never->signalCpu(2);
  never->signalCpu(0);
  require(fired == 257 && single.latch->ready(), "single crossing rewind");
  auto late = FenceWaitValue::prepare(never, 2);
  require(!late.latch->ready(), "late registration inherited crossing");
  late.subscription.cancel();

  // Fail each production setup allocation, including partial callback installation.
  bool completed = false;
  unsigned failures = 0;
  for (int n = 0; n < 32 && !completed; ++n) {
    FenceWaitValue values[] = {FenceWaitValue::prepare(never, 9), FenceWaitValue::prepare(never, 9)};
    std::weak_ptr<FenceCompletionLatch> first = values[0].latch, second = values[1].latch;
    fail_after = n;
    completed = FenceEventRegistration::set(values, 2, false, [&] { ++fired; });
    fail_after = -1;
    if (completed)
      never->signalCpu(9);
    else
      ++failures;
    values[0].latch.reset();
    values[1].latch.reset();
    require(first.expired() && second.expired(), "setup rollback retained epoch wait");
  }
  require(completed && failures >= 5, "setup fault coverage");
  never->signalCpu(0);
  for (int n = 0; n < 6; ++n) {
    std::vector<FenceWaitValue> prepared;
    std::weak_ptr<FenceCompletionLatch> first;
    try {
      prepared.push_back(FenceWaitValue::prepare(never, 10));
      first = prepared[0].latch;
      fail_after = n;
      prepared.push_back(FenceWaitValue::prepare(never, 10));
    } catch (const std::bad_alloc &) {}
    fail_after = -1;
    prepared.clear();
    require(first.expired(), "partial preparation rollback");
  }
  std::printf("FENCE_EVENT_ROLLBACK_PASS immediate_any=256 single=1 setup_failures=%u partial_prepare=1\n", failures);
}

void testArmingAndDetachedRaces() {
  auto order = std::make_shared<FenceOrderEpoch>(4);
  auto value = FenceWaitValue::prepare(order, 1);
  unsigned fired = 0;
  auto registration = std::make_shared<FenceEventRegistration>(1, true, [&] { ++fired; });
  registration->take(std::move(value.subscription));
  require(value.latch->addWaiter([registration] { registration->reach(0); }), "attach before arm");
  order->signalCpu(1);
  require(fired == 0, "fired before arm");
  registration->arm();
  registration->arm();
  require(fired == 1, "arming lost or duplicated completion");

  order->signalCpu(0);
  FenceWaitValue duplicates[] = {FenceWaitValue::prepare(order, 1), FenceWaitValue::prepare(order, 1)};
  require(FenceEventRegistration::set(duplicates, 2, false, [&] { ++fired; }), "duplicate fence ANY");
  order->signalCpu(1);
  require(fired == 2, "same epoch detached losers duplicated event");

  order->signalCpu(0);
  std::promise<void> detached, release;
  auto detached_event = detached.get_future(), release_event = release.get_future();
  require(order->addCompletionWaiter(1, [&] { detached.set_value(); await(release_event); }), "detach gate");
  FenceWaitValue values[] = {FenceWaitValue::prepare(order, 1), FenceWaitValue::prepare(order, 1)};
  auto cancelled = std::make_shared<FenceEventRegistration>(2, false, [&] { ++fired; });
  for (unsigned i = 0; i < 2; ++i) {
    cancelled->take(std::move(values[i].subscription));
    require(values[i].latch->addWaiter([cancelled, i] { cancelled->reach(i); }), "detached listener");
  }
  cancelled->arm();
  auto worker = std::async(std::launch::async, [&] { order->signalCpu(1); });
  await(detached_event);
  cancelled->cancel();
  release.set_value();
  await(worker);
  require(fired == 2, "cancelled detached callback fired");
  std::puts("FENCE_EVENT_RACE_PASS before_arm=1 detached_cancel=1");
}

void testConcurrentOneShot() {
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    auto order = std::make_shared<FenceOrderEpoch>(7);
    auto value = FenceWaitValue::prepare(order, 1);
    std::atomic<unsigned> fired = 0;
    auto registration = std::make_shared<FenceEventRegistration>(1, true, [&] { ++fired; });
    registration->take(std::move(value.subscription));
    require(value.latch->addWaiter([registration] { registration->reach(0); }), "arming race listener");
    std::promise<void> start;
    auto gate = start.get_future().share();
    auto arm = std::async(std::launch::async, [&] { gate.wait(); registration->arm(); });
    auto signal = std::async(std::launch::async, [&] { gate.wait(); order->signalCpu(1); });
    start.set_value();
    await(arm); await(signal);
    require(fired == 1, "arming race lost or duplicated event");
  }
  for (bool wait_all : {false, true}) {
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
      auto a = std::make_shared<FenceOrderEpoch>(5), b = std::make_shared<FenceOrderEpoch>(6);
      FenceWaitValue values[] = {FenceWaitValue::prepare(a, 1), FenceWaitValue::prepare(b, 1)};
      std::atomic<unsigned> fired = 0;
      require(FenceEventRegistration::set(values, 2, wait_all, [&] { ++fired; }), "concurrent registration");
      std::promise<void> start;
      auto gate = start.get_future().share();
      auto producer = a->prepare(1);
      auto first = std::async(std::launch::async, [&] { gate.wait(); a->complete(producer); });
      auto duplicate = std::async(std::launch::async, [&] { gate.wait(); a->complete(producer); });
      auto second = std::async(std::launch::async, [&] { gate.wait(); b->signalCpu(1); });
      start.set_value();
      await(first); await(duplicate); await(second);
      a->remove(); b->remove();
      require(fired == 1, "concurrent event duplicated");
    }
  }
  std::puts("FENCE_EVENT_CONCURRENT_PASS arm=64 any=64 all=64 duplicate_producer=1 removal=1");
}
} // namespace

int main() {
  testDispatchAndDestruction();
  testImmediateAndRollback();
  testArmingAndDetachedRaces();
  testConcurrentOneShot();
  std::puts("FENCE_EVENT_CANCELLATION_PASS");
}
