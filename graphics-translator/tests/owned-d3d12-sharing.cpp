// D3D12 shared handles: heaps, committed buffers/textures, fences; same-process
// cross-device, named, cross-process (both directions), GPU/CPU fence ordering,
// and rejection of non-shared, wrong-riid, forged and unrelated handles.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT64 kWords = 4096;
constexpr UINT64 kBytes = kWords * 4;
constexpr UINT kTexSize = 64;
constexpr DWORD kTimeoutMs = 20000;
const char *g_role = "parent";
int g_cases = 0;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_SHARING_FAIL role=%s step=%s hr=0x%08lx\n", g_role, step,
               static_cast<unsigned long>(hr));
  std::fflush(stderr);
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_D3D12_SHARING_HRESULT_MISMATCH step=%s expected=0x%08lx actual=0x%08lx\n", step,
                 static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    ExitProcess(1);
  }
}

void pass(const char *name) {
  ++g_cases;
  std::printf("OWNED_D3D12_SHARING_CASE %s status=PASS\n", name);
  std::fflush(stdout);
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  if (!module)
    fail("GetModuleHandleW", HRESULT_FROM_WIN32(GetLastError()));
  FARPROC raw = GetProcAddress(module, "OwnedGraphicsBackendId");
  if (!raw)
    fail("OwnedGraphicsBackendId missing");
  using BackendId = const char *(WINAPI *)();
  const char *id = reinterpret_cast<BackendId>(raw)();
  if (!id || std::strcmp(id, "owned-graphics-d3d12") != 0)
    fail("OwnedGraphicsBackendId mismatch");
  if (!std::strcmp(g_role, "parent"))
    std::printf("OWNED_D3D12_SHARING_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = 1;
  p.VisibleNodeMask = 1;
  return p;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  return d;
}

D3D12_RESOURCE_DESC textureDesc(D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = kTexSize;
  d.Height = kTexSize;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Layout = layout;
  d.Flags = flags;
  return d;
}

uint32_t pattern(uint32_t seed, uint32_t i) {
  return (seed * 0x9e3779b1u) ^ (i * 0x85ebca6bu) ^ (i >> 3);
}

struct Device {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 0;

  void init() {
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)),
          "CreateCommandList");
    check(list->Close(), "Close");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  }

  ID3D12GraphicsCommandList *begin() {
    check(allocator->Reset(), "Allocator Reset");
    check(list->Reset(allocator.Get(), nullptr), "List Reset");
    return list.Get();
  }

  void submit(bool wait = true) {
    check(list->Close(), "Close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++value), "Signal");
    if (wait)
      waitFor(fence.Get(), value);
  }

  static void waitFor(ID3D12Fence *f, UINT64 v) {
    if (f->GetCompletedValue() >= v)
      return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(f->SetEventOnCompletion(v, event), "SetEventOnCompletion");
    const DWORD r = WaitForSingleObject(event, kTimeoutMs);
    CloseHandle(event);
    if (r != WAIT_OBJECT_0)
      fail("fence wait timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
  }

  ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
    auto p = heapProps(type);
    auto d = bufferDesc(bytes);
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
          "CreateCommittedResource staging");
    return r;
  }

  // Uploads `seed` words into `dst` (a buffer in COMMON) through the GPU.
  void uploadBuffer(ID3D12Resource *dst, uint32_t seed) {
    auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint32_t *words = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&words)), "Map upload");
    for (uint32_t i = 0; i < kWords; ++i)
      words[i] = pattern(seed, i);
    upload->Unmap(0, nullptr);
    auto *l = begin();
    l->CopyBufferRegion(dst, 0, upload.Get(), 0, kBytes);
    submit();
  }

  void readBuffer(ID3D12Resource *src, uint32_t seed, const char *step) {
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    auto *l = begin();
    l->CopyBufferRegion(readback.Get(), 0, src, 0, kBytes);
    submit();
    verifyWords(readback.Get(), seed, step);
  }

  static void verifyWords(ID3D12Resource *readback, uint32_t seed, const char *step) {
    uint32_t *words = nullptr;
    D3D12_RANGE range{0, kBytes};
    check(readback->Map(0, &range, reinterpret_cast<void **>(&words)), "Map readback");
    for (uint32_t i = 0; i < kWords; ++i)
      if (words[i] != pattern(seed, i)) {
        std::fprintf(stderr, "OWNED_D3D12_SHARING_MISMATCH step=%s index=%u got=0x%08x want=0x%08x\n", step, i,
                     words[i], pattern(seed, i));
        fail(step);
      }
    readback->Unmap(0, nullptr);
  }

  // Texture <-> buffer with a 256-byte pitch footprint (64 texels * 4 bytes == 256).
  D3D12_TEXTURE_COPY_LOCATION footprint(ID3D12Resource *buf) {
    D3D12_TEXTURE_COPY_LOCATION loc{};
    loc.pResource = buf;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kTexSize, kTexSize, 1, kTexSize * 4};
    return loc;
  }

  void uploadTexture(ID3D12Resource *tex, uint32_t seed) {
    auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint32_t *words = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&words)), "Map tex upload");
    for (uint32_t i = 0; i < kWords; ++i)
      words[i] = pattern(seed, i);
    upload->Unmap(0, nullptr);
    auto *l = begin();
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto src = footprint(upload.Get());
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    // A promoted COPY_DEST texture does not decay; shared resources cross devices in COMMON.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    l->ResourceBarrier(1, &barrier);
    submit();
  }

  void readTexture(ID3D12Resource *tex, uint32_t seed, const char *step) {
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    auto *l = begin();
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    auto dst = footprint(readback.Get());
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submit();
    verifyWords(readback.Get(), seed, step);
  }
};

