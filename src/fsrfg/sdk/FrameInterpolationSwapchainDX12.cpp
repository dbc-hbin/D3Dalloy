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

#include "com/com_pointer.hpp"
#include <tuple>
#include <initguid.h>
#include "FrameInterpolationSwapchainDX12.h"

#include "sdk_backend.hpp"
#include "FrameInterpolationSwapchainDX12_UiComposition.h"
#include "FrameInterpolationSwapchainDX12_DebugPacing.h"


#include <timeapi.h>
#include <cmath>
#pragma comment(lib, "winmm.lib")




void InitConfigType(FfxFrameGenerationConfig& config)
{
    config = {};
    config.header.pNext = nullptr;
    config.header.type = FFX_API_FRAME_GENERATION_CONFIG;
}

void InitConfigType(ffxCallbackDescFrameGenerationPresent& config)
{
    config = {};
    config.header.pNext = nullptr;
    config.header.type = FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT;
}

void InitPresentCallback(ffxCallbackDescFrameGenerationPresent& desc, ffxCallbackDescFrameGenerationPresentPremulAlpha& extra, FfxDevice device, FfxCommandList cmdList, bool isInterpolated, FfxApiResource swapChainBuffer, FfxApiResource backBuffer, FfxApiResource currentUI, bool usePremulAlpha, uint64_t frameID)
{
    InitConfigType(desc);
    desc.commandList = cmdList;
    desc.device = device;
    desc.isGeneratedFrame = isInterpolated;
    desc.outputSwapChainBuffer = swapChainBuffer;
    desc.currentBackBuffer = backBuffer;
    desc.currentUI = currentUI;
    desc.frameID = frameID;

    extra.usePremulAlpha = usePremulAlpha;
    extra.header.pNext = nullptr;
    extra.header.type = FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA;

    desc.header.pNext = &extra.header;
}

static dxmt::Com<IFrameInterpolationSwapChainDX12> getOwnedSwapchain(FfxSwapchain gameSwapChain)
{
    dxmt::Com<IFrameInterpolationSwapChainDX12> owned;
    auto swapChain = ffxGetDX12SwapchainPtr(gameSwapChain);
    if (swapChain && SUCCEEDED(swapChain->QueryInterface(IID_IFrameInterpolationSwapChainDX12, reinterpret_cast<void**>(&owned))))
        return owned;
    return nullptr;
}

FfxApiResource ffxGetNamedResourceDX12(const ID3D12Resource* dx12Resource,
    FfxApiResourceDescription ffxResDescription,
    const wchar_t* ffxResName,
    uint32_t state /*=FFX_API_RESOURCE_STATE_COMPUTE_READ*/)
{
    FfxApiResource resource = {};
    resource.resource    = reinterpret_cast<void*>(const_cast<ID3D12Resource*>(dx12Resource));
    resource.state = state;
    resource.description = ffxResDescription;

    (void)ffxResName;

    return resource;
}

FfxErrorCode ffxRegisterFrameinterpolationUiResourceDX12(FfxSwapchain gameSwapChain, FfxApiResource uiResource, uint32_t flags)
{
    auto swapChain = getOwnedSwapchain(gameSwapChain);
    if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
    swapChain->registerUiResource(uiResource, flags);
    return FFX_OK;
}

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainDX12(FfxFrameGenerationConfig const* config)
{
    auto swapChain = getOwnedSwapchain(config->swapChain);
    if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
    swapChain->setFrameGenerationConfig(config);
    return FFX_OK;
}

