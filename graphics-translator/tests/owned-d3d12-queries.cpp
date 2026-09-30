#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 64;
constexpr UINT64 kFullScreenSamples = UINT64(kWidth) * kHeight;
constexpr DWORD kFenceTimeoutMs = 15000;
constexpr UINT64 kTimestampFrequency = 1000000000ull;
constexpr UINT64 kMetalCounterErrorValue = ~UINT64(0);
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";

[[noreturn]] void fail(const char* step, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "OWNED_D3D12_QUERY_FAIL step=%s hr=0x%08lx\n",
                 step, static_cast<unsigned long>(hr));
    ExitProcess(1);
}

void check(HRESULT hr, const char* step) {
    if (FAILED(hr))
        fail(step, hr);
}

void expectHRESULT(HRESULT actual, HRESULT expected, const char* name) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "OWNED_D3D12_QUERY_HRESULT_MISMATCH case=%s expected=0x%08lx actual=0x%08lx\n",
                     name, static_cast<unsigned long>(expected),
                     static_cast<unsigned long>(actual));
        fail(name, actual);
    }
    std::printf("OWNED_D3D12_QUERY_CASE_PASS case=%s exact_hr=0x%08lx\n",
                name, static_cast<unsigned long>(actual));
}

void requireOwnedBackend() {
    HMODULE module = GetModuleHandleW(L"d3d12.dll");
    if (!module)
        fail("GetModuleHandleW(d3d12)", HRESULT_FROM_WIN32(GetLastError()));
    FARPROC raw = GetProcAddress(module, "OwnedGraphicsBackendId");
    if (!raw)
        fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
    using BackendId = const char* (WINAPI*)();
    const char* id = reinterpret_cast<BackendId>(raw)();
    if (!id || std::strcmp(id, kExpectedBackendId) != 0)
        fail("OwnedGraphicsBackendId mismatch");
    std::printf("OWNED_D3D12_QUERY_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC bufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, UINT64 size,
                                    D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state) {
    const auto heap = heapProperties(heapType);
    const auto desc = bufferDescription(size);
    ComPtr<ID3D12Resource> buffer;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          state, nullptr, IID_PPV_ARGS(&buffer)),
          "CreateCommittedResource(buffer)");
    return buffer;
}

ComPtr<ID3D12Resource> createRenderTarget(ID3D12Device* device) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear.Color[3] = 1.0f;
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          &clear, IID_PPV_ARGS(&target)),
          "CreateCommittedResource(render target)");
    return target;
}

ComPtr<ID3DBlob> compileShader(const char* source, const char* entry,
                               const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(
        source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        fail("D3DCompile", hr);
    }
    return code;
}

struct Pipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
};

Pipeline createPipeline(ID3D12Device* device) {
    static constexpr char shader[] = R"hlsl(
struct VSOut { float4 position : SV_Position; };
VSOut vsMain(uint vertexId : SV_VertexID) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    VSOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}
float4 psMain(VSOut input) : SV_Target0 {
    return float4(0.25, 0.5, 0.75, 1.0);
}
)hlsl";

    const ComPtr<ID3DBlob> vs = compileShader(shader, "vsMain", "vs_5_0");
    const ComPtr<ID3DBlob> ps = compileShader(shader, "psMain", "ps_5_0");

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3DBlob> rootErrors;
    const HRESULT serialized = D3D12SerializeRootSignature(
        &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors);
    if (FAILED(serialized)) {
        if (rootErrors)
            std::fwrite(rootErrors->GetBufferPointer(), 1, rootErrors->GetBufferSize(), stderr);
        fail("D3D12SerializeRootSignature", serialized);
    }

    Pipeline pipeline;
    check(device->CreateRootSignature(
              0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
              IID_PPV_ARGS(&pipeline.root)),
          "CreateRootSignature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = pipeline.root.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline.pso)),
          "CreateGraphicsPipelineState");
    return pipeline;
}

struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

Recording createRecording(
    ID3D12Device* device,
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
    Recording recording;
    check(device->CreateCommandAllocator(
              type, IID_PPV_ARGS(&recording.allocator)),
          "CreateCommandAllocator");
    check(device->CreateCommandList(
              0, type, recording.allocator.Get(), nullptr,
              IID_PPV_ARGS(&recording.list)),
          "CreateCommandList");
    return recording;
}