ComPtr<ID3D12Resource> sharedBuffer(Device &d) {
  auto p = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  auto desc = bufferDesc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
  ComPtr<ID3D12Resource> r;
  check(d.device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, &desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)),
        "CreateCommittedResource shared buffer");
  return r;
}

ComPtr<ID3D12Resource> sharedTexture(Device &d) {
  auto p = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  auto desc = textureDesc();
  ComPtr<ID3D12Resource> r;
  check(d.device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&r)),
        "CreateCommittedResource shared texture");
  return r;
}

ComPtr<ID3D12Fence> sharedFence(Device &d, UINT64 initial) {
  ComPtr<ID3D12Fence> f;
  check(d.device->CreateFence(initial, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f)), "CreateFence shared");
  return f;
}

HANDLE share(Device &d, ID3D12DeviceChild *object, const wchar_t *name = nullptr) {
  HANDLE h = nullptr;
  check(d.device->CreateSharedHandle(object, nullptr, GENERIC_ALL, name, &h), "CreateSharedHandle");
  if (!h)
    fail("CreateSharedHandle null");
  return h;
}

template <typename T> ComPtr<T> open(Device &d, HANDLE h, const char *step) {
  ComPtr<T> out;
  check(d.device->OpenSharedHandle(h, IID_PPV_ARGS(&out)), step);
  return out;
}

// A -> B and B -> A for a buffer, a texture and a fence, all readbacks exact.
void sameProcessCases(Device &a, Device &b) {
  Device *pairs[2][2] = {{&a, &b}, {&b, &a}};
  const char *dirs[2] = {"a_to_b", "b_to_a"};
  char name[96];
  for (int dir = 0; dir < 2; ++dir) {
    Device &src = *pairs[dir][0];
    Device &dst = *pairs[dir][1];
    const uint32_t seed = 0x100 + dir;

    auto buf = sharedBuffer(src);
    src.uploadBuffer(buf.Get(), seed);
    HANDLE h = share(src, buf.Get());
    auto opened = open<ID3D12Resource>(dst, h, "OpenSharedHandle buffer");
    CloseHandle(h);
    dst.readBuffer(opened.Get(), seed, "buffer readback");
    // Write back through the opened side and observe it on the creator.
    dst.uploadBuffer(opened.Get(), seed + 7);
    src.readBuffer(buf.Get(), seed + 7, "buffer writeback");
    std::snprintf(name, sizeof(name), "buffer-%s", dirs[dir]);
    pass(name);

    auto tex = sharedTexture(src);
    src.uploadTexture(tex.Get(), seed + 11);
    h = share(src, tex.Get());
    auto otex = open<ID3D12Resource>(dst, h, "OpenSharedHandle texture");
    CloseHandle(h);
    const auto od = otex->GetDesc();
    if (od.Width != kTexSize || od.Height != kTexSize || od.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
      fail("opened texture desc");
    dst.readTexture(otex.Get(), seed + 11, "texture readback");
    dst.uploadTexture(otex.Get(), seed + 12);
    src.readTexture(tex.Get(), seed + 12, "texture writeback");
    std::snprintf(name, sizeof(name), "texture-%s", dirs[dir]);
    pass(name);

    // Fence: a GPU signal on one device completes on both, and a GPU wait on
    // the other device orders a copy after it.
    auto fence = sharedFence(src, 3);
    h = share(src, fence.Get());
    auto ofence = open<ID3D12Fence>(dst, h, "OpenSharedHandle fence");
    CloseHandle(h);
    if (ofence->GetCompletedValue() != 3)
      fail("opened fence initial value");
    ComPtr<ID3D12Fence1> ofence1;
    check(ofence.As(&ofence1), "ID3D12Fence1");
    if (!(ofence1->GetCreationFlags() & D3D12_FENCE_FLAG_SHARED))
      fail("opened fence flags");
    check(src.queue->Signal(fence.Get(), 9), "queue Signal shared");
    Device::waitFor(ofence.Get(), 9);
    if (fence->GetCompletedValue() != 9 || ofence->GetCompletedValue() != 9)
      fail("fence completed value after GPU signal");
    std::snprintf(name, sizeof(name), "fence-%s", dirs[dir]);
    pass(name);
  }
}

// CPU Signal on device A releases a GPU Wait queued on device B; both
// GetCompletedValue observations follow the shared timeline.
void cpuSignalGpuWait(Device &a, Device &b) {
  auto fence = sharedFence(a, 0);
  HANDLE h = share(a, fence.Get());
  auto ofence = open<ID3D12Fence>(b, h, "OpenSharedHandle cpu/gpu fence");
  CloseHandle(h);

  auto buf = sharedBuffer(b);
  auto upload = b.buffer(D3D12_HEAP_TYPE_UPLOAD, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint32_t *words = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&words)), "Map upload wait");
  for (uint32_t i = 0; i < kWords; ++i)
    words[i] = pattern(0x77, i);
  upload->Unmap(0, nullptr);

  check(b.queue->Wait(ofence.Get(), 5), "queue Wait shared");
  auto *l = b.begin();
  l->CopyBufferRegion(buf.Get(), 0, upload.Get(), 0, kBytes);
  b.submit(false);
  const UINT64 marker = b.value;
  Sleep(200);
  if (b.fence->GetCompletedValue() >= marker)
    fail("GPU wait did not block");
  if (fence->GetCompletedValue() != 0 || ofence->GetCompletedValue() != 0)
    fail("fence advanced before CPU signal");
  check(fence->Signal(5), "CPU Signal shared");
  Device::waitFor(b.fence.Get(), marker);
  if (fence->GetCompletedValue() != 5 || ofence->GetCompletedValue() != 5)
    fail("completed values after CPU signal");
  b.readBuffer(buf.Get(), 0x77, "gpu wait copy");
  pass("cpu-signal-gpu-wait");
}

