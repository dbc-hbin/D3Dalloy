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

void runVisibilityIntervals(ID3D12Device* device, ID3D12CommandQueue* queue,
                            const Pipeline& pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    constexpr UINT64 guard = 0x1234000000000000ull;
    constexpr UINT words = 11;
    auto heapA = createQueryHeap(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2);
    auto heapB = createQueryHeap(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, 3);
    auto upload = createBuffer(device, words * sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = createBuffer(device, words * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64* initial = nullptr;
    D3D12_RANGE noRead{0, 0};
    check(upload->Map(0, &noRead, reinterpret_cast<void**>(&initial)), "interval upload Map");
    for (UINT i = 0; i < words; ++i)
        initial[i] = guard + i;
    upload->Unmap(0, nullptr);
    Recording commands = createRecording(device);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
          "CreateFence(intervals)");
    for (unsigned execution = 0; execution < 3; ++execution) {
        // Execution 2 replays the exact closed list, without allocator/list Reset.
        if (execution < 2) {
            if (execution) {
                check(commands.allocator->Reset(), "interval allocator Reset");
                check(commands.list->Reset(commands.allocator.Get(), nullptr), "interval list Reset");
            }
            std::fprintf(stderr, "OWNED_D3D12_QUERY_INTERVAL_PHASE execution=%u phase=record_begin\n", execution);
            auto* list = commands.list.Get();
            const float clear[4]{0, 0, 0, 1};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            const D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->SetGraphicsRootSignature(pipeline.root.Get());
            list->SetPipelineState(pipeline.pso.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const auto draw = [&](LONG width, LONG row, unsigned repeats) {
                const D3D12_RECT scissor{0, row, width, row + 1};
                list->RSSetScissorRects(1, &scissor);
                for (unsigned i = 0; i < repeats; ++i)
                    list->DrawInstanced(3, 1, 0, 0);
            };
            // No barriers, clears, binding changes or ClearState within this
            // ordinary render span. Query transitions alone must not split it.
            draw(8, 0, 1); // Disabled pre-Begin: must not enter any query.
            list->BeginQuery(heapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            draw(execution ? 3 : 2, 1, 2); // A-only.
            list->BeginQuery(heapB.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            list->BeginQuery(heapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
            draw(execution ? 2 : 4, 2, 2); // A+B; binary shadows B.
            list->EndQuery(heapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            draw(execution ? 1 : 6, 3, 2); // B-only.
            list->EndQuery(heapB.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
            list->EndQuery(heapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
            draw(8, 4, 1); // Disabled post-End.
            list->BeginQuery(heapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
            list->EndQuery(heapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1);
            list->BeginQuery(heapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
            list->EndQuery(heapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2);
            // Initialize every destination word on GPU; resolve only initialized
            // heap elements, preserving six interleaved guard words exactly.
            list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, words * sizeof(UINT64));
            list->ResolveQueryData(heapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, readback.Get(), 8);
            list->ResolveQueryData(heapB.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, readback.Get(), 24);
            list->ResolveQueryData(heapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1, 1, readback.Get(), 40);
            list->ResolveQueryData(heapA.Get(), D3D12_QUERY_TYPE_OCCLUSION, 1, 1, readback.Get(), 56);
            list->ResolveQueryData(heapB.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 2, 1, readback.Get(), 72);
            check(list->Close(), "interval list Close");
            std::fprintf(stderr, "OWNED_D3D12_QUERY_INTERVAL_PHASE execution=%u phase=record_end\n", execution);
        } else {
            // Recorded query buffers must outlive heap COM objects on replay.
            heapA.Reset();
            heapB.Reset();
        }
        std::fprintf(stderr, "OWNED_D3D12_QUERY_INTERVAL_PHASE execution=%u phase=submit_begin\n", execution);
        ID3D12CommandList* lists[]{commands.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), execution + 1), "Signal(interval completion)");
        waitFence(fence.Get(), execution + 1);
        std::fprintf(stderr, "OWNED_D3D12_QUERY_INTERVAL_PHASE execution=%u phase=submit_end\n", execution);
        const D3D12_RANGE range{0, words * sizeof(UINT64)};
        UINT64* mapped = nullptr;
        check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "interval readback Map");
        std::array<UINT64, words> actual{};
        std::memcpy(actual.data(), mapped, sizeof(actual));
        readback->Unmap(0, nullptr);
        std::printf("OWNED_D3D12_QUERY_INTERVAL_VALUES execution=%u values=", execution);
        for (UINT i = 0; i < words; ++i)
            std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(actual[i]));
        std::puts("");
        std::array<UINT64, words> expected{};
        for (UINT i = 0; i < words; ++i)
            expected[i] = guard + i;
        expected[1] = execution ? 10 : 12;
        expected[3] = execution ? 6 : 20;
        expected[5] = 1;
        expected[7] = expected[9] = 0;
        for (UINT i = 0; i < words; ++i) {
            if (actual[i] != expected[i]) {
                std::fprintf(stderr, "OWNED_D3D12_QUERY_INTERVAL_MISMATCH execution=%u word=%u expected=%llu actual=%llu\n",
                             execution, i, static_cast<unsigned long long>(expected[i]),
                             static_cast<unsigned long long>(actual[i]));
                fail("interval exact counts/guards/reset/replay");
            }
        }
    }
    std::puts("OWNED_D3D12_QUERY_INTERVAL_PASS executions=3 changed_values=1 closed_list_replay=1 "
              "crossheap=1 empty_count=0 binary_visible=1 binary_empty=0 guard_words=6 "
              "heap_release_before_replay=1");
}

void runVisibilitySlotReuse(ID3D12Device* device, ID3D12CommandQueue* queue,
                            const Pipeline& pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    auto heap = createQueryHeap(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1);
    auto readback = createBuffer(device, sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    auto commands = createRecording(device);
    auto* list = commands.list.Get();
    // Start with Disabled, then reuse the same legal heap slot after End inside
    // one pass. The first interval must be accumulated before its later reset.
    recordVisibleDraw(list, pipeline, rtv);
    list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->DrawInstanced(3, 1, 0, 0);
    list->DrawInstanced(3, 1, 0, 0);
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    list->DrawInstanced(3, 1, 0, 0);
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    check(list->Close(), "slot reuse Close");
    // EndQuery leaves the final render pass active until Close. Resolve in a
    // second list so it cannot hide missing Close-time scratch allocation.
    auto resolve = createRecording(device);
    resolve.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, readback.Get(), 0);
    check(resolve.list->Close(), "slot reuse resolve Close");
    heap.Reset();
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "slot reuse fence");
    for (UINT execution = 0; execution < 2; ++execution) {
        std::fprintf(stderr, "OWNED_D3D12_QUERY_REUSE_PHASE execution=%u phase=submit_begin\n", execution);
        ID3D12CommandList* lists[]{list, resolve.list.Get()};
        queue->ExecuteCommandLists(2, lists);
        check(queue->Signal(fence.Get(), execution + 1), "slot reuse Signal");
        waitFence(fence.Get(), execution + 1);
        UINT64* mapped = nullptr;
        const D3D12_RANGE range{0, sizeof(UINT64)};
        check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "slot reuse Map");
        const auto actual = *mapped;
        readback->Unmap(0, nullptr);
        if (actual != kFullScreenSamples) fail("slot reuse reset order or released-heap replay");
        std::printf("OWNED_D3D12_QUERY_REUSE_VALUES execution=%u count=%llu\n", execution,
                    static_cast<unsigned long long>(actual));
        std::fprintf(stderr, "OWNED_D3D12_QUERY_REUSE_PHASE execution=%u phase=submit_end\n", execution);
    }
    std::puts("OWNED_D3D12_QUERY_REUSE_PASS executions=2 same_slot=1 heap_release_before_execute=1");
}

void runCopyTimestamps(ID3D12Device* device, ID3D12CommandQueue* queue) {
    constexpr unsigned rounds = 8;
    constexpr UINT64 rowWords = 9;
    constexpr UINT64 guard = 0xa5a5a5a5a5a5a5a5ull;
    constexpr UINT64 copyBytes = 64 * 1024;
    UINT64 frequency = 0, repeatedFrequency = 0;
    expectHRESULT(queue->GetTimestampFrequency(nullptr), E_INVALIDARG,
                  "copy_timestamp_frequency_null_output");
    check(queue->GetTimestampFrequency(&frequency), "COPY GetTimestampFrequency");
    check(queue->GetTimestampFrequency(&repeatedFrequency), "COPY GetTimestampFrequency repeat");
    if (!frequency || frequency != repeatedFrequency)
        fail("COPY frequency zero or changed");
    UINT64 gpuBefore = 0, cpuBefore = 0, gpuAfter = 0, cpuAfter = 0;
    expectHRESULT(queue->GetClockCalibration(nullptr, &cpuBefore), E_INVALIDARG,
                  "copy_clock_calibration_null_gpu");
    expectHRESULT(queue->GetClockCalibration(&gpuBefore, nullptr), E_INVALIDARG,
                  "copy_clock_calibration_null_cpu");
    const auto calibrate = [&](UINT64& gpu, UINT64& cpu) {
        LARGE_INTEGER before{}, after{};
        if (!QueryPerformanceCounter(&before))
            fail("COPY QPC before calibration");
        check(queue->GetClockCalibration(&gpu, &cpu), "COPY GetClockCalibration");
        if (!QueryPerformanceCounter(&after) || !gpu ||
            cpu < static_cast<UINT64>(before.QuadPart) ||
            cpu > static_cast<UINT64>(after.QuadPart))
            fail("COPY calibration CPU sample outside QPC bracket");
    };

    auto heap = createQueryHeap(device, D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP, 3);
    auto normalHeap = createQueryHeap(device, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3);
    auto readback = createBuffer(device, rounds * rowWords * sizeof(UINT64),
                                 D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload = createBuffer(device, rounds * copyBytes, D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    auto payload = createBuffer(device, rounds * copyBytes, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST);

    // The two heap kinds are mutually exclusive across COPY versus DIRECT/COMPUTE.
    for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                      D3D12_COMMAND_LIST_TYPE_COPY}) {
        auto* wrongHeap = type == D3D12_COMMAND_LIST_TYPE_COPY ? normalHeap.Get() : heap.Get();
        const char* resolveCase = type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct_copy_heap_resolve_rejected"
            : type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute_copy_heap_resolve_rejected"
            : "copy_normal_heap_resolve_rejected";
        // Existing DIRECT Begin and COPY normal-heap End cases below cover those rows.
        if (type != D3D12_COMMAND_LIST_TYPE_COPY) {
            Recording end = createRecording(device, type);
            end.list->EndQuery(wrongHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
            expectHRESULT(end.list->Close(), E_INVALIDARG,
                          type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct_copy_heap_end_rejected"
                          : "compute_copy_heap_end_rejected");
        }
        Recording resolve = createRecording(device, type);
        resolve.list->ResolveQueryData(wrongHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, readback.Get(), 8);
        expectHRESULT(resolve.list->Close(), E_INVALIDARG, resolveCase);
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            Recording begin = createRecording(device, type);
            begin.list->BeginQuery(type == D3D12_COMMAND_LIST_TYPE_COPY ? heap.Get() : normalHeap.Get(),
                                   D3D12_QUERY_TYPE_TIMESTAMP, 0);
            expectHRESULT(begin.list->Close(), E_INVALIDARG,
                          type == D3D12_COMMAND_LIST_TYPE_COPY ? "copy_timestamp_begin_rejected"
                          : "compute_timestamp_begin_rejected");
        }
    }
    {
        Recording invalid = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        invalid.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG, "copy_timestamp_end_index_rejected");
    }
    struct InvalidResolve { const char* name; UINT first; UINT count; UINT64 offset; };
    const InvalidResolve invalidResolves[] = {
        {"copy_resolve_index_rejected", 3, 1, 8},
        {"copy_resolve_count_rejected", 2, 2, 8},
        {"copy_resolve_count_overflow_rejected", 1, UINT_MAX, 8},
        {"copy_resolve_alignment_rejected", 0, 1, 4},
        {"copy_resolve_destination_bounds_rejected", 0, 2, rounds * rowWords * 8 - 8},
        {"copy_resolve_destination_overflow_rejected", 0, 1, ~UINT64(0) - 7},
    };
    for (const auto& test : invalidResolves) {
        Recording invalid = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        invalid.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       test.first, test.count, readback.Get(), test.offset);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG, test.name);
    }
    ComPtr<ID3D12Device> otherDevice;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice)),
          "COPY D3D12CreateDevice(other)");
    if (otherDevice.Get() == device)
        fail("COPY cross-device fixture received same device");
    auto foreignHeap = createQueryHeap(otherDevice.Get(), D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP, 1);
    auto foreignDestination = createBuffer(otherDevice.Get(), 8, D3D12_HEAP_TYPE_READBACK,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
    {
        Recording invalid = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        invalid.list->EndQuery(foreignHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG, "copy_cross_device_end_rejected");
    }
    {
        Recording invalid = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        invalid.list->ResolveQueryData(foreignHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, readback.Get(), 8);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG, "copy_cross_device_resolve_heap_rejected");
    }
    {
        Recording invalid = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        invalid.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, foreignDestination.Get(), 0);
        expectHRESULT(invalid.list->Close(), E_INVALIDARG, "copy_cross_device_resolve_destination_rejected");
    }
    D3D12_RANGE noRead{0, 0};
    UINT64* initialized = nullptr;
    check(readback->Map(0, &noRead, reinterpret_cast<void**>(&initialized)), "COPY guard Map");
    for (unsigned word = 0; word != rounds * rowWords; ++word)
        initialized[word] = guard;
    readback->Unmap(0, nullptr);
    unsigned char* source = nullptr;
    check(upload->Map(0, &noRead, reinterpret_cast<void**>(&source)), "COPY upload Map");
    for (unsigned round = 0; round != rounds; ++round)
        for (UINT64 byte = 0; byte != copyBytes; ++byte)
            source[round * copyBytes + byte] = static_cast<unsigned char>((byte * 17 + round * 29) ^ (byte >> 8));
    upload->Unmap(0, nullptr);

    std::array<Recording, rounds * 3> recordings;
    for (unsigned round = 0; round != rounds; ++round) {
        auto& producer = recordings[round * 3];
        auto& successor = recordings[round * 3 + 1];
        auto& reuse = recordings[round * 3 + 2];
        producer = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        successor = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        reuse = createRecording(device, D3D12_COMMAND_LIST_TYPE_COPY);
        producer.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (unsigned copy = 0; copy != 32; ++copy)
            producer.list->CopyBufferRegion(payload.Get(), round * copyBytes, upload.Get(), round * copyBytes, copyBytes);
        producer.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        successor.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        const UINT64 base = round * rowWords * sizeof(UINT64);
        successor.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, readback.Get(), base + 8);
        successor.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1, 1, readback.Get(), base + 40);
        reuse.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        reuse.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1, 1, readback.Get(), base + 56);
        check(producer.list->Close(), "COPY producer Close");
        check(successor.list->Close(), "COPY timestamp-only successor Close");
        check(reuse.list->Close(), "COPY reused timestamp Close");
    }
    calibrate(gpuBefore, cpuBefore);
    for (auto& recording : recordings) {
        ID3D12CommandList* lists[] = {recording.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        recording.list.Reset();
        recording.allocator.Reset();
    }
    heap.Reset();
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "COPY CreateFence");
    check(queue->Signal(fence.Get(), 1), "COPY final Signal");
    waitFence(fence.Get(), 1);
    calibrate(gpuAfter, cpuAfter);
    if (gpuAfter < gpuBefore || cpuAfter < cpuBefore)
        fail("COPY calibration samples not monotonic");
    UINT64* values = nullptr;
    D3D12_RANGE range{0, rounds * rowWords * sizeof(UINT64)};
    check(readback->Map(0, &range, reinterpret_cast<void**>(&values)), "COPY timestamp readback Map");
    UINT64 previous = gpuBefore;
    for (unsigned round = 0; round != rounds; ++round) {
        const auto* row = values + round * rowWords;
        const UINT64 pre = row[1], post = row[2], successor = row[3], duplicate = row[5], reused = row[7];
        if (row[0] != guard || row[4] != guard || row[6] != guard || row[8] != guard ||
            !pre || pre < previous || post < pre ||
            successor < post || duplicate != post || reused < successor || reused > gpuAfter ||
            reused == kMetalCounterErrorValue)
            fail("COPY timestamp slots, duplicate resolve, guards or queue ordering");
        previous = reused;
        std::printf("OWNED_D3D12_QUERY_COPY_VALUES round=%u pre=%llu post=%llu successor=%llu duplicate=%llu reused=%llu guards=1\n",
                    round, static_cast<unsigned long long>(pre), static_cast<unsigned long long>(post),
                    static_cast<unsigned long long>(successor), static_cast<unsigned long long>(duplicate),
                    static_cast<unsigned long long>(reused));
    }
    readback->Unmap(0, nullptr);
    unsigned char* copied = nullptr;
    D3D12_RANGE payloadRange{0, rounds * copyBytes};
    check(payload->Map(0, &payloadRange, reinterpret_cast<void**>(&copied)), "COPY payload readback Map");
    for (unsigned round = 0; round != rounds; ++round)
        for (UINT64 byte = 0; byte != copyBytes; ++byte)
            if (copied[round * copyBytes + byte] != static_cast<unsigned char>((byte * 17 + round * 29) ^ (byte >> 8)))
                fail("COPY timestamp workload payload mismatch");
    payload->Unmap(0, nullptr);
    std::printf("OWNED_D3D12_QUERY_COPY_CALIBRATION frequency=%llu gpu_before=%llu gpu_after=%llu cpu_before=%llu cpu_after=%llu qpc_bracket=1\n",
                static_cast<unsigned long long>(frequency), static_cast<unsigned long long>(gpuBefore),
                static_cast<unsigned long long>(gpuAfter), static_cast<unsigned long long>(cpuBefore),
                static_cast<unsigned long long>(cpuAfter));
    std::puts("OWNED_D3D12_QUERY_COPY_PASS rounds=8 separate_execute=1 same_slot_reuse=1 duplicate_resolve=1 guards=1 payload_bytes=524288 inter_submission_cpu_waits=0 final_fence=1 release_before_completion=1");
}

