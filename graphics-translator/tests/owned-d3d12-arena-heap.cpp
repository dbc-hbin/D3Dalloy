#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <psapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <array>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_ARENA_HEAP_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr);
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void expect(HRESULT hr, HRESULT wanted, const char *step) { if (hr != wanted) fail(step, hr); }

void arena(ID3D12Device *device, bool uploadReuse = false) {
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues = 62;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = parameters;
  ComPtr<ID3DBlob> serialized, errors, shader;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "serialize-root");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
  constexpr char source[] =
      "cbuffer Constants : register(b0) { uint4 packed[15]; uint2 tail; };"
      "RWStructuredBuffer<uint> output : register(u0);"
      "[numthreads(1,1,1)] void main() { output[packed[0].x] = packed[0].y ^ tail.y; }";
  check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors), "shader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = root.Get();
  psoDesc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "pso");
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC resource{};
  resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  constexpr UINT reuseBurst = 60000;
  constexpr uint32_t guard = 0xd15ea5edu;
  resource.Width = (uploadReuse ? reuseBurst + 2 : 40000) * sizeof(uint32_t);
  resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
  resource.SampleDesc.Count = 1;
  resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> output;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)), "output");
  ComPtr<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "allocator");
  auto record = [&](UINT base, UINT count, bool invalidFirst = false, UINT salt = 0x12340000u,
                    ID3D12GraphicsCommandList *rerecord = nullptr) {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (rerecord) {
      list = rerecord;
      check(list->Reset(allocator.Get(), pso.Get()), "list-rerecord-reset");
    } else {
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)), "list");
    }
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + (uploadReuse ? sizeof(uint32_t) : 0));
    if (invalidFirst) list->SetComputeRoot32BitConstant(2, 1, 0);
    uint32_t constants[62]{};
    for (UINT i = 0; i < count; ++i) {
      constants[0] = base + i;
      constants[1] = salt + base + i;
      constants[61] = 0x5a5a5a5au;
      list->SetComputeRoot32BitConstants(0, 62, constants, 0);
      list->Dispatch(1, 1, 1);
    }
    return list;
  };
  ComPtr<ID3D12GraphicsCommandList> single;
  if (!uploadReuse) {
    single = record(0, 20000, true);
    expect(single->Close(), E_INVALIDARG, "first-recording-error-preserved");
    single.Reset();
    check(allocator->Reset(), "allocator-reset-after-first-error");
  }
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
  properties.Type = D3D12_HEAP_TYPE_READBACK;
  resource.Flags = D3D12_RESOURCE_FLAG_NONE;
  ComPtr<ID3D12Resource> readback;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
  properties.Type = D3D12_HEAP_TYPE_UPLOAD;
  ComPtr<ID3D12Resource> zeros;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zeros)), "zero-upload");
  void *mapped = nullptr;
  check(zeros->Map(0, nullptr, &mapped), "map-zero-upload");
  std::memset(mapped, 0, size_t(resource.Width));
  if (uploadReuse) {
    auto *values = static_cast<uint32_t *>(mapped);
    for (UINT i = 0; i < resource.Width / sizeof(uint32_t); ++i) values[i] = guard;
  }
  zeros->Unmap(0, nullptr);
  ComPtr<ID3D12CommandAllocator> copyAllocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&copyAllocator)), "copy-allocator");
  ComPtr<ID3D12GraphicsCommandList> copy;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, copyAllocator.Get(), nullptr, IID_PPV_ARGS(&copy)), "copy-list");
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
  copy->ResourceBarrier(1, &barrier);
  copy->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, resource.Width);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  copy->ResourceBarrier(1, &barrier);
  // Erase the output after taking the snapshot: a later replay must execute, not reuse old output.
  copy->CopyBufferRegion(output.Get(), 0, zeros.Get(), 0, resource.Width);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  copy->ResourceBarrier(1, &barrier);
  check(copy->Close(), "copy-close");
  ComPtr<ID3D12Fence> fence, gate;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) fail("event");
  UINT64 completion = 0;
  auto execute = [&](std::initializer_list<ID3D12CommandList *> lists, UINT count, UINT salt, bool blocked = false) {
    if (blocked) check(queue->Wait(gate.Get(), 1), "block-queue");
    std::array<ID3D12CommandList *, 4> submitted{};
    UINT submittedCount = 0;
    for (auto *list : lists) submitted[submittedCount++] = list;
    submitted[submittedCount++] = copy.Get();
    check(fence->SetEventOnCompletion(++completion, event), "completion");
    queue->ExecuteCommandLists(submittedCount, submitted.data());
    check(queue->Signal(fence.Get(), completion), "signal");
    if (blocked) {
      expect(allocator->Reset(), E_FAIL, "queued-allocator-reset-rejected");
      check(gate->Signal(1), "release-queue");
    }
    const DWORD waited = WaitForSingleObject(event, 30000);
    if (waited != WAIT_OBJECT_0) fail("wait", HRESULT_FROM_WIN32(waited));
    check(readback->Map(0, nullptr, &mapped), "map-readback");
    const auto *values = static_cast<const uint32_t *>(mapped);
    for (UINT i = 0; i < count; ++i)
      if (values[i + (uploadReuse ? 1 : 0)] != ((salt + i) ^ 0x5a5a5a5au)) fail("retained-root-values");
    if (uploadReuse) {
      if (values[0] != guard) fail("upload-reuse-leading-guard");
      for (UINT i = count + 1; i < resource.Width / sizeof(uint32_t); ++i)
        if (values[i] != guard) fail("upload-reuse-tail-guard");
    }
    readback->Unmap(0, nullptr);
  };
  if (uploadReuse) {
    ComPtr<ID3D12GraphicsCommandList> initialize;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, copyAllocator.Get(), nullptr,
          IID_PPV_ARGS(&initialize)), "guard-initialize-list");
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    initialize->ResourceBarrier(1, &barrier);
    initialize->CopyBufferRegion(output.Get(), 0, zeros.Get(), 0, resource.Width);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    initialize->ResourceBarrier(1, &barrier);
    check(initialize->Close(), "guard-initialize-close");
    execute({initialize.Get()}, 0, 0);
    auto cycle = [&](const char *phase, UINT count, UINT salt, bool blocked = false) {
      std::printf("OWNED_D3D12_UPLOAD_REUSE_BEGIN phase=%s count=%u salt=%08x\n", phase, count, salt);
      single = record(0, count, false, salt, single.Get());
      check(single->Close(), "upload-reuse-close");
      execute({single.Get()}, count, salt, blocked);
      // The snapshot/erase list makes replay prove execution, not stale output.
      execute({single.Get()}, count, salt);
      check(allocator->Reset(), "upload-reuse-completed-reset");
      std::printf("OWNED_D3D12_UPLOAD_REUSE_PHASE_PASS phase=%s count=%u executions=2\n", phase, count);
    };
    cycle("cold", 20000, 0x12340000u, true);
    cycle("warm", 20000, 0x23450000u);
    // Keep chunk zero active while extra chunks age past the reference 50 resets.
    for (UINT age = 0; age < 51; ++age)
      cycle("age-small", 1, 0x34560000u + age);
    cycle("after-age", 20000, 0x45670000u);
    cycle("byte-burst", reuseBurst, 0x56780000u);
    cycle("after-byte-trim", 20000, 0x67890000u);
    CloseHandle(event);
    expect(device->GetDeviceRemovedReason(), S_OK, "device-healthy");
    std::puts("OWNED_D3D12_UPLOAD_REUSE_PASS growth=20000 burst=60000 age_resets=51 recordings=56 executions=112 guards=60002");
    return;
  }
  single = record(0, 20000);
  check(single->Close(), "single-list-growth-close");
  execute({single.Get()}, 20000, 0x12340000u, true);
  execute({single.Get()}, 20000, 0x12340000u);
  check(allocator->Reset(), "allocator-reset-after-completion");
  single.Reset();
  auto first = record(0, 8000);
  check(first->Close(), "shared-first-close");
  auto second = record(8000, 8000);
  check(second->Close(), "shared-second-close");
  auto third = record(16000, 8000);
  check(third->Close(), "shared-third-close");
  execute({first.Get(), second.Get(), third.Get()}, 24000, 0x12340000u);
  check(allocator->Reset(), "allocator-reset-after-shared-retirement");
  first.Reset(); second.Reset(); third.Reset();

  D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  arguments[0].Constant.RootParameterIndex = 0;
  arguments[0].Constant.Num32BitValuesToSet = 62;
  arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  struct IndirectArguments { uint32_t constants[62]; D3D12_DISPATCH_ARGUMENTS dispatch; };
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
  signatureDesc.ByteStride = sizeof(IndirectArguments);
  signatureDesc.NumArgumentDescs = 2;
  signatureDesc.pArgumentDescs = arguments;
  ComPtr<ID3D12CommandSignature> signature;
  check(device->CreateCommandSignature(&signatureDesc, root.Get(), IID_PPV_ARGS(&signature)), "indirect-signature");
  resource.Width = 20000 * sizeof(IndirectArguments);
  ComPtr<ID3D12Resource> argumentBuffer;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&argumentBuffer)), "indirect-arguments");
  check(argumentBuffer->Map(0, nullptr, &mapped), "map-indirect-arguments");
  auto *payload = static_cast<IndirectArguments *>(mapped);
  std::memset(payload, 0, size_t(resource.Width));
  for (UINT i = 0; i < 20000; ++i) {
    payload[i].constants[0] = i;
    payload[i].constants[1] = 0x45670000u + i;
    payload[i].constants[61] = 0x5a5a5a5au;
    payload[i].dispatch = {1, 1, 1};
  }
  argumentBuffer->Unmap(0, nullptr);
  single = record(0, 0);
  single->ExecuteIndirect(signature.Get(), 20000, argumentBuffer.Get(), 0, nullptr, 0);
  check(single->Close(), "indirect-single-large-allocation-close");
  execute({single.Get()}, 20000, 0x45670000u);
  execute({single.Get()}, 20000, 0x45670000u);
  check(allocator->Reset(), "allocator-reset-after-indirect-retirement");
  single.Reset();
  single = record(0, 32, false, 0x789a0000u);
  check(single->Close(), "new-recording-after-reset-close");
  execute({single.Get()}, 32, 0x789a0000u);
  check(allocator->Reset(), "allocator-reset-after-new-recording-retirement");
  CloseHandle(event);
  expect(device->GetDeviceRemovedReason(), S_OK, "device-healthy");
  std::puts("OWNED_D3D12_ARENA_HEAP_ARENA_PASS single=20000 shared=24000 indirect=20000 reexecute=2 reset=1 first_error=E_INVALIDARG");
}

