#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr, const char *what) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "OWNED_D3D12_DEPTH_PLANE_COPY_FAIL step=%s hr=%08lx\n", what, (unsigned long)hr);
    std::exit(1);
  }
}
static D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type;
  h.CreationNodeMask = h.VisibleNodeMask = 1;
  return h;
}
static D3D12_RESOURCE_DESC buffer(UINT64 size) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
static D3D12_RESOURCE_DESC texture(DXGI_FORMAT format) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = 8;
  d.Height = 2;
  d.DepthOrArraySize = 2;
  d.MipLevels = 2;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  return d;
}
static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  list->ResourceBarrier(1, &b);
}
static D3D12_TEXTURE_COPY_LOCATION sub(ID3D12Resource *r, UINT plane) {
  D3D12_TEXTURE_COPY_LOCATION l{};
  l.pResource = r;
  l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  l.SubresourceIndex = plane;
  return l;
}
static D3D12_TEXTURE_COPY_LOCATION placed(ID3D12Resource *r, D3D12_PLACED_SUBRESOURCE_FOOTPRINT f) {
  D3D12_TEXTURE_COPY_LOCATION l{};
  l.pResource = r;
  l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  l.PlacedFootprint = f;
  return l;
}
static void inspect(const char *tag, const uint8_t *data, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *f,
                    DXGI_FORMAT format) {
  const unsigned p = (unsigned)f[0].Offset, q = (unsigned)f[1].Offset;
  const bool d24 = format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_R24G8_TYPELESS;
  const uint32_t expected24[] = {0, 0x00400000, 0x00800000, 0x00ffffff};
  const uint32_t expected32[] = {0, 0x3e800000, 0x3f000000, 0x3f800000};
  const uint32_t clear24[] = {0, 0x0019999a, 0x004ccccd, 0x00e66665};
  const uint32_t clear32[] = {0, 0x3dcccccd, 0x3e99999a, 0x3f666666};
  const uint8_t expectedStencil[] = {0x12, 0x34, 0x56, 0x78};
  for (unsigned x = 0; x < 4; x++) {
    uint32_t bits;
    std::memcpy(&bits, data + p + x * 4, 4);
    const uint32_t expected =
        std::strcmp(tag, "CLEAR") == 0 ? (d24 ? clear24[x] : clear32[x]) : (d24 ? expected24[x] : expected32[x]);
    if (bits != expected || data[q + x] != expectedStencil[x]) {
      std::fprintf(stderr, "OWNED_D3D12_DEPTH_PLANE_COPY_FAIL format=%u path=%s x=%u depth=%08x stencil=%02x\n",
                   unsigned(format), tag, x, bits, data[q + x]);
      std::exit(1);
    }
  }
}
static void runCase(DXGI_FORMAT format) {
  const bool d24 = format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_D24_UNORM_S8_UINT;
  const bool useSrv = format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_R32G8X24_TYPELESS;
  auto module = GetModuleHandleW(L"d3d12.dll");
  auto id = reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  if (!id || std::strcmp(id(), "owned-graphics-d3d12"))
    check(E_FAIL, "backend identity");
  std::printf("OWNED_D3D12_DEPTH_PLANE_COPY_BACKEND id=%s\n", id());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  ComPtr<ID3D12Device> dev;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "device");
  auto td = texture(format);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT f[2]{};
  UINT rows[2]{};
  UINT64 sizes[2]{}, total{};
  dev->GetCopyableFootprints(&td, 3, 1, 0, f, rows, sizes, &total);
  dev->GetCopyableFootprints(&td, 7, 1, 512, f + 1, rows + 1, sizes + 1, &total);
  if (total == UINT64_MAX || total > 1048576 || f[0].Footprint.Format != DXGI_FORMAT_R32_TYPELESS ||
      f[1].Footprint.Format != DXGI_FORMAT_R8_TYPELESS || rows[0] != 1 || rows[1] != 1 || sizes[0] != 16 ||
      sizes[1] != 4 || f[0].Footprint.RowPitch != 256 || f[1].Footprint.RowPitch != 256)
    check(E_FAIL, "footprint contract");
  auto dh = heap(D3D12_HEAP_TYPE_DEFAULT), uh = heap(D3D12_HEAP_TYPE_UPLOAD), rh = heap(D3D12_HEAP_TYPE_READBACK);
  D3D12_CLEAR_VALUE cv{};
  cv.Format = d24 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  cv.DepthStencil.Depth = 1;
  ComPtr<ID3D12Resource> cleared, uploaded, copied, wide, upload, readClear, readUpload, readCopied, readWide, gpuOut,
      readShader;
  check(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                     IID_PPV_ARGS(&cleared)),
        "cleared texture");
  check(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, &cv,
                                     IID_PPV_ARGS(&uploaded)),
        "uploaded texture");
  check(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, &cv,
                                     IID_PPV_ARGS(&copied)),
        "copied texture");
  check(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, &cv,
                                     IID_PPV_ARGS(&wide)),
        "wide texture");
  auto bd = buffer(total + 4096);
  check(dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&upload)),
        "upload buffer");
  check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&readClear)),
        "clear readback");
  check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&readUpload)),
        "upload readback");
  check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&readCopied)),
        "copy readback");
  check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&readWide)),
        "wide readback");
  auto small = buffer(256);
  small.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  check(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &small, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                     IID_PPV_ARGS(&gpuOut)),
        "shader output");
  small.Flags = D3D12_RESOURCE_FLAG_NONE;
  check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &small, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&readShader)),
        "shader readback");
  uint8_t *src{};
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&src)), "upload map");
  std::memset(src, 0xa5, total + 4096);
  const uint32_t packed[4] = {0, d24 ? 0x00400000u : 0x3e800000u, d24 ? 0x00800000u : 0x3f000000u,
                              d24 ? 0x00ffffffu : 0x3f800000u};
  const uint8_t stencil[4] = {0x12, 0x34, 0x56, 0x78};
  for (unsigned x = 0; x < 4; x++) {
    std::memcpy(src + f[0].Offset + x * 4, &packed[x], 4);
    src[f[1].Offset + x] = stencil[x];
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT wideFootprints[2]{};
  dev->GetCopyableFootprints(&td, 0, 1, 1024, wideFootprints, nullptr, nullptr, nullptr);
  dev->GetCopyableFootprints(&td, 4, 1, 2048, wideFootprints + 1, nullptr, nullptr, nullptr);
  for (unsigned i = 0; i < 2; i++) {
    const auto &footprint = wideFootprints[i];
    if (footprint.Offset != (i ? 2048u : 1024u) ||
        footprint.Footprint.Format != (i ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS) ||
        footprint.Footprint.Width != 8 || footprint.Footprint.Height != 2 || footprint.Footprint.RowPitch != 256)
      check(E_FAIL, "wide footprint contract");
  }
  const uint32_t depth24[8] = {0, 1, 0x123456, 0x7fffff, 0xfffffe, 0xffffff, 0x654321, 0x800001};
  const uint8_t stencilWide[8] = {0x11, 0x23, 0x35, 0x47, 0x59, 0x6b, 0x7d, 0x8f};
  const uint32_t depth32[8] = {0, 0x3e800000, 0x3f000000, 0x3f800000, 0x3e000000, 0x3f400000, 0x3e800000, 0x3f000000};
  for (unsigned y = 0; y < 2; y++)
    for (unsigned x = 0; x < 8; x++) {
      const unsigned i = (x + y * 3) % 8;
      const uint32_t bits = d24 ? depth24[i] : depth32[i];
      std::memcpy(src + wideFootprints[0].Offset + y * 256 + x * 4, &bits, 4);
      src[wideFootprints[1].Offset + y * 256 + x] = stencilWide[i];
    }
  upload->Unmap(0, nullptr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)), "dsv heap");
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = cv.Format;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
  dsv.Texture2DArray.MipSlice = 1;
  dsv.Texture2DArray.FirstArraySlice = 1;
  dsv.Texture2DArray.ArraySize = 1;
  auto dsvh = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  dev->CreateDepthStencilView(cleared.Get(), &dsv, dsvh);
  D3D12_DESCRIPTOR_HEAP_DESC shd{};
  shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  shd.NumDescriptors = 1;
  shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srvHeap)), "srv heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = d24 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2DArray.MostDetailedMip = 1;
  srv.Texture2DArray.MipLevels = 1;
  srv.Texture2DArray.FirstArraySlice = 1;
  srv.Texture2DArray.ArraySize = 1;
  if (useSrv)
    dev->CreateShaderResourceView(uploaded.Get(), &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER rp[2]{};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  rp[0].DescriptorTable.NumDescriptorRanges = 1;
  rp[0].DescriptorTable.pDescriptorRanges = &range;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  rp[1].Descriptor.ShaderRegister = 0;
  rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 2;
  rsd.pParameters = rp;
  ComPtr<ID3DBlob> blob, error;
  check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "serialize root");
  ComPtr<ID3D12RootSignature> root;
  check(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
  const char *hlsl = "Texture2DArray<float> depthTex:register(t0); RWByteAddressBuffer outBuf:register(u0); "
                     "[numthreads(4,1,1)] void main(uint3 tid:SV_DispatchThreadID) { float "
                     "d=depthTex.Load(int4(tid.x,0,0,0)); outBuf.Store(tid.x*4,asuint(d)); }";
  blob.Reset();
  error.Reset();
  HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &error);
  if (FAILED(hr) && error)
    std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr);
  check(hr, "compile shader");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = root.Get();
  pd.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "compute pso");
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> alloc;
  check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  const float depths[4] = {0, .1f, .3f, .9f};
  for (unsigned x = 0; x < 4; x++) {
    D3D12_RECT rect{LONG(x), 0, LONG(x + 1), 1};
    list->ClearDepthStencilView(dsvh, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depths[x], stencil[x], 1,
                                &rect);
  }
  barrier(list.Get(), cleared.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (unsigned i = 0; i < 2; i++) {
    auto a = sub(cleared.Get(), i ? 7 : 3), b = placed(readClear.Get(), f[i]);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  for (unsigned i = 0; i < 2; i++) {
    auto a = placed(upload.Get(), f[i]), b = sub(uploaded.Get(), i ? 7 : 3);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  barrier(list.Get(), uploaded.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (unsigned i = 0; i < 2; i++) {
    auto a = sub(uploaded.Get(), i ? 7 : 3), b = placed(readUpload.Get(), f[i]);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  list->CopyBufferRegion(readWide.Get(), 0, upload.Get(), 0, 3072);
  for (unsigned i = 0; i < 2; i++) {
    auto a = placed(upload.Get(), wideFootprints[i]), b = sub(wide.Get(), i ? 4 : 0);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  barrier(list.Get(), wide.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (unsigned i = 0; i < 2; i++) {
    auto a = sub(wide.Get(), i ? 4 : 0), b = placed(readWide.Get(), wideFootprints[i]);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  for (unsigned i = 0; i < 2; i++) {
    auto a = sub(uploaded.Get(), i ? 7 : 3), b = sub(copied.Get(), i ? 7 : 3);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  barrier(list.Get(), copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (unsigned i = 0; i < 2; i++) {
    auto a = sub(copied.Get(), i ? 7 : 3), b = placed(readCopied.Get(), f[i]);
    list->CopyTextureRegion(&b, 0, 0, 0, &a, nullptr);
  }
  barrier(list.Get(), uploaded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (useSrv) {
    list->SetPipelineState(pso.Get());
    list->SetComputeRootSignature(root.Get());
    ID3D12DescriptorHeap *sh[] = {srvHeap.Get()};
    list->SetDescriptorHeaps(1, sh);
    list->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, gpuOut->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    barrier(list.Get(), gpuOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readShader.Get(), 0, gpuOut.Get(), 0, 16);
  }
  check(list->Close(), "close list");
  ID3D12CommandList *execute[] = {list.Get()};
  queue->ExecuteCommandLists(1, execute);
  ComPtr<ID3D12Fence> fence;
  check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  check(queue->Signal(fence.Get(), 1), "signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(fence->SetEventOnCompletion(1, event), "completion");
  DWORD wait = WaitForSingleObject(event, 30000);
  CloseHandle(event);
  if (wait != WAIT_OBJECT_0) {
    std::fprintf(stderr, "OWNED_D3D12_DEPTH_PLANE_COPY_FAIL wait=%lu removed=%08lx\n", wait,
                 (unsigned long)dev->GetDeviceRemovedReason());
    std::exit(3);
  }
  const uint8_t *ptr{};
  check(readClear->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&ptr))), "map clear");
  inspect("CLEAR", ptr, f, format);
  readClear->Unmap(0, nullptr);
  check(readUpload->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&ptr))), "map upload");
  inspect("UPLOAD", ptr, f, format);
  readUpload->Unmap(0, nullptr);
  check(readCopied->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&ptr))), "map copied");
  inspect("COPIED", ptr, f, format);
  readCopied->Unmap(0, nullptr);
  check(readWide->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&ptr))), "map wide");
  for (unsigned i = 0; i < 2; i++) {
    for (unsigned y = 0; y < 2; y++) {
      const UINT64 base = wideFootprints[i].Offset + y * 256;
      for (unsigned x = 0; x < 8; x++) {
        const unsigned index = (x + y * 3) % 8;
        if (i) {
          if (ptr[base + x] != stencilWide[index])
            check(E_FAIL, "stencil wide rows");
        } else {
          uint32_t value;
          std::memcpy(&value, ptr + base + x * 4, 4);
          if (value != (d24 ? depth24[index] : depth32[index])) {
            std::fprintf(stderr,
                         "OWNED_D3D12_DEPTH_PLANE_COPY_FAIL depth-wide format=%u x=%u y=%u actual=%08x expected=%08x\n",
                         unsigned(format), x, y, value, d24 ? depth24[index] : depth32[index]);
            std::exit(1);
          }
        }
      }
      if (ptr[base + (i ? 8 : 32)] != 0xa5 || ptr[base + 255] != 0xa5)
        check(E_FAIL, "row padding guard");
    }
    if (ptr[wideFootprints[i].Offset - 1] != 0xa5)
      check(E_FAIL, "placed offset guard");
  }
  readWide->Unmap(0, nullptr);
  if (useSrv) {
    check(readShader->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&ptr))), "map shader");
    for (unsigned x = 0; x < 4; x++) {
      float value;
      std::memcpy(&value, ptr + x * 4, 4);
      if (std::fabs(value - float(x == 0 ? 0 : x == 1 ? .25 : x == 2 ? .5 : 1)) > 0.000001f) {
        std::fprintf(stderr, "OWNED_D3D12_DEPTH_PLANE_COPY_FAIL shader-depth format=%u x=%u value=%g\n",
                     unsigned(format), x, value);
        std::exit(1);
      }
    }
    readShader->Unmap(0, nullptr);
  }
  std::printf("OWNED_D3D12_DEPTH_PLANE_COPY_CASE_PASS format=%u removed=%08lx\n", unsigned(format),
              (unsigned long)dev->GetDeviceRemovedReason());
}

int main() {
  runCase(DXGI_FORMAT_R24G8_TYPELESS);
  runCase(DXGI_FORMAT_D24_UNORM_S8_UINT);
  runCase(DXGI_FORMAT_R32G8X24_TYPELESS);
  runCase(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
  std::puts("OWNED_D3D12_DEPTH_PLANE_COPY_PASS cases=4 depth=packed24 stencil=exact controls=d32s8");
}