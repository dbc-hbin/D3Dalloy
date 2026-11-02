// Shader Model 6.x GPU fixture: every DXIL entry of tests/dxil/sm6*.hlsl runs on the
// owned D3D12 device and is checked against an independent CPU oracle.
// usage: owned-d3d12-sm6.exe [--stress-reps N]   (frozen DXIL/DXBC files beside the exe)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";
constexpr DWORD kFenceTimeoutMs = 60000;
constexpr uint32_t kSentinel = 0xcdcdcdcdu;

[[noreturn]] void die(const char *step, HRESULT hr = E_FAIL) {
  std::printf("OWNED_D3D12_SM6_FAIL step=%s hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
  std::fflush(stdout);
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    die(step, hr);
}

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    die("CreateFileW(shader)", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
    CloseHandle(file);
    die("GetFileSizeEx(shader)");
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != data.size())
    die("ReadFile(shader)");
  return data;
}

std::vector<uint8_t> shader(const char *entry) {
  std::wstring name = L"sm6-";
  for (const char *c = entry; *c; ++c)
    name += static_cast<wchar_t>(*c);
  if (!std::strchr(entry, '.')) name += L".dxil";
  return readFile(name.c_str());
}

/* ---- half precision reference (IEEE binary16, round to nearest even) ---- */
float halfToFloat(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1f;
  const uint32_t mantissa = h & 0x3ff;
  float value;
  if (exponent == 0)
    value = std::ldexp(static_cast<float>(mantissa), -24);
  else if (exponent == 31)
    value = mantissa ? NAN : INFINITY;
  else
    value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  bits |= sign;
  std::memcpy(&value, &bits, 4);
  return value;
}

uint16_t floatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
  const float magnitude = std::fabs(f);
  if (std::isnan(f))
    return sign | 0x7e00;
  if (magnitude >= 65520.0f)
    return sign | 0x7c00;
  if (magnitude < std::ldexp(1.0f, -14)) {
    /* subnormal: units of 2^-24, round half to even */
    const float units = magnitude * 16777216.0f;
    return sign | static_cast<uint16_t>(std::nearbyint(units));
  }
  int exponent;
  std::frexp(magnitude, &exponent); /* magnitude = m * 2^exponent, m in [0.5, 1) */
  const float scaled = std::ldexp(magnitude, 11 - exponent); /* [1024, 2048) */
  uint32_t mantissa = static_cast<uint32_t>(std::nearbyint(scaled));
  int e = exponent - 1 + 15;
  if (mantissa == 2048) {
    mantissa = 1024;
    ++e;
  }
  if (e >= 31)
    return sign | 0x7c00;
  return sign | static_cast<uint16_t>((e << 10) | (mantissa & 0x3ff));
}

uint32_t floatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

float bitsFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

template <typename T> T load(const std::vector<uint8_t> &bytes, size_t offset) {
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T> void put(std::vector<uint8_t> &bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

/* ---- D3D12 plumbing ---- */
D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = type;
  props.CreationNodeMask = 1;
  props.VisibleNodeMask = 1;
  return props;
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12DescriptorHeap> heap;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  UINT descriptorSize = 0;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;
  bool submissionDiagnostics = false;
  std::vector<ComPtr<ID3D12Resource>> keepAlive;

  explicit Gpu(ID3D12Device *d) : device(d) {
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "CreateCommandList");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 16;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap(RTV)");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      die("CreateEventW");
  }
  ~Gpu() { CloseHandle(event); }

  D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot) {
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};heap->GetCPUDescriptorHandleForHeapStart(&handle);
#else
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
#endif
    handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
    return handle;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot) {
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};heap->GetGPUDescriptorHandleForHeapStart(&handle);
#else
    auto handle = heap->GetGPUDescriptorHandleForHeapStart();
#endif
    handle.ptr += static_cast<UINT64>(slot) * descriptorSize;
    return handle;
  }

  ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    const auto props = heapProperties(type);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(buffer)");
    return resource;
  }

  /* A UAV-capable default buffer in COPY_DEST whose initial contents are `data`; the copy is
   * recorded on the current list. */
  ComPtr<ID3D12Resource> uavBuffer(const std::vector<uint8_t> &data) {
    auto resource = buffer(data.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto upload = buffer(data.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(upload->Map(0, nullptr, &mapped), "Map(upload)");
    std::memcpy(mapped, data.data(), data.size());
    upload->Unmap(0, nullptr);
    list->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, data.size());
    transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    keepAlive.push_back(upload);
    return resource;
  }

  ComPtr<ID3D12Resource> constants(const void *data, size_t size) {
    auto upload = buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(upload->Map(0, nullptr, &mapped), "Map(constants)");
    std::memset(mapped, 0, 256);
    std::memcpy(mapped, data, size);
    upload->Unmap(0, nullptr);
    return upload;
  }

  void transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
  }

  /* Records UAV -> readback copies for `resources`, submits, waits, and returns their bytes. */
  std::vector<std::vector<uint8_t>> finish(std::initializer_list<ID3D12Resource *> resources, double *elapsed_ms) {
    std::vector<ComPtr<ID3D12Resource>> readbacks;
    for (auto *resource : resources) {
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
      D3D12_RESOURCE_DESC resourceDesc{};resource->GetDesc(&resourceDesc);
      const UINT64 size = resourceDesc.Width;
#else
      const UINT64 size = resource->GetDesc().Width;
#endif
      transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      auto readback = buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(readback.Get(), 0, resource, 0, size);
      readbacks.push_back(readback);
    }
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY close=begin\n");
    const HRESULT closeResult = list->Close();
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY close=returned hr=%08lx\n", static_cast<unsigned long>(closeResult));
    check(closeResult, "CommandList::Close");
    LARGE_INTEGER start{}, end{}, frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    ID3D12CommandList *lists[] = {list.Get()};
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY execute=begin\n");
    queue->ExecuteCommandLists(1, lists);
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY execute=queued\n");
    const UINT64 value = ++fenceValue;
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY signal=begin value=%llu\n", static_cast<unsigned long long>(value));
    const HRESULT signalResult = queue->Signal(fence.Get(), value);
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY signal=returned hr=%08lx value=%llu\n", static_cast<unsigned long>(signalResult), static_cast<unsigned long long>(value));
    check(signalResult, "Queue::Signal");
    if (fence->GetCompletedValue() < value) {
      if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY event=begin\n");
      check(fence->SetEventOnCompletion(value, event), "Fence::SetEventOnCompletion");
      if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY wait=begin timeout_ms=%lu\n", static_cast<unsigned long>(kFenceTimeoutMs));
      const DWORD waitResult = WaitForSingleObject(event, kFenceTimeoutMs);
      if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY wait=returned status=%lu observed=%llu target=%llu\n", static_cast<unsigned long>(waitResult), static_cast<unsigned long long>(fence->GetCompletedValue()), static_cast<unsigned long long>(value));
      if (waitResult != WAIT_OBJECT_0)
        die("GPU fence timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    if (submissionDiagnostics) std::printf("OWNED_D3D12_SUBMISSION_BOUNDARY fence=observed value=%llu\n", static_cast<unsigned long long>(fence->GetCompletedValue()));
    QueryPerformanceCounter(&end);
    if (elapsed_ms)
      *elapsed_ms = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
    std::vector<std::vector<uint8_t>> result;
    for (auto &readback : readbacks) {
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
      D3D12_RESOURCE_DESC resourceDesc{};readback->GetDesc(&resourceDesc);
      const UINT64 size = resourceDesc.Width;
#else
      const UINT64 size = readback->GetDesc().Width;
#endif
      D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
      void *mapped = nullptr;
      check(readback->Map(0, &range, &mapped), "Map(readback)");
      result.emplace_back(static_cast<const uint8_t *>(mapped), static_cast<const uint8_t *>(mapped) + size);
      readback->Unmap(0, nullptr);
    }
    check(allocator->Reset(), "Allocator::Reset");
    check(list->Reset(allocator.Get(), nullptr), "List::Reset");
    keepAlive.clear();
    return result;
  }

  void rawUav(UINT slot, ID3D12Resource *resource) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
    D3D12_RESOURCE_DESC resourceDesc{};resource->GetDesc(&resourceDesc);
    desc.Buffer.NumElements = static_cast<UINT>(resourceDesc.Width / 4);
#else
    desc.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / 4);
#endif
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(resource, nullptr, &desc, cpu(slot));
  }
  void structuredUav(UINT slot, ID3D12Resource *resource, UINT stride) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
    D3D12_RESOURCE_DESC resourceDesc{};resource->GetDesc(&resourceDesc);
    desc.Buffer.NumElements = static_cast<UINT>(resourceDesc.Width / stride);
#else
    desc.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / stride);
#endif
    desc.Buffer.StructureByteStride = stride;
    device->CreateUnorderedAccessView(resource, nullptr, &desc, cpu(slot));
  }
};

ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device *device, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &desc) {
  ComPtr<ID3DBlob> blob, error;
  HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
  if (FAILED(hr)) {
    if (error)
      std::fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stdout);
    die("D3D12SerializeVersionedRootSignature", hr);
  }
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "CreateRootSignature");
  return root;
}

/* Compute layout: b0 constants, b1 CBV, u0..u2, t0..t6, point s0 and mip-linear s1. */
ComPtr<ID3D12RootSignature> computeRoot(ID3D12Device *device,
                                       D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                       float bias = 0, float minLod = 0,
                                       float maxLod = D3D12_FLOAT32_MAX, UINT anisotropy = 1) {
  static D3D12_DESCRIPTOR_RANGE1 uavs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0,
                                      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE |
                                          D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
                                      0};
  static D3D12_DESCRIPTOR_RANGE1 srvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0,
                                      D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 params[4]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants = {0, 0, 4};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[1].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable = {1, &uavs};
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable = {1, &srvs};
  for (auto &param : params)
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {sampler, sampler};
  samplers[1].ShaderRegister = 1;
  samplers[1].Filter = filter;
  samplers[1].MipLODBias = bias;
  samplers[1].MinLOD = minLod;
  samplers[1].MaxLOD = maxLod;
  samplers[1].MaxAnisotropy = anisotropy;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 4;
  desc.Desc_1_1.pParameters = params;
  desc.Desc_1_1.NumStaticSamplers = 2;
  desc.Desc_1_1.pStaticSamplers = samplers;
  return rootSignature(device, desc);
}

/* Graphics layout: b0 root constants (4) and table u0..u1, visible to all stages. */
ComPtr<ID3D12RootSignature> graphicsRoot(ID3D12Device *device) {
  static D3D12_DESCRIPTOR_RANGE1 uavs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0,
                                      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE |
                                          D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
                                      0};
  D3D12_ROOT_PARAMETER1 params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants = {0, 0, 4};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable = {1, &uavs};
  params[0].ShaderVisibility = params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 2;
  desc.Desc_1_1.pParameters = params;
  return rootSignature(device, desc);
}

HRESULT computePso(ID3D12Device *device, ID3D12RootSignature *root, const std::vector<uint8_t> &cs,
                   ComPtr<ID3D12PipelineState> &pso) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.CS = {cs.data(), cs.size()};
  pso.Reset();
  return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
}

/* ---- case bookkeeping ---- */
int g_failures = 0;
int g_case_mismatches = 0;

void mismatch(const char *name, const char *what, size_t index, uint64_t actual, uint64_t expected) {
  if (g_case_mismatches++ < 64)
    std::printf("OWNED_D3D12_SM6_MISMATCH case=%s field=%s index=%zu actual=0x%llx expected=0x%llx\n", name, what,
                index, static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
}

void begin() { g_case_mismatches = 0; }

void end(const char *name) {
  if (g_case_mismatches) {
    std::printf("OWNED_D3D12_SM6_CASE_FAIL %s mismatches=%d\n", name, g_case_mismatches);
    ++g_failures;
  } else {
    std::printf("OWNED_D3D12_SM6_CASE_PASS %s\n", name);
  }
  std::fflush(stdout);
}

#define EXPECT_EQ(name, what, index, actual, expected)                                                                 \
  do {                                                                                                                 \
    const uint64_t a_ = static_cast<uint64_t>(actual), e_ = static_cast<uint64_t>(expected);                          \
    if (a_ != e_)                                                                                                      \
      mismatch(name, what, index, a_, e_);                                                                             \
  } while (0)

struct Compute {
  Gpu &gpu;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12Resource> texture;

  /* Dispatches `entry` with u0 = out, u1 = raw, u2 = s64 (8 byte structured) and returns their bytes. */
  std::vector<std::vector<uint8_t>> run(const char *entry, std::vector<uint8_t> out, std::vector<uint8_t> raw,
                                        std::vector<uint8_t> s64, std::array<uint32_t, 4> p0, UINT groups,
                                        const void *cb1 = nullptr, size_t cb1_size = 0, double *elapsed = nullptr,
                                        UINT structure_stride = 8) {
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(computePso(gpu.device.Get(), root.Get(), shader(entry), pso))) {
      std::printf("OWNED_D3D12_SM6_MISMATCH case=%s field=CreateComputePipelineState\n", entry);
      ++g_case_mismatches;
      return {};
    }
    auto u0 = gpu.uavBuffer(out), u1 = gpu.uavBuffer(raw), u2 = gpu.uavBuffer(s64);
    gpu.rawUav(0, u0.Get());
    gpu.rawUav(1, u1.Get());
    gpu.structuredUav(2, u2.Get(), structure_stride);
    auto cb = gpu.constants(cb1 ? cb1 : p0.data(), cb1 ? cb1_size : 16);
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetPipelineState(pso.Get());
    gpu.list->SetComputeRootSignature(root.Get());
    gpu.list->SetComputeRoot32BitConstants(0, 4, p0.data(), 0);
    gpu.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
    gpu.list->SetComputeRootDescriptorTable(2, gpu.gpu(0));
    gpu.list->SetComputeRootDescriptorTable(3, gpu.gpu(3));
    gpu.list->Dispatch(groups, 1, 1);
    auto result = gpu.finish({u0.Get(), u1.Get(), u2.Get()}, elapsed);
    return result;
  }
};

std::vector<uint8_t> filled(size_t size, uint32_t word = kSentinel) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i + 4 <= size; i += 4)
    put(bytes, i, word);
  return bytes;
}

