// Compile extracted production methods, as in depth-stencil-state.test.cpp.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <source_location>
#include <utility>
#include <vector>

namespace {
bool fail_new = false, fail_malloc = false;
unsigned malloc_attempts = 0, live_blocks = 0;
struct Block { void *pointer = nullptr; bool live = false; } blocks[64];
unsigned block_count = 0;

void require(bool condition, const char *step) {
  if (!condition) {
    std::fprintf(stderr, "COMMAND_RECORDING_OOM_FAIL step=%s\n", step);
    std::exit(1);
  }
}

void *arenaMalloc(size_t size) {
  ++malloc_attempts;
  if (std::exchange(fail_malloc, false)) return nullptr;
  void *pointer = std::malloc(size);
  require(pointer && block_count < std::size(blocks), "probe allocation");
  blocks[block_count++] = {pointer, true};
  ++live_blocks;
  return pointer;
}

void arenaFree(void *pointer) {
  if (!pointer) return;
  for (unsigned i = block_count; i; --i) {
    auto &block = blocks[i - 1];
    if (block.pointer != pointer || !block.live) continue;
    block.live = false;
    --live_blocks;
    std::free(pointer);
    return;
  }
  require(false, "double free or foreign arena pointer");
}
} // namespace

void *operator new(std::size_t size) {
  if (fail_new) throw std::bad_alloc();
  if (void *pointer = std::malloc(size ? size : 1)) return pointer;
  throw std::bad_alloc();
}
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }

using UINT = unsigned;
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0, E_FAIL = HRESULT(0x80004005u), E_INVALIDARG = HRESULT(0x80070057u),
                  E_OUTOFMEMORY = HRESULT(0x8007000eu), DXGI_ERROR_DEVICE_REMOVED = HRESULT(0x887a0005u);
constexpr bool FAILED(HRESULT hr) { return hr < 0; }
constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
#define STDMETHODCALLTYPE
#define ERR(...) ((void)0)
#define unlikely(value) (value)
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 0, D3D12_COMMAND_LIST_TYPE_COMPUTE = 2;
constexpr int D3D12_MESSAGE_ID_UNKNOWN = 0, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS = 2;
constexpr int WMTResourceHazardTrackingModeUntracked = 0;
constexpr int WMTComputeCommandDispatch = 1, WMTComputeCommandDispatchIndirect = 2;

namespace WMT { struct Buffer { bool valid = false; explicit operator bool() const { return valid; } }; }
struct WMTSize { uint64_t width, height, depth; };
struct WMTBufferInfo { uint64_t length = 0; unsigned options = 0; };
enum class EncoderType { Null, Compute, Render, Temporal };
enum class RecordedEndpointReason { ListEnd };
struct EncoderData {
  virtual ~EncoderData() = default;
  EncoderType type = EncoderType::Null;
  EncoderData *next = nullptr;
  uint64_t id = 0;
  RecordedEndpointReason endpoint_reason{};
};
struct CommandNext { void *value = nullptr; void set(void *pointer) { value = pointer; } };
struct wmtcmd_base { CommandNext next; };
struct wmtcmd_compute_dispatch : wmtcmd_base { int type; WMTSize size; };
struct wmtcmd_compute_dispatch_indirect : wmtcmd_base {
  int type; WMT::Buffer indirect_args_buffer; uint64_t indirect_args_offset;
};
struct ComputeEncoderData : EncoderData {
  wmtcmd_base cmd_head;
  wmtcmd_base *cmd_tail = &cmd_head;
};
struct RenderEncoderData : EncoderData {
  void *visibility_intervals = nullptr;
  uint64_t visibility_interval_count = 0;
  WMT::Buffer visibility_scratch;
};
struct OwnedTemporalFrame {};
struct TemporalEncoderData : EncoderData { std::shared_ptr<OwnedTemporalFrame> frame; };
struct IMTLD3D12TemporalUpscaleState {};
struct MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC {};
struct D3D12_DISPATCH_ARGUMENTS { UINT X, Y, Z; };

