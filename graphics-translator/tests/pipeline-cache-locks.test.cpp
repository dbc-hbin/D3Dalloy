// CPU-only: python3 graphics-translator/tests/run-pipeline-cache-cpu.py
// Production metadata, flight, blob validation and retry code; only disk/native APIs are stubs.
#define WINEMETAL_API extern "C"
#include "winemetal.h"
#include "com/com_pointer.hpp"
#include "d3d12.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

// The vendored native D3D12 header predates these Windows SDK declarations.
enum D3D12_SHADER_CACHE_CONTROL_FLAGS {
  D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE = 1,
  D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE = 2,
  D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR = 4
};
constexpr HRESULT D3D12_ERROR_ADAPTER_NOT_FOUND = HRESULT(0x887e0001u);
constexpr HRESULT D3D12_ERROR_DRIVER_VERSION_MISMATCH = HRESULT(0x887e0002u);
constexpr HRESULT E_ABORT = HRESULT(0x80004004u);
#include "d3d12_pipeline_cache.hpp"
#include "../../src/d3d10/d3d10_blob.hpp"

namespace dxmt {
struct ShaderStageArtifact;
HRESULT ReadAutomaticStageArtifact(const std::string &, std::shared_ptr<const ShaderStageArtifact> &);
void WriteAutomaticStageArtifact(const std::string &, const ShaderStageArtifact &);
}

using namespace std::chrono_literals;
void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "PIPELINE_CACHE_CPU_FAIL %s\n", message); std::abort(); }
}
template <typename T> T await(std::future<T> &future) {
  require(future.wait_for(5s) == std::future_status::ready, "bounded signal wait");
  return future.get();
}
struct Gate {
  std::promise<void> reached, released;
  std::future<void> arrival = reached.get_future();
  std::shared_future<void> resume = released.get_future().share();
  bool used = false;
  void block() {
    if (used) return;
    used = true;
    reached.set_value();
    require(resume.wait_for(5s) == std::future_status::ready, "bounded gate release");
    resume.get();
  }
  void wait() { await(arrival); }
  void open() { released.set_value(); }
};
thread_local std::function<void(const std::string &, const std::string &)> trace_hook;
std::function<void(const std::string &, const std::string &)> worker_trace_hook;
std::mutex worker_trace_mutex;
std::function<void()> disk_hook;
std::mutex disk_hook_mutex;
std::shared_ptr<std::promise<void>> publication_signal;
void setDiskHook(std::function<void()> hook) {
  std::lock_guard lock(disk_hook_mutex);
  disk_hook = std::move(hook);
}
void invokeDiskHook() {
  std::function<void()> hook;
  { std::lock_guard lock(disk_hook_mutex); hook = disk_hook; }
  if (hook) hook();
}
namespace owned_graphics {
inline bool TraceEnabled() { return true; }
inline uint64_t NewObjectId() { static std::atomic_uint64_t next{0}; return ++next; }
template <typename... Fields> void Trace(const char *event, Fields &&...fields) {
  std::ostringstream stream;
  (stream << ... << fields);
  if (trace_hook)
    trace_hook(event, stream.str());
  std::function<void(const std::string &, const std::string &)> worker_hook;
  { std::lock_guard lock(worker_trace_mutex); worker_hook = worker_trace_hook; }
  if (worker_hook) worker_hook(event, stream.str());
}
}
namespace dxmt {
HRESULT ReadAutomaticPipelineCache(const Sha1Digest &, ID3DBlob **);
void WriteAutomaticPipelineCache(const Sha1Digest &, ID3DBlob *);
void RemoveAutomaticPipelineCache(const Sha1Digest &);
HRESULT ClearAutomaticPipelineCacheDisk();
}
#include PRODUCTION_CACHE