/* ---- cs_wave: every reduction kind, signed vs unsigned min/max with negative values ---- */
void testWave(Compute &c, const char *entry = "cs_wave", uint32_t width = 32, const char *name = "wave") {
  begin();
  auto r = c.run(entry, filled(64 * 128), filled(64), filled(64), {0, 0, 0, 0}, 1);
  if (!r.empty()) {
    const auto &o = r[0];
    auto s = [](uint32_t gi) { return static_cast<int32_t>(gi * 2654435761u) >> 8; };
    auto u = [](uint32_t gi) { return gi * 2246822519u; };
    auto f = [](uint32_t gi) { return static_cast<float>(static_cast<int>(gi % 7) - 3) * 0.5f; };
    for (uint32_t gi = 0; gi < 64; ++gi) {
      const uint32_t lane = gi % width, first = gi - lane;
      uint32_t ballot = 0, sum = 0, product = 1, band = ~0u, bor = 0, bxor = 0, count4 = 0;
      int32_t smin = INT32_MAX, smax = INT32_MIN;
      uint32_t umin = ~0u, umax = 0, psum = 0, pproduct = 1, pcount = 0;
      float fsum = 0.0f, fmin = INFINITY;
      for (uint32_t l = 0; l < width; ++l) {
        const uint32_t g = first + l;
        ballot |= ((g % 3) == 0) << l;
        sum += u(g);
        product *= g | 1;
        smin = std::min(smin, s(g));
        smax = std::max(smax, s(g));
        umin = std::min(umin, u(g));
        umax = std::max(umax, u(g));
        band &= u(g) | 0x10000000u;
        bor |= u(g) & 0xff00ffu;
        bxor ^= u(g);
        count4 += (u(g) & 4) != 0;
        fsum += f(g);
        fmin = std::min(fmin, f(g));
        if (l < lane) {
          psum += u(g);
          pproduct *= (g & 3) + 1;
          pcount += (u(g) & 8) != 0;
        }
      }
      const size_t b = gi * 128;
      const uint32_t expected[32] = {width, lane, lane == 0, 1, first <= 37 && first + width > 37,
                                     ballot, 0, 0, 0, u(first + 5 % width), u(first), sum,
                                     product, static_cast<uint32_t>(smin), static_cast<uint32_t>(smax), umin, umax,
                                     band, bor, bxor, count4, psum, pproduct, pcount, floatBits(fsum),
                                     floatBits(fmin), gi ^ 1, gi ^ 2, gi ^ 3, (gi & ~3u) | 2, 1, 0};
      static const char *names[32] = {"lanecount", "laneindex", "isfirst", "alltrue", "anytrue", "ballot.x",
                                      "ballot.y", "ballot.z", "ballot.w", "readlaneat", "readfirst", "sum.u",
                                      "product", "min.s", "max.s", "min.u", "max.u", "and", "or", "xor",
                                      "countbits", "prefixsum", "prefixproduct", "prefixcount", "sum.f", "min.f",
                                      "quadx", "quady", "quaddiag", "quadlane2", "allequal", "allequal.differ"};
      for (uint32_t k = 0; k < 32; ++k)
        EXPECT_EQ(name, names[k], gi, load<uint32_t>(o, b + k * 4), expected[k]);
    }
  }
  end(name);
}

/* ---- cs_divergent: wave ops, WaveMatch and WaveMultiPrefix* under divergence ---- */
void testDivergent(Compute &c, const char *entry = "cs_divergent", uint32_t width = 32,
                   const char *name = "divergent") {
  begin();
  auto r = c.run(entry, filled(64 * 64), filled(64), filled(64), {3, 0, 0, 0}, 1);
  if (!r.empty()) {
    const auto &o = r[0];
    auto s = [](uint32_t gi) { return static_cast<int32_t>(gi * 2654435761u) >> 8; };
    auto u = [](uint32_t gi) { return gi * 2246822519u; };
    auto active = [](uint32_t gi) { return (gi % 3) != 0; };
    for (uint32_t gi = 0; gi < 64; ++gi) {
      const size_t b = gi * 64;
      if (!active(gi)) {
        for (uint32_t k = 0; k < 16; ++k)
          EXPECT_EQ(name, "inactive", gi, load<uint32_t>(o, b + k * 4), 0xdeadbeefu);
        continue;
      }
      const uint32_t lane = gi % width, first = gi - lane;
      uint32_t count = 0, lowest = width, sum = 0, umax = 0, psum = 0, pcount = 0, ballot = 0, match = 0;
      int32_t smin = INT32_MAX;
      uint32_t mpsum = 0, mpcount = 0, mpproduct = 1, mpor = 0, mpxor = 0, mpand = ~0u;
      for (uint32_t l = 0; l < width; ++l) {
        const uint32_t g = first + l;
        if (!active(g))
          continue;
        ++count;
        lowest = std::min(lowest, l);
        sum += u(g);
        smin = std::min(smin, s(g));
        umax = std::max(umax, u(g));
        ballot |= 1u << l;
        const bool same = (g % 5) == (gi % 5);
        if (same)
          match |= 1u << l;
        if (l < lane) {
          psum += g;
          ++pcount;
          if (same) {
            mpsum += g;
            mpcount += (g & 1) != 0;
            mpproduct *= (g & 3) + 1;
            mpor |= 1u << (g & 31);
            mpxor ^= u(g);
            mpand &= ~(1u << (g & 31));
          }
        }
      }
      const uint32_t expected[16] = {count, lane == lowest, first + lowest, sum, static_cast<uint32_t>(smin), umax,
                                     psum, pcount, ballot, match, mpsum, mpcount, mpproduct, mpor, mpxor, mpand};
      static const char *names[16] = {"count", "isfirst", "readfirst", "sum", "min.s", "max.u", "prefixsum",
                                      "prefixcount", "ballot", "match", "multiprefix.sum", "multiprefix.count",
                                      "multiprefix.product", "multiprefix.or", "multiprefix.xor", "multiprefix.and"};
      for (uint32_t k = 0; k < 16; ++k)
        EXPECT_EQ(name, names[k], gi, load<uint32_t>(o, b + k * 4), expected[k]);
    }
  }
  end(name);
}

/* ---- cs_int64 ---- */
uint64_t reverse64(uint64_t v) {
  uint64_t r = 0;
  for (int i = 0; i < 64; ++i)
    r |= ((v >> i) & 1) << (63 - i);
  return r;
}

void testInt64(Compute &c, const char *entry = "cs_int64", uint32_t width = 32, const char *name = "int64") {
  begin();
  std::vector<uint8_t> raw(64 * 8);
  std::vector<int64_t> in(64);
  const int64_t edges[8] = {INT64_MIN, -1, 0, INT64_MAX, 1, static_cast<int64_t>(0x00000000ffffffffull),
                            static_cast<int64_t>(0xffffffff00000000ull), -7};
  uint64_t seed = 0x9e3779b97f4a7c15ull;
  for (uint32_t i = 0; i < 64; ++i) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    in[i] = i < 8 ? edges[i] : static_cast<int64_t>(seed ^ (seed >> 29));
    put(raw, i * 8, in[i]);
  }
  auto r = c.run(entry, filled(64 * 128), raw, filled(64), {0, 0, 0, 0}, 1);
  if (!r.empty()) {
    const auto &o = r[0];
    for (uint32_t gi = 0; gi < 64; ++gi) {
      const int64_t a = in[gi], b = in[(gi * 7 + 3) & 63];
      const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
      const uint32_t shift = (gi * 13) & 63;
      const int64_t bd = (b == 0 || (a == INT64_MIN && b == -1)) ? 3 : b;
      const uint64_t ubd = ub == 0 ? 5 : ub;
      const uint64_t hi = ua ? static_cast<uint64_t>(63 - __builtin_clzll(ua)) : 0xffffffffull;
      const uint64_t lo = ua ? static_cast<uint64_t>(__builtin_ctzll(ua)) : 0xffffffffull;
      const uint32_t lane = gi % width, first = gi - lane;
      uint64_t wsum = 0, wmax = 0, wxor = 0, prefix = 0;
      int64_t wmin = INT64_MAX;
      for (uint32_t l = 0; l < width; ++l) {
        const uint32_t g = first + l;
        const uint64_t x = static_cast<uint64_t>(in[g]);
        wsum += x;
        wmax = std::max(wmax, x);
        wmin = std::min(wmin, in[g]);
        wxor ^= static_cast<uint64_t>(in[(g * 7 + 3) & 63]);
        if (l < lane)
          prefix += x;
      }
      const uint64_t expected[16] = {
          ua + ub,
          ua - ub,
          ua * ub,
          static_cast<uint64_t>(a / bd),
          static_cast<uint64_t>(a % bd),
          ua / ubd,
          ua % ubd,
          static_cast<uint64_t>(a >> shift),
          ua >> shift,
          ua << shift,
          static_cast<uint64_t>(std::min(a, b) ^ std::max(a, b)),
          std::min(ua, ub) + static_cast<uint64_t>(__builtin_popcountll(ua)) + hi + lo + reverse64(ua),
          static_cast<uint64_t>(a >> 32) + (ua & 0xffffffffull) + (ua & 0xffffull),
          wsum ^ wmax,
          static_cast<uint64_t>(wmin) + prefix,
          static_cast<uint64_t>(in[first + 7 % width]) + wxor,
      };
      static const char *names[16] = {"add", "sub", "mul", "sdiv", "srem", "udiv", "urem", "ashr", "lshr", "shl",
                                      "minmax.s", "bits", "convert", "wave.sum^max", "wave.min+prefix",
                                      "readlane+xor"};
      for (uint32_t k = 0; k < 16; ++k)
        EXPECT_EQ(name, names[k], gi, load<uint64_t>(o, gi * 128 + k * 8), expected[k]);
    }
  }
  end(name);
}

/* ---- cs_atomic64_ops: single-thread semantics and edge values of every 64 bit atomic ---- */
void testAtomic64Ops(Compute &c) {
  begin();
  std::vector<uint8_t> raw(4096, 0);
  const uint64_t init[12] = {0x0000000100000001ull, ~0ull, 0x0000000100000000ull, 0x0123456789abcdefull,
                             3, static_cast<uint64_t>(-7ll), 0xfffffffffffffff0ull, 0x7fffffffffffffffull,
                             0x1111111111111111ull, 7, 9, 0x100000000ull};
  for (uint32_t i = 0; i < 12; ++i)
    put(raw, i * 8, init[i]);
  std::vector<uint8_t> s64(4 * 8, 0);
  put(s64, 8, static_cast<int64_t>(-3));
  put(s64, 16, INT64_MIN);
  auto r = c.run("cs_atomic64_ops", filled(128), raw, s64, {0, 0, 0, 0}, 1);
  if (!r.empty()) {
    const auto &o = r[0], &m = r[1], &s = r[2];
    const uint64_t returned[10] = {init[0], init[1], init[2], init[3], init[6], init[7], init[8], init[9], init[10], 0};
    for (uint32_t i = 0; i < 10; ++i)
      EXPECT_EQ("atomic64_ops", "returned", i, load<uint64_t>(o, i * 8), returned[i]);
    const int64_t sreturned[4] = {3, -7, -3, INT64_MIN};
    for (uint32_t i = 0; i < 4; ++i)
      EXPECT_EQ("atomic64_ops", "returned.signed", i, load<int64_t>(o, 80 + i * 8), sreturned[i]);
    const uint64_t final[12] = {0x0000000200000000ull, 0xf0f0f0f00f0f0f0full, 0x8000000100000001ull,
                                ~0x0123456789abcdefull, static_cast<uint64_t>(-5ll), static_cast<uint64_t>(-7ll), 3,
                                0xfffffffffffffffeull, 0x123456789abcdef0ull, 8, 9, 42};
    for (uint32_t i = 0; i < 12; ++i)
      EXPECT_EQ("atomic64_ops", "memory", i, load<uint64_t>(m, i * 8), final[i]);
    for (size_t i = 96; i < m.size(); ++i)
      EXPECT_EQ("atomic64_ops", "untouched", i, m[i], 0);
    EXPECT_EQ("atomic64_ops", "structured.max", 1, load<int64_t>(s, 8), -1);
    EXPECT_EQ("atomic64_ops", "structured.add", 2, load<int64_t>(s, 16), INT64_MAX);
    EXPECT_EQ("atomic64_ops", "structured.other", 0, load<int64_t>(s, 0) | load<int64_t>(s, 24), 0);
  }
  end("atomic64_ops");
}

/* ---- contended compute 64 bit atomics (cs_atomic64 / atomic64Body) ---- */
constexpr uint32_t kStressThreads = 64 * 1024;

uint32_t stressTarget(uint32_t id, uint32_t targets) { return ((id * 2654435761u) >> 7) % targets; }

/* Exact final values, and exact multiset properties of the returned values. */
bool checkStress(const char *name, const std::vector<uint8_t> &out, const std::vector<uint8_t> &raw,
                 uint32_t targets) {
  const int before = g_case_mismatches;
  std::vector<std::vector<uint8_t>> seen(targets);
  std::vector<uint64_t> count(targets, 0), max(targets, 0), bits(targets, 0);
  for (uint32_t id = 0; id < kStressThreads; ++id) {
    const uint32_t t = stressTarget(id, targets);
    ++count[t];
    if ((id & 3) == 3)
      max[t] = std::max<uint64_t>(max[t], static_cast<uint64_t>(id) << 20);
    if ((id & 3) == 1)
      bits[t] |= 1ull << (id & 63);
  }
  for (uint32_t t = 0; t < targets; ++t)
    seen[t].assign(count[t], 0);
  std::vector<uint32_t> cas_zero(targets, 0);
  std::vector<uint64_t> cas_winner(targets, ~0ull);
  for (uint32_t id = 0; id < kStressThreads; ++id) {
    const uint32_t t = stressTarget(id, targets);
    const uint64_t old = load<uint64_t>(out, id * 16), other = load<uint64_t>(out, id * 16 + 8);
    if (old >= count[t] || seen[t][old]++)
      mismatch(name, "add.returned-not-a-permutation", id, old, count[t]);
    if ((id & 3) == 3) {
      /* A returned maximum is 0 or another contender's value on the same target. */
      const uint64_t other_id = other >> 20;
      if (other != 0 && ((other & 0xfffff) || other_id >= kStressThreads || (other_id & 3) != 3 ||
                         stressTarget(static_cast<uint32_t>(other_id), targets) != t || other_id == id))
        mismatch(name, "max.returned", id, other, max[t]);
    } else if ((id & 3) == 1) {
      if (other & ~bits[t])
        mismatch(name, "or.returned", id, other, bits[t]);
    } else {
      if (other == 0) {
        ++cas_zero[t];
        cas_winner[t] = id + 1ull;
      }
    }
  }
  for (uint32_t t = 0; t < targets; ++t) {
    EXPECT_EQ(name, "add.final", t, load<uint64_t>(raw, t * 8), count[t]);
    EXPECT_EQ(name, "max.final", t, load<uint64_t>(raw, 128 + t * 8), max[t]);
    EXPECT_EQ(name, "or.final", t, load<uint64_t>(raw, 256 + t * 8), bits[t]);
    EXPECT_EQ(name, "cas.winners", t, cas_zero[t], 1);
    EXPECT_EQ(name, "cas.final", t, load<uint64_t>(raw, 384 + t * 8), cas_winner[t]);
  }
  for (uint32_t id = 0; id < kStressThreads; id += 2) {
    const uint32_t t = stressTarget(id, targets);
    const uint64_t other = load<uint64_t>(out, id * 16 + 8);
    if (other != 0 && other != cas_winner[t])
      mismatch(name, "cas.returned", id, other, cas_winner[t]);
  }
  return g_case_mismatches == before;
}