FfxErrorCode ffxConfigureFrameInterpolationSwapchainDX12(FfxSwapchain gameSwapChain, FfxFrameInterpolationSwapchainConfigureKey key, void* valuePtr)
{
    switch (key)
    {
        case FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK:
        {
            auto swapChain = getOwnedSwapchain(gameSwapChain);
            if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
            swapChain->setWaitCallback(reinterpret_cast<FfxWaitCallbackFunc>(valuePtr));
            return FFX_OK;
        }
        case FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING:
        {
            if (!valuePtr) return FFX_ERROR_INVALID_ARGUMENT;
            auto swapChain = getOwnedSwapchain(gameSwapChain);
            if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
            swapChain->setFramePacingTuning(static_cast<FfxApiSwapchainFramePacingTuning*>(valuePtr));
            return FFX_OK;
        }
    }
    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxApiResource ffxGetFrameinterpolationTextureDX12(FfxSwapchain gameSwapChain)
{
    auto swapChain = getOwnedSwapchain(gameSwapChain);
    return swapChain ? swapChain->interpolationOutput(0) : FfxApiResource{ nullptr };
}

FfxErrorCode ffxGetFrameinterpolationCommandlistDX12(FfxSwapchain gameSwapChain, FfxCommandList& gameCommandlist)
{
    auto swapChain = getOwnedSwapchain(gameSwapChain);
    if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
    gameCommandlist = swapChain->getInterpolationCommandList();
    return FFX_OK;
}

FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12(FfxSwapchain gameSwapChain, FfxApiEffectMemoryUsage* vramUsage)
{
    auto swapChain = getOwnedSwapchain(gameSwapChain);
    if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
    swapChain->GetGpuMemoryUsage(vramUsage);
    return FFX_OK;
}

FfxErrorCode ffxReplaceSwapchainForFrameinterpolationDX12(FfxCommandQueue gameQueue, FfxSwapchain& gameSwapChain)
{
    FfxErrorCode     status            = FFX_ERROR_INVALID_ARGUMENT;
    IDXGISwapChain4* dxgiGameSwapChain = reinterpret_cast<IDXGISwapChain4*>(gameSwapChain);
    FFX_ASSERT(dxgiGameSwapChain);

    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(gameQueue);
    FFX_ASSERT(queue);

    // we just need the desc, release the real swapchain as we'll replace that with one doing frameinterpolation
    HWND                            hWnd;
    DXGI_SWAP_CHAIN_DESC1           desc1;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc;
    if (SUCCEEDED(dxgiGameSwapChain->GetDesc1(&desc1)) &&
        SUCCEEDED(dxgiGameSwapChain->GetFullscreenDesc(&fullscreenDesc)) &&
        SUCCEEDED(dxgiGameSwapChain->GetHwnd(&hWnd))
        )
    {
        FFX_ASSERT_MESSAGE(fullscreenDesc.Windowed == TRUE, "Illegal to release a fullscreen swap chain.");

        IDXGIFactory* dxgiFactory = getDXGIFactoryFromSwapChain(dxgiGameSwapChain);
        if (!dxgiFactory) return FFX_ERROR_INVALID_ARGUMENT;
        SafeRelease(dxgiGameSwapChain);
        gameSwapChain = nullptr;

        FfxSwapchain proxySwapChain;
        status = ffxCreateFrameinterpolationSwapchainForHwndDX12(hWnd, &desc1, &fullscreenDesc, queue, dxgiFactory, proxySwapChain);
        if (status == FFX_OK)
        {
            gameSwapChain = proxySwapChain;
        }

        SafeRelease(dxgiFactory);
    }

    return status;
}

FfxErrorCode ffxCreateFrameinterpolationSwapchainDX12(const DXGI_SWAP_CHAIN_DESC*   desc,
                                                      ID3D12CommandQueue*           queue,
                                                      IDXGIFactory*                 dxgiFactory,
                                                      FfxSwapchain&                 outGameSwapChain)
{
    FFX_ASSERT(desc);
    FFX_ASSERT(queue);
    FFX_ASSERT(dxgiFactory);

    DXGI_SWAP_CHAIN_DESC1 desc1{};
    desc1.Width         = desc->BufferDesc.Width;
    desc1.Height        = desc->BufferDesc.Height;
    desc1.Format        = desc->BufferDesc.Format;
    desc1.SampleDesc    = desc->SampleDesc;
    desc1.BufferUsage   = desc->BufferUsage;
    desc1.BufferCount   = desc->BufferCount;
    desc1.SwapEffect    = desc->SwapEffect;
    desc1.Flags         = desc->Flags;

    // for clarity, params not part of DXGI_SWAP_CHAIN_DESC
    // implicit behavior of DXGI when you call the IDXGIFactory::CreateSwapChain
    desc1.Scaling       = DXGI_SCALING_STRETCH;
    desc1.AlphaMode     = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Stereo        = FALSE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    fullscreenDesc.Scaling          = desc->BufferDesc.Scaling;
    fullscreenDesc.RefreshRate      = desc->BufferDesc.RefreshRate;
    fullscreenDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreenDesc.Windowed         = desc->Windowed;

    return ffxCreateFrameinterpolationSwapchainForHwndDX12(desc->OutputWindow, &desc1, &fullscreenDesc, queue, dxgiFactory, outGameSwapChain);
}

FfxErrorCode ffxCreateFrameinterpolationSwapchainForHwndDX12(HWND                                   hWnd,
                                                             const DXGI_SWAP_CHAIN_DESC1*           desc1,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                             ID3D12CommandQueue*                    queue,
                                                             IDXGIFactory*                          dxgiFactory,
                                                             FfxSwapchain&                          outGameSwapChain)
{
    // don't assert fullscreenDesc, nullptr valid
    FFX_ASSERT(hWnd != 0);
    FFX_ASSERT(desc1);
    FFX_ASSERT(queue);
    FFX_ASSERT(dxgiFactory);

    FfxErrorCode err = FFX_ERROR_INVALID_ARGUMENT;

    IDXGIFactory2* dxgiFactory2 = nullptr;
    if (SUCCEEDED(dxgiFactory->QueryInterface(IID_PPV_ARGS(&dxgiFactory2))))
    {
        // Create proxy swapchain
        FrameInterpolationSwapChainDX12* fiSwapchain = new (std::nothrow) FrameInterpolationSwapChainDX12();
        if (fiSwapchain)
        {
            HRESULT hr = fiSwapchain->init(hWnd, desc1, fullscreenDesc, queue, dxgiFactory2);
            if (SUCCEEDED(hr))
            {
                outGameSwapChain = ffxGetSwapchainDX12(fiSwapchain);

                err = FFX_OK;
            }
            else
            {
                delete fiSwapchain;
                if (hr == E_ACCESSDENIED)
                    err = FFX_ERROR_ACCESS_DENIED;
                else
                    err = FFX_ERROR_INVALID_ARGUMENT;
            }
        }
        else
        {
            err = FFX_ERROR_OUT_OF_MEMORY;
        }

        SafeRelease(dxgiFactory2);
    }

    return err;
}

FfxErrorCode ffxWaitForPresents(FfxSwapchain gameSwapChain)
{
    auto swapChain = getOwnedSwapchain(gameSwapChain);
    if (!swapChain) return FFX_ERROR_INVALID_ARGUMENT;
    return swapChain->waitForPresents() ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;
}

using PresentType = FrameinterpolationPresentInfoExt;
using PresentCallbackType = ffxCallbackDescFrameGenerationPresent;

void setSwapChainBufferResourceInfo(IDXGISwapChain4* swapChain, bool isInterpolated)
{
    uint32_t        currBackbufferIndex = swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* swapchainBackbuffer = nullptr;

    if (SUCCEEDED(swapChain->GetBuffer(currBackbufferIndex, IID_PPV_ARGS(&swapchainBackbuffer))))
    {
        FfxFrameInterpolationSwapChainResourceInfo info{};
        info.version = FFX_SDK_MAKE_VERSION(FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_MAJOR,
                                            FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_MINOR,
                                            FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_PATCH);
        info.isInterpolated = isInterpolated;
        HRESULT hr = swapchainBackbuffer->SetPrivateData(IID_IFfxFrameInterpolationSwapChainResourceInfo, sizeof(info), &info);
        FFX_ASSERT(SUCCEEDED(hr));

        /*
        usage example:
        
        FfxFrameInterpolationSwapChainResourceInfo info{};
        UINT size = sizeof(info);
        if (SUCCEEDED(swapchainBackbuffer->GetPrivateData(IID_IFfxFrameInterpolationSwapChainResourceInfo, &size, &info))) {
            
        } else {
            // buffer was not presented using proxy swapchain
        }
        */

        SafeRelease(swapchainBackbuffer);
    }
}

template<typename PresentType, typename PacingData, typename PresentCallbackType>
HRESULT compositeSwapChainFrame(PresentType* presenter, PacingData* pacingEntry, uint32_t frameID)
{

    const typename PacingData::FrameInfo& frameInfo = pacingEntry->frames[frameID];

    if(frameID != FrameType::Real)
        presenter->presentQueue->Wait(presenter->interpolationFence, frameInfo.interpolationCompletedFenceValue);

    if (pacingEntry->drawDebugPacingLines)
    {
        auto gpuCommands = presenter->commandPool.get(presenter->presentQueue, L"compositeSwapChainFrame");

        uint32_t        currBackbufferIndex = presenter->swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* swapchainBackbuffer = nullptr;
        presenter->swapChain->GetBuffer(currBackbufferIndex, IID_PPV_ARGS(&swapchainBackbuffer));

        // This always calls the *current* version of the code so must use the *current* type
        ffxCallbackDescFrameGenerationPresent desc;
        ffxCallbackDescFrameGenerationPresentPremulAlpha premulAlpha;
        InitPresentCallback(desc, premulAlpha, presenter->device, ffxGetCommandListDX12(gpuCommands->reset()), frameID != FrameType::Real, ffxGetNamedResourceDX12(swapchainBackbuffer, ffxGetResourceDescriptionDX12(swapchainBackbuffer), nullptr, FFX_API_RESOURCE_STATE_PRESENT), frameInfo.resource, pacingEntry->uiSurface, pacingEntry->usePremulAlphaComposite, pacingEntry->currentFrameID);

        ffxFrameInterpolationDebugPacing(&desc);

        gpuCommands->execute(true);

        SafeRelease(swapchainBackbuffer);
    }

    FFX_ASSERT(pacingEntry->presentCallback != nullptr);

    auto gpuCommands = presenter->commandPool.get(presenter->presentQueue, L"compositeSwapChainFrame");

    uint32_t        currBackbufferIndex = presenter->swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* swapchainBackbuffer = nullptr;
    presenter->swapChain->GetBuffer(currBackbufferIndex, IID_PPV_ARGS(&swapchainBackbuffer));

    // This may call into code within the game SDK which may be older so must use the template type
    PresentCallbackType desc;
    ffxCallbackDescFrameGenerationPresentPremulAlpha premulAlpha;
    InitPresentCallback(desc, premulAlpha, presenter->device, ffxGetCommandListDX12(gpuCommands->reset()), frameID != FrameType::Real, ffxGetNamedResourceDX12(swapchainBackbuffer, ffxGetResourceDescriptionDX12(swapchainBackbuffer), nullptr, FFX_API_RESOURCE_STATE_PRESENT), frameInfo.resource, pacingEntry->uiSurface, pacingEntry->usePremulAlphaComposite, pacingEntry->currentFrameID);

    pacingEntry->presentCallback(reinterpret_cast<ffxCallbackDescFrameGenerationPresent*> (&desc), pacingEntry->presentCallbackContext);

    gpuCommands->execute(true);

    SafeRelease(swapchainBackbuffer);

    presenter->presentQueue->Signal(presenter->compositionFenceGPU, frameInfo.presentIndex);
    presenter->compositionFenceCPU->Signal(frameInfo.presentIndex);

    return S_OK;
}

template<typename PresentType, typename PacingData>
void presentToSwapChain(PresentType* presenter, PacingData* pacingEntry, FrameType frameType)
{
    const typename PacingData::FrameInfo& frameInfo = pacingEntry->frames[frameType];

    const UINT uSyncInterval            = pacingEntry->vsync ? 1 : 0;
    const bool bExclusiveFullscreen     = isExclusiveFullscreen(presenter->swapChain);
    const bool bSetAllowTearingFlag     = pacingEntry->tearingSupported && !bExclusiveFullscreen && (0 == uSyncInterval);
    const UINT uFlags                   = bSetAllowTearingFlag * DXGI_PRESENT_ALLOW_TEARING;

    // AntiLag2 AMD driver transport is not part of the host adapter.
    presenter->swapChain->Present(uSyncInterval, uFlags);

    // tick frames sent for presentation
    presenter->presentQueue->Signal(presenter->presentFence, frameInfo.presentIndex);

}

template<typename PresentType, typename PacingData, typename PresentCallbackType>
DWORD WINAPI presenterThread(LPVOID param)
{
    PresentType* presenter = static_cast<PresentType*>(param);

    if (presenter)
    {
        UINT64 numFramesSentForPresentation = 0;
        int64_t qpcFrequency                 = 0;

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFrequency = freq.QuadPart;

        TIMECAPS timerCaps;
        timerCaps.wPeriodMin = UNKNOWN_TIMER_RESOlUTION; //Default to unknown to prevent sleep without guarantees.

        presenter->previousPresentQpc = 0;

        while (!presenter->shutdown)
        {

            WaitForSingleObject(presenter->pacerEvent, INFINITE);

            if (!presenter->shutdown)
            {
                EnterCriticalSection(&presenter->criticalSectionScheduledFrame);

                PacingData entry = presenter->scheduledPresents;
                presenter->scheduledPresents.invalidate();

                LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

                if (entry.numFramesToPresent > 0)
                {
                    // we might have dropped entries so have to update here, oterwise we might deadlock
                    presenter->presentQueue->Signal(presenter->presentFence, entry.numFramesSentForPresentationBase);
                    presenter->presentQueue->Wait(presenter->interpolationFence, entry.interpolationCompletedFenceValue);

                    for (uint32_t frameType = 0; frameType < FrameType::Count; frameType++)
                    {
                        const typename PacingData::FrameInfo& frameInfo = entry.frames[frameType];
                        if (frameInfo.doPresent)
                        {
                            compositeSwapChainFrame<PresentType, PacingData, PresentCallbackType>(presenter, &entry, frameType);

                            // signal replacement buffer availability
                            if (frameInfo.presentIndex == entry.replacementBufferFenceSignal)
                            {
                                presenter->presentQueue->Signal(presenter->replacementBufferFence, entry.replacementBufferFenceSignal);
                            }

                            
                            MMRESULT result = timeGetDevCaps(&timerCaps, sizeof(timerCaps));
                            if (result != MMSYSERR_NOERROR || !presenter->allowHybridSpin)
                            {
                                timerCaps.wPeriodMin = UNKNOWN_TIMER_RESOlUTION;
                            }
                            else
                            {
                                timerCaps.wPeriodMin = std::max(1u, timerCaps.wPeriodMin);
                            }

                            // pacing without composition
                            waitForFenceValue(presenter->compositionFenceGPU, frameInfo.presentIndex);
                            uint64_t targetQpc = presenter->previousPresentQpc + frameInfo.presentQpcDelta;
                            waitForPerformanceCount(targetQpc, qpcFrequency, timerCaps.wPeriodMin, presenter->hybridSpinTime);

                            int64_t currentPresentQPC;
                            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentPresentQPC));
                            presenter->previousPresentQpc = currentPresentQPC;

                            presentToSwapChain(presenter, &entry, (FrameType)frameType);
                        }
                    }

                    numFramesSentForPresentation = entry.numFramesSentForPresentationBase + entry.numFramesToPresent;

                }
            }
        }

        waitForFenceValue(presenter->presentFence, numFramesSentForPresentation);
    }

    return 0;
}

