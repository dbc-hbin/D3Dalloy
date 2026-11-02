#include "d3d12_reflex.hpp"
#include <cstdio>
#include <cstdlib>
#include <future>

// Only diagnostics are omitted. Frame selection, locking, reservations and
// lifetime are the production ReflexState and LowLatencyScheduler implementations.
std::string d3dalloy::env::getEnvVar(const char *) { return {}; }
void d3dalloy::Logger::info(const std::string &) {}

namespace {
using State = d3dalloy::ReflexState;
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "LOW_LATENCY_REFLEX_FAIL %s\n", message);
    std::exit(1);
  }
}
template <typename T> T await(std::future<T> &future) {
  require(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "bounded capture completion");
  return future.get();
}
void captureClosesBeforeReturning() {
  State state(true);
  // Reproduce the former interleaving exactly: caller A returns from Capture,
  // caller B captures, and only then could either caller perform an external Close.
  auto first = state.Capture(true, true);
  auto second = state.Capture(true, true);
  require(first->admission->frame != second->admission->frame &&
          first->admission->frame->closed && second->admission->frame->closed,
          "real captures must select and close distinct frames before returning");
  State::PresentReservation third;
  require(state.ReservePresent(true, third) == DXGI_ERROR_WAS_STILL_DRAWING,
          "two captured real frames retain two-frame capacity");
}
void generatedAndRealShareFrame() {
  State state(true);
  auto generated = state.CapturePresent(false);
  auto real = state.CapturePresent(true);
  require(!generated.frame && real.frame && generated.work->admission->frame == real.frame,
          "generated and real members share one application frame");
  auto next = state.CapturePresent(true);
  require(next.frame != real.frame, "following real Present selects a new frame");
}
void simultaneousRealPresents() {
  State state(true);
  std::promise<void> start, first_ready, second_ready;
  auto gate = start.get_future().share();
  auto a_ready = first_ready.get_future();
  auto b_ready = second_ready.get_future();
  auto present = [&](std::promise<void> &ready) {
    State::PresentReservation reservation;
    require(state.ReservePresent(false, reservation) == S_OK, "reserve before queue preparation");
    ready.set_value();
    require(gate.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "bounded capture start");
    gate.get();
    return state.CapturePresent(true);
  };
  auto first = std::async(std::launch::async, [&] { return present(first_ready); });
  auto second = std::async(std::launch::async, [&] { return present(second_ready); });
  await(a_ready);
  await(b_ready);
  start.set_value();
  auto a = await(first);
  auto b = await(second);
  require(a.frame && b.frame && a.frame != b.frame && a.frame->closed && b.frame->closed,
          "concurrent real Presents retain independent application frames");
  State::PresentReservation third;
  require(state.ReservePresent(true, third) == DXGI_ERROR_WAS_STILL_DRAWING,
          "publication releases reservations but not GPU frame capacity");
}
}
int main() {
  captureClosesBeforeReturning();
  generatedAndRealShareFrame();
  simultaneousRealPresents();
  std::puts("LOW_LATENCY_REFLEX_PASS atomic_capture=1 generated_group=1 concurrent_present=1");
}
