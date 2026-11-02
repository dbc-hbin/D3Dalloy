#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#define __NVAPI_EMPTY_SAL
#include "nvapi.h"
#undef __NVAPI_EMPTY_SAL
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace {
[[noreturn]] void fail(const char *step, long value = -1) {
  std::fprintf(stderr, "OWNED_NVAPI_REFLEX_FAIL step=%s value=%ld\n", step, value);
  ExitProcess(1);
}
void require(bool condition, const char *step) { if (!condition) fail(step); }
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void status(NvAPI_Status actual, NvAPI_Status expected, const char *step) { if (actual != expected) fail(step, actual); }
UINT64 nowUs() {
  LARGE_INTEGER counter{}, frequency{};
  QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
  return UINT64(counter.QuadPart / frequency.QuadPart) * 1000000 +
         UINT64(counter.QuadPart % frequency.QuadPart) * 1000000 / UINT64(frequency.QuadPart);
}
using SleepFn = NvAPI_Status (__cdecl *)(IUnknown *);
using SetFn = NvAPI_Status (__cdecl *)(IUnknown *, NV_SET_SLEEP_MODE_PARAMS *);
using StatusFn = NvAPI_Status (__cdecl *)(IUnknown *, NV_GET_SLEEP_STATUS_PARAMS *);
using MarkerFn = NvAPI_Status (__cdecl *)(IUnknown *, NV_LATENCY_MARKER_PARAMS *);
using LatencyFn = NvAPI_Status (__cdecl *)(IUnknown *, NV_LATENCY_RESULT_PARAMS *);
struct Api {
  using Query = void *(__cdecl *)(NvU32);
  HMODULE module;
  SleepFn sleep;
  SetFn set;
  StatusFn get;
  MarkerFn marker;
  LatencyFn latency;
  Api() {
    module = LoadLibraryW(sizeof(void *) == 8 ? L"nvapi64.dll" : L"nvapi.dll");
    require(module != nullptr, "load-nvapi");
    auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
    require(query != nullptr, "query-export");
    sleep = reinterpret_cast<SleepFn>(query(0x852cd1d2));
    set = reinterpret_cast<SetFn>(query(0xac1ca9e0));
    get = reinterpret_cast<StatusFn>(query(0xaef96ca1));
    marker = reinterpret_cast<MarkerFn>(query(0xd9984c05));
    latency = reinterpret_cast<LatencyFn>(query(0x1a587f9c));
    require(sleep && set && get && marker && latency, "entry-points");
    auto enumerate = reinterpret_cast<decltype(&NvAPI_EnumPhysicalGPUs)>(query(0xe5ac921f));
    using AdapterIdFn = NvAPI_Status (__cdecl *)(NvPhysicalGpuHandle, void *);
    auto adapterId = reinterpret_cast<AdapterIdFn>(query(0x0ff07fde));
    require(enumerate && adapterId, "adapter-entry-points");
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS]{};
    NvU32 count = 0;
    status(enumerate(handles, &count), NVAPI_OK, "enumerate-adapters");
    require(count > 0 && count <= NVAPI_MAX_PHYSICAL_GPUS, "adapter-count");
    LUID luid{};
    status(adapterId(handles[0], &luid), NVAPI_OK, "physical-adapter-id");
    require(luid.LowPart || luid.HighPart, "physical-adapter-luid");
    auto logicalFromPhysical = reinterpret_cast<decltype(&NvAPI_GetLogicalGPUFromPhysicalGPU)>(query(0xadd604d1));
    auto physicalFromLogical = reinterpret_cast<decltype(&NvAPI_GetPhysicalGPUsFromLogicalGPU)>(query(0xaea3fa32));
    auto logicalInfo = reinterpret_cast<decltype(&NvAPI_GPU_GetLogicalGpuInfo)>(query(0x842b066e));
    require(logicalFromPhysical && physicalFromLogical && logicalInfo, "logical-adapter-entry-points");
    NvLogicalGpuHandle logical{};
    status(logicalFromPhysical(handles[0], &logical), NVAPI_OK, "logical-from-physical");
    NvPhysicalGpuHandle roundtrip[NVAPI_MAX_PHYSICAL_GPUS]{};
    status(physicalFromLogical(logical, roundtrip, &count), NVAPI_OK, "physical-from-logical");
    require(count == 1 && roundtrip[0] == handles[0], "physical-handle-roundtrip");
    LUID logicalLuid{};
    NV_LOGICAL_GPU_DATA data{}; data.version = NV_LOGICAL_GPU_DATA_VER1; data.pOSAdapterId = &logicalLuid;
    status(logicalInfo(logical, &data), NVAPI_OK, "logical-adapter-info");
    require(data.physicalGpuCount == 1 && data.physicalGpuHandles[0] == handles[0] &&
            logicalLuid.LowPart == luid.LowPart && logicalLuid.HighPart == luid.HighPart, "logical-adapter-luid");
    status(adapterId(handles[0], nullptr), NVAPI_INVALID_ARGUMENT, "null-adapter-output");
    const LUID sentinel = luid;
    status(adapterId(nullptr, &luid), NVAPI_INVALID_ARGUMENT, "invalid-physical-handle");
    require(luid.LowPart == sentinel.LowPart && luid.HighPart == sentinel.HighPart, "invalid-handle-output-preserved");
    std::puts("OWNED_NVAPI_ADAPTER_PASS valid=1 null_output=1 invalid_handle=1 output_preserved=1");
  }
  ~Api() { FreeLibrary(module); }
};
struct SleepAttempt { SleepFn function; IUnknown *device; HANDLE started, done; NvAPI_Status result; };
DWORD WINAPI sleepThread(void *data) {
  auto &attempt = *static_cast<SleepAttempt *>(data);
  SetEvent(attempt.started);
  attempt.result = attempt.function(attempt.device);
  SetEvent(attempt.done);
  return 0;
}
void idle(ID3D12Device *device, ID3D12CommandQueue *queue) {
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "idle-fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  check(queue->Signal(fence.Get(), 1), "idle-signal");
  check(fence->SetEventOnCompletion(1, event), "idle-event");
  require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "idle-completion");
  CloseHandle(event);
}
}
int main() {
  Api api;
  ComPtr<ID3D12Device> device, other;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&other)), "other-device");
  using BackendId = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned-backend");
  std::printf("OWNED_NVAPI_REFLEX_BACKEND id=owned-graphics-d3d12\n");
  NV_SET_SLEEP_MODE_PARAMS mode{}; mode.version = NV_SET_SLEEP_MODE_PARAMS_VER1;
  mode.minimumIntervalUs = 30000;
  status(api.set(device.Get(), &mode), NVAPI_OK, "interval-independent-of-low-latency");
  NV_GET_SLEEP_STATUS_PARAMS current{}; current.version = NV_GET_SLEEP_STATUS_PARAMS_VER1;
  status(api.get(other.Get(), &current), NVAPI_OK, "other-status");
  require(!current.bLowLatencyMode && !current.bUseGameSleep && !current.sleepIntervalUs, "device-isolation");
  status(api.sleep(device.Get()), NVAPI_OK, "first-sleep");
  const auto before = nowUs();
  status(api.sleep(device.Get()), NVAPI_OK, "paced-sleep");
  const auto elapsed = nowUs() - before;
  require(elapsed >= 25000 && elapsed < 1000000, "real-interval-wait");
  status(api.get(device.Get(), &current), NVAPI_OK, "sleep-status");
  require(current.bUseGameSleep && current.sleepIntervalUs >= 25000 && !current.bLowLatencyMode, "actual-sleep-status");
  mode.version = 0;
  status(api.set(device.Get(), &mode), NVAPI_INCOMPATIBLE_STRUCT_VERSION, "mode-version");
  mode.version = NV_SET_SLEEP_MODE_PARAMS_VER1; mode.rsvd[0] = 1;
  status(api.set(device.Get(), &mode), NVAPI_INVALID_ARGUMENT, "mode-reserved");
  mode.rsvd[0] = 0; mode.bLowLatencyBoost = 1;
  status(api.set(device.Get(), &mode), NVAPI_NOT_SUPPORTED, "boost-not-faked");
  mode.bLowLatencyBoost = 0; mode.minimumIntervalUs = 0; mode.bLowLatencyMode = 1;
  status(api.sleep(nullptr), NVAPI_INVALID_ARGUMENT, "null-device");
  status(api.set(device.Get(), &mode), NVAPI_OK, "low-latency");
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  check(list->Close(), "close-empty");
  ComPtr<ID3D12Fence> gate;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  check(queue->Wait(gate.Get(), 1), "blocked-queue");
  ID3D12CommandList *lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
  SleepAttempt attempt{api.sleep, device.Get(), CreateEventW(nullptr, TRUE, FALSE, nullptr),
                        CreateEventW(nullptr, TRUE, FALSE, nullptr), NVAPI_ERROR};
  HANDLE thread = CreateThread(nullptr, 0, sleepThread, &attempt, 0, nullptr);
  require(thread && WaitForSingleObject(attempt.started, 10000) == WAIT_OBJECT_0, "sleep-thread-start");
  // A single pending render frame must retain CPU/GPU overlap. Admission is
  // frame-bounded, not a drain of every Execute/native command buffer.
  require(WaitForSingleObject(attempt.done, 10000) == WAIT_OBJECT_0, "sleep-preserves-render-overlap");
  status(api.sleep(other.Get()), NVAPI_OK, "other-device-not-blocked");
  check(gate->Signal(1), "release-queue");
  status(attempt.result, NVAPI_OK, "sleep-thread-result");
  WaitForSingleObject(thread, 10000); CloseHandle(thread); CloseHandle(attempt.started); CloseHandle(attempt.done);
  idle(device.Get(), queue.Get());
  mode.bLowLatencyMode = 0;
  status(api.set(device.Get(), &mode), NVAPI_OK, "release-offscreen-admission");
  mode.bLowLatencyMode = 1;
  status(api.set(device.Get(), &mode), NVAPI_OK, "restore-frame-admission");
  NV_LATENCY_RESULT_PARAMS report{}; report.version = NV_LATENCY_RESULT_PARAMS_VER1;
  status(api.latency(device.Get(), &report), NVAPI_OK, "specified-warmup-report");
  for (const auto &r : report.frameReport) require(!r.simStartTime && !r.gpuRenderEndTime, "warmup-zero");
  WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"OwnedReflexConsumer";
  require(RegisterClassW(&cls) != 0, "window-class");
  HWND window = CreateWindowW(cls.lpszClassName, L"owned-reflex", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr, cls.hInstance, nullptr);
  require(window != nullptr, "window"); ShowWindow(window, SW_SHOWNA);
  ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = desc.Height = 32; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapchain;
  check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swapchain), "swapchain");
  ComPtr<IDXGISwapChain3> chain; check(swapchain.As(&chain), "chain3");
  D3D12_DESCRIPTOR_HEAP_DESC heapdesc{}; heapdesc.NumDescriptors = 2; heapdesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapdesc, IID_PPV_ARGS(&heap)), "rtv-heap");
  ComPtr<ID3D12Resource> buffers[2];
  auto base = heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = 0; i < 2; ++i) {
    check(chain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "buffer");
    D3D12_CPU_DESCRIPTOR_HANDLE handle{base.ptr + i * stride};
    device->CreateRenderTargetView(buffers[i].Get(), nullptr, handle);
  }
  UINT64 marker_lower[90]{}, marker_upper[90]{};
  for (UINT64 id = 1000; id < 1090; ++id) {
    status(api.sleep(device.Get()), NVAPI_OK, "frame-sleep");
    marker_lower[id - 1000] = nowUs();
    auto mark = [&](NV_LATENCY_MARKER_TYPE type) {
      NV_LATENCY_MARKER_PARAMS marker{}; marker.version = NV_LATENCY_MARKER_PARAMS_VER1;
      marker.frameID = id; marker.markerType = type;
      status(api.marker(device.Get(), &marker), NVAPI_OK, "marker");
    };
    mark(SIMULATION_START); mark(INPUT_SAMPLE); mark(SIMULATION_END); mark(RENDERSUBMIT_START);
    check(allocator->Reset(), "reset-allocator"); check(list->Reset(allocator.Get(), nullptr), "reset-list");
    const UINT index = chain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffers[index].Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{base.ptr + index * stride}; const float color[] = {0.1f, 0.2f, 0.3f, 1};
    list->ClearRenderTargetView(handle, color, 0, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier); check(list->Close(), "close-frame");
    queue->ExecuteCommandLists(1, lists); mark(RENDERSUBMIT_END); mark(PRESENT_START);
    check(chain->Present(0, 0), "present"); mark(PRESENT_END);
    marker_upper[id - 1000] = nowUs(); idle(device.Get(), queue.Get());
  }
  status(api.latency(device.Get(), &report), NVAPI_OK, "measured-latency-report");
  for (UINT i = 0; i < 64; ++i) {
    const auto &r = report.frameReport[i]; require(r.frameID == 1026 + i, "newest-at-index63");
    const auto lo = marker_lower[r.frameID - 1000], hi = marker_upper[r.frameID - 1000];
    require(r.simStartTime >= lo && r.presentEndTime <= hi, "actual-marker-clock-domain");
    require(r.simStartTime <= r.inputSampleTime && r.inputSampleTime <= r.simEndTime &&
            r.simEndTime <= r.renderSubmitStartTime && r.renderSubmitStartTime <= r.renderSubmitEndTime &&
            r.renderSubmitEndTime <= r.presentStartTime && r.presentStartTime <= r.presentEndTime, "marker-order");
    require(r.driverStartTime && r.driverEndTime >= r.driverStartTime && r.osRenderQueueStartTime &&
            r.osRenderQueueEndTime >= r.osRenderQueueStartTime, "real-driver-queue-times");
    require(r.gpuRenderStartTime && r.gpuRenderEndTime >= r.gpuRenderStartTime &&
            r.gpuRenderStartTime >= lo && r.gpuRenderEndTime <= nowUs(), "real-native-gpu-times");
  }
  // The public contract preserves the requested interval even when the game
  // omits Sleep. This must work with the launcher force option left off.
  mode.bLowLatencyMode = 0; mode.minimumIntervalUs = 30000;
  status(api.set(device.Get(), &mode), NVAPI_OK, "fallback-interval-mode");
  check(chain->Present(0, 0), "fallback-first-present");
  const auto fallback_start = nowUs();
  check(chain->Present(0, 0), "fallback-second-present");
  require(nowUs() - fallback_start >= 25000, "fallback-preserves-requested-interval");
  mode.minimumIntervalUs = 0;
  status(api.set(device.Get(), &mode), NVAPI_OK, "fallback-disable");
  status(api.latency(other.Get(), &report), NVAPI_OK, "report-device-isolation");
  for (const auto &r : report.frameReport) require(!r.simStartTime && !r.gpuRenderEndTime, "other-device-zero");
  NV_LATENCY_MARKER_PARAMS duplicate{}; duplicate.version = NV_LATENCY_MARKER_PARAMS_VER1;
  duplicate.frameID = 1089; duplicate.markerType = SIMULATION_START;
  status(api.marker(device.Get(), &duplicate), NVAPI_INVALID_ARGUMENT, "strict-frame-id");
  duplicate.frameID = 1090; duplicate.markerType = SIMULATION_END;
  status(api.marker(device.Get(), &duplicate), NVAPI_INVALID_ARGUMENT, "simulation-start-required");
  duplicate.frameID = 1089;
  duplicate.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(10);
  status(api.marker(device.Get(), &duplicate), NVAPI_INVALID_ARGUMENT, "out-of-band-render-start-required");
  duplicate.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(12);
  status(api.marker(device.Get(), &duplicate), NVAPI_INVALID_ARGUMENT, "out-of-band-present-start-required");
  for (UINT type = 9; type <= 12; ++type) {
    duplicate.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(type);
    status(api.marker(device.Get(), &duplicate), NVAPI_OK, "out-of-band-existing-frame");
  }
  status(api.marker(device.Get(), &duplicate), NVAPI_INVALID_ARGUMENT, "out-of-band-duplicate");
  duplicate.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(99);
  status(api.marker(device.Get(), &duplicate), NVAPI_INVALID_ARGUMENT, "unknown-marker");
  duplicate.version = 0; duplicate.markerType = SIMULATION_START;
  status(api.marker(device.Get(), &duplicate), NVAPI_INCOMPATIBLE_STRUCT_VERSION, "marker-version");
  mode.bLowLatencyMode = 0; status(api.set(device.Get(), &mode), NVAPI_OK, "disable-mode");
  status(api.get(device.Get(), &current), NVAPI_OK, "disabled-status"); require(!current.bLowLatencyMode, "disabled");
  // A wait may be canceled by device removal, never converted into successful GPU completion.
  mode.bLowLatencyMode = 0; mode.minimumIntervalUs = 5000000;
  status(api.set(other.Get(), &mode), NVAPI_OK, "removal-mode");
  status(api.sleep(other.Get()), NVAPI_OK, "removal-interval-anchor");
  ComPtr<ID3D12CommandQueue> removal_queue;
  check(other->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&removal_queue)), "removal-queue");
  ComPtr<ID3D12CommandAllocator> removal_allocator; ComPtr<ID3D12GraphicsCommandList> removal_list;
  check(other->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&removal_allocator)), "removal-allocator");
  check(other->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, removal_allocator.Get(), nullptr,
                               IID_PPV_ARGS(&removal_list)), "removal-list");
  check(removal_list->Close(), "removal-close");
  ComPtr<ID3D12Fence> removal_gate;
  check(other->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removal_gate)), "removal-gate");
  check(removal_queue->Wait(removal_gate.Get(), 1), "removal-queue-wait");
  ID3D12CommandList *removal_lists[] = {removal_list.Get()}; removal_queue->ExecuteCommandLists(1, removal_lists);
  SleepAttempt canceled{api.sleep, other.Get(), CreateEventW(nullptr, TRUE, FALSE, nullptr),
                       CreateEventW(nullptr, TRUE, FALSE, nullptr), NVAPI_OK};
  HANDLE canceled_thread = CreateThread(nullptr, 0, sleepThread, &canceled, 0, nullptr);
  require(canceled_thread && WaitForSingleObject(canceled.started, 10000) == WAIT_OBJECT_0, "removal-sleep-start");
  ComPtr<ID3D12Device5> device5; check(other.As(&device5), "removal-device5"); device5->RemoveDevice();
  require(WaitForSingleObject(canceled.done, 10000) == WAIT_OBJECT_0, "removal-wakes-sleep");
  status(canceled.result, NVAPI_ERROR, "removal-not-success");
  WaitForSingleObject(canceled_thread, 10000); CloseHandle(canceled_thread);
  CloseHandle(canceled.started); CloseHandle(canceled.done);
  chain.Reset(); swapchain.Reset(); DestroyWindow(window); UnregisterClassW(cls.lpszClassName, cls.hInstance);
  std::printf("OWNED_NVAPI_REFLEX_PASS cases=8 frames=90 reports=64 real_sleep_us=%llu\n", elapsed);
  return 0;
}
