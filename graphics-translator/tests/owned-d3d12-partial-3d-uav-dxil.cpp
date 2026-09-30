#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_PARTIAL_3D_UAV_FAIL step=%s hr=%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool condition, const char *step) { if (!condition) fail(step); }

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CreationNodeMask = properties.VisibleNodeMask = 1;
  return properties;
}
D3D12_RESOURCE_DESC buffer(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}
void uavBarrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  list->ResourceBarrier(1, &barrier);
}
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "DXIL shader argument");
  const auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  require(backend && !std::strcmp(backend(), "owned-graphics-d3d12"), "backend");
  std::printf("OWNED_D3D12_PARTIAL_3D_UAV_BACKEND id=%s\n", backend());

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_RESOURCE_DESC textureDesc{};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  textureDesc.Width = 4;
  textureDesc.Height = 2;
  textureDesc.DepthOrArraySize = 4;
  textureDesc.MipLevels = 1;
  textureDesc.Format = DXGI_FORMAT_R32_UINT;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  const auto defaultHeap = heap(D3D12_HEAP_TYPE_DEFAULT);
  const auto uploadHeap = heap(D3D12_HEAP_TYPE_UPLOAD);
  const auto readbackHeap = heap(D3D12_HEAP_TYPE_READBACK);
  ComPtr<ID3D12Resource> texture, upload, readback, output, outputReadback;
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "texture");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
  require(rows == 2 && rowBytes == 16 && footprint.Footprint.Depth == 4, "footprint");
  auto desc = buffer(total);
  check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload");
  check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
  desc = buffer(7 * sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)), "output");
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&outputReadback)), "output readback");

  UINT *data = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "upload map");
  std::memset(data, 0, static_cast<size_t>(total));
  const UINT slicePitch = footprint.Footprint.RowPitch * rows;
  for (UINT z = 0; z < 4; ++z)
    for (UINT y = 0; y < 2; ++y)
      for (UINT x = 0; x < 4; ++x)
        data[(footprint.Offset + z * slicePitch + y * footprint.Footprint.RowPitch) / 4 + x] =
            0x1000 + z * 0x100 + y * 0x10 + x;
  upload->Unmap(0, nullptr);

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 1;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> gpuHeap, cpuHeap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gpuHeap)), "GPU descriptor heap");
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cpuHeap)), "CPU descriptor heap");
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = textureDesc.Format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
  uav.Texture3D.FirstWSlice = 1;
  uav.Texture3D.WSize = 2;
  const auto cpu = cpuHeap->GetCPUDescriptorHandleForHeapStart();
  const auto gpu = gpuHeap->GetGPUDescriptorHandleForHeapStart();
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, cpu);
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, gpuHeap->GetCPUDescriptorHandleForHeapStart());

  HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  require(file != INVALID_HANDLE_VALUE, "open DXIL");
  const DWORD shaderSize = GetFileSize(file, nullptr);
  require(shaderSize != INVALID_FILE_SIZE && shaderSize > 0, "DXIL size");
  std::vector<BYTE> shader(shaderSize);
  DWORD bytesRead = 0;
  require(ReadFile(file, shader.data(), shaderSize, &bytesRead, nullptr) && bytesRead == shaderSize, "read DXIL");
  CloseHandle(file);

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].Descriptor.ShaderRegister = 1;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> rootBytes, error;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &error), "root serialize");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(),
                                    IID_PPV_ARGS(&root)), "root signature");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root.Get();
  pipeline.CS = {shader.data(), shader.size()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "pipeline");

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
  check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");

  D3D12_TEXTURE_COPY_LOCATION source{}, destination{}, back{};
  source.pResource = upload.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint;
  destination.pResource = texture.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  back.pResource = readback.Get();
  back.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  back.PlacedFootprint = footprint;
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *active[] = {gpuHeap.Get()};
  list->SetDescriptorHeaps(1, active);
  const UINT clear[4] = {0x55, 0, 0, 0};
  list->ClearUnorderedAccessViewUint(gpu, cpu, texture.Get(), clear, 0, nullptr);
  uavBarrier(list.Get(), texture.Get());
  list->SetComputeRootSignature(root.Get());
  list->SetPipelineState(pso.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  uavBarrier(list.Get(), texture.Get());
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&back, 0, 0, 0, &destination, nullptr);
  list->CopyResource(outputReadback.Get(), output.Get());
  check(list->Close(), "close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  check(queue->Signal(fence.Get(), 1), "signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "event");
  check(fence->SetEventOnCompletion(1, event), "completion");
  require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "wait");
  CloseHandle(event);
  check(device->GetDeviceRemovedReason(), "device removed");

  UINT *voxels = nullptr, *result = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&voxels)), "map voxels");
  check(outputReadback->Map(0, nullptr, reinterpret_cast<void **>(&result)), "map output");
  constexpr UINT expected[4][4] = {
      {0x1000, 0x1001, 0x1002, 0x1003}, {101, 0x55, 0x55, 0x55},
      {202, 0x5c, 0x55, 0x55}, {0x1300, 0x1301, 0x1302, 0x1303}};
  for (UINT z = 0; z < 4; ++z)
    for (UINT y = 0; y < 2; ++y)
      for (UINT x = 0; x < 4; ++x) {
        const UINT actual = voxels[(footprint.Offset + z * slicePitch + y * footprint.Footprint.RowPitch) / 4 + x];
        const UINT want = y == 0 ? expected[z][x] : (z == 1 || z == 2 ? 0x55 : 0x1000 + z * 0x100 + y * 0x10 + x);
        if (actual != want) {
          std::fprintf(stderr, "OWNED_D3D12_PARTIAL_3D_UAV_FAIL voxel z=%u y=%u x=%u actual=%x expected=%x\n",
                       z, y, x, actual, want);
          ExitProcess(1);
        }
      }
  require(result[0] == 202 && result[1] == 0 && result[2] == 0x55 && result[3] == 0,
          "load/atomic outputs");
  require(result[4] == 4 && result[5] == 2 && result[6] == 2, "logical GetDimensions");
  std::printf("OWNED_D3D12_PARTIAL_3D_UAV_PASS inbounds_load=%u oob_load=%u "
              "atomic_before=%u oob_atomic_before=%u bounded_clear=1\n",
              result[0], result[1], result[2], result[3]);
  readback->Unmap(0, nullptr);
  outputReadback->Unmap(0, nullptr);
}
