#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
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

  D3D12_ROOT_SIGNATURE_DESC nullGraph{};
  nullGraph.NumParameters = 1;
  ComPtr<ID3DBlob> rootBlob;
  failures += expectHr(D3D12SerializeRootSignature(&nullGraph, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                    &rootBlob, nullptr),
                       E_INVALIDARG, "root signature null parameter graph");

  D3D12_ROOT_PARAMETER tooManyConstants{};
  tooManyConstants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  tooManyConstants.Constants.Num32BitValues = 65;
  tooManyConstants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC tooExpensive{1, &tooManyConstants, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  failures += expectHr(D3D12SerializeRootSignature(&tooExpensive, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                    &rootBlob, nullptr),
                       E_INVALIDARG, "root signature 65 DWORD cost");

  std::array<D3D12_DESCRIPTOR_RANGE, 64> exactRanges{};
  std::array<D3D12_ROOT_PARAMETER, 64> exactParameters{};
  for (UINT i = 0; i < exactParameters.size(); ++i) {
    exactRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    exactRanges[i].NumDescriptors = 1;
    exactRanges[i].BaseShaderRegister = i;
    exactRanges[i].OffsetInDescriptorsFromTableStart = 0;
    exactParameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    exactParameters[i].DescriptorTable = {1, &exactRanges[i]};
    exactParameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC exactCost{static_cast<UINT>(exactParameters.size()), exactParameters.data(),
                                      0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rootBlob.Reset();
  hr = D3D12SerializeRootSignature(&exactCost, D3D_ROOT_SIGNATURE_VERSION_1_0, &rootBlob, nullptr);
  failures += expectHr(hr, S_OK, "root signature exact 64 DWORD cost");
  ComPtr<ID3D12RootSignature> exactRoot;
  if (SUCCEEDED(hr))
    failures += expectHr(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                                      IID_PPV_ARGS(&exactRoot)),
                         S_OK, "create exact 64 DWORD root signature");

  std::array<D3D12_ROOT_PARAMETER, 2> overlapParameters{};
  for (auto &parameter : overlapParameters) {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 1;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC overlapDesc{2, overlapParameters.data(), 0, nullptr,
                                        D3D12_ROOT_SIGNATURE_FLAG_NONE};
  failures += expectHr(D3D12SerializeRootSignature(&overlapDesc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                    &rootBlob, nullptr),
                       E_INVALIDARG, "overlapping root bindings");

  D3D12_STATIC_SAMPLER_DESC invalidSampler{};
  invalidSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  invalidSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  invalidSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  invalidSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  invalidSampler.MaxAnisotropy = 1;
  invalidSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  invalidSampler.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(99);
  invalidSampler.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC invalidSamplerDesc{0, nullptr, 1, &invalidSampler,
                                               D3D12_ROOT_SIGNATURE_FLAG_NONE};
  failures += expectHr(D3D12SerializeRootSignature(&invalidSamplerDesc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                    &rootBlob, nullptr),
                       E_INVALIDARG, "invalid static sampler enum");

  D3D12_DESCRIPTOR_RANGE1 invalidRange{};
  invalidRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  invalidRange.NumDescriptors = 1;
  invalidRange.Flags = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(0x10);
  D3D12_ROOT_PARAMETER1 invalidRangeParameter{};
  invalidRangeParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  invalidRangeParameter.DescriptorTable = {1, &invalidRange};
  invalidRangeParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC invalidVersioned{};
  invalidVersioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  invalidVersioned.Desc_1_1 = {1, &invalidRangeParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  failures += expectHr(D3D12SerializeVersionedRootSignature(&invalidVersioned, &rootBlob, nullptr),
                       E_INVALIDARG, "invalid 1.1 descriptor range flags");

  constexpr UINT volatileDescriptors = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  constexpr UINT volatileData = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
  constexpr UINT executeStaticData = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
  constexpr UINT staticData = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
  constexpr UINT keepBounds = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS;
  struct RangeCase {
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    UINT flags;
    bool valid;
  };
  constexpr RangeCase rangeCases[] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, volatileDescriptors, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, volatileDescriptors | volatileData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, volatileDescriptors | executeStaticData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, volatileData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, executeStaticData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, staticData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, keepBounds, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, keepBounds | volatileData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, keepBounds | executeStaticData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, keepBounds | staticData, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, volatileDescriptors, true},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, volatileDescriptors | staticData, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, volatileDescriptors | keepBounds, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, volatileDescriptors | keepBounds | volatileData, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, volatileData | executeStaticData, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, executeStaticData | staticData, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, volatileData, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, keepBounds, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, volatileDescriptors | keepBounds, false},
      {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0x10, false},
  };
  invalidRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
  invalidRange.NumDescriptors = 2;
  invalidRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  const int rangeFailuresBefore = failures;
  for (const auto &rangeCase : rangeCases) {
    invalidRange.RangeType = rangeCase.type;
    invalidRange.Flags = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(rangeCase.flags);
    rootBlob.Reset();
    const HRESULT serializeHr = D3D12SerializeVersionedRootSignature(&invalidVersioned, &rootBlob, nullptr);
    if (!rangeCase.valid) {
      if (serializeHr != E_INVALIDARG) {
        std::fprintf(stderr, "invalid range type=%u flags=0x%x serialize hr=0x%08lx\n",
                     static_cast<UINT>(rangeCase.type), rangeCase.flags, static_cast<unsigned long>(serializeHr));
        ++failures;
      }
      continue;
    }
    if (FAILED(serializeHr) || !rootBlob) {
      std::fprintf(stderr, "valid range type=%u flags=0x%x serialize hr=0x%08lx\n",
                   static_cast<UINT>(rangeCase.type), rangeCase.flags, static_cast<unsigned long>(serializeHr));
      ++failures;
      continue;
    }
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    hr = D3D12CreateVersionedRootSignatureDeserializer(rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                                        IID_PPV_ARGS(&deserializer));
    if (FAILED(hr)) {
      failures += fail("deserialize valid 1.1 range", hr);
      continue;
    }
    const auto *roundTrip = deserializer->GetUnconvertedRootSignatureDesc();
    if (!roundTrip || roundTrip->Version != D3D_ROOT_SIGNATURE_VERSION_1_1 ||
        roundTrip->Desc_1_1.NumParameters != 1 ||
        roundTrip->Desc_1_1.pParameters[0].DescriptorTable.NumDescriptorRanges != 1 ||
        roundTrip->Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].NumDescriptors != 2 ||
        roundTrip->Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].OffsetInDescriptorsFromTableStart !=
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ||
        roundTrip->Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].Flags != invalidRange.Flags ||
        roundTrip->Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].RangeType != rangeCase.type)
      failures += fail("round-trip 1.1 descriptor table bounds, flags and type");
    ComPtr<ID3D12RootSignature> rangeRoot;
    hr = device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&rangeRoot));
    if (FAILED(hr)) {
      std::fprintf(stderr, "valid range type=%u flags=0x%x CreateRootSignature hr=0x%08lx\n",
                   static_cast<UINT>(rangeCase.type), rangeCase.flags, static_cast<unsigned long>(hr));
      ++failures;
    }
  }
  if (failures == rangeFailuresBefore)
    std::puts("OWNED_D3D12_CORE_CONTRACTS_RANGE_FLAGS_PASS");

  D3D12_ROOT_PARAMETER constants{};
  constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  constants.Constants.Num32BitValues = 2;
  constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC constantsDesc{1, &constants, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rootBlob.Reset();
  hr = D3D12SerializeRootSignature(&constantsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rootBlob, nullptr);
  ComPtr<ID3D12RootSignature> constantsRoot;
  if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                                             rootBlob->GetBufferSize(),
                                                             IID_PPV_ARGS(&constantsRoot)))) {
    failures += fail("create constants root signature", hr);
  } else {
    ComPtr<ID3D12GraphicsCommandList> bindingList;
    if (FAILED(hr = device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 D3D12_COMMAND_LIST_FLAG_NONE,
                                                 IID_PPV_ARGS(&bindingList))) ||
        FAILED(hr = bindingList->Reset(directAllocator.Get(), nullptr))) {
      failures += fail("create binding validation list", hr);
    } else {
      bindingList->SetGraphicsRootSignature(constantsRoot.Get());
      bindingList->SetGraphicsRootConstantBufferView(0, 0);
      failures += expectHr(bindingList->Close(), E_INVALIDARG, "root setter type mismatch poisons Close");
      failures += expectHr(bindingList->Reset(directAllocator.Get(), nullptr), E_INVALIDARG,
                           "root setter Close failure permanently poisons Reset");
    }
  }

  if (otherDevice && rootBlob) {
    ComPtr<ID3D12RootSignature> foreignRoot;
    if (SUCCEEDED(hr = otherDevice->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                                        IID_PPV_ARGS(&foreignRoot)))) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC foreignPso{};
      const UINT invalidShader = 0;
      foreignPso.pRootSignature = foreignRoot.Get();
      foreignPso.CS = {&invalidShader, sizeof(invalidShader)};
      ComPtr<ID3D12PipelineState> rejectedPso;
      failures += expectHr(device->CreateComputePipelineState(&foreignPso, IID_PPV_ARGS(&rejectedPso)),
                           E_INVALIDARG, "compute PSO foreign root ownership");
    } else {
      failures += fail("create foreign root signature", hr);
    }
  }

  std::puts("OWNED_D3D12_CORE_CONTRACTS_ROOT_VALIDATION_PASS");

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
