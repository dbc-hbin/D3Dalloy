#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_COPY_RESOURCE_CASTS_FAIL step=%s hr=%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool condition, const char *step) { if (!condition) fail(step); }

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = p.VisibleNodeMask = 1;
  return p;
}
D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = d.DepthOrArraySize = d.MipLevels = 1;
  d.Format = DXGI_FORMAT_UNKNOWN;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
D3D12_RESOURCE_DESC texture(DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = d.Height = 4;
  d.DepthOrArraySize = d.MipLevels = 2;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  return d;
}
ComPtr<ID3D12Resource> resource(ID3D12Device *device, D3D12_HEAP_TYPE type,
                                const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state) {
  const auto properties = heap(type);
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
                                        nullptr, IID_PPV_ARGS(&result)), "CreateCommittedResource");
  return result;
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

struct Context {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event;
  UINT64 fenceValue = 0;

  Context() {
    HMODULE module = GetModuleHandleW(L"d3d12.dll");
    require(module != nullptr, "owned module");
    using BackendId = const char *(WINAPI *)();
    const auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned backend");
    std::printf("OWNED_D3D12_COPY_RESOURCE_CASTS_BACKEND id=%s\n", backend());
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "CreateEventW");
  }
  ~Context() { CloseHandle(event); }
  void submit() {
    check(list->Close(), "Close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fenceValue;
    check(queue->Signal(fence.Get(), value), "Signal");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
      require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "fence wait");
    }
    require(device->GetDeviceRemovedReason() == S_OK, "device removed");
    check(allocator->Reset(), "allocator Reset");
    check(list->Reset(allocator.Get(), nullptr), "list Reset");
  }
};

void caseCopy(Context &c, DXGI_FORMAT sourceFormat, DXGI_FORMAT destinationFormat, const char *name) {
  const auto srcDesc = texture(sourceFormat), dstDesc = texture(destinationFormat);
  auto src = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, srcDesc, D3D12_RESOURCE_STATE_COPY_DEST);
  auto dst = resource(c.device.Get(), D3D12_HEAP_TYPE_DEFAULT, dstDesc, D3D12_RESOURCE_STATE_COPY_DEST);
  constexpr UINT subresources = 4;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT srcFootprints[subresources]{}, dstFootprints[subresources]{};
  UINT64 srcBytes = 0, dstBytes = 0;
  c.device->GetCopyableFootprints(&srcDesc, 0, subresources, 0, srcFootprints, nullptr, nullptr, &srcBytes);
  c.device->GetCopyableFootprints(&dstDesc, 0, subresources, 0, dstFootprints, nullptr, nullptr, &dstBytes);
  require(srcBytes && srcBytes == dstBytes && srcBytes != UINT64_MAX, "compatible footprints");
  const UINT64 destinationUploadOffset = (srcBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                         ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);
  const auto &last = dstFootprints[subresources - 1];
  const UINT64 paddedEnd = last.Offset + UINT64(last.Footprint.RowPitch) * last.Footprint.Height;
  auto upload = resource(c.device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer(destinationUploadOffset + dstBytes),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = resource(c.device.Get(), D3D12_HEAP_TYPE_READBACK, buffer(paddedEnd + 64),
                           D3D12_RESOURCE_STATE_COPY_DEST);
  std::uint8_t *result = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&result)), "readback sentinel Map");
  std::memset(result, 0x5d, static_cast<size_t>(paddedEnd + 64));
  readback->Unmap(0, nullptr);
  std::uint8_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "upload Map");
  std::memset(mapped, 0, static_cast<size_t>(destinationUploadOffset + dstBytes));
  for (UINT subresource = 0; subresource < subresources; ++subresource) {
    const auto &f = srcFootprints[subresource];
    for (UINT y = 0; y < f.Footprint.Height; ++y)
      for (UINT x = 0; x < f.Footprint.Width; ++x) {
        auto *pixel = mapped + f.Offset + y * f.Footprint.RowPitch + x * 4;
        pixel[0] = static_cast<std::uint8_t>(0x13 + subresource * 17 + x);
        pixel[1] = static_cast<std::uint8_t>(0x59 + subresource * 13 + y);
        pixel[2] = static_cast<std::uint8_t>(0xa7 + subresource * 7 + x + y);
        pixel[3] = static_cast<std::uint8_t>(0xe1 + subresource);
      }
    const auto &destination = dstFootprints[subresource];
    for (UINT y = 0; y < destination.Footprint.Height; ++y)
      for (UINT x = 0; x < destination.Footprint.Width; ++x)
        std::memset(mapped + destinationUploadOffset + destination.Offset + y * destination.Footprint.RowPitch + x * 4,
                    0x7b, 4);
  }
  upload->Unmap(0, nullptr);

  for (UINT subresource = 0; subresource < subresources; ++subresource) {
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.pResource = src.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.PlacedFootprint = srcFootprints[subresource];
    to.SubresourceIndex = subresource;
    c.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    from.PlacedFootprint = dstFootprints[subresource];
    from.PlacedFootprint.Offset += destinationUploadOffset;
    to.pResource = dst.Get();
    c.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  transition(c.list.Get(), src.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.list->CopyResource(dst.Get(), src.Get());
  transition(c.list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT subresource = 0; subresource < subresources; ++subresource) {
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = dst.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.SubresourceIndex = subresource;
    to.PlacedFootprint = dstFootprints[subresource];
    c.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  c.submit();
  result = nullptr;
  check(readback->Map(0, nullptr, reinterpret_cast<void **>(&result)), "readback Map");
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "compare Map");
  for (UINT subresource = 0; subresource < subresources; ++subresource) {
    const auto &source = srcFootprints[subresource];
    const auto &destination = dstFootprints[subresource];
    for (UINT y = 0; y < destination.Footprint.Height; ++y)
      for (UINT x = 0; x < destination.Footprint.Width; ++x)
        require(std::memcmp(result + destination.Offset + y * destination.Footprint.RowPitch + x * 4,
                            mapped + source.Offset + y * source.Footprint.RowPitch + x * 4, 4) == 0,
                "CopyResource changed or omitted texel");
  }
  // Row padding is undefined; only bytes past the entire padded footprint
  // are a valid unchanged-memory sentinel for a texture-to-buffer copy.
  for (UINT64 offset = paddedEnd; offset < paddedEnd + 64; ++offset)
    require(result[offset] == 0x5d, "readback outside-footprint guard overwritten");
  upload->Unmap(0, nullptr);
  readback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_COPY_RESOURCE_CASTS_CASE_PASS %s mips=2 arrays=2 pixels=40\n", name);
}
} // namespace

int main() {
  Context c;
  caseCopy(c, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "linear-to-srgb");
  caseCopy(c, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, "srgb-to-linear");
  caseCopy(c, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT, "uint-to-float");
  std::puts("OWNED_D3D12_COPY_RESOURCE_CASTS_PASS");
}
