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

using owned_graphics::FenceOrderEpoch;
using owned_graphics::FenceSignalSchedule;
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

void testEpochCommitIsolation() {
  auto old_epoch = std::make_shared<FenceOrderEpoch>(11);
  auto new_epoch = std::make_shared<FenceOrderEpoch>(12);

  require(!old_epoch->ready(7), "old epoch unexpectedly ready");
  require(!old_epoch->waitFor(7, 1ms), "old epoch wait should time out before producer commit");

  old_epoch->markCommitted(7);
  require(old_epoch->ready(7), "producer commit did not make wait ready");
  require(old_epoch->committedValue() == 7, "committed value mismatch");

  // Epoch state is intentionally independent. Production currently rejects
  // decreasing signals rather than migrating live waits across epochs.
  require(!new_epoch->ready(7), "rewound epoch inherited old producer commit");
  new_epoch->markCommitted(3); // CPU Signal or committed queue Signal on new event.
  require(new_epoch->ready(3), "new epoch CPU signal was not visible");
  require(!new_epoch->ready(7), "new epoch incorrectly reached old value");

  // Equal queue signals are deliberately not dropped. If the first producer is
  // blocked, a second producer committing the same value can satisfy the wait.
  auto duplicate_epoch = std::make_shared<FenceOrderEpoch>(13);
  duplicate_epoch->markCommitted(9);
  require(duplicate_epoch->ready(9), "equal-value independent producer did not satisfy wait");
  std::puts("QUEUE_ORDER_EPOCH_PASS generations=isolated cpu_signal=ready equal_signal=ready");
}

void testExplicitRewindLimitation() {
  using Result = FenceSignalSchedule::Result;
  FenceSignalSchedule schedule;
  require(schedule.initialize(2) == Result::Accepted, "initial fence value rejected");
  require(schedule.scheduleQueue(100, 4, true) == Result::Accepted, "first queue fence value rejected");
  require(schedule.scheduleQueue(100, 4, false) == Result::Accepted, "equal-value producer was incorrectly rejected");
  require(schedule.scheduleQueue(100, 9, false) == Result::Accepted, "same-queue increasing fence value rejected");
  require(schedule.scheduleQueue(100, 8, false) == Result::DecreasingUnsupported,
          "decreasing fence value was silently accepted");
  require(schedule.scheduleQueue(200, 10, false) == Result::MixedProducerUnsupported,
          "second GPU producer queue was silently accepted");
  require(schedule.scheduleCpu(9, false) == Result::MixedProducerUnsupported,
          "CPU same-value override of GPU producer was silently accepted");
  require(schedule.lastScheduled() == 9, "rejected rewind changed scheduled value");
  require(schedule.scheduleQueue(200, 10, true) == Result::Accepted,
          "settled fence could not hand off to a second queue producer");
  require(schedule.scheduleCpu(11, true) == Result::Accepted,
          "settled fence could not hand off to a CPU producer");

  const auto before_failed_enqueue = schedule.state();
  require(schedule.scheduleCpu(12, true) == Result::Accepted, "test signal preparation rejected");
  schedule.restore(before_failed_enqueue);
  require(schedule.lastScheduled() == 11, "failed enqueue rollback did not restore fence schedule");
  std::puts("QUEUE_ORDER_REWIND_LIMIT_PASS decreasing=reject overlap=reject settled_handoff=allowed");
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
    while (!producer->ready(1))
      producer->waitFor(1, 2ms);
    record("B_WAIT_ENCODE");
  }), "enqueue B.Wait");
  require(queue_b.push([&] { record("B_EXECUTE_TEMPORAL"); }), "enqueue B.Execute");
  require(queue_a.push([&] { record("A_EXECUTE_TEMPORAL"); }), "enqueue A.Execute");
  require(queue_a.push([&] {
    record("A_SIGNAL_COMMIT");
    producer->markCommitted(1);
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
  std::puts("QUEUE_ORDER_CROSSQUEUE_PASS caller_nonblocking=1 producer_commit_gate=1 fifo=1");
}

} // namespace

int main() {
  testEpochCommitIsolation();
  testExplicitRewindLimitation();
  testCloseDrainsFifo();
  testTaskRetainsSubmissionStorage();
  testRecordHeadAddressStabilityAcrossListReset();
  testSubmissionRingDrainBeforeStop();
  testFutureSignalDoesNotBlockCallerAndDefersConsumerEncoding();
  std::puts("QUEUE_ORDER_SCHEDULER_PASS");
  return 0;
}
