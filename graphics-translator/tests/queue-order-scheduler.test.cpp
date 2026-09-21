#include "dxmt_queue_order.hpp"
#include "owned_queue_task_fifo.hpp"
#include "owned_submission_ring.hpp"
#include "owned_submission_storage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using owned_graphics::FenceCompletionLatch;
using owned_graphics::FenceOrderEpoch;
using owned_graphics::QueueTaskFifo;
using owned_graphics::SubmissionRing;
using owned_graphics::StableSubmissionStorage;
using namespace std::chrono_literals;

[[noreturn]] void fail(const char *what) {
  std::fprintf(stderr, "QUEUE_ORDER_SCHEDULER_FAIL step=%s\n", what);
  std::exit(1);
}

void require(bool value, const char *what) {
  if (!value)
    fail(what);
}

void testLogicalFenceRewindAndOverlap() {
  FenceOrderEpoch timeline(11, 10);
  timeline.signalCpu(5);
  require(timeline.completedValue() == 5, "CPU rewind did not become current completion");

  bool blocked_done = false;
  bool qualifying_done = false;
  auto blocked = timeline.prepare(5, [&] { return blocked_done; });
  auto qualifying = timeline.prepare(10, [&] { return qualifying_done; });
  timeline.publish(blocked);
  timeline.publish(qualifying);

  auto latch = std::make_shared<FenceCompletionLatch>();
  require(timeline.addCompletionWaiter(10, [latch] { latch->signal(); }), "future completion waiter allocation failed");
  require(!latch->ready(), "future waiter used stale initial completion after rewind");

  qualifying_done = true;
  timeline.reconcile();
  require(latch->ready(), "qualifying producer was pinned behind blocked producer");
  require(timeline.completedValue() == 10, "actual qualifying completion was not published");

  blocked_done = true;
  timeline.reconcile();
  require(timeline.completedValue() == 5, "late rewind completion did not become current value");
  require(latch->ready(), "completion latch was undone by rewind");
  std::puts("QUEUE_ORDER_LOGICAL_PASS cpu_rewind=1 overlap=1 any_producer=1 latch=1");
}

void testCompletionRegistrationAndRollback() {
  FenceOrderEpoch timeline(12, 0);
  bool cancelled_done = false;
  auto cancelled = timeline.prepare(99, [&] { return cancelled_done; });
  timeline.cancel(cancelled);
  cancelled_done = true;
  timeline.reconcile();
  require(timeline.completedValue() == 0, "cancelled enqueue changed logical completion");

  auto latch = std::make_shared<FenceCompletionLatch>();
  require(timeline.addCompletionWaiter(7, [latch] { latch->signal(); }), "completion registration failed");
  timeline.signalCpu(7);
  timeline.signalCpu(2);
  require(latch->ready(), "registered completion did not survive later rewind");
  require(timeline.completedValue() == 2, "CPU rewind final value mismatch");
  std::puts("QUEUE_ORDER_REGISTRATION_PASS rollback=1 crossing_latched=1 final=2");
}

void testCloseDrainsFifo() {
  QueueTaskFifo<unsigned> fifo;
  for (unsigned i = 0; i < 64; ++i)
    require(fifo.push(i), "FIFO rejected task before close");
  fifo.close();
  require(!fifo.push(64), "FIFO accepted task after close");

  std::vector<unsigned> drained;
  unsigned value = 0;
  while (fifo.waitPop(value))
    drained.push_back(value);
  require(drained.size() == 64, "FIFO did not drain all accepted tasks");
  for (unsigned i = 0; i < drained.size(); ++i)
    require(drained[i] == i, "FIFO order changed while draining");
  std::puts("QUEUE_ORDER_FIFO_PASS tasks=64 close=drain order=exact");
}

void testTaskRetainsSubmissionStorage() {
  struct Storage {
    explicit Storage(std::atomic<unsigned> &destroyed) : destroyed(destroyed) {}
    ~Storage() { destroyed.fetch_add(1); }
    std::atomic<unsigned> &destroyed;
    unsigned value = 0x1234u;
  };

  using Task = std::function<void()>;
  QueueTaskFifo<Task> fifo;
  std::atomic<unsigned> destroyed{0};
  std::atomic<unsigned> observed{0};
  auto storage = std::make_shared<Storage>(destroyed);
  require(fifo.push([storage, &observed] { observed.store(storage->value); }), "enqueue retained storage task");
  storage.reset();
  require(destroyed.load() == 0, "queued task did not retain submission storage");
  fifo.close();
  Task task;
  require(fifo.waitPop(task), "retained storage task missing");
  task();
  require(observed.load() == 0x1234u, "retained storage contents changed before worker consume");
  task = {};
  require(destroyed.load() == 1, "retained storage did not release after task lifetime");
  std::puts("QUEUE_ORDER_LIFETIME_PASS queued_storage=retained release=after_consume");
}

