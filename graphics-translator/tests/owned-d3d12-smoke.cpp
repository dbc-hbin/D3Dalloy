#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 16;
constexpr UINT kHeight = 16;
constexpr UINT kFrames = 8;
constexpr UINT kBufferCount = 2;
constexpr UINT kPatternSalt = 0x5au;
constexpr UINT kBarrierSeed = 0x12345678u;
constexpr DWORD kFenceTimeoutMs = 15000;
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";
constexpr wchar_t kGraphicsShader[] = L"owned-d3d12-graphics.hlsl";
constexpr wchar_t kComputeShader[] = L"owned-d3d12-compute.hlsl";

class SmokeFailure final : public std::runtime_error {
public:
    explicit SmokeFailure(const char* what) : std::runtime_error(what) {}
};

[[noreturn]] void fail(const char* what, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "OWNED_D3D12_SMOKE_FAIL step=%s hr=0x%08lx\n", what,
                 static_cast<unsigned long>(hr));
    throw SmokeFailure(what);
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr))
        fail(what, hr);
}

std::string utf8(const wchar_t* text) {
    if (!text)
        return {};
    const int chars = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (chars <= 1)
        return {};
    std::string result(static_cast<std::size_t>(chars), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), chars, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring modulePath(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        fail("GetModuleFileNameW", HRESULT_FROM_WIN32(GetLastError()));
    return std::wstring(path.data(), length);
}

std::wstring executableDirectory() {
    std::wstring path = modulePath(nullptr);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".";
    return path.substr(0, slash);
}

std::wstring joinPath(const std::wstring& directory, const wchar_t* name) {
    if (directory.empty())
        return name;
    const wchar_t tail = directory.back();
    if (tail == L'\\' || tail == L'/')
        return directory + name;
    return directory + L"\\" + name;
}

std::vector<char> readFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        fail("CreateFileW(shader)", HRESULT_FROM_WIN32(GetLastError()));

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        fail("GetFileSizeEx(shader)", error ? HRESULT_FROM_WIN32(error) : E_FAIL);
    }

    std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok || read != bytes.size())
        fail("ReadFile(shader)", HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT));
    return bytes;
}

ComPtr<ID3DBlob> compileShader(const std::wstring& path, const char* entry, const char* target) {
    const std::vector<char> source = readFile(path);
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        fail("D3DCompile", hr);
    }
    return code;
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

D3D12_RESOURCE_DESC textureDescription() {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = kWidth;
    description.Height = kHeight;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    auto heapDesc = heapProperties(heap);
    auto resourceDesc = bufferDescription(size, flags);
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heapDesc, D3D12_HEAP_FLAG_NONE, &resourceDesc, state,
                                          nullptr, IID_PPV_ARGS(&resource)),
          "CreateCommittedResource(buffer)");
    return resource;
}

struct TextureReadback {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
};

TextureReadback createTextureReadback(ID3D12Device* device, ID3D12Resource* texture) {
    TextureReadback readback;
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    device->GetCopyableFootprints(&description, 0, 1, 0, &readback.footprint,
                                  &readback.rows, &readback.rowBytes, &readback.totalBytes);
    if (readback.rows != kHeight || readback.rowBytes != kWidth * 4u || readback.totalBytes == 0)
        fail("unexpected texture readback footprint", E_FAIL);
    readback.buffer = createBuffer(device, readback.totalBytes, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    return readback;
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

void uavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device* device,
                                                const D3D12_ROOT_SIGNATURE_DESC& description) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT serialized = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                            &blob, &errors);
    if (FAILED(serialized)) {
        if (errors)
            std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        fail("D3D12SerializeRootSignature", serialized);
    }

    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)),
          "CreateRootSignature");
    return root;
}

struct Pipelines {
    ComPtr<ID3D12RootSignature> graphicsRoot;
    ComPtr<ID3D12PipelineState> graphicsPso;
    ComPtr<ID3D12RootSignature> computeRoot;
    ComPtr<ID3D12PipelineState> computePso;
    ComPtr<ID3D12PipelineState> barrierProducerPso;
    ComPtr<ID3D12PipelineState> barrierConsumerPso;