/* One pipeline state stream subobject, pointer aligned as D3D12 requires. */
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Value value;
};

struct Graphics {
  Gpu &gpu;
  ComPtr<ID3D12RootSignature> root;
  /* Vertices per draw (point list when not 3). */
  UINT vertex_count = 3;

  /* Draws `count` vertices with vs/ps into a width x height RGBA8 target; u0 = out, u1 = raw. With `views`, the
   * pipeline is created from a stream carrying that VIEW_INSTANCING subobject and the list sets `view_mask`. */
  std::vector<std::vector<uint8_t>> draw(const char *vs, const char *ps, UINT width, UINT height,
                                         std::vector<uint8_t> out, std::vector<uint8_t> raw,
                                         std::array<uint32_t, 4> p0, bool indexed, UINT start_vertex,
                                         UINT start_instance, double *elapsed = nullptr,
                                         const D3D12_VIEW_INSTANCING_DESC *views = nullptr, UINT view_mask = 0) {
    const auto vs_code = shader(vs), ps_code = shader(ps);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs_code.data(), vs_code.size()};
    desc.PS = {ps_code.data(), ps_code.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType =
        vertex_count == 3 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    HRESULT created;
    if (views) {
      // The same pipeline as `desc`, as a stream with the VIEW_INSTANCING subobject.
      ComPtr<ID3D12Device2> device2;
      check(gpu.device.As(&device2), "QueryInterface(ID3D12Device2)");
      D3D12_RT_FORMAT_ARRAY formats{};
      formats.RTFormats[0] = desc.RTVFormats[0];
      formats.NumRenderTargets = 1;
      struct {
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
      } stream{{.value = desc.pRootSignature}, {.value = desc.VS},          {.value = desc.PS},
               {.value = desc.RasterizerState}, {.value = desc.DepthStencilState},
               {.value = desc.PrimitiveTopologyType}, {.value = formats}, {.value = *views}};
      const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
      created = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso));
    } else {
      created = gpu.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (FAILED(created)) {
      std::printf("OWNED_D3D12_SM6_MISMATCH case=%s field=CreateGraphicsPipelineState\n", ps);
      ++g_case_mismatches;
      return {};
    }
    const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC rt{};
    rt.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt.Width = width;
    rt.Height = height;
    rt.DepthOrArraySize = 1;
    rt.MipLevels = 1;
    rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt.SampleDesc.Count = 1;
    rt.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target;
    check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              nullptr, IID_PPV_ARGS(&target)),
          "CreateCommittedResource(RT)");
#ifdef D3DALLOY_MATRIX_PREVIEW_SDK_HEADER
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart(&rtv);
#else
    const auto rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
#endif
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto u0 = gpu.uavBuffer(out), u1 = gpu.uavBuffer(raw);
    gpu.rawUav(0, u0.Get());
    gpu.rawUav(1, u1.Get());
    ComPtr<ID3D12Resource> indices;
    if (indexed) {
      const uint16_t data[3] = {0, 1, 2};
      indices = gpu.constants(data, sizeof(data));
    }
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetPipelineState(pso.Get());
    if (views) {
      ComPtr<ID3D12GraphicsCommandList1> list1;
      check(gpu.list.As(&list1), "QueryInterface(ID3D12GraphicsCommandList1)");
      list1->SetViewInstanceMask(view_mask);
    }
    gpu.list->SetGraphicsRootSignature(root.Get());
    gpu.list->SetGraphicsRoot32BitConstants(0, 4, p0.data(), 0);
    gpu.list->SetGraphicsRootDescriptorTable(1, gpu.gpu(0));
    gpu.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    gpu.list->RSSetViewports(1, &viewport);
    gpu.list->RSSetScissorRects(1, &scissor);
    gpu.list->IASetPrimitiveTopology(vertex_count == 3 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                                       : D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    if (indexed) {
      D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), 6, DXGI_FORMAT_R16_UINT};
      gpu.list->IASetIndexBuffer(&ibv);
      gpu.list->DrawIndexedInstanced(3, 1, 0, static_cast<INT>(start_vertex), start_instance);
    } else {
      gpu.list->DrawInstanced(vertex_count, 1, start_vertex, start_instance);
    }
    return gpu.finish({u0.Get(), u1.Get()}, elapsed);
  }
};

void testStress(Compute &c, int reps) {
  {
    for (const uint32_t targets : {1u, 16u}) {
      char name[64];
      std::snprintf(name, sizeof(name), "cs_atomic64_stress_%u", targets);
      begin();
      double worst = 0.0, total = 0.0;
      int exact = 0;
      for (int rep = 0; rep < reps; ++rep) {
        double elapsed = 0.0;
        auto r = c.run("cs_atomic64", filled(kStressThreads * 16, 0), filled(512, 0), filled(64), {targets, 0, 0, 0},
                       kStressThreads / 1024, nullptr, 0, &elapsed);
        if (r.empty())
          break;
        std::printf("OWNED_D3D12_SM6_STRESS_REP case=%s rep=%d ms=%.1f\n", name, rep, elapsed);
        std::fflush(stdout);
        worst = std::max(worst, elapsed);
        total += elapsed;
        exact += checkStress(name, r[0], r[1], targets);
      }
      std::printf("OWNED_D3D12_SM6_STRESS case=%s threads=%u targets=%u reps=%d exact=%d worst_ms=%.1f mean_ms=%.1f\n",
                  name, kStressThreads, targets, reps, exact, worst, reps ? total / reps : 0.0);
      if (exact != reps)
        ++g_case_mismatches;
      end(name);
    }
  }
}

/* ---- cs_atomic64_heap: SM 6.6 ResourceDescriptorHeap 64 bit atomics ---- */
void testAtomic64Heap(Gpu &gpu) {
  begin();
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
  auto root = rootSignature(gpu.device.Get(), desc);
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(computePso(gpu.device.Get(), root.Get(), shader("cs_atomic64_heap"), pso))) {
    mismatch("atomic64_heap", "CreateComputePipelineState", 0, 0, 1);
  } else {
    auto out = gpu.uavBuffer(filled(64 * 8)), raw = gpu.uavBuffer(filled(64, 0));
    gpu.rawUav(0, out.Get());
    gpu.rawUav(1, raw.Get());
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetComputeRootSignature(root.Get());
    gpu.list->SetPipelineState(pso.Get());
    gpu.list->Dispatch(1, 1, 1);
    auto r = gpu.finish({out.Get(), raw.Get()}, nullptr);
    std::vector<uint8_t> seen(64, 0);
    for (uint32_t i = 0; i < 64; ++i) {
      const uint64_t old = load<uint64_t>(r[0], i * 8);
      const uint64_t k = old / 0x100000001ull;
      if (old % 0x100000001ull || k >= 64 || seen[k]++)
        mismatch("atomic64_heap", "returned-not-a-permutation", i, old, 0);
    }
    EXPECT_EQ("atomic64_heap", "final", 0, load<uint64_t>(r[1], 8), 64 * 0x100000001ull);
    EXPECT_EQ("atomic64_heap", "neighbour", 0, load<uint64_t>(r[1], 0) | load<uint64_t>(r[1], 16), 0);
  }
  end("atomic64_heap");
}

/* ---- cs_16bit: native half/int16/uint16 ---- */
void test16Bit(Compute &c) {
  begin();
  std::vector<uint8_t> raw(512, 0);
  std::vector<uint16_t> h(64), i16(64), u16(64);
  const uint16_t special[4] = {0x0001, 0x03ff, 0x8001, 0x7bff}; /* two subnormals, a negative one, 65504 */
  for (uint32_t i = 0; i < 64; ++i) {
    h[i] = i < 60 ? floatToHalf((static_cast<float>(i) - 30.0f) * 0.25f) : special[i - 60];
    const int16_t signed_values[4] = {-32768, 32767, -1, 12345};
    i16[i] = static_cast<uint16_t>(i < 4 ? signed_values[i] : static_cast<int16_t>(i * 977 - 31000));
    u16[i] = static_cast<uint16_t>(i * 40503u + 0x8000u);
    put(raw, i * 2, h[i]);
    put(raw, 128 + i * 2, i16[i]);
    put(raw, 256 + i * 2, u16[i]);
  }
  struct {
    uint16_t ch[4];
    int16_t ci[4];
    uint16_t cu[4];
  } cb{{floatToHalf(1.5f), floatToHalf(-0.75f), floatToHalf(2.0f), floatToHalf(-1.0f)}, {3, -7, 0, 0}, {0xa5a5, 12, 0, 0}};
  auto r = c.run("cs_16bit", filled(64 * 32), raw, filled(64), {0, 0, 0, 0}, 1, &cb, sizeof(cb));
  if (!r.empty()) {
    const auto &o = r[0];
    for (uint32_t gi = 0; gi < 64; ++gi) {
      const size_t b = gi * 32;
      const float hf = halfToFloat(h[gi]);
      /* h*1.5 - 0.75 and max(h,2) - min(h,-1) are exact before the final rounding for every
       * input, so fused and unfused evaluation agree; denormal inputs vanish in both. */
      const float r0 = hf * 1.5f - 0.75f;
      const float r1 = std::max(hf, 2.0f) - std::min(hf, -1.0f);
      const int16_t s = static_cast<int16_t>(i16[gi]);
      const int16_t r2 = static_cast<int16_t>(s * 3 + (s >> 3) - 7);
      const uint16_t r3 = static_cast<uint16_t>((u16[gi] >> (gi & 15)) ^ static_cast<uint16_t>(u16[gi] << 3) ^ 0xa5a5);
      const uint16_t r4 = static_cast<uint16_t>(u16[gi] / 13 + u16[gi] % 13);
      EXPECT_EQ("16bit", "mad.half", gi, load<uint16_t>(o, b + 0), floatToHalf(r0));
      EXPECT_EQ("16bit", "maxmin.half", gi, load<uint16_t>(o, b + 2), floatToHalf(r1));
      EXPECT_EQ("16bit", "int16", gi, load<uint16_t>(o, b + 4), static_cast<uint16_t>(r2));
      EXPECT_EQ("16bit", "uint16.shift", gi, load<uint16_t>(o, b + 6), r3);
      EXPECT_EQ("16bit", "uint16.divrem", gi, load<uint16_t>(o, b + 8), r4);
      EXPECT_EQ("16bit", "int16->half", gi, load<uint16_t>(o, b + 10), floatToHalf(static_cast<float>(s)));
      if (gi != 63) /* 65504 does not fit an int16: the conversion is undefined there */
        EXPECT_EQ("16bit", "half->int16", gi, load<uint16_t>(o, b + 12),
                  static_cast<uint16_t>(static_cast<int16_t>(hf)));
      EXPECT_EQ("16bit", "half->float", gi, load<uint32_t>(o, b + 16), floatBits(hf));
      EXPECT_EQ("16bit", "float->half.rne", gi, load<uint16_t>(o, b + 20),
                floatToHalf(static_cast<float>(gi) * 1100.25f));
      EXPECT_EQ("16bit", "asuint16", gi, load<uint16_t>(o, b + 22), h[gi]);
      const uint32_t w = gi / 32;
      int16_t wmin = INT16_MAX;
      uint16_t wmax = 0;
      for (uint32_t l = 0; l < 32; ++l) {
        wmin = std::min<int16_t>(wmin, static_cast<int16_t>(i16[w * 32 + l]));
        wmax = std::max<uint16_t>(wmax, u16[w * 32 + l]);
      }
      EXPECT_EQ("16bit", "wave.sum.half", gi, load<uint16_t>(o, b + 24), floatToHalf(56.0f));
      EXPECT_EQ("16bit", "wave.min.int16", gi, load<uint16_t>(o, b + 26), static_cast<uint16_t>(wmin));
      EXPECT_EQ("16bit", "wave.max.uint16", gi, load<uint16_t>(o, b + 28), wmax);
      EXPECT_EQ("16bit", "wave.readlane.uint16", gi, load<uint16_t>(o, b + 30), u16[w * 32 + 3]);
    }
  }
  end("16bit");
}

void testMinPrecision(Compute &c) {
  begin();
  auto r = c.run("cs_minprec", filled(64 * 12), filled(64), filled(64), {0, 0, 0, 0}, 1);
  if (!r.empty()) {
    for (uint32_t gi = 0; gi < 64; ++gi) {
      EXPECT_EQ("minprec", "float", gi, load<uint32_t>(r[0], gi * 12), floatBits(gi * 0.25f * 2.0f + 1.0f));
      EXPECT_EQ("minprec", "int", gi, load<uint32_t>(r[0], gi * 12 + 4),
                static_cast<uint32_t>((static_cast<int>(gi) - 32) * 3 - 1));
      EXPECT_EQ("minprec", "uint", gi, load<uint32_t>(r[0], gi * 12 + 8), gi * 3 + 7);
    }
  }
  end("minprec");
}

