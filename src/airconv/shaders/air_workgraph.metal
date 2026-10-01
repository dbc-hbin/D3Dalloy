#include <metal_stdlib>

using namespace metal;

/*
 * Run-time helpers of compiled D3D12 work graph node kernels (dxil_workgraph.cpp).
 * Layouts are SM50_WG_NODE_CONTEXT / SM50_WG_QUEUE / SM50_WG_BATCH_RECORD of
 * dxil_workgraph.h; `ctx` is the context the runtime binds at
 * SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT.
 */

#define WG_NONE 0xffffffffu
#define WG_INPUT 0xfffffffeu

/* SM50_WG_NODE_CONTEXT dword offsets. */
#define WG_CTX_QUEUES 0
#define WG_CTX_BATCH 2
#define WG_CTX_INPUT_QUEUE 6
#define WG_CTX_BATCH_RECORDS 8
#define WG_CTX_GROUP_START 10
#define WG_CTX_OUTPUT_COUNT 12
#define WG_CTX_OUTPUTS 14

struct wg_queue {
  ulong records;
  ulong sharing;
  uint mask;
  uint stride;
  atomic_uint tail;
  uint reserved;
};

struct wg_batch_record {
  uint group_end;
  uint grid[3];
};

static inline ulong wg_ctx64(device uint *ctx, uint dword) {
  return ulong(ctx[dword]) | (ulong(ctx[dword + 1]) << 32);
}

static inline device wg_queue *wg_queue_at(device uint *ctx, uint queue) {
  return (device wg_queue *)(wg_ctx64(ctx, WG_CTX_QUEUES) + ulong(queue) * sizeof(wg_queue));
}

/* Index (relative to BatchStart) of the batch record threadgroup `group` belongs to. */
uint wg_bc_record(device uint *ctx, uint group) asm("dxmt.wg.bc_record");
uint wg_bc_record(device uint *ctx, uint group) {
  device const wg_batch_record *table = (device const wg_batch_record *)wg_ctx64(ctx, WG_CTX_BATCH);
  uint absolute = ctx[WG_CTX_GROUP_START] + group;
  uint low = 0, high = ctx[WG_CTX_BATCH_RECORDS];
  /* First record whose cumulative group end is past `absolute`. */
  while (low < high) {
    uint middle = (low + high) >> 1;
    if (table[middle].group_end > absolute)
      high = middle;
    else
      low = middle + 1;
  }
  return low;
}

/* Component `axis` of SV_GroupID of threadgroup `group` within its batch record `record`. */
uint wg_bc_group(device uint *ctx, uint group, uint record, uint axis) asm("dxmt.wg.bc_group");
uint wg_bc_group(device uint *ctx, uint group, uint record, uint axis) {
  device const wg_batch_record *table = (device const wg_batch_record *)wg_ctx64(ctx, WG_CTX_BATCH);
  uint absolute = ctx[WG_CTX_GROUP_START] + group;
  uint first = record ? table[record - 1].group_end : 0u;
  uint local = absolute - first;
  uint x = table[record].grid[0], y = table[record].grid[1];
  if (axis == 0)
    return local % x;
  if (axis == 1)
    return (local / x) % y;
  return local / (x * y);
}

/* Queue of output element `element`, or WG_NONE for an invalid / absent output. */
uint wg_output_queue(device uint *ctx, uint element) asm("dxmt.wg.output_queue");
uint wg_output_queue(device uint *ctx, uint element) {
  if (element >= ctx[WG_CTX_OUTPUT_COUNT])
    return WG_NONE;
  return ctx[WG_CTX_OUTPUTS + element];
}

/* Reserve `count` records in `queue`; returns the first record index (0 when nothing is reserved). */
uint wg_reserve(device uint *ctx, uint queue, uint count) asm("dxmt.wg.reserve");
uint wg_reserve(device uint *ctx, uint queue, uint count) {
  if (queue >= WG_INPUT || !count)
    return 0;
  return atomic_fetch_add_explicit(&wg_queue_at(ctx, queue)->tail, count, memory_order_relaxed);
}

/* Device address of record `index` of `queue` (WG_INPUT: the dispatch's input queue). */
ulong wg_record_address(device uint *ctx, uint queue, uint index) asm("dxmt.wg.record_address");
ulong wg_record_address(device uint *ctx, uint queue, uint index) {
  if (queue == WG_INPUT)
    queue = ctx[WG_CTX_INPUT_QUEUE];
  if (queue >= WG_INPUT)
    return 0;
  device wg_queue *q = wg_queue_at(ctx, queue);
  return q->records + ulong(index & q->mask) * ulong(q->stride);
}

/* FinishedCrossGroupSharing for input record `index`: 1 for the last group to finish. */
uint wg_finish_sharing(device uint *ctx, uint index) asm("dxmt.wg.finish_sharing");
uint wg_finish_sharing(device uint *ctx, uint index) {
  device wg_queue *q = wg_queue_at(ctx, ctx[WG_CTX_INPUT_QUEUE]);
  device atomic_uint *counter = (device atomic_uint *)(q->sharing + ulong(index & q->mask) * 4);
  return atomic_fetch_sub_explicit(counter, 1u, memory_order_relaxed) == 1u ? 1u : 0u;
}
