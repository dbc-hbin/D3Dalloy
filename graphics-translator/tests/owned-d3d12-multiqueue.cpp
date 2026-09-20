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
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kFrames = 32;
constexpr UINT kWordCount = 1024;
constexpr UINT kThreadsPerGroup = 64;
constexpr UINT64 kBufferBytes = static_cast<UINT64>(kWordCount) * sizeof(std::uint32_t);
constexpr DWORD kFenceTimeoutMs = 15000;
constexpr std::uint32_t kSalt = 0x6a09e667u;
constexpr std::uint32_t kTag = 0x31415926u;
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";

[[noreturn]] void fail(const char* step, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "OWNED_D3D12_MULTIQUEUE_FAIL step=%s hr=0x%08lx\n",
                 step, static_cast<unsigned long>(hr));
    ExitProcess(1);
}

void check(HRESULT hr, const char* step) {
    if (FAILED(hr))
        fail(step, hr);
}

void printModulePath(const wchar_t* moduleName, const char* label) {
    HMODULE module = GetModuleHandleW(moduleName);
    if (!module)
        fail("GetModuleHandleW", HRESULT_FROM_WIN32(GetLastError()));
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        fail("GetModuleFileNameW", HRESULT_FROM_WIN32(GetLastError()));
    std::printf("OWNED_D3D12_MULTIQUEUE_MODULE name=%s path=%ls\n", label, path.data());
}

void requireOwnedBackend() {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12)
        fail("GetModuleHandleW(d3d12)", HRESULT_FROM_WIN32(GetLastError()));
    FARPROC raw = GetProcAddress(d3d12, "OwnedGraphicsBackendId");
    if (!raw)
        fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
    using BackendId = const char* (WINAPI*)();
    const auto backendId = reinterpret_cast<BackendId>(raw);
    const char* id = backendId();
    if (!id || std::strcmp(id, kExpectedBackendId) != 0)
        fail("OwnedGraphicsBackendId mismatch");
    std::printf("OWNED_D3D12_MULTIQUEUE_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC bufferDescription(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState) {
    const D3D12_HEAP_PROPERTIES heap = heapProperties(heapType);
    const D3D12_RESOURCE_DESC description = bufferDescription(size, flags);
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, initialState,
                                          nullptr, IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(buffer)");
    return resource;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3DBlob> compileProducerShader() {
    static constexpr char shader[] = R"hlsl(
RWStructuredBuffer<uint> Output : register(u0);

cbuffer FrameConstants : register(b0)
{
    uint frameIndex;
    uint wordCount;
    uint salt;
    uint tag;
};

[numthreads(64, 1, 1)]
void csMain(uint3 threadId : SV_DispatchThreadID)
{
    uint index = threadId.x;
    if (index >= wordCount)
        return;

    Output[index] = 0xc001d00du
                  ^ (frameIndex * 0x9e3779b9u)
                  ^ (index * 0x85ebca6bu)
                  ^ salt
                  ^ (tag + index);
}
)hlsl";

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, "owned-d3d12-multiqueue.hlsl",
                                  nullptr, nullptr, "csMain", "cs_5_0",
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        fail("D3DCompile(csMain)", hr);
    }
    return code;
}

struct ProducerPipeline {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;

    explicit ProducerPipeline(ID3D12Device* device) {
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &uavRange;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].Constants.ShaderRegister = 0;
        parameters[1].Constants.RegisterSpace = 0;
        parameters[1].Constants.Num32BitValues = 4;

        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = static_cast<UINT>(parameters.size());
        rootDescription.pParameters = parameters.data();

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT serializedResult =
            D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                        &serialized, &errors);
        if (FAILED(serializedResult)) {
            if (errors)
                std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
            fail("D3D12SerializeRootSignature", serializedResult);
        }
        check(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                          serialized->GetBufferSize(), IID_PPV_ARGS(&root)),
              "CreateRootSignature");

        const ComPtr<ID3DBlob> cs = compileProducerShader();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root.Get();
        pipeline.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)),
              "CreateComputePipelineState");
    }
};

struct FrameWork {
    ComPtr<ID3D12CommandAllocator> computeAllocator;
    ComPtr<ID3D12GraphicsCommandList> computeList;
    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyList;
    ComPtr<ID3D12Resource> readback;
};

void requireListType(ID3D12CommandList* list, D3D12_COMMAND_LIST_TYPE expected,
                     const char* step) {
    const D3D12_COMMAND_LIST_TYPE actual = list->GetType();
    if (actual != expected) {
        std::fprintf(stderr,
                     "OWNED_D3D12_MULTIQUEUE_LIST_TYPE_MISMATCH step=%s expected=%u actual=%u\n",
                     step, static_cast<unsigned>(expected), static_cast<unsigned>(actual));
        fail(step);
    }
}