/* ---- cs_packed: dot2add, dot4add i8/u8, pack/unpack 4x8 ---- */
void testPacked(Compute &c) {
  begin();
  std::vector<uint8_t> raw(1280, 0);
  std::vector<uint32_t> a(64), b(64);
  std::vector<float> ha(128), hb(128), acc(64);
  for (uint32_t i = 0; i < 64; ++i) {
    a[i] = i * 0x9e3779b9u ^ 0x80ff7f01u;
    b[i] = (i * 0x85ebca6bu) ^ (i << 24);
    put(raw, i * 4, a[i]);
    put(raw, 256 + i * 4, b[i]);
    for (uint32_t k = 0; k < 2; ++k) {
      ha[i * 2 + k] = (static_cast<float>((i + k * 5) % 9) - 4.0f) * 0.5f;
      hb[i * 2 + k] = (static_cast<float>((i * 3 + k) % 11) - 5.0f) * 0.25f;
      put(raw, 512 + i * 4 + k * 2, floatToHalf(ha[i * 2 + k]));
      put(raw, 768 + i * 4 + k * 2, floatToHalf(hb[i * 2 + k]));
    }
    acc[i] = static_cast<float>(i) * 0.125f - 3.0f;
    put(raw, 1024 + i * 4, acc[i]);
  }
  auto r = c.run("cs_packed", filled(64 * 48), raw, filled(64), {0, 0, 0, 0}, 1);
  if (!r.empty()) {
    const auto &o = r[0];
    auto sbyte = [](uint32_t v, int k) { return static_cast<int32_t>(static_cast<int8_t>(v >> (k * 8))); };
    auto ubyte = [](uint32_t v, int k) { return (v >> (k * 8)) & 0xffu; };
    for (uint32_t gi = 0; gi < 64; ++gi) {
      const size_t base = gi * 48;
      const float dot = acc[gi] + ha[gi * 2] * hb[gi * 2] + ha[gi * 2 + 1] * hb[gi * 2 + 1];
      EXPECT_EQ("packed", "dot2add", gi, load<uint32_t>(o, base + 0), floatBits(dot));
      int32_t si = static_cast<int32_t>(gi * 1000) - 30000;
      uint32_t ui = gi * 77777u;
      for (int k = 0; k < 4; ++k) {
        si += sbyte(a[gi], k) * sbyte(b[gi], k);
        ui += ubyte(a[gi], k) * ubyte(b[gi], k);
      }
      EXPECT_EQ("packed", "dot4add.i8", gi, load<uint32_t>(o, base + 4), static_cast<uint32_t>(si));
      EXPECT_EQ("packed", "dot4add.u8", gi, load<uint32_t>(o, base + 8), ui);
      EXPECT_EQ("packed", "unpack.s8s16", gi, load<uint32_t>(o, base + 12),
                static_cast<uint32_t>(sbyte(a[gi], 0)) ^ (static_cast<uint32_t>(sbyte(a[gi], 3)) << 16));
      EXPECT_EQ("packed", "unpack.u8u16", gi, load<uint32_t>(o, base + 16), ubyte(a[gi], 1) | (ubyte(a[gi], 2) << 16));
      EXPECT_EQ("packed", "unpack.s8s32", gi, load<uint32_t>(o, base + 20),
                static_cast<uint32_t>(sbyte(b[gi], 0) + sbyte(b[gi], 1) * 3 + sbyte(b[gi], 2) * 5 + sbyte(b[gi], 3) * 7));
      EXPECT_EQ("packed", "unpack.u8u32", gi, load<uint32_t>(o, base + 24),
                ubyte(b[gi], 0) + ubyte(b[gi], 1) * 3 + ubyte(b[gi], 2) * 5 + ubyte(b[gi], 3) * 7);
      const int32_t wide[4] = {static_cast<int32_t>(a[gi]) >> 20, static_cast<int32_t>(a[gi] << 7) >> 22,
                               static_cast<int32_t>(b[gi]) >> 23, static_cast<int32_t>(gi) - 32};
      uint32_t pu = 0, ps = 0, cu = 0, cs = 0;
      for (int k = 0; k < 4; ++k) {
        pu |= (static_cast<uint32_t>(wide[k]) & 0xffu) << (k * 8);
        ps |= (static_cast<uint32_t>(wide[k]) & 0xffu) << (k * 8);
        cu |= static_cast<uint32_t>(std::clamp(wide[k], 0, 255)) << (k * 8);
        cs |= (static_cast<uint32_t>(std::clamp(wide[k], -128, 127)) & 0xffu) << (k * 8);
      }
      EXPECT_EQ("packed", "pack.u8", gi, load<uint32_t>(o, base + 28), pu);
      EXPECT_EQ("packed", "pack.s8", gi, load<uint32_t>(o, base + 32), ps);
      EXPECT_EQ("packed", "pack.clamp.u8", gi, load<uint32_t>(o, base + 36), cu);
      EXPECT_EQ("packed", "pack.clamp.s8", gi, load<uint32_t>(o, base + 40), cs);
      const int32_t narrow[4] = {static_cast<int16_t>(a[gi] & 0xffff), -300, 127, -128};
      uint32_t cn = 0;
      for (int k = 0; k < 4; ++k)
        cn |= (static_cast<uint32_t>(std::clamp(narrow[k], -128, 127)) & 0xffu) << (k * 8);
      EXPECT_EQ("packed", "pack.clamp.s8.i16", gi, load<uint32_t>(o, base + 44), cn);
    }
  }
  end("packed");
}

/* ---- cs_quad_deriv: compute quads, derivatives, implicit LOD, barriers ---- */
ComPtr<ID3D12Resource> mipTexture(Gpu &gpu, UINT width = 64, UINT height = 64,
                                  D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  UINT16 depthOrArraySize = 1, UINT slot = 3) {
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = dimension;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = depthOrArraySize;
  desc.MipLevels = 7;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> texture;
  check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&texture)),
        "CreateCommittedResource(texture)");
  const UINT subresources = 7 * (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : depthOrArraySize);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> row_bytes(subresources);
  UINT64 total = 0;
  gpu.device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), row_bytes.data(), &total);
  auto upload = gpu.buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mapped = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "Map(texture upload)");
  for (UINT sub = 0; sub < subresources; ++sub)
    for (UINT z = 0; z < footprints[sub].Footprint.Depth; ++z)
      for (UINT y = 0; y < rows[sub]; ++y)
        for (UINT x = 0; x < footprints[sub].Footprint.Width; ++x) {
          const float value = static_cast<float>(sub % 7 + 1);
          std::memcpy(mapped + footprints[sub].Offset +
                          (z * rows[sub] + y) * footprints[sub].Footprint.RowPitch + x * 4, &value, 4);
        }
  upload->Unmap(0, nullptr);
  for (UINT mip = 0; mip < subresources; ++mip) {
    D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = mip;
    D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint = footprints[mip];
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  gpu.keepAlive.push_back(upload);
  gpu.device->CreateShaderResourceView(texture.Get(), nullptr, gpu.cpu(slot));
  return texture;
}

void testQuadDerivatives(Compute &c) {
  begin();
  auto r = c.run("cs_quad_deriv", filled(64 * 48), filled(256), filled(64), {0, 0, 0, 0}, 1);
  if (!r.empty()) {
    const auto &o = r[0];
    auto quad_any = [](uint32_t x, uint32_t y, auto predicate) {
      const uint32_t qx = x & ~1u, qy = y & ~1u;
      return predicate(qx, qy) || predicate(qx + 1, qy) || predicate(qx, qy + 1) || predicate(qx + 1, qy + 1);
    };
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t x = 0; x < 8; ++x) {
        const uint32_t gi = y * 8 + x;
        const size_t b = gi * 48;
        const uint32_t qx = x & ~1u, qy = y & ~1u;
        const float ddx = static_cast<float>((qx + 1) * (qx + 1) - qx * qx);
        const float ddy = 10.0f * static_cast<float>((qy + 1) * (qy + 1) * (qy + 1) - qy * qy * qy);
        EXPECT_EQ("quad_deriv", "ddx_fine", gi, load<uint32_t>(o, b + 0), floatBits(ddx));
        EXPECT_EQ("quad_deriv", "ddy_fine", gi, load<uint32_t>(o, b + 4), floatBits(ddy));
        EXPECT_EQ("quad_deriv", "quadany", gi, load<uint32_t>(o, b + 8),
                  quad_any(x, y, [](uint32_t px, uint32_t py) { return px == 1 && py == 1; }));
        EXPECT_EQ("quad_deriv", "quadall", gi, load<uint32_t>(o, b + 12),
                  !quad_any(x, y, [](uint32_t px, uint32_t py) { return ((px + py) & 7) == 3; }));
        EXPECT_EQ("quad_deriv", "helper|threadid", gi, load<uint32_t>(o, b + 16), (x << 8) | (y << 16));
        EXPECT_EQ("quad_deriv", "barrier", gi, load<uint32_t>(o, b + 20), gi * 3);
        const float lods[4] = {bitsFloat(load<uint32_t>(o, b + 24)), bitsFloat(load<uint32_t>(o, b + 28)),
                               bitsFloat(load<uint32_t>(o, b + 32)), bitsFloat(load<uint32_t>(o, b + 36))};
        const float expected[4] = {2.0f, 2.0f, 6.0f, 8.0f};
        for (int k = 0; k < 4; ++k)
          if (!(std::fabs(lods[k] - expected[k]) <= 0.01f))
            mismatch("quad_deriv", "calculate_lod", gi * 4 + k, load<uint32_t>(o, b + 24 + k * 4),
                     floatBits(expected[k]));
        EXPECT_EQ("quad_deriv", "sample.implicit", gi, load<uint32_t>(o, b + 40), floatBits(3.0f));
        EXPECT_EQ("quad_deriv", "sample.bias", gi, load<uint32_t>(o, b + 44), floatBits(4.0f));
      }
  }
  end("quad_deriv");
}

/* ---- CalculateLOD: analytical values plus independent native mip sampling ---- */
struct LodInput {
  float base[4];
  float dx[4];
  float dy[4];
};

void clearLodShapeSrvs(Gpu &gpu) {
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_FLOAT;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1;
  for (UINT slot = 4; slot <= 9; ++slot)
    gpu.device->CreateShaderResourceView(nullptr, &view, gpu.cpu(slot));
}

void expectLod(const char *name, const char *field, size_t index,
               const std::vector<uint8_t> &out, size_t offset, float expected) {
  const uint32_t bits = load<uint32_t>(out, offset);
  const float actual = bitsFloat(bits);
  if (!std::isfinite(actual) || !std::isfinite(expected) || std::fabs(actual - expected) > 1.0f / 64.0f)
    mismatch(name, field, index, bits, floatBits(expected));
}

void testCalculateLod(Compute &c) {
  begin();
  auto square = c.texture;
  auto nonsquare = mipTexture(c.gpu, 64, 32);
  auto large = mipTexture(c.gpu, 256, 256);
  c.gpu.finish({}, nullptr);
  const auto savedRoot = c.root;
  const D3D12_FILTER linear = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  const D3D12_FILTER aniso = D3D12_FILTER_ANISOTROPIC;
  struct Case {
    const char *name;
    UINT resource, baseMip, mips;
    float resourceMin;
    D3D12_FILTER filter;
    UINT anisotropy;
    float bias, minLod, maxLod, dxU, dxV, dyU, dyV, clamped, unclamped;
    bool sample;
  };
  // All targets are specified analytically, not by a copy of the lowering.
  // Orthogonal anisotropic targets below are chosen-modern-policy evidence,
  // not universal hardware conformance; native Sample is a separate GPU oracle.
  const Case cases[] = {
      {"linear", 0, 0, 7, 0, linear, 1, 0, 0, 16, .125f, 0, 0, .03125f, 3, 3, true},
      {"positive_bias", 0, 0, 7, 0, linear, 1, .75f, 0, 16, .125f, 0, 0, .03125f, 3.75f, 3.75f, true},
      {"negative_bias", 0, 0, 7, 0, linear, 1, -.75f, 0, 16, .125f, 0, 0, .03125f, 2.25f, 2.25f, true},
      {"bias_before_lower_bound", 0, 0, 7, 0, linear, 1, .75f, 0, 16, 1.f/256, 0, 0, 1.f/512, 0, -1.25f, true},
      {"sampler_min", 0, 0, 7, 0, linear, 1, 0, 4.25f, 16, .125f, 0, 0, .03125f, 4.25f, 3, true},
      {"bias_before_sampler_max", 0, 0, 7, 0, linear, 1, 2, 0, 2.25f, .125f, 0, 0, .03125f, 2.25f, 5, true},
      {"view_upper", 0, 0, 3, 0, linear, 1, 0, 0, 16, .125f, 0, 0, .03125f, 2, 3, true},
      {"view_upper_wins_sampler_min", 0, 0, 3, 0, linear, 1, 0, 5, 16, .125f, 0, 0, .03125f, 2, 3, true},
      {"resource_wins_sampler_max", 0, 0, 7, 4.5f, linear, 1, 0, 0, 2, .125f, 0, 0, .03125f, 4.5f, 3, false},
      {"resource_exact_last", 0, 0, 7, 6, linear, 1, 0, 0, 16, .125f, 0, 0, .03125f, 6, 3, false},
      {"resource_fractional_empty", 0, 0, 7, 6.125f, linear, 1, 0, 0, 16, .125f, 0, 0, .03125f, 0, 3, false},
      {"view_base_dimensions", 2, 2, 3, 0, linear, 1, 0, 0, 16, .0625f, 0, 0, .03125f, 2, 2, true},
      {"full_resource_dimensions", 2, 0, 7, 0, linear, 1, 0, 0, 16, .0625f, 0, 0, .03125f, 4, 4, true},
      {"nonsquare_height", 1, 0, 7, 0, linear, 1, 0, 0, 16, 0, .125f, .015625f, 0, 2, 2, true},
      {"chosen_policy_aniso1", 0, 0, 7, 0, aniso, 1, 0, 0, 16, .125f, 0, 0, .03125f, 3, 3, true},
      {"chosen_policy_aniso2", 0, 0, 7, 0, aniso, 2, 0, 0, 16, .125f, 0, 0, .03125f, 2, 2, true},
      {"chosen_policy_aniso4", 0, 0, 7, 0, aniso, 4, 0, 0, 16, .125f, 0, 0, .03125f, 1, 1, true},
      {"chosen_policy_aniso_limited", 0, 0, 7, 0, aniso, 4, 0, 0, 16, .125f, 0, 0, 1.f/256, 1, 1, true},
      {"chosen_policy_aniso_bias", 0, 0, 7, 0, aniso, 4, 1.25f, 0, 16, .125f, 0, 0, .03125f, 2.25f, 2.25f, true},
      {"portable_aniso_equal_range", 0, 0, 7, 0, aniso, 4, 0, 2.25f, 2.25f, .125f, 0, 0, .03125f, 2.25f, 1, true},
  };
  ID3D12Resource *resources[] = {square.Get(), nonsquare.Get(), large.Get()};
  for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    const auto &test = cases[index];
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MostDetailedMip = test.baseMip;
    view.Texture2D.MipLevels = test.mips;
    view.Texture2D.ResourceMinLODClamp = test.resourceMin;
    c.gpu.device->CreateShaderResourceView(resources[test.resource], &view, c.gpu.cpu(3));
    c.root = computeRoot(c.gpu.device.Get(), test.filter, test.bias, test.minLod, test.maxLod, test.anisotropy);
    const LodInput input{{.25f, .25f, 0, 0}, {test.dxU, test.dxV, 0, 0}, {test.dyU, test.dyV, 0, 0}};
    const auto result = c.run("cs_lod", filled(4 * 12), filled(64), filled(64),
                              {test.baseMip, test.sample ? 1u : 0u, 0, 0}, 1, &input, sizeof(input));
    if (result.empty())
      continue;
    for (UINT lane = 0; lane < 4; ++lane) {
      const size_t offset = lane * 12;
      expectLod("calculate_lod", test.name, index * 4 + lane, result[0], offset, test.clamped);
      expectLod("calculate_lod", "unclamped", index * 4 + lane, result[0], offset + 4, test.unclamped);
      if (test.sample)
        expectLod("calculate_lod", "native_sample_minus_one", index * 4 + lane, result[0], offset + 8,
                   bitsFloat(load<uint32_t>(result[0], offset)));
    }
  }
  c.root = savedRoot;
  c.gpu.device->CreateShaderResourceView(square.Get(), nullptr, c.gpu.cpu(3));
  end("calculate_lod");
}

