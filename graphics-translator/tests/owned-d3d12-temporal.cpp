#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

// Exact copied public COM ABI from the owned candidate
// src/d3d12/d3d12_interfaces.hpp. No WMT/Metal/private implementation types
// cross this fixture boundary.
struct MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC {
    UINT StructSize;
    UINT InputWidth, InputHeight;
    UINT OutputWidth, OutputHeight;
    UINT FeatureFlags;
    BOOL OutputSubrectsEnabled;
    UINT CreationNodeMask, VisibilityNodeMask;
    BOOL DynamicOutputEnabled;
};

struct MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC {
    UINT StructSize;
    ID3D12Resource* Color;
    ID3D12Resource* Output;
    ID3D12Resource* Depth;
    ID3D12Resource* MotionVector;
    ID3D12Resource* ExposureTexture;
    ID3D12Resource* BiasCurrentColorMask;
    ID3D12Resource* TransparencyMask;
    UINT InputContentWidth, InputContentHeight;
    UINT ColorBaseX, ColorBaseY, DepthBaseX, DepthBaseY;
    UINT MotionBaseX, MotionBaseY, BiasBaseX, BiasBaseY;
    UINT OutputBaseX, OutputBaseY;
    FLOAT JitterOffsetX, JitterOffsetY;
    FLOAT MotionVectorScaleX, MotionVectorScaleY;
    FLOAT PreExposure;
    BOOL ResetHistory;
    UINT OutputWidth, OutputHeight;
    UINT MotionContentWidth, MotionContentHeight;
    UINT CompositionBaseX, CompositionBaseY;
    UINT ColorTransfer;
    BOOL CombineCompositionMask, EnableSharpening;
    FLOAT Sharpness;
    BOOL CapOutputToTemporalMaxScale;
    BOOL DrawDebugView;
    ID3D12Resource* DiffuseAlbedo;
    ID3D12Resource* SpecularAlbedo;
    ID3D12Resource* Normals;
    ID3D12Resource* Roughness;
    ID3D12Resource* SpecularHitDistance;
    FLOAT WorldToViewMatrix[16], ViewToClipMatrix[16];
    UINT RoughnessPacked, DepthLinear;
    UINT DiffuseAlbedoBaseX, DiffuseAlbedoBaseY, SpecularAlbedoBaseX, SpecularAlbedoBaseY;
    UINT NormalsBaseX, NormalsBaseY, RoughnessBaseX, RoughnessBaseY;
    UINT SpecularHitDistanceBaseX, SpecularHitDistanceBaseY;
};

struct IMTLD3D12TemporalUpscaleState : public IUnknown {};

struct IMTLD3D12DeviceExt : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SupportsTemporalUpscale(BOOL* supported) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTemporalUpscaleState(
        const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC* desc,
        IMTLD3D12TemporalUpscaleState** state) = 0;
    virtual HRESULT STDMETHODCALLTYPE SupportsTemporalFeature(UINT feature_flags, BOOL* supported) = 0;
};

struct IMTLD3D12GraphicsCommandListExt : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE TemporalUpscale(
        IMTLD3D12TemporalUpscaleState* state,
        const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC* desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE ValidateRecording() = 0;
};

constexpr GUID kIID_IMTLD3D12TemporalUpscaleState = {
    0xc8a9134e, 0xf27c, 0x4e75, {0x90, 0x9f, 0xf1, 0x44, 0x39, 0xb9, 0x18, 0xe1}};
constexpr GUID kIID_IMTLD3D12DeviceExt = {
    0xed85c4e9, 0xe8fd, 0x491b, {0x95, 0x95, 0xd9, 0xa3, 0x69, 0xec, 0x12, 0xb3}};
constexpr GUID kIID_IMTLD3D12GraphicsCommandListExt = {
    0xd7bc6ed0, 0x6a8a, 0x4a0e, {0x84, 0x08, 0x5a, 0x26, 0xc0, 0xb0, 0x6d, 0x0f}};

static_assert(sizeof(MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC) == 40);
#ifdef _WIN64
static_assert(offsetof(MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC, Color) == 8);
static_assert(offsetof(MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC, DiffuseAlbedo) == 184);
static_assert(sizeof(MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC) == 400);
#else
static_assert(offsetof(MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC, Color) == 4);
static_assert(offsetof(MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC, DiffuseAlbedo) == 152);
static_assert(sizeof(MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC) == 348);
#endif

namespace {

constexpr UINT kInputWidth = 64;
constexpr UINT kInputHeight = 64;
constexpr UINT kOutputWidth = 128;
constexpr UINT kOutputHeight = 128;
constexpr UINT kOutputBacking = 136;
constexpr UINT kFrames = 8;
constexpr DWORD kFenceTimeoutMs = 15000;
constexpr UINT kDeferredResetCount = 96;
constexpr UINT kDeferredRecordHeads = kDeferredResetCount + 1;
constexpr UINT kOppositeOrderIterations = 8;
constexpr UINT kSubrectBaseX = 3;
constexpr UINT kSubrectBaseY = 5;
constexpr UINT kFlags = 74;
constexpr UINT kFlagMvLowRes = 1u << 1;
constexpr UINT kFlagDepthInverted = 1u << 3;
constexpr UINT kFlagAutoExposure = 1u << 6;
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";

static_assert(kFlags == (kFlagMvLowRes | kFlagDepthInverted | kFlagAutoExposure));
static_assert(DXGI_FORMAT_R32G8X24_TYPELESS == 19);
static_assert(DXGI_FORMAT_D32_FLOAT_S8X24_UINT == 20);
static_assert(DXGI_FORMAT_R8G8B8A8_TYPELESS == 27);
static_assert(DXGI_FORMAT_R8G8B8A8_UNORM == 28);
static_assert(DXGI_FORMAT_R16G16_TYPELESS == 33);
static_assert(DXGI_FORMAT_R16G16_FLOAT == 34);
static_assert(kDeferredRecordHeads > 64);
static_assert(kSubrectBaseX + kOutputWidth <= kOutputBacking);
static_assert(kSubrectBaseY + kOutputHeight <= kOutputBacking);

[[noreturn]] void fail(const char* step, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "OWNED_D3D12_TEMPORAL_FAIL step=%s hr=0x%08lx\n",
                 step, static_cast<unsigned long>(hr));
    ExitProcess(1);
}

void check(HRESULT hr, const char* step) {
    if (FAILED(hr))
        fail(step, hr);
}

void expectHRESULT(HRESULT actual, HRESULT expected, const char* caseName) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "OWNED_D3D12_TEMPORAL_HRESULT_MISMATCH case=%s "
                     "expected=0x%08lx actual=0x%08lx\n",
                     caseName, static_cast<unsigned long>(expected),
                     static_cast<unsigned long>(actual));
        fail(caseName, actual);
    }
    std::printf("OWNED_D3D12_TEMPORAL_CASE_PASS case=%s exact_hr=0x%08lx\n",
                caseName, static_cast<unsigned long>(actual));
}

void printModulePath(const wchar_t* moduleName, const char* label) {
    HMODULE module = GetModuleHandleW(moduleName);
    if (!module)
        fail("GetModuleHandleW", HRESULT_FROM_WIN32(GetLastError()));
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        fail("GetModuleFileNameW", HRESULT_FROM_WIN32(GetLastError()));
    std::printf("OWNED_D3D12_TEMPORAL_MODULE name=%s path=%ls\n", label, path.data());
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
    std::printf("OWNED_D3D12_TEMPORAL_BACKEND id=%s\n", id);
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC bufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

D3D12_RESOURCE_DESC textureDescription(UINT width, UINT height, DXGI_FORMAT format,
                                       D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return description;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, UINT64 size,
                                    D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state) {
    const auto heap = heapProperties(heapType);
    const auto description = bufferDescription(size);
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                          state, nullptr, IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(buffer)");
    return resource;
}

ComPtr<ID3D12Resource> createTexture(ID3D12Device* device, UINT width, UINT height,
                                     DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state,
                                     const D3D12_CLEAR_VALUE* clear = nullptr) {
    const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const auto description = textureDescription(width, height, format, flags);
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                          state, clear, IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(texture)");
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

struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<IMTLD3D12GraphicsCommandListExt> extension;
};

class Gpu final {
public:
    Gpu() {
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
              "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)),
              "CreateCommandQueue(DIRECT)");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "CreateFence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    }

    ~Gpu() {
        if (event)
            CloseHandle(event);
    }

    Recording makeRecording() {
        Recording recording;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&recording.allocator)),
              "CreateCommandAllocator(DIRECT)");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        recording.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&recording.list)),
              "CreateCommandList(DIRECT)");
        if (recording.list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            fail("command list GetType(DIRECT)");
        check(recording.list->QueryInterface(
                  kIID_IMTLD3D12GraphicsCommandListExt,
                  reinterpret_cast<void**>(recording.extension.GetAddressOf())),
              "QueryInterface(IMTLD3D12GraphicsCommandListExt)");
        check(recording.extension->ValidateRecording(), "ValidateRecording(open)");
        return recording;
    }

    void executeAndWait(ID3D12CommandList* list) {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fenceValue;
        check(queue->Signal(fence.Get(), value), "CommandQueue::Signal");
        if (fence->GetCompletedValue() < value) {
            check(fence->SetEventOnCompletion(value, event), "Fence::SetEventOnCompletion");
            const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
            if (wait != WAIT_OBJECT_0)
                fail("GPU fence timeout",
                     HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
        }
        // Device removal also wakes fence events and reports UINT64_MAX,
        // which is not successful completion of the recorded GPU work.
        check(device->GetDeviceRemovedReason(), "GPU completion device removed");
    }

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

private:
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
};