template<typename PresentType, typename PacingData, typename PresentCallbackType>
DWORD WINAPI interpolationThread(LPVOID param)
{
    PresentType* presenter = static_cast<PresentType*>(param);

    if (presenter)
    {
        HANDLE presenterThreadHandle = CreateThread(nullptr, 0, presenterThread<PresentType, PacingData, PresentCallbackType>, param, 0, nullptr);
        FFX_ASSERT(presenterThreadHandle != NULL);

        if (presenterThreadHandle != 0)
        {
            SetThreadPriority(presenterThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(presenterThreadHandle, L"AMD FSR Presenter Thread");

            SimpleMovingAverage<10, double> frameTime{};

            int64_t previousQpc = 0;
            int64_t previousDelta = 0;
            int64_t qpcFrequency;
            QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFrequency)); 

            while (!presenter->shutdown)
            {
                WaitForSingleObject(presenter->presentEvent, INFINITE);

                if (!presenter->shutdown)
                {
                    EnterCriticalSection(&presenter->criticalSectionScheduledFrame);

                    PacingData entry = presenter->scheduledInterpolations;
                    presenter->scheduledInterpolations.invalidate();

                    LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);
                    
                    int64_t preWaitQPC = 0;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&preWaitQPC));
                    int64_t previousPresentQPC = presenter->previousPresentQpc;
                    int64_t targetDelta = (previousPresentQPC + previousDelta) - preWaitQPC;
                    
                    //Risk of late wake if overthreading. If allowed, use WaitForSingleObject to wait for interpolationFence if the target is more than 2ms later.
                    if (previousPresentQPC && (targetDelta * 1000000) / qpcFrequency > 2000)  
                    {
                        waitForFenceValue(
                            presenter->interpolationFence, 
                            entry.frames[FrameType::Interpolated_1].interpolationCompletedFenceValue, 
                            INFINITE,
                            nullptr,
                            presenter->allowWaitForSingleObjectOnFence
                        );
                        
                    }
                    else
                    {
                        // spin to wait for interpolationFence if the target is less than 2ms.
                        waitForFenceValue(
                            presenter->interpolationFence, 
                            entry.frames[FrameType::Interpolated_1].interpolationCompletedFenceValue, 
                            INFINITE,
                            nullptr,
                            false
                        );
                    }
                    
                    SetEvent(presenter->interpolationEvent);

                    int64_t currentQpc = 0;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentQpc));

                    const double deltaQpc = double(currentQpc - previousQpc) * (previousQpc > 0);
                    previousQpc           = currentQpc;

                    // reset pacing averaging if delta > 10 fps,
                    const float fTimeoutInSeconds       = 0.1f;
                    double      deltaQpcResetThreashold = double(qpcFrequency * fTimeoutInSeconds);
                    if ((deltaQpc > deltaQpcResetThreashold) || presenter->resetTimer)
                    {
                        frameTime.reset();
                    }
                    else
                    {
                        frameTime.update(deltaQpc);
                    }

                    // set presentation time: reduce based on variance and subract safety margin so we don't lock on a framerate lower than necessary
                    int64_t qpcSafetyMargin         = int64_t(qpcFrequency * presenter->safetyMarginInSec);
                    const int64_t conservativeAvg   = int64_t(frameTime.getAverage() * 0.5 - frameTime.getVariance() * presenter->varianceFactor);
                    const int64_t deltaToUse        = conservativeAvg > qpcSafetyMargin ? (conservativeAvg - qpcSafetyMargin) : 0;
                    entry.frames[FrameType::Interpolated_1].presentQpcDelta = deltaToUse;
                    entry.frames[FrameType::Real].presentQpcDelta           = deltaToUse;
                    previousDelta                                                       = deltaToUse;
                    
                    // schedule presents
                    EnterCriticalSection(&presenter->criticalSectionScheduledFrame);
                    presenter->scheduledPresents = entry;
                    LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);
                    SetEvent(presenter->pacerEvent);
                }
            }

            // signal event to allow thread to finish
            SetEvent(presenter->pacerEvent);
            WaitForSingleObject(presenterThreadHandle, INFINITE);
            SafeCloseHandle(presenterThreadHandle);
        }
    }

    return 0;
}

bool FrameInterpolationSwapChainDX12::verifyBackbufferDuplicateResources()
{
    HRESULT hr = S_OK;

    ID3D12Device*   device = nullptr;
    ID3D12Resource* buffer = nullptr;
    if (SUCCEEDED(real()->GetBuffer(0, IID_PPV_ARGS(&buffer))))
    {
        if (SUCCEEDED(buffer->GetDevice(IID_PPV_ARGS(&device))))
        {
            auto bufferDesc = buffer->GetDesc();
            D3D12_CLEAR_VALUE clearValue{ bufferDesc.Format, 0.f, 0.f, 0.f, 1.f };

            D3D12_HEAP_PROPERTIES heapProperties{};
            D3D12_HEAP_FLAGS      heapFlags;
            buffer->GetHeapProperties(&heapProperties, &heapFlags);

            heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
            heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

            for (size_t i = 0; i < gameBufferCount; i++)
            {
                if (replacementSwapBuffers[i].resource == nullptr)
                {
                    
                    // create game render output resource
                    if (FAILED(device->CreateCommittedResource(&heapProperties,
                                                                heapFlags,
                                                                &bufferDesc,
                                                                D3D12_RESOURCE_STATE_PRESENT,
                                                                &clearValue,
                                                                IID_PPV_ARGS(&replacementSwapBuffers[i].resource))))
                    {
                        hr |= E_FAIL;
                    }
                    else
                    {
                        uint64_t resourceSize = GetResourceGpuMemorySize(replacementSwapBuffers[i].resource);
                        totalUsageInBytes += resourceSize;
                        replacementSwapBuffers[i].resource->SetName(L"AMD FSR Replacement BackBuffer");
                    }
                }
            }

            for (size_t i = 0; i < _countof(interpolationOutputs); i++)
            {
                // create interpolation output resource
                bufferDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (interpolationOutputs[i].resource == nullptr)
                {
                    if (FAILED(device->CreateCommittedResource(
                            &heapProperties, heapFlags, &bufferDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &clearValue, IID_PPV_ARGS(&interpolationOutputs[i].resource))))
                    {
                        hr |= E_FAIL;
                    }
                    else
                    {
                        uint64_t resourceSize = GetResourceGpuMemorySize(interpolationOutputs[i].resource);
                        totalUsageInBytes += resourceSize;
                        interpolationOutputs[i].resource->SetName(L"AMD FSR Interpolation Output");
                    }
                }
            }

            SafeRelease(device);
        }

        SafeRelease(realBackBuffer0);
        realBackBuffer0 = buffer;
    }

    return SUCCEEDED(hr);
}

