#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";

int fail(const char* step, HRESULT hr = E_FAIL) {
    std::fprintf(stderr, "OWNED_CAPABILITIES_FAIL step=%s hr=0x%08lx\n",
                 step, static_cast<unsigned long>(hr));
    return 1;
}

bool has1(const D3D12_FEATURE_DATA_FORMAT_SUPPORT& support, D3D12_FORMAT_SUPPORT1 bit) {
    return (static_cast<std::uint32_t>(support.Support1) & static_cast<std::uint32_t>(bit)) != 0;
}

bool has2(const D3D12_FEATURE_DATA_FORMAT_SUPPORT& support, D3D12_FORMAT_SUPPORT2 bit) {
    return (static_cast<std::uint32_t>(support.Support2) & static_cast<std::uint32_t>(bit)) != 0;
}

bool isAllOnes(const D3D12_FEATURE_DATA_FORMAT_SUPPORT& support) {
    return static_cast<std::uint32_t>(support.Support1) == 0xffffffffu ||
           static_cast<std::uint32_t>(support.Support2) == 0xffffffffu;
}

void printModulePath(HMODULE module, const char* label) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) {
        std::printf("OWNED_CAPABILITIES_MODULE name=%s path=<unavailable>\n", label);
        return;
    }
    std::wprintf(L"OWNED_CAPABILITIES_MODULE name=%hs path=%ls\n", label, path);
}

int requireFormat(ID3D12Device* device, DXGI_FORMAT format,
                  D3D12_FORMAT_SUPPORT1 required1, D3D12_FORMAT_SUPPORT1 forbidden1,
                  D3D12_FORMAT_SUPPORT2 required2, D3D12_FORMAT_SUPPORT2 forbidden2,
                  const char* label) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    std::printf("OWNED_CAPABILITIES_FORMAT name=%s format=%u hr=0x%08lx support1=0x%08x support2=0x%08x\n",
                label, static_cast<unsigned>(format), static_cast<unsigned long>(hr),
                static_cast<unsigned>(support.Support1), static_cast<unsigned>(support.Support2));
    if (FAILED(hr))
        return fail(label, hr);
    if (isAllOnes(support))
        return fail("format mask remained all-ones");
    if ((static_cast<std::uint32_t>(support.Support1) & static_cast<std::uint32_t>(required1)) !=
        static_cast<std::uint32_t>(required1))
        return fail("missing required Support1 bit");
    if (static_cast<std::uint32_t>(support.Support1) & static_cast<std::uint32_t>(forbidden1))
        return fail("unexpected Support1 bit");
    if ((static_cast<std::uint32_t>(support.Support2) & static_cast<std::uint32_t>(required2)) !=
        static_cast<std::uint32_t>(required2))
        return fail("missing required Support2 bit");
    if (static_cast<std::uint32_t>(support.Support2) & static_cast<std::uint32_t>(forbidden2))
        return fail("unexpected Support2 bit");
    return 0;
}

