#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_EXPANDED_FORMATS_FAIL step=%s hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool value, const char *step) { if (!value) fail(step); }
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{}; p.Type = type; p.CreationNodeMask = p.VisibleNodeMask = 1; return p;
}
D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
  d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
D3D12_RESOURCE_DESC texture(DXGI_FORMAT format, D3D12_RESOURCE_DIMENSION dimension,
                             UINT width, UINT height, UINT16 depthOrArray, UINT16 mipLevels) {
  D3D12_RESOURCE_DESC d{}; d.Dimension = dimension; d.Width = width; d.Height = height;
  d.DepthOrArraySize = depthOrArray; d.MipLevels = mipLevels; d.Format = format;
  d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; return d;
}
ComPtr<ID3D12Resource> make(ID3D12Device *device, D3D12_HEAP_TYPE type,
                            const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state) {
  ComPtr<ID3D12Resource> result; const auto props = heap(type);
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state,
                                        nullptr, IID_PPV_ARGS(&result)), "create-resource");
  return result;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before; b.Transition.StateAfter = after; list->ResourceBarrier(1, &b);
}
struct Context {
  ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence; HANDLE event = nullptr; UINT64 value = 0;
  Context() {
    const auto module = GetModuleHandleW(L"d3d12.dll");
    require(module != nullptr, "backend-module");
    using BackendId = const char *(WINAPI *)();
    const auto id = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    require(id && std::strcmp(id(), "owned-graphics-d3d12") == 0, "backend-id");
    std::puts("OWNED_D3D12_EXPANDED_FORMATS_BACKEND id=owned-graphics-d3d12");
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event");
  }
  ~Context() { if (event) CloseHandle(event); }
  void submit() {
    check(list->Close(), "close"); ID3D12CommandList *commands[] = {list.Get()};
    queue->ExecuteCommandLists(1, commands); check(queue->Signal(fence.Get(), ++value), "signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "completion-event");
      require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "completion-wait");
    }
    check(allocator->Reset(), "allocator-reset"); check(list->Reset(allocator.Get(), nullptr), "list-reset");
  }
};