void heapCase(Device &a, Device &b) {
  D3D12_HEAP_DESC hd{};
  hd.SizeInBytes = 4 * 65536;
  hd.Properties = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ComPtr<ID3D12Heap> heap;
  check(a.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "CreateHeap shared");
  HANDLE h = share(a, heap.Get());
  auto oheap = open<ID3D12Heap>(b, h, "OpenSharedHandle heap");
  CloseHandle(h);
  const auto od = oheap->GetDesc();
  if (od.SizeInBytes != hd.SizeInBytes || !(od.Flags & D3D12_HEAP_FLAG_SHARED))
    fail("opened heap desc");

  auto desc = bufferDesc(kBytes);
  ComPtr<ID3D12Resource> pa0, pa1, pb0, pb1;
  check(a.device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&pa0)), "CreatePlacedResource a0");
  check(a.device->CreatePlacedResource(heap.Get(), 2 * 65536, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&pa1)), "CreatePlacedResource a1");
  check(b.device->CreatePlacedResource(oheap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&pb0)), "CreatePlacedResource b0");
  check(b.device->CreatePlacedResource(oheap.Get(), 2 * 65536, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&pb1)), "CreatePlacedResource b1");
  a.uploadBuffer(pa0.Get(), 0x201);
  b.uploadBuffer(pb1.Get(), 0x202);
  b.readBuffer(pb0.Get(), 0x201, "heap placed a->b");
  a.readBuffer(pa1.Get(), 0x202, "heap placed b->a");
  pass("heap-placed");

  // A row-major cross-adapter texture placed in the shared heap aliases the same bytes.
  D3D12_HEAP_DESC th = hd;
  th.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
  ComPtr<ID3D12Heap> xheap;
  check(a.device->CreateHeap(&th, IID_PPV_ARGS(&xheap)), "CreateHeap cross-adapter");
  h = share(a, xheap.Get());
  auto oxheap = open<ID3D12Heap>(b, h, "OpenSharedHandle cross-adapter heap");
  CloseHandle(h);
  auto rm = textureDesc(D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
  ComPtr<ID3D12Resource> ta, tb;
  check(a.device->CreatePlacedResource(xheap.Get(), 0, &rm, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&ta)), "CreatePlacedResource row-major a");
  check(b.device->CreatePlacedResource(oxheap.Get(), 0, &rm, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&tb)), "CreatePlacedResource row-major b");
  a.uploadTexture(ta.Get(), 0x203);
  b.readTexture(tb.Get(), 0x203, "row-major placed a->b");
  pass("heap-row-major-texture");
}

void namedCase(Device &a, Device &b) {
  wchar_t name[64];
  std::swprintf(name, 64, L"OwnedD3D12Sharing.%lu.buffer", GetCurrentProcessId());
  auto buf = sharedBuffer(a);
  a.uploadBuffer(buf.Get(), 0x301);
  HANDLE h = share(a, buf.Get(), name);
  HANDLE dup = nullptr;
  expect(a.device->CreateSharedHandle(buf.Get(), nullptr, GENERIC_ALL, name, &dup),
         static_cast<HRESULT>(0x887a002c) /* DXGI_ERROR_NAME_ALREADY_EXISTS */, "duplicate name");
  HANDLE byName = nullptr;
  check(b.device->OpenSharedHandleByName(name, GENERIC_ALL, &byName), "OpenSharedHandleByName");
  auto opened = open<ID3D12Resource>(b, byName, "OpenSharedHandle by name");
  b.readBuffer(opened.Get(), 0x301, "named readback");
  CloseHandle(byName);
  CloseHandle(h);
  HANDLE gone = nullptr;
  expect(b.device->OpenSharedHandleByName(L"OwnedD3D12Sharing.never-created", GENERIC_ALL, &gone), E_INVALIDARG,
         "OpenSharedHandleByName missing");
  pass("named-open");
}