std::uint32_t expectedWord(UINT frame, UINT index) {
    return 0xc001d00du
         ^ (static_cast<std::uint32_t>(frame) * 0x9e3779b9u)
         ^ (static_cast<std::uint32_t>(index) * 0x85ebca6bu)
         ^ kSalt
         ^ (kTag + static_cast<std::uint32_t>(index));
}

void validateFrame(const FrameWork& frameWork, UINT frame) {
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(kBufferBytes)};
    std::uint32_t* words = nullptr;
    check(frameWork.readback->Map(0, &readRange, reinterpret_cast<void**>(&words)),
          "readback Map");
    for (UINT index = 0; index < kWordCount; ++index) {
        const std::uint32_t expected = expectedWord(frame, index);
        const std::uint32_t actual = words[index];
        if (actual != expected) {
            std::fprintf(stderr,
                         "OWNED_D3D12_MULTIQUEUE_MISMATCH frame=%u index=%u "
                         "expected=0x%08x actual=0x%08x\n",
                         frame, index, expected, actual);
            D3D12_RANGE written{0, 0};
            frameWork.readback->Unmap(0, &written);
            fail("readback mismatch");
        }
    }
    D3D12_RANGE written{0, 0};
    frameWork.readback->Unmap(0, &written);
    std::printf("OWNED_D3D12_MULTIQUEUE_FRAME frame=%u words=%u exact=ok\n",
                frame, kWordCount);
}

ComPtr<ID3D12CommandQueue> createQueue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                       const char* step) {
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = type;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    description.NodeMask = 0;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)), step);
    const D3D12_COMMAND_QUEUE_DESC actual = queue->GetDesc();
    if (actual.Type != type) {
        std::fprintf(stderr,
                     "OWNED_D3D12_MULTIQUEUE_QUEUE_TYPE_MISMATCH expected=%u actual=%u\n",
                     static_cast<unsigned>(type), static_cast<unsigned>(actual.Type));
        fail("ID3D12CommandQueue::GetDesc Type mismatch");
    }
    return queue;
}

void recordFrame(ID3D12Device* device, const ProducerPipeline& pipeline,
                 ID3D12DescriptorHeap* descriptorHeap, ID3D12Resource* shared,
                 UINT frame, FrameWork& work) {
    work.readback = createBuffer(device, kBufferBytes, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                         IID_PPV_ARGS(&work.computeAllocator)),
          "CreateCommandAllocator(COMPUTE)");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                    work.computeAllocator.Get(), nullptr,
                                    IID_PPV_ARGS(&work.computeList)),
          "CreateCommandList(COMPUTE)");
    requireListType(work.computeList.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE,
                    "GetType(COMPUTE)");

    if (frame != 0) {
        transition(work.computeList.Get(), shared, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    work.computeList->SetDescriptorHeaps(1, heaps);
    work.computeList->SetComputeRootSignature(pipeline.root.Get());
    work.computeList->SetPipelineState(pipeline.pso.Get());
    work.computeList->SetComputeRootDescriptorTable(
        0, descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    const std::array<UINT, 4> constants{frame, kWordCount, kSalt, kTag};
    work.computeList->SetComputeRoot32BitConstants(
        1, static_cast<UINT>(constants.size()), constants.data(), 0);
    work.computeList->Dispatch((kWordCount + kThreadsPerGroup - 1) / kThreadsPerGroup, 1, 1);
    transition(work.computeList.Get(), shared, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    check(work.computeList->Close(), "Close(COMPUTE)");

    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                         IID_PPV_ARGS(&work.copyAllocator)),
          "CreateCommandAllocator(COPY)");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                    work.copyAllocator.Get(), nullptr,
                                    IID_PPV_ARGS(&work.copyList)),
          "CreateCommandList(COPY)");
    requireListType(work.copyList.Get(), D3D12_COMMAND_LIST_TYPE_COPY,
                    "GetType(COPY)");
    work.copyList->CopyBufferRegion(work.readback.Get(), 0, shared, 0, kBufferBytes);
    check(work.copyList->Close(), "Close(COPY)");
}

void waitForFinalFence(ID3D12Fence* fence, UINT64 value) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));

    if (fence->GetCompletedValue() < value) {
        const HRESULT eventResult = fence->SetEventOnCompletion(value, event);
        if (FAILED(eventResult)) {
            CloseHandle(event);
            fail("SetEventOnCompletion(final copy fence)", eventResult);
        }
        const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
        if (wait != WAIT_OBJECT_0) {
            const HRESULT hr = HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
            CloseHandle(event);
            fail("final copy fence timeout", hr);
        }
    }
    CloseHandle(event);
}

