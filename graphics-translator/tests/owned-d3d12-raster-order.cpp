#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

enum class Case { RawDirect, StructuredDirect, RawTable, TypedTable, StructuredTable,
                  Unbounded, Mixed, Spaces, SampleDirect, SampleTable, LayerTable };

void check(HRESULT result, const char *step) {
  if (FAILED(result)) {
    std::fprintf(stderr, "RASTER_ORDER_FAIL step=%s hr=%08lx\n", step, static_cast<unsigned long>(result));
    ExitProcess(1);
  }
}

std::vector<unsigned char> readFile(const wchar_t *path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) ExitProcess(2);
  const DWORD size = GetFileSize(file, nullptr);
  std::vector<unsigned char> data(size);
  DWORD received = 0;
  if (!ReadFile(file, data.data(), size, &received, nullptr) || received != size) ExitProcess(2);
  CloseHandle(file);
  return data;
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CreationNodeMask = properties.VisibleNodeMask = 1;
  return properties;
}

D3D12_RESOURCE_DESC buffer(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}

ComPtr<ID3D12Resource> create(ID3D12Device *device, D3D12_HEAP_TYPE type,
                              const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state) {
  auto properties = heap(type);
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)), "CreateCommittedResource");
  return resource;
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}

int run(Case test, const wchar_t *vsPath, const wchar_t *psPath, bool cached = false) {
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12")) return 3;
  std::printf("OWNED_D3D12_RASTER_ORDER_BACKEND id=%s\n", backend());
  auto vs = readFile(vsPath), ps = readFile(psPath);
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[0].NumDescriptors = test == Case::Unbounded ? UINT_MAX : 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace = test == Case::Spaces ? 3 : 0;
  ranges[0].OffsetInDescriptorsFromTableStart = test == Case::Unbounded ? 0 : 8;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = test == Case::Spaces ? 0 : 1;
  ranges[1].RegisterSpace = test == Case::Spaces ? 7 : 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 12;
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  if (test == Case::RawDirect || test == Case::StructuredDirect || test == Case::SampleDirect) {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 0;
  } else {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = test == Case::Mixed || test == Case::Spaces ? 2 : 1;
    parameter.DescriptorTable.pDescriptorRanges = ranges;
  }
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 1;
  rootDesc.pParameters = &parameter;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> rootBlob, errors;
  HRESULT result = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &rootBlob, &errors);
  if (FAILED(result) && errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  check(result, "D3D12SerializeRootSignature");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "CreateRootSignature");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
  const bool sample = test == Case::SampleTable || test == Case::SampleDirect;
  const bool layered = test == Case::LayerTable;
  pipelineDesc.pRootSignature = root.Get();
  pipelineDesc.VS = {vs.data(), vs.size()};
  pipelineDesc.PS = {ps.data(), ps.size()};
  pipelineDesc.SampleMask = UINT_MAX;
  pipelineDesc.SampleDesc.Count = sample ? 4 : 1;
  if (layered) {
    pipelineDesc.NumRenderTargets = 1;
    pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
  ComPtr<ID3D12PipelineState> pipeline;
  check(device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)),
        "CreateGraphicsPipelineState");
  ComPtr<ID3DBlob> cachedBlob;
  if (cached) {
    check(pipeline->GetCachedBlob(&cachedBlob), "GetCachedBlob");
    pipeline.Reset();
    pipelineDesc.CachedPSO = {cachedBlob->GetBufferPointer(), cachedBlob->GetBufferSize()};
    check(device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)),
          "CreateGraphicsPipelineState(cached)");
  }

  const bool structured = test == Case::StructuredDirect || test == Case::StructuredTable;
  const bool multiple = test == Case::Unbounded;
  const bool withTexture = test == Case::Mixed || test == Case::Spaces;
  const UINT64 bytes = sample ? 32 : layered || structured ? 16 : 8;
  const auto targetDesc = buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  const auto stagingDesc = buffer(bytes);
  std::array<ComPtr<ID3D12Resource>, 2> target, upload, readback;
  for (unsigned i = 0; i < (multiple ? 2u : 1u); ++i) {
    target[i] = create(device.Get(), D3D12_HEAP_TYPE_DEFAULT, targetDesc, D3D12_RESOURCE_STATE_COPY_DEST);
    upload[i] = create(device.Get(), D3D12_HEAP_TYPE_UPLOAD, stagingDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback[i] = create(device.Get(), D3D12_HEAP_TYPE_READBACK, stagingDesc, D3D12_RESOURCE_STATE_COPY_DEST);
    void *mapped = nullptr;
    check(upload[i]->Map(0, nullptr, &mapped), "Map(upload)");
    std::memset(mapped, 0, static_cast<size_t>(bytes));
    if (structured) {
      static_cast<UINT *>(mapped)[1] = 0xC0DEu;
      static_cast<UINT *>(mapped)[3] = 0xBEEFu;
    }
    upload[i]->Unmap(0, nullptr);
  }

  ComPtr<ID3D12Resource> texture, textureReadback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureFootprint{};
  if (withTexture) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    texture = create(device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &textureFootprint, &rows, &rowBytes, &total);
    textureReadback = create(device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total),
                             D3D12_RESOURCE_STATE_COPY_DEST);
  }

  ComPtr<ID3D12Resource> layerTarget;
  ComPtr<ID3D12DescriptorHeap> layerViews;
  if (layered) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2;
    desc.Height = 1;
    desc.DepthOrArraySize = 2;
    desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    layerTarget = create(device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&layerViews)),
          "CreateDescriptorHeap(layer RTV)");
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = desc.Format;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.ArraySize = 2;
    device->CreateRenderTargetView(layerTarget.Get(), &view,
                                   layerViews->GetCPUDescriptorHandleForHeapStart());
  }

  ComPtr<ID3D12DescriptorHeap> descriptors;
  if (test != Case::RawDirect && test != Case::StructuredDirect && test != Case::SampleDirect) {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = withTexture ? 13 : multiple ? 2 : 9;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors)), "CreateDescriptorHeap");
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (unsigned i = 0; i < (multiple ? 2u : 1u); ++i) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
      view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      view.Buffer.NumElements = structured ? 2 : static_cast<UINT>(bytes / 4);
      if (structured) view.Buffer.StructureByteStride = 8;
      else if (test == Case::TypedTable || test == Case::Mixed || multiple) view.Format = DXGI_FORMAT_R32_UINT;
      else {
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      }
      auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += (multiple ? i : 8 + i) * increment;
      device->CreateUnorderedAccessView(target[i].Get(), nullptr, &view, handle);
    }
    if (withTexture) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
      view.Format = DXGI_FORMAT_R32_UINT;
      view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += 12 * increment;
      device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, handle);
    }
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  ComPtr<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "CreateCommandAllocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                  IID_PPV_ARGS(&list)), "CreateCommandList");
  for (unsigned i = 0; i < (multiple ? 2u : 1u); ++i) {
    list->CopyBufferRegion(target[i].Get(), 0, upload[i].Get(), 0, bytes);
    transition(list.Get(), target[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (withTexture) {
    const UINT clear[4]{};
    auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += 12 * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += 12 * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *heaps[]{descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->ClearUnorderedAccessViewUint(gpu, cpu, texture.Get(), clear, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = texture.Get();
    list->ResourceBarrier(1, &barrier);
  }
  D3D12_VIEWPORT viewport{0, 0, 2, 1, 0, 1};
  D3D12_RECT scissor{0, 0, 2, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  if (layered) {
    auto handle = layerViews->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
  }
  list->SetGraphicsRootSignature(root.Get());
  if (test == Case::RawDirect || test == Case::StructuredDirect || test == Case::SampleDirect)
    list->SetGraphicsRootUnorderedAccessView(0, target[0]->GetGPUVirtualAddress());
  else {
    ID3D12DescriptorHeap *heaps[]{descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
  }
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(layered ? 18 : 9, 1, 0, 0);
  for (unsigned i = 0; i < (multiple ? 2u : 1u); ++i) {
    transition(list.Get(), target[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback[i].Get(), 0, target[i].Get(), 0, bytes);
  }
  if (withTexture) {
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = textureReadback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = textureFootprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }
  check(list->Close(), "Close");
  ID3D12CommandList *lists[]{list.Get()};
  queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(queue->Signal(fence.Get(), 1), "Signal");
  check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
  const DWORD wait = WaitForSingleObject(event, 15000);
  CloseHandle(event);
  if (wait != WAIT_OBJECT_0) return 4;

  std::array<std::array<UINT, 8>, 2> values{};
  for (unsigned i = 0; i < (multiple ? 2u : 1u); ++i) {
    void *mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readback[i]->Map(0, &range, &mapped), "Map(readback)");
    std::memcpy(values[i].data(), mapped, static_cast<size_t>(bytes));
    readback[i]->Unmap(0, nullptr);
  }
  std::array<UINT, 2> texels{};
  if (withTexture) {
    void *mapped = nullptr;
    D3D12_RANGE range{0, textureFootprint.Footprint.RowPitch};
    check(textureReadback->Map(0, &range, &mapped), "Map(texture readback)");
    std::memcpy(texels.data(), static_cast<const unsigned char *>(mapped) + textureFootprint.Offset, 8);
    textureReadback->Unmap(0, nullptr);
  }
  bool valid = structured
      ? values[0][0] == 27 && values[0][1] == 0xC0DEu &&
        values[0][2] == 27 && values[0][3] == 0xBEEFu
      : multiple
          ? values[0][0] == 27 && values[0][1] == 0 &&
            values[1][0] == 0 && values[1][1] == 27
          : values[0][0] == 27 && values[0][1] == 27 &&
            (!withTexture || (texels[0] == 27 && texels[1] == 27));
  if (sample)
    for (unsigned value : values[0]) valid = valid && value == 27;
  if (layered)
    valid = values[0][0] == 27 && values[0][1] == 27 &&
            values[0][2] == 90 && values[0][3] == 90;
  std::printf("RASTER_ORDER_RESULT mode=%u buffer=%u,%u second=%u,%u texture=%u,%u expected=27 status=%s\n",
              static_cast<unsigned>(test), values[0][0], values[0][structured ? 2 : 1],
              values[1][0], values[1][1], texels[0], texels[1], valid ? "PASS" : "FAIL");
  if (sample)
    std::printf("RASTER_ORDER_SAMPLES values=%u,%u,%u,%u,%u,%u,%u,%u expected=27\n",
                values[0][0], values[0][1], values[0][2], values[0][3], values[0][4],
                values[0][5], values[0][6], values[0][7]);
  if (layered)
    std::printf("RASTER_ORDER_LAYERS values=%u,%u,%u,%u expected=27,27,90,90\n",
                values[0][0], values[0][1], values[0][2], values[0][3]);
  return valid ? 0 : 5;
}

} // namespace

int wmain(int argc, wchar_t **) {
  if (argc != 1) return 2;
  wchar_t executable[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (!length || length >= MAX_PATH) return 2;
  std::wstring directory(executable, executable + length);
  const auto separator = directory.find_last_of(L"\\/");
  if (separator == std::wstring::npos) return 2;
  directory.resize(separator + 1);
  struct Test {
    Case mode;
    const char *label;
    const wchar_t *pixel;
  };
  const Test tests[]{
      {Case::RawDirect, "raw-direct-dxil", L"raster-order-rov-raw.dxil"},
      {Case::StructuredDirect, "structured-direct-dxbc", L"raster-order-rov-structured.dxbc"},
      {Case::StructuredDirect, "structured-direct-dxil", L"raster-order-rov-structured.dxil"},
      {Case::RawTable, "raw-table-dxil", L"raster-order-rov-raw.dxil"},
      {Case::TypedTable, "typed-table-dxbc", L"raster-order-rov-typed.dxbc"},
      {Case::StructuredTable, "structured-table-dxbc", L"raster-order-rov-structured.dxbc"},
      {Case::StructuredTable, "structured-table-dxil", L"raster-order-rov-structured.dxil"},
      {Case::Unbounded, "unbounded-dxil", L"raster-order-rov-unbounded.dxil"},
      {Case::Mixed, "mixed-dxbc", L"raster-order-mixed.dxbc"},
      {Case::Mixed, "mixed-dxil", L"raster-order-mixed.dxil"},
      {Case::Spaces, "spaces-dxil", L"raster-order-rov-spaces.dxil"},
      {Case::LayerTable, "layer-table-dxil", L"raster-order-rov-layer.dxil"},
  };
  const auto vertex = directory + L"raster-order-vertex.dxil";
  for (const auto &test : tests) {
    const auto pixel = directory + test.pixel;
    const auto layeredVertex = directory + L"raster-order-layer-vertex.dxil";
    const int result = run(test.mode,
                           (test.mode == Case::LayerTable ? layeredVertex : vertex).c_str(),
                           pixel.c_str());
    std::printf("RASTER_ORDER_CASE %s status=%s\n", test.label, result ? "FAIL" : "PASS");
    if (result) return result;
  }
  const int cached = run(Case::RawTable, vertex.c_str(),
                         (directory + L"raster-order-rov-raw.dxil").c_str(), true);
  std::printf("RASTER_ORDER_CASE raw-table-dxil-cached status=%s\n", cached ? "FAIL" : "PASS");
  if (cached) return cached;
  const int sample = run(Case::SampleTable, vertex.c_str(),
                         (directory + L"raster-order-rov-sample.dxil").c_str());
  std::printf("RASTER_ORDER_CASE sample-table-4x-dxil status=%s\n", sample ? "FAIL" : "PASS");
  if (sample) return sample;
  const int directSample = run(Case::SampleDirect, vertex.c_str(),
                               (directory + L"raster-order-rov-sample.dxil").c_str());
  std::printf("RASTER_ORDER_CASE sample-direct-4x-dxil status=%s\n",
              directSample ? "FAIL" : "PASS");
  if (directSample) return directSample;
  std::puts("OWNED_D3D12_RASTER_ORDER_PASS");
  return 0;
}
