#include <metal_stdlib>
using namespace metal;

// This ABI mirrors airconv_public.h. No 64-bit native atomic is used: application
// values are immutable pairs, published by a sequentially consistent 32-bit root.
struct Atomic64Node { atomic_uint state; uint low, high, unused; };
struct Atomic64Context {
  ulong rawRanges, typedViews, roots, nodes, freeBitmap;
  uint rawCount, typedCount, nodeCapacity, bitmapWordCount;
  atomic_uint cursor, hint;
  uint reserved0, reserved1;
};
constant uint atomic64_retired = 0x80000000u;
constant uint atomic64_reserved = 0x40000000u;
constant uint atomic64_reference_mask = 0x3fffffffu;

static uint atomic64_load(device atomic_uint *p) {
  return atomic_fetch_or_explicit(p, 0u, memory_order_seq_cst, mem_flags::mem_device);
}
static bool atomic64_cas(device atomic_uint *p, thread uint &expected, uint desired) {
  return atomic_compare_exchange_weak_explicit(p, &expected, desired,
    memory_order_seq_cst, memory_order_seq_cst, mem_flags::mem_device);
}
static void atomic64_publish_free(device Atomic64Context *context, uint index) {
  auto bitmap = reinterpret_cast<device atomic_uint *>(context->freeBitmap);
  atomic_fetch_or_explicit(bitmap + index / 32, 1u << (index % 32),
    memory_order_seq_cst, mem_flags::mem_device);
  atomic_exchange_explicit(&context->hint, index / 32,
    memory_order_seq_cst, mem_flags::mem_device);
}
static void atomic64_release(device Atomic64Context *context, uint index) {
  auto nodes = reinterpret_cast<device Atomic64Node *>(context->nodes);
  uint previous = atomic_fetch_sub_explicit(&nodes[index].state, 1u,
    memory_order_seq_cst, mem_flags::mem_device);
  if (previous == (atomic64_retired | 1u)) atomic64_publish_free(context, index);
}
static void atomic64_retire(device Atomic64Context *context, uint index) {
  auto nodes = reinterpret_cast<device Atomic64Node *>(context->nodes);
  uint previous = atomic_fetch_or_explicit(&nodes[index].state, atomic64_retired,
    memory_order_seq_cst, mem_flags::mem_device);
  if ((previous & atomic64_reference_mask) == 0) atomic64_publish_free(context, index);
}
static uint atomic64_protect(device Atomic64Context *context, device atomic_uint *root) {
  auto nodes = reinterpret_cast<device Atomic64Node *>(context->nodes);
  for (;;) {
    uint index = atomic64_load(root);
    uint state = atomic64_load(&nodes[index].state);
    if ((state & atomic64_reserved) || (state & atomic64_reference_mask) == atomic64_reference_mask) continue;
    uint expected = state;
    if (!atomic64_cas(&nodes[index].state, expected, state + 1)) continue;
    if (atomic64_load(root) == index) return index;
    atomic64_release(context, index);
  }
}
static uint atomic64_allocate(device Atomic64Context *context) {
  auto bitmap = reinterpret_cast<device atomic_uint *>(context->freeBitmap);
  auto nodes = reinterpret_cast<device Atomic64Node *>(context->nodes);
  // Bitmap bits are advisory: a delayed retirement publisher can advertise a
  // reused node. Only RETIRED|0 -> RESERVED acquires ownership. The exclusive
  // phase admits P invocations into C+3P+1 nodes, so no owner must run to free one.
  uint start = atomic64_load(&context->hint) % context->bitmapWordCount;
  for (uint scanned = 0;; ++scanned) {
    uint word = (start + scanned) % context->bitmapWordCount;
    uint bits = atomic64_load(bitmap + word);
    while (bits) {
      uint bit = ctz(bits), mask = 1u << bit;
      uint expected_bits = bits;
      if (!atomic64_cas(bitmap + word, expected_bits, bits & ~mask)) { bits = expected_bits; continue; }
      uint index = word * 32 + bit;
      uint expected = atomic64_retired;
      if (index < context->nodeCapacity && atomic64_cas(&nodes[index].state, expected, atomic64_reserved)) {
        atomic_exchange_explicit(&context->hint, word, memory_order_seq_cst, mem_flags::mem_device);
        return index;
      }
      // A weak-CAS miss on a genuinely free node must not lose its bitmap bit.
      if (index < context->nodeCapacity && expected == atomic64_retired)
        atomic64_publish_free(context, index);
      bits = atomic64_load(bitmap + word);
    }
  }
}
extern "C" ulong dxmt_atomic64_read(device uchar *bytes, device atomic_uint *root) {
  if (!root) return 0;
  auto context = reinterpret_cast<device Atomic64Context *>(bytes);
  auto nodes = reinterpret_cast<device Atomic64Node *>(context->nodes);
  uint index = atomic64_protect(context, root);
  ulong value = ulong(nodes[index].low) | (ulong(nodes[index].high) << 32);
  atomic64_release(context, index);
  return value;
}
extern "C" ulong dxmt_atomic64_compare_exchange(
  device uchar *bytes, device atomic_uint *root, ulong comparison, ulong desired
) {
  if (!root) return 0;
  auto context = reinterpret_cast<device Atomic64Context *>(bytes);
  auto nodes = reinterpret_cast<device Atomic64Node *>(context->nodes);
  for (;;) {
    uint index = atomic64_protect(context, root);
    ulong value = ulong(nodes[index].low) | (ulong(nodes[index].high) << 32);
    if (value != comparison) { atomic64_release(context, index); return value; }
    uint candidate = atomic64_allocate(context);
    nodes[candidate].low = uint(desired);
    nodes[candidate].high = uint(desired >> 32);
    atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
    atomic_exchange_explicit(&nodes[candidate].state, 0u, memory_order_seq_cst, mem_flags::mem_device);
    uint expected = index;
    bool committed = atomic64_cas(root, expected, candidate);
    // Pre-publication stale readers can already hold references to a candidate;
    // losing publication retires it without overwriting those reference counts.
    atomic64_retire(context, committed ? index : candidate);
    atomic64_release(context, index);
    if (committed) return comparison;
  }
}
