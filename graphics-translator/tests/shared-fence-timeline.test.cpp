#include "dxmt_shared_fence.hpp"
#include "dxmt_queue_order.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *step) {
  if (!condition) {
    std::fprintf(stderr, "SHARED_FENCE_TIMELINE_FAIL step=%s\n", step);
    std::exit(1);
  }
}

void drain(dxmt::SharedFenceTimeline &shared, uint32_t index, owned_graphics::FenceOrderEpoch &local) {
  const auto observation = shared.take(index);
  local.signalCpu(observation.highest);
  local.signalCpu(observation.logical);
}

void testUnobservedRewindAndRegistration() {
  dxmt::SharedFenceTimeline shared{};
  const auto observer = shared.attach(1);
  owned_graphics::FenceOrderEpoch local(1, 0);
  bool event = false, queue = false, late = false;
  require(local.addCompletionWaiter(10, [&] { event = true; }), "register event");
  require(local.addCompletionWaiter(10, [&] { queue = true; }), "register queue wait");
  shared.publish(10);
  for (unsigned i = 0; i < 5000; ++i)
    shared.publish(0);
  // No observer read occurred during any of those publications.
  drain(shared, observer, local);
  require(event && queue, "peak was lost before delayed observer read");
  require(local.completedValue() == 0, "peak replaced current rewind value");
  require(local.addCompletionWaiter(10, [&] { late = true; }), "register late wait");
  drain(shared, observer, local);
  require(!late, "late wait inherited a pre-registration peak");
  shared.publish(10);
  drain(shared, observer, local);
  require(late, "late wait missed new qualifying completion");
  std::puts("SHARED_FENCE_UNOBSERVED_REWIND_PASS event=1 queue=1 late=1 publications=5002");
}

void testCompletionNotPreparationOrder() {
  dxmt::SharedFenceTimeline shared{};
  const auto observer = shared.attach(1);
  owned_graphics::FenceOrderEpoch producer(1, 0), local(2, 0);
  auto older = producer.prepare(10);
  auto newer = producer.prepare(3);
  bool reached = false;
  require(local.addCompletionWaiter(10, [&] { reached = true; }), "register delayed producer wait");
  // The second prepared producer completes first, then a CPU rewind, then the
  // first prepared producer. Only completion publishes shared state.
  producer.complete(newer);
  shared.publish(newer->value);
  shared.publish(1);
  drain(shared, observer, local);
  require(local.completedValue() == 1 && !reached, "uncompleted older producer affected shared completion");
  producer.complete(older);
  shared.publish(older->value);
  drain(shared, observer, local);
  require(reached && local.completedValue() == 10, "late earlier-prepared value was not current");
  shared.publish(0);
  drain(shared, observer, local);
  require(reached && local.completedValue() == 0, "later completion of earlier producer was discarded");
  std::puts("SHARED_FENCE_COMPLETION_ORDER_PASS delayed_older=1 cpu_overlap=1 latch=1");
}

void testIndependentAndNewObservers() {
  dxmt::SharedFenceTimeline shared{};
  auto first = shared.attach(1), second = shared.attach(2);
  shared.publish(UINT64_MAX);
  shared.publish(2);
  const auto a = shared.take(first);
  require(a.highest == UINT64_MAX && a.logical == 2, "unsigned maximum peak was lost");
  auto opened = shared.attach(3);
  const auto fresh = shared.take(opened);
  require(fresh.highest == 2 && fresh.logical == 2, "new observer inherited old peak");
  const auto b = shared.take(second);
  require(b.highest == UINT64_MAX && b.logical == 2, "one observer drain consumed another observer peak");
  shared.observers[first].pid = 0;
  auto reused = shared.attach(4);
  require(reused == first && shared.take(reused).highest == 2, "reused slot inherited detached peak");
  std::puts("SHARED_FENCE_OBSERVERS_PASS independent=1 fresh=1 reuse=1 uint64_max=1");
}

void testAbandonedPublicationRecovery() {
  dxmt::SharedFenceTimeline shared{};
  const auto first = shared.attach(1), second = shared.attach(2);
  // State at interruption halfway through a publisher's protected update.
  shared.pending_value = 11;
  shared.publishing = 1;
  shared.observers[first].highest = 11;
  shared.finishPublication();
  shared.finishPublication();
  shared.publish(0);
  const auto a = shared.take(first), b = shared.take(second);
  require(a.highest == 11 && b.highest == 11 && a.logical == 0 && b.logical == 0,
          "abandoned publication recovery lost a crossing");
  require(a.serial == 2, "completed recovery was replayed again");
  std::puts("SHARED_FENCE_RECOVERY_PASS partial_publication=1 idempotent=1 rewind=1");
}

void testObserverCapacityDoesNotBlockPublication() {
  dxmt::SharedFenceTimeline shared{};
  for (uint32_t i = 0; i < dxmt::SharedFenceTimeline::kObservers; ++i)
    require(shared.attach(i + 1) == int32_t(i), "attach observer at capacity boundary");
  require(shared.attach(9999) == -1, "observer capacity did not reject exhaustion");
  shared.publish(99);
  shared.publish(0);
  for (uint32_t i = 0; i < dxmt::SharedFenceTimeline::kObservers; ++i) {
    const auto observation = shared.take(i);
    require(observation.highest == 99 && observation.logical == 0, "full observer capacity blocked publication");
  }
  shared.observers[17].pid = 0;
  require(shared.attach(9999) == 17 && shared.take(17).highest == 0, "capacity reuse retained old crossing");
  std::puts("SHARED_FENCE_CAPACITY_PASS full=1024 publication=unblocked reuse=current");
}

} // namespace

int main() {
  testUnobservedRewindAndRegistration();
  testCompletionNotPreparationOrder();
  testIndependentAndNewObservers();
  testAbandonedPublicationRecovery();
  testObserverCapacityDoesNotBlockPublication();
  std::puts("SHARED_FENCE_TIMELINE_PASS");
}