void checkCase(Context &c, DXGI_FORMAT format, D3D12_RESOURCE_DIMENSION dimension,
               UINT width, UINT height, UINT16 depthOrArray, UINT16 mips) {
  const bool packed = format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{}; support.Format = format;
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "format-query");
  const auto has = [&](D3D12_FORMAT_SUPPORT1 bit) {
    return (static_cast<UINT>(support.Support1) & static_cast<UINT>(bit)) != 0;
  };
  require(has(D3D12_FORMAT_SUPPORT1_TEXTURE1D) && has(D3D12_FORMAT_SUPPORT1_TEXTURE2D) &&
          has(D3D12_FORMAT_SUPPORT1_TEXTURE3D) && has(D3D12_FORMAT_SUPPORT1_TEXTURECUBE) &&
          has(D3D12_FORMAT_SUPPORT1_SHADER_LOAD) && has(D3D12_FORMAT_SUPPORT1_MIP), "required-format-bits");
  require(has(D3D12_FORMAT_SUPPORT1_BUFFER) == !packed, "typed-buffer-policy");
  require(!has(D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW), "no-typed-uav");
  if (packed) {
    require(has(D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) && has(D3D12_FORMAT_SUPPORT1_SHADER_GATHER) &&
            !has(D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
            !has(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD), "packed-access-policy");
  } else {
    require(has(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD), "rgb32-msload");
    if (format == DXGI_FORMAT_R32G32B32_FLOAT)
      require(has(D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE), "rgb32-float-resolve");
  }
  const auto desc = texture(format, dimension, width, height, depthOrArray, mips);
  const UINT subresources = mips * (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : depthOrArray);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subresources);
  std::vector<UINT> rows(subresources); std::vector<UINT64> rowBytes(subresources);
  UINT64 total = 0; c.device->GetCopyableFootprints(&desc, 0, subresources, 0,
                                                       fp.data(), rows.data(), rowBytes.data(), &total);
  require(total != UINT64_MAX, "footprint-valid");
  auto image = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto source = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(total + 512), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto result = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total + 512), D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *data = nullptr; check(source->Map(0, nullptr, reinterpret_cast<void **>(&data)), "source-map");
  std::memset(data, 0xa5, static_cast<size_t>(total + 512));
  uint8_t *guard = nullptr; check(result->Map(0, nullptr, reinterpret_cast<void **>(&guard)), "result-guard-map");
  std::memset(guard, 0xcd, static_cast<size_t>(total + 512)); result->Unmap(0, nullptr);
  for (UINT s = 0; s < subresources; ++s) {
    const UINT mip = s % mips;
    const UINT logicalWidth = std::max(1u, width >> mip);
    const UINT logicalHeight = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : std::max(1u, height >> mip);
    const UINT logicalDepth = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max(1u, UINT(depthOrArray) >> mip) : 1u;
    const UINT expectedRow = packed ? ((logicalWidth + 1) / 2) * 4 : logicalWidth * 12;
    require(rowBytes[s] == expectedRow && rows[s] == logicalHeight, "footprint-dxgi-pitch");
    for (UINT z = 0; z < logicalDepth; ++z)
      for (UINT y = 0; y < logicalHeight; ++y) {
        uint8_t *row = data + fp[s].Offset + (UINT64(z) * logicalHeight + y) * fp[s].Footprint.RowPitch;
        for (UINT x = 0; x < expectedRow; ++x) row[x] = static_cast<uint8_t>((s * 29 + z * 17 + y * 11 + x * 37) & 255);
      }
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = image.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = s;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = source.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[s];
    c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  source->Unmap(0, nullptr);
  transition(c.list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT s = 0; s < subresources; ++s) {
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = image.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = s;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = result.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp[s];
    c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  c.submit();
  const uint8_t *input = nullptr, *output = nullptr;
  check(source->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&input))), "source-remap");
  check(result->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&output))), "result-map");
  for (UINT s = 0; s < subresources; ++s) {
    const UINT mip = s % mips;
    const UINT logicalWidth = std::max(1u, width >> mip);
    const UINT logicalHeight = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : std::max(1u, height >> mip);
    const UINT logicalDepth = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max(1u, UINT(depthOrArray) >> mip) : 1u;
    const UINT bytes = packed ? ((logicalWidth + 1) / 2) * 4 : logicalWidth * 12;
    for (UINT z = 0; z < logicalDepth; ++z)
      for (UINT y = 0; y < logicalHeight; ++y) {
        const UINT64 offset = fp[s].Offset + (UINT64(z) * logicalHeight + y) * fp[s].Footprint.RowPitch;
        // The second G of a one-pixel packed tail lies outside the logical mip.
        for (UINT x = 0; x < bytes; ++x) {
          if (packed && (logicalWidth & 1) && x == bytes -
              (format == DXGI_FORMAT_G8R8_G8B8_UNORM ? 2u : 1u))
            continue; // The second pixel's G lies outside this tail mip.
          require(input[offset + x] == output[offset + x], "roundtrip-bits");
        }
        for (UINT x = bytes; x < fp[s].Footprint.RowPitch; ++x)
          require(output[offset + x] == 0xcd, "row-padding-guard");
      }
  }
  result->Unmap(0, nullptr); source->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_CASE format=%u dim=%u width=%u layers=%u mips=%u\n",
              unsigned(format), unsigned(dimension), width, unsigned(depthOrArray), unsigned(mips));
}

void checkTypedBufferLoad(Context &c, DXGI_FORMAT format, const char *hlslType, bool remap = false) {
  constexpr UINT count = 8;
  auto source = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer((count + 2) * 12),
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  auto targetDesc = buffer((count + 2) * 16);
  targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto target = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, targetDesc,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto result = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer((count + 2) * 16),
                     D3D12_RESOURCE_STATE_COPY_DEST);
  uint32_t *input = nullptr;
  check(source->Map(0, nullptr, reinterpret_cast<void **>(&input)), "typed-map");
  for (UINT i = 0; i < (count + 2) * 3; ++i) {
    if (format == DXGI_FORMAT_R32G32B32_FLOAT) {
      const float value = float(i) * 0.25f + 0.5f;
      std::memcpy(&input[i], &value, sizeof(value));
    } else {
      input[i] = 0x12340000u + i * 0x010203u;
    }
  }
  source->Unmap(0, nullptr);

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)), "typed-descriptors");
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format;
  view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  view.Shader4ComponentMapping = remap ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
      D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
      D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
      D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
      D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Buffer.FirstElement = 2;
  view.Buffer.NumElements = count;
  c.device->CreateShaderResourceView(source.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  parameters[0].DescriptorTable.pDescriptorRanges = &range;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = parameters;
  ComPtr<ID3DBlob> rootCode, errors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &rootCode, &errors), "typed-serialize-root");
  ComPtr<ID3D12RootSignature> root;
  check(c.device->CreateRootSignature(0, rootCode->GetBufferPointer(), rootCode->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "typed-root");
  char hlsl[512];
  std::snprintf(hlsl, sizeof(hlsl),
                "Buffer<%s4> src:register(t0); RWByteAddressBuffer dst:register(u0); "
                "[numthreads(16,1,1)] void main(uint3 id:SV_DispatchThreadID) { "
                "if (id.x < 10) { uint4 v = asuint(src.Load(id.x)); dst.Store4(id.x*16,v); } }", hlslType);
  ComPtr<ID3DBlob> shader;
  HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
  if (FAILED(hr) && errors)
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  check(hr, "typed-compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root.Get();
  pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "typed-pso");
  c.list->SetDescriptorHeaps(1, descriptors.GetAddressOf());
  c.list->SetComputeRootSignature(root.Get());
  c.list->SetPipelineState(pso.Get());
  c.list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
  c.list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
  c.list->Dispatch(1, 1, 1);
  transition(c.list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->CopyBufferRegion(result.Get(), 0, target.Get(), 0, (count + 2) * 16);
  c.submit();

  check(source->Map(0, nullptr, reinterpret_cast<void **>(&input)), "typed-source-remap");
  const uint32_t *output = nullptr;
  check(result->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint32_t **>(&output))), "typed-readback");
  for (UINT i = 0; i < count + 2; ++i)
    for (UINT channel = 0; channel < 4; ++channel) {
      const uint32_t one = format == DXGI_FORMAT_R32G32B32_FLOAT ? 0x3f800000u : 1u;
      const uint32_t expected = i >= count ? 0u : remap ?
          (channel == 0 || channel == 3 ? one : channel == 1 ? input[(i + 2) * 3 + 2] : 0u) :
          channel == 3 ? one : input[(i + 2) * 3 + channel];
      require(output[i * 4 + channel] == expected, "typed-buffer-load");
    }
  result->Unmap(0, nullptr);
  source->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_TYPED_BUFFER format=%u remap=%u\n",
              unsigned(format), unsigned(remap));
}