namespace dxmt {
class TestBlob final : public ID3DBlob {
  std::atomic<ULONG> refs{1};
  std::vector<uint8_t> bytes;
public:
  explicit TestBlob(size_t size) : bytes(size) {}
  HRESULT QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { auto n = --refs; if (!n) delete this; return n; }
  void *GetBufferPointer() override { return bytes.data(); }
  SIZE_T GetBufferSize() override { return bytes.size(); }
};
HRESULT CreateBlobFromMalloc(SIZE_T size, ID3DBlob **blob) { *blob = new TestBlob(size); return S_OK; }
std::unordered_map<Sha1Digest, Com<ID3DBlob>> disk;
std::mutex test_disk_mutex;
std::condition_variable test_disk_ready;
unsigned writes = 0, removals = 0, reads = 0;
HRESULT ReadAutomaticPipelineCache(const Sha1Digest &key, ID3DBlob **blob) {
  invokeDiskHook();
  std::lock_guard lock(test_disk_mutex);
  ++reads;
  auto it = disk.find(key);
  *blob = it == disk.end() ? nullptr : it->second.ref();
  return *blob ? S_OK : DXGI_ERROR_NOT_FOUND;
}
void WriteAutomaticPipelineCache(const Sha1Digest &key, ID3DBlob *blob) {
  invokeDiskHook();
  {
    std::lock_guard lock(test_disk_mutex);
    ++writes; disk[key] = blob;
  }
  test_disk_ready.notify_all();
}
void RemoveAutomaticPipelineCache(const Sha1Digest &key) {
  invokeDiskHook();
  std::lock_guard lock(test_disk_mutex);
  ++removals; disk.erase(key);
}
HRESULT ClearAutomaticPipelineCacheDisk() {
  invokeDiskHook();
  std::lock_guard lock(test_disk_mutex);
  disk.clear(); return S_OK;
}
HRESULT ReadAutomaticStageArtifact(const std::string &, std::shared_ptr<const ShaderStageArtifact> &artifact) {
  artifact.reset(); return DXGI_ERROR_NOT_FOUND;
}
void WriteAutomaticStageArtifact(const std::string &, const ShaderStageArtifact &) {}
unsigned stage_publications = 0;
void QueueAutomaticStageArtifact(const std::string &, std::shared_ptr<const ShaderStageArtifact>, uint64_t) { ++stage_publications; }
void PublishAutomaticStageArtifact(const std::string &, std::shared_ptr<const ShaderStageArtifact>, uint64_t) {}
}
extern "C" bool WMTGetPipelineCacheIdentity(WMTPipelineCacheIdentity *identity) {
  *identity = {}; identity->compiler_uuid[0] = 1; return true;
}
extern "C" void SM50DestroyBitcode(sm50_bitcode_t) {}
extern "C" int SM50CopyCompiledBitcode(sm50_bitcode_t, SM50_COMPILED_BITCODE_PART, void *, uint64_t) { return 1; }
extern "C" void SM50GetCompiledBitcode(sm50_bitcode_t bitcode, SM50_COMPILED_BITCODE *out) {
  *out = {}; out->Data = bitcode; out->Size = 8; out->ConstantDataAlignment = 1;
}

