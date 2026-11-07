// Native operation counts/failure injection around the actual production methods.
// GPU behavior is covered separately by the owned predication/resolve/MetalFX fixtures.
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Device;
namespace WMT {
struct Error {};
struct Library { Device *owner; };
struct Function { std::string name; };
struct ComputePipelineState { unsigned identity; };
template <typename T> using Reference = std::shared_ptr<T>;
}
struct Device {
#include PRODUCTION_HELPER_KINDS
  std::mutex owned_helper_pipeline_mutex;
  WMT::Reference<WMT::Library> owned_predication_library;
  std::array<WMT::Reference<WMT::ComputePipelineState>, PredicationKernelCount> owned_predication_pipelines;
  unsigned libraryCalls = 0, functionCalls = 0, pipelineCalls = 0, failAt = 0;
  bool failLibrary = false;
  WMT::Reference<WMT::Library> builtIn;
  unsigned builtInCreations = 0;
  Device &GetLib() { return *this; }
  WMT::Reference<WMT::Library> getLibrary() {
    if (!builtIn) { ++builtInCreations; builtIn = std::make_shared<WMT::Library>(WMT::Library{this}); }
    return builtIn;
  }
  Device &GetMTLDevice() { return *this; }
  Device &GetMetal4Compiler() { return *this; }
  WMT::Reference<WMT::Library> newLibraryWithSource(const char *, WMT::Reference<WMT::Error> &) {
    ++libraryCalls;
    if (failLibrary) { failLibrary = false; return {}; }
    return std::make_shared<WMT::Library>(WMT::Library{this});
  }
  WMT::Reference<WMT::ComputePipelineState> newComputePipelineState(
      const WMT::Reference<WMT::Function> &, WMT::Reference<WMT::Error> &) {
    if (++pipelineCalls == failAt) return {};
    return std::make_shared<WMT::ComputePipelineState>(WMT::ComputePipelineState{pipelineCalls});
  }
};
namespace WMT4 {
struct Library {
  WMT::Reference<WMT::Library> value;
  explicit Library(WMT::Reference<WMT::Library> library) : value(std::move(library)) {}
  WMT::Reference<WMT::Function> newFunction(const char *name) {
    ++value->owner->functionCalls;
    return std::make_shared<WMT::Function>(WMT::Function{name});
  }
};
}
struct List {
  using CommandHelper = Device::CommandHelper;
  Device *device_;
  using PredicationKernel = Device::PredicationKernel;
  using enum Device::PredicationKernel;
  decltype(Device::owned_predication_pipelines) predicate_pipelines_;
  explicit List(Device &device) : device_(&device) {}
#include PRODUCTION_HELPER
};

using UINT = unsigned;
struct TextureViewKey {};
struct TextureDesc {
  uint32_t format = 1, type = 2, firstMiplevel = 3, miplevelCount = 4, firstArraySlice = 5, arraySize = 6;
};
struct PoolInfo { uint32_t format, type, firstMip, mipCount, firstSlice, slices; };
struct Texture {
  bool supported = true;
  unsigned identity;
  bool supportsDescriptorViewPool(TextureViewKey) { return supported; }
  TextureDesc viewDescriptor(TextureViewKey) { return {}; }
  Texture *current() { return this; }
  unsigned texture() { return identity; }
};
struct Pool {
  unsigned clears = 0, sets = 0, texture = 0;
  bool fail = false;
  void clear(unsigned) { ++clears; texture = 0; }
  uint64_t setTextureView(unsigned value, PoolInfo info, unsigned) {
    assert(info.firstMip == 3 && info.slices == 6);
    ++sets;
    if (fail) return 0;
    texture = value; return value + 100;
  }
};
struct Heap {
  struct Slot { unsigned texture = 0; PoolInfo info{}; };
  struct Argument { struct { uint64_t resource_id = 0; } SRVTexture; };
  std::array<Slot, 1> pool_slots_{};
  std::array<Argument, 1> mapped_argument_buffer_{};
  Pool view_pool_;
  bool pool_supported_ = true;
#include PRODUCTION_SLOTS
};