void testCalculateLodShapes(Compute &c) {
  begin();
  auto t1 = mipTexture(c.gpu, 64, 1, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 1, 4);
  auto t1Array = mipTexture(c.gpu, 64, 1, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4, 5);
  auto t2Array = mipTexture(c.gpu, 64, 32, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 4, 6);
  auto t3 = mipTexture(c.gpu, 64, 16, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 8, 7);
  auto cube = mipTexture(c.gpu, 64, 64, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6, 8);
  auto cubeArray = mipTexture(c.gpu, 64, 64, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 18, 9);
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_FLOAT;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  view.TextureCube.MipLevels = 7;
  c.gpu.device->CreateShaderResourceView(cube.Get(), &view, c.gpu.cpu(8));
  view = {};
  view.Format = DXGI_FORMAT_R32_FLOAT;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
  view.TextureCubeArray.MipLevels = 7;
  view.TextureCubeArray.NumCubes = 3;
  c.gpu.device->CreateShaderResourceView(cubeArray.Get(), &view, c.gpu.cpu(9));
  c.gpu.finish({}, nullptr);
  const LodInput faces[] = {
      {{1, .125f, -.25f, 0}, {0, 0, -.125f, 0}, {0, -.125f, 0, 0}},
      {{-1, .125f, .25f, 0}, {0, 0, .125f, 0}, {0, -.125f, 0, 0}},
      {{.25f, 1, -.125f, 0}, {.125f, 0, 0, 0}, {0, 0, .125f, 0}},
      {{.25f, -1, .125f, 0}, {.125f, 0, 0, 0}, {0, 0, -.125f, 0}},
      {{.25f, .125f, 1, 0}, {.125f, 0, 0, 0}, {0, -.125f, 0, 0}},
      {{-.25f, .125f, -1, 0}, {-.125f, 0, 0, 0}, {0, -.125f, 0, 0}},
  };
  const char *fields[] = {"1d.clamped", "1d.unclamped", "1darray.clamped", "1darray.unclamped",
                           "2darray.clamped", "2darray.unclamped", "3d.clamped", "3d.unclamped",
                           "cube.clamped", "cube.unclamped", "cubearray.clamped", "cubearray.unclamped"};
  for (UINT face = 0; face < 6; ++face)
    for (UINT scale = 1; scale <= 4; scale += 3) {
      LodInput input = faces[face];
      for (UINT axis = 0; axis < 3; ++axis) {
        input.base[axis] *= scale;
        input.dx[axis] *= scale;
        input.dy[axis] *= scale;
      }
      const auto result = c.run("cs_lod_shapes", filled(4 * 64), filled(64), filled(64),
                                {0, 1, 0, 0}, 1, &input, sizeof(input));
      if (result.empty())
        continue;
      for (UINT lane = 0; lane < 4; ++lane) {
        for (UINT field = 0; field < 12; ++field)
          expectLod("calculate_lod_shapes", fields[field], (face * 2 + (scale == 4)) * 4 + lane,
                     result[0], lane * 64 + field * 4, field == 4 || field == 5 ? 3.f : 2.f);
        expectLod("calculate_lod_shapes", "2darray.native_sample_minus_one", face * 4 + lane,
                   result[0], lane * 64 + 48, bitsFloat(load<uint32_t>(result[0], lane * 64 + 16)));
      }
    }
  // Quotient-rule derivative at each CURRENT lane. Neighbor projected differences
  // would incorrectly yield log2(8/3) in both columns. Negative faces need signed dm.
  for (float sign : {1.f, -1.f}) {
    const LodInput input{{.25f, 0, sign, 0}, {.25f, 0, sign * .5f, 0}, {0, 0, 0, 0}};
    const auto result = c.run("cs_lod_shapes", filled(4 * 64), filled(64), filled(64),
                              {0, 0, 0, 0}, 1, &input, sizeof(input));
    if (!result.empty())
      for (UINT lane = 0; lane < 4; ++lane)
        for (UINT field = 8; field < 12; ++field)
          expectLod("calculate_lod_shapes", sign > 0 ? "cube.quotient_positive" : "cube.quotient_negative",
                     lane, result[0], lane * 64 + field * 4, (lane & 1) ? std::log2(16.f / 9.f) : 2.f);
  }
  const auto savedRoot = c.root;
  for (UINT anisotropy : {1u, 2u, 4u}) {
    c.root = computeRoot(c.gpu.device.Get(), D3D12_FILTER_ANISOTROPIC, 0, 0, 16, anisotropy);
    const auto result = c.run("cs_lod_shapes", filled(4 * 64), filled(64), filled(64),
                              {0, 1, 0, 0}, 1, &faces[4], sizeof(LodInput));
    if (!result.empty())
      for (UINT lane = 0; lane < 4; ++lane) {
        const float expected = anisotropy == 1 ? 3.f : anisotropy == 2 ? 2.f : 1.f;
        expectLod("calculate_lod_shapes", "2darray.chosen_policy_aniso.clamped", lane,
                   result[0], lane * 64 + 16, expected);
        expectLod("calculate_lod_shapes", "2darray.chosen_policy_aniso.unclamped", lane,
                   result[0], lane * 64 + 20, expected);
        expectLod("calculate_lod_shapes", "2darray.chosen_policy_native_sample", lane,
                   result[0], lane * 64 + 48, bitsFloat(load<uint32_t>(result[0], lane * 64 + 16)));
      }
    // Cube anisotropy may provide only trilinear quality: intentionally no minor-axis assertion.
  }
  c.root = savedRoot;
  clearLodShapeSrvs(c.gpu);
  end("calculate_lod_shapes");
}

void testWideIntegerHierarchy(Compute &c) {
  struct Case {const char* entry;uint32_t width,threads;};
  for (const auto& test : {Case{"cs_wide_exact64",64,173},Case{"cs_wide_exact128",128,173},
      Case{"cs_wide_exact64_short",64,17},Case{"cs_wide_exact128_short",128,17}}) {
    begin();
    auto result=c.run(test.entry,filled(test.threads*256),filled(64),filled(64),{0,0,0,0},1);
    if(!result.empty()) for(uint32_t gi=0;gi<test.threads;++gi) {
      const uint32_t lane=gi%test.width,base=gi-lane,live=std::min(test.width,test.threads-base);
      uint64_t expected[13]{0,0,UINT64_MAX,0,0,0,0,0,UINT64_MAX,0,0,1,1};
      int64_t minimum=INT64_MAX,maximum=INT64_MIN;
      uint16_t sum16=0,prefix16=0,min16=UINT16_MAX,max16=0;
      int16_t minSigned16=INT16_MAX,maxSigned16=INT16_MIN;
      uint32_t count=0,prefixCount=0;
      for(uint32_t s=0;s<live;++s) {
        const uint32_t id=base+s;
        const uint64_t value=(uint64_t(id+1)<<32)|(uint64_t(id)*0x9e3779b9u+17u);
        const int64_t signedValue=(id&1)?-int64_t(value):int64_t(value);
        const uint64_t product=(s%3)*2+1;
        expected[0]+=value;if(s<lane)expected[1]+=value;
        expected[2]=std::min(expected[2],value);expected[3]=std::max(expected[3],value);
        expected[4]+=uint64_t(signedValue);if(s<lane)expected[5]+=uint64_t(signedValue);
        minimum=std::min(minimum,signedValue);maximum=std::max(maximum,signedValue);
        expected[8]&=value;expected[9]|=value;expected[10]^=value;expected[11]*=product;if(s<lane)expected[12]*=product;
        const uint16_t small=uint16_t(id*257+32761);const int16_t signedSmall=int16_t(small);
        sum16=uint16_t(sum16+small);if(s<lane)prefix16=uint16_t(prefix16+small);
        min16=std::min(min16,small);max16=std::max(max16,small);
        minSigned16=std::min(minSigned16,signedSmall);maxSigned16=std::max(maxSigned16,signedSmall);
        if(s%5==0){++count;if(s<lane)++prefixCount;}
      }
      expected[6]=uint64_t(minimum);expected[7]=uint64_t(maximum);
      for(uint32_t i=0;i<13;++i)EXPECT_EQ(test.entry,"i64",gi*13+i,load<uint64_t>(result[0],gi*256+i*8),expected[i]);
      const uint32_t other[]={sum16,prefix16,min16,max16,uint32_t(int32_t(minSigned16)),uint32_t(int32_t(maxSigned16)),
          count,prefixCount,uint32_t(live>33),1};
      for(uint32_t i=0;i<10;++i)EXPECT_EQ(test.entry,"i16_vote",gi*10+i,load<uint32_t>(result[0],gi*256+104+i*4),other[i]);
    }
    end(test.entry);
  }
}
void testWideWaves(Compute &c) {
  testWideIntegerHierarchy(c);
  struct Case { const char *entry; uint32_t width, threads; };
  for (const auto &test : {Case{"cs_wavesize64", 64, 128}, Case{"cs_wavesize128", 128, 256},
       Case{"cs_wavesize_range64only", 64, 128}, Case{"cs_wavesize64_partial", 64, 160},
       Case{"cs_wavesize128_partial", 128, 288}}) {
    begin();
    auto result = c.run(test.entry, filled(test.threads * 128), filled(64), filled(64), {0, 0, 0, 0}, 1);
    if (!result.empty()) for (uint32_t gi = 0; gi < test.threads; ++gi) {
      const uint32_t lane = gi % test.width, base = gi - lane;
      const uint32_t live = std::min(test.width, test.threads - base);
      uint32_t expected[29] = {test.width, lane, live, lane == 0};
      uint32_t sum = 0, prefix = 0, multi_sum = 0, multi_count = 0, bit_or = 0, bit_xor = 0;
      for (uint32_t source = 0; source < live; ++source) {
        const uint32_t value = base + source + 1;
        sum += value;
        if (source < lane) prefix += value;
        if ((source & 1) == 0) expected[4 + source / 32] |= 1u << (source % 32);
        if ((source & 3) == (lane & 3)) {
          expected[16 + source / 32] |= 1u << (source % 32);
          if (source < lane) { multi_sum += value; ++multi_count; }
        }
        bit_or |= 1u << (source % 32);
        bit_xor ^= value;
      }
      expected[8] = base + (lane + 33) % live + 1;
      expected[9] = base + 1;
      expected[10] = sum;
      expected[11] = prefix;
      expected[12] = base + 1;
      expected[13] = base + live;
      expected[14] = (live + 1) / 2;
      expected[15] = (lane + 1) / 2;
      expected[20] = multi_sum;
      expected[21] = multi_count;
      expected[22] = live > 33;
      expected[23] = 1;
      expected[24] = live == 1;
      expected[25] = 1;
      expected[26] = bit_or;
      expected[27] = bit_xor;
      const float float_sum = static_cast<float>(sum);
      std::memcpy(&expected[28], &float_sum, sizeof(float_sum));
      for (uint32_t word = 0; word < 29; ++word)
        EXPECT_EQ(test.entry, "word", gi * 29 + word, load<uint32_t>(result[0], gi * 128 + word * 4), expected[word]);
    }
    end(test.entry);
  }
}

/* ---- negative compile tests and WaveSize(32) ---- */
void testRejections(Compute &c) {
  begin();
  for (const char *entry : {"cs_wavesize64_divergent", "cs_wavesize128_divergent", "cs_wavesize64_return"}) {
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = computePso(c.gpu.device.Get(), c.root.Get(), shader(entry), pso);
    std::printf("OWNED_D3D12_SM6_REJECT entry=%s hr=0x%08lx\n", entry, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) || pso)
      mismatch("rejections", entry, 0, static_cast<uint32_t>(hr), 0x80070057u);
  }
  end("rejections");
  begin();
  auto gs64=c.run("cs_gs64",filled(8),filled(64),filled(64),{0,0,0,0},1);
  if(!gs64.empty())
    EXPECT_EQ("groupshared64","add",0,load<uint64_t>(gs64[0],0),1);
  end("groupshared64");
  begin();
  auto sm69 = c.run("cs_sm69", filled(4), filled(64), filled(64), {0, 0, 0, 0}, 1);
  if (!sm69.empty())
    EXPECT_EQ("sm69_input", "store", 0, load<uint32_t>(sm69[0], 0), 1);
  end("sm69_input");
  begin();
  auto r = c.run("cs_wavesize32", filled(128), filled(64), filled(64), {0, 0, 0, 0}, 1);
  if (!r.empty())
    for (uint32_t i = 0; i < 32; ++i)
      EXPECT_EQ("wavesize32", "lanecount", i, load<uint32_t>(r[0], i * 4), 32);
  end("wavesize32");
}