    Pipelines(ID3D12Device* device, const std::wstring& shaderDirectory, bool barrierStress) {
        const auto vs = compileShader(joinPath(shaderDirectory, kGraphicsShader), "vsMain", "vs_5_0");
        const auto ps = compileShader(joinPath(shaderDirectory, kGraphicsShader), "psMain", "ps_5_0");
        const auto cs = compileShader(joinPath(shaderDirectory, kComputeShader), "csMain", "cs_5_0");

        D3D12_ROOT_PARAMETER graphicsParameter{};
        graphicsParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        graphicsParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        graphicsParameter.Constants.ShaderRegister = 0;
        graphicsParameter.Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
        graphicsDescription.NumParameters = 1;
        graphicsDescription.pParameters = &graphicsParameter;
        graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        graphicsRoot = createRootSignature(device, graphicsDescription);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
        graphics.pRootSignature = graphicsRoot.Get();
        graphics.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        graphics.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        graphics.SampleMask = UINT_MAX;
        graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        graphics.RasterizerState.DepthClipEnable = TRUE;
        graphics.DepthStencilState.DepthEnable = FALSE;
        graphics.DepthStencilState.StencilEnable = FALSE;
        graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics.NumRenderTargets = 1;
        graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        graphics.SampleDesc.Count = 1;
        check(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&graphicsPso)),
              "CreateGraphicsPipelineState");

        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        std::array<D3D12_ROOT_PARAMETER, 2> computeParameters{};
        computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeParameters[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
        computeParameters[0].DescriptorTable.pDescriptorRanges = ranges.data();
        computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        computeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        computeParameters[1].Constants.ShaderRegister = 0;
        computeParameters[1].Constants.Num32BitValues = 4;

        D3D12_ROOT_SIGNATURE_DESC computeDescription{};
        computeDescription.NumParameters = static_cast<UINT>(computeParameters.size());
        computeDescription.pParameters = computeParameters.data();
        computeRoot = createRootSignature(device, computeDescription);

        D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
        compute.pRootSignature = computeRoot.Get();
        compute.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        check(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&computePso)),
              "CreateComputePipelineState");
        if (barrierStress) {
            const auto barrierProducer =
                compileShader(joinPath(shaderDirectory, kComputeShader), "csBarrierProducer", "cs_5_0");
            const auto barrierConsumer =
                compileShader(joinPath(shaderDirectory, kComputeShader), "csBarrierConsumer", "cs_5_0");
            compute.CS = {barrierProducer->GetBufferPointer(), barrierProducer->GetBufferSize()};
            check(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&barrierProducerPso)),
                  "CreateComputePipelineState(barrier producer)");
            compute.CS = {barrierConsumer->GetBufferPointer(), barrierConsumer->GetBufferSize()};
            check(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&barrierConsumerPso)),
                  "CreateComputePipelineState(barrier consumer)");
        }
    }
};

class CommandContext final {
public:
    CommandContext(ID3D12Device* device, ID3D12CommandQueue* queue) : device_(device), queue_(queue) {
        check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_)
            fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    }

    ~CommandContext() {
        if (event_)
            CloseHandle(event_);
    }

    ID3D12GraphicsCommandList* begin() {
        if (!allocator_) {
            check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator_)),
                  "CreateCommandAllocator");
            check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
                                             nullptr, IID_PPV_ARGS(&list_)),
                  "CreateCommandList");
        } else {
            check(allocator_->Reset(), "CommandAllocator::Reset");
            check(list_->Reset(allocator_.Get(), nullptr), "GraphicsCommandList::Reset");
        }
        return list_.Get();
    }

    void submitAndWait() {
        check(list_->Close(), "GraphicsCommandList::Close");
        ID3D12CommandList* commandLists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, commandLists);
        signalAndWait("command submission fence timeout");
    }

    void flushQueue() {
        signalAndWait("queue drain fence timeout");
    }