namespace {
using namespace dxmt;
constexpr uint32_t stages = 1u << PipelineCache::Compute;
uint8_t library1[8] = {'M','T','L','B',1,0,0,0};
uint8_t library2[8] = {'M','T','L','B',2,0,0,0};
Sha1Digest key(unsigned value) { Sha1Digest key{}; key.data[0] = uint8_t(value); return key; }
Sha1Digest autoKey(unsigned value) {
  WMTPipelineCacheIdentity identity{}; PipelineCacheIdentity(identity);
  Sha1HashState hash; hash.update(key(value)).update(stages).update(uint64_t(7)).update(identity).update(kVersion);
  return hash.final();
}
void initialize(PipelineCache &cache, unsigned value) {
  require(cache.Initialize({}, key(value), stages, 7) == S_OK, "initialize");
}
void compile(PipelineCache &cache, bool newer = false) {
  *cache.output(PipelineCache::Compute) = newer ? library2 : library1;
  cache.Complete(S_OK);
}
void reset() {
  publication_signal.reset();
  require(AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR) == S_OK, "clear");
  require(AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) == S_OK, "enable");
}
void controls(unsigned flags) {
  require(AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAGS(flags)) == S_OK, "control");
  require(AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) == S_OK, "re-enable");
}
void ramHit(unsigned value) {
  auto hit = std::async(std::launch::async, [=] { PipelineCache cache; initialize(cache, value); return cache.loaded(); });
  require(await(hit), "RAM hit blocked by disk");
}
void newer(unsigned value) {
  PipelineCache cache; initialize(cache, value);
  if (cache.loaded()) {
    PipelineCache fresh; cache.RetryAutomatic(fresh); initialize(fresh, value); compile(fresh, true);
  } else {
    compile(cache, true);
  }
}
Com<ID3DBlob> waitForDisk(unsigned value) {
  const auto expected = autoKey(value);
  std::unique_lock lock(test_disk_mutex);
  require(test_disk_ready.wait_for(lock, 5s, [&] {
    auto it = disk.find(expected);
    return it != disk.end();
  }), "disk publication not observed");
  return disk.at(expected);
}
void expectNewDisk(unsigned value) {
  PipelineCache explicit_cache;
  auto blob = waitForDisk(value);
  require(explicit_cache.Initialize({blob->GetBufferPointer(), blob->GetBufferSize()}, key(value), stages, 7) == S_OK,
          "disk blob valid");
  require(static_cast<const uint8_t *>(explicit_cache.view(PipelineCache::Compute).Data)[4] == 2, "old disk write replaced new");
}
void phaseHook(Gate &gate, const char *event, const char *phase) {
  auto signal = publication_signal;
  std::lock_guard lock(worker_trace_mutex);
  worker_trace_hook = [&gate, event, phase, signal](const auto &name, const auto &fields) {
    if (name == event && fields.find(phase) != std::string::npos) gate.block();
    if (name == "pipeline_cache_auto_published" && signal &&
        fields.find(autoKey(3).string()) != std::string::npos)
      try { signal->set_value(); } catch (...) {}
  };
}
void clearWorkerTraceHook() {
  std::lock_guard lock(worker_trace_mutex);
  worker_trace_hook = {};
}
void armPublication(unsigned value) {
  publication_signal = std::make_shared<std::promise<void>>();
  const auto expected = autoKey(value).string();
  auto signal = publication_signal;
  std::function<void(const std::string &, const std::string &)> previous;
  { std::lock_guard lock(worker_trace_mutex); previous = worker_trace_hook; }
  std::lock_guard lock(worker_trace_mutex);
  worker_trace_hook = [previous, signal, expected](const auto &name, const auto &fields) {
    if (previous) previous(name, fields);
    if (name == "pipeline_cache_auto_published" && fields.find(expected) != std::string::npos)
      try { signal->set_value(); } catch (...) {}
  };
}
void awaitPublication() {
  auto signal = publication_signal;
  require(signal && signal->get_future().wait_for(5s) == std::future_status::ready, "publication drain");
  publication_signal.reset();
  { std::lock_guard lock(worker_trace_mutex); worker_trace_hook = {}; }
}

void testBlockedDisk() {
  for (unsigned operation = 0; operation < 2; ++operation) {
    reset(); PipelineCache resident; initialize(resident, 1); compile(resident);
    auto gate = std::make_shared<Gate>();
    auto blocked = std::async(std::launch::async, [&] {
      if (operation != 1) setDiskHook([gate] { gate->block(); });
      if (operation == 0) { PipelineCache miss; initialize(miss, 2); }
      if (operation == 1) { PipelineCache miss; initialize(miss, 2); setDiskHook([gate] { gate->block(); }); compile(miss); }
    });
    gate->wait();
    if (operation == 0) ramHit(1);
    gate->open(); await(blocked);
    setDiskHook({});
  }
  reset(); PipelineCache resident; initialize(resident, 1); compile(resident); controls(4);
  { std::lock_guard lock(test_disk_mutex); require(disk.empty(), "clear removed complete-pipeline publications"); }
  std::puts("PIPELINE_CACHE_BLOCKED_DISK_PASS phases=3 memory_independent=1 clear=1");
}