int run() {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12)
        return fail("GetModuleHandleW(d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));
    printModulePath(d3d12, "d3d12.dll");

    using BackendIdFn = const char* (WINAPI*)();
    auto backendId = reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"));
    if (!backendId)
        return fail("OwnedGraphicsBackendId missing", HRESULT_FROM_WIN32(GetLastError()));
    const char* id = backendId();
    if (!id || std::strcmp(id, kExpectedBackendId) != 0)
        return fail("OwnedGraphicsBackendId mismatch");
    std::printf("OWNED_CAPABILITIES_BACKEND id=%s\n", id);

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return fail("CreateDXGIFactory2", hr);

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr))
        return fail("EnumAdapters1(0)", hr);

    DXGI_ADAPTER_DESC1 adapterDesc{};
    hr = adapter->GetDesc1(&adapterDesc);
    if (FAILED(hr))
        return fail("IDXGIAdapter1::GetDesc1", hr);
    std::wprintf(L"OWNED_CAPABILITIES_ADAPTER description=%ls vendor=0x%04x device=0x%04x\n",
                 adapterDesc.Description, adapterDesc.VendorId, adapterDesc.DeviceId);

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
        return fail("D3D12CreateDevice(FL11_0)", hr);
    std::puts("OWNED_CAPABILITIES_DEVICE minimum_fl=11_0 create=ok");

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (FAILED(hr))
        return fail("D3D12_FEATURE_D3D12_OPTIONS", hr);
    std::printf("OWNED_CAPABILITIES_OPTIONS binding_tier=%u tiled_tier=%u typed_uav_extra=%d logic_op=%d\n",
                static_cast<unsigned>(options.ResourceBindingTier),
                static_cast<unsigned>(options.TiledResourcesTier),
                options.TypedUAVLoadAdditionalFormats, options.OutputMergerLogicOp);

    const std::array<D3D_FEATURE_LEVEL, 5> requestedLevels{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_2,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
    levels.NumFeatureLevels = static_cast<UINT>(requestedLevels.size());
    levels.pFeatureLevelsRequested = requestedLevels.data();
    hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels));
    if (FAILED(hr))
        return fail("D3D12_FEATURE_FEATURE_LEVELS", hr);
    if (levels.MaxSupportedFeatureLevel < D3D_FEATURE_LEVEL_11_0)
        return fail("MaxSupportedFeatureLevel below 11_0");
    std::printf("OWNED_CAPABILITIES_FEATURE_LEVEL max=0x%04x\n",
                static_cast<unsigned>(levels.MaxSupportedFeatureLevel));

    int contractFailures = 0;
    // The feature-level and conservative-raster suites verify the FL 12_1 execution requirements.
    if (levels.MaxSupportedFeatureLevel != D3D_FEATURE_LEVEL_12_1)
        contractFailures += fail("MaxSupportedFeatureLevel is not 12_1");
    const HRESULT fl111Probe = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_1,
                                                 __uuidof(ID3D12Device), nullptr);
    const HRESULT fl12Probe = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                                __uuidof(ID3D12Device), nullptr);
    const HRESULT fl121Probe = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1,
                                                 __uuidof(ID3D12Device), nullptr);
    const HRESULT fl122Probe = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2,
                                                 __uuidof(ID3D12Device), nullptr);
    std::printf("OWNED_CAPABILITIES_FL12_PROBE fl11_1=0x%08lx fl12_0=0x%08lx fl12_1=0x%08lx max_reported=0x%04x\n",
                static_cast<unsigned long>(fl111Probe), static_cast<unsigned long>(fl12Probe),
                static_cast<unsigned long>(fl121Probe), static_cast<unsigned>(levels.MaxSupportedFeatureLevel));
    // A null device pointer probes support: S_FALSE when the level is supported.
    if (fl111Probe != S_FALSE)
        contractFailures += fail("FL11_1 probe", fl111Probe);
    if (fl12Probe != S_FALSE)
        contractFailures += fail("FL12_0 probe", fl12Probe);
    if (fl121Probe != S_FALSE)
        contractFailures += fail("FL12_1 probe", fl121Probe);
    if (SUCCEEDED(fl122Probe))
        contractFailures += fail("FL12_2 probe succeeded above reported max feature level", fl122Probe);
    if (!options.OutputMergerLogicOp || !options.TypedUAVLoadAdditionalFormats ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2 ||
        options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2)
        contractFailures += fail("FL12_0 required D3D12_OPTIONS caps");
    if (!options.ROVsSupported || options.ConservativeRasterizationTier < D3D12_CONSERVATIVE_RASTERIZATION_TIER_1)
        contractFailures += fail("FL12_1 required D3D12_OPTIONS caps");

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_7};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
    if (FAILED(hr))
        return fail("D3D12_FEATURE_SHADER_MODEL", hr);
    std::printf("OWNED_CAPABILITIES_SHADER_MODEL highest=0x%02x\n",
                static_cast<unsigned>(shaderModel.HighestShaderModel));

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    std::printf("OWNED_CAPABILITIES_OPTIONS5 hr=0x%08lx raytracing_tier=%u render_pass_tier=%u optional=1\n",
                static_cast<unsigned long>(hr), static_cast<unsigned>(options5.RaytracingTier),
                static_cast<unsigned>(options5.RenderPassesTier));

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
    if (FAILED(hr))
        return fail("D3D12_FEATURE_D3D12_OPTIONS7", hr);
    std::printf("OWNED_CAPABILITIES_OPTIONS7 mesh_shader_tier=%u sampler_feedback_tier=%u\n",
                static_cast<unsigned>(options7.MeshShaderTier),
                static_cast<unsigned>(options7.SamplerFeedbackTier));

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_UNKNOWN,
                                      D3D12_FORMAT_SUPPORT1_BUFFER,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                                          D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL |
                                          D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW),
                                      D3D12_FORMAT_SUPPORT2_NONE,
                                      static_cast<D3D12_FORMAT_SUPPORT2>(
                                          D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE |
                                          D3D12_FORMAT_SUPPORT2_TILED),
                                      "UNKNOWN");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                                          D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW),
                                      D3D12_FORMAT_SUPPORT2_NONE,
                                      static_cast<D3D12_FORMAT_SUPPORT2>(
                                          D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE |
                                          D3D12_FORMAT_SUPPORT2_TILED),
                                      "R8G8B8A8_TYPELESS");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                                          D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET),
                                      D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL,
                                      // TiledResourcesTier 2: reserved 2D textures (tiled suite).
                                      D3D12_FORMAT_SUPPORT2_TILED, D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP,
                                      "R8G8B8A8_UNORM");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_R16G16_TYPELESS,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                                          D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW),
                                      D3D12_FORMAT_SUPPORT2_NONE,
                                      static_cast<D3D12_FORMAT_SUPPORT2>(
                                          D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE),
                                      "R16G16_TYPELESS");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_R16G16_FLOAT,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                                          D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE),
                                      D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL,
                                      D3D12_FORMAT_SUPPORT2_TILED, D3D12_FORMAT_SUPPORT2_NONE,
                                      "R16G16_FLOAT");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_R32G8X24_TYPELESS,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT),
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                                          D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW),
                                      D3D12_FORMAT_SUPPORT2_NONE,
                                      static_cast<D3D12_FORMAT_SUPPORT2>(
                                          D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE),
                                      "R32G8X24_TYPELESS");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                                          D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL),
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                                          D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                                          D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                                          D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW |
                                          D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE),
                                      D3D12_FORMAT_SUPPORT2_NONE,
                                      static_cast<D3D12_FORMAT_SUPPORT2>(
                                          D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE |
                                          D3D12_FORMAT_SUPPORT2_TILED),
                                      "D32_FLOAT_S8X24_UINT");

    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD),
                                      D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL,
                                      D3D12_FORMAT_SUPPORT2_NONE, D3D12_FORMAT_SUPPORT2_NONE,
                                      "R32_FLOAT_X8X24_TYPELESS");
    contractFailures += requireFormat(device.Get(), DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,
                                      static_cast<D3D12_FORMAT_SUPPORT1>(
                                          D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD),
                                      D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL,
                                      D3D12_FORMAT_SUPPORT2_NONE, D3D12_FORMAT_SUPPORT2_NONE,
                                      "X32_TYPELESS_G8X24_UINT");


    D3D12_FEATURE_DATA_FORMAT_SUPPORT invalid{};
    invalid.Format = static_cast<DXGI_FORMAT>(0x7fffffff);
    invalid.Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(0xffffffffu);
    invalid.Support2 = static_cast<D3D12_FORMAT_SUPPORT2>(0xffffffffu);
    hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &invalid, sizeof(invalid));
    std::printf("OWNED_CAPABILITIES_FORMAT name=INVALID format=%u hr=0x%08lx support1=0x%08x support2=0x%08x\n",
                static_cast<unsigned>(invalid.Format), static_cast<unsigned long>(hr),
                static_cast<unsigned>(invalid.Support1), static_cast<unsigned>(invalid.Support2));
    if (SUCCEEDED(hr) || invalid.Support1 != D3D12_FORMAT_SUPPORT1_NONE ||
        invalid.Support2 != D3D12_FORMAT_SUPPORT2_NONE)
        contractFailures += fail("invalid format did not fail with zero masks", hr);

    D3D12_FEATURE_DATA_FORMAT_SUPPORT bc1{};
    bc1.Format = DXGI_FORMAT_BC1_UNORM;
    hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &bc1, sizeof(bc1));
    if (FAILED(hr)) {
        contractFailures += fail("BC1_UNORM query", hr);
    } else if (isAllOnes(bc1) || has1(bc1, D3D12_FORMAT_SUPPORT1_BUFFER) ||
               has1(bc1, D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ||
               has1(bc1, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
               has2(bc1, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) ||
               has2(bc1, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        contractFailures += fail("BC1_UNORM advertised unsupported write/buffer capability");
    }
    std::printf("OWNED_CAPABILITIES_FORMAT name=BC1_UNORM format=%u hr=0x%08lx support1=0x%08x support2=0x%08x runtime_bc_support=%d\n",
                static_cast<unsigned>(bc1.Format), static_cast<unsigned long>(hr),
                static_cast<unsigned>(bc1.Support1), static_cast<unsigned>(bc1.Support2),
                has1(bc1, D3D12_FORMAT_SUPPORT1_TEXTURE2D) ? 1 : 0);

    if (contractFailures) {
        std::printf("OWNED_CAPABILITIES_FAIL_SUMMARY count=%d\n", contractFailures);
        return 1;
    }

    std::puts("OWNED_CAPABILITIES_PASS");
    return 0;
}

} // namespace

int wmain() {
    return run();
}