ComPtr<ID3D12CommandQueue> createDirectQueue(ID3D12Device* device,
                                             const char* step) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), step);
    return queue;
}

void waitForFenceValue(ID3D12Fence* fence, UINT64 value, const char* step) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        fail("CreateEventW(fence wait)", HRESULT_FROM_WIN32(GetLastError()));
    if (fence->GetCompletedValue() < value) {
        const HRESULT hr = fence->SetEventOnCompletion(value, event);
        if (FAILED(hr)) {
            CloseHandle(event);
            fail(step, hr);
        }
        const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
        if (wait != WAIT_OBJECT_0) {
            const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
            CloseHandle(event);
            fail(step, HRESULT_FROM_WIN32(error));
        }
    }
    CloseHandle(event);
}

void runThreadBounded(LPTHREAD_START_ROUTINE routine, void* context,
                      const char* timeoutStep) {
    HANDLE thread = CreateThread(nullptr, 0, routine, context, 0, nullptr);
    if (!thread)
        fail("CreateThread", HRESULT_FROM_WIN32(GetLastError()));
    const DWORD wait = WaitForSingleObject(thread, kFenceTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        CloseHandle(thread);
        fail(timeoutStep, HRESULT_FROM_WIN32(error));
    }
    DWORD exitCode = 0;
    if (!GetExitCodeThread(thread, &exitCode)) {
        const DWORD error = GetLastError();
        CloseHandle(thread);
        fail("GetExitCodeThread", HRESULT_FROM_WIN32(error));
    }
    CloseHandle(thread);
    if (exitCode != 0)
        fail("bounded caller thread nonzero exit");
}