void testDelayedWrite() {
  for (unsigned flags : {4u, 1u, 5u}) {
    reset(); PipelineCache owner; initialize(owner, 3);
    std::promise<void> joined; auto joined_signal = joined.get_future();
    auto waiter = std::async(std::launch::async, [&] {
      trace_hook = [&](const auto &event, const auto &) { if (event == "pipeline_cache_auto_wait") joined.set_value(); };
      PipelineCache cache; initialize(cache, 3); return cache.loaded();
    });
    await(joined_signal);
    Gate gate;
    armPublication(3);
    auto producer = std::async(std::launch::async, [&] { phaseHook(gate, "pipeline_cache_auto_phase", "disk_write"); compile(owner); });
    gate.wait(); require(await(waiter), "joined waiter waited for persistence"); ramHit(3);
    controls(flags); gate.open(); await(producer); clearWorkerTraceHook(); armPublication(3); newer(3); awaitPublication(); expectNewDisk(3);
  }
  // Same generation, exact blob identity: old completed publication cannot overwrite a cold retry.
  reset(); PipelineCache owner; initialize(owner, 3); Gate gate; armPublication(3);
  auto producer = std::async(std::launch::async, [&] { phaseHook(gate, "pipeline_cache_auto_phase", "disk_write"); compile(owner); });
  gate.wait(); PipelineCache hit, fresh; initialize(hit, 3); hit.RetryAutomatic(fresh); initialize(fresh, 3); compile(fresh, true);
  gate.open(); await(producer); awaitPublication(); expectNewDisk(3);
  std::puts("PIPELINE_CACHE_DELAYED_WRITE_PASS controls=3 exact_blob=1 joined_before_persist=1");
}

void testDelayedReadAndInvalid() {
  for (bool invalid : {false, true}) for (unsigned flags : {4u, 1u, 5u}) {
    reset(); PipelineCache seed; initialize(seed, 4); compile(seed); waitForDisk(4);
    { auto lock = LockMetadata(Automatic()); Automatic().entries.clear(); Automatic().bytes = 0; }
    if (invalid) static_cast<uint8_t *>(waitForDisk(4)->GetBufferPointer())[0] ^= 1;
    Gate gate; PipelineCache owner;
    auto read = std::async(std::launch::async, [&] {
      phaseHook(gate, invalid ? "pipeline_cache_auto_phase" : "pipeline_cache_auto_timing",
                      invalid ? "disk_remove" : "disk_read");
      initialize(owner, 4);
    });
    gate.wait();
    std::promise<void> joined; auto joined_signal = joined.get_future();
    auto waiter = std::async(std::launch::async, [&] {
      trace_hook = [&](const auto &event, const auto &) { if (event == "pipeline_cache_auto_wait") joined.set_value(); };
      PipelineCache cache; initialize(cache, 4);
      return cache.loaded() ? static_cast<const uint8_t *>(cache.view(PipelineCache::Compute).Data)[4] : 0;
    });
    await(joined_signal); controls(flags); newer(4); gate.open(); await(read);
    clearWorkerTraceHook();
    if (invalid) compile(owner); else owner.Complete(S_OK);
    require(await(waiter) == 1, "old successful joined flight lost"); expectNewDisk(4);
  }
  std::puts("PIPELINE_CACHE_DELAYED_READ_PASS valid=3 invalid=3 old_waiters=6");
}