int run() {
    requireOwnedBackend();

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "D3D12CreateDevice");
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options)),
          "COPY D3D12_OPTIONS3");
    if (options.CopyQueueTimestampQueriesSupported != TRUE)
        fail("COPY timestamp capability not reported");
    std::puts("OWNED_D3D12_QUERY_COPY_CAP supported=1");
    D3D12_COMMAND_QUEUE_DESC copyQueueDesc{};
    copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ComPtr<ID3D12CommandQueue> copyQueue;
    check(device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&copyQueue)),
          "CreateCommandQueue(COPY)");
    runCopyTimestamps(device.Get(), copyQueue.Get());

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)),
          "CreateCommandQueue(DIRECT)");

    expectHRESULT(queue->GetTimestampFrequency(nullptr), E_INVALIDARG,
                  "timestamp_frequency_null_output");
    UINT64 frequency = 0;
    check(queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency");
    if (!frequency)
        fail("timestamp frequency is zero");
    std::printf("OWNED_D3D12_QUERY_FREQUENCY ticks_per_second=%llu\n",
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
    runVisibilityIntervals(device.Get(), queue.Get(), pipeline, rtv);
    runVisibilitySlotReuse(device.Get(), queue.Get(), pipeline, rtv);

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
    UINT64 executionGpuBefore = 0, executionCpuBefore = 0;
    check(queue->GetClockCalibration(&executionGpuBefore, &executionCpuBefore),
          "GetClockCalibration(before query execution)");
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
    UINT64 executionGpuAfter = 0, executionCpuAfter = 0;
    check(queue->GetClockCalibration(&executionGpuAfter, &executionCpuAfter),
          "GetClockCalibration(after query completion)");
    LARGE_INTEGER qpcFrequency{};
    if (!QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart <= 0 ||
        executionGpuAfter <= executionGpuBefore || executionCpuAfter <= executionCpuBefore)
        fail("execution clock calibration interval invalid");
    const double calibratedSeconds = double(executionGpuAfter - executionGpuBefore) / double(frequency);
    const double qpcSeconds = double(executionCpuAfter - executionCpuBefore) / double(qpcFrequency.QuadPart);
    const double clockErrorSeconds = calibratedSeconds > qpcSeconds
        ? calibratedSeconds - qpcSeconds : qpcSeconds - calibratedSeconds;
    if (clockErrorSeconds > 0.005 + qpcSeconds * 0.05)
        fail("GPU frequency/calibration and Wine QPC intervals disagree");

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
        timestamp1 == kMetalCounterErrorValue || timestamp1 <= timestamp0 ||
        timestamp0 < executionGpuBefore || timestamp1 > executionGpuAfter)
        fail("GPU timestamp samples invalid/nonmonotonic");
    std::printf("OWNED_D3D12_QUERY_CLOCK_INTERVAL gpu_seconds=%.9f qpc_seconds=%.9f "
                "error_seconds=%.9f samples_in_calibration_bracket=1\n",
                calibratedSeconds, qpcSeconds, clockErrorSeconds);

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

    // A timestamp heap is an opaque GPU resource. Resolving it in a different
    // command buffer must not require a CPU producer-completion wait. Across
    // queues, the application explicitly supplies the D3D12 fence dependency.
    for (unsigned crossQueue = 0; crossQueue != 2; ++crossQueue) {
        ComPtr<ID3D12CommandQueue> consumerQueue;
        if (crossQueue)
            check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&consumerQueue)),
                  "CreateCommandQueue(timestamp consumer)");
        else
            consumerQueue = queue;
        auto splitHeap = createQueryHeap(device.Get(), D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2);
        auto splitReadback = createBuffer(device.Get(), 2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
        Recording producer = createRecording(device.Get());
        producer.list->EndQuery(splitHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (unsigned draw = 0; draw != 32; ++draw)
            recordVisibleDraw(producer.list.Get(), pipeline, rtv);
        producer.list->EndQuery(splitHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        check(producer.list->Close(), "split timestamp producer Close");
        Recording consumer = createRecording(device.Get());
        consumer.list->ResolveQueryData(splitHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                        0, 2, splitReadback.Get(), 0);
        check(consumer.list->Close(), "split timestamp consumer Close");
        splitHeap.Reset();
        ComPtr<ID3D12Fence> orderingFence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&orderingFence)),
              "CreateFence(split timestamp)");
        UINT64 splitGpuBefore = 0, splitCpuBefore = 0;
        check(queue->GetClockCalibration(&splitGpuBefore, &splitCpuBefore),
              "GetClockCalibration(before split timestamp)");
        ID3D12CommandList* producerLists[] = {producer.list.Get()};
        queue->ExecuteCommandLists(1, producerLists);
        if (crossQueue) {
            check(queue->Signal(orderingFence.Get(), 1), "Signal(timestamp producer)");
            check(consumerQueue->Wait(orderingFence.Get(), 1), "Wait(timestamp consumer GPU)");
        }
        ID3D12CommandList* consumerLists[] = {consumer.list.Get()};
        consumerQueue->ExecuteCommandLists(1, consumerLists);
        producer.list.Reset(); producer.allocator.Reset();
        consumer.list.Reset(); consumer.allocator.Reset();
        check(consumerQueue->Signal(orderingFence.Get(), 2), "Signal(timestamp consumer)");
        waitFence(orderingFence.Get(), 2);
        UINT64 splitGpuAfter = 0, splitCpuAfter = 0;
        check(consumerQueue->GetClockCalibration(&splitGpuAfter, &splitCpuAfter),
              "GetClockCalibration(after split timestamp)");
        UINT64* splitValues = nullptr;
        check(splitReadback->Map(0, &timestampRange, reinterpret_cast<void**>(&splitValues)),
              "split timestamp readback Map");
        const UINT64 splitStart = splitValues[0], splitEnd = splitValues[1];
        splitReadback->Unmap(0, nullptr);
        if (!splitStart || splitEnd <= splitStart || splitStart < splitGpuBefore || splitEnd > splitGpuAfter)
            fail("split timestamp resolve lost producer data or ordering");
        std::printf("OWNED_D3D12_QUERY_SPLIT_PASS cross_queue=%u delta_ticks=%llu "
                    "gpu_fence_dependency=%u release_before_completion=1\n", crossQueue,
                    static_cast<unsigned long long>(splitEnd - splitStart), crossQueue);
    }

    // A timestamp at the start of its own submission must wait for the end
    // sample of preceding queue work, not merely commands in its own buffer.
    // Resolve before reusing slot zero, with no CPU waits between submissions.
    constexpr unsigned orderingRounds = 8;
    auto orderingHeap = createQueryHeap(device.Get(), D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2);
    auto orderingReadback = createBuffer(device.Get(), orderingRounds * 3 * sizeof(UINT64),
                                         D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<Recording, orderingRounds * 3> orderingLists;
    for (unsigned round = 0; round != orderingRounds; ++round) {
        auto& producer = orderingLists[round * 3];
        auto& successor = orderingLists[round * 3 + 1];
        auto& reuse = orderingLists[round * 3 + 2];
        producer = createRecording(device.Get());
        successor = createRecording(device.Get());
        reuse = createRecording(device.Get());
        for (unsigned draw = 0; draw != 2048; ++draw)
            recordVisibleDraw(producer.list.Get(), pipeline, rtv);
        producer.list->EndQuery(orderingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        successor.list->EndQuery(orderingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        successor.list->ResolveQueryData(orderingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                         0, 2, orderingReadback.Get(), round * 3 * sizeof(UINT64));
        reuse.list->EndQuery(orderingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        reuse.list->ResolveQueryData(orderingHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     0, 1, orderingReadback.Get(), (round * 3 + 2) * sizeof(UINT64));
        check(producer.list->Close(), "bottom-of-pipe producer Close");
        check(successor.list->Close(), "bottom-of-pipe successor Close");
        check(reuse.list->Close(), "bottom-of-pipe reuse Close");
    }
    for (auto& commands : orderingLists) {
        ID3D12CommandList* submitted[] = {commands.list.Get()};
        queue->ExecuteCommandLists(1, submitted);
    }
    check(queue->Signal(fence.Get(), 2), "Signal(bottom-of-pipe completion)");
    waitFence(fence.Get(), 2);
    D3D12_RANGE orderingRange{0, orderingRounds * 3 * sizeof(UINT64)};
    UINT64* ordered = nullptr;
    check(orderingReadback->Map(0, &orderingRange, reinterpret_cast<void**>(&ordered)),
          "bottom-of-pipe readback Map");
    for (unsigned round = 0; round != orderingRounds; ++round) {
        const UINT64 end = ordered[round * 3], next = ordered[round * 3 + 1], reused = ordered[round * 3 + 2];
        std::printf("OWNED_D3D12_QUERY_BOP_VALUES round=%u end=%llu successor=%llu reused=%llu\n", round,
                    static_cast<unsigned long long>(end), static_cast<unsigned long long>(next),
                    static_cast<unsigned long long>(reused));
        if (!end || end == kMetalCounterErrorValue || next == kMetalCounterErrorValue ||
            reused == kMetalCounterErrorValue || next < end || reused < next ||
            (round && end < ordered[round * 3 - 1]))
            fail("cross-submission bottom-of-pipe timestamp ordering or same-slot reuse");
    }
    orderingReadback->Unmap(0, nullptr);
    std::printf("OWNED_D3D12_QUERY_BOP_PASS rounds=%u separate_execute=1 same_slot_reuse=1 "
                "inter_submission_cpu_waits=0\n", orderingRounds);

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
        "frequency_valid=1 gpu_resolve=1 release_before_completion=1 no_cpu_timestamp=1 "
        "overlap=1 crossheap=1 visible_empty_preserve=1 cross_device_validation=1 "
        "closed_list_guard=1 copy_timestamp=1 copy_normal_timestamp_rejected=1 clear_state_preserves_queries=1 "
        "failed_close_allocator_reuse=1 active_resolve_rejected=1");
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return run();
}