/* ---- graphics: start locations, barycentrics, attribute at vertex, helper lanes, 16 bit I/O ---- */
void testBarycentrics(Graphics &g, bool indexed) {
  const char *name = indexed ? "barycentrics_indexed" : "barycentrics";
  begin();
  /* D3D's SV_VertexID/SV_InstanceID exclude StartVertexLocation/BaseVertexLocation and
   * StartInstanceLocation, which only SV_Start*Location report: corner k is vertex k. */
  const UINT start_vertex = indexed ? 10 : 5, start_instance = indexed ? 3 : 7;
  auto r = g.draw("vs_main", "ps_main", 8, 8, filled(64 * 128), filled(64), {0, 0, 0, 0}, indexed, start_vertex,
                  start_instance);
  if (!r.empty()) {
    const auto &o = r[0];
    const uint32_t corner[3] = {0, 1, 2};
    for (uint32_t py = 0; py < 8; ++py)
      for (uint32_t px = 0; px < 8; ++px) {
        const uint32_t pixel = py * 8 + px;
        const size_t b = pixel * 128;
        EXPECT_EQ(name, "SV_VertexID", pixel, load<uint32_t>(o, b + 0), 0);
        EXPECT_EQ(name, "SV_InstanceID", pixel, load<uint32_t>(o, b + 4), 0);
        EXPECT_EQ(name, "SV_StartVertexLocation", pixel, load<uint32_t>(o, b + 8), start_vertex);
        EXPECT_EQ(name, "SV_StartInstanceLocation", pixel, load<uint32_t>(o, b + 12), start_instance);
        /* Screen-space barycentrics: corner 0 at (0,0), corner 1 at (16,0), corner 2 at (0,16). */
        const float x = px + 0.5f, y = py + 0.5f;
        const float by_corner[3] = {1.0f - x / 16.0f - y / 16.0f, x / 16.0f, y / 16.0f};
        for (uint32_t k = 0; k < 3; ++k) {
          const float expected = by_corner[corner[k]];
          const uint32_t perspective = load<uint32_t>(o, b + 16 + k * 4), linear = load<uint32_t>(o, b + 68 + k * 4);
          if (!(std::fabs(bitsFloat(perspective) - expected) <= 1e-4f))
            mismatch(name, "SV_Barycentrics", pixel * 3 + k, perspective, floatBits(expected));
          if (!(std::fabs(bitsFloat(linear) - expected) <= 1e-4f))
            mismatch(name, "SV_Barycentrics.noperspective", pixel * 3 + k, linear, floatBits(expected));
          EXPECT_EQ(name, "GetAttributeAtVertex", pixel * 3 + k, load<uint32_t>(o, b + 32 + k * 4),
                    floatBits(100.0f + corner[k]));
        }
        if (!(std::fabs(bitsFloat(load<uint32_t>(o, b + 28)) - 1.0f) <= 1e-5f))
          mismatch(name, "noperspective.sum", pixel, load<uint32_t>(o, b + 28), floatBits(1.0f));
        EXPECT_EQ(name, "IsHelperLane", pixel, load<uint32_t>(o, b + 44), 0);
        const uint32_t lanes = load<uint32_t>(o, b + 48);
        /* D3D does not fix how pixels pack into waves; helper lanes are excluded. */
        if (lanes == 0 || lanes > 32)
          mismatch(name, "wave.countbits", pixel, lanes, 32);
        const uint32_t qx = px & ~1u, qy = py & ~1u;
        EXPECT_EQ(name, "QuadAny", pixel, load<uint32_t>(o, b + 52), qx == 0 && qy == 0);
        EXPECT_EQ(name, "QuadAll", pixel, load<uint32_t>(o, b + 56), !(qx == 2 && qy == 2));
        if (!(std::fabs(bitsFloat(load<uint32_t>(o, b + 60)) - 0.25f) <= 1e-4f))
          mismatch(name, "ddx_fine", pixel, load<uint32_t>(o, b + 60), floatBits(0.25f));
        EXPECT_EQ(name, "QuadReadAcrossDiagonal", pixel, load<uint32_t>(o, b + 64), (px ^ 1) + 10 * (py ^ 1));
      }
  }
  end(name);
}

void testHelperLanes(Graphics &g, bool include) {
  const char *name = include ? "helper_lanes_included" : "helper_lanes";
  begin();
  auto r = g.draw("vs_tiny", include ? "ps_helper_all" : "ps_helper", 8, 8, filled(8192), filled(64),
                  {0, 0, 0, 0}, false, 0, 0);
  if (!r.empty()) {
    const auto &o = r[0];
    for (uint32_t pixel = 0; pixel < 64; ++pixel) {
      const size_t b = 4096 + pixel * 32;
      if (pixel != 2 * 8 + 5) {
        /* helper lanes never write memory */
        for (uint32_t k = 0; k < 8; ++k)
          EXPECT_EQ(name, "untouched", pixel, load<uint32_t>(o, b + k * 4), kSentinel);
        continue;
      }
      const uint32_t lanes = include ? 4 : 1;
      const uint32_t expected[8] = {lanes, lanes, lanes, 0, 7, 1, include ? 0u : 1u, include ? 0u : 1u};
      static const char *names[8] = {"wave.sum", "wave.countbits", "wave.ballot", "IsHelperLane",
                                     "quad.read.helpers", "QuadAny(helper)", "wave.alltrue(!helper)",
                                     "wave.isfirst"};
      for (uint32_t k = 0; k < 8; ++k)
        EXPECT_EQ(name, names[k], pixel, load<uint32_t>(o, b + k * 4), expected[k]);
    }
    for (size_t i = 0; i < 4096; i += 4)
      EXPECT_EQ(name, "untouched.low", i, load<uint32_t>(o, i), kSentinel);
  }
  end(name);
}

void test16BitIo(Graphics &g) {
  begin();
  auto r = g.draw("vs_16bit", "ps_16bit", 8, 8, filled(512), filled(64), {0, 0, 0, 0}, false, 0, 0);
  if (!r.empty())
    for (uint32_t pixel = 0; pixel < 64; ++pixel) {
      EXPECT_EQ("16bit_io", "uint16.nointerpolation", pixel, load<uint16_t>(r[0], pixel * 8), 0xfffe);
      EXPECT_EQ("16bit_io", "half.interpolated", pixel, load<uint16_t>(r[0], pixel * 8 + 2), floatToHalf(-0.5f));
      EXPECT_EQ("16bit_io", "uint16.vertexid", pixel, load<uint16_t>(r[0], pixel * 8 + 4), 40);
    }
  end("16bit_io");
}

/* ---- vertex-shader wave ops over a partial draw (45 vertices) ---- */
void testVertexWave(Graphics &g) {
  begin();
  constexpr uint32_t kVertices = 45;
  g.vertex_count = kVertices;
  auto r = g.draw("vs_wave", "ps_null", 8, 8, filled(kVertices * 48), filled(64), {0, 0, 0, 0}, false, 0, 0);
  g.vertex_count = 3;
  if (!r.empty()) {
    const auto &o = r[0];
    auto field = [&](uint32_t vid, uint32_t k) { return load<uint32_t>(o, vid * 48 + k * 4); };
    auto s = [](uint32_t vid) { return static_cast<int32_t>(vid * 2654435761u) >> 8; };
    /* Group vertices into waves by their WaveReadLaneFirst value; D3D does not fix the packing,
     * so the oracle is computed from each wave's actual members and their lane indices. */
    std::vector<uint8_t> seen(kVertices, 0);
    uint32_t covered = 0;
    for (uint32_t vid = 0; vid < kVertices; ++vid) {
      EXPECT_EQ("vs_wave", "vid", vid, field(vid, 0), vid);
      EXPECT_EQ("vs_wave", "lanecount", vid, field(vid, 11), 32);
      const uint32_t first = field(vid, 3), ballot = field(vid, 2);
      if (first >= kVertices || field(first, 3) != first || field(first, 2) != ballot) {
        mismatch("vs_wave", "wave.membership", vid, first, vid);
        continue;
      }
      if (seen[vid]++)
        mismatch("vs_wave", "duplicate", vid, 1, 0);
      ++covered;
      /* Members of this wave, by lane. */
      uint32_t member[32];
      uint32_t present = 0;
      for (uint32_t other = 0; other < kVertices; ++other)
        if (field(other, 3) == first && field(other, 2) == ballot) {
          const uint32_t lane = field(other, 1);
          if (lane >= 32 || (present >> lane & 1))
            mismatch("vs_wave", "lane.unique", other, lane, 0);
          else {
            present |= 1u << lane;
            member[lane] = other;
          }
        }
      EXPECT_EQ("vs_wave", "ballot=active", vid, ballot, present);
      const uint32_t lane = field(vid, 1);
      uint32_t lowest = 32, sum = 0, prefix = 0, odd = 0, prefix3 = 0;
      int32_t lo = INT32_MAX, hi = INT32_MIN;
      for (uint32_t l = 0; l < 32; ++l) {
        if (!(present >> l & 1))
          continue;
        const uint32_t m = member[l];
        lowest = std::min(lowest, l);
        sum += m;
        lo = std::min(lo, s(m));
        hi = std::max(hi, s(m));
        odd |= (m & 1u) << l;
        if (l < lane) {
          prefix += m;
          prefix3 += (m % 3) == 0;
        }
      }
      EXPECT_EQ("vs_wave", "readfirst", vid, first, member[lowest]);
      EXPECT_EQ("vs_wave", "countbits", vid, field(vid, 4), static_cast<uint32_t>(__builtin_popcount(present)));
      EXPECT_EQ("vs_wave", "sum", vid, field(vid, 5), sum);
      EXPECT_EQ("vs_wave", "prefixsum", vid, field(vid, 6), prefix);
      EXPECT_EQ("vs_wave", "min.s", vid, field(vid, 7), static_cast<uint32_t>(lo));
      EXPECT_EQ("vs_wave", "max.s", vid, field(vid, 8), static_cast<uint32_t>(hi));
      EXPECT_EQ("vs_wave", "ballot.odd", vid, field(vid, 9), odd);
      EXPECT_EQ("vs_wave", "prefixcountbits", vid, field(vid, 10), prefix3);
    }
    EXPECT_EQ("vs_wave", "covered", 0, covered, kVertices);
    /* 45 vertices cannot fill whole 32-lane waves: at least one wave is partial. */
    bool partial = false;
    for (uint32_t vid = 0; vid < kVertices; ++vid)
      partial |= field(vid, 4) < 32;
    EXPECT_EQ("vs_wave", "partial.wave", 0, partial, 1);
  }
  end("vs_wave");
}

/* ---- graphics-stage 64 bit: arithmetic works, atomics are rejected ---- */
void testGraphicsInt64(Graphics &g) {
  begin();
  auto r = g.draw("vs_int64", "ps_int64", 8, 8, filled(64 * 32), filled(64), {0, 0, 0, 0}, false, 0, 0);
  if (!r.empty()) {
    for (uint32_t py = 0; py < 8; ++py)
      for (uint32_t px = 0; px < 8; ++px) {
        const uint32_t pixel = py * 8 + px;
        /* nointerpolation takes the provoking (first) vertex: vid 0. */
        const int64_t a = static_cast<int64_t>(0x7fffffff00000001ull * 3ull - 0x123456789ull);
        const int64_t b = static_cast<int64_t>(uint64_t(px + 1) * uint64_t(-0x100000003ll)) + int64_t(py);
        const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
        EXPECT_EQ("graphics_int64", "passthrough", pixel, load<uint64_t>(r[0], pixel * 32 + 0), ua);
        EXPECT_EQ("graphics_int64", "mul+ashr", pixel, load<uint64_t>(r[0], pixel * 32 + 8),
                  ua * ub + static_cast<uint64_t>(a >> 7));
        EXPECT_EQ("graphics_int64", "udiv^shl", pixel, load<uint64_t>(r[0], pixel * 32 + 16),
                  (ua / (ub | 1)) ^ (ub << (px * 7)));
        EXPECT_EQ("graphics_int64", "min-srem", pixel, load<uint64_t>(r[0], pixel * 32 + 24),
                  static_cast<uint64_t>(std::min(a, b) - a % (b | 1)));
      }
  }
  end("graphics_int64");
}

/* SV_ViewID with view instancing off (no VIEW_INSTANCING subobject) is 0 in the VS and the PS. */
void testViewId(Graphics &g) {
  begin();
  auto r = g.draw("vs_viewid", "ps_viewid", 8, 8, filled(64 + 64 * 8), filled(64), {0, 0, 0, 0}, false, 0, 0);
  if (!r.empty()) {
    for (uint32_t vid = 0; vid < 3; ++vid)
      EXPECT_EQ("sv_viewid", "vs.SV_ViewID", vid, load<uint32_t>(r[0], vid * 4), 0);
    for (uint32_t i = 12; i < 64; i += 4)
      EXPECT_EQ("sv_viewid", "untouched", i, load<uint32_t>(r[0], i), kSentinel);
    for (uint32_t pixel = 0; pixel < 64; ++pixel) {
      EXPECT_EQ("sv_viewid", "ps.SV_ViewID", pixel, load<uint32_t>(r[0], 64 + pixel * 8), 0);
      EXPECT_EQ("sv_viewid", "vs.SV_ViewID.varying", pixel, load<uint32_t>(r[0], 64 + pixel * 8 + 4), 0);
    }
  }
  end("sv_viewid");
}

/* SV_ViewID under VIEW_INSTANCING (count 2, masking, mask 0b10): only view 1 runs, so VS and PS both read 1. */
void testViewIdInstanced(Graphics &g) {
  begin();
  static const D3D12_VIEW_INSTANCE_LOCATION locations[2] = {{0, 0}, {0, 0}};
  const D3D12_VIEW_INSTANCING_DESC views{2, locations, D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};
  auto r = g.draw("vs_viewid", "ps_viewid_vi", 8, 8, filled(64 + 64 * 8), filled(64), {0, 0, 0, 0}, false, 0, 0,
                  nullptr, &views, 0b10);
  if (!r.empty()) {
    for (uint32_t vid = 0; vid < 3; ++vid)
      EXPECT_EQ("sv_viewid_instanced", "vs.SV_ViewID", vid, load<uint32_t>(r[0], vid * 4), 1);
    for (uint32_t i = 12; i < 64; i += 4)
      EXPECT_EQ("sv_viewid_instanced", "untouched", i, load<uint32_t>(r[0], i), kSentinel);
    for (uint32_t pixel = 0; pixel < 64; ++pixel) {
      EXPECT_EQ("sv_viewid_instanced", "ps.SV_ViewID", pixel, load<uint32_t>(r[0], 64 + pixel * 8), 1);
      EXPECT_EQ("sv_viewid_instanced", "vs.SV_ViewID.varying", pixel, load<uint32_t>(r[0], 64 + pixel * 8 + 4), 1);
    }
  }
  end("sv_viewid_instanced");
}


/*
 * ---- SM 6.2 fp32 denorm modes (cs_denorm_any / _ftz / _preserve, same body) ----
 * The oracle is the host's IEEE binary32 arithmetic with SSE denormals on (MXCSR DAZ and
 * FTZ clear), which is exact: the expected "preserve" value, and with every denormal
 * input and result flushed to a signed zero the expected "ftz" value. "any" accepts
 * either per value (DXC's HLK rule).
 */
float flushed(float x) {
  const uint32_t b = floatBits(x);
  return (b & 0x7f800000u) == 0 ? bitsFloat(b & 0x80000000u) : x;
}

struct DenormInputs {
  std::vector<uint32_t> a, b, c;
};

