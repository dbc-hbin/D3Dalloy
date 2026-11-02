#include "owned_stage_key.hpp"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <unordered_map>

std::atomic<unsigned> largeAllocations{0};
void *operator new(size_t size) {
  if (size >= 512 * 1024) ++largeAllocations;
  if (auto p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

int main() {
  using namespace d3dalloy;
  ShaderBytecodePool pool;
  std::string input(1024 * 1024, char(37));
  for (size_t i = 0; i < input.size(); i += 17) input[i] = char(i);
  uint32_t revision = 7;
  uint64_t size = input.size();
  const char suffix[] = "entry";
  const auto build = [&] {
    CanonicalStageKey key;
    key.append(reinterpret_cast<const char*>(&revision),sizeof(revision));
    key.append(reinterpret_cast<const char*>(&size),sizeof(size));
    key.reference(input.data(),input.size());
    key.append(suffix,sizeof(suffix));
    return key;
  };
  std::string golden(reinterpret_cast<const char*>(&revision),sizeof(revision));
  golden.append(reinterpret_cast<const char*>(&size),sizeof(size));
  golden.append(input).append(suffix,sizeof(suffix));
  auto first = build();
  assert(first.size() == golden.size() && first.digest() == Sha1HashState::compute(golden.data(),golden.size()));
  std::unordered_map<CanonicalStageKey,int,CanonicalStageKey::Hash> map;
  auto frozen = first.owned(pool);
  map.emplace(frozen,17);
  largeAllocations = 0;
  for (unsigned repeat=0;repeat<50;++repeat) {
    auto probe = build();
    assert(map.find(probe) != map.end() && map.find(probe)->second == 17);
    assert(probe.digest() == frozen.digest());
  }
  assert(largeAllocations == 0);
  const unsigned hotAllocations=largeAllocations;
  assert(first.string() == golden && frozen.string() == golden);
  CanonicalStageKey flat(golden);
  assert(flat == frozen);
  CanonicalStageKey chunks;
  for (size_t offset=0;offset<golden.size();) {
    const size_t length=std::min<size_t>(13,golden.size()-offset);
    chunks.reference(golden.data()+offset,length); offset+=length;
  }
  assert(chunks==flat && chunks.digest()==flat.digest());
  auto moved=std::move(frozen); first=CanonicalStageKey{};
  input[0]^=1;
  assert(map.find(build())==map.end() && moved.string()==golden);
  struct Collision { size_t operator()(const CanonicalStageKey &) const { return 0; } };
  std::unordered_map<CanonicalStageKey,int,Collision> collisions;
  for (unsigned i=0;i<32;++i) collisions.emplace(CanonicalStageKey(std::string(i+1,'x')),i);
  for (unsigned i=0;i<32;++i) assert(collisions.at(std::string(i+1,'x'))==int(i));
  assert(collisions.find("z")==collisions.end());
  CanonicalStageKey filled;
  filled.append(suffix,sizeof(suffix));
  assert(!filled.filled(1024,[](char*) { return false; }) && filled.size()==sizeof(suffix));
  assert(filled.filled(input.size(),[&](char *target) { std::memcpy(target,input.data(),input.size());return true; }));
  auto copy=filled.owned(pool); filled=CanonicalStageKey{};
  assert(copy.string()==std::string(suffix,sizeof(suffix))+input);
  std::printf("CANONICAL_STAGE_KEY_PASS hot_lookups=50 input_bytes=1048576 hot_large_allocations=%u canonical_disk_bytes=exact digest=unchanged segmented_equality=1 collision_equality=1 lifetime=1 failed_fill=1\n",hotAllocations);
}