HRESULT FrameInterpolationSwapChainDX12::init(HWND                                  hWnd,
                                              const DXGI_SWAP_CHAIN_DESC1*          desc,
                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                              ID3D12CommandQueue* queue,
                                              IDXGIFactory2* dxgiFactory)
{
    FFX_ASSERT(desc);
    FFX_ASSERT(queue);
    FFX_ASSERT(dxgiFactory);

    // store values we modify, to return when application asks for info
    gameBufferCount    = desc->BufferCount;
    gameFlags          = desc->Flags;
    gameSwapEffect     = desc->SwapEffect;

    // set default ui composition / frame interpolation present function
    // This has to be handdled differently if compiling with a compatibility type to ensure proper offsets are provided.
    presentCallback = ffxFrameInterpolationUiComposition;

    HRESULT hr = E_FAIL;

    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&presentInfo.device))))
    {
        presentInfo.gameQueue       = queue;

        InitializeCriticalSection(&criticalSection);
        InitializeCriticalSection(&criticalSectionUpdateConfig);
        InitializeCriticalSection(&presentInfo.criticalSectionScheduledFrame);
        presentInfo.presentEvent       = CreateEvent(NULL, FALSE, FALSE, nullptr);
        presentInfo.interpolationEvent = CreateEvent(NULL, FALSE, TRUE, nullptr);
        presentInfo.pacerEvent         = CreateEvent(NULL, FALSE, FALSE, nullptr);
        tearingSupported               = isTearingSupported(dxgiFactory);

        // Create presentation queue
        D3D12_COMMAND_QUEUE_DESC presentQueueDesc = queue->GetDesc();
        presentQueueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        presentQueueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        presentQueueDesc.Priority                 = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        presentQueueDesc.NodeMask                 = 0;
        presentInfo.device->CreateCommandQueue(&presentQueueDesc, IID_PPV_ARGS(&presentInfo.presentQueue));
        presentInfo.presentQueue->SetName(L"AMD FSR PresentQueue");

        // Setup pass-through swapchain default state is disabled/passthrough
        IDXGISwapChain1* pSwapChain1 = nullptr;

        DXGI_SWAP_CHAIN_DESC1 realDesc = getInterpolationEnabledSwapChainDescription(desc);
        hr = dxgiFactory->CreateSwapChainForHwnd(presentInfo.presentQueue, hWnd, &realDesc, fullscreenDesc, nullptr, &pSwapChain1);
        if (SUCCEEDED(hr) && queue)
        {
            if (SUCCEEDED(hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&presentInfo.swapChain))))
            {

                SafeRelease(pSwapChain1);
            }
            else
            {
                FFX_ASSERT_MESSAGE(hr == S_OK, "Could not query swapchain interface. Application will crash.");
                return hr;
            }
        }
        else
        {
            FFX_ASSERT_MESSAGE(hr == S_OK, "Could not create replacement swapchain. Application will crash.");
            return hr;
        }

        // init min and lax luminance according to monitor metadata
        // in case app doesn't set it through SetHDRMetadata
        getMonitorLuminanceRange(presentInfo.swapChain, &minLuminance, &maxLuminance);

        presentInfo.device->CreateFence(gameFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.gameFence));
        presentInfo.gameFence->SetName(L"AMD FSR GameFence");

        presentInfo.device->CreateFence(interpolationFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.interpolationFence));
        presentInfo.interpolationFence->SetName(L"AMD FSR InterpolationFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.presentFence));
        presentInfo.presentFence->SetName(L"AMD FSR PresentFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.replacementBufferFence));
        presentInfo.replacementBufferFence->SetName(L"AMD FSR ReplacementBufferFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.compositionFenceGPU));
        presentInfo.compositionFenceGPU->SetName(L"AMD FSR CompositionFence GPU");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.compositionFenceCPU));
        presentInfo.compositionFenceCPU->SetName(L"AMD FSR CompositionFence CPU");

        if ((desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        {
            replacementFrameLatencyWaitableObjectHandle = CreateEvent(0, FALSE, TRUE, nullptr);
        }

        // Create interpolation queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
        queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Priority                 = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        queueDesc.NodeMask                 = 0;
        presentInfo.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&presentInfo.asyncComputeQueue));
        presentInfo.asyncComputeQueue->SetName(L"AMD FSR AsyncComputeQueue");

        // Default to dispatch interpolation workloads on the game queue
        presentInfo.interpolationQueue = presentInfo.gameQueue;
    }

    return hr;
}

FrameInterpolationSwapChainDX12::FrameInterpolationSwapChainDX12()
{
    InitConfigType(nextFrameGenerationConfig);
}

FrameInterpolationSwapChainDX12::~FrameInterpolationSwapChainDX12()
{
    shutdown();
}

UINT FrameInterpolationSwapChainDX12::getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags)
{
    UINT flags = nonAdjustedFlags;

    // The DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT flag changes the D3D runtime behavior for fences
    // We will make our own waitable object for the app to wait on, but we need to keep the flag 
    flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (tearingSupported)
    {
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    return flags;
}

DXGI_SWAP_CHAIN_DESC1 FrameInterpolationSwapChainDX12::getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc)
{
    DXGI_SWAP_CHAIN_DESC1 fiDesc = *nonAdjustedDesc;

    // adjust swap chain descriptor to fit FI requirements
    fiDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    fiDesc.BufferCount = 3;
    fiDesc.Flags = getInterpolationEnabledSwapChainFlags(fiDesc.Flags);

    return fiDesc;
}

IDXGISwapChain4* FrameInterpolationSwapChainDX12::real()
{
    return presentInfo.swapChain;
}

HRESULT FrameInterpolationSwapChainDX12::shutdown()
{
    //m_pDevice will be nullptr already shutdown
    if (presentInfo.device)
    {
        destroyReplacementResources();
        
        EnterCriticalSection(&criticalSection);
        killPresenterThread();
        releaseUiBlitGpuResources();
        LeaveCriticalSection(&criticalSection);

        SafeCloseHandle(presentInfo.presentEvent);
        SafeCloseHandle(presentInfo.interpolationEvent);
        SafeCloseHandle(presentInfo.pacerEvent);

        // if we failed initialization, we may not have an interpolation queue or fence
        if (presentInfo.interpolationQueue)
        {
            if (presentInfo.interpolationFence)
            {
                presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
                waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE, presentInfo.waitCallback);
            }
        }
        
        SafeRelease(presentInfo.asyncComputeQueue);
        SafeRelease(presentInfo.presentQueue);

        SafeRelease(presentInfo.interpolationFence);
        SafeRelease(presentInfo.presentFence);
        SafeRelease(presentInfo.replacementBufferFence);
        SafeRelease(presentInfo.compositionFenceGPU);
        SafeRelease(presentInfo.compositionFenceCPU);

        std::ignore = SafeRelease(presentInfo.swapChain);

        if (presentInfo.gameFence)
        {
            waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
        }
        SafeRelease(presentInfo.gameFence);

        DeleteCriticalSection(&criticalSection);
        DeleteCriticalSection(&criticalSectionUpdateConfig);
        DeleteCriticalSection(&presentInfo.criticalSectionScheduledFrame);

        std::ignore = SafeRelease(presentInfo.device);
    }

    return S_OK;
}

bool FrameInterpolationSwapChainDX12::killPresenterThread()
{
    if (interpolationThreadHandle != NULL)
    {
        // prepare present CPU thread for shutdown
        presentInfo.shutdown = true;

        // signal event to allow thread to finish
        SetEvent(presentInfo.presentEvent);
        WaitForSingleObject(interpolationThreadHandle, INFINITE);
        SafeCloseHandle(interpolationThreadHandle);
    }

    return interpolationThreadHandle == nullptr;
}

