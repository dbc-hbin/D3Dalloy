#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kTimeoutMs = 15000;
constexpr UINT kWords = 64;
constexpr UINT64 kBytes = kWords * sizeof(std::uint32_t);
constexpr UINT kPrimaryWord = 9;
constexpr UINT kSourceWord = 3;
constexpr std::uint32_t kGuard = 0xa5a5a5a5u;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void require(bool condition, const char *step) {
  if (!condition)
    fail(step);
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using BackendId = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backendId != nullptr, "owned-backend-id-export");
  const char *id = backendId();
  require(id && std::strcmp(id, "owned-graphics-d3d12") == 0, "owned-backend-id");
  std::printf("OWNED_D3D12_ATOMIC_COPY_BACKEND id=%s\n", id);
}

std::vector<std::uint8_t> readShader(const char *path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("open-atomic-reader-dxil", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16ll * 1024 * 1024) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    fail("size-atomic-reader-dxil", error ? HRESULT_FROM_WIN32(error) : E_INVALIDARG);
  }
  std::vector<std::uint8_t> bytes(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    fail("read-atomic-reader-dxil", HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT));
  }
  CloseHandle(file);
  return bytes;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = heapType;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)), "create-buffer");
  return resource;
}

ComPtr<ID3D12Resource> createTexture(ID3D12Device *device) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 4;
  desc.Height = 4;
  desc.DepthOrArraySize = 2;
  desc.MipLevels = 2;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                        nullptr, IID_PPV_ARGS(&resource)), "create-texture");
  return resource;
}

void writeWords(ID3D12Resource *upload, std::uint32_t base) {
  std::uint32_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "upload-map");
  for (UINT i = 0; i < kWords; ++i)
    mapped[i] = base + i;
  upload->Unmap(0, nullptr);
}

void writeQwords(ID3D12Resource *upload, std::uint64_t fill, UINT sourceWord, std::uint64_t value) {
  std::uint64_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "upload64-map");
  for (UINT i = 0; i < kBytes / 8; ++i)
    mapped[i] = i == sourceWord ? value : fill;
  upload->Unmap(0, nullptr);
}

D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

struct Queue {
  D3D12_COMMAND_LIST_TYPE type;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 value = 0;

  ~Queue() {
    if (event)
      CloseHandle(event);
  }

  void create(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE listType) {
    type = listType;
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(list->Close(), "initial-close");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
  }

  ID3D12GraphicsCommandList1 *begin() {
    check(allocator->Reset(), "allocator-reset");
    check(list->Reset(allocator.Get(), nullptr), "list-reset");
    return list.Get();
  }

  void submit() {
    check(list->Close(), "close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "set-event");
      require(WaitForSingleObject(event, kTimeoutMs) == WAIT_OBJECT_0, "wait");
    }
  }

};

// A failed Close permanently poisons the list, so each error case records into
// a fresh allocator/list pair.
template <typename Record>
HRESULT recordAndClose(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, Record &&record) {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "case-allocator");
  check(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "case-list");
  record(list.Get());
  return list->Close();
}