void errorCases(Device &a, Device &b) {
  // Non-shared objects.
  auto plain = a.buffer(D3D12_HEAP_TYPE_DEFAULT, kBytes, D3D12_RESOURCE_STATE_COMMON);
  HANDLE h = reinterpret_cast<HANDLE>(1);
  expect(a.device->CreateSharedHandle(plain.Get(), nullptr, GENERIC_ALL, nullptr, &h), E_INVALIDARG,
         "non-shared resource");
  if (h)
    fail("non-shared handle not cleared");
  expect(a.device->CreateSharedHandle(a.fence.Get(), nullptr, GENERIC_ALL, nullptr, &h), E_INVALIDARG,
         "non-shared fence");
  D3D12_HEAP_DESC hd{};
  hd.SizeInBytes = 65536;
  hd.Properties = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Heap> plainHeap;
  check(a.device->CreateHeap(&hd, IID_PPV_ARGS(&plainHeap)), "CreateHeap plain");
  expect(a.device->CreateSharedHandle(plainHeap.Get(), nullptr, GENERIC_ALL, nullptr, &h), E_INVALIDARG,
         "non-shared heap");
  expect(a.device->CreateSharedHandle(nullptr, nullptr, GENERIC_ALL, nullptr, &h), E_INVALIDARG, "null object");

  // Wrong riid.
  auto fence = sharedFence(a, 1);
  h = share(a, fence.Get());
  ComPtr<ID3D12Resource> wrong;
  expect(b.device->OpenSharedHandle(h, IID_PPV_ARGS(&wrong)), E_NOINTERFACE, "fence as resource");
  auto buf = sharedBuffer(a);
  HANDLE hb = share(a, buf.Get());
  ComPtr<ID3D12Fence> wrongFence;
  expect(b.device->OpenSharedHandle(hb, IID_PPV_ARGS(&wrongFence)), E_NOINTERFACE, "resource as fence");
  ComPtr<ID3D12Heap> wrongHeap;
  expect(b.device->OpenSharedHandle(hb, IID_PPV_ARGS(&wrongHeap)), E_NOINTERFACE, "resource as heap");
  CloseHandle(hb);
  CloseHandle(h);

  // Forged and unrelated handles.
  ComPtr<ID3D12Resource> out;
  expect(b.device->OpenSharedHandle(nullptr, IID_PPV_ARGS(&out)), E_INVALIDARG, "null handle");
  expect(b.device->OpenSharedHandle(reinterpret_cast<HANDLE>(0x7ffc), IID_PPV_ARGS(&out)), E_INVALIDARG,
         "garbage handle");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  expect(b.device->OpenSharedHandle(event, IID_PPV_ARGS(&out)), E_INVALIDARG, "event handle");
  CloseHandle(event);
  HANDLE anon = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1 << 20, nullptr);
  expect(b.device->OpenSharedHandle(anon, IID_PPV_ARGS(&out)), E_INVALIDARG, "unrelated section");
  CloseHandle(anon);
  // A section that mimics the naming scheme but carries a forged header/token.
  wchar_t forgedName[96];
  std::swprintf(forgedName, 96, L"Local\\dxmt.share.%lu.00112233445566778899aabbccddeeff", GetCurrentProcessId());
  HANDLE forged = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1 << 20, forgedName);
  if (!forged)
    fail("CreateFileMapping forged");
  auto *bytes = static_cast<uint8_t *>(MapViewOfFile(forged, FILE_MAP_WRITE, 0, 0, 0));
  const uint64_t magic = 0x5245485354584d44ull;
  std::memcpy(bytes, &magic, 8);
  const uint32_t version = 1, kind = 2;
  std::memcpy(bytes + 8, &version, 4);
  std::memcpy(bytes + 12, &kind, 4);
  std::memset(bytes + 16, 0xa5, 16); // token disagrees with the name
  UnmapViewOfFile(bytes);
  expect(b.device->OpenSharedHandle(forged, IID_PPV_ARGS(&out)), E_INVALIDARG, "forged token");
  CloseHandle(forged);
  // A forged named link pointing at a real share with the wrong token.
  auto victim = sharedBuffer(a);
  HANDLE hv = share(a, victim.Get(), L"OwnedD3D12Sharing.victim");
  HANDLE link = nullptr;
  check(b.device->OpenSharedHandleByName(L"OwnedD3D12Sharing.victim", GENERIC_ALL, &link), "open victim link");
  auto *lv = static_cast<uint8_t *>(MapViewOfFile(link, FILE_MAP_WRITE, 0, 0, 0));
  lv[8] ^= 0xff; // corrupt the token copy in the link
  UnmapViewOfFile(lv);
  expect(b.device->OpenSharedHandle(link, IID_PPV_ARGS(&out)), E_INVALIDARG, "stale link token");
  CloseHandle(link);
  CloseHandle(hv);
  if (out)
    fail("rejected open returned an object");
  pass("errors");
}

// --- Host memory heaps (OpenExistingHeapFromAddress / FromFileMapping) -------

void hostMemoryCase(Device &a) {
  ComPtr<ID3D12Device3> device3;
  check(a.device.As(&device3), "ID3D12Device3");
  constexpr SIZE_T kHostBytes = 4 * 65536;
  auto *memory = static_cast<uint32_t *>(VirtualAlloc(nullptr, kHostBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!memory)
    fail("VirtualAlloc");
  std::memset(memory, 0, kHostBytes);
  ComPtr<ID3D12Heap> heap;
  check(device3->OpenExistingHeapFromAddress(memory, IID_PPV_ARGS(&heap)), "OpenExistingHeapFromAddress");
  const auto desc = heap->GetDesc();
  if (desc.SizeInBytes != kHostBytes || desc.Properties.Type != D3D12_HEAP_TYPE_CUSTOM ||
      desc.Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
      desc.Properties.MemoryPoolPreference != D3D12_MEMORY_POOL_L0 ||
      desc.Flags != (D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS))
    fail("host heap desc");
  // GPU writes into a placed sub-range at 64 KiB; the CPU reads the app pointer directly.
  auto bd = bufferDesc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
  ComPtr<ID3D12Resource> placed;
  check(a.device->CreatePlacedResource(heap.Get(), 65536, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&placed)), "CreatePlacedResource host");
  a.uploadBuffer(placed.Get(), 0x601);
  const uint32_t *words = memory + 65536 / 4;
  for (uint32_t i = 0; i < kWords; ++i)
    if (words[i] != pattern(0x601, i))
      fail("host memory GPU write not visible");
  if (memory[0] != 0 || memory[65536 / 4 + kWords] != 0)
    fail("host memory write outside placement");
  // CPU writes are seen by the GPU without a copy.
  for (uint32_t i = 0; i < kWords; ++i)
    memory[65536 / 4 + i] = pattern(0x602, i);
  a.readBuffer(placed.Get(), 0x602, "host memory CPU write");
  placed.Reset();
  heap.Reset();

  // Rejections: misaligned (4 KiB and 1-byte offsets), unknown/free, reserved-only, null.
  expect(device3->OpenExistingHeapFromAddress(reinterpret_cast<uint8_t *>(memory) + 4096, IID_PPV_ARGS(&heap)),
         E_INVALIDARG, "host heap 4K misaligned");
  expect(device3->OpenExistingHeapFromAddress(reinterpret_cast<uint8_t *>(memory) + 1, IID_PPV_ARGS(&heap)),
         E_INVALIDARG, "host heap 1B misaligned");
  void *reserved = VirtualAlloc(nullptr, kHostBytes, MEM_RESERVE, PAGE_NOACCESS);
  expect(device3->OpenExistingHeapFromAddress(reserved, IID_PPV_ARGS(&heap)), E_INVALIDARG, "host heap reserved");
  VirtualFree(reserved, 0, MEM_RELEASE);
  expect(device3->OpenExistingHeapFromAddress(nullptr, IID_PPV_ARGS(&heap)), E_INVALIDARG, "host heap null");
  VirtualFree(memory, 0, MEM_RELEASE);
  expect(device3->OpenExistingHeapFromAddress(memory, IID_PPV_ARGS(&heap)), E_INVALIDARG, "host heap freed");
  pass("host-memory-address");

  // File mapping: the GPU writes through the heap; a separate view of the same mapping sees it.
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kHostBytes, nullptr);
  if (!mapping)
    fail("CreateFileMapping host");
  check(device3->OpenExistingHeapFromFileMapping(mapping, IID_PPV_ARGS(&heap)), "OpenExistingHeapFromFileMapping");
  if (heap->GetDesc().SizeInBytes != kHostBytes)
    fail("file mapping heap size");
  check(a.device->CreatePlacedResource(heap.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&placed)), "CreatePlacedResource mapping");
  a.uploadBuffer(placed.Get(), 0x603);
  auto *view = static_cast<const uint32_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
  for (uint32_t i = 0; i < kWords; ++i)
    if (view[i] != pattern(0x603, i))
      fail("file mapping GPU write not visible");
  UnmapViewOfFile(view);
  placed.Reset();
  heap.Reset();
  CloseHandle(mapping);
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  expect(device3->OpenExistingHeapFromFileMapping(event, IID_PPV_ARGS(&heap)), E_INVALIDARG, "file mapping event");
  CloseHandle(event);
  pass("host-memory-file-mapping");
}

