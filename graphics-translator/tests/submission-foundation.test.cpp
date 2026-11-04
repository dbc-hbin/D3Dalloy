#include "owned_submission_access.hpp"
#include "owned_sparse_heap_index.hpp"
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
  const auto oldGeneration=q.generation;assert(registry.Remove(140,second).retired_identity==second);assert(registry.Lookup(170).owner==first);
  first.reset();registry.Remove(100,p.owner);assert(!lifetime.expired() && p.owner->immutable[0]==1);p={};assert(lifetime.expired());
  auto replacement=std::make_shared<Owner>(3);assert(registry.Insert(140,20,replacement));
  assert(registry.Lookup(150).owner==replacement && registry.Lookup(150).generation>oldGeneration && q.owner->tag==2);
  assert(!registry.Insert(UINT64_MAX-4,8,replacement));
  std::atomic<bool> stop{false};std::array<std::thread,4> readers;
  for (auto& reader : readers) reader=std::thread([&] {
    while (!stop.load()) {
      const auto pin=registry.Lookup(1004);
      if (pin.owner) {
        assert(pin.offset==4 && pin.end==1064);
        for (auto value : pin.owner->immutable) assert(value==pin.owner->tag);
        std::this_thread::yield();
      }
      const auto owners=registry.Snapshot();
      std::this_thread::yield();
      for (const auto& owner : *owners)
        for (auto value : owner->immutable) assert(value==owner->tag);
    }
  });
  std::shared_ptr<Owner> previous;
  for(unsigned i=0;i<20000;++i){registry.Remove(1000,previous);previous=std::make_shared<Owner>(i+10);assert(registry.Insert(1000,64,previous));}
  stop=true;for(auto& reader:readers)reader.join();
  std::puts("PINNED_VA_REGISTRY_PASS unregister_lifetime=1 reused_address_generation=1 latest_start_clipping=1 overflow=1 concurrent_readers=4 concurrent_snapshot=1 publication_cycles=20000 native_lock_independent=1");
}
void registrySnapshotProof() {
  std::atomic<uint64_t> generation{0};OwnedVARegistry<std::shared_ptr<Owner>> registry(generation);
  assert(registry.Snapshot()->empty());
  auto first=std::make_shared<Owner>(1),second=std::make_shared<Owner>(2);
  std::weak_ptr<Owner> old_first=first,old_second=second;
  assert(registry.Insert(100,64,first));assert(registry.Insert(200,64,second));
  auto original=registry.Snapshot();
  assert(original->size()==2 && original==registry.Snapshot());
  const auto before=generation.load();
  registry.Remove(100,first);registry.Remove(200,second);first.reset();second.reset();
  assert(!old_first.expired() && !old_second.expired() && registry.Snapshot()->empty());
  auto replacement=std::make_shared<Owner>(3);
  assert(registry.Insert(100,64,replacement));
  auto current=registry.Snapshot();
  assert(generation.load()>before && original!=current && current->size()==1 && (*current)[0]==replacement);
  std::set<unsigned> original_tags;
  for(const auto& owner:*original) {
    original_tags.insert(owner->tag);
    for(auto value:owner->immutable)assert(value==owner->tag);
  }
  assert(original_tags==std::set<unsigned>({1,2}));
  original.reset();assert(old_first.expired() && old_second.expired());
  std::weak_ptr<Owner> latest=replacement;
  registry.Remove(100,replacement);replacement.reset();
  assert(!latest.expired() && (*current)[0]->tag==3 && registry.Snapshot()->empty());
  current.reset();assert(latest.expired());
  std::puts("PINNED_VA_SNAPSHOT_PASS empty=1 complete_owner_set=1 remove_and_replace_lifetime=1 fresh_generation_owners=1 release_after_consumers=1");
}
void registryAliasProof() {
  std::atomic<uint64_t> generation{0};
  OwnedVARegistry<std::shared_ptr<Owner>> registry(generation);
  auto a=std::make_shared<Owner>(1),b=std::make_shared<Owner>(2),c=std::make_shared<Owner>(3);
  std::weak_ptr<Owner> canonical=a;
  auto same=[](const auto &,const auto &) { return true; }; // caller has proven the physical range
  size_t members=0;
  assert(registry.Insert(4096,512,a,same,&members) && !members);
  assert(registry.Insert(4096,512,b,same,&members) && members==2);
  const auto before=generation.load();
  // Failed registration followed by destruction must not unregister either
  // existing owner, even when the base address alone identifies the cohort.
  assert(!registry.Insert(4096,256,c,same) && !registry.Insert(4096,512,c));
  assert(!registry.Insert(4096,512,b,same) && !registry.Remove(4096,c).removed);
  assert(generation==before);
  auto pin=registry.Lookup(4096+511);
  assert(pin.owner==a && pin.begin==4096 && pin.end==4608 && pin.offset==511);
  auto remove_b=registry.Remove(4096,b);
  assert(remove_b.removed && remove_b.remaining==1 && !remove_b.retired_identity);
  assert(registry.Lookup(4096).owner==a && registry.Snapshot()->size()==1);
  assert(registry.Insert(4096,512,b,same) && registry.Insert(4096,512,c,same));
  auto remove_a=registry.Remove(4096,a);
  assert(remove_a.removed && remove_a.remaining==2 && !remove_a.retired_identity && generation>before);
  assert(!registry.Remove(4096,a).removed);
  a.reset(); pin={};
  assert(!canonical.expired() && registry.Lookup(4100).owner==canonical.lock());
  auto snapshot=registry.Snapshot();
  assert((std::set<std::shared_ptr<Owner>>(snapshot->begin(),snapshot->end()) ==
          std::set<std::shared_ptr<Owner>>{canonical.lock(),b,c}) && snapshot->size()==3);
  assert(registry.Remove(4096,b).remaining==1);
  auto last=registry.Remove(4096,c);
  assert(last.removed && !last.remaining && last.retired_identity==canonical.lock());
  assert(!registry.Lookup(4096).owner && registry.Snapshot()->empty());
  assert(registry.Insert(4096,128,b)); // Independent address reuse after group retirement.
  assert(!registry.Remove(4096,last.retired_identity).removed && registry.Lookup(4097).owner==b);
  last={}; snapshot.reset(); assert(canonical.expired());
  std::puts("PLACED_VA_ALIAS_PASS equal_extent=1 unequal_rejected=1 physical_proof_required=1 wrong_owner_noop=1 stable_rtas_identity=1 removal_orders=2 snapshot_all_pinned=1 reuse=1");
}
void heapExtentProof() {
  SubmissionAccess a{}, b{};
  a.physical = b.physical = {PhysicalDomain::Heap, 7};
  a.aspects = 4; a.mip_begin = 2; a.mip_end = 3;
  SetHeapAccessExtent(a, 64, 16);
  SetHeapAccessExtent(b, 80, 16);
  assert(a.begin == 64 && a.end == 80 && !AccessOverlaps(a, b));
  SetHeapAccessExtent(b, 79, 1);
  assert(AccessOverlaps(a, b));
  b.physical.owner = 8;
  assert(!AccessOverlaps(a, b));
  b.physical.owner = 7;
  SetHeapAccessExtent(a, UINT64_MAX - 32, 16);
  SetHeapAccessExtent(b, UINT64_MAX - 16, 16);
  assert(a.begin == UINT64_MAX - 32 && a.end == UINT64_MAX - 16 && !AccessOverlaps(a, b));
  SetHeapAccessExtent(b, UINT64_MAX - 17, 1);
  assert(AccessOverlaps(a, b));
  for (const auto &[offset, size] : std::array<std::pair<uint64_t, uint64_t>, 3>{
           {{UINT64_MAX - 32, 33}, {12, 0}, {UINT64_MAX, 1}}}) {
    SetHeapAccessExtent(a, offset, size);
    assert(a.begin == 0 && a.end == UINT64_MAX && AccessOverlaps(a, b));
    assert(a.Key() == PhysicalIdentity(PhysicalDomain::Heap, 7));
    assert(a.aspects == 4 && a.mip_begin == 2 && a.mip_end == 3);
  }
}

