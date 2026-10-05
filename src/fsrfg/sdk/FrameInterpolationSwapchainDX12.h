// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <comdef.h>
#include <synchapi.h>

#include "FrameInterpolationSwapchainDX12_Helpers.h"

#include "sdk_backend.hpp"
#include "../../d3d12/d3d12_frame_generation_interfaces.hpp"




#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_MAJOR    3
#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_MINOR    1
#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_PATCH    6
#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT_SDK1 6
#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT_SDK2 DXGI_MAX_SWAP_CHAIN_BUFFERS
#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_INTERPOLATION_OUTPUTS_COUNT 8

typedef enum FrameType
{
    Interpolated_1,
    Interpolated_2,
    Interpolated_3,
    Interpolated_4,
    Real,
    Count
} FrameType;

// No modifications to this type are permitted to maintain ABI stability
template<typename ResourceType>
struct TFrameInterpolationFrameInfo
{
    bool            doPresent;
    ResourceType    resource;
    UINT64          interpolationCompletedFenceValue;
    UINT64          presentIndex;
    UINT64          presentQpcDelta;
};

// Inherits from TFrameInterpolationFrameInfo but can be extended without becoming part of the ABI
struct FrameInterpolationFrameInfoExt : public TFrameInterpolationFrameInfo<FfxApiResource>
{
};

// Owned scheduling layout, never exposed through the public FFX ABI.
template<typename ResourceType, typename TFrameInfo>
struct TFrameInterpolationPacingData
{
    typedef TFrameInfo FrameInfo;

    FfxApiPresentCallbackFunc       presentCallback = nullptr;
    void*                           presentCallbackContext = nullptr;
    ResourceType                    uiSurface;

    bool                            vsync;
    bool                            tearingSupported;
    bool                            usePremulAlphaComposite;
    bool                            drawDebugPacingLines;

    UINT64                          interpolationCompletedFenceValue;
    UINT64                          replacementBufferFenceSignal;
    UINT64                          numFramesSentForPresentationBase;
    UINT32                          numFramesToPresent;
    UINT64                          currentFrameID;

    FrameInfo frames[FrameType::Count];

    void invalidate()
    {
        memset(this, 0, sizeof(TFrameInterpolationPacingData));
    }
};

// Inherits from TFrameInterpolationPacingData but can be extended without becoming part of the ABI
struct FrameInterpolationPacingDataExt : public TFrameInterpolationPacingData<FfxApiResource, FrameInterpolationFrameInfoExt>
{
    IMTLD3D12FrameGenerationState* generationState = nullptr;
    UINT requestedCount = 0;
    bool onlyGenerated = false;
    void releaseResources() {
        for (auto& frame : frames) {
            if (frame.resource.resource) static_cast<IUnknown*>(frame.resource.resource)->Release();
            frame.resource = {};
        }
        if (uiSurface.resource) static_cast<IUnknown*>(uiSurface.resource)->Release();
        uiSurface = {};
        SafeRelease(generationState);
    }
    void invalidate() { memset(this, 0, sizeof(*this)); }
};

// No modifications to this type are permitted to maintain ABI stability
typedef TFrameInterpolationPacingData<FfxApiResource, TFrameInterpolationFrameInfo<FfxApiResource>> FrameInterpolationPacingDataSDK2;

// No modifications to this type are permitted to maintain ABI stability
template <size_t PoolSize, typename ResourceType, typename PacingType>
struct TFrameInterpolationPresentInfo
{
    typedef PacingType PacingData;

    CRITICAL_SECTION    criticalSectionScheduledFrame;
    ID3D12Device*       device    = nullptr;
    IDXGISwapChain4*    swapChain = nullptr;
    Dx12CommandPool<PoolSize>  commandPool;

    PacingData          scheduledInterpolations;
    PacingData          scheduledPresents;

    ResourceType        currentUiSurface;
    uint32_t            uiCompositionFlags = 0;

    ID3D12CommandQueue* interpolationQueue      = nullptr;
    ID3D12CommandQueue* asyncComputeQueue       = nullptr;
    ID3D12CommandQueue* gameQueue               = nullptr;
    ID3D12CommandQueue* presentQueue            = nullptr;