// The source word is itself produced earlier in the same list and the payload
// is copied into a dependent buffer before the publish. The destination is
// consumed by a later copy in the same list, so both sides of the ordering
// contract are observed.
void runOrderedCase(ID3D12Device *device, Queue &queue, UINT dependencies) {
  auto upload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto destination = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto payload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto readback = createBuffer(device, 3 * kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto guards = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  const std::uint32_t base = 0x10000000u * (static_cast<std::uint32_t>(queue.type) + 1) + dependencies * 0x100u;
  writeWords(upload.Get(), base);
  {
    std::uint32_t *mapped = nullptr;
    check(guards->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "guard-map");
    for (UINT i = 0; i < kWords; ++i)
      mapped[i] = kGuard;
    guards->Unmap(0, nullptr);
  }

  auto *list = queue.begin();
  list->CopyBufferRegion(destination.Get(), 0, guards.Get(), 0, kBytes);
  list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, kBytes);
  list->CopyBufferRegion(payload.Get(), 0, upload.Get(), 0, kBytes);
  auto sourceReady = transitionBarrier(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &sourceReady);
  ID3D12Resource *dependents[] = {payload.Get()};
  D3D12_SUBRESOURCE_RANGE_UINT64 ranges[] = {{0, {0, kBytes}}};
  list->AtomicCopyBufferUINT(destination.Get(), kPrimaryWord * sizeof(std::uint32_t), source.Get(),
                             kSourceWord * sizeof(std::uint32_t), dependencies,
                             dependencies ? dependents : nullptr, dependencies ? ranges : nullptr);
  D3D12_RESOURCE_BARRIER readReady[] = {
      transitionBarrier(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transitionBarrier(payload.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
  };
  list->ResourceBarrier(2, readReady);
  list->CopyBufferRegion(readback.Get(), 0, destination.Get(), 0, kBytes);
  list->CopyBufferRegion(readback.Get(), kBytes, payload.Get(), 0, kBytes);
  list->CopyBufferRegion(readback.Get(), 2 * kBytes, source.Get(), 0, kBytes);
  queue.submit();

  const D3D12_RANGE range{0, static_cast<SIZE_T>(3 * kBytes)};
  std::uint32_t *mapped = nullptr;
  check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "readback-map");
  for (UINT i = 0; i < kWords; ++i) {
    const std::uint32_t expected = i == kPrimaryWord ? base + kSourceWord : kGuard;
    if (mapped[i] != expected) {
      std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY_MISMATCH type=%d deps=%u word=%u got=0x%08x want=0x%08x\n",
                   queue.type, dependencies, i, mapped[i], expected);
      fail("destination");
    }
    require(mapped[kWords + i] == base + i, "payload");
    require(mapped[2 * kWords + i] == base + i, "source-preserved");
  }
  const D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
}

// Whole-resource GPU readback proves both halves and the adjacent guards.
// A pair of 32-bit shader Load operations is NOT an atomic 64-bit reader, so
// this completed-work check makes no claim about intermediate live visibility.
void run64Case(ID3D12Device *device, Queue &queue, UINT dependencies, UINT scenario) {
  constexpr std::uint64_t guard = 0x9a3b6c1d82e4f507ull;
  constexpr UINT destinationWord = 10, sourceWord = 5;
  struct Values { std::uint64_t initial, source; };
  constexpr Values values[] = {
      {0, 0xfedcba9876543210ull},
      {0xffffffffffffffffull, 0x0123456789abcdefull},
      {0x8765432100000000ull, 0x87654321ffffffffull},
      {0x87654321ffffffffull, 0x8765432100000000ull},
      {0x0123456789abcdefull, 0},
      {0, 0xffffffffffffffffull},
  };
  require(scenario < sizeof(values) / sizeof(values[0]), "64-scenario");
  const Values v = values[scenario];
  auto initial = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto upload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto destination = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto source = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto payload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto readback = createBuffer(device, 3 * kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  writeQwords(initial.Get(), guard, destinationWord, v.initial);
  writeQwords(upload.Get(), guard, sourceWord, v.source);
  auto *list = queue.begin();
  list->CopyBufferRegion(destination.Get(), 0, initial.Get(), 0, kBytes);
  // GPU-produced source and payload, both submitted before the atomic.
  list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, kBytes);
  list->CopyBufferRegion(payload.Get(), 0, upload.Get(), 0, kBytes);
  auto sourceReady = transitionBarrier(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &sourceReady);
  ID3D12Resource *dependents[] = {payload.Get(), payload.Get()};
  D3D12_SUBRESOURCE_RANGE_UINT64 ranges[] = {{0, {0, 8}}, {0, {8, kBytes}}};
  list->AtomicCopyBufferUINT64(destination.Get(), destinationWord * 8, source.Get(), sourceWord * 8,
                               dependencies, dependencies ? dependents : nullptr,
                               dependencies ? ranges : nullptr);
  D3D12_RESOURCE_BARRIER readReady[] = {
      transitionBarrier(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transitionBarrier(payload.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
  };
  list->ResourceBarrier(2, readReady);
  list->CopyBufferRegion(readback.Get(), 0, destination.Get(), 0, kBytes);
  list->CopyBufferRegion(readback.Get(), kBytes, payload.Get(), 0, kBytes);
  list->CopyBufferRegion(readback.Get(), 2 * kBytes, source.Get(), 0, kBytes);
  queue.submit();
  const D3D12_RANGE range{0, static_cast<SIZE_T>(3 * kBytes)};
  std::uint64_t *mapped = nullptr;
  check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "readback64-map");
  for (UINT i = 0; i < kBytes / 8; ++i) {
    const std::uint64_t expected = i == destinationWord ? v.source : guard;
    if (mapped[i] != expected || mapped[kBytes / 8 + i] != (i == sourceWord ? v.source : guard) ||
        mapped[2 * kBytes / 8 + i] != (i == sourceWord ? v.source : guard)) {
      std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY64_MISMATCH type=%d deps=%u scenario=%u word=%u dst=%016" PRIx64
                           " payload=%016" PRIx64 " source=%016" PRIx64 " expected=%016" PRIx64 "\n",
                   queue.type, dependencies, scenario, i, mapped[i], mapped[kBytes / 8 + i],
                   mapped[2 * kBytes / 8 + i], expected);
      fail("uint64-readback");
    }
  }
  const D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
}