void checkTextureLoad(Context &c, DXGI_FORMAT format, const char *hlslType,
                      DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN,
                      D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
  constexpr UINT width = 4;
  const UINT height = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 2;
  const UINT count = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? width * height : width;
  const UINT subresource = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : 3;
  const bool packed = format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM;
  const auto desc = texture(resourceFormat == DXGI_FORMAT_UNKNOWN ? format : resourceFormat,
                            dimension, 8, dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 4,
                            dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 4 : 2, 2);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  c.device->GetCopyableFootprints(&desc, subresource, 1, 0, &fp, &rows, &rowBytes, &total);
  require(rows == height && rowBytes == (packed ? 8u : width * 12u), "texture-load-footprint");
  auto image = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(total + 512),
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *input = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&input)), "texture-load-map");
  std::memset(input, 0, static_cast<size_t>(total + 512));
  for (UINT z = 0; z < fp.Footprint.Depth; ++z)
  for (UINT y = 0; y < height; ++y) {
    auto *row = input + fp.Offset + (UINT64(z) * height + y) * fp.Footprint.RowPitch;
    if (packed) {
      for (UINT pair = 0; pair < width / 2; ++pair) {
        const UINT x = pair * 2;
        const uint8_t red = uint8_t(20 + x * 17 + y * 7);
        const uint8_t blue = uint8_t(30 + x * 13 + y * 11);
        const uint8_t g0 = uint8_t(40 + x * 9 + y * 5);
        const uint8_t g1 = uint8_t(g0 + 9);
        if (format == DXGI_FORMAT_R8G8_B8G8_UNORM) {
          row[pair * 4 + 0] = red; row[pair * 4 + 1] = g0;
          row[pair * 4 + 2] = blue; row[pair * 4 + 3] = g1;
        } else {
          row[pair * 4 + 0] = g0; row[pair * 4 + 1] = red;
          row[pair * 4 + 2] = g1; row[pair * 4 + 3] = blue;
        }
      }
    } else {
      for (UINT x = 0; x < width * 3; ++x) {
        uint32_t bits = 0x12003000u + x * 119u + y * 67u + z * 41u;
        if (format == DXGI_FORMAT_R32G32B32_FLOAT) {
          const float value = float(x + 1) * .125f + float(y + z * 2);
          std::memcpy(&bits, &value, sizeof(bits));
        }
        std::memcpy(row + x * 4, &bits, sizeof(bits));
      }
    }
  }
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = image.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = subresource;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(c.list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  auto targetDesc = buffer(count * 16);
  targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto target = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, targetDesc,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto result = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(count * 16),
                     D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)), "texture-load-heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format;
  view.ViewDimension = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY :
                       dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_SRV_DIMENSION_TEXTURE3D :
                       D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
    view.Texture1DArray.MipLevels = 2; view.Texture1DArray.ArraySize = 2;
  } else if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    view.Texture3D.MipLevels = 2;
  } else {
    view.Texture2DArray.MipLevels = 2; view.Texture2DArray.ArraySize = 2;
  }
  c.device->CreateShaderResourceView(image.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 2; rootDesc.pParameters = params;
  ComPtr<ID3DBlob> rootCode, errors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &rootCode, &errors), "texture-load-serialize");
  ComPtr<ID3D12RootSignature> root;
  check(c.device->CreateRootSignature(0, rootCode->GetBufferPointer(), rootCode->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "texture-load-root");
  char hlsl[512];
  const char *shaderSource = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ?
      "Texture1DArray<%s4> src:register(t0); RWByteAddressBuffer dst:register(u0); "
      "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID) { "
      "dst.Store4(id.x*16,asuint(src.Load(int3(id.x,1,1)))); }" :
      dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ?
      "Texture3D<%s4> src:register(t0); RWByteAddressBuffer dst:register(u0); "
      "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID) { "
      "dst.Store4(id.x*16,asuint(src.Load(int4(id.x,1,1,1)))); }" :
      "Texture2DArray<%s4> src:register(t0); RWByteAddressBuffer dst:register(u0); "
      "[numthreads(8,1,1)] void main(uint3 id:SV_DispatchThreadID) { "
      "dst.Store4(id.x*16,asuint(src.Load(int4(id.x%%4,id.x/4,1,1)))); }";
  std::snprintf(hlsl, sizeof(hlsl), shaderSource, hlslType);
  ComPtr<ID3DBlob> shader;
  HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
  if (FAILED(hr) && errors)
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  check(hr, "texture-load-compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root.Get();
  pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "texture-load-pso");
  c.list->SetDescriptorHeaps(1, descriptors.GetAddressOf());
  c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
  c.list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
  c.list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
  c.list->Dispatch(1, 1, 1);
  transition(c.list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->CopyBufferRegion(result.Get(), 0, target.Get(), 0, count * 16);
  c.submit();

  const uint32_t *output = nullptr;
  check(result->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint32_t **>(&output))), "texture-load-result");
  for (UINT y = 0; y < count / width; ++y) {
    const UINT sourceY = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : y;
    const UINT sourceZ = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : 0;
    const uint8_t *row = input + fp.Offset + (UINT64(sourceZ) * height + sourceY) * fp.Footprint.RowPitch;
    for (UINT x = 0; x < width; ++x)
      for (UINT channel = 0; channel < 4; ++channel) {
        uint32_t expected = 0;
        if (packed) {
          const uint8_t *pair = row + (x / 2) * 4;
          const bool rgbg = format == DXGI_FORMAT_R8G8_B8G8_UNORM;
          const uint8_t value = channel == 0 ? pair[rgbg ? 0 : 1] :
              channel == 1 ? pair[rgbg ? (x & 1 ? 3 : 1) : (x & 1 ? 2 : 0)] :
              channel == 2 ? pair[rgbg ? 2 : 3] : 255;
          const float normalized = float(value) / 255.0f;
          std::memcpy(&expected, &normalized, sizeof(expected));
        } else if (channel == 3) {
          expected = format == DXGI_FORMAT_R32G32B32_FLOAT ? 0x3f800000u : 1u;
        } else {
          std::memcpy(&expected, row + (x * 3 + channel) * 4, sizeof(expected));
        }
        if (packed) {
          float actual = 0, expectedFloat = 0;
          std::memcpy(&actual, &output[(y * width + x) * 4 + channel], sizeof(actual));
          std::memcpy(&expectedFloat, &expected, sizeof(expectedFloat));
          require(actual >= expectedFloat - 0.00001f && actual <= expectedFloat + 0.00001f,
                  "texture-load-packed-channel");
        } else {
          require(output[(y * width + x) * 4 + channel] == expected, "texture-load-bits");
        }
      }
  }
  result->Unmap(0, nullptr); upload->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_TEXTURE_LOAD format=%u dim=%u\n",
              unsigned(format), unsigned(dimension));
}