    ID3D12Fence*        gameFence               = nullptr;
    ID3D12Fence*        interpolationFence      = nullptr;
    ID3D12Fence*        presentFence            = nullptr;
    ID3D12Fence*        replacementBufferFence  = nullptr;
    ID3D12Fence*        compositionFenceCPU     = nullptr;
    ID3D12Fence*        compositionFenceGPU     = nullptr;

    HANDLE              presentEvent            = 0;
    HANDLE              interpolationEvent      = 0;
    HANDLE              pacerEvent              = 0;

    volatile bool       resetTimer              = false;
    volatile bool       shutdown                = false;

    volatile double     safetyMarginInSec       = 0.0001; //0.1ms
    volatile double     varianceFactor          = 0.1;
    volatile bool       allowHybridSpin         = false;
    volatile uint32_t   hybridSpinTime          = 2; //Measured in system timer resolution units. Default is 2. Below 1 will frequently result in overshoot. Overshoots stop showing up >=2.
    volatile bool       allowWaitForSingleObjectOnFence = false;

    FfxWaitCallbackFunc waitCallback            = nullptr;

    volatile int64_t    previousPresentQpc      = 0;
};

// No modifications to this type are permitted to maintain ABI stability

// This type can be extended without breaking the ABI
struct FrameinterpolationPresentInfoExt : public TFrameInterpolationPresentInfo<32, FfxApiResource, FrameInterpolationPacingDataExt>
{
    volatile LONG presentationError = S_OK;
    IMTLD3D12FrameGenerationState* generationState = nullptr;
    IUnknown* uiOwner = nullptr;
    ID3D12CommandQueue* bufferPresentQueues[DXGI_MAX_SWAP_CHAIN_BUFFERS] = {};
    UINT bufferPresentQueueCount = 0;
    // Store before enqueuing Signal so a drained mode/queue handoff sees this serial.
    UINT64 lastQueuedPresent = 0;

    ID3D12CommandQueue* currentPresentQueue()
    {
        return bufferPresentQueueCount ? bufferPresentQueues[swapChain->GetCurrentBackBufferIndex()] : presentQueue;
    }
};

typedef struct ReplacementResource
{
    ID3D12Resource* resource                = nullptr;
    UINT64          availabilityFenceValue  = 0;

    void destroy()
    {
        SafeRelease(resource);
        availabilityFenceValue = 0;
    }
} ReplacementResource;

// {548CA0F7-DD5A-4CBC-BE21-7B0415E34907}
static const GUID IID_IFfxFrameInterpolationSwapChainResourceInfo = {0x548ca0f7, 0xdd5a, 0x4cbc, {0xbe, 0x21, 0x7b, 0x4, 0x15, 0xe3, 0x49, 0x7}};
// Owned private interface, never a foreign SDK C++ vtable.
static const GUID IID_IFrameInterpolationSwapChainDX12 = {0xc72e27a2, 0x8adc, 0x48df, {0xb6, 0xf8, 0xc2, 0x8d, 0xc9, 0xc8, 0xf4, 0x76}};

typedef struct FfxFrameInterpolationSwapChainResourceInfo
{
    int  version;
    bool isInterpolated;
} FfxFrameInterpolationSwapChainResourceInfo;

// Private owned C++ interface; all callers are compiled together. IDXGISwapChain4
// retains its public COM ABI; foreign SDK private vtables are never accepted.
class IFrameInterpolationSwapChainDX12 : public IDXGISwapChain4
{
public:
    IFrameInterpolationSwapChainDX12() {}
    