bool FrameInterpolationSwapChainDX12::spawnPresenterThread()
{
    if (interpolationThreadHandle == NULL)
    {
        presentInfo.shutdown      = false;
        interpolationThreadHandle = CreateThread(nullptr, 0, &interpolationThread<PresentType, typename PresentType::PacingData, PresentCallbackType>, reinterpret_cast<void*>(&presentInfo), 0, nullptr);
        
        FFX_ASSERT(interpolationThreadHandle != NULL);

        if (interpolationThreadHandle != 0)
        {
            SetThreadPriority(interpolationThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(interpolationThreadHandle, L"AMD FSR Interpolation Thread");
        }

        SetEvent(presentInfo.interpolationEvent);
    }

    return interpolationThreadHandle != NULL;
}

void FrameInterpolationSwapChainDX12::discardOutstandingInterpolationCommandLists()
{
    // drop any outstanding interpolaton command lists
    for (size_t i = 0; i < _countof(registeredInterpolationCommandLists); i++)
    {
        if (registeredInterpolationCommandLists[i] != nullptr)
        {
            registeredInterpolationCommandLists[i]->drop(true);
            registeredInterpolationCommandLists[i] = nullptr;
        }
    }
}

void FrameInterpolationSwapChainDX12::setFrameGenerationConfig(FfxFrameGenerationConfig const* config)
{
    EnterCriticalSection(&criticalSectionUpdateConfig);

    //FrameInterpolationSwapChainDX12* obj = reinterpret_cast<FrameInterpolationSwapChainDX12*>(config->swapChain);
    //CRITICAL_SECTION* pSection = (CRITICAL_SECTION*)((char*)obj + 3344);
    //EnterCriticalSection(pSection);

    FFX_ASSERT(config);

    // if config is a pointer to the internal config ::present called this function to apply the changes
    bool applyChangesNow = (config == (FfxFrameGenerationConfig*)&nextFrameGenerationConfig);

    FfxFrameGenerationConfig temp;
    if (applyChangesNow)
    {
        InitConfigType(temp);
        temp.swapChain = nextFrameGenerationConfig.swapChain;
        temp.presentCallback = nextFrameGenerationConfig.presentCallback;
        temp.presentCallbackContext = nextFrameGenerationConfig.presentCallbackContext;
        temp.frameGenerationCallback = nextFrameGenerationConfig.frameGenerationCallback;
        temp.frameGenerationCallbackContext = nextFrameGenerationConfig.frameGenerationCallbackContext;
        temp.frameGenerationEnabled = nextFrameGenerationConfig.frameGenerationEnabled;
        temp.allowAsyncWorkloads = nextFrameGenerationConfig.allowAsyncWorkloads;
        temp.allowAsyncPresent = nextFrameGenerationConfig.allowAsyncPresent;
        temp.HUDLessColor = nextFrameGenerationConfig.HUDLessColor;
        temp.flags = nextFrameGenerationConfig.flags;
        temp.onlyPresentInterpolated = nextFrameGenerationConfig.onlyPresentInterpolated;
        temp.interpolationRect = nextFrameGenerationConfig.interpolationRect;
        temp.frameID = nextFrameGenerationConfig.frameID;
        temp.drawDebugPacingLines = nextFrameGenerationConfig.drawDebugPacingLines;
        config = &temp;
    }
        
    FfxApiPresentCallbackFunc inputPresentCallback = (nullptr != config->presentCallback) ? config->presentCallback : ffxFrameInterpolationUiComposition;
    void*                  inputPresentCallbackCtx = (nullptr != config->presentCallback) ? config->presentCallbackContext : nullptr;
    ID3D12CommandQueue*    inputInterpolationQueue = config->allowAsyncWorkloads ? presentInfo.asyncComputeQueue : presentInfo.gameQueue;

    // if this is called externally just copy the new config to the internal copy to avoid potentially stalling on criticalSection
    if (!applyChangesNow)
    {  
        nextFrameGenerationConfig = *config;

        // in case of actual reconfiguration: apply the changes immediately
        if ( presentInfo.interpolationQueue != inputInterpolationQueue 
           || interpolationEnabled != config->frameGenerationEnabled 
           || presentCallback != inputPresentCallback
           || presentCallbackContext != inputPresentCallbackCtx 
           || frameGenerationCallback != config->frameGenerationCallback
           || frameGenerationCallbackContext != config->frameGenerationCallbackContext
           || drawDebugPacingLines != config->drawDebugPacingLines)
        {
            applyChangesNow = true;
        }
    }

    if (applyChangesNow)
    {
        EnterCriticalSection(&criticalSection);

        currentFrameID          = config->frameID;
        presentInterpolatedOnly = config->onlyPresentInterpolated;
        interpolationRect       = config->interpolationRect;
        drawDebugPacingLines    = config->drawDebugPacingLines;

        if (presentInfo.interpolationQueue != inputInterpolationQueue)
        {
            waitForPresents();
            discardOutstandingInterpolationCommandLists();

            // change interpolation queue and reset fence value
            presentInfo.interpolationQueue = inputInterpolationQueue;
            interpolationFenceValue        = 0;
            presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, interpolationFenceValue);
        }

        if (interpolationEnabled != config->frameGenerationEnabled || presentCallback != inputPresentCallback ||
            frameGenerationCallback != config->frameGenerationCallback || configFlags != (FfxFrameInterpolationDispatchFlags)config->flags ||
            presentCallbackContext != inputPresentCallbackCtx || frameGenerationCallbackContext != config->frameGenerationCallbackContext)
        {
            waitForPresents();
            presentCallback                = inputPresentCallback;
            presentCallbackContext         = inputPresentCallbackCtx;
            frameGenerationCallback        = config->frameGenerationCallback;
            configFlags                    = FfxFrameInterpolationDispatchFlags(config->flags);
            frameGenerationCallbackContext = config->frameGenerationCallbackContext;

            // handle interpolation mode change
            if (interpolationEnabled != config->frameGenerationEnabled)
            {
                interpolationEnabled = config->frameGenerationEnabled;
                if (interpolationEnabled)
                {
                    frameInterpolationResetCondition = true;
                    nextPresentWaitValue             = framesSentForPresentation;

                    spawnPresenterThread();
                }
                else
                {
                    killPresenterThread();
                }
            }
        }
        LeaveCriticalSection(&criticalSection);
    }

    LeaveCriticalSection(&criticalSectionUpdateConfig);
}

bool FrameInterpolationSwapChainDX12::destroyReplacementResources()
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&criticalSection);

    waitForPresents();

    const bool recreatePresenterThread = interpolationThreadHandle != nullptr;
    if (recreatePresenterThread)
    {
        killPresenterThread();
    }

    discardOutstandingInterpolationCommandLists();

    {
        for (size_t i = 0; i < _countof(replacementSwapBuffers); i++)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(replacementSwapBuffers[i].resource);
            totalUsageInBytes -= resourceSize;
            replacementSwapBuffers[i].destroy();
        }

        SafeRelease(realBackBuffer0);
        
        for (size_t i = 0; i < _countof(interpolationOutputs); i++)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(interpolationOutputs[i].resource);
            totalUsageInBytes -= resourceSize;
            interpolationOutputs[i].destroy();
        }

        if (uiReplacementBuffer.resource !=nullptr)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= resourceSize;
        }
        
        uiReplacementBuffer.destroy();
    }

    // reset counters used in buffer management
    framesSentForPresentation        = 0;
    nextPresentWaitValue             = 0;
    replacementSwapBufferIndex       = 0;
    presentCount                     = 0;
    interpolationFenceValue          = 0;
    gameFenceValue                   = 0;

    // if we didn't init correctly, some parameters may not exist
    if (presentInfo.gameFence)
    {
        presentInfo.gameFence->Signal(gameFenceValue);
    }
    
    if (presentInfo.interpolationFence)
    {
        presentInfo.interpolationFence->Signal(interpolationFenceValue);
    }
    
    if (presentInfo.presentFence)
    {
        presentInfo.presentFence->Signal(framesSentForPresentation);
    }

    if (presentInfo.replacementBufferFence)
    {
        presentInfo.replacementBufferFence->Signal(framesSentForPresentation);
    }

    if (presentInfo.compositionFenceGPU)
    {
        presentInfo.compositionFenceGPU->Signal(framesSentForPresentation);
    }

    if (presentInfo.compositionFenceCPU)
    {
        presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);
    }

    frameInterpolationResetCondition = true;

    if (recreatePresenterThread)
    {
        spawnPresenterThread();
    }

    discardOutstandingInterpolationCommandLists();

    LeaveCriticalSection(&criticalSection);

    return SUCCEEDED(hr);
}

bool FrameInterpolationSwapChainDX12::waitForPresents()
{
    // wait for interpolation to finish
    waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.presentFence, framesSentForPresentation, INFINITE, presentInfo.waitCallback);

    return true;
}