int run() {
    printModulePath(L"d3d12.dll", "d3d12");
    requireOwnedBackend();

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "D3D12CreateDevice");

    const ComPtr<ID3D12CommandQueue> computeQueue =
        createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, "CreateCommandQueue(COMPUTE)");
    const ComPtr<ID3D12CommandQueue> copyQueue =
        createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_COPY, "CreateCommandQueue(COPY)");
    const D3D12_COMMAND_QUEUE_DESC computeDescription = computeQueue->GetDesc();
    const D3D12_COMMAND_QUEUE_DESC copyDescription = copyQueue->GetDesc();
    std::printf("OWNED_D3D12_MULTIQUEUE_QUEUE compute_type=%u copy_type=%u\n",
                static_cast<unsigned>(computeDescription.Type),
                static_cast<unsigned>(copyDescription.Type));

    ComPtr<ID3D12Fence> producerFence;
    ComPtr<ID3D12Fence> copyFence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerFence)),
          "CreateFence(producer)");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence)),
          "CreateFence(copy)");

    const ProducerPipeline pipeline(device.Get());
    const ComPtr<ID3D12Resource> shared =
        createBuffer(device.Get(), kBufferBytes, D3D12_HEAP_TYPE_DEFAULT,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_HEAP_DESC descriptorDescription{};
    descriptorDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorDescription.NumDescriptors = 1;
    descriptorDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    check(device->CreateDescriptorHeap(&descriptorDescription, IID_PPV_ARGS(&descriptorHeap)),
          "CreateDescriptorHeap(UAV)");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = kWordCount;
    uav.Buffer.StructureByteStride = sizeof(std::uint32_t);
    device->CreateUnorderedAccessView(shared.Get(), nullptr, &uav,
                                      descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    std::vector<FrameWork> frames(kFrames);
    for (UINT frame = 0; frame < kFrames; ++frame)
        recordFrame(device.Get(), pipeline, descriptorHeap.Get(), shared.Get(), frame, frames[frame]);

    std::puts("OWNED_D3D12_MULTIQUEUE_PHASE submit");
    for (UINT frame = 0; frame < kFrames; ++frame) {
        const UINT64 value = static_cast<UINT64>(frame) + 1;

        // Enqueue the consumer dependency before the producer Signal exists.
        check(copyQueue->Wait(producerFence.Get(), value), "COPY queue Wait(producerFence)");

        // Reuse of the one shared UAV is legal only after the previous COPY queue
        // has consumed it into that frame's private readback resource.
        if (frame != 0)
            check(computeQueue->Wait(copyFence.Get(), frame), "COMPUTE queue Wait(copyFence)");

        ID3D12CommandList* computeLists[] = {frames[frame].computeList.Get()};
        computeQueue->ExecuteCommandLists(1, computeLists);
        check(computeQueue->Signal(producerFence.Get(), value),
              "COMPUTE queue Signal(producerFence)");

        ID3D12CommandList* copyLists[] = {frames[frame].copyList.Get()};
        copyQueue->ExecuteCommandLists(1, copyLists);
        check(copyQueue->Signal(copyFence.Get(), value), "COPY queue Signal(copyFence)");

        std::printf("OWNED_D3D12_MULTIQUEUE_SUBMIT frame=%u value=%llu "
                    "consumer_wait_before_producer_signal=yes producer_wait_previous_copy=%s\n",
                    frame, static_cast<unsigned long long>(value), frame ? "yes" : "n/a");
    }

    std::printf("OWNED_D3D12_MULTIQUEUE_CPU_WAIT first_and_only_value=%u timeout_ms=%lu\n",
                kFrames, static_cast<unsigned long>(kFenceTimeoutMs));
    waitForFinalFence(copyFence.Get(), kFrames);
    if (copyFence->GetCompletedValue() < kFrames)
        fail("copy fence incomplete after final wait");
    if (producerFence->GetCompletedValue() < kFrames)
        fail("producer fence incomplete after final copy fence");

    std::puts("OWNED_D3D12_MULTIQUEUE_PHASE validate");
    for (UINT frame = 0; frame < kFrames; ++frame)
        validateFrame(frames[frame], frame);

    std::printf("OWNED_D3D12_MULTIQUEUE_PASS frames=%u words_per_frame=%u "
                "cross_queue_wait_signal=ok exact_readback=ok\n",
                kFrames, kWordCount);
    return 0;
}

} // namespace

int main() {
    return run();
}
