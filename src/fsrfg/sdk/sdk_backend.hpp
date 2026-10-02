// SPDX-License-Identifier: MIT
// Narrow DX12 host adapter for the pinned MIT swapchain; not an AMD renderer.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <algorithm>
#include "../../fsr/include/framegeneration/include/ffx_framegeneration.h"
#include "../../fsr/include/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#define FFX_API
#define FFX_SDK_MAKE_VERSION(major,minor,patch) (((major)<<22)|((minor)<<12)|(patch))
#define FFX_ASSERT(x) assert(x)
#define FFX_ASSERT_MESSAGE(x,msg) assert(x)
#define FFX_UNUSED(x) ((void)(x))
#define FFX_PRINT_MESSAGE(type,msg) OutputDebugStringW(msg)
using FfxDevice = void*;
using FfxCommandList = void*;
using FfxCommandQueue = void*;
using FfxSwapchain = void*;
using FfxFrameInterpolationDispatchFlags = uint32_t;
typedef int32_t FfxErrorCode;

/// Error codes and their meaning
///
/// @ingroup Errors
typedef enum FfxErrorCodes {

    FFX_OK                            = 0,           ///< The operation completed successfully.
    FFX_ERROR_INVALID_POINTER         = 0x80000000,  ///< The operation failed due to an invalid pointer.
    FFX_ERROR_INVALID_ALIGNMENT       = 0x80000001,  ///< The operation failed due to an invalid alignment.
    FFX_ERROR_INVALID_SIZE            = 0x80000002,  ///< The operation failed due to an invalid size.
    FFX_EOF                           = 0x80000003,  ///< The end of the file was encountered.
    FFX_ERROR_INVALID_PATH            = 0x80000004,  ///< The operation failed because the specified path was invalid.
    FFX_ERROR_EOF                     = 0x80000005,  ///< The operation failed because end of file was reached.
    FFX_ERROR_MALFORMED_DATA          = 0x80000006,  ///< The operation failed because of some malformed data.
    FFX_ERROR_OUT_OF_MEMORY           = 0x80000007,  ///< The operation failed because it ran out memory.
    FFX_ERROR_INCOMPLETE_INTERFACE    = 0x80000008,  ///< The operation failed because the interface was not fully configured.
    FFX_ERROR_INVALID_ENUM            = 0x80000009,  ///< The operation failed because of an invalid enumeration value.
    FFX_ERROR_INVALID_ARGUMENT        = 0x8000000a,  ///< The operation failed because an argument was invalid.
    FFX_ERROR_OUT_OF_RANGE            = 0x8000000b,  ///< The operation failed because a value was out of range.
    FFX_ERROR_NULL_DEVICE             = 0x8000000c,  ///< The operation failed because a device was null.
    FFX_ERROR_BACKEND_API_ERROR       = 0x8000000d,  ///< The operation failed because the backend API returned an error code.
    FFX_ERROR_INSUFFICIENT_MEMORY     = 0x8000000e,  ///< The operation failed because there was not enough memory.
    FFX_ERROR_INVALID_VERSION         = 0x8000000f,  ///< The operation failed because the wrong backend was linked.
    FFX_ERROR_ACCESS_DENIED           = 0x80000010,  ///< The operation failed because access to the resource was denied.

}FfxErrorCodes;

/// Helper macro to return error code y from a function when a specific condition, x, is not met.
///
/// @ingroup Errors
#define FFX_RETURN_ON_ERROR(x, y)                   \
    if (!(x))                                       \
    {                                               \
        return (y);                                 \
    }

/// Helper macro to return error code x from a function when it is not FFX_OK.
///
/// @ingroup Errors
#define FFX_VALIDATE(x)                             \
    {                                               \
        FfxErrorCode ret = x;                       \
        FFX_RETURN_ON_ERROR(ret == FFX_OK, ret);    \
    }

FFX_API FfxErrorCode ffxReplaceSwapchainForFrameinterpolationDX12(FfxCommandQueue gameQueue, FfxSwapchain& gameSwapChain);