// Shared fences rewind like non-shared ones: logical values follow the last Signal
// on every device, while waits latch at the first qualifying published value.
void fenceRewindCase(Device &a, Device &b) {
  auto fence = sharedFence(a, 0);
  HANDLE h = share(a, fence.Get());
  auto ofence = open<ID3D12Fence>(b, h, "OpenSharedHandle rewind fence");
  CloseHandle(h);

  auto buf = sharedBuffer(b);
  auto upload = b.buffer(D3D12_HEAP_TYPE_UPLOAD, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint32_t *words = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&words)), "Map rewind upload");
  for (uint32_t i = 0; i < kWords; ++i)
    words[i] = pattern(0x88, i);
  upload->Unmap(0, nullptr);

  // Wait(3) registered before the fence reaches 10 and is rewound to 5.
  check(b.queue->Wait(ofence.Get(), 3), "queue Wait 3");
  check(b.queue->Signal(b.fence.Get(), ++b.value), "marker 3");
  const UINT64 early = b.value;
  check(a.queue->Signal(fence.Get(), 10), "queue Signal 10");
  Device::waitFor(fence.Get(), 10);
  Device::waitFor(ofence.Get(), 10);
  check(fence->Signal(5), "CPU Signal 5 (rewind)");
  if (fence->GetCompletedValue() != 5 || ofence->GetCompletedValue() != 5)
    fail("rewound completed value");
  Device::waitFor(b.fence.Get(), early);

  // Wait(7) registered after the rewind blocks until Signal(7).
  check(b.queue->Wait(ofence.Get(), 7), "queue Wait 7");
  auto *l = b.begin();
  l->CopyBufferRegion(buf.Get(), 0, upload.Get(), 0, kBytes);
  b.submit(false);
  const UINT64 late = b.value;
  Sleep(200);
  if (b.fence->GetCompletedValue() >= late)
    fail("Wait(7) did not block after rewind");
  if (fence->GetCompletedValue() != 5 || ofence->GetCompletedValue() != 5)
    fail("value changed while blocked");
  check(ofence->Signal(7), "CPU Signal 7 on opened fence");
  Device::waitFor(b.fence.Get(), late);
  if (fence->GetCompletedValue() != 7 || ofence->GetCompletedValue() != 7)
    fail("completed values after Signal 7");
  b.readBuffer(buf.Get(), 0x88, "rewind wait copy");
  pass("fence-rewind");
}

// Two enqueued producers and a CPU producer complete out of enqueue order. A
// queue wait is a deterministic gate; it must not block either API thread.
void fenceProducerCompletionCase(Device &a, Device &b) {
  auto fence = sharedFence(a, 0);
  HANDLE h = share(a, fence.Get());
  auto remote = open<ID3D12Fence>(b, h, "open completion-order fence");
  CloseHandle(h);
  ComPtr<ID3D12Fence> gate;
  check(a.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "create producer gate");
  HANDLE crossing = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(remote->SetEventOnCompletion(10, crossing), "completion-order crossing event");
  check(a.queue->Wait(gate.Get(), 1), "gate older producer");
  check(a.queue->Signal(fence.Get(), 10), "enqueue older producer 10");
  check(fence->Signal(3), "complete CPU producer 3");
  if (remote->GetCompletedValue() != 3 || WaitForSingleObject(crossing, 0) != WAIT_TIMEOUT)
    fail("incomplete older producer changed completion");
  check(b.queue->Signal(remote.Get(), 2), "enqueue later producer 2");
  const UINT64 second = ++b.value;
  check(b.queue->Signal(b.fence.Get(), second), "marker later producer 2");
  Device::waitFor(b.fence.Get(), second);
  if (fence->GetCompletedValue() != 2 || remote->GetCompletedValue() != 2)
    fail("later producer must complete before gated older");
  // Register a queue wait only after B is free to consume the older completion.
  check(b.queue->Wait(remote.Get(), 10), "older completion queue wait");
  const UINT64 crossed = ++b.value;
  check(b.queue->Signal(b.fence.Get(), crossed), "older completion queue marker");
  check(gate->Signal(1), "release older producer");
  if (WaitForSingleObject(crossing, kTimeoutMs) != WAIT_OBJECT_0)
    fail("older completion crossing event");
  Device::waitFor(b.fence.Get(), crossed);
  if (fence->GetCompletedValue() != 10 || remote->GetCompletedValue() != 10)
    fail("older enqueued producer completed last but was discarded");
  check(remote->Signal(0), "rewind after older completion");
  if (fence->GetCompletedValue() != 0)
    fail("CPU rewind after older completion");
  CloseHandle(crossing);
  pass("fence-producer-completion-order");
}

