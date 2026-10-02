// Public llvm-mingw D3D12 consumer: normal aggregate-return ABI on x64 and i686.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok, const char *step) {
  if (!ok) {
    std::fprintf(stderr, "RESOURCE_INTERFACES_FAIL step=%s\n", step);
    std::exit(1);
  }
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "RESOURCE_INTERFACES_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr);
    std::exit(1);
  }
}
D3D12_RESOURCE_DESC desc(bool texture) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = texture ? D3D12_RESOURCE_DIMENSION_TEXTURE2D : D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = texture ? 64 : 65536;
  d.Height = texture ? 64 : 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = texture ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
  d.SampleDesc.Count = 1;
  d.Layout = texture ? D3D12_TEXTURE_LAYOUT_UNKNOWN : D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
D3D12_RESOURCE_DESC1 extended(const D3D12_RESOURCE_DESC &d, D3D12_MIP_REGION region = {}) {
  return {d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels,
          d.Format, d.SampleDesc, d.Layout, d.Flags, region};
}
const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                               D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
const GUID private_id = {0x38a49888, 0x31a2, 0x4ecf, {0xa2, 0xda, 0xd1, 0x23, 0, 0, 0, 1}};

void identity(ID3D12DeviceChild *object, ID3D12Device *device) {
  ComPtr<IUnknown> base, other, owner, expected;
  check(object->QueryInterface(IID_PPV_ARGS(&base)), "IUnknown");
  ComPtr<ID3D12Object> o;
  check(object->QueryInterface(IID_PPV_ARGS(&o)), "Object");
  check(o->QueryInterface(IID_PPV_ARGS(&other)), "Object IUnknown");
  require(base.Get() == other.Get(), "canonical IUnknown");
  check(object->GetDevice(IID_PPV_ARGS(&owner)), "GetDevice");
  check(device->QueryInterface(IID_PPV_ARGS(&expected)), "device identity");
  require(owner.Get() == expected.Get(), "device ownership");
  UINT value = 0x917ac4e2, got = 0, bytes = sizeof(got);
  check(object->SetPrivateData(private_id, sizeof(value), &value), "SetPrivateData");
  check(o->GetPrivateData(private_id, &bytes, &got), "GetPrivateData through queried interface");
  require(bytes == sizeof(got) && got == value, "private data identity");
  ULONG n = object->AddRef();
  require(object->Release() == n - 1, "balanced refcount");
}
void resourceContract(ID3D12Resource *resource, ID3D12Device *device, const D3D12_RESOURCE_DESC &wanted,
                      D3D12_MIP_REGION region = {}) {
  ComPtr<ID3D12Resource1> r1;
  ComPtr<ID3D12Resource2> r2;
  check(resource->QueryInterface(IID_PPV_ARGS(&r1)), "QI Resource1");
  check(r1->QueryInterface(IID_PPV_ARGS(&r2)), "QI Resource2");
  identity(r2.Get(), device);
  auto d = r2->GetDesc1();
  auto old = resource->GetDesc();
  auto expected = extended(old, region);
  require(d.Dimension == expected.Dimension && d.Alignment == expected.Alignment && d.Width == expected.Width &&
          d.Height == expected.Height && d.DepthOrArraySize == expected.DepthOrArraySize &&
          d.MipLevels == expected.MipLevels && d.Format == expected.Format &&
          d.SampleDesc.Count == expected.SampleDesc.Count && d.SampleDesc.Quality == expected.SampleDesc.Quality &&
          d.Layout == expected.Layout && d.Flags == expected.Flags &&
          d.SamplerFeedbackMipRegion.Width == region.Width && d.SamplerFeedbackMipRegion.Height == region.Height &&
          d.SamplerFeedbackMipRegion.Depth == region.Depth, "GetDesc1 logical fields and exact region");
  require(d.Dimension == wanted.Dimension && d.Width == wanted.Width && d.Height == wanted.Height &&
          d.Format == wanted.Format && d.MipLevels == wanted.MipLevels && d.Alignment == 65536,
          "logical description not physical backing");
  void *session = reinterpret_cast<void *>(uintptr_t{1});
  require(r1->GetProtectedResourceSession(__uuidof(ID3D12ProtectedResourceSession), &session) == E_NOINTERFACE &&
          session == nullptr, "resource unprotected session");
  session = reinterpret_cast<void *>(uintptr_t{1});
  require(r2->GetProtectedResourceSession(__uuidof(IUnknown), &session) == E_NOINTERFACE && session == nullptr,
          "resource unprotected alternate IID");
  require(r2->GetProtectedResourceSession(__uuidof(IUnknown), nullptr) == E_POINTER, "resource null session output");
}
void heapContract(ID3D12Heap *heap, ID3D12Device *device) {
  ComPtr<ID3D12Heap1> h;
  check(heap->QueryInterface(IID_PPV_ARGS(&h)), "QI Heap1");
  identity(h.Get(), device);
  auto d = h->GetDesc();
  require(d.SizeInBytes >= 65536 && d.Alignment == 65536, "heap aggregate return");
  void *session = reinterpret_cast<void *>(uintptr_t{1});
  require(h->GetProtectedResourceSession(__uuidof(ID3D12ProtectedResourceSession), &session) == E_NOINTERFACE &&
          session == nullptr, "heap unprotected session");
  require(h->GetProtectedResourceSession(__uuidof(IUnknown), nullptr) == E_POINTER, "heap null session output");
}