void uploadTexture(Gpu& gpu, ID3D12Resource* texture, const void* pixels,
                   std::size_t sourceRowBytes, D3D12_RESOURCE_STATES finalState) {
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    gpu.device->GetCopyableFootprints(&description, 0, 1, 0, &footprint,
                                      &rows, &rowBytes, &totalBytes);
    if (sourceRowBytes < rowBytes)
        fail("uploadTexture source row too small");

    ComPtr<ID3D12Resource> upload =
        createBuffer(gpu.device.Get(), totalBytes, D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
    std::uint8_t* mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "upload Map");
    for (UINT y = 0; y < rows; ++y) {
        std::memcpy(mapped + footprint.Offset +
                        static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                    static_cast<const std::uint8_t*>(pixels) +
                        static_cast<std::size_t>(y) * sourceRowBytes,
                    static_cast<std::size_t>(rowBytes));
    }
    upload->Unmap(0, nullptr);

    Recording recording = gpu.makeRecording();
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    recording.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(recording.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    check(recording.list->Close(), "upload Close");
    gpu.executeAndWait(recording.list.Get());
}

struct TextureReadback {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
};

TextureReadback createTextureReadback(Gpu& gpu, ID3D12Resource* texture) {
    TextureReadback result;
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    gpu.device->GetCopyableFootprints(&description, 0, 1, 0, &result.footprint,
                                      &result.rows, &result.rowBytes, &result.totalBytes);
    result.buffer = createBuffer(gpu.device.Get(), result.totalBytes,
                                 D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    return result;
}

void copyTextureToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
                           const TextureReadback& readback) {
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback.footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

ComPtr<ID3D12Resource> makeDepth(
    Gpu& gpu,
    DXGI_FORMAT parentFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    UINT backingWidth = kInputWidth,
    UINT backingHeight = kInputHeight) {
    D3D12_CLEAR_VALUE clear{};
    clear.Format = dsvFormat;
    clear.DepthStencil.Depth = 0.75f;
    clear.DepthStencil.Stencil = 0;
    ComPtr<ID3D12Resource> depth =
        createTexture(gpu.device.Get(), backingWidth, backingHeight,
                      parentFormat,
                      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.NumDescriptors = 1;
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(gpu.device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)),
          "CreateDescriptorHeap(DSV)");
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = dsvFormat;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateDepthStencilView(depth.Get(), &view, dsv);

    Recording recording = gpu.makeRecording();
    recording.list->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        0.75f, 0, 0, nullptr);
    transition(recording.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    check(recording.list->Close(), "depth Close");
    gpu.executeAndWait(recording.list.Get());
    return depth;
}

ComPtr<ID3D12Resource> makeMotion(
    Gpu& gpu, DXGI_FORMAT parentFormat = DXGI_FORMAT_R16G16_FLOAT,
    UINT backingWidth = kInputWidth,
    UINT backingHeight = kInputHeight) {
    ComPtr<ID3D12Resource> motion =
        createTexture(gpu.device.Get(), backingWidth, backingHeight,
                      parentFormat, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<std::uint16_t> zeros(
        static_cast<std::size_t>(backingWidth) * backingHeight * 2, 0);
    uploadTexture(gpu, motion.Get(), zeros.data(), backingWidth * 4,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return motion;
}

ComPtr<ID3D12Resource> makeColor(
    Gpu& gpu, std::uint8_t tag,
    DXGI_FORMAT parentFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
    UINT backingWidth = kInputWidth,
    UINT backingHeight = kInputHeight) {
    ComPtr<ID3D12Resource> color =
        createTexture(gpu.device.Get(), backingWidth, backingHeight,
                      parentFormat, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(backingWidth) * backingHeight * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = tag;
        pixels[i + 1] = tag;
        pixels[i + 2] = tag;
        pixels[i + 3] = 255;
    }
    uploadTexture(gpu, color.Get(), pixels.data(), backingWidth * 4,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return color;
}

struct OutputResource {
    ComPtr<ID3D12Resource> resource;
    TextureReadback readback;
    std::array<std::uint8_t, 4> sentinel{};
};

OutputResource makeOutput(
    Gpu& gpu, UINT tag,
    DXGI_FORMAT parentFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
    UINT backingWidth = kOutputBacking, UINT backingHeight = kOutputBacking) {
    OutputResource result;
    result.resource =
        createTexture(gpu.device.Get(), backingWidth, backingHeight,
                      parentFormat,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_DEST);
    result.sentinel = {
        static_cast<std::uint8_t>(17u + (tag % 101u)),
        static_cast<std::uint8_t>(37u + (tag % 83u)),
        static_cast<std::uint8_t>(59u + (tag % 67u)),
        static_cast<std::uint8_t>(83u + (tag % 47u)),
    };
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(backingWidth) * backingHeight * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        std::copy(result.sentinel.begin(), result.sentinel.end(), pixels.begin() + i);
    uploadTexture(gpu, result.resource.Get(), pixels.data(), backingWidth * 4,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    result.readback = createTextureReadback(gpu, result.resource.Get());
    return result;
}

struct OutputStats {
    double mean = 0.0;
    UINT outputTag = 0;
    std::size_t changedInside = 0;
};

OutputStats validateOutput(const OutputResource& output, std::uint8_t inputTag,
                           const char* caseName, UINT frame,
                           UINT outputBaseX = 0, UINT outputBaseY = 0,
                           UINT outputWidth = kOutputWidth, UINT outputHeight = kOutputHeight,
                           const D3D12_RESOURCE_DESC* releasedBacking = nullptr) {
    if (!output.resource && !releasedBacking) fail("released output validation requires recorded dimensions");
    const auto backing = releasedBacking ? *releasedBacking : output.resource->GetDesc();
    if (!outputWidth || !outputHeight || outputBaseX + outputWidth > backing.Width ||
        outputBaseY + outputHeight > backing.Height)
        fail("output validation rectangle exceeds backing");
    D3D12_RANGE range{0, static_cast<SIZE_T>(output.readback.totalBytes)};
    std::uint8_t* data = nullptr;
    check(output.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&data)),
          "output readback Map");

    double sum = 0.0;
    std::size_t samples = 0;
    std::size_t changedInside = 0;
    for (UINT y = 0; y < backing.Height; ++y) {
        const std::uint8_t* row =
            data + output.readback.footprint.Offset +
            static_cast<std::size_t>(y) * output.readback.footprint.Footprint.RowPitch;
        for (UINT x = 0; x < backing.Width; ++x) {
            const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
            const bool inside =
                x >= outputBaseX && x < outputBaseX + outputWidth &&
                y >= outputBaseY && y < outputBaseY + outputHeight;
            if (!inside) {
                if (!std::equal(output.sentinel.begin(), output.sentinel.end(), pixel)) {
                    output.readback.buffer->Unmap(0, nullptr);
                    fail("outside logical output sentinel changed");
                }
                continue;
            }

            if (!std::equal(output.sentinel.begin(), output.sentinel.end(), pixel))
                ++changedInside;
            for (UINT channel = 0; channel < 3; ++channel) {
                const double value = static_cast<double>(pixel[channel]) / 255.0;
                if (!std::isfinite(value)) {
                    output.readback.buffer->Unmap(0, nullptr);
                    fail("nonfinite output");
                }
                sum += value;
                ++samples;
            }
        }
    }
    output.readback.buffer->Unmap(0, nullptr);

    OutputStats stats;
    stats.mean = sum / static_cast<double>(samples);
    stats.outputTag = static_cast<UINT>(std::lround(stats.mean * 255.0));
    stats.changedInside = changedInside;
    const double expectedMean = static_cast<double>(inputTag) / 255.0;
    const double delta = std::fabs(stats.mean - expectedMean);
    const std::size_t logicalPixels =
        static_cast<std::size_t>(outputWidth) * outputHeight;
    if (changedInside < logicalPixels * 3 / 4)
        fail("logical output remained sentinel/stale");
    if (!(stats.mean > 0.05 && stats.mean < 0.95))
        fail("logical output black/implausible");
    if (!(delta <= 0.20)) {
        std::fprintf(stderr,
                     "OWNED_D3D12_TEMPORAL_OUTPUT case=%s frame=%u input_tag=%u "
                     "output_mean=%.9g delta=%.9g\n",
                     caseName, frame, static_cast<unsigned>(inputTag),
                     stats.mean, delta);
        fail("constant color loose tolerance");
    }
    return stats;
}

MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC makeCreateDesc(
    BOOL outputSubrectsEnabled = FALSE) {
    MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC desc{};
    desc.StructSize = sizeof(desc);
    desc.InputWidth = kInputWidth;
    desc.InputHeight = kInputHeight;
    desc.OutputWidth = kOutputWidth;
    desc.OutputHeight = kOutputHeight;
    desc.FeatureFlags = kFlags;
    desc.OutputSubrectsEnabled = outputSubrectsEnabled;
    desc.CreationNodeMask = 1;
    desc.VisibilityNodeMask = 1;
    return desc;
}

ComPtr<IMTLD3D12TemporalUpscaleState> createState(
    IMTLD3D12DeviceExt* extension, BOOL outputSubrectsEnabled = FALSE) {
    const auto desc = makeCreateDesc(outputSubrectsEnabled);
    ComPtr<IMTLD3D12TemporalUpscaleState> state;
    check(extension->CreateTemporalUpscaleState(&desc, &state),
          "CreateTemporalUpscaleState(flags74)");
    if (!state)
        fail("CreateTemporalUpscaleState returned null");
    ComPtr<IMTLD3D12TemporalUpscaleState> roundTrip;
    check(state->QueryInterface(
              kIID_IMTLD3D12TemporalUpscaleState,
              reinterpret_cast<void**>(roundTrip.GetAddressOf())),
          "QueryInterface(IMTLD3D12TemporalUpscaleState)");
    return state;
}

MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC makeFrameDesc(
    ID3D12Resource* color, ID3D12Resource* output,
    ID3D12Resource* depth, ID3D12Resource* motion,
    BOOL resetHistory = FALSE) {
    MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC desc{};
    desc.StructSize = sizeof(desc);
    desc.Color = color;
    desc.Output = output;
    desc.Depth = depth;
    desc.MotionVector = motion;
    desc.ExposureTexture = nullptr;
    desc.BiasCurrentColorMask = nullptr;
    desc.TransparencyMask = nullptr;
    desc.InputContentWidth = kInputWidth;
    desc.InputContentHeight = kInputHeight;
    desc.ColorBaseX = 0;
    desc.ColorBaseY = 0;
    desc.DepthBaseX = 0;
    desc.DepthBaseY = 0;
    desc.MotionBaseX = 0;
    desc.MotionBaseY = 0;
    desc.BiasBaseX = 0;
    desc.BiasBaseY = 0;
    desc.OutputBaseX = 0;
    desc.OutputBaseY = 0;
    desc.JitterOffsetX = 0.0f;
    desc.JitterOffsetY = 0.0f;
    desc.MotionVectorScaleX = 1.0f;
    desc.MotionVectorScaleY = 1.0f;
    desc.PreExposure = 1.0f;
    desc.ResetHistory = resetHistory;
    return desc;
}

Recording recordTemporal(Gpu& gpu,
                         IMTLD3D12TemporalUpscaleState* state,
                         MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC desc,
                         OutputResource& output) {
    Recording recording = gpu.makeRecording();
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    check(recording.extension->TemporalUpscale(state, &desc), "TemporalUpscale(record)");
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "Temporal Close");
    return recording;
}

Recording recordTemporalPair(
    Gpu& gpu,
    IMTLD3D12TemporalUpscaleState* firstState,
    MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC firstDesc,
    OutputResource& firstOutput,
    IMTLD3D12TemporalUpscaleState* secondState,
    MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC secondDesc,
    OutputResource& secondOutput) {
    Recording recording = gpu.makeRecording();

    transition(recording.list.Get(), firstOutput.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    check(recording.extension->TemporalUpscale(firstState, &firstDesc),
          "TemporalUpscale(pair first)");
    transition(recording.list.Get(), firstOutput.resource.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(
        recording.list.Get(), firstOutput.resource.Get(), firstOutput.readback);

    transition(recording.list.Get(), secondOutput.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    check(recording.extension->TemporalUpscale(secondState, &secondDesc),
          "TemporalUpscale(pair second)");
    transition(recording.list.Get(), secondOutput.resource.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(
        recording.list.Get(), secondOutput.resource.Get(), secondOutput.readback);
    check(recording.list->Close(), "Temporal pair Close");
    return recording;
}

void runCreateFlagNegative(IMTLD3D12DeviceExt* extension) {
    auto desc = makeCreateDesc();
    desc.FeatureFlags |= 0x80000000u;
    ComPtr<IMTLD3D12TemporalUpscaleState> state;
    const HRESULT hr = extension->CreateTemporalUpscaleState(&desc, &state);
    if (state)
        state.Reset();
    expectHRESULT(hr, E_NOTIMPL, "unrecognized_feature_flags");
    desc = makeCreateDesc();
    desc.FeatureFlags |= 128;
    expectHRESULT(extension->CreateTemporalUpscaleState(&desc, &state),
                  S_OK, "alpha_upscaling_create");
    if (!state) fail("alpha upscaling returned null state");
}

void runR32ExposureReplay(Gpu& gpu, IMTLD3D12DeviceExt* extension,
                          ID3D12Resource* depth, ID3D12Resource* motion) {
    auto create = makeCreateDesc();
    create.FeatureFlags = kFlagMvLowRes | kFlagDepthInverted | 1; // HDR, manual exposure.
    ComPtr<IMTLD3D12TemporalUpscaleState> state32, state16;
    check(extension->CreateTemporalUpscaleState(&create, &state32), "manual exposure state32");
    check(extension->CreateTemporalUpscaleState(&create, &state16), "manual exposure state16");
    auto color = createTexture(gpu.device.Get(), kInputWidth, kInputHeight,
                               DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<std::uint16_t> pixels(kInputWidth * kInputHeight * 4);
    for (UINT y = 0; y < kInputHeight; ++y)
        for (UINT x = 0; x < kInputWidth; ++x) {
            const UINT i = (y * kInputWidth + x) * 4;
            // Structured HDR contrasts exercise exposure-dependent reconstruction.
            pixels[i] = ((x ^ y) & 1) ? 0x4400 : 0x3400; // 4 / 0.25
            pixels[i + 1] = (x % 7) ? 0x3c00 : 0x4800; // 1 / 8
            pixels[i + 2] = (y % 5) ? 0x3800 : 0x4000; // 0.5 / 2
            pixels[i + 3] = 0x3c00;
        }
    uploadTexture(gpu, color.Get(), pixels.data(), kInputWidth * 8,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto exposure32 = createTexture(gpu.device.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto exposure16 = createTexture(gpu.device.Get(), 1, 1, DXGI_FORMAT_R16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "exposure RTV heap");
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(exposure32.Get(), nullptr, rtv);
    OutputResource output32, output16;
    for (auto* output : {&output32, &output16}) {
        output->resource = createTexture(gpu.device.Get(), kOutputWidth, kOutputHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        output->readback = createTextureReadback(gpu, output->resource.Get());
    }
    auto desc32 = makeFrameDesc(color.Get(), output32.resource.Get(), depth, motion, TRUE);
    desc32.ExposureTexture = exposure32.Get();
    auto desc16 = makeFrameDesc(color.Get(), output16.resource.Get(), depth, motion, TRUE);
    desc16.ExposureTexture = exposure16.Get();
    // Record before the R32 texel has ever been GPU-produced. This must not
    // snapshot exposure on the CPU or convert only at recording time.
    auto recording32 = recordTemporal(gpu, state32.Get(), desc32, output32);
    auto recording16 = recordTemporal(gpu, state16.Get(), desc16, output16);
    state32.Reset();
    state16.Reset();
    constexpr float values[] = {0.3333f, 1.234567f, 3.14159f};
    // Independent IEEE round-to-nearest-even oracle, uploaded as half bits.
    constexpr std::uint16_t rounded[] = {0x3555, 0x3cf0, 0x4248};
    std::vector<std::uint16_t> previous;
    UINT changedWithExposure = 0;
    for (UINT replay = 0; replay < 3; ++replay) {
        if (replay) {
            auto barrier = gpu.makeRecording();
            transition(barrier.list.Get(), exposure16.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            check(barrier.list->Close(), "exposure16 reupload transition");
            gpu.executeAndWait(barrier.list.Get());
        }
        uploadTexture(gpu, exposure16.Get(), &rounded[replay], sizeof(rounded[replay]),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto producer = gpu.makeRecording();
        if (replay)
            transition(producer.list.Get(), exposure32.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[] = {values[replay], 0, 0, 1};
        producer.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        transition(producer.list.Get(), exposure32.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        check(producer.list->Close(), "GPU exposure producer Close");
        ComPtr<ID3D12Fence> gate;
        if (replay == 2) {
            check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)),
                  "exposure release gate");
            check(gpu.queue->Wait(gate.Get(), 1), "exposure release gate wait");
        }
        ID3D12CommandList* lists[] = {producer.list.Get(), recording32.list.Get(), recording16.list.Get()};
        gpu.queue->ExecuteCommandLists(3, lists);
        ComPtr<ID3D12Fence> completion;
        check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion)),
              "exposure completion fence");
        check(gpu.queue->Signal(completion.Get(), 1), "exposure completion signal");
        if (replay == 2) {
            exposure32.Reset();
            exposure16.Reset();
            color.Reset();
            heap.Reset();
            recording32.extension.Reset();
            recording16.extension.Reset();
            check(recording32.list->Reset(recording32.allocator.Get(), nullptr), "exposure release recording32");
            check(recording16.list->Reset(recording16.allocator.Get(), nullptr), "exposure release recording16");
            check(recording32.list->Close(), "exposure release32 Close");
            check(recording16.list->Close(), "exposure release16 Close");
            producer.extension.Reset();
            check(producer.list->Reset(producer.allocator.Get(), nullptr), "exposure release producer");
            check(producer.list->Close(), "exposure release producer Close");
            check(gate->Signal(1), "exposure release gate signal");
        }
        waitForFenceValue(completion.Get(), 1, "exposure GPU completion timeout");
        std::uint8_t* mapped32 = nullptr;
        std::uint8_t* mapped16 = nullptr;
        check(output32.readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped32)),
              "exposure output32 Map");
        check(output16.readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped16)),
              "exposure output16 Map");
        std::vector<std::uint16_t> current;
        UINT maxUlp = 0;
        for (UINT y = 0; y < kOutputHeight; ++y) {
            const auto* row32 = reinterpret_cast<const std::uint16_t*>(mapped32 + output32.readback.footprint.Offset +
                y * output32.readback.footprint.Footprint.RowPitch);
            const auto* row16 = reinterpret_cast<const std::uint16_t*>(mapped16 + output16.readback.footprint.Offset +
                y * output16.readback.footprint.Footprint.RowPitch);
            for (UINT x = 0; x < kOutputWidth * 4; ++x) {
                if ((row32[x] & 0x7c00) == 0x7c00 || (row16[x] & 0x7c00) == 0x7c00)
                    fail("nonfinite manual exposure output");
                maxUlp = std::max(maxUlp, static_cast<UINT>(std::abs(int(row32[x]) - int(row16[x]))));
                current.push_back(row32[x]);
            }
        }
        output32.readback.buffer->Unmap(0, nullptr);
        output16.readback.buffer->Unmap(0, nullptr);
        if (maxUlp > 1)
            fail("R32 exposure differs from correctly rounded R16 oracle");
        if (!previous.empty() && current != previous)
            ++changedWithExposure;
        previous = std::move(current);
        std::printf("OWNED_D3D12_TEMPORAL_CASE_PASS case=r32_exposure_numeric replay=%u exposure=%.9g "
                    "r16_bits=0x%04x max_output_ulp=%u gpu_producer=rtv_clear release_before_completion=%u\n",
                    replay, values[replay], rounded[replay], maxUlp, replay == 2);
    }
    if (!changedWithExposure)
        fail("manual exposure oracle insensitive to changed replay source");
}

void runCommandListCreationNegatives(Gpu& gpu) {
    ComPtr<ID3D12CommandAllocator> directAllocator;
    check(gpu.device->CreateCommandAllocator(
              D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&directAllocator)),
          "CreateCommandAllocator(command-list negatives)");

    {
        ComPtr<ID3D12GraphicsCommandList> list;
        const HRESULT hr = gpu.device->CreateCommandList(
            2, D3D12_COMMAND_LIST_TYPE_DIRECT, directAllocator.Get(), nullptr,
            IID_PPV_ARGS(&list));
        expectHRESULT(hr, E_INVALIDARG, "single_node_create_command_list_mask2");
        if (list)
            fail("invalid NodeMask returned command list");
    }

    {
        ComPtr<ID3D12GraphicsCommandList> list;
        const HRESULT hr = gpu.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_COMPUTE, directAllocator.Get(), nullptr,
            IID_PPV_ARGS(&list));
        expectHRESULT(hr, E_INVALIDARG, "allocator_list_type_mismatch");
        if (list)
            fail("allocator/list type mismatch returned command list");
    }
}

void runPositiveValidationControl(Gpu& gpu,
                                  IMTLD3D12TemporalUpscaleState* state,
                                  ID3D12Resource* depth,
                                  ID3D12Resource* motion) {
    ComPtr<ID3D12Resource> color = makeColor(gpu, 108);
    OutputResource output = makeOutput(gpu, 450);
    const auto desc =
        makeFrameDesc(color.Get(), output.resource.Get(), depth, motion, FALSE);
    Recording recording = gpu.makeRecording();
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                  S_OK, "positive_valid_temporal_record");
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    check(recording.list->Close(), "positive control Close");
    std::puts("OWNED_D3D12_TEMPORAL_CASE_PASS case=positive_valid_control");
}

void runOutputSubrectPositive(Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension,
                              ID3D12Resource* depth, ID3D12Resource* motion) {
    constexpr std::uint8_t inputTag = 132;
    ComPtr<IMTLD3D12TemporalUpscaleState> state =
        createState(deviceExtension, TRUE);
    ComPtr<ID3D12Resource> color = makeColor(gpu, inputTag);
    OutputResource output = makeOutput(gpu, 475);
    auto desc =
        makeFrameDesc(color.Get(), output.resource.Get(), depth, motion, FALSE);
    desc.OutputBaseX = kSubrectBaseX;
    desc.OutputBaseY = kSubrectBaseY;

    Recording recording = gpu.makeRecording();
    auto invalid = desc;
    invalid.OutputBaseX = kOutputBacking - kOutputWidth + 1;
    invalid.CapOutputToTemporalMaxScale = TRUE;
    expectHRESULT(recording.extension->TemporalUpscale(state.Get(), &invalid),
                  E_INVALIDARG, "capped_output_full_rect_bounds");
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    expectHRESULT(recording.extension->TemporalUpscale(state.Get(), &desc),
                  S_OK, "positive_nonzero_output_subrect_record");
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "output subrect Close");
    gpu.executeAndWait(recording.list.Get());

    const OutputStats stats = validateOutput(
        output, inputTag, "output_subrect", 0, kSubrectBaseX, kSubrectBaseY);
    std::printf(
        "OWNED_D3D12_TEMPORAL_CASE_PASS case=output_subrect_positive "
        "enabled=1 base=%u,%u size=%ux%u backing=%ux%u input_tag=%u "
        "output_tag=%u mean=%.9g outside_sentinel=1\n",
        kSubrectBaseX, kSubrectBaseY, kOutputWidth, kOutputHeight,
        kOutputBacking, kOutputBacking, static_cast<unsigned>(inputTag),
        stats.outputTag, stats.mean);
}