struct ComputeProducer {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pipeline;

  void create(ID3D12Device *device) {
    // One bounded dispatch writes the source on GPU; subsequent copy and
    // AtomicCopy require the prior compute encoder's writes to be visible.
    constexpr char shader[] = R"(
RWByteAddressBuffer source : register(u0);
cbuffer Values : register(b0) { uint low; uint high; };
[numthreads(1, 1, 1)] void main() { source.Store2(40, uint2(low, high)); }
)";
    ComPtr<ID3DBlob> bytecode, errors;
    const HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
                                  0, 0, &bytecode, &errors);
    if (FAILED(hr) && errors)
      std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY_SHADER_ERROR %s\n",
                   static_cast<const char *>(errors->GetBufferPointer()));
    check(hr, "compile-compute-producer");
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 2;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
          "serialize-compute-producer");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "root-compute-producer");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
    pipelineDesc.pRootSignature = root.Get();
    pipelineDesc.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
    check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)), "pipeline-compute-producer");
  }
};

void runComputeProducedCase(ID3D12Device *device, Queue &queue, const ComputeProducer &producer,
                            bool predicateFalse) {
  constexpr std::uint64_t guard = 0xac3f71d2559908e4ull;
  constexpr std::uint64_t value = 0xe123fabc87654321ull;
  auto upload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto destination = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto payload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = createBuffer(device, 3 * kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicate = createBuffer(device, 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = createBuffer(device, 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  writeQwords(upload.Get(), guard, 10, 0);
  std::uint64_t *zeroMapped = nullptr;
  check(zero->Map(0, nullptr, reinterpret_cast<void **>(&zeroMapped)), "predicate-map");
  *zeroMapped = 0;
  zero->Unmap(0, nullptr);
  auto *list = queue.begin();
  list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, kBytes);
  list->CopyBufferRegion(destination.Get(), 0, upload.Get(), 0, kBytes);
  list->CopyBufferRegion(predicate.Get(), 0, zero.Get(), 0, 8);
  D3D12_RESOURCE_BARRIER barriers[3] = {};
  const auto transition = [&](UINT index, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after) {
    barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[index].Transition.pResource = resource;
    barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[index].Transition.StateBefore = before;
    barriers[index].Transition.StateAfter = after;
  };
  transition(0, source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(2, predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
  list->ResourceBarrier(1, barriers);
  if (predicateFalse)
    list->ResourceBarrier(1, barriers + 2);
  list->SetPipelineState(producer.pipeline.Get());
  list->SetComputeRootSignature(producer.root.Get());
  list->SetComputeRootUnorderedAccessView(0, source->GetGPUVirtualAddress());
  const UINT constants[2] = {static_cast<UINT>(value), static_cast<UINT>(value >> 32)};
  list->SetComputeRoot32BitConstants(1, 2, constants, 0);
  list->Dispatch(1, 1, 1);
  transition(0, source.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, barriers);
  list->CopyBufferRegion(payload.Get(), 0, source.Get(), 0, kBytes);
  if (predicateFalse)
    list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  ID3D12Resource *dependents[] = {payload.Get()};
  D3D12_SUBRESOURCE_RANGE_UINT64 ranges[] = {{0, {0, kBytes}}};
  list->AtomicCopyBufferUINT64(destination.Get(), 10 * 8, source.Get(), 5 * 8, 1, dependents, ranges);
  if (predicateFalse)
    list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(0, destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(1, payload.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(2, barriers);
  list->CopyBufferRegion(readback.Get(), 0, destination.Get(), 0, kBytes);
  list->CopyBufferRegion(readback.Get(), kBytes, payload.Get(), 0, kBytes);
  list->CopyBufferRegion(readback.Get(), 2 * kBytes, source.Get(), 0, kBytes);
  queue.submit();
  const D3D12_RANGE range{0, static_cast<SIZE_T>(3 * kBytes)};
  std::uint64_t *mapped = nullptr;
  check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "compute-readback-map");
  for (UINT i = 0; i < kBytes / 8; ++i) {
    // The shared initializer puts zero at destination word 10 in all three
    // buffers; only the destination is subsequently overwritten by AtomicCopy.
    const std::uint64_t expectedSource = i == 5 ? value : (i == 10 ? 0 : guard);
    const std::uint64_t expectedDestination = i == 10 ? value : guard;
    if (mapped[i] != expectedDestination || mapped[kBytes / 8 + i] != expectedSource ||
        mapped[2 * kBytes / 8 + i] != expectedSource) {
      std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY64_COMPUTE_MISMATCH list=%d predication_false=%d word=%u dst=%016" PRIx64
                           " payload=%016" PRIx64 " source=%016" PRIx64
                           " expected_dst=%016" PRIx64 " expected_source=%016" PRIx64 "\n",
                   queue.type, predicateFalse, i, mapped[i], mapped[kBytes / 8 + i],
                   mapped[2 * kBytes / 8 + i], expectedDestination, expectedSource);
      fail("compute-readback");
    }
  }
  const D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
  std::printf("OWNED_D3D12_ATOMIC_COPY64_COMPUTE_PASS list=%d predication_false=%d payload=1\n",
              queue.type, predicateFalse);
}

// Delaying execution after the application releases its closed list checks
// that the command stream retains the internally generated Metal pipeline.
// The application resources remain alive through fence completion.
void runLifetimeReplay(ID3D12Device *device) {
  Queue queue;
  queue.create(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  constexpr std::uint64_t value = 0xecdaf12356789abfull;
  auto upload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto destination = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto readback = createBuffer(device, kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  writeQwords(upload.Get(), 0, 5, value);
  auto *list = queue.begin();
  list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, kBytes);
  list->CopyBufferRegion(destination.Get(), 0, upload.Get(), 0, kBytes);
  auto sourceReady = transitionBarrier(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &sourceReady);
  list->AtomicCopyBufferUINT64(destination.Get(), 10 * 8, source.Get(), 5 * 8, 0, nullptr, nullptr);
  auto destinationReady = transitionBarrier(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &destinationReady);
  list->CopyBufferRegion(readback.Get(), 0, destination.Get(), 0, kBytes);
  check(list->Close(), "lifetime-close");
  const auto checkResult = [&] {
    const D3D12_RANGE range{0, static_cast<SIZE_T>(kBytes)};
    std::uint64_t *mapped = nullptr;
    check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "lifetime-map");
    for (UINT i = 0; i < kBytes / 8; ++i)
      require(mapped[i] == ((i == 5 || i == 10) ? value : 0), "lifetime-result");
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
  };
  ID3D12CommandList *lists[] = {list};
  queue.queue->ExecuteCommandLists(1, lists);
  check(queue.queue->Signal(queue.fence.Get(), ++queue.value), "replay-signal");
  check(queue.fence->SetEventOnCompletion(queue.value, queue.event), "replay-event");
  require(WaitForSingleObject(queue.event, kTimeoutMs) == WAIT_OBJECT_0, "replay-wait");
  checkResult();
  // Re-execute the exact same closed list, not a freshly re-recorded clone.
  queue.queue->ExecuteCommandLists(1, lists);
  check(queue.queue->Signal(queue.fence.Get(), ++queue.value), "replay2-signal");
  check(queue.fence->SetEventOnCompletion(queue.value, queue.event), "replay2-event");
  require(WaitForSingleObject(queue.event, kTimeoutMs) == WAIT_OBJECT_0, "replay2-wait");
  checkResult();
  ComPtr<ID3D12Fence> gate;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "lifetime-gate");
  check(queue.queue->Wait(gate.Get(), 1), "lifetime-queue-wait");
  queue.queue->ExecuteCommandLists(1, lists);
  queue.list.Reset();
  check(gate->Signal(1), "lifetime-release-gate");
  check(queue.queue->Signal(queue.fence.Get(), ++queue.value), "lifetime-signal");
  check(queue.fence->SetEventOnCompletion(queue.value, queue.event), "lifetime-event");
  require(WaitForSingleObject(queue.event, kTimeoutMs) == WAIT_OBJECT_0, "lifetime-wait");
  checkResult();
  std::puts("OWNED_D3D12_ATOMIC_COPY64_LIFETIME_PASS replay=2 released_list=1");
}

// Bounded concurrent observation, not a 64-bit atomic-read oracle. DXBC
// ByteAddressBuffer.Load2 lowers to two 32-bit loads; only the low generation
// is sampled here (the high half stays fixed). No shader waits for a queue.
// The immutable payload slot for generation g must be visible once g is read.
// This detects observed publication ordering, but cannot certify a whole
// 64-bit snapshot or rule out tearing of a separately loaded high half.
void runLiveReader(ID3D12Device *device, const std::vector<std::uint8_t> &readerShader, bool strict) {
  constexpr UINT count = 128, threads = 64, groups = 4;
  constexpr std::uint32_t high = 0xdeadbeefu;
  constexpr std::uint32_t seed = 0x7135a9cdu;
  ComPtr<ID3DBlob> errors, serialized;
  D3D12_ROOT_PARAMETER params[4] = {};
  for (UINT i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[i].Descriptor.ShaderRegister = i;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[3].Constants.ShaderRegister = 0;
  params[3].Constants.Num32BitValues = 1;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 4; rootDesc.pParameters = params;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
        "serialize-live-reader");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "root-live-reader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = root.Get();
  psoDesc.CS = {readerShader.data(), readerShader.size()};
  ComPtr<ID3D12PipelineState> pipeline;
  check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline)), "pipeline-live-reader");

  auto init = createBuffer(device, 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto sourceUpload = createBuffer(device, count * 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto payloadUpload = createBuffer(device, count * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto primary = createBuffer(device, 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto source = createBuffer(device, count * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto payload = createBuffer(device, count * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto samples = createBuffer(device, groups * threads * 12, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto result = createBuffer(device, groups * threads * 12, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST);
  std::uint64_t *initMap = nullptr, *sourceMap = nullptr;
  std::uint32_t *payloadMap = nullptr;
  check(init->Map(0, nullptr, reinterpret_cast<void **>(&initMap)), "live-init-map");
  *initMap = (std::uint64_t(high) << 32);
  init->Unmap(0, nullptr);
  check(sourceUpload->Map(0, nullptr, reinterpret_cast<void **>(&sourceMap)), "live-source-map");
  check(payloadUpload->Map(0, nullptr, reinterpret_cast<void **>(&payloadMap)), "live-payload-map");
  for (UINT i = 0; i < count; ++i) {
    sourceMap[i] = (std::uint64_t(high) << 32) | (i + 1);
    payloadMap[i] = seed ^ ((i + 1) * 0x9e3779b9u);
  }
  sourceUpload->Unmap(0, nullptr);
  payloadUpload->Unmap(0, nullptr);
  Queue writer, reader;
  writer.create(device, D3D12_COMMAND_LIST_TYPE_COPY);
  reader.create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
  auto *initialList = writer.begin();
  initialList->CopyBufferRegion(primary.Get(), 0, init.Get(), 0, 8);
  initialList->CopyBufferRegion(source.Get(), 0, sourceUpload.Get(), 0, count * 8);
  writer.submit();
  auto *readerList = reader.begin();
  readerList->SetPipelineState(pipeline.Get());
  readerList->SetComputeRootSignature(root.Get());
  readerList->SetComputeRootUnorderedAccessView(0, primary->GetGPUVirtualAddress());
  readerList->SetComputeRootUnorderedAccessView(1, payload->GetGPUVirtualAddress());
  readerList->SetComputeRootUnorderedAccessView(2, samples->GetGPUVirtualAddress());
  for (UINT group = 0; group < groups; ++group) {
    readerList->SetComputeRoot32BitConstant(3, group * threads, 0);
    readerList->Dispatch(1, 1, 1);
  }
  D3D12_RESOURCE_BARRIER outputBarrier = {};
  outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  outputBarrier.Transition.pResource = samples.Get();
  outputBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  outputBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  outputBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  readerList->ResourceBarrier(1, &outputBarrier);
  readerList->CopyBufferRegion(result.Get(), 0, samples.Get(), 0, groups * threads * 12);
  check(readerList->Close(), "live-reader-close");
  auto *writerList = writer.begin();
  const auto recordPublication = [&](UINT i) {
    writerList->CopyBufferRegion(payload.Get(), i * 4, payloadUpload.Get(), i * 4, 4);
    ID3D12Resource *dependents[] = {payload.Get()};
    D3D12_SUBRESOURCE_RANGE_UINT64 ranges[] = {{0, {i * 4, i * 4 + 4}}};
    writerList->AtomicCopyBufferUINT64(primary.Get(), 0, source.Get(), i * 8, 1, dependents, ranges);
  };
  recordPublication(0);
  check(writerList->Close(), "live-writer-first-close");
  // Close and prepare the first publication before the reader starts, then
  // stream short submissions so it can observe early committed generations.
  // Reuse the closed list but never reset its allocator while GPU work is pending.
  ID3D12CommandList *readerLists[] = {readerList}, *writerLists[] = {writerList};
  reader.queue->ExecuteCommandLists(1, readerLists);
  writer.queue->ExecuteCommandLists(1, writerLists);
  check(reader.queue->Signal(reader.fence.Get(), ++reader.value), "live-reader-signal");
  for (UINT i = 1; i < count; ++i) {
    check(writer.list->Reset(writer.allocator.Get(), nullptr), "live-writer-reset");
    recordPublication(i);
    check(writerList->Close(), "live-writer-close");
    writer.queue->ExecuteCommandLists(1, writerLists);
  }
  check(writer.queue->Signal(writer.fence.Get(), ++writer.value), "live-writer-signal");
  if (writer.fence->GetCompletedValue() < writer.value) {
    check(writer.fence->SetEventOnCompletion(writer.value, writer.event), "live-writer-event");
    require(WaitForSingleObject(writer.event, kTimeoutMs) == WAIT_OBJECT_0, "live-writer-wait");
  }
  if (reader.fence->GetCompletedValue() < reader.value) {
    check(reader.fence->SetEventOnCompletion(reader.value, reader.event), "live-reader-event");
    require(WaitForSingleObject(reader.event, kTimeoutMs) == WAIT_OBJECT_0, "live-reader-wait");
  }
  const D3D12_RANGE readRange{0, groups * threads * 12};
  std::uint32_t *mapped = nullptr;
  check(result->Map(0, &readRange, reinterpret_cast<void **>(&mapped)), "live-result-map");
  UINT observed = 0, changedThreads = 0, first = count + 1, last = 0;
  for (UINT i = 0; i < groups * threads; ++i) {
    require(mapped[3 * i + 2] == 1, "live-payload-before-publish");
    require(mapped[3 * i] <= mapped[3 * i + 1] && mapped[3 * i + 1] <= count,
            "live-sample-written");
    if (mapped[3 * i + 1] > 0) {
      ++observed;
      if (mapped[3 * i] < first) first = mapped[3 * i];
      if (mapped[3 * i + 1] > last) last = mapped[3 * i + 1];
    }
    if (mapped[3 * i] < mapped[3 * i + 1])
      ++changedThreads;
  }
  const D3D12_RANGE written{0, 0};
  result->Unmap(0, &written);
  std::printf("OWNED_D3D12_ATOMIC_COPY64_LIVE_%s observed=%u changed_threads=%u first=%u last=%u atomic64_snapshot=unproven\n",
              changedThreads ? "OBSERVED" : "INCONCLUSIVE", observed, changedThreads, first, last);
  // Ordered control: the same closed reader list must see the final publish
  // and its payload after the writer fence. This is not an overlap claim.
  reader.queue->ExecuteCommandLists(1, readerLists);
  check(reader.queue->Signal(reader.fence.Get(), ++reader.value), "live-control-signal");
  if (reader.fence->GetCompletedValue() < reader.value) {
    check(reader.fence->SetEventOnCompletion(reader.value, reader.event), "live-control-event");
    require(WaitForSingleObject(reader.event, kTimeoutMs) == WAIT_OBJECT_0, "live-control-wait");
  }
  check(result->Map(0, &readRange, reinterpret_cast<void **>(&mapped)), "live-control-map");
  for (UINT i = 0; i < groups * threads; ++i) {
    if (mapped[3 * i] != count || mapped[3 * i + 1] != count || mapped[3 * i + 2] != 1) {
      std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY64_CONTROL_MISMATCH thread=%u first=%u last=%u bad_plus_one=%u\n",
                   i, mapped[3 * i], mapped[3 * i + 1], mapped[3 * i + 2]);
      fail("live-control-result");
    }
  }
  result->Unmap(0, &written);
  std::printf("OWNED_D3D12_ATOMIC_COPY64_CONTROL_PASS threads=%u final=%u payload=1\n",
              groups * threads, count);
  if (strict)
    require(changedThreads > 0, "live-overlap-not-observed");
}

void runErrors(ID3D12Device *device, ID3D12Device *otherDevice, Queue &queue) {
  auto source = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto destination = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto texture = createTexture(device);
  auto foreign = createBuffer(otherDevice, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  int cases = 0;
  const auto expect = [&](HRESULT expected, const char *step, auto &&record) {
    const HRESULT hr = recordAndClose(device, queue.type, record);
    if (hr != expected) {
      std::fprintf(stderr, "OWNED_D3D12_ATOMIC_COPY_FAIL step=%s expected=0x%08lx actual=0x%08lx\n", step,
                   static_cast<unsigned long>(expected), static_cast<unsigned long>(hr));
      ExitProcess(1);
    }
    ++cases;
  };
  ID3D12Resource *textureDependent[] = {texture.Get()};
  D3D12_SUBRESOURCE_RANGE_UINT64 validTexture[] = {{3, {0, 0}}};
  D3D12_SUBRESOURCE_RANGE_UINT64 invalidTexture[] = {{4, {0, 0}}};
  ID3D12Resource *bufferDependent[] = {source.Get()};
  D3D12_SUBRESOURCE_RANGE_UINT64 oobBuffer[] = {{0, {0, kBytes + 1}}};
  D3D12_SUBRESOURCE_RANGE_UINT64 reversed[] = {{0, {8, 4}}};
  D3D12_SUBRESOURCE_RANGE_UINT64 bufferSubresource[] = {{1, {0, 4}}};
  ID3D12Resource *foreignDependent[] = {foreign.Get()};
  D3D12_SUBRESOURCE_RANGE_UINT64 foreignRange[] = {{0, {0, 4}}};

  expect(S_OK, "texture-dependency", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, textureDependent, validTexture);
  });
  expect(S_OK, "last-element", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), kBytes - 4, source.Get(), kBytes - 4, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "dst-misaligned", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 2, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "src-misaligned", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 6, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "dst-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), kBytes, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "src-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), kBytes, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "null-primary", [&](auto *list) {
    list->AtomicCopyBufferUINT(nullptr, 0, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "texture-primary", [&](auto *list) {
    list->AtomicCopyBufferUINT(texture.Get(), 0, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "foreign-primary", [&](auto *list) {
    list->AtomicCopyBufferUINT(foreign.Get(), 0, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "null-dependency-arrays", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "texture-subresource-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, textureDependent, invalidTexture);
  });
  expect(E_INVALIDARG, "buffer-range-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, bufferDependent, oobBuffer);
  });
  expect(E_INVALIDARG, "reversed-range", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, bufferDependent, reversed);
  });
  expect(E_INVALIDARG, "buffer-subresource", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, bufferDependent, bufferSubresource);
  });
  expect(E_INVALIDARG, "foreign-dependency", [&](auto *list) {
    list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 1, foreignDependent, foreignRange);
  });
  expect(E_INVALIDARG, "uint64-misaligned", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 4, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(S_OK, "uint64-supported", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 8, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(S_OK, "uint64-last-element", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), kBytes - 8, source.Get(), kBytes - 8, 0, nullptr, nullptr);
  });
  expect(S_OK, "uint64-dependency", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, textureDependent, validTexture);
  });
  expect(E_INVALIDARG, "uint64-src-misaligned", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 4, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-dst-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), kBytes, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-src-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), kBytes, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-null-primary", [&](auto *list) {
    list->AtomicCopyBufferUINT64(nullptr, 0, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-texture-primary", [&](auto *list) {
    list->AtomicCopyBufferUINT64(texture.Get(), 0, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-foreign-primary", [&](auto *list) {
    list->AtomicCopyBufferUINT64(foreign.Get(), 0, source.Get(), 0, 0, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-null-dependency-arrays", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, nullptr, nullptr);
  });
  expect(E_INVALIDARG, "uint64-texture-subresource-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, textureDependent, invalidTexture);
  });
  expect(E_INVALIDARG, "uint64-buffer-range-oob", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, bufferDependent, oobBuffer);
  });
  expect(E_INVALIDARG, "uint64-reversed-range", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, bufferDependent, reversed);
  });
  expect(E_INVALIDARG, "uint64-buffer-subresource", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, bufferDependent, bufferSubresource);
  });
  expect(E_INVALIDARG, "uint64-foreign-dependency", [&](auto *list) {
    list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 1, foreignDependent, foreignRange);
  });

  require(recordAndClose(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, [&](auto *list) {
            list->AtomicCopyBufferUINT(destination.Get(), 0, source.Get(), 0, 0, nullptr, nullptr);
          }) == E_INVALIDARG, "bundle-rejected");
  ++cases;
  require(recordAndClose(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, [&](auto *list) {
            list->AtomicCopyBufferUINT64(destination.Get(), 0, source.Get(), 0, 0, nullptr, nullptr);
          }) == E_INVALIDARG, "uint64-bundle-rejected");
  ++cases;
  std::printf("OWNED_D3D12_ATOMIC_COPY_VALIDATION_PASS cases=%d\n", cases);
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  require(argc == 2 || (argc == 3 && std::strcmp(argv[2], "--live-reader") == 0),
          "usage-atomic-copy-reader-dxil-[--live-reader]");
  const auto readerShader = readShader(argv[1]);
  requireOwnedBackend();
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Device> otherDevice;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice)), "other-device");
  require(device.Get() != otherDevice.Get(), "distinct-devices");
  ComputeProducer producer;
  producer.create(device.Get());

  const D3D12_COMMAND_LIST_TYPE types[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                           D3D12_COMMAND_LIST_TYPE_COPY};
  const char *names[] = {"direct", "compute", "copy"};
  for (UINT t = 0; t < 3; ++t) {
    Queue queue;
    queue.create(device.Get(), types[t]);
    runOrderedCase(device.Get(), queue, 0);
    runOrderedCase(device.Get(), queue, 1);
    const UINT dependencyCounts[] = {0, 2};
    for (UINT deps : dependencyCounts)
      for (UINT scenario = 0; scenario < 6; ++scenario)
        run64Case(device.Get(), queue, deps, scenario);
    std::printf("OWNED_D3D12_ATOMIC_COPY_CASE_PASS list=%s uint32=1 dependencies=0,1 ordered=1\n", names[t]);
    std::printf("OWNED_D3D12_ATOMIC_COPY64_CASE_PASS list=%s scenarios=6 dependencies=0,2 exact=1\n", names[t]);
    if (t != 2)
      runComputeProducedCase(device.Get(), queue, producer, t == 0);
    if (t == 0)
      runErrors(device.Get(), otherDevice.Get(), queue);
  }
  runLifetimeReplay(device.Get());
  runLiveReader(device.Get(), readerShader, argc == 3);
  std::puts("OWNED_D3D12_ATOMIC_COPY_PASS uint32=1 uint64=1");
  return 0;
}