    virtual void presentPassthrough(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT presentWithUiComposition(UINT SyncInterval, UINT Flags) = 0;

    virtual void dispatchInterpolationCommands(FrameInterpolationPacingDataExt& entry) = 0;
    virtual void presentInterpolated(UINT SyncInterval, UINT Flags) = 0;

    virtual bool verifyUiDuplicateResource() = 0;
    virtual void copyUiResource() = 0;

    virtual bool verifyBackbufferDuplicateResources() = 0;
    virtual bool destroyReplacementResources() = 0;
    virtual bool killPresenterThread() = 0;
    virtual bool spawnPresenterThread() = 0;
    virtual void discardOutstandingInterpolationCommandLists() = 0;

    virtual IDXGISwapChain4* real() = 0;

    virtual void setFrameGenerationConfig(FfxFrameGenerationConfig const* config) = 0;
    virtual void setFrameGenerationState(IMTLD3D12FrameGenerationState* state) = 0;
    // True only for dispatches captured by an active or registered proxy list.
    virtual bool recordFrameGeneration(const ffxDispatchDescFrameGeneration& desc) = 0;
    virtual bool waitForPresents(bool consumeError = true) = 0;

    virtual FfxApiResource interpolationOutput(int index = 0) = 0;
    virtual ID3D12GraphicsCommandList* getInterpolationCommandList() = 0;

    virtual void registerUiResource(FfxApiResource uiResource, uint32_t flags) = 0;
    virtual void setWaitCallback(FfxWaitCallbackFunc waitCallbackFunc) = 0;
    virtual void setFramePacingTuning(const FfxApiSwapchainFramePacingTuning * framePacingTuning) = 0;

    virtual void GetGpuMemoryUsage(FfxApiEffectMemoryUsage * vramUsage) = 0;

    virtual HRESULT init(
        HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1 * swapChainDesc1,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC * fullscreenDesc,
        ID3D12CommandQueue * queue,
        IDXGIFactory2 * dxgiFactory) = 0;

protected:
    virtual HRESULT               shutdown() = 0;
    virtual UINT                  getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags) = 0;
    virtual DXGI_SWAP_CHAIN_DESC1 getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1 * nonAdjustedDesc) = 0;
};

// Owned private template/vtable: all callers are compiled together. Foreign
// SDK private ABI layouts are deliberately not accepted by the provider.
template<typename ParentType, typename PresentType, typename ConfigType, typename PresentCallbackType, size_t NumBuffers>
class TFrameInterpolationSwapChainDX12 : public ParentType
{
#if FFX_ABI_TEST
    friend struct FrameInterpolationSwapChainDX12ABITester;
#endif
protected:
    HRESULT               shutdown();
    UINT                  getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags);
    DXGI_SWAP_CHAIN_DESC1 getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc);

    PresentType         presentInfo = {};
    ConfigType          nextFrameGenerationConfig;

    CRITICAL_SECTION    criticalSection{};
    CRITICAL_SECTION    criticalSectionUpdateConfig{};

    UINT                gameBufferCount            = 0;
    UINT                gameFlags                  = 0;
    DXGI_SWAP_EFFECT    gameSwapEffect             = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT                gameMaximumFrameLatency    = 1;  // default is 1
    ID3D12Resource*     realBackBuffer0            = nullptr;  //hold reference to avoid DXGI deadlock on Windows 10

    HANDLE              replacementFrameLatencyWaitableObjectHandle  = 0;

    UINT64              interpolationFenceValue              = 0;
    UINT64              gameFenceValue                       = 0;
    bool                frameInterpolationResetCondition     = false;
    FfxApiRect2D        interpolationRect;

    Dx12Commands*       registeredInterpolationCommandLists[NumBuffers] = {};
    FrameInterpolationPacingDataExt registeredGenerations[NumBuffers] = {};
    ID3D12GraphicsCommandList* activeGenerationList = nullptr;
    FrameInterpolationPacingDataExt* activeGeneration = nullptr;
    ReplacementResource replacementSwapBuffers[NumBuffers]  = {};
    ReplacementResource interpolationOutputs[FFX_FRAME_INTERPOLATION_SWAP_CHAIN_INTERPOLATION_OUTPUTS_COUNT] = {};
    ReplacementResource uiReplacementBuffer                                                          = {};
    int                 replacementSwapBufferIndex                                                   = 0;
    int                 interpolationBufferIndex                                                     = 0;
    UINT64              presentCount                                                                 = 0;

    FfxFrameInterpolationDispatchFlags configFlags             = {};

    bool                tearingSupported                = false;
    bool                interpolationEnabled            = false;
    bool                presentInterpolatedOnly         = false;
    bool                previousFrameWasInterpolated    = false;
    bool                drawDebugPacingLines            = false;

    UINT64              currentFrameID              = 0;

    UINT64              framesSentForPresentation   = 0;
    UINT64              nextPresentWaitValue        = 0;
    HANDLE              interpolationThreadHandle   = 0;
    ULONG               refCount                    = 1;

    UINT                backBufferTransferFunction      = 0;
    float               minLuminance                    = 0.0f;
    float               maxLuminance                    = 0.0f;

    FfxApiPresentCallbackFunc           presentCallback                 = nullptr;
    void*                               presentCallbackContext          = nullptr;
    FfxApiFrameGenerationDispatchFunc   frameGenerationCallback         = nullptr;
    void*                               frameGenerationCallbackContext  = nullptr;

    UINT64 totalUsageInBytes = 0;
    UINT64 aliasableUsageInBytes = 0;

    void presentPassthrough(UINT SyncInterval, UINT Flags);
    HRESULT presentWithUiComposition(UINT SyncInterval, UINT Flags);

    void dispatchInterpolationCommands(FrameInterpolationPacingDataExt& entry);
    void presentInterpolated(UINT SyncInterval, UINT Flags);

    bool verifyUiDuplicateResource();
    void copyUiResource();

    bool verifyBackbufferDuplicateResources();
    bool destroyReplacementResources();
    HRESULT resizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT Flags,
                          const UINT* nodeMasks, IUnknown* const* queues);
    bool killPresenterThread();
    bool spawnPresenterThread();
    void discardOutstandingInterpolationCommandLists();

    IDXGISwapChain4* real();

