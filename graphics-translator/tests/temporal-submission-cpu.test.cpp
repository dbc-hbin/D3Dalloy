#include "../../src/d3d12/owned_temporal_submission.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <optional>
#include <thread>

int main() {
  using d3dalloy::OwnedTemporalSubmissionLocks;
  std::array<std::mutex,2> features;
  std::barrier bothHeld(2), bothTried(2);
  std::array<std::atomic<unsigned>,2> committed{};
  std::array<std::thread,2> inverse;
  for (unsigned i=0;i<2;++i) inverse[i]=std::thread([&,i] {
    std::optional<OwnedTemporalSubmissionLocks> segment; segment.emplace();
    segment->AcquireFirst(features[i]);
    assert(segment->TryAcquire(features[i])); // repeated evaluation of the same feature
    bothHeld.arrive_and_wait();
    assert(!segment->TryAcquire(features[1-i]));
    bothTried.arrive_and_wait();
    committed[i].store(1); // native commit and timeline publication precede unlock
    segment.reset();
    segment.emplace(); segment->AcquireFirst(features[1-i]);
    assert(committed[1-i].load()==1);
  });
  for (auto& thread:inverse) thread.join();
  std::mutex sameFeature;
  unsigned encoded=0,published=0;
  std::array<std::thread,8> workers;
  for (auto& worker:workers) worker=std::thread([&] {
    for(unsigned i=0;i<1000;++i) {
      OwnedTemporalSubmissionLocks segment; segment.AcquireFirst(sameFeature);
      assert(encoded==published); ++encoded;
      std::this_thread::yield(); published=encoded;
    }
  });
  for (auto& worker:workers) worker.join();
  assert(encoded==8000 && published==encoded);
  try { OwnedTemporalSubmissionLocks segment; segment.AcquireFirst(sameFeature); throw 1; } catch(int) {}
  { OwnedTemporalSubmissionLocks segment; assert(segment.TryAcquire(sameFeature)); }
  std::puts("TEMPORAL_RESERVATION_PASS independent_features_overlap=1 reverse_order_no_deadlock=1 repeated_feature=1 commit_before_unlock=1 same_feature_submissions=8000 exception_release=1");
}
