#include "sha1/sha1_util.hpp"
#ifdef FORCE_DIGEST_COLLISION
namespace d3dalloy {
struct CollisionHash { static Sha1Digest compute(const void *, size_t) { return {}; } };
}
#define Sha1HashState CollisionHash
#endif
#include "owned_shader_bytecode.hpp"
#ifdef FORCE_DIGEST_COLLISION
#undef Sha1HashState
#endif
#include <array>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
  using namespace d3dalloy;
  ShaderBytecodePool pool;
  std::array<uint8_t, 4> source{1,2,3,4};
  auto first = pool.Capture(source.data(), source.size());
  auto same = pool.Capture(source.data(), source.size());
  assert(first.data() == same.data() && first.size() == source.size());
  source[0] = 9;
  auto mutated = pool.Capture(source.data(), source.size());
  assert(mutated.data() != first.data() && first.data()[0] == 1 && mutated.data()[0] == 9);
  auto shorter = pool.Capture(source.data(), source.size()-1);
  assert(shorter.data() != mutated.data() && shorter.size() == 3);
  assert(pool.Capture(nullptr, 0).empty());
  std::array<SharedShaderBytecode, 12> parallel;
  std::array<std::thread, 12> workers;
  std::barrier start(int(workers.size()));
  std::array<uint8_t, 4096> large{}; large[0] = 42;
  for (size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
    start.arrive_and_wait(); parallel[i] = pool.Capture(large.data(), large.size());
  });
  for (auto &worker : workers) worker.join();
  for (const auto &entry : parallel) assert(entry.data() == parallel[0].data());
#ifndef FORCE_DIGEST_COLLISION
  assert(parallel[0].digest() == Sha1HashState::compute(large.data(), large.size()));
#endif
  SharedShaderBytecode retained;
  { ShaderBytecodePool temporary; retained = temporary.Capture(large.data(), large.size()); }
  assert(retained.size() == large.size() && retained.data()[0] == 42);
  ShaderBytecodePool bounded;
  std::vector<SharedShaderBytecode> live;
  for (uint32_t value = 0; value < 256; ++value) live.push_back(bounded.Capture(&value, sizeof(value)));
  uint32_t value = 300;
  auto overflow = bounded.Capture(&value, sizeof(value));
  auto overflowAgain = bounded.Capture(&value, sizeof(value));
  assert(overflow.data() != overflowAgain.data() && !std::memcmp(overflow.data(), &value, sizeof(value)));
  live.clear();
  auto admitted = bounded.Capture(&value, sizeof(value));
  auto admittedAgain = bounded.Capture(&value, sizeof(value));
  assert(admitted.data() == admittedAgain.data());
  std::printf("SHARED_BYTECODE_PASS exact_bytes=1 caller_mutation=1 concurrent=12 weak_bound=256 live_ownership=1 collision_mode=%u\n",
#ifdef FORCE_DIGEST_COLLISION
      1u
#else
      0u
#endif
  );
}