struct Context {
  ComPtr<ID3D12Device10> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList7> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 serial = 0;
  Context() {
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    auto backend = reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
    require(backend && !std::strcmp(backend(), "owned-graphics-d3d12"), "owned backend");
    D3D12_COMMAND_QUEUE_DESC q{};
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(list->Close(), "initial close");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  }
  ComPtr<ID3D12Heap1> heap(D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE, bool version1 = false) {
    D3D12_HEAP_DESC d{4 * 65536, gpu, 65536, flags};
    ComPtr<ID3D12Heap1> h;
    check(version1 ? device->CreateHeap1(&d, nullptr, IID_PPV_ARGS(&h)) : device->CreateHeap(&d, IID_PPV_ARGS(&h)), "create heap requested Heap1");
    heapContract(h.Get(), device.Get());
    return h;
  }
  ComPtr<ID3D12Resource2> buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
    auto d = desc(false); d.Width = bytes;
    auto props = gpu; props.Type = type;
    ComPtr<ID3D12Resource2> r;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "aux buffer requested Resource2");
    return r;
  }
  void begin() {
    check(allocator->Reset(), "allocator reset");
    check(list->Reset(allocator.Get(), nullptr), "list reset");
  }
  void submit() {
    check(list->Close(), "close");
    ID3D12CommandList *lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++serial), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
    check(fence->SetEventOnCompletion(serial, event), "wait registration");
    require(WaitForSingleObject(event, 20000) == WAIT_OBJECT_0, "GPU completion");
    CloseHandle(event);
    check(device->GetDeviceRemovedReason(), "device intact");
  }
  void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &b);
  }
  void copyProof(ID3D12Resource2 *r, bool texture, bool reserved, ID3D12Heap *tiles = nullptr, bool enhanced = false) {
    auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, 65536, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, 65536, D3D12_RESOURCE_STATE_COPY_DEST);
    void *memory = nullptr;
    check(upload->Map(0, nullptr, &memory), "Map Resource2 upload");
    for (UINT i = 0; i < 16384; ++i) static_cast<UINT *>(memory)[i] = 0x93210badu ^ i;
    upload->Unmap(0, nullptr);
    if (reserved) {
      D3D12_TILED_RESOURCE_COORDINATE coord{};
      D3D12_TILE_REGION_SIZE size{1, FALSE, 0, 0, 0};
      D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
      UINT offset = 0, count = 1;
      queue->UpdateTileMappings(r, 1, &coord, &size, tiles, 1, &flags, &offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    begin();
    if (texture) {
      D3D12_TEXTURE_COPY_LOCATION source{}, target{}, rb{};
      source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      auto logical = r->GetDesc();
      device->GetCopyableFootprints(&logical, 0, 1, 0, &source.PlacedFootprint, nullptr, nullptr, nullptr);
      target.pResource = r; target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      rb = source; rb.pResource = readback.Get();
      list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
      if (enhanced) {
        D3D12_TEXTURE_BARRIER b{D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY,
                                D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_SOURCE, r, {},
                                D3D12_TEXTURE_BARRIER_FLAG_NONE};
        b.Subresources.IndexOrFirstMipLevel = UINT_MAX;
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
        group.pTextureBarriers = &b;
        list->Barrier(1, &group);
      } else {
        barrier(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      list->CopyTextureRegion(&rb, 0, 0, 0, &target, nullptr);
    } else {
      list->CopyBufferRegion(r, 0, upload.Get(), 0, 16384);
      barrier(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, r, 0, 16384);
    }
    submit();
    check(readback->Map(0, nullptr, &memory), "Map Resource2 readback");
    for (UINT i = 0; i < 4096; ++i) require(static_cast<UINT *>(memory)[i] == (0x93210badu ^ i), "exact GPU copy readback");
    readback->Unmap(0, nullptr);
  }
};
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Context c;
  // Starts with the old creation IID so the immutable baseline fails specifically at QI Resource1.
  auto first_desc = desc(false);
  ComPtr<ID3D12Resource> first;
  check(c.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &first_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&first)), "legacy create before QI");
  resourceContract(first.Get(), c.device.Get(), first_desc);
  ComPtr<ID3D12Resource2> retained;
  check(first.As(&retained), "retain Resource2");
  first.Reset();
  c.copyProof(retained.Get(), false, false);
  retained.Reset();
  ComPtr<ID3D12Resource1> direct1;
  check(c.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &first_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&direct1)), "create requested Resource1");
  resourceContract(direct1.Get(), c.device.Get(), first_desc);
  {
    auto d = desc(true);
    d.Format = DXGI_FORMAT_R32G32B32_FLOAT; // Metal backing uses four components, logical resource has three.
    d.MipLevels = 0;
    auto d1 = extended(d);
    ComPtr<ID3D12Resource2> r;
    check(c.device->CreateCommittedResource2(&gpu, D3D12_HEAP_FLAG_NONE, &d1, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, nullptr, IID_PPV_ARGS(&r)), "emulated storage full-mip Resource2");
    d.MipLevels = 7;
    resourceContract(r.Get(), c.device.Get(), d);
    c.copyProof(r.Get(), true, false);
    std::puts("RESOURCE_INTERFACES_CASE emulated_rgb32 normalized_mips=7 descriptor=logical gpu=exact");
  }
  for (bool texture : {false, true}) {
    for (UINT api = 0; api < 10; ++api) {
      auto d = desc(texture);
      bool reserved = api >= 7;
      if (reserved && texture) d.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
      auto d1 = extended(d);
      auto h = c.heap();
      ComPtr<ID3D12Resource2> r;
      HRESULT hr = E_FAIL;
      auto layout = texture ? D3D12_BARRIER_LAYOUT_COPY_DEST : D3D12_BARRIER_LAYOUT_UNDEFINED;
      switch (api) {
      case 0: hr = c.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)); break;
      case 1: hr = c.device->CreateCommittedResource1(&gpu, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, nullptr, IID_PPV_ARGS(&r)); break;
      case 2: hr = c.device->CreateCommittedResource2(&gpu, D3D12_HEAP_FLAG_NONE, &d1, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, nullptr, IID_PPV_ARGS(&r)); break;
      case 3: hr = c.device->CreateCommittedResource3(&gpu, D3D12_HEAP_FLAG_NONE, &d1, layout, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)); break;
      case 4: hr = c.device->CreatePlacedResource(h.Get(), 0, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)); break;
      case 5: hr = c.device->CreatePlacedResource1(h.Get(), 0, &d1, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)); break;
      case 6: hr = c.device->CreatePlacedResource2(h.Get(), 0, &d1, layout, nullptr, 0, nullptr, IID_PPV_ARGS(&r)); break;
      case 7: hr = c.device->CreateReservedResource(&d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)); break;
      case 8: hr = c.device->CreateReservedResource1(&d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, nullptr, IID_PPV_ARGS(&r)); break;
      case 9: hr = c.device->CreateReservedResource2(&d, layout, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)); break;
      }
      check(hr, "create requested Resource2 matrix");
      resourceContract(r.Get(), c.device.Get(), d);
      // Placed resources must retain the heap after the caller drops it.
      if (!reserved) h.Reset();
      c.copyProof(r.Get(), texture, reserved, h.Get(), api == 3 || api == 6 || api == 9);
      std::printf("RESOURCE_INTERFACES_CASE texture=%u api=%u descriptor=exact gpu=exact\n", texture, api);
    }
  }
  for (bool texture : {false, true}) {
    auto d = desc(texture);
    if (texture)
      d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    ComPtr<ID3D12Resource2> r, imported;
    check(c.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&r)), "shared resource");
    HANDLE share = nullptr;
    check(c.device->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &share), "shared resource handle");
    check(c.device->OpenSharedHandle(share, IID_PPV_ARGS(&imported)), "open requested Resource2");
    ComPtr<ID3D12Resource1> imported1;
    check(c.device->OpenSharedHandle(share, IID_PPV_ARGS(&imported1)), "open requested Resource1");
    resourceContract(imported1.Get(), c.device.Get(), d);
    imported1.Reset();
    CloseHandle(share); r.Reset();
    resourceContract(imported.Get(), c.device.Get(), d);
    c.copyProof(imported.Get(), texture, false);
    std::printf("RESOURCE_INTERFACES_CASE shared_texture=%u descriptor=exact gpu=exact\n", texture);
  }
  for (bool version1 : {false, true}) {
    auto h = c.heap(D3D12_HEAP_FLAG_SHARED, version1);
    HANDLE share = nullptr;
    check(c.device->CreateSharedHandle(h.Get(), nullptr, GENERIC_ALL, nullptr, &share), "heap handle");
    ComPtr<ID3D12Heap1> imported;
    check(c.device->OpenSharedHandle(share, IID_PPV_ARGS(&imported)), "open requested Heap1");
    CloseHandle(share); h.Reset();
    heapContract(imported.Get(), c.device.Get());
    auto d = desc(false);
    ComPtr<ID3D12Resource2> placed;
    check(c.device->CreatePlacedResource(imported.Get(), 0, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&placed)), "placed on imported Heap1");
    imported.Reset();
    c.copyProof(placed.Get(), false, false);
    std::printf("RESOURCE_INTERFACES_CASE shared_heap_api=%u lifetime=retained gpu=exact\n", version1);
  }
  for (bool mapping : {false, true}) {
    void *address = nullptr;
    HANDLE section = nullptr;
    ComPtr<ID3D12Heap1> h;
    if (mapping) {
      section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, nullptr);
      require(section != nullptr, "existing heap section");
      check(c.device->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&h)), "existing mapping requested Heap1");
      CloseHandle(section);
    } else {
      address = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      require(address != nullptr, "existing heap address");
      check(c.device->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&h)), "existing address requested Heap1");
    }
    heapContract(h.Get(), c.device.Get());
    auto d = desc(false);
    ComPtr<ID3D12Resource2> r;
    check(c.device->CreatePlacedResource(h.Get(), 0, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)),
          "placed on existing Heap1");
    h.Reset();
    resourceContract(r.Get(), c.device.Get(), d);
    c.copyProof(r.Get(), false, false);
    r.Reset();
    c.begin();
    c.submit();
    if (address) require(VirtualFree(address, 0, MEM_RELEASE) != FALSE, "free caller host memory");
    std::printf("RESOURCE_INTERFACES_CASE existing_mapping=%u lifetime=retained gpu=exact\n", mapping);
  }
  for (UINT depth : {0u, 1u}) {
    auto logical = desc(true);
    logical.Format = DXGI_FORMAT(189);
    logical.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto d = extended(logical, {8, 8, depth});
    for (bool placed : {false, true}) {
      auto h = c.heap();
      ComPtr<ID3D12Resource2> r;
      check(placed ? c.device->CreatePlacedResource1(h.Get(), 0, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r))
                   : c.device->CreateCommittedResource2(&gpu, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr, IID_PPV_ARGS(&r)), "feedback requested Resource2");
      h.Reset();
      resourceContract(r.Get(), c.device.Get(), logical, d.SamplerFeedbackMipRegion);
      auto decoded = c.buffer(D3D12_HEAP_TYPE_DEFAULT, 64, D3D12_RESOURCE_STATE_RESOLVE_DEST);
      auto rb = c.buffer(D3D12_HEAP_TYPE_READBACK, 64, D3D12_RESOURCE_STATE_COPY_DEST);
      c.begin();
      c.barrier(r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      c.list->ResolveSubresourceRegion(decoded.Get(), 0, 0, 0, r.Get(), UINT_MAX, nullptr,
                                       DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
      c.barrier(decoded.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      c.list->CopyBufferRegion(rb.Get(), 0, decoded.Get(), 0, 64);
      c.submit();
      void *memory = nullptr;
      check(rb->Map(0, nullptr, &memory), "feedback readback");
      for (UINT i = 0; i < 64; ++i) require(static_cast<BYTE *>(memory)[i] == 255, "feedback pristine decode exact");
      rb->Unmap(0, nullptr);
      std::printf("RESOURCE_INTERFACES_CASE feedback_placed=%u depth=%u descriptor=exact gpu=exact\n", placed, depth);
    }
  }
  std::puts("RESOURCE_INTERFACES_PASS abi=public aggregate=exact lifetime=retained gpu=exact");
}