DenormInputs denormInputs() {
  /* Row 0..12 are DXC's HLK Denorm* vectors (ShaderOpArithTable.xml); the rest mix
   * denormals, tiny normals, ±0, 1 and specials. */
  static const uint32_t hlk[][3] = {
    {0x007E0000, 0x007E0000, 0x00800000}, {0x00200000, 0x00200000, 0}, {0x007E0000, 0x807E0000, 0},
    {0x007E0000, 0x800E0000, 0x80780000}, {0x00000300, 0x43000000, 0}, {0x007F0000, 0x3f800000, 0},
    {0x007F0000, 0x007F0000, 0},          {0x001E0000, 0x41a00000, 0}, {0x00000300, 0x78000000, 0},
    {0x807F0000, 0x40800000, 0},          {0x20000000, 0x607F0000, 0}, {0x00800000, 0x40000000, 0},
    {0x80780000, 0x40000000, 0x00800000},
  };
  static const uint32_t pool[] = {0x00000001, 0x007fffff, 0x00400000, 0x80000123, 0x00800000, 0x80800001,
                                  0x00ffffff, 0x01000000, 0x3f800000, 0xbf000000, 0x00000000, 0x80000000,
                                  0x7f800000, 0x7fc00000, 0x40400000, 0x0c000000};
  DenormInputs in;
  uint32_t state = 0x9e3779b9u;
  auto next = [&]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  for (uint32_t i = 0; i < 64; ++i) {
    if (i < 13) {
      in.a.push_back(hlk[i][0]);
      in.b.push_back(hlk[i][1]);
      in.c.push_back(hlk[i][2]);
      continue;
    }
    auto pick = [&]() {
      const uint32_t r = next();
      if (r % 3 == 0)
        return pool[r / 3 % 16];
      /* A random denormal or a normal close to the denormal range. */
      return (r & 0x80000000u) | ((r >> 8) % 2 ? (r & 0x007fffffu) : (((r >> 9) % 4 + 1) << 23 | (r & 0x7fffffu)));
    };
    in.a.push_back(pick());
    in.b.push_back(pick());
    in.c.push_back(pick());
  }
  return in;
}

/* ---- binary64 emulation: exact edge-case bits, shared storage and both binary32 denorm modes ---- */
std::vector<uint8_t> fp64Inputs(uint32_t count) {
  const uint64_t edges[] = {0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL,
      0x000fffffffffffffULL, 0x0010000000000000ULL, 0x3ff0000000000000ULL, 0x3ff0000000000001ULL,
      0x3fefffffffffffffULL, 0x4000000000000000ULL, 0x4008000000000000ULL, 0xbff8000000000000ULL,
      0x7fefffffffffffffULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000123ULL,
      0x7ff0000000000001ULL, 0x41e0000000000000ULL, 0xc1e0000000000000ULL, 0x43e0000000000000ULL,
      0xc3e0000000000000ULL, 0x43f0000000000000ULL, 0x36a0000000000000ULL, 0xb6a0000000000000ULL};
  constexpr uint32_t edgeCount = sizeof(edges) / sizeof(edges[0]);
  std::vector<uint8_t> raw(count * 24);
  uint64_t random = 0xF064;
  for (uint32_t i = 0; i < count; ++i) {
    random = random * 6364136223846793005ULL + 1442695040888963407ULL;
    put(raw, i * 24, i < edgeCount * edgeCount ? edges[i / edgeCount] : random);
    random = random * 6364136223846793005ULL + 1442695040888963407ULL;
    put(raw, i * 24 + 8, i < edgeCount * edgeCount ? edges[i % edgeCount] : random);
    random = random * 6364136223846793005ULL + 1442695040888963407ULL;
    put(raw, i * 24 + 16, random);
  }
  return raw;
}

void testFp64(Compute &c, bool legacy) {
  const unsigned saved = _mm_getcsr();
  _mm_setcsr(saved & ~0x8040u);
  constexpr uint32_t count = 1024, stride = 160;
  auto bits = [](double x) { uint64_t u; std::memcpy(&u, &x, 8); return u; };
  auto value = [](uint64_t u) { double x; std::memcpy(&x, &u, 8); return x; };
  auto integer = [](double x, bool sign, unsigned width) -> uint64_t {
    if (std::isnan(x)) return 0;
    if (sign) {
      if (width == 32) {
        if (x >= 0x1p31) return INT32_MAX;
        if (x <= -0x1p31) return static_cast<uint64_t>(INT32_MIN);
        return static_cast<uint64_t>(static_cast<int32_t>(x));
      }
      if (x >= 0x1p63) return INT64_MAX;
      if (x <= -0x1p63) return static_cast<uint64_t>(INT64_MIN);
      return static_cast<uint64_t>(static_cast<int64_t>(x));
    }
    if (x <= 0) return 0;
    if (width == 32) return x >= 0x1p32 ? UINT32_MAX : static_cast<uint32_t>(x);
    return x >= 0x1p64 ? UINT64_MAX : static_cast<uint64_t>(x);
  };
  const auto raw = fp64Inputs(count);
  for (bool second : {false, true}) {
    const bool flush = !legacy && second;
    const char *name = legacy ? (second ? "fp64_legacy51" : "fp64_legacy50") : (flush ? "fp64_ftz" : "fp64");
    const char *entry = legacy ? (second ? "cs_fp64_legacy51.dxbc" : "cs_fp64_legacy50.dxbc") :
                                (flush ? "cs_fp64_ftz" : "cs_fp64");
    begin();
    auto r = c.run(entry, filled(count * stride), raw, filled(64), {0, 0, 0, 0}, count / 4);
    if (!r.empty()) for (uint32_t i = 0; i < count; ++i) {
      uint64_t au = load<uint64_t>(raw, i * 24), bu = load<uint64_t>(raw, i * 24 + 8);
      volatile double a = value(au), b = value(bu), z = value(load<uint64_t>(raw, i * 24 + 16));
      double minimum = std::isnan(a) ? b : std::isnan(b) ? a : std::fmin(a, b);
      double maximum = std::isnan(a) ? b : std::isnan(b) ? a : std::fmax(a, b);
      if (a == 0 && b == 0) { minimum = value(au | bu); maximum = value(au & bu); }
      uint32_t from = static_cast<uint32_t>(au), to = floatBits(static_cast<float>(a));
      if (flush) {
        if ((from & 0x7fffffff) < 0x00800000) from &= 0x80000000;
        if ((to & 0x7fffffff) < 0x00800000) to &= 0x80000000;
      }
      const uint32_t neighbor = (i & ~3u) | ((i + 1) & 3u);
      const double expected[] = {a + b, a - b, a * b, a / b, std::fma(a, b, z), -a,
          legacy ? static_cast<double>(static_cast<int32_t>(au)) : static_cast<double>(static_cast<int64_t>(au)),
          legacy ? static_cast<double>(static_cast<uint32_t>(au)) : static_cast<double>(au),
          static_cast<double>(bitsFloat(from)), value(load<uint64_t>(raw, neighbor * 24)),
          i & 1 ? a : b, minimum, maximum, std::fabs(a)};
      const uint32_t offsets[] = {0, 8, 16, 24, 32, 40, 88, 96, 104, 112, 120, 128, 136, 144};
      for (uint32_t op = 0; op < 14; ++op) {
        const uint64_t actual = load<uint64_t>(r[0], i * stride + offsets[op]);
        if (actual != bits(expected[op]) && !(std::isnan(value(actual)) && std::isnan(expected[op])))
          mismatch(name, "binary64.bits", i * 14 + op, actual, bits(expected[op]));
      }
      const uint32_t actualFloat = load<uint32_t>(r[0], i * stride + 48);
      if (actualFloat != to && !(std::isnan(bitsFloat(actualFloat)) && std::isnan(bitsFloat(to))))
        mismatch(name, "double.to.float", i, actualFloat, to);
      EXPECT_EQ(name, "double.to.int32", i, load<uint32_t>(r[0], i * stride + 52), static_cast<uint32_t>(integer(a, true, 32)));
      EXPECT_EQ(name, "double.to.uint32", i, load<uint32_t>(r[0], i * stride + 56), integer(a, false, 32));
      if (!legacy) {
        EXPECT_EQ(name, "double.to.int64", i, load<uint64_t>(r[0], i * stride + 64), integer(a, true, 64));
        EXPECT_EQ(name, "double.to.uint64", i, load<uint64_t>(r[0], i * stride + 72), integer(a, false, 64));
      } else for (uint32_t pad : {64u, 68u, 72u, 76u})
        EXPECT_EQ(name, "untouched.legacy.padding", i * stride + pad, load<uint32_t>(r[0], i * stride + pad), kSentinel);
      const uint32_t cmp = (a < b ? 1 : 0) | (a == b ? 2 : 0) | (a != b ? 4 : 0) |
          (a <= b ? 8 : 0) | (a > b ? 16 : 0) | (a >= b ? 32 : 0);
      EXPECT_EQ(name, "binary64.compare", i, load<uint32_t>(r[0], i * stride + 80), cmp);
      for (uint32_t pad : {60u, 84u, 152u, 156u})
        EXPECT_EQ(name, "untouched.padding", i * stride + pad, load<uint32_t>(r[0], i * stride + pad), kSentinel);
    }
    end(name);
  }
  _mm_setcsr(saved);
}

