#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";

int fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_CORE_CONTRACTS_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  return 1;
}

int expectHr(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) {
    std::fprintf(stderr,
                 "OWNED_D3D12_CORE_CONTRACTS_FAIL step=%s expected=0x%08lx actual=0x%08lx\n",
                 step, static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    return 1;
  }
  return 0;
}

D3D12_HEAP_PROPERTIES defaultHeapProperties() {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  properties.CreationNodeMask = 1;
  properties.VisibleNodeMask = 1;
  return properties;
}

D3D12_RESOURCE_DESC smallBufferDesc() {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 64 * 1024;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

int run() {
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  if (!d3d12)
    return fail("GetModuleHandleW(d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));

  using BackendIdFn = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"));
  if (!backendId)
    return fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
  const char *id = backendId();
  if (!id || std::strcmp(id, kExpectedBackendId) != 0)
    return fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_CORE_CONTRACTS_BACKEND id=%s\n", id);

  ComPtr<IDXGIFactory4> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr))
    return fail("CreateDXGIFactory2", hr);

  ComPtr<IDXGIAdapter1> adapter;
  hr = factory->EnumAdapters1(0, &adapter);
  if (FAILED(hr))
    return fail("EnumAdapters1", hr);

  ComPtr<ID3D12Device> device;
  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr))
    return fail("D3D12CreateDevice", hr);

  ComPtr<ID3D12Device4> device4;
  hr = device.As(&device4);
  if (FAILED(hr))
    return fail("QueryInterface(ID3D12Device4)", hr);

  ComPtr<ID3D12CommandAllocator> directAllocator;
  ComPtr<ID3D12CommandAllocator> computeAllocator;
  if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&directAllocator))))
    return fail("CreateCommandAllocator(DIRECT)", hr);
  if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&computeAllocator))))
    return fail("CreateCommandAllocator(COMPUTE)", hr);

  ComPtr<ID3D12GraphicsCommandList> list;
  if (FAILED(hr = device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               D3D12_COMMAND_LIST_FLAG_NONE,
                                               IID_PPV_ARGS(&list))))
    return fail("CreateCommandList1(DIRECT)", hr);
  if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return fail("CreateCommandList1 GetType mismatch");

  int failures = 0;
  failures += expectHr(list->Close(), E_FAIL, "CreateCommandList1 must start closed");
  failures += expectHr(list->Reset(directAllocator.Get(), nullptr), S_OK,
                       "Reset matching DIRECT allocator");
  failures += expectHr(list->Close(), S_OK, "Close after matching Reset");
  failures += expectHr(list->Reset(computeAllocator.Get(), nullptr), E_INVALIDARG,
                       "Reset wrong COMPUTE allocator type");

  ComPtr<ID3D12Device> otherDevice;
  ComPtr<ID3D12CommandAllocator> otherDirectAllocator;
  if (SUCCEEDED(hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice))) &&
      SUCCEEDED(hr = otherDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&otherDirectAllocator)))) {
    failures += expectHr(list->Reset(otherDirectAllocator.Get(), nullptr), E_INVALIDARG,
                         "Reset allocator from another device");
  } else {
    failures += fail("create second device/allocator for ownership test", hr);
  }

  failures += expectHr(list->Reset(directAllocator.Get(), nullptr), S_OK,
                       "Reset remains usable after wrong allocator rejection");
  failures += expectHr(list->Close(), S_OK, "Close after recovery Reset");

  void *invalidOut = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
  hr = device4->CreateCommandList1(2, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   D3D12_COMMAND_LIST_FLAG_NONE,
                                   __uuidof(ID3D12GraphicsCommandList), &invalidOut);
  failures += expectHr(hr, E_INVALIDARG, "CreateCommandList1 invalid NodeMask");
  if (invalidOut != nullptr)
    failures += fail("CreateCommandList1 invalid NodeMask did not clear output");

  invalidOut = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
  hr = device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   static_cast<D3D12_COMMAND_LIST_FLAGS>(1),
                                   __uuidof(ID3D12GraphicsCommandList), &invalidOut);
  failures += expectHr(hr, E_INVALIDARG, "CreateCommandList1 invalid flags");
  if (invalidOut != nullptr)
    failures += fail("CreateCommandList1 invalid flags did not clear output");

  invalidOut = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
  hr = device4->CreateCommandList1(0, static_cast<D3D12_COMMAND_LIST_TYPE>(99),
                                   D3D12_COMMAND_LIST_FLAG_NONE,
                                   __uuidof(ID3D12GraphicsCommandList), &invalidOut);
  failures += expectHr(hr, E_INVALIDARG, "CreateCommandList1 invalid type");
  if (invalidOut != nullptr)
    failures += fail("CreateCommandList1 invalid type did not clear output");

  invalidOut = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
  hr = device->CreateGraphicsPipelineState(nullptr, __uuidof(ID3D12PipelineState), &invalidOut);
  failures += expectHr(hr, E_INVALIDARG, "CreateGraphicsPipelineState null desc");
  if (invalidOut != nullptr)
    failures += fail("CreateGraphicsPipelineState null desc did not clear output");

  invalidOut = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
  hr = device->CreateComputePipelineState(nullptr, __uuidof(ID3D12PipelineState), &invalidOut);
  failures += expectHr(hr, E_INVALIDARG, "CreateComputePipelineState null desc");
  if (invalidOut != nullptr)
    failures += fail("CreateComputePipelineState null desc did not clear output");

  const D3D12_HEAP_PROPERTIES heapProperties = defaultHeapProperties();
  const D3D12_RESOURCE_DESC bufferDesc = smallBufferDesc();

  ComPtr<ID3D12Resource> resource;
  hr = device4->CreateCommittedResource1(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr, nullptr,
                                         IID_PPV_ARGS(&resource));
  if (FAILED(hr))
    failures += fail("CreateCommittedResource1(null protected session)", hr);

  hr = device4->CreateCommittedResource1(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr, nullptr,
                                         __uuidof(ID3D12Resource), nullptr);
  failures += expectHr(hr, S_FALSE, "CreateCommittedResource1 capability probe");

  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  heapDesc.Properties = heapProperties;
  heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

  ComPtr<ID3D12Heap> heap;
  hr = device4->CreateHeap1(&heapDesc, nullptr, IID_PPV_ARGS(&heap));
  if (FAILED(hr))
    failures += fail("CreateHeap1(null protected session)", hr);

  hr = device4->CreateHeap1(&heapDesc, nullptr, __uuidof(ID3D12Heap), nullptr);
  failures += expectHr(hr, S_FALSE, "CreateHeap1 capability probe");

  if (failures)
    return 1;

  std::puts("OWNED_D3D12_CORE_CONTRACTS_PASS");
  return 0;
}

} // namespace

int wmain() {
  return run();
}