/// Creates a <c><i>FfxSwapchain</i></c> from passed in parameters.
///
/// @param [in] desc                    The DXGI_SWAP_CHAIN_DESC describing the swapchain creation parameters from the calling application.
/// @param [in] queue                   The ID3D12CommandQueue to use for frame interpolation presentation.
/// @param [in] dxgiFactory             The IDXGIFactory to use for DX12 swapchain creation.
/// @param [out] outGameSwapChain       The created <c><i>FfxSwapchain</i></c>.
///
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          One of the parameters is invalid.
/// FFX_ERROR_OUT_OF_MEMORY             Insufficient memory available to allocate <c><i>FfxSwapchain</i></c> or underlying component.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxCreateFrameinterpolationSwapchainDX12(const DXGI_SWAP_CHAIN_DESC* desc,
                                                      ID3D12CommandQueue* queue,
                                                      IDXGIFactory* dxgiFactory,
                                                      FfxSwapchain& outGameSwapChain);

/// Creates a <c><i>FfxSwapchain</i></c> from passed in parameters.
///
/// @param [in] hWnd                    The HWND handle for the calling application.
/// @param [in] desc1                   The DXGI_SWAP_CHAIN_DESC1 describing the swapchain creation parameters from the calling application.
/// @param [in] fullscreenDesc          The DXGI_SWAP_CHAIN_FULLSCREEN_DESC describing the full screen swapchain creation parameters from the calling application.
/// @param [in] queue                   The ID3D12CommandQueue to use for frame interpolation presentation.
/// @param [in] dxgiFactory             The IDXGIFactory to use for DX12 swapchain creation.
/// @param [out] outGameSwapChain       The created <c><i>FfxSwapchain</i></c>.
///
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          One of the parameters is invalid.
/// FFX_ERROR_OUT_OF_MEMORY             Insufficient memory available to allocate <c><i>FfxSwapchain</i></c> or underlying component.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxCreateFrameinterpolationSwapchainForHwndDX12(HWND hWnd,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc1,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                             ID3D12CommandQueue* queue,
                                                             IDXGIFactory* dxgiFactory,
                                                             FfxSwapchain& outGameSwapChain);

/// Waits for the <c><i>FfxSwapchain</i></c> to complete presentation.
///
/// @param [in] gameSwapChain           The <c><i>FfxSwapchain</i></c> to wait on.
///
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxWaitForPresents(FfxSwapchain gameSwapChain);

/// Registers a <c><i>FfxApiResource</i></c> to use for UI with the provided <c><i>FfxSwapchain</i></c>.
///
/// @param [in] gameSwapChain           The <c><i>FfxSwapchain</i></c> to to register the UI resource with.
/// @param [in] uiResource              The <c><i>FfxApiResource</i></c> representing the UI resource.
/// @param [in] flags                   A set of <c><i>FfxUiCompositionFlags</i></c>.
/// 
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxRegisterFrameinterpolationUiResourceDX12(FfxSwapchain gameSwapChain, FfxApiResource uiResource, uint32_t flags);

/// Fetches a <c><i>FfxCommandList</i></c> from the <c><i>FfxSwapchain</i></c>.
///
/// @param [in] gameSwapChain           The <c><i>FfxSwapchain</i></c> to get a <c><i>FfxCommandList</i></c> from.
/// @param [out] gameCommandlist        The <c><i>FfxCommandList</i></c> from the provided <c><i>FfxSwapchain</i></c>.
///
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxGetFrameinterpolationCommandlistDX12(FfxSwapchain gameSwapChain, FfxCommandList& gameCommandlist);

/// Fetches a <c><i>FfxApiResource</i></c>  representing the backbuffer from the <c><i>FfxSwapchain</i></c>.
///
/// @param [in] gameSwapChain           The <c><i>FfxSwapchain</i></c> to get a <c><i>FfxApiResource</i></c> backbuffer from.
///
/// @returns
/// An abstract FidelityFX resources for the swapchain backbuffer.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxApiResource ffxGetFrameinterpolationTextureDX12(FfxSwapchain gameSwapChain);

/// Sets a <c><i>FfxFrameGenerationConfig</i></c> to the internal FrameInterpolationSwapChain (in the backend).
///
/// @param [in] config                  The <c><i>FfxFrameGenerationConfig</i></c> to set.
///
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainDX12(FfxFrameGenerationConfig const* config);

//enum value matches enum FfxApiConfigureFrameGenerationSwapChainKeyDX12
typedef enum FfxFrameInterpolationSwapchainConfigureKey
{
    FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK = 0,
    FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING = 2,
} FfxFrameInterpolationSwapchainConfigureKey;