void checkMultisampleLoadAndResolve(Context &c, DXGI_FORMAT format, const char *hlslType) {
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
  quality.Format = format; quality.SampleCount = 4;
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                      &quality, sizeof(quality)), "ms-quality-query");
  require(quality.NumQualityLevels > 0, "ms-quality-advertised");
  auto multiDesc = texture(format, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 1, 1, 1);
  multiDesc.SampleDesc.Count = 4;
  multiDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto multi = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, multiDesc, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtv;
  check(c.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtv)), "ms-rtv-heap");
  D3D12_RENDER_TARGET_VIEW_DESC renderView{};
  renderView.Format = format;
  renderView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
  c.device->CreateRenderTargetView(multi.Get(), &renderView, rtv->GetCPUDescriptorHandleForHeapStart());
  const FLOAT clear[4] = {1, 2, 3, 0.25f};
  c.list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
  transition(c.list.Get(), multi.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)), "ms-srv-heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  c.device->CreateShaderResourceView(multi.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());
  auto targetDesc = buffer(4 * 16);
  targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto target = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, targetDesc,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto read = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(4 * 16),
                   D3D12_RESOURCE_STATE_COPY_DEST);

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 2; rootDesc.pParameters = params;
  ComPtr<ID3DBlob> rootCode, errors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &rootCode, &errors), "ms-serialize-root");
  ComPtr<ID3D12RootSignature> root;
  check(c.device->CreateRootSignature(0, rootCode->GetBufferPointer(), rootCode->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "ms-root");
  char hlsl[512];
  std::snprintf(hlsl, sizeof(hlsl),
                "Texture2DMS<%s4> src:register(t0); RWByteAddressBuffer dst:register(u0); "
                "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID) { "
                "uint4 v=asuint(src.Load(int2(0,0),id.x)); dst.Store4(id.x*16,v); }", hlslType);
  ComPtr<ID3DBlob> shader;
  HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
  if (FAILED(hr) && errors)
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  check(hr, "ms-compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root.Get();
  pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "ms-pso");
  c.list->SetDescriptorHeaps(1, descriptors.GetAddressOf());
  c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
  c.list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
  c.list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
  c.list->Dispatch(1, 1, 1);
  transition(c.list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->CopyBufferRegion(read.Get(), 0, target.Get(), 0, 4 * 16);

  ComPtr<ID3D12Resource> resolved, resolvedRead;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  if (format == DXGI_FORMAT_R32G32B32_FLOAT) {
    auto singleDesc = texture(format, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 1, 1, 1);
    resolved = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, singleDesc,
                    D3D12_RESOURCE_STATE_RESOLVE_DEST);
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    c.device->GetCopyableFootprints(&singleDesc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    require(rows == 1 && rowBytes == 12, "ms-resolve-footprint");
    resolvedRead = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total + 512),
                        D3D12_RESOURCE_STATE_COPY_DEST);
    transition(c.list.Get(), multi.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    c.list->ResolveSubresource(resolved.Get(), 0, multi.Get(), 0, format);
    transition(c.list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resolved.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = resolvedRead.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  c.submit();

  const uint32_t *bits = nullptr;
  check(read->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint32_t **>(&bits))), "ms-load-readback");
  for (UINT sample = 0; sample < 4; ++sample)
    for (UINT channel = 0; channel < 4; ++channel) {
      const uint32_t expected = format == DXGI_FORMAT_R32G32B32_FLOAT ?
          (channel == 0 ? 0x3f800000u : channel == 1 ? 0x40000000u :
           channel == 2 ? 0x40400000u : 0x3f800000u) :
          channel == 3 ? 1u : channel + 1;
      require(bits[sample * 4 + channel] == expected, "ms-load-sample-bits");
    }
  read->Unmap(0, nullptr);
  if (resolvedRead) {
    const uint8_t *bytes = nullptr;
    check(resolvedRead->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&bytes))),
          "ms-resolve-readback");
    const uint32_t *resolvedBits = reinterpret_cast<const uint32_t *>(bytes + fp.Offset);
    require(resolvedBits[0] == 0x3f800000u && resolvedBits[1] == 0x40000000u &&
            resolvedBits[2] == 0x40400000u, "rgb32-float-resolve-bits");
    resolvedRead->Unmap(0, nullptr);
  }
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_MSLOAD format=%u samples=4\n", unsigned(format));
}

