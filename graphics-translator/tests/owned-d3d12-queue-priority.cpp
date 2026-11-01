#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "../../src/d3d12/d3d12_interfaces_ext.hpp"
#include <array>

#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

static void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_QUEUE_PRIORITY_FAIL step=%s actual=%08lx expected=%08lx\n", step,
                 static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
    ExitProcess(1);
  }
}

static void checkPriorities(ID3D12CommandQueue1 *queue, int creation) {
  D3D12_COMMAND_QUEUE_PROCESS_PRIORITY process;
  D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY global;
  expect(queue->GetProcessPriority(&process), S_OK, "GetProcessPriority");
  expect(queue->GetGlobalPriority(&global), S_OK, "GetGlobalPriority");
  expect(process == (creation ? D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH :
                               D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_NORMAL) ? S_OK : E_FAIL, S_OK, "process unchanged");
  expect(global == (creation == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME ?
      D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_HARD_REALTIME : D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_DEFAULT) ?
      S_OK : E_FAIL, S_OK, "global unchanged");
}

static void schedulingCopy(ID3D12Device *device, ID3D12CommandQueue1 *queue) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 4; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{};
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  ComPtr<ID3D12Resource> upload, readback;
  expect(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         nullptr, IID_PPV_ARGS(&upload)), S_OK, "upload");
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  expect(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&readback)), S_OK, "readback");
  void *mapped = nullptr;
  D3D12_RANGE empty{0, 0};
  expect(upload->Map(0, &empty, &mapped), S_OK, "map upload");
  const UINT sentinel = 0x2da61c93;
  std::memcpy(mapped, &sentinel, sizeof(sentinel)); upload->Unmap(0, nullptr);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  expect(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), S_OK, "allocator");
  expect(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)), S_OK, "list");
  list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 4);
  expect(list->Close(), S_OK, "close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  expect(queue->SetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH), S_FALSE, "pending process ignored");
  expect(queue->SetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_IDLE), S_FALSE, "pending global ignored");
  ComPtr<ID3D12Fence> fence;
  expect(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), S_OK, "fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  expect(event ? S_OK : E_FAIL, S_OK, "event");
  expect(queue->Signal(fence.Get(), 1), S_OK, "signal");
  expect(fence->SetEventOnCompletion(1, event), S_OK, "completion");
  expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0 ? S_OK : E_FAIL, S_OK, "wait");
  CloseHandle(event);
  D3D12_RANGE range{0, 4};
  expect(readback->Map(0, &range, &mapped), S_OK, "map readback");
  UINT result; std::memcpy(&result, mapped, sizeof(result)); readback->Unmap(0, &empty);
  expect(result == sentinel ? S_OK : E_FAIL, S_OK, "native scheduled copy");
  checkPriorities(queue, 0);
}

int main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  expect(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), S_OK, "device");
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  using BackendId = const char *(WINAPI *)();
  const auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  expect(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0 ? S_OK : E_FAIL, S_OK, "owned backend");
  std::puts("OWNED_QUEUE_PRIORITY_BACKEND id=owned-graphics-d3d12");
  D3D12_FEATURE_DATA_HARDWARE_SCHEDULING_QUEUE_GROUPINGS groups{~0u};
  expect(device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(d3dalloy::kFeatureHardwareSchedulingQueueGroupings),
                                     &groups, sizeof(groups)), S_OK, "groups");
  expect(groups.ComputeQueuesPer3DQueue == 0 ? S_OK : E_FAIL, S_OK, "one native queue per group");
  expect(device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(d3dalloy::kFeatureHardwareSchedulingQueueGroupings),
                                     &groups, 0), E_INVALIDARG, "groups size");
  for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY}) {
    for (int creation : {0, 100, 10000}) {
      for (bool dynamic : {false, true}) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type; desc.Priority = creation;
        desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(dynamic ? d3dalloy::kCommandQueueFlagAllowDynamicPriority : 0);
        ComPtr<ID3D12CommandQueue1> queue;
        expect(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), S_OK, "queue1");
        ComPtr<IUnknown> identity, base_identity;
        ComPtr<ID3D12CommandQueue> base;
        expect(queue.As(&identity), S_OK, "queue identity");
        expect(queue.As(&base), S_OK, "base QI");
        expect(base.As(&base_identity), S_OK, "base identity");
        expect(identity.Get() == base_identity.Get() ? S_OK : E_FAIL, S_OK, "COM identity");
        expect(queue->GetProcessPriority(nullptr), E_INVALIDARG, "null process");
        expect(queue->GetGlobalPriority(nullptr), E_INVALIDARG, "null global");
        for (int p : {-1, 2, 100})
          expect(queue->SetProcessPriority(static_cast<D3D12_COMMAND_QUEUE_PROCESS_PRIORITY>(p)), E_INVALIDARG, "bad process");
        for (int p : {-1, 3, 17, 33, 10000})
          expect(queue->SetGlobalPriority(static_cast<D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY>(p)), E_INVALIDARG, "bad global");
        const HRESULT ignored = dynamic ? S_FALSE : DXGI_ERROR_UNSUPPORTED;
        for (int p : {0, 1})
          expect(queue->SetProcessPriority(static_cast<D3D12_COMMAND_QUEUE_PROCESS_PRIORITY>(p)), ignored, "process setter");
        for (int p = 0; p <= 32; ++p)
          if (p <= 2 || p >= 18)
            expect(queue->SetGlobalPriority(static_cast<D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY>(p)), ignored, "global setter");
        std::array<std::thread, 4> threads;
        for (auto &thread : threads) thread = std::thread([&] {
          for (unsigned i = 0; i < 128; ++i) {
            expect(queue->SetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH), ignored, "concurrent process");
            expect(queue->SetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_IDLE), ignored, "concurrent global");
            checkPriorities(queue.Get(), creation);
          }
        });
        for (auto &thread : threads) thread.join();
        checkPriorities(queue.Get(), creation);
      }
    }
  }
  ComPtr<ID3D12Device9> device9;
  expect(device.As(&device9), S_OK, "device9");
  D3D12_COMMAND_QUEUE_DESC desc{};
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(d3dalloy::kCommandQueueFlagAllowDynamicPriority);
  ComPtr<ID3D12CommandQueue1> queue, other;
  const GUID creator = {0x27393735,0x145a,0x4b55,{0x89,0xe6,0x20,0x49,0xb8,0x4b,0xe0,0x5b}};
  expect(device9->CreateCommandQueue1(&desc, creator, IID_PPV_ARGS(&queue)), S_OK, "creator queue");
  expect(device9->CreateCommandQueue1(&desc, creator, IID_PPV_ARGS(&other)), S_OK, "same creator separate native group");
  expect(queue->SetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY_HARD_REALTIME), S_FALSE, "creator ignored");
  checkPriorities(other.Get(), 0);
  desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(4);
  ComPtr<ID3D12CommandQueue> invalid;
  expect(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&invalid)), E_INVALIDARG, "bad queue flags");
  const bool gpu = argc == 2 && std::strcmp(argv[1], "--gpu") == 0;
  if (gpu) schedulingCopy(device.Get(), queue.Get());
  std::printf("OWNED_QUEUE_PRIORITY_PASS setters=ignored getters=creation concurrency=4 native_copy=%u\n", gpu ? 1u : 0u);
  return 0;
}