/// Configures <c><i>FfxSwapchain</i></c> via KeyValue API post <c><i>FfxSwapchain</i></c> context creation
///
/// @param [in] gameSwapChain           The <c><i>FfxSwapchain</i></c> to configure via KeyValue API
/// @param [in] key                     The <c><i>FfxFrameInterpolationSwapchainConfigureKey</i></c> is key
/// @param [in] valuePtr                The <c><i><void *></i></c> pointer to value. What this pointer deference to depends on key.
/// 
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxConfigureFrameInterpolationSwapchainDX12(FfxSwapchain gameSwapChain, FfxFrameInterpolationSwapchainConfigureKey key, void* valuePtr);

/// Query how much GPU memory created by <c><i>FfxSwapchain</i></c>. This excludes GPU memory created by DXGI (ie. size of DXGI swapchaim backbuffers).
///
/// @param [in] gameSwapChain           The <c><i>FfxSwapchain</i></c>
/// @param [in out] vramUsage           The <c><i>FfxApiEffectMemoryUsage</i></c> is the GPU memory created by FrameInterpolationSwapchain
/// 
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12(FfxSwapchain gameSwapChain, FfxApiEffectMemoryUsage* vramUsage);

/// Query how much GPU memory created by <c><i>FfxSwapchain</i></c>. This excludes GPU memory created by DXGI (ie. size of DXGI swapchaim backbuffers).
///
/// @param [in] device                  The <c><i>FfxDevice</i></c>
/// @param [in] displaySize             A pointer to a <c><i>FfxApiDimensions2D</i></c> structure.
/// @param [in] backbufferFormat        The <c><i>FfxApiSurfaceFormat</i></c> structure.
/// @param [in] backbufferCount         The <c><i>uint32_t</i></c>.
/// @param [in] uiResourceSize          A pointer to a <c><i>FfxApiDimensions2D</i></c> structure.
/// @param [in] uiResourceFormat        The <c><i>FfxApiSurfaceFormat</i></c> structure.
/// @param [in] flags                   The <c><i>uint32_t</i></c> value
/// @param [in out] vramUsage           The <c><i>FfxApiEffectMemoryUsage</i></c> is the GPU memory created by FrameInterpolationSwapchain
/// 
/// @retval
/// FFX_OK                              The operation completed successfully.
/// @retval
/// FFX_ERROR_INVALID_ARGUMENT          Could not query the interface for the frame interpolation swap chain.
///
/// @ingroup DX12FrameInterpolation
FFX_API FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12V2(FfxDevice device, FfxApiDimensions2D* displaySize, FfxApiSurfaceFormat backbufferFormat, uint32_t backBufferCount, FfxApiDimensions2D* uiResourceSize, FfxApiSurfaceFormat uiResourceFormat, uint32_t flags, FfxApiEffectMemoryUsage* vramUsage);



FfxErrorCode ffxSetFrameGenerationConfigToSwapchainDX12(const FfxFrameGenerationConfig*);
FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12V2(FfxDevice, FfxApiDimensions2D*, FfxApiSurfaceFormat, uint32_t, FfxApiDimensions2D*, FfxApiSurfaceFormat, uint32_t, FfxApiEffectMemoryUsage*);
FfxApiResource ffxGetNamedResourceDX12(const ID3D12Resource*, FfxApiResourceDescription, const wchar_t*, uint32_t);
FfxApiResourceDescription ffxGetResourceDescriptionDX12(const ID3D12Resource*, FfxApiResourceUsage = FFX_API_RESOURCE_USAGE_READ_ONLY);
FfxApiResource ffxGetResourceDX12(const ID3D12Resource*, FfxApiResourceDescription, const wchar_t*, uint32_t);
DXGI_FORMAT ffxGetDX12FormatFromSurfaceFormat(FfxApiSurfaceFormat);
D3D12_RESOURCE_STATES ffxGetDX12StateFromResourceState(FfxApiResourceState);
inline FfxCommandList ffxGetCommandListDX12(ID3D12GraphicsCommandList* list) { return list; }
inline FfxSwapchain ffxGetSwapchainDX12(IDXGISwapChain4* sc) { return sc; }
inline IDXGISwapChain4* ffxGetDX12SwapchainPtr(FfxSwapchain sc) { return static_cast<IDXGISwapChain4*>(sc); }