void checkPackedSampleGather(Context &c, DXGI_FORMAT format) {
  auto desc = texture(format, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 4, 1, 1);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0; UINT64 rowBytes = 0, total = 0;
  c.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  require(rows == 4 && rowBytes == 8, "packed-filter-footprint");
  auto image = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(total + 512),
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *data = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "packed-filter-map");
  std::memset(data, 0, static_cast<size_t>(total + 512));
  for (UINT y = 0; y < 4; ++y)
    for (UINT pair = 0; pair < 2; ++pair) {
      uint8_t *p = data + fp.Offset + UINT64(y) * fp.Footprint.RowPitch + pair * 4;
      if (format == DXGI_FORMAT_R8G8_B8G8_UNORM) {
        p[0] = 77; p[1] = 20; p[2] = 60; p[3] = 40;
      } else {
        p[0] = 20; p[1] = 77; p[2] = 40; p[3] = 60;
      }
    }
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = image.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(c.list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = 1; srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(c.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap)), "packed-filter-srv-heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Texture2D.MipLevels = 1; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  c.device->CreateShaderResourceView(image.Get(), &view, srvHeap->GetCPUDescriptorHandleForHeapStart());
  D3D12_DESCRIPTOR_HEAP_DESC samplerDesc{};
  samplerDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  samplerDesc.NumDescriptors = 1; samplerDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> samplerHeap;
  check(c.device->CreateDescriptorHeap(&samplerDesc, IID_PPV_ARGS(&samplerHeap)), "packed-filter-sampler-heap");
  D3D12_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  c.device->CreateSampler(&sampler, samplerHeap->GetCPUDescriptorHandleForHeapStart());
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  ranges[0].NumDescriptors = ranges[1].NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[3]{};
  for (UINT i = 0; i < 2; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i].DescriptorTable.NumDescriptorRanges = 1;
    params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
  }
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 3; rootDesc.pParameters = params;
  ComPtr<ID3DBlob> rootCode, errors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &rootCode, &errors), "packed-filter-serialize");
  ComPtr<ID3D12RootSignature> root;
  check(c.device->CreateRootSignature(0, rootCode->GetBufferPointer(), rootCode->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "packed-filter-root");
  const char *hlsl = "Texture2D<float4> src:register(t0); SamplerState samp:register(s0); "
      "RWByteAddressBuffer dst:register(u0); [numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) { "
      "float4 a=src.SampleLevel(samp,float2(0.375,0.125),0); "
      "float4 b=src.GatherRed(samp,float2(0.375,0.125)); "
      "dst.Store4(0,asuint(a)); dst.Store4(16,asuint(b)); }";
  ComPtr<ID3DBlob> shader;
  HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
  if (FAILED(hr) && errors)
    std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
  check(hr, "packed-filter-compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = root.Get(); pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(c.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "packed-filter-pso");
  auto targetDesc = buffer(32); targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto target = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, targetDesc,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(32),
                       D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12DescriptorHeap *heaps[] = {srvHeap.Get(), samplerHeap.Get()};
  c.list->SetDescriptorHeaps(2, heaps);
  c.list->SetComputeRootSignature(root.Get()); c.list->SetPipelineState(pso.Get());
  c.list->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
  c.list->SetComputeRootDescriptorTable(1, samplerHeap->GetGPUDescriptorHandleForHeapStart());
  c.list->SetComputeRootUnorderedAccessView(2, target->GetGPUVirtualAddress());
  c.list->Dispatch(1, 1, 1);
  transition(c.list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, 32);
  c.submit();
  const float *output = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(const_cast<float **>(&output))),
        "packed-filter-result");
  const float expected[] = {77.f / 255.f, 40.f / 255.f, 60.f / 255.f, 1.f};
  for (UINT channel = 0; channel < 8; ++channel) {
    const float value = channel < 4 ? expected[channel] : expected[0];
    require(output[channel] >= value - 0.00001f && output[channel] <= value + 0.00001f,
            "packed-sample-gather-channel");
  }
  readback->Unmap(0, nullptr); upload->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_FILTER format=%u\n", unsigned(format));
}