void placedAttachmentOrderProof() {
  int texture, alias;
  SubmissionAccess depth{&texture, 64, 128, 0, 1, 0, 1, 1, true, true, 2,
      {PhysicalDomain::Heap, 7}};
  auto stencil = depth;
  stencil.aspects = 2;
  // The byte frontier intentionally sees both aspects of a D32S8 placement.
  // Its consumer must not manufacture a fragment-completion edge between them.
  OwnedAccessFrontier<SubmissionAccess> frontier;
  frontier.Publish(depth, depth);
  frontier.Publish(stencil, stencil, false);
  unsigned aspects = 0, ordered = 0;
  frontier.Visit(depth, [&](const auto &prior) {
    assert(AccessOverlaps(depth, prior));
    assert(AttachmentDependencyOrdered(depth, prior));
    aspects |= prior.aspects;
    ++ordered;
  });
  assert(aspects == 3 && ordered == 2);
  assert(AttachmentDependencyOrdered(stencil, depth));
  auto color = depth;
  color.aspects = 4;
  assert(AttachmentDependencyOrdered(color, color));

  auto changed = depth;
  changed.allocation = &alias;
  assert(AccessOverlaps(depth, changed) && !AttachmentDependencyOrdered(depth, changed));
  changed = depth;
  changed.attachment = false;
  changed.writes = false;
  assert(AccessOverlaps(depth, changed) && !AttachmentDependencyOrdered(depth, changed));
  assert(!AttachmentDependencyOrdered(changed, depth));
  changed = depth;
  changed.mip_begin = 1; changed.mip_end = 2;
  assert(AccessOverlaps(depth, changed) && !AttachmentDependencyOrdered(depth, changed));
  changed = depth;
  changed.slice_begin = 1; changed.slice_end = 2;
  assert(AccessOverlaps(depth, changed) && !AttachmentDependencyOrdered(depth, changed));
  changed = depth;
  changed.end = 129;
  assert(!AttachmentDependencyOrdered(depth, changed));
  changed = depth;
  changed.physical.owner = 8;
  assert(!AttachmentDependencyOrdered(depth, changed));
  changed = depth;
  changed.allocation = nullptr;
  assert(!AttachmentDependencyOrdered(changed, changed));

  assert(HasFiniteHeapExtent(0, 64) && HasFiniteHeapExtent(UINT64_MAX - 2, 1));
  for (const auto &[offset, size] : std::array<std::pair<uint64_t, uint64_t>, 4>{
           {{0, 0}, {64, 0}, {UINT64_MAX - 16, 16}, {UINT64_MAX - 16, 17}}}) {
    assert(!HasFiniteHeapExtent(offset, size));
    changed = depth;
    SetHeapAccessExtent(changed, offset, size);
    assert(!AttachmentDependencyOrdered(changed, changed));
  }
  auto committed = depth;
  committed.physical = {PhysicalDomain::Texture, 9};
  committed.begin = committed.end = 0;
  assert(AttachmentDependencyOrdered(committed, committed));
  std::puts("PLACED_ATTACHMENT_ORDER_PASS same_texture=1 depth_stencil_cross_aspect=1 alias_and_shader_edges=external subresources=exact unknown_extent=conservative");
}