void runFrameNegatives(Gpu& gpu, IMTLD3D12TemporalUpscaleState* state,
                       ID3D12Resource* depth, ID3D12Resource* motion) {
    ComPtr<ID3D12Resource> color = makeColor(gpu, 112);
    OutputResource output = makeOutput(gpu, 500);
    const auto base = makeFrameDesc(color.Get(), output.resource.Get(), depth, motion);

    {
        Recording recording = gpu.makeRecording();
        check(recording.list->Close(), "negative closed Close");
        expectHRESULT(recording.extension->ValidateRecording(), E_FAIL,
                      "closed_list_validate_recording");
        expectHRESULT(recording.extension->TemporalUpscale(state, &base), E_FAIL,
                      "closed_list_temporal_evaluate");
    }

    {
        Recording recording = gpu.makeRecording();
        auto desc = base;
        desc.JitterOffsetX = std::numeric_limits<float>::quiet_NaN();
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "nonfinite_jitter");
        check(recording.list->Close(), "negative jitter Close");
    }

    {
        Recording recording = gpu.makeRecording();
        auto desc = base;
        desc.DepthBaseY = 1;
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "depth_input_bounds");
        check(recording.list->Close(), "negative depth bounds Close");
    }

    {
        Recording recording = gpu.makeRecording();
        auto desc = base;
        desc.ColorBaseX = 1;
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "color_input_bounds");
        check(recording.list->Close(), "negative color bounds Close");
    }

    {
        Recording recording = gpu.makeRecording();
        auto desc = base;
        desc.MotionBaseX = std::numeric_limits<UINT>::max();
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "motion_input_bounds_overflow");
        desc = base;
        desc.MotionVector = color.Get();
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      DXGI_ERROR_UNSUPPORTED, "unsupported_motion_format");
        desc = base;
        desc.Output = nullptr;
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "missing_output");
        desc = base;
        desc.OutputBaseX = 1;
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "output_subrect_without_opt_in");
        check(recording.list->Close(), "negative output guards Close");
    }

    {
        Gpu foreign;
        auto foreignColor = makeColor(foreign, 112);
        Recording recording = gpu.makeRecording();
        auto desc = base;
        desc.Color = foreignColor.Get();
        expectHRESULT(recording.extension->TemporalUpscale(state, &desc),
                      E_INVALIDARG, "foreign_device_color");
        ComPtr<IMTLD3D12DeviceExt> foreignExtension;
        check(foreign.device->QueryInterface(kIID_IMTLD3D12DeviceExt,
                  reinterpret_cast<void**>(foreignExtension.GetAddressOf())),
              "foreign device extension");
        auto foreignState = createState(foreignExtension.Get());
        expectHRESULT(recording.extension->TemporalUpscale(foreignState.Get(), &base),
                      E_INVALIDARG, "foreign_device_state");
        check(recording.list->Close(), "negative foreign device Close");
    }
}