unsigned mappedViews(const wchar_t *name) {
  unsigned count = 0;
  uintptr_t cursor = 0;
  MEMORY_BASIC_INFORMATION info{};
  while (VirtualQuery(reinterpret_cast<void *>(cursor), &info, sizeof(info))) {
    if (info.Type == MEM_MAPPED && info.BaseAddress == info.AllocationBase) {
      wchar_t path[32768]{};
      if (GetMappedFileNameW(GetCurrentProcess(), info.AllocationBase, path, 32768) && !std::wcscmp(path, name))
        ++count;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= cursor) break;
    cursor = next;
  }
  return count;
}
void mapping(ID3D12Device *device) {
  ComPtr<ID3D12Device3> device3;
  check(device->QueryInterface(IID_PPV_ARGS(&device3)), "device3");
  wchar_t directory[MAX_PATH]{}, filename[MAX_PATH]{};
  if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"arh", 0, filename)) fail("temp-file");
  HANDLE file = CreateFileW(filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) fail("file");
  HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
  if (!section) fail("section");
  void *probe = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (!probe) fail("probe-view");
  wchar_t name[32768]{};
  if (!GetMappedFileNameW(GetCurrentProcess(), probe, name, 32768)) fail("mapped-name");
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(probe, &info, sizeof(info)) || info.RegionSize >= 16384) fail("probe-must-fail-host-page-validation");
  const unsigned before = mappedViews(name);
  if (before != 1) fail("probe-view-count");
  for (unsigned i = 0; i < 32; ++i) {
    ID3D12Heap *heap = reinterpret_cast<ID3D12Heap *>(uintptr_t(1));
    expect(device3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heap)), E_INVALIDARG, "small-mapping-rejection");
    if (heap) fail("failed-output-not-null");
  }
  const unsigned after = mappedViews(name);
  if (after != before) fail("failed-construction-leaks-view");
  CloseHandle(section);
  section = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 65536, nullptr);
  if (!section) fail("valid-section");
  ComPtr<ID3D12Heap> heap;
  check(device3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heap)), "valid-mapping");
  if (mappedViews(name) != before + 1 || heap->GetDesc().SizeInBytes != 65536) fail("successful-view-retention");
  heap.Reset();
  if (mappedViews(name) != before) fail("successful-view-release");
  ComPtr<ID3D12Resource> wrongInterface;
  expect(device3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&wrongInterface)), E_NOINTERFACE, "mapping-query-failure");
  if (wrongInterface || mappedViews(name) != before) fail("query-failure-view-release");
  UnmapViewOfFile(probe);
  CloseHandle(section);
  CloseHandle(file);
  std::printf("OWNED_D3D12_ARENA_HEAP_MAPPING_PASS failures=32 views_before=%u views_after=%u\n", before, after);
}
}
int main(int argc, char **argv) {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  using BackendId = const char *(WINAPI *)();
  const auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) fail("owned-backend");
  std::puts("OWNED_D3D12_ARENA_HEAP_BACKEND id=owned-graphics-d3d12");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  if (argc != 2) fail("usage-arena-or-mapping-or-upload-reuse");
  if (!std::strcmp(argv[1], "arena")) arena(device.Get());
  else if (!std::strcmp(argv[1], "mapping")) mapping(device.Get());
  else if (!std::strcmp(argv[1], "upload-reuse")) arena(device.Get(), true);
  else fail("unknown-mode");
  return 0;
}