// 3000 back-to-back GPU Signals on device A while device B polls:
// B must reach the final value, and never observe a value outside the signaled range.
void fenceStressCase(Device &a, Device &b) {
  constexpr UINT64 kSignals = 3000;
  auto fence = sharedFence(a, 0);
  HANDLE h = share(a, fence.Get());
  auto ofence = open<ID3D12Fence>(b, h, "OpenSharedHandle stress fence");
  CloseHandle(h);
  for (UINT64 v = 1; v <= kSignals; ++v)
    check(a.queue->Signal(fence.Get(), v), "stress Signal");
  const DWORD start = GetTickCount();
  UINT64 last = 0, polls = 0;
  while (last != kSignals) {
    const UINT64 value = ofence->GetCompletedValue();
    if (value > kSignals || value < last)
      fail("stress observed value out of order");
    last = value;
    ++polls;
    if (GetTickCount() - start > kTimeoutMs)
      fail("stress final value not reached", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
  }
  Device::waitFor(fence.Get(), kSignals);
  if (fence->GetCompletedValue() != kSignals)
    fail("stress creator value");
  std::printf("OWNED_D3D12_SHARING_STRESS signals=%llu final=%llu\n", static_cast<unsigned long long>(kSignals),
              static_cast<unsigned long long>(last));
  pass("fence-stress");
}

void namedObjectNames(DWORD pid, wchar_t buffer[64], wchar_t fence[64]) {
  std::swprintf(buffer, 64, L"OwnedD3D12Sharing.%lu.named-buffer", pid);
  std::swprintf(fence, 64, L"OwnedD3D12Sharing.%lu.named-fence", pid);
}

// A process that inherits nothing opens both objects by name only.
int namedChildMain(DWORD parent) {
  g_role = "named-child";
  requireOwnedBackend();
  wchar_t bufferName[64], fenceName[64];
  namedObjectNames(parent, bufferName, fenceName);
  Device c;
  c.init();
  HANDLE hb = nullptr, hf = nullptr;
  check(c.device->OpenSharedHandleByName(bufferName, GENERIC_ALL, &hb), "named child open buffer name");
  check(c.device->OpenSharedHandleByName(fenceName, GENERIC_ALL, &hf), "named child open fence name");
  auto buf = open<ID3D12Resource>(c, hb, "named child open buffer");
  auto fence = open<ID3D12Fence>(c, hf, "named child open fence");
  CloseHandle(hb);
  CloseHandle(hf);
  // The parent signaled 45 and then rewound to 40 before launching this process.
  if (fence->GetCompletedValue() != 40)
    fail("named child rewound value");
  c.readBuffer(buf.Get(), 0x701, "named child buffer");
  c.uploadBuffer(buf.Get(), 0x702);
  check(c.queue->Signal(fence.Get(), 50), "named child queue Signal 50");
  Device::waitFor(fence.Get(), 50);
  std::printf("OWNED_D3D12_SHARING_NAMED_CHILD ok rewind=40\n");
  return 0;
}

void namedProcessCase(Device &a) {
  wchar_t bufferName[64], fenceName[64];
  namedObjectNames(GetCurrentProcessId(), bufferName, fenceName);
  auto buf = sharedBuffer(a);
  a.uploadBuffer(buf.Get(), 0x701);
  auto fence = sharedFence(a, 45);
  check(fence->Signal(40), "rewind 45 -> 40");
  HANDLE hb = share(a, buf.Get(), bufferName);
  HANDLE hf = share(a, fence.Get(), fenceName);

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t cmd[MAX_PATH + 48];
  std::swprintf(cmd, MAX_PATH + 48, L"\"%ls\" --named %lu", exe, GetCurrentProcessId());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    fail("CreateProcess named", HRESULT_FROM_WIN32(GetLastError()));
  Device::waitFor(fence.Get(), 50);
  if (WaitForSingleObject(pi.hProcess, kTimeoutMs) != WAIT_OBJECT_0)
    fail("named child exit");
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0)
    fail("named child exit code", HRESULT(code));
  if (fence->GetCompletedValue() != 50)
    fail("named child fence value");
  a.readBuffer(buf.Get(), 0x702, "named child write");
  CloseHandle(hb);
  CloseHandle(hf);
  pass("named-unrelated-process");
}

// --- Cross process -------------------------------------------------------

// Handle values travel through a mailbox section named by the child's pid:
// the parent duplicates its handles into the suspended child and the child
// publishes handles to its own shared objects, which the parent duplicates
// back out of the child process.
struct Mailbox {
  uint64_t buffer, texture, fence, ready, done; // parent -> child (values valid in the child)
  uint64_t latchReady;
  uint64_t childBuffer, childTexture, childFence; // child -> parent (values valid in the child)
  uint32_t seed;
};

HANDLE mailboxSection(DWORD pid, bool create) {
  wchar_t name[64];
  std::swprintf(name, 64, L"Local\\OwnedD3D12SharingMailbox.%lu", pid);
  return create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Mailbox), name)
                : OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
}