ComPtr<ID3D12QueryHeap> createQueryHeap(ID3D12Device* device,
                                        D3D12_QUERY_HEAP_TYPE type,
                                        UINT count) {
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = type;
    desc.Count = count;
    desc.NodeMask = 1;
    ComPtr<ID3D12QueryHeap> heap;
    check(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)), "CreateQueryHeap(valid)");
    return heap;
}

void waitFence(ID3D12Fence* fence, UINT64 value) {
    if (fence->GetCompletedValue() >= value)
        return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    const HRESULT hr = fence->SetEventOnCompletion(value, event);
    if (FAILED(hr)) {
        CloseHandle(event);
        fail("SetEventOnCompletion", hr);
    }
    const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
    const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0)
        fail("GPU query fence timeout", HRESULT_FROM_WIN32(error));
}

void runCreationNegatives(ID3D12Device* device) {
    void* output = reinterpret_cast<void*>(UINT_PTR(1));
    expectHRESULT(device->CreateQueryHeap(nullptr, __uuidof(ID3D12QueryHeap), &output),
                  E_INVALIDARG, "query_heap_null_desc");
    if (output)
        fail("null query heap desc did not clear output");

    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.NodeMask = 1;
    output = reinterpret_cast<void*>(UINT_PTR(1));
    expectHRESULT(device->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), &output),
                  E_INVALIDARG, "query_heap_zero_count");
    if (output)
        fail("zero-count query heap returned object");

    desc.Count = 1;
    desc.NodeMask = 2;
    output = reinterpret_cast<void*>(UINT_PTR(1));
    expectHRESULT(device->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), &output),
                  E_INVALIDARG, "query_heap_node_mask2");
    if (output)
        fail("invalid node mask query heap returned object");

}

void recordDraw(ID3D12GraphicsCommandList* list,
                const Pipeline& pipeline,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                UINT vertexCount) {
    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight),
        0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(pipeline.root.Get());
    list->SetPipelineState(pipeline.pso.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(vertexCount, 1, 0, 0);
}

void recordVisibleDraw(ID3D12GraphicsCommandList* list,
                       const Pipeline& pipeline,
                       D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    recordDraw(list, pipeline, rtv, 3);
}

void recordEmptyDraw(ID3D12GraphicsCommandList* list,
                     const Pipeline& pipeline,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    recordDraw(list, pipeline, rtv, 0);
}

