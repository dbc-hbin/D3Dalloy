// A queued entry may already be running on a synchronous PSO consumer.
// The sole worker must make progress on the next job before that consumer finishes.
#include "owned_async_pipeline.hpp"
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

int main() {
  using namespace owned_graphics;
  using namespace std::chrono_literals;
  AsyncCompileQueue queue;
  std::promise<void> first_started, release_first, second_finished;
  auto started = first_started.get_future();
  auto release = release_first.get_future().share();
  auto second_done = second_finished.get_future();
  std::atomic<unsigned> first_calls = 0, second_calls = 0;
  auto first = std::make_shared<AsyncCompileJob>([&] {
    ++first_calls;
    first_started.set_value();
    release.wait();
    return true;
  });
  auto second = std::make_shared<AsyncCompileJob>([&] {
    ++second_calls;
    second_finished.set_value();
    return true;
  });
  queue.Submit(first);
  queue.Submit(second);
  bool consumer_ready = false;
  std::thread consumer([&] { consumer_ready = first->Wait(); });
  if (started.wait_for(5s) != std::future_status::ready) {
    release_first.set_value();
    consumer.join();
    queue.Stop();
    std::fprintf(stderr, "FAIL: synchronous consumer did not start\n");
    return 1;
  }
  std::thread worker([&] { queue.RunWorker(); });
  // This deadline is a deadlock watchdog, not a compile-time performance gate.
  const bool progressed = second_done.wait_for(5s) == std::future_status::ready;
  const bool first_pending = first->status() == AsyncCompileJob::Pending;
  release_first.set_value();
  consumer.join();
  queue.Stop();
  worker.join();
  if (!progressed || !first_pending || !consumer_ready || first_calls != 1 || second_calls != 1 ||
      first->status() != AsyncCompileJob::Ready || second->status() != AsyncCompileJob::Ready) {
    std::fprintf(stderr, "FAIL: worker waited for consumer-owned job (progress=%d, calls=%u,%u)\n",
                 int(progressed), first_calls.load(), second_calls.load());
    return 1;
  }
  std::puts("PASS: worker progresses while consumer-owned job is running; each job runs once");
}