HANDLE fromValue(uint64_t v) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v)); }
uint64_t toValue(HANDLE h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

int childMain() {
  g_role = "child";
  requireOwnedBackend();
  HANDLE section = mailboxSection(GetCurrentProcessId(), false);
  if (!section)
    fail("child mailbox", HRESULT_FROM_WIN32(GetLastError()));
  auto *box = static_cast<Mailbox *>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(Mailbox)));
  const uint32_t seed = box->seed;

  Device c;
  c.init();
  auto buf = open<ID3D12Resource>(c, fromValue(box->buffer), "child open buffer");
  auto tex = open<ID3D12Resource>(c, fromValue(box->texture), "child open texture");
  auto fence = open<ID3D12Fence>(c, fromValue(box->fence), "child open fence");
  CloseHandle(fromValue(box->buffer));
  CloseHandle(fromValue(box->texture));
  CloseHandle(fromValue(box->fence));

  // Child-created objects for the child -> parent direction.
  auto cbuf = sharedBuffer(c);
  auto ctex = sharedTexture(c);
  auto cfence = sharedFence(c, 0);
  box->childBuffer = toValue(share(c, cbuf.Get()));
  box->childTexture = toValue(share(c, ctex.Get()));
  box->childFence = toValue(share(c, cfence.Get()));
  SetEvent(fromValue(box->ready));

  // Child -> parent first: the parent's GPU waits for 30 on its copy of the fence.
  c.uploadBuffer(cbuf.Get(), seed + 4);
  c.uploadTexture(ctex.Get(), seed + 5);
  check(c.queue->Signal(cfence.Get(), 30), "child queue Signal 30");

  // Parent -> child: the parent writes after `ready` and GPU-signals 10; this
  // queue's copies are ordered behind that signal by a GPU wait.
  check(c.queue->Wait(fence.Get(), 10), "child queue Wait");
  c.readBuffer(buf.Get(), seed, "child buffer readback");
  c.readTexture(tex.Get(), seed + 1, "child texture readback");
  if (fence->GetCompletedValue() < 10)
    fail("child fence completed");
  // Reply into the parent's objects, then GPU-signal 20 on the parent's fence.
  c.uploadBuffer(buf.Get(), seed + 2);
  c.uploadTexture(tex.Get(), seed + 3);
  check(c.queue->Signal(fence.Get(), 20), "child queue Signal 20");
  Device::waitFor(fence.Get(), 20);

  if (WaitForSingleObject(fromValue(box->done), kTimeoutMs) != WAIT_OBJECT_0)
    fail("child done wait");
  // The parent rewound its fence 20 -> 15 before `done`.
  if (fence->GetCompletedValue() != 15)
    fail("child rewound value");
  std::printf("OWNED_D3D12_SHARING_CHILD_REWIND value=15\n");

  // The parent suspends *all* observer threads after these registrations,
  // publishes a qualifying completion followed by thousands of rewinds, then
  // resumes us. Neither event nor queue wait may depend on sampling the peak.
  HANDLE latch = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(fence->SetEventOnCompletion(100, latch), "child crossing event registration");
  check(c.queue->Wait(fence.Get(), 100), "child crossing queue registration");
  const UINT64 crossed = ++c.value;
  check(c.queue->Signal(c.fence.Get(), crossed), "child crossing queue marker");
  SetEvent(fromValue(box->latchReady));
  if (WaitForSingleObject(latch, kTimeoutMs) != WAIT_OBJECT_0)
    fail("child unobserved crossing event");
  Device::waitFor(c.fence.Get(), crossed);
  if (fence->GetCompletedValue() != 0)
    fail("child crossing current value must be rewind");
  HANDLE late = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(fence->SetEventOnCompletion(100, late), "child late crossing registration");
  if (WaitForSingleObject(late, 0) != WAIT_TIMEOUT)
    fail("child late wait inherited pre-registration crossing");
  check(fence->Signal(100), "child release late crossing wait");
  if (WaitForSingleObject(late, kTimeoutMs) != WAIT_OBJECT_0)
    fail("child late crossing event");
  CloseHandle(latch);
  CloseHandle(late);
  std::printf("OWNED_D3D12_SHARING_CHILD_LATCH event=1 queue=1 late=1 current=0\n");
  UnmapViewOfFile(box);
  CloseHandle(section);
  std::printf("OWNED_D3D12_SHARING_CHILD ok\n");
  return 0;
}