void checkPredicatedConversion(Context &c, DXGI_FORMAT format) {
  const bool packed = format == DXGI_FORMAT_R8G8_B8G8_UNORM;
  const auto desc = texture(format, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 1, 1, 1);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0; UINT64 rowBytes = 0, total = 0;
  c.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  require(rows == 1 && rowBytes == (packed ? 8u : 48u), "predicate-footprint");
  auto zeroSource = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(total + 512),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  auto source = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(total + 512),
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  auto falseTexture = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto trueTexture = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto falseRead = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total + 512),
                        D3D12_RESOURCE_STATE_COPY_DEST);
  auto trueRead = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total + 512),
                       D3D12_RESOURCE_STATE_COPY_DEST);
  auto suppressedRead = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total + 512),
                             D3D12_RESOURCE_STATE_COPY_DEST);
  auto placedRead = make(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(total + 512),
                         D3D12_RESOURCE_STATE_COPY_DEST);
  auto predicateSource = make(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(16),
                              D3D12_RESOURCE_STATE_GENERIC_READ);
  auto predicate = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, buffer(16),
                        D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *zero = nullptr, *payload = nullptr, *suppressed = nullptr, *placed = nullptr;
  check(zeroSource->Map(0, nullptr, reinterpret_cast<void **>(&zero)), "predicate-zero-map");
  check(source->Map(0, nullptr, reinterpret_cast<void **>(&payload)), "predicate-source-map");
  std::memset(zero, 0, static_cast<size_t>(total + 512));
  std::memset(payload, 0, static_cast<size_t>(total + 512));
  for (UINT byte = 0; byte < rowBytes; ++byte)
    payload[fp.Offset + byte] = uint8_t((byte * 17 + 23) & 255);
  zeroSource->Unmap(0, nullptr); source->Unmap(0, nullptr);
  check(suppressedRead->Map(0, nullptr, reinterpret_cast<void **>(&suppressed)), "predicate-guard-map");
  std::memset(suppressed, 0xcd, static_cast<size_t>(total + 512));
  suppressedRead->Unmap(0, nullptr);
  check(placedRead->Map(0, nullptr, reinterpret_cast<void **>(&placed)), "predicate-placed-map");
  std::memset(placed, 0xcd, static_cast<size_t>(total + 512));
  placedRead->Unmap(0, nullptr);
  UINT64 *predicates = nullptr;
  check(predicateSource->Map(0, nullptr, reinterpret_cast<void **>(&predicates)), "predicate-values-map");
  predicates[0] = 0; predicates[1] = 1;
  predicateSource->Unmap(0, nullptr);

  D3D12_TEXTURE_COPY_LOCATION src{};
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = zeroSource.Get();
  dst.pResource = falseTexture.Get(); c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  dst.pResource = trueTexture.Get(); c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  c.list->CopyBufferRegion(predicate.Get(), 0, predicateSource.Get(), 0, 16);
  transition(c.list.Get(), predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
  src.pResource = source.Get();
  c.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  dst.pResource = falseTexture.Get(); c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  c.list->SetPredication(predicate.Get(), 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
  dst.pResource = trueTexture.Get(); c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_TEXTURE_COPY_LOCATION placedDst{};
  placedDst.pResource = placedRead.Get(); placedDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  placedDst.PlacedFootprint = fp;
  c.list->CopyTextureRegion(&placedDst, 0, 0, 0, &src, nullptr);
  c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(c.list.Get(), falseTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(c.list.Get(), trueTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
  src.pResource = falseTexture.Get(); dst.pResource = falseRead.Get();
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  src.pResource = trueTexture.Get(); dst.pResource = trueRead.Get();
  c.list->SetPredication(predicate.Get(), 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
  c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  c.list->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  dst.pResource = suppressedRead.Get(); c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  c.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  c.submit();
  const uint8_t *actualFalse = nullptr, *actualTrue = nullptr, *actualSuppressed = nullptr, *actualPlaced = nullptr;
  check(falseRead->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&actualFalse))),
        "predicate-false-read");
  check(trueRead->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&actualTrue))),
        "predicate-true-read");
  check(suppressedRead->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&actualSuppressed))),
        "predicate-suppressed-read");
  check(placedRead->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&actualPlaced))),
        "predicate-placed-read");
  for (UINT byte = 0; byte < rowBytes; ++byte) {
    // The second green byte of an odd packed tail is not observable. Width 4 is all pairs.
    require(actualFalse[fp.Offset + byte] == 0, "predicate-false-unchanged");
    require(actualTrue[fp.Offset + byte] == uint8_t((byte * 17 + 23) & 255), "predicate-true-bits");
    require(actualSuppressed[fp.Offset + byte] == 0xcd, "predicate-readback-suppressed");
    require(actualPlaced[fp.Offset + byte] == uint8_t((byte * 17 + 23) & 255), "predicate-placed-bits");
  }
  for (UINT64 byte = fp.Offset + rowBytes; byte < fp.Offset + fp.Footprint.RowPitch; ++byte)
    require(actualPlaced[byte] == 0xcd, "predicate-placed-row-guard");
  falseRead->Unmap(0, nullptr); trueRead->Unmap(0, nullptr);
  suppressedRead->Unmap(0, nullptr); placedRead->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_PREDICATED format=%u\n", unsigned(format));
}