void runReleaseBeforeExecuteReplay(Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension,
                                   ID3D12Resource* depth, ID3D12Resource* motion) {
    constexpr std::uint8_t inputTag = 116;
    ComPtr<ID3D12Resource> color = makeColor(gpu, inputTag);
    OutputResource output = makeOutput(gpu, 600);
    ComPtr<IMTLD3D12TemporalUpscaleState> state = createState(deviceExtension);
    auto desc = makeFrameDesc(color.Get(), output.resource.Get(), depth, motion, FALSE);
    Recording recording = recordTemporal(gpu, state.Get(), desc, output);

    state.Reset();
    std::puts("OWNED_D3D12_TEMPORAL_CASE case=release_before_execute "
              "caller_state_released=1 caller_reset=0");

    gpu.executeAndWait(recording.list.Get());
    OutputStats first = validateOutput(output, inputTag, "release_before_execute", 0);
    std::printf("OWNED_D3D12_TEMPORAL_CASE_REPLAY case=release_before_execute "
                "execution=1 expected_effective_reset=1 input_tag=%u output_tag=%u mean=%.9g\n",
                static_cast<unsigned>(inputTag), first.outputTag, first.mean);

    gpu.executeAndWait(recording.list.Get());
    OutputStats second = validateOutput(output, inputTag, "release_before_execute", 1);
    std::printf("OWNED_D3D12_TEMPORAL_CASE_REPLAY case=release_before_execute "
                "execution=2 expected_effective_reset=0 input_tag=%u output_tag=%u mean=%.9g\n",
                static_cast<unsigned>(inputTag), second.outputTag, second.mean);
    std::puts("OWNED_D3D12_TEMPORAL_CASE_PASS case=release_before_execute_replay "
              "recorded_state_retained=1");
}

void runCallerResetReplay(Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension,
                          ID3D12Resource* depth, ID3D12Resource* motion) {
    constexpr std::uint8_t inputTag = 124;
    ComPtr<ID3D12Resource> color = makeColor(gpu, inputTag);
    OutputResource output = makeOutput(gpu, 700);
    ComPtr<IMTLD3D12TemporalUpscaleState> state = createState(deviceExtension);
    auto desc = makeFrameDesc(color.Get(), output.resource.Get(), depth, motion, TRUE);
    Recording recording = recordTemporal(gpu, state.Get(), desc, output);

    for (UINT execution = 1; execution <= 2; ++execution) {
        gpu.executeAndWait(recording.list.Get());
        OutputStats stats =
            validateOutput(output, inputTag, "caller_reset_replay", execution - 1);
        std::printf("OWNED_D3D12_TEMPORAL_CASE_REPLAY case=caller_reset "
                    "execution=%u expected_effective_reset=1 input_tag=%u "
                    "output_tag=%u mean=%.9g\n",
                    execution, static_cast<unsigned>(inputTag),
                    stats.outputTag, stats.mean);
    }
    std::puts("OWNED_D3D12_TEMPORAL_CASE_PASS case=caller_reset_replay "
              "caller_reset_each_execution=1");
}

void runGenerationTransitionReplay(
    Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension) {
    constexpr UINT kLargeBacking = 80;
    constexpr std::uint8_t kTagA = 104;
    constexpr std::uint8_t kTagB = 152;

    ComPtr<IMTLD3D12TemporalUpscaleState> state = createState(deviceExtension);

    ComPtr<ID3D12Resource> depthA = makeDepth(gpu);
    ComPtr<ID3D12Resource> motionA = makeMotion(gpu);
    ComPtr<ID3D12Resource> colorA = makeColor(gpu, kTagA);
    OutputResource outputA = makeOutput(gpu, 800);
    auto descA =
        makeFrameDesc(colorA.Get(), outputA.resource.Get(), depthA.Get(), motionA.Get(), FALSE);
    Recording recordingA = recordTemporal(gpu, state.Get(), descA, outputA);

    ComPtr<ID3D12Resource> depthB =
        makeDepth(gpu, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                  DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                  kLargeBacking, kLargeBacking);
    ComPtr<ID3D12Resource> motionB =
        makeMotion(gpu, DXGI_FORMAT_R16G16_FLOAT, kLargeBacking, kLargeBacking);
    ComPtr<ID3D12Resource> colorB =
        makeColor(gpu, kTagB, DXGI_FORMAT_R8G8B8A8_UNORM,
                  kLargeBacking, kLargeBacking);
    OutputResource outputB = makeOutput(gpu, 801);
    auto descB =
        makeFrameDesc(colorB.Get(), outputB.resource.Get(), depthB.Get(), motionB.Get(), FALSE);
    Recording recordingB = recordTemporal(gpu, state.Get(), descB, outputB);

    std::puts(
        "OWNED_D3D12_TEMPORAL_GENERATION_CAPTURE same_feature=1 "
        "A_backing=64x64 B_backing=80x80 logical_content=64x64 "
        "captured_before_execute=1");

    struct Execution {
        const char* name;
        Recording* recording;
        OutputResource* output;
        std::uint8_t inputTag;
        UINT expectedReset;
    };
    const std::array<Execution, 4> executions{{
        {"A1", &recordingA, &outputA, kTagA, 1},
        {"B1", &recordingB, &outputB, kTagB, 1},
        {"A2", &recordingA, &outputA, kTagA, 1},
        {"A3", &recordingA, &outputA, kTagA, 0},
    }};

    for (std::size_t index = 0; index < executions.size(); ++index) {
        const Execution& execution = executions[index];
        gpu.executeAndWait(execution.recording->list.Get());
        const OutputStats stats = validateOutput(
            *execution.output, execution.inputTag,
            "generation_transition", static_cast<UINT>(index));
        std::printf(
            "OWNED_D3D12_TEMPORAL_GENERATION_PASS execution=%s sequence=%u "
            "expected_effective_reset=%u input_tag=%u output_tag=%u "
            "mean=%.9g outside_sentinel=1\n",
            execution.name, static_cast<unsigned>(index + 1), execution.expectedReset,
            static_cast<unsigned>(execution.inputTag), stats.outputTag, stats.mean);
    }
    std::puts(
        "OWNED_D3D12_TEMPORAL_CASE_PASS case=generation_transition_replay "
        "expected_reset_sequence=1,1,1,0");
}

void runDescriptorABCReuse(Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension) {
    constexpr UINT inputWidth = 1280, inputHeight = 720;
    const std::array<UINT, 4> widths{1920, 1600, 2560, 1920};
    const UINT maxWidth = *std::max_element(widths.begin(), widths.end());
    const UINT maxHeight = maxWidth * inputHeight / inputWidth;
    const UINT padding = kOutputBacking - kOutputWidth;
    auto depth = makeDepth(gpu, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                           DXGI_FORMAT_D32_FLOAT_S8X24_UINT, inputWidth, inputHeight);
    auto motion = makeMotion(gpu, DXGI_FORMAT_R16G16_FLOAT, inputWidth, inputHeight);
    auto create = makeCreateDesc();
    create.InputWidth = inputWidth;
    create.InputHeight = inputHeight;
    create.OutputWidth = maxWidth;
    create.OutputHeight = maxHeight;
    create.DynamicOutputEnabled = TRUE;
    ComPtr<IMTLD3D12TemporalUpscaleState> state;
    check(deviceExtension->CreateTemporalUpscaleState(&create, &state),
          "ABC descriptor reuse CreateTemporalUpscaleState");
    const std::array<std::uint8_t, 4> tags{112, 144, 176, 112};
    std::array<OutputResource, 4> outputs;
    std::array<Recording, 4> recordings;
    for (std::size_t i = 0; i < recordings.size(); ++i) {
        auto color = makeColor(gpu, tags[i], DXGI_FORMAT_R8G8B8A8_UNORM, inputWidth, inputHeight);
        outputs[i] = makeOutput(gpu, 880 + static_cast<UINT>(i), DXGI_FORMAT_R8G8B8A8_UNORM,
                                maxWidth + padding, maxHeight + padding);
        auto desc = makeFrameDesc(color.Get(), outputs[i].resource.Get(), depth.Get(), motion.Get(), TRUE);
        desc.InputContentWidth = inputWidth;
        desc.InputContentHeight = inputHeight;
        desc.OutputWidth = widths[i];
        desc.OutputHeight = widths[i] * inputHeight / inputWidth;
        recordings[i] = recordTemporal(gpu, state.Get(), desc, outputs[i]);
    }
    state.Reset(); // Each immutable recording owns the feature and input color.
    double firstA = 0;
    for (std::size_t i = 0; i < recordings.size(); ++i) {
        for (UINT replay = 0; replay < 2; ++replay) {
            gpu.executeAndWait(recordings[i].list.Get());
            const auto stats = validateOutput(outputs[i], tags[i], "descriptor_ABC_reuse",
                static_cast<UINT>(i * 2 + replay), 0, 0, widths[i], widths[i] * inputHeight / inputWidth);
            if (i == 0 && replay == 0) firstA = stats.mean;
            if (i == 3 && std::fabs(stats.mean - firstA) > 1.0 / 255.0)
                fail("ABC restored A differs from cold explicit-reset A");
            std::printf("OWNED_D3D12_TEMPORAL_ABC sequence=%u replay=%u width=%u height=%u "
                "explicit_reset=1 caller_released=1 mean=%.9g outside_sentinel=1\n",
                static_cast<unsigned>(i), replay, widths[i], widths[i] * inputHeight / inputWidth, stats.mean);
        }
    }
    std::puts("OWNED_D3D12_TEMPORAL_CASE_PASS case=descriptor_ABC_reuse "
              "sequence=A,B,C,A executions=8 explicit_reset=1 caller_released=1");
}