void testFp64Wave(Compute &c) {
  begin();
  const unsigned saved = _mm_getcsr();
  _mm_setcsr(saved & ~0x8040u);
  const uint64_t patterns[][4] = {
      {0, 0x8000000000000000ULL, 0, 0x8000000000000000ULL},
      {0x7ff8000000000123ULL, 0x3ff0000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL},
      {0xbff0000000000000ULL, 0xc000000000000000ULL, 0xc010000000000000ULL, 0x4020000000000000ULL},
      {1, 2, 4, 8},
      {0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0, 0x8000000000000000ULL},
      {0x3ff0000000000000ULL, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL},
      {0, 0, 0, 0x8000000000000000ULL},
      {0x7ff0000000000001ULL, 0xbff0000000000000ULL, 0xc000000000000000ULL, 0xc008000000000000ULL},
  };
  std::vector<uint8_t> raw(sizeof(patterns));
  std::memcpy(raw.data(), patterns, sizeof(patterns));
  constexpr uint32_t groups = sizeof(patterns) / sizeof(patterns[0]);
  auto r = c.run("cs_fp64_wave", filled(groups * 4 * 64), raw, filled(64), {0, 0, 0, 0}, groups);
  if (!r.empty()) for (uint32_t group = 0; group < groups; ++group) {
    bool equal = true;
    double minimum = INFINITY, maximum = -INFINITY;
    uint64_t minBits = 0x7ff0000000000000ULL, maxBits = 0xfff0000000000000ULL;
    volatile double sum = 0, product = 1;
    const double first = load<double>(raw, group * 32);
    for (uint32_t lane = 0; lane < 4; ++lane) {
      const double x = load<double>(raw, group * 32 + lane * 8);
      equal &= x == first;
      sum = sum + x;
      product = product * x;
      if (x < minimum) { minimum = x; minBits = patterns[group][lane]; }
      if (x > maximum) { maximum = x; maxBits = patterns[group][lane]; }
      if (x == 0 && minimum == 0) minBits |= patterns[group][lane];
      if (x == 0 && maximum == 0) maxBits &= patterns[group][lane];
    }
    uint64_t sumBits, productBits;
    const double sumValue = sum, productValue = product;
    std::memcpy(&sumBits, &sumValue, 8); std::memcpy(&productBits, &productValue, 8);
    for (uint32_t lane = 0; lane < 4; ++lane) {
      const uint32_t base = (group * 4 + lane) * 64;
      EXPECT_EQ("fp64_wave", "numeric.AllEqual", base, load<uint32_t>(r[0], base), equal);
      EXPECT_EQ("fp64_wave", "subwave.lane", base, load<uint32_t>(r[0], base + 4), lane);
      EXPECT_EQ("fp64_wave", "bitcopy.first", base, load<uint64_t>(r[0], base + 8), patterns[group][0]);
      EXPECT_EQ("fp64_wave", "bitcopy.shuffle", base, load<uint64_t>(r[0], base + 16), patterns[group][(lane + 1) & 3]);
      for (const auto pair : {std::pair{24u, sumBits}, std::pair{32u, productBits},
                              std::pair{40u, minBits}, std::pair{48u, maxBits}}) {
        const uint64_t actual = load<uint64_t>(r[0], base + pair.first);
        const bool nan = (actual & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL &&
                         (pair.second & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL;
        if (actual != pair.second && !nan) mismatch("fp64_wave", "reduce.bits", base + pair.first, actual, pair.second);
      }
      for (uint32_t pad : {56u, 60u})
        EXPECT_EQ("fp64_wave", "untouched.padding", base + pad, load<uint32_t>(r[0], base + pad), kSentinel);
    }
  }
  _mm_setcsr(saved);
  end("fp64_wave");
}
void testFp64Graphics(Graphics &g) {
  const unsigned saved = _mm_getcsr();
  _mm_setcsr(saved & ~0x8040u);
  auto raw = fp64Inputs(64);
  // Exact cancellation below binary32 precision, produced in the vertex shader.
  put(raw, 0, uint64_t{0x3ff0000000000001ULL});
  put(raw, 8, uint64_t{0x3feffffffffffffeULL});
  put(raw, 16, uint64_t{0xbff0000000000000ULL});
  auto value = [](uint64_t u) { double x; std::memcpy(&x, &u, 8); return x; };
  auto bits = [](double x) { uint64_t u; std::memcpy(&u, &x, 8); return u; };
  const double vertex = std::fma(value(load<uint64_t>(raw, 0)), value(load<uint64_t>(raw, 8)), value(load<uint64_t>(raw, 16)));
  struct Mode { const char *name, *vertex, *pixel; };
  for (const Mode mode : {Mode{"fp64_graphics", "vs_fp64", "ps_fp64"},
                         Mode{"fp64_graphics_legacy50", "vs_fp64_legacy50.dxbc", "ps_fp64_legacy50.dxbc"},
                         Mode{"fp64_graphics_legacy51", "vs_fp64_legacy51.dxbc", "ps_fp64_legacy51.dxbc"}}) {
    begin();
    auto r = g.draw(mode.vertex, mode.pixel, 8, 8, filled(64 * 32), raw, {0, 0, 0, 0}, false, 0, 0);
    if (!r.empty()) for (uint32_t i = 0; i < 64; ++i) {
      volatile double a = value(load<uint64_t>(raw, i * 24)), b = value(load<uint64_t>(raw, i * 24 + 8));
      volatile double z = value(load<uint64_t>(raw, i * 24 + 16));
      const double fused = std::fma(a, b, z);
      const double expected[] = {vertex, a + b, fused, fused + vertex};
      for (uint32_t op = 0; op < 4; ++op) {
        const uint64_t actual = load<uint64_t>(r[0], i * 32 + op * 8);
        if (actual != bits(expected[op]) && !(std::isnan(value(actual)) && std::isnan(expected[op])))
          mismatch(mode.name, "vertex.fragment.bits", i * 4 + op, actual, bits(expected[op]));
      }
    }
    end(mode.name);
  }
  _mm_setcsr(saved);
}

void testDenorm(Compute &c) {
  /* SSE denormals on for the oracle; restored afterwards. */
  const unsigned int saved = _mm_getcsr();
  _mm_setcsr(saved & ~0x8040u);
  const DenormInputs in = denormInputs();
  std::vector<uint8_t> raw(64 * 16);
  for (uint32_t i = 0; i < 64; ++i) {
    put(raw, i * 16 + 0, in.a[i]);
    put(raw, i * 16 + 4, in.b[i]);
    put(raw, i * 16 + 8, in.c[i]);
    put(raw, i * 16 + 12, 0x00000100u + i); /* the wave sum of 32 small denormals is still denormal */
  }
  struct Mode {
    const char *entry, *name;
    bool preserve, flush;
  };
  for (const Mode mode : {Mode{"cs_denorm_any", "denorm_any", true, true},
                          Mode{"cs_denorm_ftz", "denorm_ftz", false, true},
                          Mode{"cs_denorm_preserve", "denorm_preserve", true, false}}) {
    begin();
    auto r = c.run(mode.entry, filled(64 * 48), raw, filled(64), {0, 0, 0, 0}, 1);
    if (!r.empty()) {
      const auto &o = r[0];
      /* Compares one result against the exact and the flushed evaluation. NaNs match any NaN. */
      auto expect = [&](const char *what, uint32_t index, uint32_t actual, float exact, float flush_value) {
        const auto matches = [&](float want) {
          return actual == floatBits(want) || (std::isnan(want) && std::isnan(bitsFloat(actual)));
        };
        const bool ok = (mode.preserve && matches(exact)) || (mode.flush && matches(flush_value));
        if (!ok)
          mismatch(mode.name, what, index, actual, floatBits(mode.preserve ? exact : flush_value));
      };
      for (uint32_t gi = 0; gi < 64; ++gi) {
        const size_t base = gi * 48;
        const float a = bitsFloat(in.a[gi]), b = bitsFloat(in.b[gi]), cc = bitsFloat(in.c[gi]);
        const float fa = flushed(a), fb = flushed(b), fc = flushed(cc);
        expect("add", gi, load<uint32_t>(o, base + 0), a + b, flushed(fa + fb));
        expect("sub", gi, load<uint32_t>(o, base + 4), a - b, flushed(fa - fb));
        expect("mul", gi, load<uint32_t>(o, base + 8), a * b, flushed(fa * fb));
        {
          /* Native fp32 division (ftz/any) only has to be within 2.5 ulp (D3D11.3 functional
           * spec, 3.1.4); the exact helper under preserve is correctly rounded. */
          const uint32_t actual = load<uint32_t>(o, base + 12);
          const float exact = a / b, flush_value = flushed(fa / fb);
          const auto within = [&](float want, uint32_t ulps) {
            if (std::isnan(want))
              return std::isnan(bitsFloat(actual));
            const int64_t d = static_cast<int64_t>(floatBits(want)) - static_cast<int64_t>(actual);
            return (floatBits(want) >> 31) == (actual >> 31) && d >= -int64_t(ulps) && d <= int64_t(ulps);
          };
          if (!((mode.preserve && within(exact, mode.flush ? 2 : 0)) || (mode.flush && within(flush_value, 2))))
            mismatch(mode.name, "div", gi, actual, floatBits(mode.preserve ? exact : flush_value));
        }
        /* mad: unfused under preserve (DXIL FMad may be split); ftz/any are fused (air.fma),
         * so the flushed value is the fused one. Products go through volatile so the host
         * compiler cannot contract them into an fma. */
        volatile float ab = a * b, cb = cc * b;
        {
          /* ftz/any fused: flush-to-zero may detect tininess before rounding (the GPU
           * returns ±0 for an exact result just below 2^-126 that rounds up to it). */
          const uint32_t actual = load<uint32_t>(o, base + 16);
          const float fused = std::fma(fa, fb, fc);
          const bool tiny = std::fabs(fused) <= bitsFloat(0x00800000u);
          const bool flush_ok = floatBits(flushed(fused)) == actual ||
                                (tiny && (actual & 0x7fffffffu) == 0) ||
                                (std::isnan(fused) && std::isnan(bitsFloat(actual)));
          const float exact = ab + cc;
          const bool exact_ok = floatBits(exact) == actual || (std::isnan(exact) && std::isnan(bitsFloat(actual)));
          if (!((mode.preserve && exact_ok) || (mode.flush && flush_ok)))
            mismatch(mode.name, "mad", gi, actual, floatBits(mode.preserve ? exact : flushed(fused)));
        }
        /* dot2: an unfused multiply/add chain under preserve. The ftz flavour is Metal's
         * air.dot, whose fusion is not specified, so only the preserve value is exact. */
        if (mode.preserve && !mode.flush)
          expect("dot2", gi, load<uint32_t>(o, base + 20), ab + cb, 0.0f);
        const auto compare = [](float x, float y, float z) {
          return (x < y ? 1u : 0u) | (x == y ? 2u : 0u) | (x == 0.0f ? 4u : 0u) | (x != z ? 8u : 0u) |
                 (x >= z ? 16u : 0u);
        };
        const uint32_t cmp = load<uint32_t>(o, base + 24);
        if (!((mode.preserve && cmp == compare(a, b, cc)) || (mode.flush && cmp == compare(fa, fb, fc))))
          mismatch(mode.name, "compare", gi, cmp, mode.preserve ? compare(a, b, cc) : compare(fa, fb, fc));
        /* Group shared, UAV stores, negation: bit copies in every mode. */
        EXPECT_EQ(mode.name, "groupshared", gi, load<uint32_t>(o, base + 28), in.a[63 - gi]);
        /* 32 denormals: exactly their sum (the lane loop order does not matter, every
         * partial sum is an exact multiple of 2^-149), or 0 when flushed. */
        float exact_sum = 0.0f;
        for (uint32_t l = gi / 32 * 32; l < gi / 32 * 32 + 32; ++l)
          exact_sum += bitsFloat(0x00000100u + l);
        expect("wave_sum", gi, load<uint32_t>(o, base + 32), exact_sum, 0.0f);
        EXPECT_EQ(mode.name, "neg", gi, load<uint32_t>(o, base + 36), in.a[gi] ^ 0x80000000u);
        EXPECT_EQ(mode.name, "store", gi, load<uint32_t>(o, base + 40), in.a[gi]);
        EXPECT_EQ(mode.name, "untouched", gi, load<uint32_t>(o, base + 44), kSentinel);
      }
    }
    end(mode.name);
  }
  _mm_setcsr(saved);
}

/* The capabilities this device reports; the runner gates the exact line. */
void printCaps(ID3D12Device *device) {
  D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
  const HRESULT sm_hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
  D3D12_FEATURE_DATA_D3D12_OPTIONS o0{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof(o0)), "CheckFeatureSupport(OPTIONS)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)), "CheckFeatureSupport(OPTIONS1)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3)), "CheckFeatureSupport(OPTIONS3)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4)), "CheckFeatureSupport(OPTIONS4)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9)), "CheckFeatureSupport(OPTIONS9)");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &o11, sizeof(o11)),
        "CheckFeatureSupport(OPTIONS11)");
  std::printf("OWNED_D3D12_SM6_CAPS shader_model=0x%x/0x%lx double=%d minprec=%d wave=%d lanes=%u-%u total_lanes=%u "
              "int64=%d barycentrics=%d view_instancing=%d native16=%d atomic64_typed=%d atomic64_groupshared=%d "
              "atomic64_heap=%d wave_mma=%d\n",
              sm.HighestShaderModel, static_cast<unsigned long>(sm_hr), o0.DoublePrecisionFloatShaderOps,
              o0.MinPrecisionSupport, o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax, o1.TotalLaneCount,
              o1.Int64ShaderOps, o3.BarycentricsSupported, o3.ViewInstancingTier, o4.Native16BitShaderOpsSupported,
              o9.AtomicInt64OnTypedResourceSupported, o9.AtomicInt64OnGroupSharedSupported,
              o11.AtomicInt64OnDescriptorHeapResourceSupported, o9.WaveMMATier);
}

int run(int argc, wchar_t **argv) {
  int reps = 20;
  bool waves_only = false;
  bool wide_waves_only = false;
  bool fp64_only = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::wcscmp(argv[i], L"--stress-reps") && i + 1 < argc) {
      reps = _wtoi(argv[++i]);
    } else if (!std::wcscmp(argv[i], L"--waves-only")) {
      waves_only = true;
    } else if (!std::wcscmp(argv[i], L"--wide-waves-only")) {
      wide_waves_only = true;
    } else if (!std::wcscmp(argv[i], L"--fp64-only")) {
      fp64_only = true;
    } else {
      std::printf("usage: owned-d3d12-sm6.exe [--stress-reps N] [--waves-only] [--wide-waves-only] [--fp64-only]\n");
      return 2;
    }
  }
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId) != 0)
    die("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_SM6_BACKEND id=%s\n", backend());

  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  printCaps(device.Get());

  Gpu gpu(device.Get());
  Compute compute{gpu, computeRoot(device.Get()), nullptr};
  if (wide_waves_only) {
    testWave(compute);
    testWideWaves(compute);
    begin();
    for (const char *entry : {"cs_wavesize64_divergent", "cs_wavesize128_divergent", "cs_wavesize64_return"}) {
      ComPtr<ID3D12PipelineState> pso;
      const HRESULT hr = computePso(gpu.device.Get(), compute.root.Get(), shader(entry), pso);
      std::printf("OWNED_D3D12_WAVES_REJECT entry=%s hr=0x%08lx\n", entry, static_cast<unsigned long>(hr));
      if (hr != E_FAIL || pso)
        mismatch("wide_wave_requirements", entry, 0, static_cast<uint32_t>(hr), static_cast<uint32_t>(E_FAIL));
    }
    auto widths = compute.run("cs_wavesize32", filled(128), filled(64), filled(64), {0, 0, 0, 0}, 1);
    if (!widths.empty()) for (uint32_t i = 0; i < 32; ++i)
      EXPECT_EQ("wide_wave_requirements", "native32", i, load<uint32_t>(widths[0], i * 4), 32);
    end("wide_wave_requirements");
    if (!g_failures) std::puts("OWNED_D3D12_WIDE_WAVES_PASS");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
  }
  if (fp64_only) {
    testFp64(compute, false);
    testFp64(compute, true);
    testFp64Wave(compute);
    Graphics graphics{gpu, graphicsRoot(device.Get())};
    testFp64Graphics(graphics);
    if (!g_failures) std::puts("OWNED_D3D12_FP64_PASS cases=8");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
  }
  compute.texture = mipTexture(gpu);
  clearLodShapeSrvs(gpu);
  gpu.finish({}, nullptr);
  Graphics graphics{gpu, graphicsRoot(device.Get())};

  testWave(compute);
  testWave(compute, "cs_wave4", 4, "wave4");
  testWave(compute, "cs_wave8", 8, "wave8");
  testWave(compute, "cs_wave16", 16, "wave16");
  testWave(compute, "cs_wave_range4", 4, "wave_range4");
  testWave(compute, "cs_wave_range8", 8, "wave_range8");
  testWave(compute, "cs_wave_range16", 16, "wave_range16");
  testWave(compute, "cs_wave_range32", 32, "wave_range32");
  testWave(compute, "cs_wave_range64", 32, "wave_range64_unavailable");
  testWave(compute, "cs_wave_range16max", 16, "wave_range16max");
  testWave(compute, "cs_wave_sm68fixed4", 4, "wave_sm68fixed4");
  testDivergent(compute);
  testDivergent(compute, "cs_divergent4", 4, "divergent4");
  testDivergent(compute, "cs_divergent8", 8, "divergent8");
  testDivergent(compute, "cs_divergent16", 16, "divergent16");
  testInt64(compute);
  testInt64(compute, "cs_int64wave4", 4, "int64wave4");
  testInt64(compute, "cs_int64wave8", 8, "int64wave8");
  testInt64(compute, "cs_int64wave16", 16, "int64wave16");
  if (waves_only) {
    begin();
    for (const char *entry : {"cs_wavesize64_divergent", "cs_wavesize128_divergent", "cs_wavesize64_return"}) {
      ComPtr<ID3D12PipelineState> pso;
      const HRESULT hr = computePso(gpu.device.Get(), compute.root.Get(), shader(entry), pso);
      std::printf("OWNED_D3D12_WAVES_REJECT entry=%s hr=0x%08lx\n", entry, static_cast<unsigned long>(hr));
      // Shader initialization reports unsatisfied execution contracts as E_FAIL.
      if (hr != E_FAIL || pso)
        mismatch("wave_requirements", entry, 0, static_cast<uint32_t>(hr), static_cast<uint32_t>(E_FAIL));
    }
    auto widths = compute.run("cs_wavesize32", filled(128), filled(64), filled(64), {0, 0, 0, 0}, 1);
    if (!widths.empty())
      for (uint32_t i = 0; i < 32; ++i)
        EXPECT_EQ("wave_requirements", "fixed32", i, load<uint32_t>(widths[0], i * 4), 32);
    end("wave_requirements");
    testQuadDerivatives(compute);
    testHelperLanes(graphics, false);
    testHelperLanes(graphics, true);
    testVertexWave(graphics);
    if (!g_failures)
      std::printf("OWNED_D3D12_WAVES_PASS\n");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
  }
  testAtomic64Ops(compute);
  testAtomic64Heap(gpu);
  test16Bit(compute);
  testMinPrecision(compute);
  testPacked(compute);
  testQuadDerivatives(compute);
  testCalculateLod(compute);
  testCalculateLodShapes(compute);
  testWideWaves(compute);
  testRejections(compute);
  testBarycentrics(graphics, false);
  testBarycentrics(graphics, true);
  testHelperLanes(graphics, false);
  testHelperLanes(graphics, true);
  test16BitIo(graphics);
  testGraphicsInt64(graphics);
  testVertexWave(graphics);
  testViewId(graphics);
  testViewIdInstanced(graphics);
  testDenorm(compute);
  testFp64(compute, false);
  testFp64(compute, true);
  testFp64Wave(compute);
  testFp64Graphics(graphics);
  testStress(compute, reps);

  if (!g_failures)
    std::printf("OWNED_D3D12_SM6_PASS stress_reps=%d\n", reps);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) { return run(argc, argv); }