void checkCpuTextureTransfer(Context &c, DXGI_FORMAT format) {
  const bool packed = format == DXGI_FORMAT_R8G8_B8G8_UNORM || format == DXGI_FORMAT_G8R8_G8B8_UNORM;
  const auto desc = texture(format, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 4, 2, 4);
  auto image = make(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
  constexpr UINT subresource = 5; // array slice 1, mip 1: width 4, height 2.
  const UINT unit = packed ? 2u : 12u;
  const UINT wholeRow = 4 * unit;
  std::vector<uint8_t> initial(wholeRow * 2);
  check(image->WriteToSubresource(subresource, nullptr, initial.data(), wholeRow, wholeRow * 2),
        "cpu-initial-write");
  const D3D12_BOX box{2, 0, 0, 4, 2, 1};
  const UINT boxRow = 2 * unit, inputPitch = boxRow + 5, outputPitch = wholeRow + 7;
  std::vector<uint8_t> input(inputPitch * 2, 0xa5);
  for (UINT row = 0; row < 2; ++row)
    for (UINT byte = 0; byte < boxRow; ++byte)
      input[row * inputPitch + byte] = uint8_t((byte * 31 + row * 19 + 7) & 255);
  check(image->WriteToSubresource(subresource, &box, input.data(), inputPitch, inputPitch * 2),
        "cpu-box-write");
  std::vector<uint8_t> output(outputPitch * 2, 0xcd);
  check(image->ReadFromSubresource(output.data(), outputPitch, outputPitch * 2, subresource, nullptr),
        "cpu-full-read");
  for (UINT row = 0; row < 2; ++row) {
    for (UINT byte = 0; byte < wholeRow; ++byte) {
      const uint8_t expected = byte < 2 * unit ? 0 : input[row * inputPitch + byte - 2 * unit];
      require(output[row * outputPitch + byte] == expected, "cpu-partial-box-bits");
    }
    for (UINT byte = wholeRow; byte < outputPitch; ++byte)
      require(output[row * outputPitch + byte] == 0xcd, "cpu-row-guard");
  }
  if (packed) {
    constexpr UINT tail = 7; // array slice 1, mip 3: odd width 1, one packed pair.
    const uint8_t logical[4] = {31, 47, 59, 71};
    uint8_t recovered[4]{};
    check(image->WriteToSubresource(tail, nullptr, logical, 4, 4), "cpu-tail-write");
    check(image->ReadFromSubresource(recovered, 4, 4, tail, nullptr), "cpu-tail-read");
    for (UINT byte = 0; byte < 4; ++byte)
      if (byte != (format == DXGI_FORMAT_G8R8_G8B8_UNORM ? 2u : 3u))
        require(recovered[byte] == logical[byte], "cpu-tail-pixel");
  }
  std::printf("OWNED_D3D12_EXPANDED_FORMATS_CPU format=%u\n", unsigned(format));
}
}
int wmain() {
  Context c;
  for (auto format : {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT,
                      DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R8G8_B8G8_UNORM,
                      DXGI_FORMAT_G8R8_G8B8_UNORM}) {
    checkCase(c, format, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 8, 1, 2, 4);
    checkCase(c, format, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 8, 4, 6, 4);
    checkCase(c, format, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 8, 4, 4, 4);
  }
  checkTypedBufferLoad(c, DXGI_FORMAT_R32G32B32_FLOAT, "float");
  checkTypedBufferLoad(c, DXGI_FORMAT_R32G32B32_UINT, "uint");
  checkTypedBufferLoad(c, DXGI_FORMAT_R32G32B32_SINT, "int");
  checkTypedBufferLoad(c, DXGI_FORMAT_R32G32B32_FLOAT, "float", true);
  checkTypedBufferLoad(c, DXGI_FORMAT_R32G32B32_UINT, "uint", true);
  checkTypedBufferLoad(c, DXGI_FORMAT_R32G32B32_SINT, "int", true);
  checkTextureLoad(c, DXGI_FORMAT_R32G32B32_FLOAT, "float");
  checkTextureLoad(c, DXGI_FORMAT_R32G32B32_UINT, "uint");
  checkTextureLoad(c, DXGI_FORMAT_R32G32B32_SINT, "int");
  checkTextureLoad(c, DXGI_FORMAT_R32G32B32_FLOAT, "float", DXGI_FORMAT_R32G32B32_TYPELESS);
  checkTextureLoad(c, DXGI_FORMAT_R32G32B32_UINT, "uint", DXGI_FORMAT_R32G32B32_TYPELESS);
  checkTextureLoad(c, DXGI_FORMAT_R8G8_B8G8_UNORM, "float");
  checkTextureLoad(c, DXGI_FORMAT_G8R8_G8B8_UNORM, "float");
  for (auto dimension : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
    checkTextureLoad(c, DXGI_FORMAT_R32G32B32_FLOAT, "float", DXGI_FORMAT_UNKNOWN, dimension);
    checkTextureLoad(c, DXGI_FORMAT_R32G32B32_UINT, "uint", DXGI_FORMAT_UNKNOWN, dimension);
    checkTextureLoad(c, DXGI_FORMAT_R32G32B32_SINT, "int", DXGI_FORMAT_UNKNOWN, dimension);
  }
  checkMultisampleLoadAndResolve(c, DXGI_FORMAT_R32G32B32_FLOAT, "float");
  checkMultisampleLoadAndResolve(c, DXGI_FORMAT_R32G32B32_UINT, "uint");
  checkMultisampleLoadAndResolve(c, DXGI_FORMAT_R32G32B32_SINT, "int");
  checkPackedSampleGather(c, DXGI_FORMAT_R8G8_B8G8_UNORM);
  checkPackedSampleGather(c, DXGI_FORMAT_G8R8_G8B8_UNORM);
  checkPredicatedConversion(c, DXGI_FORMAT_R32G32B32_FLOAT);
  checkPredicatedConversion(c, DXGI_FORMAT_R8G8_B8G8_UNORM);
  for (auto format : {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT,
                      DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R8G8_B8G8_UNORM,
                      DXGI_FORMAT_G8R8_G8B8_UNORM})
    checkCpuTextureTransfer(c, format);
  std::puts("OWNED_D3D12_EXPANDED_FORMATS_PASS copy-roundtrip typed-buffer texture-load msload resolve filter predication cpu");
  return 0;
}