void testDelayedRetry() {
  for (unsigned flags : {4u, 1u, 5u}) {
    reset(); PipelineCache seed; initialize(seed, 5); compile(seed); waitForDisk(5);
    PipelineCache old, fresh; initialize(old, 5); require(old.CanRetryAutomatic(E_INVALIDARG), "automatic retry allowed");
    Gate gate;
    auto retry = std::async(std::launch::async, [&] { phaseHook(gate, "pipeline_cache_auto_phase", "disk_remove"); old.RetryAutomatic(fresh); });
    gate.wait(); controls(flags); newer(5); gate.open(); await(retry); clearWorkerTraceHook(); expectNewDisk(5);
    initialize(fresh, 5); require(!fresh.CanRetryAutomatic(E_INVALIDARG), "retry is bounded");
  }
  // A different exact blob in the same generation survives an older restored caller's retry.
  reset(); PipelineCache seed; initialize(seed, 5); compile(seed); waitForDisk(5); PipelineCache old, first, second;
  initialize(old, 5); initialize(first, 5); first.RetryAutomatic(second); initialize(second, 5); compile(second, true);
  PipelineCache fresh; old.RetryAutomatic(fresh); expectNewDisk(5);
  for (unsigned flags : {4u, 1u, 5u}) {
    reset(); PipelineCache seed; initialize(seed, 7); compile(seed); waitForDisk(7);
    { auto lock = LockMetadata(Automatic()); Automatic().entries.clear(); Automatic().bytes = 0; }
    PipelineCache restored, cold; initialize(restored, 7); require(restored.loaded(), "restore producer");
    std::promise<void> joined; auto signal = joined.get_future();
    auto waiter = std::async(std::launch::async, [&] {
      trace_hook = [&](const auto &event, const auto &) { if (event == "pipeline_cache_auto_wait") joined.set_value(); };
      PipelineCache cache; initialize(cache, 7);
      return cache.loaded() ? static_cast<const uint8_t *>(cache.view(PipelineCache::Compute).Data)[4] : 0;
    });
    await(signal); Gate gate;
    auto retry = std::async(std::launch::async, [&] {
      phaseHook(gate, "pipeline_cache_auto_phase", "disk_remove"); restored.RetryAutomatic(cold);
    });
    gate.wait(); controls(flags); newer(7); gate.open(); await(retry); clearWorkerTraceHook();
    initialize(cold, 7); require(!cold.loaded(), "fresh cold retry reused old native state");
    require(!cold.CanRetryAutomatic(E_INVALIDARG), "cold producer retries twice");
    compile(cold); require(await(waiter) == 1, "retired retry lost joined caller"); expectNewDisk(7);
  }
  std::puts("PIPELINE_CACHE_DELAYED_RETRY_PASS controls=3 exact_blob=1 bounded=1 old_cold_waiters=3");
}
void testStrictAndBounds() {
  reset(); PipelineCache seed; initialize(seed, 6); compile(seed); auto blob = waitForDisk(6);
  auto *bytes = static_cast<uint8_t *>(blob->GetBufferPointer());
  bytes[0] ^= 1;
  PipelineCache explicit_cache;
  require(explicit_cache.Initialize({bytes, blob->GetBufferSize()}, key(6), stages, 7) == E_INVALIDARG, "explicit corruption strict");
  require(!explicit_cache.CanRetryAutomatic(E_INVALIDARG), "explicit corruption never retries");
  bytes[0] ^= 1;
  for (unsigned n = 0; n < 520; ++n) {
    Sha1Digest retained{}; std::memcpy(retained.data, &n, sizeof(n));
    auto lock = LockMetadata(Automatic()); Automatic().Retain(retained, ShareBlob(blob.ptr()));
    require(Automatic().entries.size() <= kAutomaticEntries && Automatic().bytes <= kAutomaticBytes, "RAM bounds");
  }
  std::puts("PIPELINE_CACHE_STRICT_BOUNDS_PASS explicit=1 retry=0 entries=512");
}
}
void testStageArtifactKeys() {
  using namespace dxmt;
  SM50_SHADER_PSO_PIXEL_SHADER_DATA pixel{};
  pixel.type = SM50_SHADER_PSO_PIXEL_SHADER; pixel.sample_mask = 1; pixel.native_sample_count = 1;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA constant{};
  constant.type = SM50_SHADER_D3D12_SAMPLE_MASK_CONSTANT; constant.next = &pixel;
  auto key = [&](SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
    StageKeyBuilder builder; require(builder.arguments(args), "supported artifact key"); return builder.key;
  };
  const auto structural = key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&pixel));
  const auto specialized = key(&constant);
  pixel.sample_mask = 2;
  require(structural != key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&pixel)), "structural mask key");
  require(specialized == key(&constant), "constant value excluded from base AIR");
  pixel.native_sample_count = 2; require(specialized != key(&constant), "native signature still keyed");

  SM50_MSAA_VERTEX_LAYOUT first{}, second{};
  SM50_MSAA_INTERPOLATION_LAYOUT interpolation{};
  SM50_SHADER_D3D12_MSAA_VERTEX_DATA capture{};
  capture.type = SM50_SHADER_D3D12_MSAA_VERTEX; capture.variant = SM50_MSAA_VERTEX_CAPTURE;
  capture.sample_count = 4; capture.layout_out = &first; capture.interpolation = &interpolation;
  const auto captureKey = key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&capture));
  capture.layout_out = &second;
  require(captureKey == key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&capture)), "output addresses not keyed");
  interpolation.flat_count = 1; interpolation.flat_user_hashes[0] = 123;
  require(captureKey != key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&capture)), "interpolation linkage keyed");

  SM50_SHADER_GS_TESSELLATION_INPUT_DATA tess{};
  tess.type = SM50_SHADER_GS_TESSELLATION_INPUT;
  const auto tessKey = key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&tess));
  tess.layout.vertex_registers = 4;
  require(tessKey != key(reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&tess)), "TS GS linkage keyed");
  std::puts("PIPELINE_STAGE_KEY_PASS constant_values=2 structural=1 capture_output_addresses=2 interpolation=1 tess_linkage=1");
}