void sparseHeapProof() {
  constexpr uint64_t tile_bytes = OwnedSparseHeapIndex::kTileBytes;
  std::mutex native_gate;
  OwnedSparseHeapIndex index;
  auto prove = [&](std::initializer_list<OwnedHeapRange> ranges) {
    return index.Prove(std::span(ranges.begin(), ranges.size()), native_gate);
  };
  const auto initial = prove({{7, 0, 1}});
  assert(initial.disjoint && initial.generation == 1 && index.Current(initial, native_gate));
  { auto unchanged = index.BeginUpdate(); }
  assert(index.Current(initial, native_gate));
  assert(!prove({}).disjoint && !prove({{0, 0, 1}}).disjoint);
  assert(!prove({{7, 1, 1}}).disjoint && !prove({{7, UINT64_MAX, 0}}).disjoint);
  {
    auto update = index.BeginUpdate();
    update.Add(7, 2); update.Add(7, 2); update.Add(7, 6); update.Add(8, 2);
  }
  assert(!index.Current(initial, native_gate));
  assert(prove({{7, 0, 2 * tile_bytes}}).disjoint);
  assert(!prove({{7, 2 * tile_bytes, 2 * tile_bytes + 1}}).disjoint);
  assert(!prove({{7, 3 * tile_bytes - 1, 3 * tile_bytes}}).disjoint);
  assert(prove({{7, 3 * tile_bytes, 3 * tile_bytes + 1}}).disjoint);
  assert(prove({{9, 2 * tile_bytes, 3 * tile_bytes}}).disjoint);
  assert(!prove({{9, 0, 1}, {8, 2 * tile_bytes, 3 * tile_bytes}}).disjoint);
  { auto update = index.BeginUpdate(); update.Remove(7, 2); }
  assert(!prove({{7, 2 * tile_bytes, 3 * tile_bytes}}).disjoint);
  { auto update = index.BeginUpdate(); update.Remove(7, 2); }
  assert(prove({{7, 2 * tile_bytes, 3 * tile_bytes}}).disjoint);
  const auto before_remap = prove({{7, 3 * tile_bytes, 4 * tile_bytes}});
  {
    auto update = index.BeginUpdate();
    update.Add(7, 3); update.Remove(7, 6);
  }
  assert(!index.Current(before_remap, native_gate));
  const auto after_remap = prove({{7, 6 * tile_bytes, 7 * tile_bytes}});
  assert(after_remap.disjoint && after_remap.generation == before_remap.generation + 1);
  assert(!prove({{7, 3 * tile_bytes, 4 * tile_bytes}}).disjoint);
  // Reusing the same numeric heap identity after its last mapping disappears
  // cannot revive a negative proof from the earlier catalog generation.
  { auto update = index.BeginUpdate(); update.Remove(7, 3); }
  const auto before_reuse = prove({{7, 3 * tile_bytes, 4 * tile_bytes}});
  { auto update = index.BeginUpdate(); update.Add(7, 3); }
  assert(!index.Current(before_reuse, native_gate));
  assert(!prove({{7, 3 * tile_bytes, 4 * tile_bytes}}).disjoint);

  // Hold the real native gate from another thread. Both calls must return
  // before the holder is released; the runner timeout detects a blocking lock.
  const auto fresh = prove({{9, 0, 1}});
  std::barrier rendezvous(2);
  std::thread mapping([&] {
    std::lock_guard lock(native_gate);
    rendezvous.arrive_and_wait();
    rendezvous.arrive_and_wait();
  });
  rendezvous.arrive_and_wait();
  const bool busy_proof = prove({{9, 0, 1}}).disjoint;
  const bool busy_current = index.Current(fresh, native_gate);
  rendezvous.arrive_and_wait();
  mapping.join();
  assert(!busy_proof && !busy_current && index.Current(fresh, native_gate));

  // CPU unmap/remap publication is not native completion. Old heap pages may
  // still be visible to an earlier GPU reader after the enqueue mutex unlocks.
  OwnedSparseHeapIndex asynchronous;
  const std::array<OwnedHeapRange, 1> old_page{{{71, 0, tile_bytes}}};
  const std::array<OwnedHeapRange, 1> new_page{{{71, tile_bytes, 2 * tile_bytes}}};
  { auto update = asynchronous.BeginUpdate(); update.Add(71, 0); }
  const auto previously_disjoint = asynchronous.Prove(new_page, native_gate);
  assert(previously_disjoint.disjoint);
  auto first_native = asynchronous.BeginPending();
  { auto update = asynchronous.BeginUpdate(); update.Remove(71, 0); }
  assert(!asynchronous.Prove(old_page, native_gate).disjoint);
  assert(!asynchronous.Current(previously_disjoint, native_gate));
  auto second_native = asynchronous.BeginPending();
  { auto update = asynchronous.BeginUpdate(); update.Add(71, 1); }
  // Different public completion threads may observe their callbacks out of
  // order. Completing the newer publication cannot make the older one ready.
  second_native.Retire(true);
  assert(!asynchronous.Prove(old_page, native_gate).disjoint);
  auto moved_native = std::move(first_native);
  assert(!first_native && moved_native);
  {
    // Retirement must not take the native enqueue mutex: synchronous creation
    // helpers may retain that mutex while waiting for an earlier publication.
    std::lock_guard lock(native_gate);
    std::thread retire([pending = std::move(moved_native)]() mutable { pending.Retire(true); });
    retire.join();
  }
  assert(asynchronous.Prove(old_page, native_gate).disjoint);
  assert(!asynchronous.Prove(new_page, native_gate).disjoint);
  assert(!asynchronous.Current(previously_disjoint, native_gate));
  auto failed_native = asynchronous.BeginPending();
  { auto update = asynchronous.BeginUpdate(); update.Remove(71, 1); }
  failed_native.Retire(false);
  assert(!asynchronous.Prove(old_page, native_gate).disjoint);
  OwnedSparseHeapIndex abandoned;
  { auto unpublished = abandoned.BeginPending(); }
  assert(!abandoned.Prove(old_page, native_gate).disjoint);
  std::puts("SPARSE_MAPPING_PENDING_PASS old_page_guard=1 out_of_order_retirement=1 moved_owner=1 failure_poison=1 enqueue_lock_independent=1");

  {
    auto update = index.BeginUpdate();
    update.Add(7, UINT32_MAX);
  }
  const uint64_t high = uint64_t(UINT32_MAX) * tile_bytes;
  assert(!prove({{7, high, high + tile_bytes}}).disjoint);
  assert(prove({{7, high + tile_bytes, high + tile_bytes + 1}}).disjoint);
  assert(prove({{7, UINT64_MAX - 1, UINT64_MAX}}).disjoint);

  OwnedSparseHeapIndex damaged;
  const std::array<OwnedHeapRange, 1> unrelated{{{99, 0, 1}}};
  const auto before_failure = damaged.Prove(unrelated, native_gate);
  try {
    auto update = damaged.BeginUpdate();
    update.Add(7, 0);
    throw std::bad_alloc();
  } catch (const std::bad_alloc &) {}
  assert(!damaged.Current(before_failure, native_gate));
  assert(!damaged.Prove(unrelated, native_gate).disjoint);
  { auto update = damaged.BeginUpdate(); update.Add(8, 0); update.Remove(7, 0); }
  assert(!damaged.Prove(unrelated, native_gate).disjoint);
  OwnedSparseHeapIndex inconsistent;
  { auto update = inconsistent.BeginUpdate(); update.Remove(7, 0); }
  assert(!inconsistent.Prove(unrelated, native_gate).disjoint);

  // Independent oracle: scan live physical pages as byte intervals. It does
  // not share the production lower_bound or byte-to-page calculations.
  OwnedSparseHeapIndex random_index;
  std::map<std::pair<uintptr_t, uint32_t>, unsigned> references;
  std::mt19937 random(0x5a125e);
  unsigned queries = 0;
  for (unsigned step = 0; step < 512; ++step) {
    const auto key = std::pair<uintptr_t, uint32_t>{1 + random() % 4, random() % 16};
    auto &count = references[key];
    {
      auto update = random_index.BeginUpdate();
      if (!count || random() % 2) { update.Add(key.first, key.second); ++count; }
      else { update.Remove(key.first, key.second); --count; }
    }
    for (unsigned check = 0; check < 4; ++check) {
      std::array<OwnedHeapRange, 2> ranges{};
      for (auto &range : ranges) {
        range.heap = 1 + random() % 5;
        range.begin = uint64_t(random() % 19) * tile_bytes + random() % tile_bytes;
        range.end = range.begin + 1 + random() % (3 * tile_bytes);
      }
      bool expected = true;
      for (const auto &range : ranges)
        for (const auto &[page, refcount] : references) {
          const uint64_t begin = uint64_t(page.second) * tile_bytes, end = begin + tile_bytes;
          if (refcount && page.first == range.heap && range.begin < end && begin < range.end) expected = false;
        }
      const auto proof = random_index.Prove(ranges, native_gate);
      assert(proof.disjoint == expected);
      assert(random_index.Current(proof, native_gate) == expected);
      ++queries;
    }
  }
  heapExtentProof();
  std::printf("SPARSE_HEAP_INDEX_PASS oracle_queries=%u aliases=refcounted remap_generations=1 native_gate_nonblocking=1 stale_proof=conservative exception_unwind=conservative half_open_ranges=exact high_offsets=1 heap_extent_fallback=1\n", queries);
}
int main(){frontierProof();registryProof();registrySnapshotProof();registryAliasProof();sparseHeapProof();placedAttachmentOrderProof();}