FfxApiResource FrameInterpolationSwapChainDX12::interpolationOutput(int index)
{
    index = interpolationBufferIndex;

    FfxApiResourceDescription interpolateDesc = ffxGetResourceDescriptionDX12(interpolationOutputs[index].resource);
    return ffxGetResourceDX12(interpolationOutputs[index].resource, interpolateDesc, nullptr, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

//IUnknown
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::QueryInterface(REFIID riid, void** ppvObject)
{

    const GUID guidReplacements[] = {
        IID_IUnknown,
        IID_IDXGIObject,
        IID_IDXGIDeviceSubObject,
        IID_IDXGISwapChain,
        IID_IDXGISwapChain1,
        IID_IDXGISwapChain2,
        IID_IDXGISwapChain3,
        IID_IDXGISwapChain4,
        IID_IFrameInterpolationSwapChainDX12
    };

    for (auto guid : guidReplacements)
    {
        if (IsEqualGUID(riid, guid) == TRUE)
        {
            AddRef();
            *ppvObject = this;
            return S_OK;
        }
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::AddRef()
{
    InterlockedIncrement(&refCount);

    return refCount;
}

ULONG STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::Release()
{
    ULONG ref = InterlockedDecrement(&refCount);
    if (ref != 0)
    {
        return refCount;
    }

    delete this;

    return 0;
}

// IDXGIObject
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return real()->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return real()->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return real()->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetParent(REFIID riid, void** ppParent)
{
    return real()->GetParent(riid, ppParent);
}

// IDXGIDeviceSubObject
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetDevice(REFIID riid, void** ppDevice)
{
    return real()->GetDevice(riid, ppDevice);
}

void FrameInterpolationSwapChainDX12::registerUiResource(FfxApiResource uiResource, uint32_t flags)
{
    EnterCriticalSection(&criticalSection);

    presentInfo.currentUiSurface = uiResource;
    presentInfo.uiCompositionFlags = flags;
    if (nullptr == uiResource.resource)
        presentInfo.uiCompositionFlags &= ~FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;

    LeaveCriticalSection(&criticalSection);
}

void FrameInterpolationSwapChainDX12::setWaitCallback(FfxWaitCallbackFunc waitCallbackFunc)
{
    presentInfo.waitCallback = waitCallbackFunc;
}

void FrameInterpolationSwapChainDX12::setFramePacingTuning(const FfxApiSwapchainFramePacingTuning* framePacingTuning)
{
    presentInfo.safetyMarginInSec = static_cast<double> (framePacingTuning->safetyMarginInMs) / 1000.0;
    presentInfo.varianceFactor = static_cast<double> (framePacingTuning->varianceFactor);
    presentInfo.allowHybridSpin = framePacingTuning->allowHybridSpin;
    presentInfo.hybridSpinTime = framePacingTuning->hybridSpinTime;
    presentInfo.allowWaitForSingleObjectOnFence = framePacingTuning->allowWaitForSingleObjectOnFence;
}

void FrameInterpolationSwapChainDX12::GetGpuMemoryUsage(FfxApiEffectMemoryUsage* vramUsage)
{
    vramUsage->totalUsageInBytes = totalUsageInBytes;
    vramUsage->aliasableUsageInBytes = aliasableUsageInBytes;
}

void FrameInterpolationSwapChainDX12::presentPassthrough(UINT SyncInterval, UINT Flags)
{
    ID3D12Resource* dx12SwapchainBuffer    = nullptr;
    UINT            currentBackBufferIndex = presentInfo.swapChain->GetCurrentBackBufferIndex();
    presentInfo.swapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&dx12SwapchainBuffer));

    auto passthroughList = presentInfo.commandPool.get(presentInfo.presentQueue, L"passthroughList()");
    auto list            = passthroughList->reset();

    ID3D12Resource* dx12ResourceSrc = replacementSwapBuffers[replacementSwapBufferIndex].resource;
    ID3D12Resource* dx12ResourceDst = dx12SwapchainBuffer;

    D3D12_TEXTURE_COPY_LOCATION dx12SourceLocation = {};
    dx12SourceLocation.pResource                   = dx12ResourceSrc;
    dx12SourceLocation.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dx12SourceLocation.SubresourceIndex            = 0;

    D3D12_TEXTURE_COPY_LOCATION dx12DestinationLocation = {};
    dx12DestinationLocation.pResource                   = dx12ResourceDst;
    dx12DestinationLocation.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dx12DestinationLocation.SubresourceIndex            = 0;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = dx12ResourceSrc;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

    barriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource   = dx12ResourceDst;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(_countof(barriers), barriers);

    list->CopyResource(dx12ResourceDst, dx12ResourceSrc);

    for (size_t i = 0; i < _countof(barriers); ++i)
    {
        D3D12_RESOURCE_STATES tmpStateBefore = barriers[i].Transition.StateBefore;
        barriers[i].Transition.StateBefore   = barriers[i].Transition.StateAfter;
        barriers[i].Transition.StateAfter    = tmpStateBefore;
    }

    list->ResourceBarrier(_countof(barriers), barriers);

    passthroughList->execute(true);

    presentInfo.presentQueue->Signal(presentInfo.replacementBufferFence, ++framesSentForPresentation);
    presentInfo.presentQueue->Signal(presentInfo.compositionFenceGPU, framesSentForPresentation);
    presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);

    setSwapChainBufferResourceInfo(presentInfo.swapChain, false);
    presentInfo.swapChain->Present(SyncInterval, Flags);

    presentInfo.presentQueue->Signal(presentInfo.presentFence, framesSentForPresentation);
    presentInfo.gameQueue->Wait(presentInfo.presentFence, framesSentForPresentation);

    SafeRelease(dx12SwapchainBuffer);
}

void FrameInterpolationSwapChainDX12::presentWithUiComposition(UINT SyncInterval, UINT Flags)
{
    auto uiCompositionList = presentInfo.commandPool.get(presentInfo.presentQueue, L"uiCompositionList()");
    auto list              = uiCompositionList->reset();

    ID3D12Resource* dx12SwapchainBuffer    = nullptr;
    UINT            currentBackBufferIndex = presentInfo.swapChain->GetCurrentBackBufferIndex();
    presentInfo.swapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&dx12SwapchainBuffer));

    FfxApiResourceDescription outBufferDesc = ffxGetResourceDescriptionDX12(dx12SwapchainBuffer);
    FfxApiResourceDescription inBufferDesc  = ffxGetResourceDescriptionDX12(replacementSwapBuffers[replacementSwapBufferIndex].resource);

    FfxApiResource currentUI;
    if (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)
    {
        FfxApiResourceDescription uiBufferDesc = ffxGetResourceDescriptionDX12(uiReplacementBuffer.resource);
        currentUI = ffxGetNamedResourceDX12(uiReplacementBuffer.resource, uiBufferDesc, nullptr, presentInfo.currentUiSurface.state);
    }
    else
    {
        currentUI = presentInfo.currentUiSurface;
    }

    // This may call into the game SDK so must use the template type
    PresentCallbackType desc;
    ffxCallbackDescFrameGenerationPresentPremulAlpha premulAlpha;
    InitPresentCallback(desc, premulAlpha, presentInfo.device, ffxGetCommandListDX12(list), false, ffxGetNamedResourceDX12(dx12SwapchainBuffer, outBufferDesc, nullptr, FFX_API_RESOURCE_STATE_PRESENT), ffxGetNamedResourceDX12(replacementSwapBuffers[replacementSwapBufferIndex].resource, inBufferDesc, nullptr, FFX_API_RESOURCE_STATE_PRESENT), currentUI, (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA) != 0, currentFrameID);

    presentCallback(reinterpret_cast<ffxCallbackDescFrameGenerationPresent*> (&desc), presentCallbackContext);

    uiCompositionList->execute(true);

    presentInfo.presentQueue->Signal(presentInfo.replacementBufferFence, ++framesSentForPresentation);
    presentInfo.presentQueue->Signal(presentInfo.compositionFenceGPU, framesSentForPresentation);
    presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);

    setSwapChainBufferResourceInfo(presentInfo.swapChain, false);
    presentInfo.swapChain->Present(SyncInterval, Flags);

    presentInfo.presentQueue->Signal(presentInfo.presentFence, framesSentForPresentation);
    presentInfo.gameQueue->Wait(presentInfo.presentFence, framesSentForPresentation);

    SafeRelease(dx12SwapchainBuffer);
}

void FrameInterpolationSwapChainDX12::dispatchInterpolationCommands(FfxApiResource* pInterpolatedFrame, FfxApiResource* pRealFrame)
{
    FFX_ASSERT(pInterpolatedFrame);
    FFX_ASSERT(pRealFrame);

    const UINT             currentBackBufferIndex   = GetCurrentBackBufferIndex();
    ID3D12Resource*        pCurrentBackBuffer       = replacementSwapBuffers[currentBackBufferIndex].resource;
    FfxApiResourceDescription gameFrameDesc            = ffxGetResourceDescriptionDX12(pCurrentBackBuffer);
    FfxApiResource backbuffer                          = ffxGetResourceDX12(replacementSwapBuffers[currentBackBufferIndex].resource, gameFrameDesc, nullptr, FFX_API_RESOURCE_STATE_PRESENT);

    *pRealFrame = backbuffer;

    // interpolation queue must wait for output resource to become available
    presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU, interpolationOutputs[interpolationBufferIndex].availabilityFenceValue);

    auto pRegisteredCommandList = registeredInterpolationCommandLists[currentBackBufferIndex];
    if (pRegisteredCommandList != nullptr)
    {
        pRegisteredCommandList->execute(true);

        presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);

        *pInterpolatedFrame = interpolationOutput();
        frameInterpolationResetCondition = false;
    }
    else {
        Dx12Commands* interpolationCommandList = presentInfo.commandPool.get(presentInfo.interpolationQueue, L"getInterpolationCommandList()");
        auto dx12CommandList = interpolationCommandList->reset();

        ffxDispatchDescFrameGeneration desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        desc.header.pNext = nullptr;
        desc.commandList = dx12CommandList;
        desc.outputs[0] = interpolationOutput();
        desc.presentColor = backbuffer;
        desc.reset = frameInterpolationResetCondition;
        desc.numGeneratedFrames = 1;
        desc.backbufferTransferFunction = static_cast<FfxApiBackbufferTransferFunction>(backBufferTransferFunction);
        desc.minMaxLuminance[0] = minLuminance;
        desc.minMaxLuminance[1] = maxLuminance;
        desc.generationRect  = interpolationRect;
        desc.frameID            = currentFrameID;

        if (frameGenerationCallback(&desc, frameGenerationCallbackContext) == FFX_OK)
        {
            interpolationCommandList->execute(true);

            presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
        }
        else
        {
            interpolationCommandList->drop();
        }

        // reset condition if at least one frame was interpolated
        if (desc.numGeneratedFrames > 0)
        {
            frameInterpolationResetCondition = false;
            *pInterpolatedFrame = interpolationOutput();
        }
    }
}