void testStageArtifactFlights() {
  using namespace dxmt;
  controls(6);
  stage_publications = 0;
  ArtifactCache<ShaderStageArtifact> cache;
  Gate producer;
  std::atomic_uint operations{0};
  auto first = std::async(std::launch::async, [&] {
    return cache.Get("linked-stage", "air_compile", [&] {
      ++operations; producer.block(); auto value = std::make_shared<ShaderStageArtifact>();
      value->metadata = {1, 2, 3, 4}; return value;
    });
  });
  producer.wait();
  std::promise<void> joined; auto signal = joined.get_future();
  auto second = std::async(std::launch::async, [&] {
    trace_hook = [&](const auto &event, const auto &) {
      if (event == "pipeline_artifact_wait") { joined.set_value(); throw E_ABORT; }
    };
    return cache.Get("linked-stage", "air_compile", [&] { ++operations; return std::make_shared<ShaderStageArtifact>(); });
  });
  await(signal); producer.open(); auto a = await(first), b = await(second);
  require(operations == 1 && a == b && a->metadata == std::vector<uint8_t>({1,2,3,4}), "single flight immutable metadata");
  require(stage_publications >= 1, "stage artifact publication was not queued");
  controls(4);
  auto rebuilt = cache.Get("linked-stage", "air_compile", [&] { ++operations; return std::make_shared<ShaderStageArtifact>(); });
  require(operations == 2 && rebuilt != a && a->metadata[0] == 1, "generation retires membership not ownership");
  Gate retired;
  auto old = std::async(std::launch::async, [&] {
    return cache.Get("retired-stage", "air_compile", [&] {
      ++operations; retired.block(); auto value = std::make_shared<ShaderStageArtifact>();
      value->metadata = {9}; return value;
    });
  });
  retired.wait(); controls(4);
  auto fresh = cache.Get("retired-stage", "air_compile", [&] { ++operations; return std::make_shared<ShaderStageArtifact>(); });
  retired.open(); auto retained = await(old);
  require(operations == 4 && fresh != retained && retained->metadata[0] == 9,
          "clear during conversion separates generation and retains old result");
  for (bool throws : {false, true}) {
    Gate failing;
    auto failed = std::async(std::launch::async, [&] {
      try {
        auto value = cache.Get("failed", "air_compile", [&]() -> std::shared_ptr<ShaderStageArtifact> {
          failing.block();
          if (throws) throw E_FAIL;
          return {};
        });
        require(!throws && !value, "null producer failure preserved");
      } catch (HRESULT hr) {
        require(throws && hr == E_FAIL, "producer exception preserved");
      }
    });
    failing.wait();
    std::promise<void> waiting; auto waiting_signal = waiting.get_future();
    auto waiter = std::async(std::launch::async, [&] {
      trace_hook = [&](const auto &event, const auto &) {
        if (event == "pipeline_artifact_wait") waiting.set_value();
      };
      return cache.Get("failed", "air_compile", [] {
        require(false, "failure waiter must join pending flight");
        return std::make_shared<ShaderStageArtifact>();
      });
    });
    await(waiting_signal); failing.open(); await(failed);
    require(!await(waiter), "producer failure wakes pending waiter with no artifact");
    auto retry = cache.Get("failed", "air_compile", [] { return std::make_shared<ShaderStageArtifact>(); });
    require(bool(retry), "failed stage not cached");
    controls(4);
  }
  for (unsigned i = 0; i < 300; ++i)
    cache.Get(std::to_string(i), "air_compile", [] { return std::make_shared<ShaderStageArtifact>(); });
  require(cache.completed <= 256 && cache.bytes <= 32u * 1024u * 1024u, "artifact retention bounded");
  std::puts("PIPELINE_STAGE_FLIGHT_PASS conversions=4 same_artifact=1 retained_after_clear=1 clear_pending=1 failure_retry=1 failure_wakeups=2 trace_exception=1 bounded=1");
}
void testCompiledArtifactKinds() {
  using namespace dxmt;
  ArtifactCache<WorkGraphArtifact> workgraphs;
  std::atomic_uint workgraph_compiles{0};
  auto workgraph = workgraphs.Get("workgraph", "wg_compile", [&] {
    ++workgraph_compiles;
    auto value = std::make_shared<WorkGraphArtifact>();
    value->library = {1, 2, 3};
    value->info.ThreadgroupSize[0] = 32;
    return value;
  });
  auto workgraph_hit = workgraphs.Get("workgraph", "wg_compile", [&] {
    ++workgraph_compiles;
    return std::make_shared<WorkGraphArtifact>();
  });
  require(workgraph == workgraph_hit && workgraph_compiles == 1 && workgraph->info.ThreadgroupSize[0] == 32,
          "workgraph artifact single flight");

  ArtifactCache<RaytracingArtifact> raytracing;
  std::atomic_uint raytracing_compiles{0};
  auto rt = raytracing.Get("raytracing", "rt_compile", [&] {
    ++raytracing_compiles;
    auto value = std::make_shared<RaytracingArtifact>();
    value->library = {4, 5, 6};
    value->names.emplace_back("metal_raygen");
    value->export_names.emplace_back("RayGen");
    value->functions.push_back({value->names[0].c_str(), value->export_names[0].c_str(),
                                SM50_RT_FUNCTION_RAYGEN, 0, 0, SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY});
    return value;
  });
  auto rt_hit = raytracing.Get("raytracing", "rt_compile", [&] {
    ++raytracing_compiles;
    return std::make_shared<RaytracingArtifact>();
  });
  require(rt == rt_hit && raytracing_compiles == 1 && rt->functions.size() == 1 &&
              std::strcmp(rt->functions[0].Name, "metal_raygen") == 0,
          "raytracing artifact owns reflection");
  std::puts("PIPELINE_COMPILED_ARTIFACT_KINDS_PASS workgraph=1 raytracing=1 reflection=1");
}
int main() {
  testStageArtifactKeys(); testStageArtifactFlights(); testCompiledArtifactKinds();
  testBlockedDisk(); testDelayedWrite(); testDelayedReadAndInvalid(); testDelayedRetry(); testStrictAndBounds();
  std::puts("PIPELINE_CACHE_CPU_PASS");
}
