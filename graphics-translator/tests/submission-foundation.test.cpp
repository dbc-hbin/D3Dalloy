#include "owned_submission_access.hpp"
#include "owned_va_registry.hpp"
#include <array>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <memory>
#include <random>
#include <set>
#include <thread>

using namespace d3dalloy;
struct AccessValue {unsigned node,access; bool operator==(const AccessValue&) const = default; bool operator<(const AccessValue& other) const {
  return std::tie(node,access)<std::tie(other.node,other.access);
}};
using Point=std::tuple<PhysicalIdentity,unsigned,unsigned,unsigned>;
std::vector<Point> points(const SubmissionAccess& a) {
  std::vector<Point> result;
  if(a.Key().domain==PhysicalDomain::Heap || !a.aspects) {
    for(unsigned x=unsigned(a.begin);x<a.end;++x)result.emplace_back(a.Key(),0,0,x);
  } else for(unsigned aspect:{1u,2u,4u})if(a.aspects&aspect)
    for(unsigned mip=a.mip_begin;mip<a.mip_end;++mip)
      for(unsigned slice=a.slice_begin;slice<a.slice_end;++slice)result.emplace_back(a.Key(),aspect,mip,slice);
  return result;
}
void frontierProof() {
  std::mt19937 random(0xc001d00d);
  unsigned operations=0;
  for(bool inflight:{false,true})for(unsigned trial=0;trial<80;++trial) {
    OwnedAccessFrontier<AccessValue> frontier;
    struct References {std::set<AccessValue> readers,writers;};
    std::map<Point,References> reference;
    for(unsigned node=0;node<64;++node)for(unsigned slot=0;slot<3;++slot) {
      SubmissionAccess a{};a.allocation=reinterpret_cast<const void*>(uintptr_t(random()%100+1));
      const auto domain=random()%3;a.physical={domain==0?PhysicalDomain::Buffer:domain==1?PhysicalDomain::Texture:PhysicalDomain::Heap,random()%4+1};
      a.begin=random()%24;a.end=a.begin+1+random()%8;a.writes=random()%2;
      a.mip_begin=random()%3;a.mip_end=a.mip_begin+1+random()%2;
      a.slice_begin=random()%6;a.slice_end=a.slice_begin+1+random()%3;a.aspects=domain==0?0:uint8_t(1+random()%7);
      std::set<AccessValue> actual,expected;
      frontier.Visit(a,[&](const auto& old){actual.insert(old);});
      const auto covered=points(a);
      for(const auto& point:covered) {
        auto& entry=reference[point];expected.insert(entry.writers.begin(),entry.writers.end());
        if(a.writes)expected.insert(entry.readers.begin(),entry.readers.end());
      }
      assert(actual==expected);
      AccessValue value{node,slot};
      auto keep=[&](const auto& old){return inflight || old.node==node;};
      frontier.PublishPreserving(a,value,keep);
      for(const auto& point:covered) {
        auto& entry=reference[point];
        if(a.writes){std::erase_if(entry.writers,[&](const auto& old){return !keep(old);});
          std::erase_if(entry.readers,[&](const auto& old){return !keep(old);});entry.writers.insert(value);}
        else entry.readers.insert(value);
      }
      ++operations;
    }
    frontier.Clear();SubmissionAccess all{};all.physical={PhysicalDomain::Heap,1};all.end=UINT64_MAX;all.writes=true;
    unsigned hits=0;frontier.Visit(all,[&](const auto&){++hits;});assert(!hits);
  }
  OwnedAccessFrontier<unsigned> disjoint;
  for(unsigned i=0;i<4096;++i) {
    SubmissionAccess a{};a.physical={PhysicalDomain::Buffer,42};a.begin=uint64_t(i)*32;a.end=a.begin+8;a.writes=true;
    disjoint.Publish(a,i);
  }
  SubmissionAccess query{};query.physical={PhysicalDomain::Buffer,42};query.begin=2048*32;query.end=query.begin+1;
  unsigned visits=0;disjoint.Visit(query,[&](unsigned value){assert(value==2048);++visits;});assert(visits==1);
  SubmissionAccess heapA=query,heapB=query;
  heapA.physical=heapB.physical={PhysicalDomain::Heap,7};heapA.aspects=4;heapB.aspects=0;
  heapA.begin=0;heapA.end=UINT64_MAX;heapB.begin=UINT64_MAX-1;heapB.end=UINT64_MAX;
  assert(AccessOverlaps(heapA,heapB));heapB.physical.domain=PhysicalDomain::Buffer;assert(!AccessOverlaps(heapA,heapB));
  std::printf("PHYSICAL_FRONTIER_PASS byte_texture_heap_operations=%u exact_reference=1 same_node_stages=preserved partial_write_readers=preserved inflight_all_producers=1 disjoint_ranges=4096 visited=1 domain_isolation=1\n",operations);
}
struct Owner {unsigned tag; std::array<unsigned,32> immutable; explicit Owner(unsigned id):tag(id){immutable.fill(id);} };
void registryProof() {
  std::atomic<uint64_t> generation{0};OwnedVARegistry<std::shared_ptr<Owner>> registry(generation);
  auto first=std::make_shared<Owner>(1);std::weak_ptr<Owner> lifetime=first;
  assert(registry.Insert(100,100,first));assert(!registry.Insert(100,100,first));
  auto second=std::make_shared<Owner>(2);assert(registry.Insert(140,20,second));
  auto p=registry.Lookup(130);assert(p.owner==first && p.offset==30 && p.begin==100 && p.end==140);
  auto q=registry.Lookup(150);assert(q.owner==second && q.end==160);assert(!registry.Lookup(170).owner);
  const auto oldGeneration=q.generation;assert(registry.Remove(140)==second);assert(registry.Lookup(170).owner==first);
  first.reset();registry.Remove(100);assert(!lifetime.expired() && p.owner->immutable[0]==1);p={};assert(lifetime.expired());
  auto replacement=std::make_shared<Owner>(3);assert(registry.Insert(140,20,replacement));
  assert(registry.Lookup(150).owner==replacement && registry.Lookup(150).generation>oldGeneration && q.owner->tag==2);
  assert(!registry.Insert(UINT64_MAX-4,8,replacement));
  std::atomic<bool> stop{false};std::array<std::thread,4> readers;
  for(auto& reader:readers)reader=std::thread([&]{while(!stop.load()) {
    const auto pin=registry.Lookup(1004);if(pin.owner){assert(pin.offset==4 && pin.end==1064);
      for(auto value:pin.owner->immutable)assert(value==pin.owner->tag);std::this_thread::yield();}
  }});
  for(unsigned i=0;i<20000;++i){registry.Remove(1000);assert(registry.Insert(1000,64,std::make_shared<Owner>(i+10)));}
  stop=true;for(auto& reader:readers)reader.join();
  std::puts("PINNED_VA_REGISTRY_PASS unregister_lifetime=1 reused_address_generation=1 latest_start_clipping=1 overflow=1 concurrent_readers=4 publication_cycles=20000 native_lock_independent=1");
}
int main(){frontierProof();registryProof();}