private:
    void signalAndWait(const char* timeoutStep) {
        const UINT64 value = ++fenceValue_;
        check(queue_->Signal(fence_.Get(), value), "CommandQueue::Signal");
        if (fence_->GetCompletedValue() < value) {
            check(fence_->SetEventOnCompletion(value, event_), "Fence::SetEventOnCompletion");
            const DWORD wait = WaitForSingleObject(event_, kFenceTimeoutMs);
            if (wait != WAIT_OBJECT_0)
                fail(timeoutStep,
                     HRESULT_FROM_WIN32(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
        }
    }

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0;
};

class HiddenWindow final {
public:
    HiddenWindow() {
        instance_ = GetModuleHandleW(nullptr);
        className_ = L"OwnedD3D12SmokeWindowClass";
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = instance_;
        cls.lpszClassName = className_;
        atom_ = RegisterClassExW(&cls);
        if (!atom_)
            fail("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError()));
        window_ = CreateWindowExW(0, className_, L"owned-d3d12-smoke", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr,
                                  instance_, nullptr);
        if (!window_)
            fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
        ShowWindow(window_, SW_SHOWNA);
        UpdateWindow(window_);
    }

    ~HiddenWindow() {
        if (window_)
            DestroyWindow(window_);
        if (atom_)
            UnregisterClassW(className_, instance_);
    }

    HWND get() const { return window_; }

private:
    HINSTANCE instance_ = nullptr;
    const wchar_t* className_ = nullptr;
    ATOM atom_ = 0;
    HWND window_ = nullptr;
};

struct Options {
    bool present = true;
    bool barrierStress = false;
    std::wstring shaderDirectory;
};

Options parseOptions(int argc, wchar_t** argv) {
    Options options;
    options.shaderDirectory = executableDirectory();
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--no-present") == 0) {
            options.present = false;
        } else if (std::wcscmp(argv[i], L"--barrier-stress") == 0) {
            options.barrierStress = true;
        } else if (std::wcscmp(argv[i], L"--shader-dir") == 0) {
            if (++i >= argc)
                fail("--shader-dir requires a path", E_INVALIDARG);
            options.shaderDirectory = argv[i];
        } else {
            fail("unknown command line option", E_INVALIDARG);
        }
    }
    return options;
}

void printModulePath(const wchar_t* name) {
    HMODULE module = GetModuleHandleW(name);
    if (!module)
        fail("GetModuleHandleW", HRESULT_FROM_WIN32(GetLastError()));
    const std::string path = utf8(modulePath(module).c_str());
    std::printf("OWNED_D3D12_SMOKE_MODULE name=%s path=%s\n",
                std::wcscmp(name, L"d3d12.dll") == 0 ? "d3d12" : "dxgi", path.c_str());
}

void requireOwnedBackend() {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12)
        fail("GetModuleHandleW(d3d12)", HRESULT_FROM_WIN32(GetLastError()));
    FARPROC raw = GetProcAddress(d3d12, "OwnedGraphicsBackendId");
    if (!raw)
        fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
    using BackendId = const char* (WINAPI*)();
    BackendId function = reinterpret_cast<BackendId>(raw);
    const char* id = function();
    if (!id || std::strcmp(id, kExpectedBackendId) != 0)
        fail("OwnedGraphicsBackendId mismatch", E_FAIL);
    std::printf("OWNED_D3D12_SMOKE_BACKEND id=%s\n", id);
}

std::uint32_t expectedValue(UINT frame, UINT x, UINT y) {
    const UINT selector = (x + 3u * y + frame + kPatternSalt) & 3u;
    const std::uint32_t mask = 1u << selector;
    const std::uint32_t coordinateTag = (x ^ (y * 17u) ^ kPatternSalt) & 0xffu;
    return 0xa5000000u | (coordinateTag << 16) | ((frame & 0xffu) << 8) | mask;
}

void validateReadback(ID3D12Resource* readback, UINT frame) {
    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * sizeof(std::uint32_t);
    D3D12_RANGE readRange{0, bytes};
    std::uint32_t* mapped = nullptr;
    check(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "readback Map");
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const std::uint32_t actual = mapped[y * kWidth + x];
            const std::uint32_t expected = expectedValue(frame, x, y);
            if (actual != expected) {
                std::fprintf(stderr,
                             "OWNED_D3D12_SMOKE_MISMATCH frame=%u x=%u y=%u expected=0x%08x actual=0x%08x\n",
                             frame, x, y, expected, actual);
                D3D12_RANGE written{0, 0};
                readback->Unmap(0, &written);
                fail("readback mismatch", E_FAIL);
            }
        }
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
}

void validatePresentedBackbuffer(const TextureReadback& readback, UINT frame) {
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(readback.totalBytes)};
    std::uint8_t* mapped = nullptr;
    check(readback.buffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)),
          "present readback Map");

    for (UINT y = 0; y < kHeight; ++y) {
        const std::uint8_t* row =
            mapped + readback.footprint.Offset +
            static_cast<std::size_t>(y) * readback.footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kWidth; ++x) {
            const UINT selector = (x + 3u * y + frame + kPatternSalt) & 3u;
            std::array<std::uint8_t, 4> expected{};
            expected[selector] = 255;
            const std::uint8_t* actual = row + static_cast<std::size_t>(x) * 4;
            if (std::memcmp(actual, expected.data(), expected.size()) != 0) {
                std::fprintf(stderr,
                             "OWNED_D3D12_SMOKE_PRESENT_MISMATCH frame=%u x=%u y=%u "
                             "expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x\n",
                             frame, x, y,
                             expected[0], expected[1], expected[2], expected[3],
                             actual[0], actual[1], actual[2], actual[3]);
                D3D12_RANGE written{0, 0};
                readback.buffer->Unmap(0, &written);
                fail("present backbuffer mismatch", E_FAIL);
            }
        }
    }

    D3D12_RANGE written{0, 0};
    readback.buffer->Unmap(0, &written);
}