public:
    void setFrameGenerationConfig(FfxFrameGenerationConfig const* config);
    void setFrameGenerationState(IMTLD3D12FrameGenerationState* state);
    bool recordFrameGeneration(const ffxDispatchDescFrameGeneration& desc);
    bool waitForPresents(bool consumeError = true);

    FfxApiResource interpolationOutput(int index = 0);
    ID3D12GraphicsCommandList* getInterpolationCommandList();

    void registerUiResource(FfxApiResource uiResource, uint32_t flags);
    void setWaitCallback(FfxWaitCallbackFunc waitCallbackFunc);
    void setFramePacingTuning(const FfxApiSwapchainFramePacingTuning* framePacingTuning);

    void GetGpuMemoryUsage(FfxApiEffectMemoryUsage * vramUsage);

    TFrameInterpolationSwapChainDX12();
    virtual ~TFrameInterpolationSwapChainDX12();

    HRESULT init(
        HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1* swapChainDesc1,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
        ID3D12CommandQueue* queue,
        IDXGIFactory2* dxgiFactory);

    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    // IDXGIObject
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData);
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown * pUnknown);
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT * pDataSize, void* pData);
    virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent);

    // IDXGIDeviceSubObject
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice);

    // IDXGISwapChain1
    virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface);
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput * pTarget);
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL * pFullscreen, IDXGIOutput * *ppTarget);
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC * pDesc);
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC * pNewTargetParameters);
    virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput * *ppOutput);
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS * pStats);
    virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT * pLastPresentCount);

    // IDXGISwapChain1
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 * pDesc);
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC * pDesc);
    virtual HRESULT STDMETHODCALLTYPE GetHwnd(HWND * pHwnd);
    virtual HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk);
    virtual HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS * pPresentParameters);
    virtual BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void);
    virtual HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput * *ppRestrictToOutput);
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA * pColor);
    virtual HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA * pColor);
    virtual HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation);
    virtual HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION * pRotation);

    // IDXGISwapChain2
    virtual HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height);
    virtual HRESULT STDMETHODCALLTYPE GetSourceSize(UINT * pWidth, UINT * pHeight);
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency);
    virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT * pMaxLatency);
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void);
    virtual HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F * pMatrix);
    virtual HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F * pMatrix);

    // IDXGISwapChain3
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void);
    virtual HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT * pColorSpaceSupport);
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace);
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT * pCreationNodeMask, IUnknown* const* ppPresentQueue);

    // IDXGISwapChain4
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData);
};