void crossProcess(Device &a, Device &b) {
  const uint32_t seed = 0x501;
  auto buf = sharedBuffer(a);
  auto tex = sharedTexture(a);
  auto fence = sharedFence(a, 1);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE latchReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t cmd[MAX_PATH + 32];
  std::swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --child", exe);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
    fail("CreateProcess", HRESULT_FROM_WIN32(GetLastError()));
  HANDLE section = mailboxSection(pi.dwProcessId, true);
  if (!section)
    fail("mailbox", HRESULT_FROM_WIN32(GetLastError()));
  auto *box = static_cast<Mailbox *>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(Mailbox)));
  auto dupTo = [&](HANDLE h, bool close) {
    HANDLE out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, pi.hProcess, &out, 0, FALSE, DUPLICATE_SAME_ACCESS))
      fail("DuplicateHandle to child", HRESULT_FROM_WIN32(GetLastError()));
    if (close)
      CloseHandle(h);
    return toValue(out);
  };
  // The share handles are closed right after duplication: the child's copies
  // and the parent's objects keep the shares alive.
  box->buffer = dupTo(share(a, buf.Get()), true);
  box->texture = dupTo(share(a, tex.Get()), true);
  box->fence = dupTo(share(a, fence.Get()), true);
  box->ready = dupTo(ready, false);
  box->done = dupTo(done, false);
  box->latchReady = dupTo(latchReady, false);
  box->seed = seed;
  ResumeThread(pi.hThread);

  HANDLE waits[2] = {ready, pi.hProcess};
  if (WaitForMultipleObjects(2, waits, FALSE, kTimeoutMs) != WAIT_OBJECT_0)
    fail("child ready");
  auto dupFrom = [&](uint64_t v) {
    HANDLE out = nullptr;
    if (!DuplicateHandle(pi.hProcess, fromValue(v), GetCurrentProcess(), &out, 0, FALSE,
                         DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
      fail("DuplicateHandle from child", HRESULT_FROM_WIN32(GetLastError()));
    return out;
  };
  HANDLE hcb = dupFrom(box->childBuffer), hct = dupFrom(box->childTexture), hcf = dupFrom(box->childFence);
  auto cbuf = open<ID3D12Resource>(b, hcb, "parent open child buffer");
  auto ctex = open<ID3D12Resource>(b, hct, "parent open child texture");
  auto cfence = open<ID3D12Fence>(b, hcf, "parent open child fence");
  CloseHandle(hcb);
  CloseHandle(hct);
  CloseHandle(hcf);

  // Child -> parent ordering: device B's GPU waits for the child's signal 30
  // before copying, and the copy must see the child's data exactly.
  check(b.queue->Wait(cfence.Get(), 30), "parent queue Wait child fence");
  b.readBuffer(cbuf.Get(), seed + 4, "child->parent buffer");
  b.readTexture(ctex.Get(), seed + 5, "child->parent texture");
  if (cfence->GetCompletedValue() != 30)
    fail("child fence value in parent");

  // Parent -> child: write, then GPU-signal 10; the child replies and signals 20.
  a.uploadBuffer(buf.Get(), seed);
  a.uploadTexture(tex.Get(), seed + 1);
  check(a.queue->Signal(fence.Get(), 10), "parent queue Signal 10");
  Device::waitFor(fence.Get(), 20);
  if (fence->GetCompletedValue() != 20)
    fail("parent fence value after child signal");
  a.readBuffer(buf.Get(), seed + 2, "child reply buffer");
  a.readTexture(tex.Get(), seed + 3, "child reply texture");

  check(fence->Signal(15), "parent rewind 20 -> 15");
  SetEvent(done);
  if (WaitForSingleObject(latchReady, kTimeoutMs) != WAIT_OBJECT_0)
    fail("child crossing registrations ready");

  // Owned-backend deterministic scheduling probe: acquire the section mutex
  // before suspension so the child cannot be frozen inside publication. The
  // old backend has no such mutex; creating it leaves its behavior unchanged.
  HANDLE rawShare = share(a, fence.Get());
  auto *header = static_cast<const uint8_t *>(MapViewOfFile(rawShare, FILE_MAP_READ, 0, 0, 32));
  if (!header)
    fail("map shared fence scheduling identity");
  const auto *token = header + 16;
  WCHAR mutexName[80];
  std::swprintf(mutexName, 80, L"Local\\dxmt.fence.%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                token[0], token[1], token[2], token[3], token[4], token[5], token[6], token[7],
                token[8], token[9], token[10], token[11], token[12], token[13], token[14], token[15]);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
  UnmapViewOfFile(header);
  CloseHandle(rawShare);
  if (!mutex || WaitForSingleObject(mutex, kTimeoutMs) != WAIT_OBJECT_0)
    fail("lock observer scheduling mutex");
  using ProcessControl = LONG(WINAPI *)(HANDLE);
  auto suspend = reinterpret_cast<ProcessControl>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess"));
  auto resume = reinterpret_cast<ProcessControl>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess"));
  if (!suspend || !resume || suspend(pi.hProcess) < 0)
    fail("suspend child observers");
  ReleaseMutex(mutex);
  check(fence->Signal(100), "unobserved qualifying publication");
  for (unsigned i = 0; i < 3000; ++i)
    check(fence->Signal(0), "unobserved rewind publication");
  if (fence->GetCompletedValue() != 0)
    fail("parent crossing final rewind");
  if (resume(pi.hProcess) < 0)
    fail("resume child observers");
  CloseHandle(mutex);
  if (WaitForSingleObject(pi.hProcess, kTimeoutMs) != WAIT_OBJECT_0)
    fail("child exit");
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  if (code != 0)
    fail("child exit code", HRESULT(code));
  // Objects opened from the exited child stay valid in this process.
  b.readBuffer(cbuf.Get(), seed + 4, "child buffer after child exit");
  b.readTexture(ctex.Get(), seed + 5, "child texture after child exit");
  UnmapViewOfFile(box);
  CloseHandle(section);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(ready);
  CloseHandle(done);
  CloseHandle(latchReady);
  pass("cross-process-parent-to-child");
  pass("cross-process-child-to-parent");
  pass("cross-process-unobserved-rewind-latches");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc > 1 && !std::wcscmp(argv[1], L"--child"))
    return childMain();
  if (argc > 2 && !std::wcscmp(argv[1], L"--named"))
    return namedChildMain(static_cast<DWORD>(wcstoul(argv[2], nullptr, 10)));
  requireOwnedBackend();
  Device a, b;
  a.init();
  b.init();

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  check(a.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "OPTIONS");
  std::printf("OWNED_D3D12_SHARING_CAPS cross_node=%d row_major=%d\n", int(options.CrossNodeSharingTier),
              int(options.CrossAdapterRowMajorTextureSupported));

  sameProcessCases(a, b);
  cpuSignalGpuWait(a, b);
  heapCase(a, b);
  namedCase(a, b);
  errorCases(a, b);
  fenceRewindCase(a, b);
  fenceProducerCompletionCase(a, b);
  fenceStressCase(a, b);
  namedProcessCase(a);
  hostMemoryCase(a);
  crossProcess(a, b);
  std::printf("OWNED_D3D12_SHARING_PASS cases=%d\n", g_cases);
  return 0;
}