int run() {
    requireOwnedBackend();

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)),
          "CreateCommandQueue(DIRECT)");

    expectHRESULT(queue->GetTimestampFrequency(nullptr), E_INVALIDARG,
                  "timestamp_frequency_null_output");
    UINT64 frequency = 0;
    check(queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency");
    if (frequency != kTimestampFrequency)
        fail("timestamp frequency is not 1 GHz");
    std::printf("OWNED_D3D12_QUERY_FREQUENCY ticks_per_second=%llu tick_ns=1\n",
                static_cast<unsigned long long>(frequency));

    UINT64 calibratedGpu = 0;
    UINT64 calibratedCpu = 0;
    expectHRESULT(queue->GetClockCalibration(nullptr, &calibratedCpu), E_INVALIDARG,
                  "clock_calibration_null_gpu");
    expectHRESULT(queue->GetClockCalibration(&calibratedGpu, nullptr), E_INVALIDARG,
                  "clock_calibration_null_cpu");
    LARGE_INTEGER qpcBefore{}, qpcAfter{};
    if (!QueryPerformanceCounter(&qpcBefore))
        fail("QueryPerformanceCounter(before calibration)");
    check(queue->GetClockCalibration(&calibratedGpu, &calibratedCpu),
          "GetClockCalibration");
    if (!QueryPerformanceCounter(&qpcAfter))
        fail("QueryPerformanceCounter(after calibration)");
    if (!calibratedGpu || calibratedCpu < static_cast<UINT64>(qpcBefore.QuadPart) ||
        calibratedCpu > static_cast<UINT64>(qpcAfter.QuadPart))
        fail("clock calibration did not return a paired GPU/Wine-QPC sample");
    UINT64 nextGpu = 0;
    UINT64 nextCpu = 0;
    check(queue->GetClockCalibration(&nextGpu, &nextCpu),
          "GetClockCalibration(second)");
    if (nextGpu < calibratedGpu || nextCpu < calibratedCpu)
        fail("clock calibration samples are not monotonic");
    std::printf("OWNED_D3D12_QUERY_CLOCK_CALIBRATION gpu=%llu cpu_qpc=%llu monotonic=1\n",
                static_cast<unsigned long long>(calibratedGpu),
                static_cast<unsigned long long>(calibratedCpu));

    D3D12_COMMAND_QUEUE_DESC copyQueueDesc{};
    copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ComPtr<ID3D12CommandQueue> copyQueue;
    check(device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&copyQueue)),
          "CreateCommandQueue(COPY)");
    UINT64 copyFrequency = 0;
    expectHRESULT(copyQueue->GetTimestampFrequency(&copyFrequency), E_FAIL,
                  "copy_queue_timestamp_frequency_unsupported");
    expectHRESULT(copyQueue->GetClockCalibration(&calibratedGpu, &calibratedCpu), E_FAIL,
                  "copy_queue_clock_calibration_unsupported");

    runCreationNegatives(device.Get());

    Pipeline pipeline = createPipeline(device.Get());
    ComPtr<ID3D12Resource> target = createRenderTarget(device.Get());

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)),
          "CreateDescriptorHeap(RTV)");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    ComPtr<ID3D12QueryHeap> timestampHeap =
        createQueryHeap(device.Get(), D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2);
    ComPtr<ID3D12QueryHeap> visibilityHeapA =
        createQueryHeap(device.Get(), D3D12_QUERY_HEAP_TYPE_OCCLUSION, 3);
    ComPtr<ID3D12QueryHeap> visibilityHeapB =
        createQueryHeap(device.Get(), D3D12_QUERY_HEAP_TYPE_OCCLUSION, 3);

    ComPtr<ID3D12Resource> timestampReadback =
        createBuffer(device.Get(), 2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                     D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> visibilityReadback =
        createBuffer(device.Get(), 6 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                     D3D12_RESOURCE_STATE_COPY_DEST);

    {
        Recording invalid = createRecording(device.Get());
        invalid.list->BeginQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "begin_timestamp_query_rejected");
        expectHRESULT(invalid.allocator->Reset(), S_OK,
                      "failed_query_close_releases_allocator_slot");
        expectHRESULT(invalid.list->Reset(invalid.allocator.Get(), nullptr), E_INVALIDARG,
                      "failed_query_close_remains_poisoned");
    }
    {
        Recording invalid = createRecording(device.Get());
        invalid.list->EndQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "end_occlusion_without_begin_rejected");
    }
    {
        Recording invalid = createRecording(device.Get());
        invalid.list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        invalid.list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "restart_same_occlusion_element_rejected");
    }
    {
        Recording invalid = createRecording(device.Get());
        invalid.list->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       0, 1, timestampReadback.Get(), 4);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "resolve_query_misaligned_offset_rejected");
    }
    {
        Recording invalid = createRecording(device.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
        invalid.list->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "copy_list_timestamp_end_rejected");
    }
    {
        Recording invalid = createRecording(device.Get());
        invalid.list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "unfinished_query_close_rejected");
        expectHRESULT(invalid.allocator->Reset(), S_OK,
                      "unfinished_query_close_releases_allocator_slot");
    }
    {
        Recording invalid = createRecording(device.Get());
        invalid.list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        invalid.list->ResolveQueryData(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       0, 1, visibilityReadback.Get(), 0);
        invalid.list->EndQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "resolve_active_query_rejected");
    }
    {
        Recording valid = createRecording(device.Get());
        valid.list->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     2, 0, timestampReadback.Get(), 16);
        expectHRESULT(valid.list->Close(), S_OK, "zero_count_resolve_valid_noop");
    }
    {
        Recording invalid = createRecording(device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
        invalid.list->ResolveQueryData(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       0, 1, visibilityReadback.Get(), 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG,
                      "compute_list_occlusion_resolve_rejected");
    }
    {
        Recording closed = createRecording(device.Get());
        check(closed.list->Close(), "closed-list query guard initial Close");
        closed.list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        closed.list->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        closed.list->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                      0, 1, timestampReadback.Get(), 0);
        check(closed.list->Reset(closed.allocator.Get(), nullptr),
              "closed-list query guard Reset");
        check(closed.list->Close(), "closed-list query guard final Close");
        std::puts("OWNED_D3D12_QUERY_CASE_PASS case=closed_list_query_handlers_no_allocator_access");
    }
    {
        ComPtr<ID3D12Device> otherDevice;
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                IID_PPV_ARGS(&otherDevice)),
              "D3D12CreateDevice(other)");
        if (otherDevice.Get() == device.Get())
            fail("cross-device query fixture received same device object");
        ComPtr<ID3D12QueryHeap> foreignHeap =
            createQueryHeap(otherDevice.Get(), D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1);
        ComPtr<ID3D12Resource> foreignReadback =
            createBuffer(otherDevice.Get(), sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST);

        Recording heapMismatch = createRecording(device.Get());
        heapMismatch.list->BeginQuery(
            foreignHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        expectHRESULT(heapMismatch.list->Close(), E_INVALIDARG,
                      "cross_device_query_heap_rejected");

        Recording dstMismatch = createRecording(device.Get());
        dstMismatch.list->ResolveQueryData(
            timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            0, 1, foreignReadback.Get(), 0);
        expectHRESULT(dstMismatch.list->Close(), E_INVALIDARG,
                      "cross_device_query_destination_rejected");
    }

    Recording recording = createRecording(device.Get());
    ID3D12GraphicsCommandList* list = recording.list.Get();

    list->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    // Outer A spans three visible render passes. Cross-heap counting C spans
    // the second and third pass, while binary B is nested in the second pass.
    // Every element is independently started/stopped as D3D12 requires.
    list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    recordVisibleDraw(list, pipeline, rtv);
    list->BeginQuery(visibilityHeapB.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->BeginQuery(visibilityHeapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    recordVisibleDraw(list, pipeline, rtv);
    // ClearState resets graphics state, not an active query. The following
    // draw restores bindings and must add samples to all three active elements.
    list->ClearState(nullptr);
    recordVisibleDraw(list, pipeline, rtv);
    list->EndQuery(visibilityHeapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    recordVisibleDraw(list, pipeline, rtv);
    list->EndQuery(visibilityHeapB.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->EndQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);

    // Preserve an accumulated result across a second render pass that emits no
    // samples. The nested empty query forces a pass break and independently
    // validates a zero counting result.
    list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
    recordVisibleDraw(list, pipeline, rtv);
    list->BeginQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2);
    recordEmptyDraw(list, pipeline, rtv);
    list->EndQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2);
    list->EndQuery(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);

    list->BeginQuery(visibilityHeapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
    list->EndQuery(visibilityHeapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);

    list->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                           0, 2, timestampReadback.Get(), 0);
    list->ResolveQueryData(visibilityHeapA.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                           0, 3, visibilityReadback.Get(), 0);
    list->ResolveQueryData(visibilityHeapB.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                           0, 1, visibilityReadback.Get(), 3 * sizeof(UINT64));
    list->ResolveQueryData(visibilityHeapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION,
                           1, 2, visibilityReadback.Get(), 4 * sizeof(UINT64));
    check(list->Close(), "query command list Close");

    // Query heap COM objects are no longer needed once the list owns the WMT
    // sample/visibility buffers. Release them before submission to prove that
    // recorded query work has its own resource lifetime.
    timestampHeap.Reset();
    visibilityHeapA.Reset();
    visibilityHeapB.Reset();

    ID3D12CommandList* lists[] = {recording.list.Get()};
    queue->ExecuteCommandLists(1, lists);

    // The deferred queue worker snapshots the allocator at Execute. Drop caller
    // references immediately and require query resolves to survive GPU retirement.
    recording.list.Reset();
    recording.allocator.Reset();

    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
          "CreateFence");
    check(queue->Signal(fence.Get(), 1), "CommandQueue::Signal(query completion)");
    waitFence(fence.Get(), 1);

    D3D12_RANGE timestampRange{0, 2 * sizeof(UINT64)};
    UINT64* timestamps = nullptr;
    check(timestampReadback->Map(0, &timestampRange,
                                 reinterpret_cast<void**>(&timestamps)),
          "timestamp readback Map");
    const UINT64 timestamp0 = timestamps[0];
    const UINT64 timestamp1 = timestamps[1];
    timestampReadback->Unmap(0, nullptr);
    std::printf("OWNED_D3D12_QUERY_TIMESTAMP_VALUES t0=%llu t1=%llu\n",
                static_cast<unsigned long long>(timestamp0), static_cast<unsigned long long>(timestamp1));

    D3D12_RANGE visibilityRange{0, 6 * sizeof(UINT64)};
    UINT64* visibility = nullptr;
    check(visibilityReadback->Map(0, &visibilityRange,
                                  reinterpret_cast<void**>(&visibility)),
          "visibility readback Map");
    const std::array<UINT64, 6> results{
        visibility[0], visibility[1], visibility[2],
        visibility[3], visibility[4], visibility[5]};
    visibilityReadback->Unmap(0, nullptr);
    std::printf("OWNED_D3D12_QUERY_VISIBILITY_VALUES values=%llu,%llu,%llu,%llu,%llu,%llu\n",
                static_cast<unsigned long long>(results[0]), static_cast<unsigned long long>(results[1]),
                static_cast<unsigned long long>(results[2]), static_cast<unsigned long long>(results[3]),
                static_cast<unsigned long long>(results[4]), static_cast<unsigned long long>(results[5]));
    if (!timestamp0 || !timestamp1 || timestamp0 == kMetalCounterErrorValue ||
        timestamp1 == kMetalCounterErrorValue || timestamp1 < timestamp0)
        fail("GPU timestamp samples invalid/nonmonotonic");

    if (results[0] != 4 * kFullScreenSamples)
        fail("outer overlapping occlusion did not accumulate four passes across ClearState");
    if (results[1] != kFullScreenSamples)
        fail("visible-then-empty occlusion did not preserve first pass");
    if (results[2] != 0)
        fail("nested empty counting occlusion returned nonzero");
    if (results[3] != 3 * kFullScreenSamples)
        fail("cross-heap overlapping occlusion did not accumulate three passes across ClearState");
    if (results[4] != 1)
        fail("nested binary occlusion visible draw did not normalize to one");
    if (results[5] != 0)
        fail("binary occlusion empty query returned nonzero");

    std::printf(
        "OWNED_D3D12_QUERY_TIMESTAMP_PASS frequency=%llu t0=%llu t1=%llu delta_ticks=%llu "
        "gpu_counter_samples=1 async_gpu_resolve=1\n",
        static_cast<unsigned long long>(frequency),
        static_cast<unsigned long long>(timestamp0),
        static_cast<unsigned long long>(timestamp1),
        static_cast<unsigned long long>(timestamp1 - timestamp0));
    std::printf(
        "OWNED_D3D12_QUERY_OCCLUSION_PASS outer_four_pass=%llu preserve_visible_empty=%llu "
        "nested_empty=%llu crossheap_three_pass=%llu nested_binary=%llu binary_empty=%llu "
        "fullscreen_samples=%llu overlap=1 crossheap=1 gpu_fanout_accumulate=1\n",
        static_cast<unsigned long long>(results[0]),
        static_cast<unsigned long long>(results[1]),
        static_cast<unsigned long long>(results[2]),
        static_cast<unsigned long long>(results[3]),
        static_cast<unsigned long long>(results[4]),
        static_cast<unsigned long long>(results[5]),
        static_cast<unsigned long long>(kFullScreenSamples));
    std::puts(
        "OWNED_D3D12_QUERY_PASS timestamp=1 occlusion=1 binary_occlusion=1 "
        "frequency_1ghz=1 gpu_resolve=1 release_before_completion=1 no_cpu_timestamp=1 "
        "overlap=1 crossheap=1 visible_empty_preserve=1 cross_device_validation=1 "
        "closed_list_guard=1 copy_timestamp_rejected=1 clear_state_preserves_queries=1 "
        "failed_close_allocator_reuse=1 active_resolve_rejected=1");
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return run();
}