namespace owned_graphics {
bool trace_enabled = false, submission_committed = false;
unsigned signal_traces = 0, committed_traces = 0;
uint64_t NewObjectId() { static uint64_t id = 0; return ++id; }
bool TraceEnabled() { return trace_enabled; }
template <typename... Fields> void Trace(const char *event, Fields &&...) {
  if (!trace_enabled) return;
  if (!std::strcmp(event, "signal")) {
    require(!submission_committed, "signal trace before commit");
    ++signal_traces;
  }
  if (!std::strcmp(event, "signal_committed")) {
    require(submission_committed, "signal_committed trace after scope commit");
    ++committed_traces;
  }
}
} // namespace owned_graphics

#include PRODUCTION_ARENA_POINTERS

struct Device {
  struct Diagnostics { void Validation(int, const char *) {} } diagnostics;
  bool fail_close_buffer = false;
  unsigned removals = 0;
  HRESULT RemovedReason() const { return S_OK; }
  void ReportRemoval(HRESULT) { ++removals; }
  Device &GetMTLDevice() { return *this; }
  WMT::Buffer newBuffer(const WMTBufferInfo &) {
    if (fail_close_buffer) throw std::bad_alloc();
    return {true};
  }
};

struct Arena {
  std::vector<void *> spilled_cpu_heap_;
  void *cpu_heap_ = nullptr;
  size_t cpu_heap_offset_ = 0;
  int type_ = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  struct Chunk { size_t used = 0; };
  std::vector<Chunk> gpu_heap_chunks_;
  size_t gpu_heap_active_ = 0;
  EncoderData head;
  EncoderData *encoder_current = nullptr, *encoder_last = nullptr, *recording_head_ = nullptr;
  size_t encoder_count_ = 0;
  std::vector<int> icb_;
  struct CopyTemp { void free_blocks(uint64_t) {} } copy_temp_allocator_;
  uint64_t copy_temp_version_ = 0;

  bool ReserveGPUHeap(uint64_t, size_t) { return true; }
  void BindGPUHeap(WMT::Buffer &, uint64_t &, uint64_t) {}
  void InvalidateCurrentPass() {
    if (!encoder_current) return;
    encoder_last->next = encoder_current;
    encoder_last = encoder_current;
    encoder_current = nullptr;
    ++encoder_count_;
  }
  void releaseRecords() {
    for (auto *pass = head.next; pass;) {
      auto *next = pass->next;
      pass->~EncoderData();
      pass = next;
    }
    if (encoder_current) encoder_current->~EncoderData();
    head.next = nullptr;
    encoder_current = encoder_last = recording_head_ = nullptr;
    for (auto *pointer : spilled_cpu_heap_) arenaFree(pointer);
    spilled_cpu_heap_.clear();
  }
  ~Arena() { releaseRecords(); arenaFree(cpu_heap_); }

#define malloc arenaMalloc
#define free arenaFree
#include PRODUCTION_ARENA_METHODS
#undef free
#undef malloc
};

HRESULT PrepareOwnedTemporalFrame(Device *, IMTLD3D12TemporalUpscaleState *,
    const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *, std::shared_ptr<OwnedTemporalFrame> &frame) {
  frame = std::make_shared<OwnedTemporalFrame>();
  return S_OK;
}

struct RecordingInterface {
  virtual HRESULT ValidateRecording() = 0;
  virtual HRESULT TemporalUpscale(IMTLD3D12TemporalUpscaleState *, const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC *) = 0;
};

struct MTLD3D12GraphicsCommandListImpl : RecordingInterface {
  Arena *allocator_;
  Device *device_;
  int type_ = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  HRESULT recording_error_ = S_OK, close_error_ = S_OK;
  size_t encoder_count = std::numeric_limits<size_t>::max();
  void *entry;
  std::vector<int> active_visibility_queries_, active_so_queries_, active_pipeline_statistics_;
  bool render_pass_active_ = false, render_pass_pending_local_ = false, render_pass_suspended_ = false;
  unsigned render_pass_flags_ = 0;
  bool owned_closed_valid = false, predication_enabled_ = false;
  uint64_t owned_id = 1, owned_recording = 1;
  struct Pipeline { bool uses_atomic64 = false; } pipeline;
  Pipeline *pso_compute_ = &pipeline;