void runDynamicOutputOldRecordReplay(
    Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension) {
    constexpr UINT inputWidth = 1280, inputHeight = 720;
    constexpr UINT widthA = 1600, heightA = 900, widthB = 1920, heightB = 1080;
    const UINT padding = kOutputBacking - kOutputWidth;
    const UINT backingWidth = widthB + padding, backingHeight = heightB + padding;
    constexpr std::uint8_t tagA = 72, tagB = 184;
    auto depth = makeDepth(gpu, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                           DXGI_FORMAT_D32_FLOAT_S8X24_UINT, inputWidth, inputHeight);
    auto motion = makeMotion(gpu, DXGI_FORMAT_R16G16_FLOAT, inputWidth, inputHeight);
    auto create = makeCreateDesc();
    create.InputWidth = inputWidth;
    create.InputHeight = inputHeight;
    create.OutputWidth = widthB;
    create.OutputHeight = heightB;
    create.DynamicOutputEnabled = TRUE;
    ComPtr<IMTLD3D12TemporalUpscaleState> state;
    check(deviceExtension->CreateTemporalUpscaleState(&create, &state),
          "dynamic output CreateTemporalUpscaleState");
    auto colorA = makeColor(gpu, tagA, DXGI_FORMAT_R8G8B8A8_UNORM, inputWidth, inputHeight);
    auto colorB = makeColor(gpu, tagB, DXGI_FORMAT_R8G8B8A8_UNORM, inputWidth, inputHeight);
    auto outputA = makeOutput(gpu, 850, DXGI_FORMAT_R8G8B8A8_UNORM, backingWidth, backingHeight);
    auto outputB = makeOutput(gpu, 851, DXGI_FORMAT_R8G8B8A8_UNORM, backingWidth, backingHeight);
    auto descA = makeFrameDesc(colorA.Get(), outputA.resource.Get(), depth.Get(), motion.Get());
    descA.InputContentWidth = inputWidth;
    descA.InputContentHeight = inputHeight;
    descA.OutputWidth = widthA;
    descA.OutputHeight = heightA;
    auto recordingA = recordTemporal(gpu, state.Get(), descA, outputA);
    auto descB = makeFrameDesc(colorB.Get(), outputB.resource.Get(), depth.Get(), motion.Get());
    descB.InputContentWidth = inputWidth;
    descB.InputContentHeight = inputHeight;
    auto recordingB = recordTemporal(gpu, state.Get(), descB, outputB);
    state.Reset();
    colorA.Reset();
    colorB.Reset();

    gpu.executeAndWait(recordingA.list.Get());
    validateOutput(outputA, tagA, "dynamic_output_A1", 0, 0, 0, widthA, heightA);
    gpu.executeAndWait(recordingB.list.Get());
    validateOutput(outputB, tagB, "dynamic_output_B1", 1, 0, 0, widthB, heightB);
    recordingB = {};
    outputB = {};
    auto clear = gpu.makeRecording();
    transition(clear.list.Get(), outputA.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    check(clear.list->Close(), "dynamic replay sentinel Close");
    gpu.executeAndWait(clear.list.Get());
    std::vector<std::array<std::uint8_t, 4>> sentinel(
        backingWidth * backingHeight, outputA.sentinel);
    uploadTexture(gpu, outputA.resource.Get(), sentinel.data(), backingWidth * 4,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto poisonReadback = gpu.makeRecording();
    copyTextureToReadback(poisonReadback.list.Get(), outputA.resource.Get(), outputA.readback);
    check(poisonReadback.list->Close(), "dynamic replay sentinel readback Close");
    gpu.executeAndWait(poisonReadback.list.Get());
    gpu.executeAndWait(recordingA.list.Get());
    const auto stats = validateOutput(
        outputA, tagA, "dynamic_output_A2", 2, 0, 0, widthA, heightA);
    // Compare the restored A generation with an independent first/reset frame.
    ComPtr<IMTLD3D12TemporalUpscaleState> freshState;
    create.OutputWidth = widthA;
    create.OutputHeight = heightA;
    check(deviceExtension->CreateTemporalUpscaleState(&create, &freshState),
          "dynamic replay reference CreateTemporalUpscaleState");
    auto freshColor = makeColor(gpu, tagA, DXGI_FORMAT_R8G8B8A8_UNORM, inputWidth, inputHeight);
    auto freshOutput = makeOutput(gpu, 852, DXGI_FORMAT_R8G8B8A8_UNORM, backingWidth, backingHeight);
    auto freshDesc = makeFrameDesc(freshColor.Get(), freshOutput.resource.Get(), depth.Get(), motion.Get(), TRUE);
    freshDesc.InputContentWidth = inputWidth;
    freshDesc.InputContentHeight = inputHeight;
    auto freshRecording = recordTemporal(gpu, freshState.Get(), freshDesc, freshOutput);
    gpu.executeAndWait(freshRecording.list.Get());
    const auto freshStats = validateOutput(
        freshOutput, tagA, "dynamic_output_fresh", 3, 0, 0, widthA, heightA);
    if (std::fabs(stats.mean - freshStats.mean) > 1.0 / 255.0)
        fail("old dynamic output generation replay differs from fresh reset");
    std::printf("OWNED_D3D12_TEMPORAL_CASE_PASS case=dynamic_output_old_record_replay "
                "sequence=A,B,A A=%ux%u B=%ux%u caller_released=1 B_retired=1 "
                "replay_mean=%.9g fresh_mean=%.9g outside_sentinel=1\n",
                widthA, heightA, widthB, heightB, stats.mean, freshStats.mean);
}

struct FutureSignalCallerContext {
    ID3D12CommandQueue* queueA = nullptr;
    ID3D12CommandQueue* queueB = nullptr;
    ID3D12Fence* dependency = nullptr;
    ID3D12Fence* completionB = nullptr;
    Recording* recordingA = nullptr;
    Recording* recordingB = nullptr;
};

DWORD WINAPI runFutureSignalCallerThread(LPVOID opaque) {
    auto* context = static_cast<FutureSignalCallerContext*>(opaque);
    check(context->queueB->Wait(context->dependency, 1),
          "crossqueue B.Wait(F,1)");

    ID3D12CommandList* listsB[] = {context->recordingB->list.Get()};
    context->queueB->ExecuteCommandLists(1, listsB);

    // The B queue cannot CPU-encode the submitted recording until A commits F=1.
    // Grow the same allocator beyond the old 64 inline heads while that exact
    // submission is deferred. Resetting the list is legal after Execute returns;
    // resetting the allocator itself is deliberately never done here.
    for (UINT reset = 0; reset < kDeferredResetCount; ++reset) {
        check(context->recordingB->list->Reset(
                  context->recordingB->allocator.Get(), nullptr),
              "deferred submitted list Reset");
        if (reset == 0 || reset + 1 == kDeferredResetCount)
            check(context->recordingB->extension->ValidateRecording(),
                  "deferred Reset ValidateRecording");
        check(context->recordingB->list->Close(),
              "deferred reset recording Close");
    }

    // The queue task must own the exact allocator/recording it captured at the
    // Execute API boundary. Drop every caller-side reference before unblocking B.
    context->recordingB->extension.Reset();
    context->recordingB->list.Reset();
    context->recordingB->allocator.Reset();

    ID3D12CommandList* listsA[] = {context->recordingA->list.Get()};
    context->queueA->ExecuteCommandLists(1, listsA);
    check(context->queueA->Signal(context->dependency, 1),
          "crossqueue A.Signal(F,1)");
    check(context->queueB->Signal(context->completionB, 1),
          "crossqueue B.Signal(done,1)");
    return 0;
}

void runCrossQueueFutureSignal(Gpu& gpu,
                               IMTLD3D12DeviceExt* deviceExtension,
                               ID3D12Resource* depth,
                               ID3D12Resource* motion) {
    constexpr std::uint8_t tagA = 108;
    constexpr std::uint8_t tagB = 124;

    ComPtr<ID3D12CommandQueue> queueA =
        createDirectQueue(gpu.device.Get(), "CreateCommandQueue(crossqueue A)");
    ComPtr<ID3D12CommandQueue> queueB =
        createDirectQueue(gpu.device.Get(), "CreateCommandQueue(crossqueue B)");
    ComPtr<ID3D12Fence> dependency;
    ComPtr<ID3D12Fence> completionB;
    check(gpu.device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dependency)),
          "CreateFence(crossqueue dependency)");
    check(gpu.device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completionB)),
          "CreateFence(crossqueue completion B)");

    ComPtr<IMTLD3D12TemporalUpscaleState> state = createState(deviceExtension);
    ComPtr<ID3D12Resource> colorB = makeColor(gpu, tagB);
    OutputResource outputB = makeOutput(gpu, 900);
    auto descB = makeFrameDesc(
        colorB.Get(), outputB.resource.Get(), depth, motion, FALSE);
    Recording recordingB = recordTemporal(gpu, state.Get(), descB, outputB);

    ComPtr<ID3D12Resource> colorA = makeColor(gpu, tagA);
    OutputResource outputA = makeOutput(gpu, 901);
    auto descA = makeFrameDesc(
        colorA.Get(), outputA.resource.Get(), depth, motion, FALSE);
    Recording recordingA = recordTemporal(gpu, state.Get(), descA, outputA);

    // Both recorded frames retain the same feature core/generation. Caller-side
    // feature release must not alter the queued execution contract.
    state.Reset();

    FutureSignalCallerContext context{
        queueA.Get(), queueB.Get(), dependency.Get(), completionB.Get(),
        &recordingA, &recordingB};
    runThreadBounded(runFutureSignalCallerThread, &context,
                     "crossqueue caller API sequence timeout");
    if (recordingB.list.Get() || recordingB.extension.Get() ||
        recordingB.allocator.Get())
        fail("crossqueue caller retained submitted B objects");

    waitForFenceValue(
        completionB.Get(), 1, "crossqueue future-signal GPU completion timeout");
    const OutputStats statsA =
        validateOutput(outputA, tagA, "crossqueue_A", 0);
    const OutputStats statsB =
        validateOutput(outputB, tagB, "crossqueue_B", 0);

    std::printf(
        "OWNED_D3D12_TEMPORAL_CASE_PASS case=crossqueue_future_signal "
        "order=B.Wait(F1),B.Execute,A.Execute,A.Signal(F1) bounded_timeout_ms=%lu "
        "same_feature=1 expected_encode_order=A,B A_output_tag=%u B_output_tag=%u "
        "post_submit_list_reset=1 reset_count=%u record_heads=%u "
        "caller_list_released_before_unblock=1 caller_allocator_released_before_unblock=1 "
        "gpu_retirement_after_caller_release=1\n",
        static_cast<unsigned long>(kFenceTimeoutMs), statsA.outputTag, statsB.outputTag,
        kDeferredResetCount, kDeferredRecordHeads);
}