void testRecordHeadAddressStabilityAcrossListReset() {
  struct RecordHead {
    unsigned recording = 0;
  };

  StableSubmissionStorage<RecordHead> records;
  records.emplace_back(RecordHead{1});
  RecordHead *first = &records.front();
  for (unsigned recording = 2; recording <= 512; ++recording)
    records.emplace_back(RecordHead{recording});

  require(&records.front() == first, "new list recordings moved a submitted record head");
  require(first->recording == 1, "submitted record head contents changed after append");
  std::puts("QUEUE_ORDER_RECORD_STORAGE_PASS resets=511 first_entry_address=stable");
}

void testSubmissionRingDrainBeforeStop() {
  SubmissionRing<2> ring;
  const auto sequence = ring.reserve();
  require(sequence == 1, "unexpected first submission sequence");
  ring.publish(sequence);
  require(ring.lastPublished() == sequence, "lastPublished did not expose committed submission");

  std::thread completion([&] {
    require(ring.wait(sequence), "completion thread saw stop before published work");
    ring.retire(sequence);
    require(!ring.wait(sequence + 1), "completion thread did not observe stop sentinel");
  });

  ring.waitUntilRetired(sequence);
  ring.stop();
  completion.join();
  std::puts("QUEUE_ORDER_RING_DRAIN_PASS published=retired stop=after-drain");
}

void testFutureSignalDoesNotBlockCallerAndDefersConsumerEncoding() {
  using Task = std::function<void()>;
  QueueTaskFifo<Task> queue_a;
  QueueTaskFifo<Task> queue_b;
  auto producer = std::make_shared<FenceOrderEpoch>(21);
  auto producer_latch = std::make_shared<FenceCompletionLatch>();
  require(producer->addCompletionWaiter(1, [producer_latch] { producer_latch->signal(); }),
          "producer latch registration failed");
  std::mutex events_mutex;
  std::vector<std::string> events;

  auto record = [&](const char *name) {
    std::lock_guard lock(events_mutex);
    events.emplace_back(name);
  };
  auto worker = [](QueueTaskFifo<Task> &fifo) {
    Task task;
    while (fifo.waitPop(task))
      task();
  };

  std::thread worker_a(worker, std::ref(queue_a));
  std::thread worker_b(worker, std::ref(queue_b));

  // Problem ordering from the actual temporal deadlock report:
  //   B.Wait(F1) -> B.Execute(temporalB) -> A.Execute(temporalA) -> A.Signal(F1)
  // Enqueue calls all return; only B's worker waits. A is free to encode first.
  require(queue_b.push([&] {
    while (!producer_latch->waitFor(2ms))
      producer->reconcile();
    record("B_WAIT_ENCODE");
  }), "enqueue B.Wait");
  require(queue_b.push([&] { record("B_EXECUTE_TEMPORAL"); }), "enqueue B.Execute");
  require(queue_a.push([&] { record("A_EXECUTE_TEMPORAL"); }), "enqueue A.Execute");
  require(queue_a.push([&] {
    record("A_SIGNAL_COMMIT");
    producer->signalCpu(1);
  }), "enqueue A.Signal");

  queue_a.close();
  queue_b.close();
  worker_a.join();
  worker_b.join();

  auto index = [&](const char *name) {
    auto it = std::find(events.begin(), events.end(), name);
    require(it != events.end(), "expected scheduler event missing");
    return static_cast<std::size_t>(it - events.begin());
  };
  const auto a_execute = index("A_EXECUTE_TEMPORAL");
  const auto a_signal = index("A_SIGNAL_COMMIT");
  const auto b_wait = index("B_WAIT_ENCODE");
  const auto b_execute = index("B_EXECUTE_TEMPORAL");
  require(a_execute < a_signal, "queue A FIFO order changed");
  require(a_signal < b_wait, "consumer Wait encoded before producer Signal commit");
  require(b_wait < b_execute, "queue B encoded Execute before its deferred Wait");
  std::puts("QUEUE_ORDER_CROSSQUEUE_PASS caller_nonblocking=1 producer_completion_gate=1 fifo=1");
}

} // namespace

int main() {
  testLogicalFenceRewindAndOverlap();
  testCompletionRegistrationAndRollback();
  testCloseDrainsFifo();
  testTaskRetainsSubmissionStorage();
  testRecordHeadAddressStabilityAcrossListReset();
  testSubmissionRingDrainBeforeStop();
  testFutureSignalDoesNotBlockCallerAndDefersConsumerEncoding();
  std::puts("QUEUE_ORDER_SCHEDULER_PASS");
  return 0;
}