void FrameInterpolationSwapChainDX12::presentInterpolated(UINT SyncInterval, UINT)
{
    const bool bVsync = SyncInterval > 0;

    // interpolation needs to wait for the game queue
    presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
    presentInfo.interpolationQueue->Wait(presentInfo.gameFence, gameFenceValue);

    FfxApiResource interpolatedFrame{}, realFrame{};
    dispatchInterpolationCommands(&interpolatedFrame, &realFrame);

    EnterCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    typename PresentType::PacingData entry{};
    entry.presentCallback                   = presentCallback;
    entry.presentCallbackContext            = presentCallbackContext;
    entry.drawDebugPacingLines              = drawDebugPacingLines;

    if (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)
    {
        FfxApiResourceDescription uiBufferDesc = ffxGetResourceDescriptionDX12(uiReplacementBuffer.resource);
        entry.uiSurface                     = ffxGetResourceDX12(uiReplacementBuffer.resource, uiBufferDesc, nullptr, presentInfo.currentUiSurface.state);
    }
    else
    {
        entry.uiSurface = presentInfo.currentUiSurface;
    }
    entry.vsync                             = bVsync;
    entry.tearingSupported                  = tearingSupported;
    entry.numFramesSentForPresentationBase  = framesSentForPresentation;
    entry.interpolationCompletedFenceValue  = interpolationFenceValue;
    entry.usePremulAlphaComposite           = (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA) != 0;
    entry.currentFrameID                    = currentFrameID;

    // interpolated
    typename PresentType::PacingData::FrameInfo& fiInterpolated = entry.frames[FrameType::Interpolated_1];
    if (interpolatedFrame.resource != nullptr)
    {
        fiInterpolated.doPresent                        = true;
        fiInterpolated.resource                         = interpolatedFrame;
        fiInterpolated.interpolationCompletedFenceValue = interpolationFenceValue;
        fiInterpolated.presentIndex                     = ++framesSentForPresentation;
    }

    // real
    if (!presentInterpolatedOnly)
    {
        typename PresentType::PacingData::FrameInfo& fiReal = entry.frames[FrameType::Real];
        if (realFrame.resource != nullptr)
        {
            fiReal.doPresent    = true;
            fiReal.resource     = realFrame;
            fiReal.presentIndex = ++framesSentForPresentation;
        }
    }

    entry.replacementBufferFenceSignal  = framesSentForPresentation;
    entry.numFramesToPresent            = UINT32(framesSentForPresentation - entry.numFramesSentForPresentationBase);

    interpolationOutputs[interpolationBufferIndex].availabilityFenceValue = entry.numFramesSentForPresentationBase + fiInterpolated.doPresent;

    presentInfo.resetTimer              = frameInterpolationResetCondition;
    presentInfo.scheduledInterpolations = entry;
    LeaveCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    // Set event to kick off async CPU present thread
    SetEvent(presentInfo.presentEvent);

    // hold the replacement object back until previous frame or interpolated is presented
    nextPresentWaitValue = entry.numFramesSentForPresentationBase;
    
    if (replacementFrameLatencyWaitableObjectHandle)
    {
        UINT64 frameLatencyObjectWaitValue = (entry.numFramesSentForPresentationBase - 1) * (entry.numFramesSentForPresentationBase > 0);
        FFX_ASSERT(SUCCEEDED(presentInfo.presentFence->SetEventOnCompletion(frameLatencyObjectWaitValue, replacementFrameLatencyWaitableObjectHandle)));
    }

}

bool FrameInterpolationSwapChainDX12::verifyUiDuplicateResource()
{
    HRESULT hr = S_OK;

    ID3D12Device*   device     = nullptr;
    ID3D12Resource* uiResource = reinterpret_cast<ID3D12Resource*>(presentInfo.currentUiSurface.resource);

    if ((0 == (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)) || (uiResource == nullptr))
    {
        if (nullptr != uiReplacementBuffer.resource)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= resourceSize;
            waitForFenceValue(presentInfo.compositionFenceGPU, framesSentForPresentation, INFINITE, presentInfo.waitCallback);
            SafeRelease(uiReplacementBuffer.resource);
            uiReplacementBuffer = {};
        }
    }
    else
    {
        auto uiResourceDesc = uiResource->GetDesc();

        if (uiReplacementBuffer.resource != nullptr)
        {
            auto internalDesc = uiReplacementBuffer.resource->GetDesc();

            if (uiResourceDesc.Format != internalDesc.Format || uiResourceDesc.Width != internalDesc.Width || uiResourceDesc.Height != internalDesc.Height)
            {
                waitForFenceValue(presentInfo.compositionFenceGPU, framesSentForPresentation, INFINITE, presentInfo.waitCallback);
                SafeRelease(uiReplacementBuffer.resource);
            }
        }

        if (uiReplacementBuffer.resource == nullptr)
        {
            if (SUCCEEDED(uiResource->GetDevice(IID_PPV_ARGS(&device))))
            {

                D3D12_HEAP_PROPERTIES heapProperties{};
                D3D12_HEAP_FLAGS      heapFlags;
                uiResource->GetHeapProperties(&heapProperties, &heapFlags);

                heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
                heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

                // create game render output resource
                if (FAILED(device->CreateCommittedResource(&heapProperties,
                                                           heapFlags,
                                                           &uiResourceDesc,
                                                           ffxGetDX12StateFromResourceState((FfxApiResourceState)presentInfo.currentUiSurface.state),
                                                           nullptr,
                                                           IID_PPV_ARGS(&uiReplacementBuffer.resource))))
                {
                    hr |= E_FAIL;
                }
                else
                {
                    uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
                    totalUsageInBytes += resourceSize;
                    uiReplacementBuffer.resource->SetName(L"AMD FSR Internal Ui Resource");
                }

                SafeRelease(device);
            }
        }
    }

    return SUCCEEDED(hr);
}