struct OppositeOrderCallerContext {
    ID3D12CommandQueue* queueA = nullptr;
    ID3D12CommandQueue* queueB = nullptr;
    ID3D12Fence* start = nullptr;
    ID3D12Fence* completionA = nullptr;
    ID3D12Fence* completionB = nullptr;
    // D3D12 drops re-execution of a list whose previous execution has not completed, and every
    // submission here waits behind the start gate; each iteration therefore owns its recording.
    Recording* recordingsA = nullptr;
    Recording* recordingsB = nullptr;
};

DWORD WINAPI runOppositeOrderCallerThread(LPVOID opaque) {
    auto* context = static_cast<OppositeOrderCallerContext*>(opaque);
    check(context->queueA->Wait(context->start, 1),
          "opposite-order A.Wait(start,1)");
    check(context->queueB->Wait(context->start, 1),
          "opposite-order B.Wait(start,1)");

    for (UINT iteration = 0; iteration < kOppositeOrderIterations; ++iteration) {
        ID3D12CommandList* listsA[] = {context->recordingsA[iteration].list.Get()};
        ID3D12CommandList* listsB[] = {context->recordingsB[iteration].list.Get()};
        context->queueA->ExecuteCommandLists(1, listsA);
        context->queueB->ExecuteCommandLists(1, listsB);
    }
    check(context->queueA->Signal(context->completionA, 1),
          "opposite-order A.Signal(done,1)");
    check(context->queueB->Signal(context->completionB, 1),
          "opposite-order B.Signal(done,1)");

    // All queue waits and submissions are now recorded before releasing their shared gate.
    check(context->start->Signal(1), "opposite-order CPU start Signal(1)");
    return 0;
}

void runOppositeFeatureOrder(Gpu& gpu,
                             IMTLD3D12DeviceExt* deviceExtension) {
    constexpr std::uint8_t tagXA = 100;
    constexpr std::uint8_t tagXB = 108;
    constexpr std::uint8_t tagYA = 116;
    constexpr std::uint8_t tagYB = 124;

    ComPtr<IMTLD3D12TemporalUpscaleState> stateX = createState(deviceExtension);
    ComPtr<IMTLD3D12TemporalUpscaleState> stateY = createState(deviceExtension);

    // Every evaluation owns separate app-visible resources. The only shared
    // state is the intended temporal feature X/Y history itself.
    ComPtr<ID3D12Resource> depthXA = makeDepth(gpu);
    ComPtr<ID3D12Resource> motionXA = makeMotion(gpu);
    ComPtr<ID3D12Resource> colorXA = makeColor(gpu, tagXA);
    OutputResource outputXA = makeOutput(gpu, 920);

    ComPtr<ID3D12Resource> depthYA = makeDepth(gpu);
    ComPtr<ID3D12Resource> motionYA = makeMotion(gpu);
    ComPtr<ID3D12Resource> colorYA = makeColor(gpu, tagYA);
    OutputResource outputYA = makeOutput(gpu, 921);

    ComPtr<ID3D12Resource> depthYB = makeDepth(gpu);
    ComPtr<ID3D12Resource> motionYB = makeMotion(gpu);
    ComPtr<ID3D12Resource> colorYB = makeColor(gpu, tagYB);
    OutputResource outputYB = makeOutput(gpu, 922);

    ComPtr<ID3D12Resource> depthXB = makeDepth(gpu);
    ComPtr<ID3D12Resource> motionXB = makeMotion(gpu);
    ComPtr<ID3D12Resource> colorXB = makeColor(gpu, tagXB);
    OutputResource outputXB = makeOutput(gpu, 923);

    auto descXA = makeFrameDesc(
        colorXA.Get(), outputXA.resource.Get(), depthXA.Get(), motionXA.Get(), FALSE);
    auto descYA = makeFrameDesc(
        colorYA.Get(), outputYA.resource.Get(), depthYA.Get(), motionYA.Get(), FALSE);
    auto descYB = makeFrameDesc(
        colorYB.Get(), outputYB.resource.Get(), depthYB.Get(), motionYB.Get(), FALSE);
    auto descXB = makeFrameDesc(
        colorXB.Get(), outputXB.resource.Get(), depthXB.Get(), motionXB.Get(), FALSE);

    Recording recordingsA[kOppositeOrderIterations];
    Recording recordingsB[kOppositeOrderIterations];
    for (UINT iteration = 0; iteration < kOppositeOrderIterations; ++iteration) {
        recordingsA[iteration] = recordTemporalPair(
            gpu, stateX.Get(), descXA, outputXA,
            stateY.Get(), descYA, outputYA);
        recordingsB[iteration] = recordTemporalPair(
            gpu, stateY.Get(), descYB, outputYB,
            stateX.Get(), descXB, outputXB);
    }

    ComPtr<ID3D12CommandQueue> queueA =
        createDirectQueue(gpu.device.Get(), "CreateCommandQueue(opposite-order A)");
    ComPtr<ID3D12CommandQueue> queueB =
        createDirectQueue(gpu.device.Get(), "CreateCommandQueue(opposite-order B)");
    ComPtr<ID3D12Fence> start;
    ComPtr<ID3D12Fence> completionA;
    ComPtr<ID3D12Fence> completionB;
    check(gpu.device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&start)),
          "CreateFence(opposite-order start)");
    check(gpu.device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completionA)),
          "CreateFence(opposite-order completion A)");
    check(gpu.device->CreateFence(
              0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completionB)),
          "CreateFence(opposite-order completion B)");

    OppositeOrderCallerContext context{
        queueA.Get(), queueB.Get(), start.Get(), completionA.Get(), completionB.Get(),
        recordingsA, recordingsB};
    runThreadBounded(runOppositeOrderCallerThread, &context,
                     "opposite-order caller API sequence timeout");
    waitForFenceValue(
        completionA.Get(), 1, "opposite-order queue A GPU completion timeout");
    waitForFenceValue(
        completionB.Get(), 1, "opposite-order queue B GPU completion timeout");

    const OutputStats statsXA =
        validateOutput(outputXA, tagXA, "opposite_order_XA", 0);
    const OutputStats statsYA =
        validateOutput(outputYA, tagYA, "opposite_order_YA", 0);
    const OutputStats statsYB =
        validateOutput(outputYB, tagYB, "opposite_order_YB", 0);
    const OutputStats statsXB =
        validateOutput(outputXB, tagXB, "opposite_order_XB", 0);
    std::printf(
        "OWNED_D3D12_TEMPORAL_CASE_PASS case=opposite_feature_order "
        "queueA_order=X,Y queueB_order=Y,X iterations=%u disjoint_resources=1 "
        "cpu_start_gate=1 bounded_timeout_ms=%lu tags_XA_XB=%u,%u tags_YA_YB=%u,%u\n",
        kOppositeOrderIterations, static_cast<unsigned long>(kFenceTimeoutMs),
        statsXA.outputTag, statsXB.outputTag, statsYA.outputTag, statsYB.outputTag);
}