int main() {
  Device shared;
  std::array<std::unique_ptr<List>, 12> lists;
  std::array<std::thread, 12> workers;
  std::barrier start(int(workers.size()));
  for (size_t i = 0; i < workers.size(); ++i) {
    lists[i] = std::make_unique<List>(shared);
    workers[i] = std::thread([&, i] {
      start.arrive_and_wait();
      for (unsigned k = 0; k < Device::PredicationKernelCount; ++k) {
        const auto kind = static_cast<Device::PredicationKernel>(k);
        assert(lists[i]->EnsurePredicationPipeline(kind));
        assert(lists[i]->EnsurePredicationPipeline(kind));
      }
    });
  }
  for (auto &worker : workers) worker.join();
  constexpr unsigned count = Device::PredicationKernelCount;
  assert(shared.libraryCalls == 1 && shared.functionCalls == count && shared.pipelineCalls == count);
  for (const auto &list : lists) assert(list->predicate_pipelines_ == shared.owned_predication_pipelines);
  Device other; List independent(other);
  assert(independent.EnsurePredicationPipeline(Device::PredicateArguments) && other.pipelineCalls == 1);
  assert(independent.predicate_pipelines_[0] != lists[0]->predicate_pipelines_[0]);
  Device failed; failed.failAt = 2; List first(failed), retry(failed);
  assert(first.EnsurePredicationPipeline(Device::PredicateArguments));
  auto preserved = failed.owned_predication_pipelines[0];
  assert(!first.EnsurePredicationPipeline(Device::PredicateCopy));
  assert(retry.EnsurePredicationPipeline(Device::PredicateCopy) && retry.EnsurePredicationPipeline(Device::PredicateArguments));
  assert(failed.libraryCalls == 1 && failed.pipelineCalls == 3 && retry.predicate_pipelines_[0] == preserved);
  Device failedLibrary; failedLibrary.failLibrary = true; List libraryRetry(failedLibrary);
  assert(!libraryRetry.EnsurePredicationPipeline(Device::PredicateCopy) && libraryRetry.EnsurePredicationPipeline(Device::PredicateCopy));
  assert(failedLibrary.libraryCalls == 2 && failedLibrary.pipelineCalls == 1);
  std::printf("STATIC_OPT_HELPER_PASS lists=12 library_creations=1 pipeline_creations=%u device_isolation=1 failed_retry=1\n", count);

  Device commands;
  using Kind = Device::CommandHelper;
  constexpr size_t kinds = size_t(Kind::Count);
  std::array<std::array<WMT::Reference<WMT::ComputePipelineState>, kinds>, 12> results;
  for (size_t i = 0; i < workers.size(); ++i)
    workers[i] = std::thread([&, i] {
      List list(commands);
      for (size_t j = 0; j < kinds; ++j) {
        const auto kind = Kind(j);
        const auto name = std::to_string(j);
        results[i][j] = list.SharedCommandHelper(kind, name.c_str(),
            kind >= Kind::AtomicCopyU32 && kind <= Kind::RaysIndirect ? "fixed source" : nullptr);
        assert(results[i][j]);
      }
    });
  for (auto &worker : workers) worker.join();
  assert(commands.pipelineCalls == kinds && commands.functionCalls == kinds &&
      commands.libraryCalls == 4 && commands.builtInCreations == 1);
  for (const auto &row : results) for (size_t j = 0; j < kinds; ++j) assert(row[j] == results[0][j]);
  List unrelated(other);
  assert(unrelated.SharedCommandHelper(Kind::MatrixConversion, "matrix", "source") !=
      results[0][size_t(Kind::MatrixConversion)]);
  Device miss; miss.failAt = 1; List retryHelper(miss);
  assert(!retryHelper.SharedCommandHelper(Kind::MatrixConversion, "matrix", "source"));
  assert(retryHelper.SharedCommandHelper(Kind::MatrixConversion, "matrix", "source"));
  assert(miss.libraryCalls == 1 && miss.pipelineCalls == 2);
  std::printf("STATIC_OPT_REMAINING_HELPER_PASS lists=12 helpers=%zu source_libraries=4 builtin_library=1 retry=1 isolation=1\n", kinds);

  Heap heap; Texture a{true, 1}, b{true, 2};
  assert(heap.SetPoolSlot(0, &a, {}) && heap.SetPoolSlot(0, &b, {}));
  assert(heap.view_pool_.sets == 2 && heap.view_pool_.clears == 0 && heap.view_pool_.texture == 2);
  b.supported = false;
  assert(!heap.SetPoolSlot(0, &b, {}) && heap.view_pool_.clears == 1 && !heap.pool_slots_[0].texture);
  assert(heap.SetPoolSlot(0, &a, {}));
  heap.view_pool_.fail = true;
  assert(heap.SetPoolSlot(0, &a, {}) && heap.view_pool_.clears == 2 && !heap.view_pool_.texture);
  assert(heap.mapped_argument_buffer_[0].SRVTexture.resource_id == 0);
  heap.pool_supported_ = false;
  const auto sets = heap.view_pool_.sets;
  assert(!heap.SetPoolSlot(0, &a, {}) && heap.view_pool_.sets == sets);
  std::puts("STATIC_OPT_DESCRIPTOR_PASS replace_without_clear=1 fallback_clear=1 failed_set_null=1 disabled_pool=1");
}