  MTLD3D12GraphicsCommandListImpl(Arena &arena, Device &device) : allocator_(&arena), device_(&device), entry(&arena.head) {
    arena.encoder_last = arena.recording_head_ = &arena.head;
  }
  template <typename... Args> bool RecordBundle(Args...) { return false; }
  bool RejectDispatchInRenderPass() { return false; }
  uint64_t PreparePredicatedArguments(const void *, size_t) { return 0; }
  bool PreDispatch() {
    if (!allocator_->encoder_current) {
      auto *pass = allocator_->AllocatePass<ComputeEncoderData>();
      pass->type = EncoderType::Compute;
    }
    return true;
  }

#include PRODUCTION_RECORDING_METHODS
};

struct SignalTask {
  std::shared_ptr<int> fence;
  uint64_t fence_id;
  struct Ticket { uint64_t value, generation; } ticket;
};

struct Queue {
  Device *device_;
  uint64_t owned_id_ = 1;
  struct Inflight { std::vector<SignalTask> fence_completions; } inflight;
  bool retire_on_commit = false;
  explicit Queue(Device &device) : device_(&device) {}
  struct CommittingScope {
    Queue *queue;
    Inflight &inflight;
    uint64_t seq = 1;
    ~CommittingScope() {
      owned_graphics::submission_committed = true;
      if (queue->retire_on_commit) inflight.fence_completions.clear();
    }
  };
  CommittingScope StartCommitting() { return {this, inflight}; }

#include PRODUCTION_SIGNAL_BATCH
};