void runNominalInputGrowth(Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension) {
    constexpr UINT backing = 80;
    auto create = makeCreateDesc(); // Nominal 64x64 is not an active-content cap.
    // Neutral manual exposure keeps alternating tags distinguishable.
    create.FeatureFlags = kFlagMvLowRes | kFlagDepthInverted;
    ComPtr<IMTLD3D12TemporalUpscaleState> state;
    check(deviceExtension->CreateTemporalUpscaleState(&create, &state),
          "nominal input growth CreateTemporalUpscaleState");
    auto depth = makeDepth(gpu, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                           DXGI_FORMAT_D32_FLOAT_S8X24_UINT, backing, backing);
    auto motion = makeMotion(gpu, DXGI_FORMAT_R16G16_FLOAT, backing, backing);
    auto color = createTexture(gpu.device.Get(), backing, backing,
                               DXGI_FORMAT_R8G8B8A8_UNORM,
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)),
          "GPU freshness RTV heap");
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    auto output = makeOutput(gpu, 1100);
    ComPtr<ID3D12Fence> completion;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion)),
          "GPU freshness completion fence");

    // Start above nominal create dimensions, shrink, then grow again using the
    // same valid backing. Each pair also exercises ResetHistory=false.
    constexpr std::array<std::uint8_t, 6> tags{{64, 192, 64, 192, 64, 192}};
    for (UINT frame = 0; frame < tags.size(); ++frame) {
        auto desc = makeFrameDesc(color.Get(), output.resource.Get(), depth.Get(), motion.Get(),
                                  frame == 0 ? TRUE : FALSE);
        const bool shrink = frame == 2 || frame == 3;
        desc.InputContentWidth = shrink ? kInputWidth : backing;
        desc.InputContentHeight = shrink ? kInputHeight : backing;
        // Actual resource bounds still apply after nominal growth is accepted.
        if (frame == 0) {
            auto negative = gpu.makeRecording();
            auto invalid = desc;
            invalid.ColorBaseX = 1;
            expectHRESULT(negative.extension->TemporalUpscale(state.Get(), &invalid),
                          E_INVALIDARG, "nominal_growth_color_actual_oob");
            invalid = desc;
            invalid.DepthBaseY = 1;
            expectHRESULT(negative.extension->TemporalUpscale(state.Get(), &invalid),
                          E_INVALIDARG, "nominal_growth_depth_actual_oob");
            invalid = desc;
            invalid.MotionBaseX = 1;
            expectHRESULT(negative.extension->TemporalUpscale(state.Get(), &invalid),
                          E_INVALIDARG, "nominal_growth_motion_actual_oob");
            check(negative.list->Close(), "nominal growth negative Close");
        }
        Recording recording = gpu.makeRecording();
        transition(recording.list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (frame)
            transition(recording.list.Get(), color.Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float value = static_cast<float>(tags[frame]) / 255.0f;
        const float clear[] = {value, value, value, 1.0f};
        recording.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        transition(recording.list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // No upload, submission, or CPU wait separates the GPU producer and SR.
        check(recording.extension->TemporalUpscale(state.Get(), &desc),
              "nominal input growth TemporalUpscale");
        transition(recording.list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "GPU freshness Close");

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            fail("GPU freshness event", HRESULT_FROM_WIN32(GetLastError()));
        const UINT64 fenceValue = frame + 1;
        check(completion->SetEventOnCompletion(fenceValue, event),
              "GPU freshness register completion before submit");
        struct Submission {
            ID3D12CommandQueue* queue;
            ID3D12CommandList* list;
            ID3D12Fence* fence;
            UINT64 value;
        } submission{gpu.queue.Get(), recording.list.Get(), completion.Get(), fenceValue};
        runThreadBounded([](LPVOID opaque) -> DWORD {
            auto* submission = static_cast<Submission*>(opaque);
            ID3D12CommandList* lists[] = {submission->list};
            submission->queue->ExecuteCommandLists(1, lists);
            check(submission->queue->Signal(submission->fence, submission->value),
                  "GPU freshness Signal");
            return 0;
        }, &submission, "GPU freshness submission timeout");
        const DWORD wait = WaitForSingleObject(event, kFenceTimeoutMs);
        if (wait != WAIT_OBJECT_0)
            fail("GPU freshness completion timeout",
                 HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
        CloseHandle(event);
        waitForFenceValue(completion.Get(), fenceValue, "GPU freshness fence wait");
        check(gpu.device->GetDeviceRemovedReason(), "GPU freshness device removed");

        const auto stats = validateOutput(output, tags[frame], "nominal_input_growth", frame);
        const double mean = stats.mean * 255.0;
        const double delta = std::fabs(mean - tags[frame]);
        // +/-32 bands are disjoint: a previous 64/192 frame cannot pass.
        if (delta > 32 ||
            (frame && std::fabs(mean - tags[frame - 1]) < 96))
            fail("GPU freshness current tag missing or previous tag retained");
        std::printf("OWNED_D3D12_TEMPORAL_NOMINAL_GROWTH_PASS frame=%u "
                    "create_input=64x64 backing=80x80 input=%ux%u tag=%u mean=%.9g delta=%.9g "
                    "caller_reset=%u same_resources=1 gpu_producer=1 outside_sentinel=1\n",
                    frame, desc.InputContentWidth,
                    desc.InputContentHeight, static_cast<unsigned>(tags[frame]), mean, delta,
                    static_cast<unsigned>(desc.ResetHistory));
    }
}

void runContinuousFrames(Gpu& gpu, IMTLD3D12DeviceExt* deviceExtension,
                         ID3D12Resource* depth, ID3D12Resource* motion,
                         const char* variant = "typed",
                         DXGI_FORMAT colorParentFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
                         DXGI_FORMAT outputParentFormat = DXGI_FORMAT_R8G8B8A8_UNORM) {
    ComPtr<IMTLD3D12TemporalUpscaleState> state = createState(deviceExtension);
    std::printf(
        "OWNED_D3D12_TEMPORAL_CREATE variant=%s flags=74 input=64x64 "
        "logical_output=128x128 output_backing=136x136 color_parent_dxgi=%u "
        "output_parent_dxgi=%u reverse_depth=1 auto_exposure=1 caller_first_reset=0\n",
        variant, static_cast<unsigned>(colorParentFormat),
        static_cast<unsigned>(outputParentFormat));

    for (UINT frame = 0; frame < kFrames; ++frame) {
        const std::uint8_t inputTag = static_cast<std::uint8_t>(92u + frame * 8u);
        ComPtr<ID3D12Resource> color =
            makeColor(gpu, inputTag, colorParentFormat);
        OutputResource output =
            makeOutput(gpu, frame, outputParentFormat);
        auto desc = makeFrameDesc(color.Get(), output.resource.Get(), depth, motion, FALSE);
        // Auto exposure must ignore even an incompatible caller-supplied texture.
        desc.ExposureTexture = color.Get();
        Recording recording = recordTemporal(gpu, state.Get(), desc, output);
        gpu.executeAndWait(recording.list.Get());
        OutputStats stats = validateOutput(output, inputTag, variant, frame);
        const double expected = static_cast<double>(inputTag) / 255.0;
        std::printf("OWNED_D3D12_TEMPORAL_FRAME_PASS variant=%s frame=%u caller_reset=0 "
                    "expected_effective_reset=%u "
                    "input_tag=%u input_mean=%.9g output_tag=%u output_mean=%.9g "
                    "sameframe_delta=%.9g outside_sentinel=1\n",
                    variant, frame, frame == 0 ? 1u : 0u,
                    static_cast<unsigned>(inputTag), expected,
                    stats.outputTag, stats.mean, std::fabs(stats.mean - expected));
    }
    std::printf(
        "OWNED_D3D12_TEMPORAL_CASE_PASS case=continuous_8_frames variant=%s "
        "first_reset_backend_owned=1 caller_reset_all_false=1\n",
        variant);
}

int run() {
    printModulePath(L"d3d12.dll", "d3d12");
    requireOwnedBackend();

    Gpu gpu;
    ComPtr<IMTLD3D12DeviceExt> deviceExtension;
    check(gpu.device->QueryInterface(
              kIID_IMTLD3D12DeviceExt,
              reinterpret_cast<void**>(deviceExtension.GetAddressOf())),
          "QueryInterface(IMTLD3D12DeviceExt)");
    BOOL supported = FALSE;
    check(deviceExtension->SupportsTemporalUpscale(&supported), "SupportsTemporalUpscale");
    if (!supported)
        fail("SupportsTemporalUpscale returned FALSE");
    std::puts("OWNED_D3D12_TEMPORAL_CASE_PASS case=device_extension supported=1");

    ComPtr<ID3D12Resource> depth = makeDepth(gpu);
    ComPtr<ID3D12Resource> motion = makeMotion(gpu);
    runNominalInputGrowth(gpu, deviceExtension.Get());
    runR32ExposureReplay(gpu, deviceExtension.Get(), depth.Get(), motion.Get());
    std::puts("OWNED_D3D12_TEMPORAL_FORMAT role=color dxgi=28");
    std::puts("OWNED_D3D12_TEMPORAL_FORMAT role=output dxgi=28 backing=136 logical=128");
    std::puts("OWNED_D3D12_TEMPORAL_FORMAT role=motion dxgi=34 zeros=1");
    std::puts("OWNED_D3D12_TEMPORAL_FORMAT role=depth dxgi=20 dsv_clear=1 reverse_depth=1");

    ComPtr<IMTLD3D12TemporalUpscaleState> validationState = createState(deviceExtension.Get());
    runPositiveValidationControl(
        gpu, validationState.Get(), depth.Get(), motion.Get());
    runCreateFlagNegative(deviceExtension.Get());
    runFrameNegatives(gpu, validationState.Get(), depth.Get(), motion.Get());
    validationState.Reset();
    runOutputSubrectPositive(
        gpu, deviceExtension.Get(), depth.Get(), motion.Get());

    runCommandListCreationNegatives(gpu);
    runReleaseBeforeExecuteReplay(gpu, deviceExtension.Get(), depth.Get(), motion.Get());
    runCallerResetReplay(gpu, deviceExtension.Get(), depth.Get(), motion.Get());
    runGenerationTransitionReplay(gpu, deviceExtension.Get());
    runDescriptorABCReuse(gpu, deviceExtension.Get());
    runDynamicOutputOldRecordReplay(gpu, deviceExtension.Get());
    runCrossQueueFutureSignal(
        gpu, deviceExtension.Get(), depth.Get(), motion.Get());
    runOppositeFeatureOrder(gpu, deviceExtension.Get());
    runContinuousFrames(
        gpu, deviceExtension.Get(), depth.Get(), motion.Get(), "typed");

    ComPtr<ID3D12Resource> typelessDepth =
        makeDepth(gpu, DXGI_FORMAT_R32G8X24_TYPELESS,
                  DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    ComPtr<ID3D12Resource> typelessMotion =
        makeMotion(gpu, DXGI_FORMAT_R16G16_TYPELESS);
    std::puts(
        "OWNED_D3D12_TEMPORAL_FORMAT variant=typeless "
        "color_parent_dxgi=27 output_parent_dxgi=27 "
        "motion_parent_dxgi=33 depth_parent_dxgi=19 depth_dsv_dxgi=20");
    runContinuousFrames(
        gpu, deviceExtension.Get(), typelessDepth.Get(), typelessMotion.Get(),
        "typeless_zzz", DXGI_FORMAT_R8G8B8A8_TYPELESS,
        DXGI_FORMAT_R8G8B8A8_TYPELESS);

    std::puts("OWNED_D3D12_TEMPORAL_PASS "
              "flags74=1 typed_frames=8 typeless_frames=8 release_before_execute=1 "
              "closed_list_replay=1 sentinel=1 negatives_exact_hr=1 "
              "positive_control=1 typeless_zzz=1 generation_transition=1 "
              "command_list_validation=1 output_subrect=1 crossqueue_future_signal=1 "
              "submitted_list_reset=1 record_head_growth=1 allocator_retention=1 "
              "opposite_feature_order=1 r32_exposure_numeric=1 gpu_exposure_replay=1 "
              "dynamic_output_old_record_replay=1 nominal_input_growth=1");
    return 0;
}

} // namespace

int main() {
    return run();
}