std::uint32_t expectedBarrierValue(UINT index) {
    const std::uint32_t produced =
        0x13579bdfu ^ (index * 0x9e3779b9u) ^ kBarrierSeed ^ (kPatternSalt << 24);
    return ((produced << 7) | (produced >> 25)) ^ 0xa5a5a5a5u;
}

void validateBarrierReadback(ID3D12Resource* readback) {
    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * sizeof(std::uint32_t);
    D3D12_RANGE readRange{0, bytes};
    std::uint32_t* mapped = nullptr;
    check(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)),
          "barrier readback Map");
    for (UINT index = 0; index < kWidth * kHeight; ++index) {
        const std::uint32_t actual = mapped[index];
        const std::uint32_t expected = expectedBarrierValue(index);
        if (actual != expected) {
            std::fprintf(stderr,
                         "OWNED_D3D12_SMOKE_BARRIER_MISMATCH index=%u expected=0x%08x actual=0x%08x\n",
                         index, expected, actual);
            D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            fail("UAV barrier stress mismatch", E_FAIL);
        }
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
}

int run(const Options& options) {
    printModulePath(L"d3d12.dll");
    printModulePath(L"dxgi.dll");
    requireOwnedBackend();

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    DXGI_ADAPTER_DESC1 adapterDescription{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT enumerated = factory->EnumAdapters1(index, &candidate);
        if (enumerated == DXGI_ERROR_NOT_FOUND)
            break;
        check(enumerated, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 description{};
        check(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        ComPtr<ID3D12Device> candidateDevice;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&candidateDevice)))) {
            adapter = candidate;
            device = candidateDevice;
            adapterDescription = description;
            break;
        }
    }
    if (!device)
        fail("no D3D12 hardware adapter", DXGI_ERROR_UNSUPPORTED);

    const std::string adapterName = utf8(adapterDescription.Description);
    std::printf("OWNED_D3D12_SMOKE_ADAPTER description=%s vendor=0x%04x device=0x%04x\n",
                adapterName.c_str(), adapterDescription.VendorId, adapterDescription.DeviceId);
    std::printf("OWNED_D3D12_SMOKE_MODE mode=%s frames=%u barrier_stress=%s\n",
                options.present ? "present" : "no-present", kFrames,
                options.barrierStress ? "yes" : "no");

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

    Pipelines pipelines(device.Get(), options.shaderDirectory, options.barrierStress);

    auto textureDesc = textureDescription();
    auto defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                          IID_PPV_ARGS(&target)),
          "CreateCommittedResource(render target)");

    constexpr UINT64 resultBytes =
        static_cast<UINT64>(kWidth) * kHeight * sizeof(std::uint32_t);
    auto result = createBuffer(device.Get(), resultBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback = createBuffer(device.Get(), resultBytes, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    check(device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&rtvHeap)),
          "CreateDescriptorHeap(RTV)");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    D3D12_DESCRIPTOR_HEAP_DESC resourceHeapDescription{};
    resourceHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    resourceHeapDescription.NumDescriptors = 2;
    resourceHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> resourceHeap;
    check(device->CreateDescriptorHeap(&resourceHeapDescription, IID_PPV_ARGS(&resourceHeap)),
          "CreateDescriptorHeap(SRV/UAV)");
    const UINT descriptorSize =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = resourceHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.Get(), &srv, descriptor);
    descriptor.ptr += descriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kWidth * kHeight;
    uav.Buffer.StructureByteStride = sizeof(std::uint32_t);
    device->CreateUnorderedAccessView(result.Get(), nullptr, &uav, descriptor);

    std::unique_ptr<HiddenWindow> window;
    ComPtr<IDXGISwapChain3> swapchain;
    TextureReadback presentReadback;
    if (options.present) {
        window = std::make_unique<HiddenWindow>();
        DXGI_SWAP_CHAIN_DESC1 swapchainDescription{};
        swapchainDescription.Width = kWidth;
        swapchainDescription.Height = kHeight;
        swapchainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapchainDescription.SampleDesc.Count = 1;
        swapchainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapchainDescription.BufferCount = kBufferCount;
        swapchainDescription.Scaling = DXGI_SCALING_STRETCH;
        swapchainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapchainDescription.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        ComPtr<IDXGISwapChain1> baseSwapchain;
        check(factory->CreateSwapChainForHwnd(queue.Get(), window->get(), &swapchainDescription,
                                              nullptr, nullptr, &baseSwapchain),
              "CreateSwapChainForHwnd");
        check(baseSwapchain.As(&swapchain), "Query IDXGISwapChain3");
        presentReadback = createTextureReadback(device.Get(), target.Get());
    }

    CommandContext commands(device.Get(), queue.Get());
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kWidth),
                                  static_cast<float>(kHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};

    for (UINT frame = 0; frame < kFrames; ++frame) {
        const std::array<UINT, 4> constants{frame, kWidth, kHeight, kPatternSalt};
        UINT beforeIndex = 0;
        ComPtr<ID3D12Resource> backbuffer;
        if (swapchain) {
            beforeIndex = swapchain->GetCurrentBackBufferIndex();
            if (beforeIndex >= kBufferCount)
                fail("GetCurrentBackBufferIndex out of range", E_FAIL);
            check(swapchain->GetBuffer(beforeIndex, IID_PPV_ARGS(&backbuffer)),
                  "IDXGISwapChain::GetBuffer");
        }

        ID3D12GraphicsCommandList* list = commands.begin();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(pipelines.graphicsRoot.Get());
        list->SetPipelineState(pipelines.graphicsPso.Get());
        list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()),
                                           constants.data(), 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        transition(list, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[] = {resourceHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(pipelines.computeRoot.Get());
        list->SetPipelineState(pipelines.computePso.Get());
        list->SetComputeRootDescriptorTable(0, resourceHeap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRoot32BitConstants(1, static_cast<UINT>(constants.size()),
                                          constants.data(), 0);
        list->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, 1);

        transition(list, result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, resultBytes);
        transition(list, result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (backbuffer) {
            transition(list, target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list, backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(backbuffer.Get(), target.Get());
            transition(list, backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            copyTextureToReadback(list, backbuffer.Get(), presentReadback);
            transition(list, backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_PRESENT);
            transition(list, target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else {
            transition(list, target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        commands.submitAndWait();
        validateReadback(readback.Get(), frame);

        if (swapchain) {
            validatePresentedBackbuffer(presentReadback, frame);
            check(swapchain->Present(0, 0), "IDXGISwapChain::Present");
            const UINT afterIndex = swapchain->GetCurrentBackBufferIndex();
            const UINT expectedIndex = (beforeIndex + 1) % kBufferCount;
            if (afterIndex != expectedIndex) {
                std::fprintf(stderr,
                             "OWNED_D3D12_SMOKE_INDEX_MISMATCH frame=%u before=%u expected=%u actual=%u\n",
                             frame, beforeIndex, expectedIndex, afterIndex);
                fail("swapchain index did not advance", E_FAIL);
            }
            std::printf("OWNED_D3D12_SMOKE_FRAME frame=%u readback=ok "
                        "present_backbuffer=exact present=ok index=%u->%u\n",
                        frame, beforeIndex, afterIndex);
        } else {
            std::printf("OWNED_D3D12_SMOKE_FRAME frame=%u readback=ok present=skipped\n", frame);
        }
    }

    if (swapchain) {
        commands.flushQueue();
        std::puts("OWNED_D3D12_SMOKE_PRESENT_DRAIN fence=complete");
    }

    if (options.barrierStress) {
        const std::array<UINT, 4> constants{kBarrierSeed, kWidth, kHeight, kPatternSalt};
        ID3D12GraphicsCommandList* list = commands.begin();
        ID3D12DescriptorHeap* heaps[] = {resourceHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(pipelines.computeRoot.Get());
        list->SetComputeRootDescriptorTable(0, resourceHeap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRoot32BitConstants(1, static_cast<UINT>(constants.size()),
                                          constants.data(), 0);
        list->SetPipelineState(pipelines.barrierProducerPso.Get());
        list->Dispatch((kWidth * kHeight + 63) / 64, 1, 1);
        uavBarrier(list, result.Get());
        list->SetPipelineState(pipelines.barrierConsumerPso.Get());
        list->Dispatch((kWidth * kHeight + 63) / 64, 1, 1);
        transition(list, result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, resultBytes);
        transition(list, result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands.submitAndWait();
        validateBarrierReadback(readback.Get());
        std::puts("OWNED_D3D12_SMOKE_BARRIER_STRESS readback=ok pairs=1");
    }

    std::puts("OWNED_D3D12_SMOKE_PASS");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        return run(options);
    } catch (const SmokeFailure&) {
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "OWNED_D3D12_SMOKE_FAIL exception=%s\n", error.what());
        return 1;
    }
}