void FrameInterpolationSwapChainDX12::copyUiResource()
{
    auto copyList = presentInfo.commandPool.get(presentInfo.gameQueue, L"uiResourceCopyList");
    auto dx12List = copyList->reset();

    ID3D12Resource* dx12ResourceSrc = reinterpret_cast<ID3D12Resource*>(presentInfo.currentUiSurface.resource);
    ID3D12Resource* dx12ResourceDst = uiReplacementBuffer.resource;

    D3D12_TEXTURE_COPY_LOCATION dx12SourceLocation = {};
    dx12SourceLocation.pResource                   = dx12ResourceSrc;
    dx12SourceLocation.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dx12SourceLocation.SubresourceIndex            = 0;

    D3D12_TEXTURE_COPY_LOCATION dx12DestinationLocation = {};
    dx12DestinationLocation.pResource                   = dx12ResourceDst;
    dx12DestinationLocation.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dx12DestinationLocation.SubresourceIndex            = 0;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = dx12ResourceSrc;
    barriers[0].Transition.StateBefore = ffxGetDX12StateFromResourceState((FfxApiResourceState)presentInfo.currentUiSurface.state);
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

    barriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource   = dx12ResourceDst;
    barriers[1].Transition.StateBefore = ffxGetDX12StateFromResourceState((FfxApiResourceState)presentInfo.currentUiSurface.state);
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    dx12List->ResourceBarrier(_countof(barriers), barriers);

    dx12List->CopyResource(dx12ResourceDst, dx12ResourceSrc);

    for (size_t i = 0; i < _countof(barriers); ++i)
    {
        D3D12_RESOURCE_STATES tmpStateBefore = barriers[i].Transition.StateBefore;
        barriers[i].Transition.StateBefore   = barriers[i].Transition.StateAfter;
        barriers[i].Transition.StateAfter    = tmpStateBefore;
    }

    dx12List->ResourceBarrier(_countof(barriers), barriers);

    copyList->execute(true);

    presentInfo.currentUiSurface.resource = nullptr;
}

    // IDXGISwapChain1
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::Present(UINT SyncInterval, UINT Flags)
{

    const UINT64 previousFramesSentForPresentation = framesSentForPresentation;

    if (Flags & DXGI_PRESENT_TEST)
    {
        return presentInfo.swapChain->Present(SyncInterval, Flags);
    }

    setFrameGenerationConfig((FfxFrameGenerationConfig*)&nextFrameGenerationConfig);

    EnterCriticalSection(&criticalSection);

    const UINT currentBackBufferIndex = GetCurrentBackBufferIndex();

    // determine what present path to execute
    const bool fgCallbackConfigured    = frameGenerationCallback != nullptr;
    const bool fgCommandListConfigured = registeredInterpolationCommandLists[currentBackBufferIndex] != nullptr;
    const bool runInterpolation        = interpolationEnabled && (fgCallbackConfigured || fgCommandListConfigured);

    // Ensure presenter thread has signaled before applying any wait to the game queue
    waitForFenceValue(presentInfo.compositionFenceCPU, previousFramesSentForPresentation);
    presentInfo.gameQueue->Wait(presentInfo.compositionFenceGPU, previousFramesSentForPresentation);

    // Verify integrity of internal Ui resource
    if (verifyUiDuplicateResource())
    {
        if ((presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING) && (presentInfo.currentUiSurface.resource != nullptr))
        {
            copyUiResource();
        }
    }

    previousFrameWasInterpolated = runInterpolation;
    if (runInterpolation)
    {
        WaitForSingleObject(presentInfo.interpolationEvent, INFINITE);

        presentInterpolated(SyncInterval, Flags);
    }
    else
    {
        // if no interpolation, then we copied directly to the swapchain. Render UI, present and be done
        presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
        presentInfo.presentQueue->Wait(presentInfo.gameFence, gameFenceValue);

        FFX_ASSERT(presentCallback != nullptr);
        presentWithUiComposition(SyncInterval, Flags);

        if (replacementFrameLatencyWaitableObjectHandle)
        {
            // respect game provided latency settings
            UINT64 frameLatencyObjectWaitValue = (framesSentForPresentation - gameMaximumFrameLatency) * (framesSentForPresentation >= gameMaximumFrameLatency);
            FFX_ASSERT(SUCCEEDED(presentInfo.presentFence->SetEventOnCompletion(frameLatencyObjectWaitValue, replacementFrameLatencyWaitableObjectHandle)));
        }
    }

    replacementSwapBuffers[currentBackBufferIndex].availabilityFenceValue = framesSentForPresentation;

    // Unregister any potential command list
    registeredInterpolationCommandLists[currentBackBufferIndex] = nullptr;
    presentCount++;
    interpolationBufferIndex                                                   = presentCount % _countof(interpolationOutputs);

    //update active backbuffer and block when no buffer is available
    replacementSwapBufferIndex = presentCount % gameBufferCount;

    LeaveCriticalSection(&criticalSection);

    waitForFenceValue(presentInfo.replacementBufferFence, replacementSwapBuffers[replacementSwapBufferIndex].availabilityFenceValue, INFINITE, presentInfo.waitCallback);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    EnterCriticalSection(&criticalSection);

    HRESULT hr = E_FAIL;

    if (riid == IID_ID3D12Resource || riid == IID_ID3D12Resource1 || riid == IID_ID3D12Resource2)
    {
        if (verifyBackbufferDuplicateResources())
        {
            ID3D12Resource* pBuffer = replacementSwapBuffers[Buffer].resource;

            pBuffer->AddRef();
            *ppSurface = pBuffer;

            hr = S_OK;
        }
    }

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    return real()->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return real()->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    HRESULT hr = real()->GetDesc(pDesc);

    // hide interpolation swapchaindesc to keep FI transparent for ISV
    if (SUCCEEDED(hr))
    {
        //update values we changed
        pDesc->BufferCount  = gameBufferCount;
        pDesc->Flags        = gameFlags;
        pDesc->SwapEffect   = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    destroyReplacementResources();

    EnterCriticalSection(&criticalSection);

    const UINT fiAdjustedFlags = getInterpolationEnabledSwapChainFlags(SwapChainFlags);

    // update params expected by the application
    if (BufferCount > 0)
    {
        FFX_ASSERT(BufferCount <= FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT_SDK2);
        gameBufferCount = BufferCount;
    }
    gameFlags       = SwapChainFlags;

    HRESULT hr = real()->ResizeBuffers(0 /* preserve count */, Width, Height, NewFormat, fiAdjustedFlags);

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return real()->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetContainingOutput(IDXGIOutput** ppOutput)
{
    HRESULT hr = DXGI_ERROR_INVALID_CALL;

    if (ppOutput)
    {
        *ppOutput = getMostRelevantOutputFromSwapChain(real());
        hr        = S_OK;
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return real()->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetLastPresentCount(UINT* pLastPresentCount)
{
    return real()->GetLastPresentCount(pLastPresentCount);
}

// IDXGISwapChain1
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    HRESULT hr = real()->GetDesc1(pDesc);

    // hide interpolation swapchaindesc to keep FI transparent for ISV
    if (SUCCEEDED(hr))
    {
        //update values we changed
        pDesc->BufferCount  = gameBufferCount;
        pDesc->Flags        = gameFlags;
        pDesc->SwapEffect   = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return real()->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetHwnd(HWND* pHwnd)
{
    return real()->GetHwnd(pHwnd);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return real()->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS*)
{
    return Present(SyncInterval, PresentFlags);
}

BOOL STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::IsTemporaryMonoSupported(void)
{
    return real()->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return real()->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return real()->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return real()->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return real()->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return real()->GetRotation(pRotation);
}

// IDXGISwapChain2
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetSourceSize(UINT Width, UINT Height)
{
    return real()->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return real()->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetMaximumFrameLatency(UINT MaxLatency)
{
    // store value, so correct value is returned if game asks for it
    gameMaximumFrameLatency = MaxLatency;

    return S_OK;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (pMaxLatency)
    {
        *pMaxLatency = gameMaximumFrameLatency;
    }

    return pMaxLatency ? S_OK : DXGI_ERROR_INVALID_CALL;
}

HANDLE STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFrameLatencyWaitableObject(void)
{
    return replacementFrameLatencyWaitableObjectHandle;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->GetMatrixTransform(pMatrix);
}

// IDXGISwapChain3
UINT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetCurrentBackBufferIndex(void)
{
    EnterCriticalSection(&criticalSection);

    UINT result = (UINT)replacementSwapBufferIndex;

    LeaveCriticalSection(&criticalSection);

    return result;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport)
{
    return real()->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    switch (ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
        break;
    default:
        break;
    }

    return real()->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::ResizeBuffers1(
    UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*)
{
    return E_NOTIMPL;
}

// IDXGISwapChain4
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (Size > 0 && pMetaData != nullptr)
    {
        DXGI_HDR_METADATA_HDR10* HDR10MetaData = NULL;

        switch (Type)
        {
        case DXGI_HDR_METADATA_TYPE_NONE:
            break;
        case DXGI_HDR_METADATA_TYPE_HDR10:
            HDR10MetaData = static_cast<DXGI_HDR_METADATA_HDR10*>(pMetaData);
            break;
        case DXGI_HDR_METADATA_TYPE_HDR10PLUS:
            break;
        }

        FFX_ASSERT_MESSAGE(HDR10MetaData != NULL, "FSR3 Frame interpolaton pxory swapchain: could not initialize HDR metadata");

        if (HDR10MetaData)
        {
            minLuminance = HDR10MetaData->MinMasteringLuminance / 10000.0f;
            maxLuminance = float(HDR10MetaData->MaxMasteringLuminance);
        }
    }

    return real()->SetHDRMetaData(Type, Size, pMetaData);
}

ID3D12GraphicsCommandList* FrameInterpolationSwapChainDX12::getInterpolationCommandList()
{
    EnterCriticalSection(&criticalSection);

    ID3D12GraphicsCommandList* dx12CommandList = nullptr;

    // store active backbuffer index to the command list, used to verify list usage later
    if (interpolationEnabled) {
        ID3D12Resource* currentBackBuffer = nullptr;
        const UINT      currentBackBufferIndex = GetCurrentBackBufferIndex();
        if (SUCCEEDED(GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&currentBackBuffer))))
        {
            Dx12Commands* registeredCommands = registeredInterpolationCommandLists[currentBackBufferIndex];

            // drop if already existing
            if (registeredCommands != nullptr)
            {
                registeredCommands->drop(true);
                registeredCommands = nullptr;
            }

            registeredCommands = presentInfo.commandPool.get(presentInfo.interpolationQueue, L"getInterpolationCommandList()");
            FFX_ASSERT(registeredCommands);

            dx12CommandList = registeredCommands->reset();

            registeredInterpolationCommandLists[currentBackBufferIndex] = registeredCommands;

            SafeRelease(currentBackBuffer);
        }
    }
    
    LeaveCriticalSection(&criticalSection);

    return dx12CommandList;
}
