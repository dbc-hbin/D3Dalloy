#include "owned_low_latency.hpp"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <thread>
#include <vector>

namespace {
using owned_graphics::LowLatencyController;
using Scheduler = owned_graphics::LowLatencyScheduler;
using namespace std::chrono_literals;
void require(bool condition, const char *name) {
  if (!condition) { std::fprintf(stderr, "LOW_LATENCY_FAIL %s\n", name); std::exit(1); }
}
template <typename T> T await(std::future<T> &future) {
  require(future.wait_for(5s) == std::future_status::ready, "bounded event completion");
  return future.get();
}
struct Events {
  std::vector<Scheduler::Event> events;
  std::promise<void> waiting;
  bool subscribed = false;
  bool capacity_only = true;
  unsigned waits_remaining = 1;
  static void Observe(void *context, const Scheduler::Event &event) {
    auto &self = *static_cast<Events *>(context);
    self.events.push_back(event);
    if (self.subscribed && event.kind == Scheduler::EventKind::Wait && (!self.capacity_only || event.capacity)) {
      if (!--self.waits_remaining) {
        self.subscribed = false;
        self.waiting.set_value();
      }
    }
  }
  size_t Count(Scheduler::EventKind kind, Scheduler::Source source = Scheduler::Source::Forced) const {
    size_t result = 0;
    for (const auto &event : events) if (event.kind == kind && event.source == source) ++result;
    return result;
  }
};

struct Sample { double latency, interval; };
Sample simulate(bool adaptive, uint64_t cpu, uint64_t gpu) {
  LowLatencyController controller;
  struct Completion { uint64_t id, at, presented; };
  std::deque<Completion> pending;
  std::vector<uint64_t> ends, latencies;
  uint64_t now = 1000000000, gpu_free = 0, cpu_estimate = 0, closed_at = 0, close_duration = 0;
  for (uint64_t frame = 1; frame <= 600; ++frame) {
    if (ends.size() >= 2) now = std::max(now, ends[ends.size() - 2]);
    while (!pending.empty() && pending.front().at <= now) {
      auto completion = pending.front(); pending.pop_front();
      controller.End(completion.id, completion.at);
      close_duration = completion.at - completion.presented;
    }
    uint64_t begin = now;
    if (adaptive) {
      auto target = controller.Target(frame);
      auto drain = pending.empty() || !close_duration ? 0 : closed_at + close_duration;
      auto overlap = drain > cpu_estimate ? drain - cpu_estimate : 0;
      begin = std::max(now, std::min(target, overlap));
      controller.Begin(frame, target, begin);
      cpu_estimate = cpu;
    }
    const auto submitted = begin + cpu;
    const auto end = std::max(submitted, gpu_free) + gpu;
    gpu_free = end;
    if (adaptive) pending.push_back({frame, end, submitted});
    closed_at = submitted;
    ends.push_back(end);
    latencies.push_back(end - begin);
    now = submitted;
  }
  double total = 0;
  for (size_t i = 300; i < latencies.size(); ++i) total += double(latencies[i]);
  return {total / 300, double(ends.back() - ends[300]) / 299};
}
void adaptiveOverlap() {
  const auto fixed = simulate(false, 2000000, 20000000);
  const auto adaptive = simulate(true, 2000000, 20000000);
  require(fixed.latency > 36000000 && adaptive.latency < 30000000, "adaptive drains GPU-bound queue");
  require(adaptive.interval < fixed.interval * 1.03, "GPU throughput preserved");
  const auto cpu_fixed = simulate(false, 16000000, 4000000);
  const auto cpu_adaptive = simulate(true, 16000000, 4000000);
  require(cpu_adaptive.interval < cpu_fixed.interval * 1.03 && cpu_adaptive.latency < cpu_fixed.latency * 1.03,
          "CPU-bound frames retain overlap");
}
void outOfOrderGpuTimingDoesNotRewindPrediction() {
  LowLatencyController controller;
  controller.Begin(1, 0, 1000000);
  controller.Begin(2, 0, 2000000);
  controller.End(1, 20000000);
  auto ordered = controller;
  controller.End(2, 15000000);
  require(controller.Target(3) == ordered.Target(3),
          "later retirement with earlier GPU completion cannot rewind prediction");
}
void sharedOwnershipAndFragments() {
  Events events;
  Scheduler scheduler(true, Events::Observe, &events);
  require(scheduler.SetMode(Scheduler::Source::Reflex, true, 0) == Scheduler::Result::Ready, "Reflex mode");
  require(events.Count(Scheduler::EventKind::Admit, Scheduler::Source::Reflex) == 0, "mode does not own admission");
  require(scheduler.Sleep() == Scheduler::Result::Ready, "Reflex admission");
  auto execute = scheduler.Capture();
  auto fragment = execute;
  scheduler.SetMode(Scheduler::Source::AntiLag, true, 0);
  require(scheduler.Sleep(Scheduler::Source::AntiLag) == Scheduler::Result::Ready, "AMD shares reservation");
  auto generated = scheduler.Capture(true);
  auto real = scheduler.Capture(true);
  require(execute->frame == generated->frame && generated->frame == real->frame, "generated real group");
  scheduler.Close(real);
  auto frame = real->frame;
  scheduler.Complete(generated, true); generated.reset();
  scheduler.Complete(real, true); real.reset();
  scheduler.Complete(execute, true); execute.reset();
  require(events.Count(Scheduler::EventKind::Complete) == 0, "last native fragment still owns frame");
  scheduler.Complete(fragment, true); fragment.reset();
  require(events.Count(Scheduler::EventKind::Complete) == 1, "one completion per application frame");
  require(scheduler.PresentReturn(frame, false) == Scheduler::Result::Ready, "cooperative Present return");
  require(events.Count(Scheduler::EventKind::Admit) == 0 &&
          events.Count(Scheduler::EventKind::Admit, Scheduler::Source::Reflex) == 1 &&
          events.Count(Scheduler::EventKind::Admit, Scheduler::Source::AntiLag) == 0, "no duplicate SDK or forced admission");
  // No Sleep for the next frame: force ownership returns on its real Present.
  auto next = scheduler.Capture(true);
  scheduler.Close(next);
  auto next_frame = next->frame;
  scheduler.Complete(next, true); next.reset();
  require(scheduler.PresentReturn(next_frame, false) == Scheduler::Result::Ready, "force returns when Sleep stops");
  require(events.Count(Scheduler::EventKind::Admit) == 1, "forced next-input admission");
}
void asynchronousPresenter() {
  Events events;
  Scheduler scheduler(false, Events::Observe, &events);
  scheduler.SetMode(Scheduler::Source::AntiLag, true, 0);
  require(scheduler.Sleep(Scheduler::Source::AntiLag) == Scheduler::Result::Ready, "first CPU reservation");
  auto first_render = std::async(std::launch::async, [&] {
    auto work = scheduler.Capture(); scheduler.EndFrame(); scheduler.EndFrame(); return work;
  });
  auto first = await(first_render);
  require(scheduler.Sleep(Scheduler::Source::AntiLag) == Scheduler::Result::Ready, "second CPU reservation");
  auto second_render = std::async(std::launch::async, [&] {
    auto work = scheduler.Capture(); scheduler.EndFrame(); return work;
  });
  auto second = await(second_render);
  require(first->frame != second->frame, "CPU frames remain distinct before presentation");
  auto presenter = std::async(std::launch::async, [&] {
    auto generated = scheduler.Capture(true);
    auto real = scheduler.Capture(true);
    require(generated->frame == first->frame && real->frame == first->frame, "oldest CPU frame presented first");
    scheduler.Close(real);
    scheduler.Complete(generated, true); scheduler.Complete(real, true);
    auto following = scheduler.Capture(true);
    require(following->frame == second->frame, "following real belongs to second CPU frame");
    scheduler.Close(following); scheduler.Complete(following, true);
  });
  await(presenter);
  scheduler.Complete(second, true); second.reset();
  require(events.Count(Scheduler::EventKind::Complete) == 0, "out of order retirement retains oldest frame");
  scheduler.Complete(first, true); first.reset();
  require(events.Count(Scheduler::EventKind::Complete) == 2, "ordered exact frame retirement");
}
void modeDoesNotTakeOwnership() {
  Events events;
  Scheduler scheduler(false, Events::Observe, &events);
  scheduler.SetMode(Scheduler::Source::Reflex, true, 0);
  auto work = scheduler.Capture(true);
  scheduler.Close(work);
  auto frame = work->frame;
  scheduler.Complete(work, true); work.reset();
  require(scheduler.PresentReturn(frame, false) == Scheduler::Result::Ready, "mode-only Present returns");
  require(events.Count(Scheduler::EventKind::Admit) == 1, "game-requested Reflex fallback works without launcher force");
  scheduler.SetMode(Scheduler::Source::AntiLag, true, 0);
  scheduler.SetMode(Scheduler::Source::Reflex, false, 0);
  require(scheduler.Sleep(Scheduler::Source::AntiLag) == Scheduler::Result::Ready, "Reflex disable preserves AMD request");
  require(events.Count(Scheduler::EventKind::Admit, Scheduler::Source::AntiLag) == 0,
          "AMD takes over already-paced fallback without duplicate admission");
  scheduler.SkipPresent();
  require(events.events.back().kind == Scheduler::EventKind::Complete && !events.events.back().success,
          "occlusion retires reservation without inventing successful GPU work");
}
void gpuTimingRetainsFragmentOwnership() {
  Events events;
  Scheduler scheduler(true, Events::Observe, &events);
  auto work = scheduler.Capture();
  auto fragment = work;
  auto present = scheduler.Capture(true);
  scheduler.Close(present);
  auto frame = present->frame;
  const auto gpu_end = frame->closed_at;
  scheduler.Submitted(work, frame->begin);
  scheduler.Submitted(present, gpu_end);
  scheduler.Complete(present, true, gpu_end, gpu_end);
  present.reset();
  scheduler.Complete(work, true, frame->begin, frame->begin);
  work.reset();
  require(events.Count(Scheduler::EventKind::Complete) == 0,
          "GPU timestamp cannot release retained native fragments");
  scheduler.Complete(fragment, true, frame->begin, frame->begin);
  fragment.reset();
  require(events.Count(Scheduler::EventKind::Complete) == 1 && frame->timing_valid &&
          frame->gpu_completed_at == gpu_end && frame->completed_at >= gpu_end &&
          frame->gpu_started_at == frame->begin && frame->first_submitted_at == frame->begin &&
          frame->submitted_at == frame->begin,
          "prediction uses latest GPU timestamp independently of CPU receipt order");
}
void explicitRenderEndPreservesOtherFrame() {
  Scheduler scheduler;
  scheduler.SetMode(Scheduler::Source::Reflex, true, 0);
  scheduler.Sleep();
  auto first = scheduler.Capture();
  scheduler.Sleep();
  auto second = scheduler.Capture();
  scheduler.EndFrame(first->frame);
  auto continuing = scheduler.Capture();
  require(first->frame != second->frame && continuing->frame == second->frame,
          "ending an identified earlier frame cannot redirect the current render thread");
  scheduler.Close(first);
  scheduler.Close(second);
  scheduler.Complete(first, true);
  scheduler.Complete(second, true);
  scheduler.Complete(continuing, true);
}
void waitingReservationConsumedByPresent() {
  Events events;
  Scheduler scheduler(true, Events::Observe, &events);
  scheduler.SetMode(Scheduler::Source::Reflex, true, 0);
  std::vector<std::shared_ptr<Scheduler::Work>> pending;
  for (unsigned i = 0; i < 2; ++i) {
    require(scheduler.Sleep() == Scheduler::Result::Ready, "initial CPU admission");
    auto work = scheduler.Capture(true);
    scheduler.Close(work);
    pending.push_back(std::move(work));
  }
  events.subscribed = true;
  auto waiting = events.waiting.get_future();
  auto result = std::async(std::launch::async, [&] { return scheduler.Sleep(); });
  await(waiting);
  // Another presentation can consume the open reservation while admission waits.
  // Retire in reverse order so the oldest release removes every frame at once.
  auto raced = scheduler.Capture(true);
  scheduler.Close(raced);
  scheduler.Complete(raced, true); raced.reset();
  scheduler.Complete(pending[1], true); pending[1].reset();
  scheduler.Complete(pending[0], true); pending[0].reset();
  require(await(result) == Scheduler::Result::Ready, "retired reservation cannot leave admission waiting on idle GPU");
  auto next = scheduler.Capture();
  require(next && next->frame->admitted && !next->frame->closed, "wake reserves a live successor frame");
  scheduler.Complete(next, true);
  scheduler.SkipPresent();
}
void intervalFallbackWithoutSleep() {
  Events events;
  Scheduler scheduler(false, Events::Observe, &events);
  scheduler.SetMode(Scheduler::Source::Reflex, false,
                    std::chrono::duration_cast<std::chrono::microseconds>(1min).count());
  auto first = scheduler.Capture(true);
  scheduler.Close(first);
  auto frame = first->frame;
  scheduler.Complete(first, true); first.reset();
  require(scheduler.PresentReturn(frame, false) == Scheduler::Result::Ready, "interval fallback anchor");
  auto second = scheduler.Capture(true);
  scheduler.Close(second);
  frame = second->frame;
  scheduler.Complete(second, true); second.reset();
  events.capacity_only = false;
  events.subscribed = true;
  auto waiting = events.waiting.get_future();
  auto result = std::async(std::launch::async, [&] { return scheduler.PresentReturn(frame, false); });
  await(waiting);
  scheduler.SetMode(Scheduler::Source::Reflex, false, 0);
  require(await(result) == Scheduler::Result::Ready, "disabling requested interval releases fallback");
}
void atomicPresentAdmission() {
  Scheduler scheduler(true);
  std::vector<std::shared_ptr<Scheduler::Work>> pending;
  {
    Scheduler::PresentReservation first, second;
    require(scheduler.ReservePresent(true, first) == Scheduler::Result::Ready &&
            scheduler.ReservePresent(true, second) == Scheduler::Result::Ready,
            "two independent presenters can prepare without a device-wide lock");
    auto contender = std::async(std::launch::async, [&] {
      Scheduler::PresentReservation competing;
      return scheduler.ReservePresent(true, competing);
    });
    require(await(contender) == Scheduler::Result::WouldBlock, "unpublished reservations count toward capacity");
    for (unsigned i = 0; i < 2; ++i) {
      auto work = scheduler.Capture(true);
      scheduler.Close(work);
      pending.push_back(std::move(work));
    }
  }
  Scheduler::PresentReservation third;
  require(scheduler.ReservePresent(true, third) == Scheduler::Result::WouldBlock,
          "third nonblocking Present cannot exceed two-frame bound");
  for (auto &work : pending) scheduler.Complete(work, true);
}
void canceledPreparationReturnsCapacity() {
  Events events;
  Scheduler scheduler(true, Events::Observe, &events);
  auto first = std::make_unique<Scheduler::PresentReservation>();
  Scheduler::PresentReservation second;
  scheduler.ReservePresent(false, *first);
  scheduler.ReservePresent(false, second);
  events.subscribed = true;
  auto waiting = events.waiting.get_future();
  auto next = std::async(std::launch::async, [&] {
    Scheduler::PresentReservation third;
    return scheduler.ReservePresent(false, third);
  });
  await(waiting);
  first.reset(); // Failed/nonblocking preparation never published a GPU frame.
  require(await(next) == Scheduler::Result::Ready, "canceled preparation releases its reservation and wakes a waiter");
}
void multipleTimedWaitersCancelTogether() {
  Events events;
  Scheduler scheduler(false, Events::Observe, &events);
  scheduler.SetMode(Scheduler::Source::Reflex, false,
                    std::chrono::duration_cast<std::chrono::microseconds>(1min).count());
  scheduler.Sleep();
  events.capacity_only = false;
  events.waits_remaining = 2;
  events.subscribed = true;
  auto waiting = events.waiting.get_future();
  auto first = std::async(std::launch::async, [&] { return scheduler.Sleep(); });
  auto second = std::async(std::launch::async, [&] { return scheduler.Sleep(); });
  await(waiting);
  scheduler.Remove();
  require(await(first) == Scheduler::Result::Removed && await(second) == Scheduler::Result::Removed,
          "device removal interrupts every registered timed waiter");
}
void cancellationAndNonblocking() {
  Events events;
  Scheduler scheduler(false, Events::Observe, &events);
  require(!scheduler.Capture(), "default off does not track work");
  scheduler.SetMode(Scheduler::Source::Reflex, true, 0);
  require(scheduler.Sleep() == Scheduler::Result::Ready && scheduler.Sleep() == Scheduler::Result::Ready,
          "two CPU frames admitted without draining GPU");
  events.subscribed = true;
  auto waiting = events.waiting.get_future();
  auto result = std::async(std::launch::async, [&] { return scheduler.Sleep(); });
  await(waiting); // Subscribe to the actual capacity-wait event before removing the device.
  scheduler.Remove();
  require(await(result) == Scheduler::Result::Removed, "device removal cancels admission wait");

  Scheduler nonblocking(true);
  std::vector<std::shared_ptr<Scheduler::Work>> retained;
  for (unsigned i = 0; i < 2; ++i) {
    auto work = nonblocking.Capture(true);
    nonblocking.Close(work);
    // Retain both tasks so the capacity predicate is independent of wall-clock timing.
    retained.push_back(std::move(work));
  }
  Scheduler::PresentReservation reservation;
  require(nonblocking.ReservePresent(true, reservation) == Scheduler::Result::WouldBlock,
          "nonblocking admission preserves cap");
  auto accepted = retained.back()->frame;
  require(nonblocking.PresentReturn(accepted, true) == Scheduler::Result::Ready, "accepted DO_NOT_WAIT never sleeps");
  nonblocking.Complete(retained.front(), true);
  retained.erase(retained.begin());
  require(nonblocking.ReservePresent(true, reservation) == Scheduler::Result::Ready,
          "actual retirement releases capacity");
}
}
int main() {
  adaptiveOverlap();
  outOfOrderGpuTimingDoesNotRewindPrediction();
  sharedOwnershipAndFragments();
  asynchronousPresenter();
  waitingReservationConsumedByPresent();
  modeDoesNotTakeOwnership();
  gpuTimingRetainsFragmentOwnership();
  explicitRenderEndPreservesOtherFrame();
  intervalFallbackWithoutSleep();
  atomicPresentAdmission();
  canceledPreparationReturnsCapacity();
  multipleTimedWaitersCancelTogether();
  cancellationAndNonblocking();
  std::puts("LOW_LATENCY_SCHEDULER_PASS adaptive=1 overlap=1 grouping=1 asynchronous=1 cancellation=1");
}