namespace {
void fillCurrent(Arena &arena) {
  const size_t remaining = kCPUHeapSize - arena.cpu_heap_offset_ - 16;
  if (remaining) arena.AllocateCPUHeap(remaining, 1);
}

template <typename Action> void expectAllocationFailure(Action action) {
  bool caught = false;
  try { action(); } catch (const std::bad_alloc &) { caught = true; }
  require(caught, "allocation failure propagated");
}

void testArenaFailure(bool metadata, bool alreadySpilled) {
  Arena arena;
  require(arena.Initialize() == S_OK, "arena initialize");
  auto *first = static_cast<unsigned char *>(arena.AllocateCPUHeap(kCPUHeapSize - 32, 16));
  first[0] = 0x5a;
  first[kCPUHeapSize - 33] = 0xa5;
  if (alreadySpilled) {
    arena.AllocateCPUHeap(64, 16);
    fillCurrent(arena);
    while (arena.spilled_cpu_heap_.size() < arena.spilled_cpu_heap_.capacity()) {
      arena.AllocateCPUHeap(64, 16);
      fillCurrent(arena);
    }
  }
  const auto pointer = arena.cpu_heap_;
  const auto offset = arena.cpu_heap_offset_;
  const auto spilled = arena.spilled_cpu_heap_.size();
  const auto live = live_blocks;
  fail_new = metadata;
  fail_malloc = !metadata;
  expectAllocationFailure([&] { arena.AllocateCPUHeap(128, 16); });
  fail_new = fail_malloc = false;
  require(arena.cpu_heap_ == pointer && arena.cpu_heap_offset_ == offset, "failed expansion preserves cursor");
  require(arena.spilled_cpu_heap_.size() == spilled && live_blocks == live, "failed expansion preserves ownership");
  require(first[0] == 0x5a && first[kCPUHeapSize - 33] == 0xa5, "failed expansion preserves old allocations");
  auto *next = arena.AllocateCPUHeap(128, 16);
  require(next && next != pointer && uintptr_t(next) % 16 == 0, "retry returns valid aligned storage");
  require(first[0] == 0x5a && first[kCPUHeapSize - 33] == 0xa5, "successful expansion retains old storage");
  const auto reused = arena.cpu_heap_;
  arena.releaseRecords();
  require(arena.Initialize() == S_OK && arena.cpu_heap_ == reused && !arena.cpu_heap_offset_, "reset reuses current storage");
}

void testRecordingFailure(bool passAllocation) {
  Arena arena;
  Device device;
  require(arena.Initialize() == S_OK, "recording arena initialize");
  MTLD3D12GraphicsCommandListImpl list(arena, device);
  if (!passAllocation) list.Dispatch(1, 2, 3);
  auto *compute = static_cast<ComputeEncoderData *>(arena.encoder_current);
  auto *tail = compute ? compute->cmd_tail : nullptr;
  fillCurrent(arena);
  const auto offset = arena.cpu_heap_offset_;
  fail_malloc = true;
  list.Dispatch(4, 5, 6);
  require(list.recording_error_ == E_OUTOFMEMORY && arena.cpu_heap_offset_ == offset, "void recording retains OOM");
  if (compute) require(compute->cmd_tail == tail && !tail->next.value, "failed command does not publish invalid linkage");
  const auto attempts = malloc_attempts;
  list.Dispatch(7, 8, 9);
  list.RecordError(E_INVALIDARG);
  require(malloc_attempts == attempts && list.Close() == E_OUTOFMEMORY && !list.owned_closed_valid,
          "first recording failure survives later commands and Close");
  require(!arena.encoder_last && !arena.encoder_current, "failed Close releases recording slot");
}

void testFirstErrorAndHresult() {
  {
    Arena arena;
    Device device;
    require(arena.Initialize() == S_OK, "first-error initialize");
    MTLD3D12GraphicsCommandListImpl list(arena, device);
    list.RecordError(E_INVALIDARG);
    fillCurrent(arena);
    fail_malloc = true;
    try { arena.AllocateCPUHeap(128, 16); }
    catch (const std::bad_alloc &) { list.RecordError(E_OUTOFMEMORY); }
    require(list.recording_error_ == E_INVALIDARG && list.Close() == E_INVALIDARG, "earlier validation error survives OOM");
  }
  {
    Arena arena;
    Device device;
    require(arena.Initialize() == S_OK, "HRESULT initialize");
    MTLD3D12GraphicsCommandListImpl list(arena, device);
    fillCurrent(arena);
    fail_malloc = true;
    require(list.TemporalUpscale(nullptr, nullptr) == E_OUTOFMEMORY, "HRESULT recording returns OOM");
    require(list.ValidateRecording() == E_OUTOFMEMORY && list.Close() == E_OUTOFMEMORY, "HRESULT failure poisons recording");
  }
  {
    Arena arena;
    Device device;
    require(arena.Initialize() == S_OK, "Close initialize");
    MTLD3D12GraphicsCommandListImpl list(arena, device);
    auto *render = arena.AllocatePass<RenderEncoderData>();
    render->type = EncoderType::Render;
    render->visibility_intervals = render;
    render->visibility_interval_count = 1;
    device.fail_close_buffer = true;
    require(list.Close() == E_OUTOFMEMORY && !list.owned_closed_valid && !arena.encoder_last,
            "Close allocation exception stays inside HRESULT boundary");
  }
}

void testSignalTrace(bool enabled, bool retireAtCommit) {
  Device device;
  Queue queue(device);
  queue.retire_on_commit = retireAtCommit;
  auto owner = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = owner;
  std::vector<SignalTask> tasks;
  tasks.push_back({owner, 7, {10, 1}});
  tasks.push_back({std::move(owner), 7, {2, 2}});
  owned_graphics::trace_enabled = enabled;
  owned_graphics::submission_committed = false;
  owned_graphics::signal_traces = owned_graphics::committed_traces = 0;
  fail_new = !enabled;
  queue.ProcessSignalBatch(tasks);
  fail_new = false;
  require(!device.removals && tasks.empty() && owned_graphics::submission_committed, "signal batch commits without trace-only allocation");
  require(lifetime.expired() == retireAtCommit, "inflight tasks retain fence until retirement");
  require(owned_graphics::signal_traces == (enabled ? 2u : 0u) &&
          owned_graphics::committed_traces == (enabled ? 2u : 0u), "independent committed signal snapshots");
  queue.inflight.fence_completions.clear();
  require(lifetime.expired(), "logical retirement releases signal ownership");
  owned_graphics::trace_enabled = false;
}
} // namespace

int main() {
  {
    Arena arena;
    fail_malloc = true;
    require(arena.Initialize() == E_OUTOFMEMORY && !arena.cpu_heap_ && !arena.cpu_heap_offset_, "initial allocation failure");
    require(arena.Initialize() == S_OK, "initial allocation retry");
  }
  for (bool metadata : {false, true})
    for (bool spilled : {false, true}) testArenaFailure(metadata, spilled);
  testRecordingFailure(false);
  testRecordingFailure(true);
  testFirstErrorAndHresult();
  testSignalTrace(false, false);
  testSignalTrace(true, false);
  testSignalTrace(true, true);
  require(!live_blocks, "all arena storage released exactly once");
  std::puts("COMMAND_RECORDING_OOM_PASS arena_failures=4 initial=1 recording=2 first_error=1 hresult=1 close=1 signal_trace=3");
}
