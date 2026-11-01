/* AMD RR 1.0/1.1/1.2 public contracts, implemented by the D3Dalloy MetalFX provider.
 * Imported from zzz-wine-yaagl-pr-runtime e9d0e50db84 dlls/amd_fidelityfx_denoiser_dx12/main.c. */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../fsr/include/denoisers/include/ffx_denoiser.h"
#include "bridge.h"
#include "legacy.h"

#define PROVIDER_ID 0x4d46585252000001ull
#define BACKEND_DX12 2u
#define SIGNAL_MASK 0x7fu
#define RADIANCE_MASK 0x3eu
struct backend_dx12_desc { ffxCreateContextDescHeader header; void *device; };
/* A binary marker, not an AMD model/version claim. */
static const char provider_name[] = "MetalFX (D3Dalloy owned Ray Regeneration 1.2.0; d3dalloy-owned-fsr-rr-metalfx-v1)";

struct rr_context
{
    struct rr_context *next;
    uint64_t native;
    void *device;
    ffxAllocationCallbacks allocation;
    uint32_t width, height, signals, checkerboard, flags;
    uint32_t version, mode;
    ffxApiMessage message;
    SRWLOCK lock;
};
static SRWLOCK contexts_lock = SRWLOCK_INIT;
static struct rr_context *contexts;
static SRWLOCK debug_lock = SRWLOCK_INIT;
static ffxApiMessage global_message;
static uint32_t global_debug_level;

static ffxReturnCode_t finish(ffxReturnCode_t result)
{
    ffxApiMessage callback;
    uint32_t level;
    AcquireSRWLockShared(&debug_lock);
    callback = global_message;
    level = global_debug_level;
    ReleaseSRWLockShared(&debug_lock);
    /* Call user code only after releasing all provider locks. */
    if (result && callback && (level & FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS))
        callback(FFX_API_MESSAGE_TYPE_ERROR, L"D3Dalloy MetalFX RR: operation rejected or native processing failed.");
    return result;
}

static void init_packet(struct rr_packet_header *header, uint32_t size,
                        uint32_t operation, const struct rr_context *context)
{
    header->size = size;
    header->operation = operation;
    header->context = context ? context->native : 0;
}

/* Keep the registry read lock until the call completes: destroy cannot release
 * the native handle or its device while another API call uses either. */
static struct rr_context *lock_context(ffxContext *handle)
{
    struct rr_context *context;
    AcquireSRWLockShared(&contexts_lock);
    if (handle)
        for (context = contexts; context; context = context->next)
            if (context == *handle)
            {
                AcquireSRWLockExclusive(&context->lock);
                return context;
            }
    ReleaseSRWLockShared(&contexts_lock);
    return NULL;
}

static void unlock_context(struct rr_context *context)
{
    ReleaseSRWLockExclusive(&context->lock);
    ReleaseSRWLockShared(&contexts_lock);
}

static ffxReturnCode_t finish_context(struct rr_context *context, ffxReturnCode_t result)
{
    ffxApiMessage callback = context->message;
    unlock_context(context);
    if (result && callback)
        callback(FFX_API_MESSAGE_TYPE_ERROR, L"D3Dalloy MetalFX RR: operation rejected or native processing failed.");
    return finish(result);
}

static int valid_chain(const ffxApiHeader *head)
{
    const ffxApiHeader *slow = head, *fast = head;
    while (fast && fast->pNext)
    {
        slow = slow->pNext;
        fast = fast->pNext->pNext;
        if (slow == fast) return 0;
    }
    return 1;
}

static int valid_create(uint32_t width, uint32_t height, uint32_t signals,
                        uint32_t checkerboard, uint32_t flags)
{
    return width && height && !(signals & ~SIGNAL_MASK) && (signals & RADIANCE_MASK) &&
           !(checkerboard & ~signals) && !(flags & ~(FFX_DENOISER_ENABLE_DEBUGGING | FFX_DENOISER_ENABLE_VALIDATION));
}

static ffxReturnCode_t unknown_type(uint64_t type)
{
    return (type & FFX_API_EFFECT_MASK) == FFX_API_EFFECT_ID_DENOISER
        ? FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE : FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t ffxCreateContext(ffxContext *output, ffxCreateContextDescHeader *desc,
                                const ffxAllocationCallbacks *callbacks)
{
    const ffxApiHeader *entry;
    const ffxCreateContextDescDenoiser *create = NULL;
    ffxCreateContextDescDenoiser normalized;
    uint32_t mode = 0;
    ffxApiMessage message = NULL;
    const struct backend_dx12_desc *backend = NULL;
    struct rr_context *context;
    struct rr_bridge_create_packet packet = {0};
    ffxReturnCode_t result;
    int override_seen = 0;
    if (!output || !desc || (callbacks && (!!callbacks->alloc != !!callbacks->dealloc)) || !valid_chain(desc))
        return finish(FFX_API_RETURN_ERROR_PARAMETER);
    *output = NULL;
    for (entry = desc; entry; entry = entry->pNext)
    {
        switch (entry->type)
        {
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER:
            if (create) return finish(FFX_API_RETURN_ERROR_PARAMETER);
            create = (const void *)entry;
            break;
        case BACKEND_DX12:
            if (backend) return finish(FFX_API_RETURN_ERROR_PARAMETER);
            backend = (const void *)entry;
            break;
        case FFX_API_DESC_TYPE_OVERRIDE_VERSION:
            if (override_seen++) return finish(FFX_API_RETURN_ERROR_PARAMETER);
            if (((const struct ffxOverrideVersion *)entry)->versionId != PROVIDER_ID)
                return finish(FFX_API_RETURN_NO_PROVIDER);
            break;
        default: return finish(unknown_type(entry->type));
        }
    }
    if (create && (create->version == RR_10 || create->version == RR_11))
    {
        const struct rr_legacy_create *old = (const void *)create;
        if (old->mode > 2 || (old->flags & ~3u)) return finish(FFX_API_RETURN_ERROR_PARAMETER);
        memset(&normalized, 0, sizeof(normalized));
        normalized.version = old->version;
        normalized.maxRenderSize = old->maxRenderSize;
        message = old->fpMessage;
        normalized.signalFlags = old->mode == 0 ? 0x36 : old->mode == 1 ? 0x30 : 0x20;
        if (old->flags & 2) normalized.signalFlags |= 8;
        normalized.flags = old->flags & 1;
        mode = old->mode + 1;
        create = &normalized;
    }
    if (!create || !backend || !backend->device ||
        (create->version != FFX_DENOISER_VERSION && create->version != RR_10 && create->version != RR_11) ||
        !valid_create(create->maxRenderSize.width, create->maxRenderSize.height,
                      create->signalFlags, create->checkerboardSignalFlags, create->flags))
        return finish(FFX_API_RETURN_ERROR_PARAMETER);
    context = callbacks && callbacks->alloc ? callbacks->alloc(callbacks->pUserData, sizeof(*context)) : malloc(sizeof(*context));
    if (!context) return finish(FFX_API_RETURN_ERROR_MEMORY);
    memset(context, 0, sizeof(*context));
    InitializeSRWLock(&context->lock);
    if (callbacks) context->allocation = *callbacks;
    init_packet(&packet.header, sizeof(packet), RR_BRIDGE_CREATE, NULL);
    packet.device = (uint64_t)(uintptr_t)backend->device;
    packet.version = create->version;
    context->version = create->version;
    context->mode = mode;
    context->message = message;
    packet.width = context->width = create->maxRenderSize.width;
    packet.height = context->height = create->maxRenderSize.height;
    packet.signals = context->signals = create->signalFlags;
    packet.checkerboard = context->checkerboard = create->checkerboardSignalFlags;
    packet.flags = context->flags = create->flags;
    result = rr_backend_call(&packet);
    if (result || !packet.header.context)
    {
        if (context->allocation.dealloc) context->allocation.dealloc(context->allocation.pUserData, context);
        else free(context);
        return finish(result ? result : FFX_API_RETURN_ERROR_RUNTIME_ERROR);
    }
    context->native = packet.header.context;
    context->device = backend->device;
    ((ULONG (WINAPI *)(void *))(*(void ***)context->device)[1])(context->device);
    AcquireSRWLockExclusive(&contexts_lock);
    context->next = contexts;
    contexts = context;
    *output = context;
    ReleaseSRWLockExclusive(&contexts_lock);
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDestroyContext(ffxContext *handle, const ffxAllocationCallbacks *callbacks)
{
    struct rr_context **link, *context = NULL;
    struct rr_packet_header packet = {0};
    ffxAllocationCallbacks allocation = {0};
    ffxReturnCode_t result;
    if (!handle) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (callbacks) allocation = *callbacks;
    AcquireSRWLockExclusive(&contexts_lock);
    for (link = &contexts; *link; link = &(*link)->next)
        if (*link == *handle) { context = *link; break; }
    if (!context || context->allocation.alloc != allocation.alloc ||
        context->allocation.dealloc != allocation.dealloc || context->allocation.pUserData != allocation.pUserData)
    {
        ReleaseSRWLockExclusive(&contexts_lock);
        return finish(FFX_API_RETURN_ERROR_PARAMETER);
    }
    init_packet(&packet, sizeof(packet), RR_BRIDGE_DESTROY, context);
    result = rr_backend_call(&packet);
    if (!result) { *link = context->next; *handle = NULL; }
    ReleaseSRWLockExclusive(&contexts_lock);
    if (!result)
    {
        ((ULONG (WINAPI *)(void *))(*(void ***)context->device)[2])(context->device);
        if (allocation.dealloc) allocation.dealloc(allocation.pUserData, context);
        else free(context);
    }
    return finish(result);
}

static int finite_values(const float *values, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; ++i) if (!isfinite(values[i])) return 0;
    return 1;
}

ffxReturnCode_t ffxConfigure(ffxContext *handle, const ffxConfigureDescHeader *desc)
{
    const ffxConfigureDescDenoiserKeyValue *config;
    struct rr_context *context;
    struct rr_bridge_configure_packet packet = {0};
    ffxReturnCode_t result;
    if (!desc || !valid_chain(desc)) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (desc->pNext) return finish(FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG)
    {
        const struct ffxConfigureDescGlobalDebug *debug = (const void *)desc;
        if (handle) return finish(FFX_API_RETURN_ERROR_PARAMETER);
        if (debug->effectId != FFX_API_EFFECT_ID_DENOISER) return finish(FFX_API_RETURN_NO_PROVIDER);
        if (debug->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE &&
            debug->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS &&
            debug->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS &&
            debug->debugLevel != FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE)
            return finish(FFX_API_RETURN_ERROR_PARAMETER);
        AcquireSRWLockExclusive(&debug_lock);
        global_message = debug->fpMessage;
        global_debug_level = debug->debugLevel;
        ReleaseSRWLockExclusive(&debug_lock);
        return FFX_API_RETURN_OK;
    }
    if (!(context = lock_context(handle))) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (desc->type != (context->mode ? RR_TYPE(0x08) : FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE))
    {
        return finish_context(context, unknown_type(desc->type));
    }
    if (context->version == RR_10)
    {
        const struct rr_configure_10 *old = (const void *)desc;
        packet.key = 8;
        memcpy(packet.legacy_values, &old->settings.historyRejectionStrength, sizeof(packet.legacy_values));
        init_packet(&packet.header, sizeof(packet), RR_BRIDGE_CONFIGURE, context);
        result = rr_backend_call(&packet);
        return finish_context(context, result);
    }
    config = (const void *)desc;
    if (config->key < 1 || config->key > (context->mode ? 6 : 7) || config->count != 1 || !config->data)
    {
        return finish_context(context, FFX_API_RETURN_ERROR_PARAMETER);
    }
    packet.key = config->key;
    memcpy(packet.value, config->data, config->key == 7 ? sizeof(packet.value) : sizeof(float));
    if (!finite_values(packet.value, config->key == 7 ? 2 : 1))
    {
        return finish_context(context, FFX_API_RETURN_ERROR_PARAMETER);
    }
    init_packet(&packet.header, sizeof(packet), RR_BRIDGE_CONFIGURE, context);
    result = rr_backend_call(&packet);
    return finish_context(context, result);
}

ffxReturnCode_t ffxQuery(ffxContext *handle, ffxQueryDescHeader *desc)
{
    struct rr_context *context = NULL;
    struct rr_bridge_query_packet packet = {0};
    ffxReturnCode_t result;
    if (!desc || !valid_chain(desc)) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (desc->pNext) return finish(FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)
    {
        struct ffxQueryDescGetVersions *query = (void *)desc;
        if (handle || !query->outputCount || !query->device) return finish(FFX_API_RETURN_ERROR_PARAMETER);
        if (query->createDescType != FFX_API_EFFECT_ID_DENOISER &&
            query->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER) return finish(FFX_API_RETURN_NO_PROVIDER);
        if (*query->outputCount)
        {
            if (query->versionIds) query->versionIds[0] = PROVIDER_ID;
            if (query->versionNames) query->versionNames[0] = provider_name;
        }
        *query->outputCount = 1;
        return FFX_API_RETURN_OK;
    }
    if (handle && !(context = lock_context(handle))) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    init_packet(&packet.header, sizeof(packet), RR_BRIDGE_QUERY, context);
    if (desc->type == RR_TYPE(0x09) || desc->type == RR_TYPE(0x0a) || desc->type == RR_TYPE(0x0b))
    {
        /* 0x0b has incompatible 1.0 and 1.1 layouts. A context is required
         * to discriminate them; never infer a version from pointer contents. */
        result = FFX_API_RETURN_NO_PROVIDER;
        if (desc->type == RR_TYPE(0x0a) && !context)
        {
            struct rr_legacy_version *query = (void *)desc;
            if (!query->device || !query->major || !query->minor || !query->patch)
                result = FFX_API_RETURN_ERROR_PARAMETER;
            else
            {
                *query->major = 1; *query->minor = 2; *query->patch = 0;
                result = FFX_API_RETURN_OK;
            }
            goto query_done;
        }
        if (!context || !context->mode) goto query_done;
        if (desc->type == RR_TYPE(0x0a))
        {
            struct rr_legacy_version *query = (void *)desc;
            if (!query->major || !query->minor || !query->patch) result = FFX_API_RETURN_ERROR_PARAMETER;
            else
            {
                *query->major = 1; *query->minor = (context->version >> 12) & 0x3ff; *query->patch = 0;
                result = FFX_API_RETURN_OK;
            }
        }
        else if (desc->type == RR_TYPE(0x09))
        {
            struct rr_legacy_memory *query = (void *)desc;
            if (!query->gpuMemoryUsage) result = FFX_API_RETURN_ERROR_PARAMETER;
            else if (!(result = rr_backend_call(&packet)))
            {
                query->gpuMemoryUsage->totalUsageInBytes = packet.total_bytes;
                query->gpuMemoryUsage->aliasableUsageInBytes = packet.aliasable_bytes;
            }
        }
        else if (context->version == RR_10)
        {
            struct rr_defaults_10 *query = (void *)desc;
            struct rr_legacy_settings defaults = {0};
            float fields[5];
            unsigned int i;
            if (!query->defaultSettings) result = FFX_API_RETURN_ERROR_PARAMETER;
            else
            {
                for (i = 0; i < 5; ++i)
                {
                    packet.key = i + 1;
                    if ((result = rr_backend_call(&packet))) break;
                    fields[i] = packet.value[0];
                }
                if (!result)
                {
                    defaults.crossBilateralNormalStrength = fields[0];
                    defaults.stabilityBias = fields[1];
                    defaults.maxRadiance = fields[2];
                    defaults.radianceClipStdK = fields[3];
                    defaults.gaussianKernelRelaxation = fields[4];
                    *query->defaultSettings = defaults;
                }
            }
        }
        else
        {
            ffxQueryDescDenoiserGetDefaultKeyValue *query = (void *)desc;
            if (query->key < 1 || query->key > 6 || query->count != 1 || !query->data)
                result = FFX_API_RETURN_ERROR_PARAMETER;
            else
            {
                packet.key = query->key;
                if (!(result = rr_backend_call(&packet))) memcpy(query->data, packet.value, sizeof(float));
            }
        }
        goto query_done;
    }
    if (context && context->mode && (desc->type & FFX_API_EFFECT_MASK) == FFX_API_EFFECT_ID_DENOISER)
    {
        result = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        goto query_done;
    }
    switch (desc->type)
    {
    case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION:
    {
        struct ffxQueryGetProviderVersion *query = (void *)desc;
        query->versionId = context ? PROVIDER_ID : 0;
        query->versionName = context ? provider_name : NULL;
        result = context ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER;
        break;
    }
    case FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE:
    {
        ffxQueryDescDenoiserGetDefaultKeyValue *query = (void *)desc;
        if (!context || query->key < 1 || query->key > 7 || query->count != 1 || !query->data)
        { result = FFX_API_RETURN_ERROR_PARAMETER; break; }
        packet.key = query->key;
        result = rr_backend_call(&packet);
        if (!result) memcpy(query->data, packet.value, query->key == 7 ? sizeof(packet.value) : sizeof(float));
        break;
    }
    case FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE:
    {
        ffxQueryDescDenoiserGetGPUMemoryUsage *query = (void *)desc;
        if (!query->gpuMemoryUsage || (!context && (!query->device ||
            !valid_create(query->maxRenderSize.width, query->maxRenderSize.height,
                          query->signalFlags, query->checkerboardSignalFlags, query->flags))))
        { result = FFX_API_RETURN_ERROR_PARAMETER; break; }
        packet.device = (uint64_t)(uintptr_t)(context ? context->device : query->device);
        packet.width = context ? context->width : query->maxRenderSize.width;
        packet.height = context ? context->height : query->maxRenderSize.height;
        packet.signals = context ? context->signals : query->signalFlags;
        packet.checkerboard = context ? context->checkerboard : query->checkerboardSignalFlags;
        packet.flags = context ? context->flags : query->flags;
        result = rr_backend_call(&packet);
        if (!result)
        {
            query->gpuMemoryUsage->totalUsageInBytes = packet.total_bytes;
            query->gpuMemoryUsage->aliasableUsageInBytes = packet.aliasable_bytes;
        }
        break;
    }
    default: result = unknown_type(desc->type); break;
    }
query_done:
    if (context) return finish_context(context, result);
    return finish(result);
}

static int pack_resource(struct rr_bridge_resource *out, const struct FfxApiResource *resource, int required)
{
    uint32_t state = resource->state;
    if (!resource->resource) return !required && !state;
    /* FFX states, not D3D12 states. Reject unknown bits and write/read mixtures. */
    if (!state || (state & ~0x3ffu)) return 0;
    if ((state & (FFX_API_RESOURCE_STATE_COMMON | FFX_API_RESOURCE_STATE_UNORDERED_ACCESS |
                  FFX_API_RESOURCE_STATE_COPY_DEST | FFX_API_RESOURCE_STATE_PRESENT |
                  FFX_API_RESOURCE_STATE_RENDER_TARGET | FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT)) &&
        (state & (state - 1))) return 0;
    out->object = (uint64_t)(uintptr_t)resource->resource;
    out->state = state;
    return 1;
}

static ffxReturnCode_t dispatch_current(ffxContext *handle, const ffxDispatchDescHeader *desc,
                                      uint32_t legacy_mode, const struct FfxApiResource *fused)
{
    const ffxApiHeader *entry;
    const ffxDispatchDescDenoiser *frame = NULL;
    const ffxDispatchDescDenoiserDebugView *debug = NULL;
    const ffxDispatchDescDenoiserDominantLight *light = NULL;
    const FfxApiDenoiserSignal *signals[RR_BRIDGE_SIGNAL_COUNT] = {0};
    struct rr_bridge_dispatch_packet packet = {0};
    struct rr_context *context;
    ffxReturnCode_t result = FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t seen = 0;
    unsigned int i;
    if (!desc || !valid_chain(desc)) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_DENOISER)
        return finish(unknown_type(desc->type));
    for (entry = desc; entry; entry = entry->pNext)
    {
        const FfxApiDenoiserSignal *signal = NULL;
        switch (entry->type)
        {
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER:
            if (frame) return finish(FFX_API_RETURN_ERROR_PARAMETER);
            frame = (const void *)entry;
            continue;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW:
            if (debug) return finish(FFX_API_RETURN_ERROR_PARAMETER);
            debug = (const void *)entry;
            continue;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION:
            i = 0; signal = &((const ffxDispatchDescDenoiserAmbientOcclusion *)entry)->signal; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE:
            i = 1; signal = &((const ffxDispatchDescDenoiserDirectDiffuse *)entry)->signal; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR:
            i = 2; signal = &((const ffxDispatchDescDenoiserDirectSpecular *)entry)->signal; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT:
            i = 3; light = (const void *)entry; signal = &light->signal; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE:
            i = 4; signal = &((const ffxDispatchDescDenoiserIndirectDiffuse *)entry)->signal; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR:
            i = 5; signal = &((const ffxDispatchDescDenoiserIndirectSpecular *)entry)->signal; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION:
            i = 6; signal = &((const ffxDispatchDescDenoiserSpecularOcclusion *)entry)->signal; break;
        default: return finish(unknown_type(entry->type));
        }
        if (signals[i]) return finish(FFX_API_RETURN_ERROR_PARAMETER);
        signals[i] = signal;
        seen |= 1u << i;
    }
    if (!frame || !frame->commandList) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (!(context = lock_context(handle))) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (legacy_mode != context->mode) goto done;
    if (seen != context->signals || !frame->renderSize.width || !frame->renderSize.height ||
        frame->renderSize.width > context->width || frame->renderSize.height > context->height ||
        (frame->flags & ~(FFX_DENOISER_DISPATCH_RESET | FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO))) goto done;
    init_packet(&packet.header, sizeof(packet), RR_BRIDGE_DISPATCH, context);
    packet.command_list = (uint64_t)(uintptr_t)frame->commandList;
    if (!pack_resource(&packet.resources[0], &frame->linearDepth, 1) ||
        !pack_resource(&packet.resources[1], &frame->motionVectors, 1) ||
        !pack_resource(&packet.resources[2], &frame->normals, 1) ||
        !pack_resource(&packet.resources[3], &frame->specularAlbedo, !!(seen & 0x2c)) ||
        !pack_resource(&packet.resources[4], &frame->diffuseAlbedo, !!(seen & 0x1a) || legacy_mode == 3)) goto done;
    for (i = 0; i < RR_BRIDGE_SIGNAL_COUNT; ++i)
    {
        if (!signals[i]) continue;
        if (!pack_resource(&packet.resources[5 + i * 2], &signals[i]->input, 1) ||
            !pack_resource(&packet.resources[6 + i * 2], &signals[i]->output, 1) ||
            signals[i]->checkerboardOrigin > 1 || (!(context->checkerboard & (1u << i)) && signals[i]->checkerboardOrigin)) goto done;
        packet.checkerboard_origin[i] = signals[i]->checkerboardOrigin;
    }
    memcpy(packet.motion_scale, &frame->motionVectorScale, sizeof(packet.motion_scale));
    memcpy(packet.jitter, &frame->jitterOffsets, sizeof(packet.jitter));
    memcpy(packet.camera_delta, &frame->cameraPositionDelta, sizeof(packet.camera_delta));
    memcpy(packet.view, &frame->view, sizeof(packet.view));
    memcpy(packet.projection, &frame->projection, sizeof(packet.projection));
    packet.depth_min = frame->linearDepthBounds.min;
    packet.depth_max = frame->linearDepthBounds.max;
    packet.width = frame->renderSize.width;
    packet.height = frame->renderSize.height;
    packet.frame_index = frame->frameIndex;
    packet.flags = frame->flags;
    packet.legacy_mode = legacy_mode;
    if (fused && !pack_resource(&packet.resources[RR_BRIDGE_FUSED_ALBEDO], fused, 1)) goto done;
    if (!finite_values(packet.motion_scale, 3) || !finite_values(packet.jitter, 2) ||
        !finite_values(packet.camera_delta, 3) || !finite_values(packet.view, 16) ||
        !finite_values(packet.projection, 16) || !isfinite(packet.depth_min) || !isfinite(packet.depth_max) ||
        packet.depth_min < 0 || packet.depth_min > packet.depth_max) goto done;
    if (light)
    {
        memcpy(packet.light_direction, &light->direction, sizeof(packet.light_direction));
        memcpy(packet.light_emission, &light->emission, sizeof(packet.light_emission));
        packet.light_angular_radius = light->angularRadius;
        if (!finite_values(packet.light_direction, 3) || !finite_values(packet.light_emission, 3) ||
            !isfinite(packet.light_angular_radius) || packet.light_angular_radius < 0) goto done;
    }
    if (debug)
    {
        if (!(context->flags & FFX_DENOISER_ENABLE_DEBUGGING) || !debug->outputSize.width || !debug->outputSize.height ||
            debug->mode > FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT ||
            !pack_resource(&packet.resources[RR_BRIDGE_RESOURCE_COUNT - 1], &debug->output, 1)) goto done;
        packet.debug_width = debug->outputSize.width;
        packet.debug_height = debug->outputSize.height;
        packet.debug_mode = debug->mode;
        packet.debug_viewport = debug->viewportIndex;
    }
    result = rr_backend_call(&packet);
done:
    return finish_context(context, result);
}

ffxReturnCode_t ffxDispatch(ffxContext *handle, const ffxDispatchDescHeader *desc)
{
    struct rr_context *context;
    uint32_t version, mode;
    struct rr_frame_11 frame;
    ffxDispatchDescDenoiser converted = {0};
    struct { ffxDispatchDescHeader header; FfxApiDenoiserSignal signal; } signals[4] = {0};
    ffxDispatchDescDenoiserDominantLight light = {0};
    const struct rr_legacy_signal *sources[4] = {0};
    const struct FfxApiResource *fused = NULL;
    const struct rr_legacy_light *old_light = NULL;
    const ffxApiHeader *entry;
    ffxApiHeader *tail = &converted.header;
    unsigned int count = 0, i;
    uint32_t types[4];
    float right[3], up[3], forward[3], tangent;
    if (!desc || !valid_chain(desc)) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    if (!(context = lock_context(handle))) return finish(FFX_API_RETURN_ERROR_PARAMETER);
    version = context->version; mode = context->mode;
    unlock_context(context);
    if (!mode) return dispatch_current(handle, desc, 0, NULL);
    if (desc->type != RR_TYPE(0x02)) return finish(unknown_type(desc->type));
    if (version == RR_10)
    {
        const struct rr_frame_10 *old = (const void *)desc;
        memset(&frame, 0, sizeof(frame));
#define COPY_FIELD(field) frame.field = old->field
        COPY_FIELD(header); COPY_FIELD(commandList); COPY_FIELD(linearDepth); COPY_FIELD(motionVectors);
        COPY_FIELD(normals); COPY_FIELD(specularAlbedo); COPY_FIELD(diffuseAlbedo);
        COPY_FIELD(jitterOffsets); COPY_FIELD(cameraPositionDelta); COPY_FIELD(cameraRight);
        COPY_FIELD(cameraUp); COPY_FIELD(cameraForward); COPY_FIELD(cameraAspectRatio);
        COPY_FIELD(cameraNear); COPY_FIELD(cameraFar); COPY_FIELD(cameraFovAngleVertical);
        COPY_FIELD(renderSize); COPY_FIELD(deltaTime); COPY_FIELD(frameIndex); COPY_FIELD(flags);
#undef COPY_FIELD
        frame.motionVectorScale.x = old->motionVectorScale.x;
        frame.motionVectorScale.y = old->motionVectorScale.y;
        /* v2.1 sample: depth=-viewZ; delta=previousViewZ-currentViewZ. */
        frame.motionVectorScale.z = -1;
    }
    else frame = *(const struct rr_frame_11 *)desc;
    for (entry = desc->pNext; entry; entry = entry->pNext)
    {
        if (entry->type == RR_TYPE(0x07))
        {
            if (old_light) return finish(FFX_API_RETURN_ERROR_PARAMETER);
            old_light = (const void *)entry;
        }
        else if (entry->type == RR_TYPE(0x0c) || entry->type == RR_TYPE(0x06))
            return finish(FFX_API_RETURN_NO_PROVIDER);
        else if (entry->type == RR_TYPE(0x03) && mode == 1 && !count)
        {
            const struct rr_legacy_four *old = (const void *)entry;
            sources[0] = &old->directDiffuseRadiance; sources[1] = &old->directSpecularRadiance;
            sources[2] = &old->indirectDiffuseRadiance; sources[3] = &old->indirectSpecularRadiance;
            types[0] = 1; types[1] = 2; types[2] = 4; types[3] = 5; count = 4;
        }
        else if (entry->type == RR_TYPE(0x04) && mode == 2 && !count)
        {
            const struct rr_legacy_two *old = (const void *)entry;
            sources[0] = &old->diffuseRadiance; sources[1] = &old->specularRadiance;
            types[0] = 4; types[1] = 5; count = 2;
        }
        else if (entry->type == RR_TYPE(0x05) && mode == 3 && !count)
        {
            const struct rr_legacy_one *old = (const void *)entry;
            sources[0] = &old->radiance; fused = &old->fusedAlbedo; types[0] = 5; count = 1;
        }
        else return finish(FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE);
    }
    if (!count || !isfinite(frame.cameraAspectRatio) || frame.cameraAspectRatio <= 0 ||
        !isfinite(frame.cameraFovAngleVertical) || frame.cameraFovAngleVertical <= 0 ||
        frame.cameraFovAngleVertical >= 3.14159265f || !isfinite(frame.cameraNear) ||
        !isfinite(frame.cameraFar) || frame.cameraNear <= 0 || frame.cameraFar <= frame.cameraNear)
        return finish(FFX_API_RETURN_ERROR_PARAMETER);
    converted.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    converted.commandList = frame.commandList; converted.linearDepth = frame.linearDepth;
    converted.motionVectors = frame.motionVectors; converted.normals = frame.normals;
    converted.specularAlbedo = frame.specularAlbedo; converted.diffuseAlbedo = frame.diffuseAlbedo;
    converted.motionVectorScale = frame.motionVectorScale; converted.jitterOffsets = frame.jitterOffsets;
    converted.cameraPositionDelta = frame.cameraPositionDelta; converted.renderSize = frame.renderSize;
    converted.frameIndex = frame.frameIndex; converted.flags = frame.flags;
    converted.linearDepthBounds.min = frame.cameraNear; converted.linearDepthBounds.max = frame.cameraFar;
    memcpy(right, &frame.cameraRight, sizeof(right)); memcpy(up, &frame.cameraUp, sizeof(up));
    memcpy(forward, &frame.cameraForward, sizeof(forward));
    if (!finite_values(right, 3) || !finite_values(up, 3) || !finite_values(forward, 3))
        return finish(FFX_API_RETURN_ERROR_PARAMETER);
    /* v2.1 sample's GetDirection() is inverse-view column 2, while its
     * depth is -viewZ. v2.2 instead specifies the looking direction. */
    if (version == RR_10) for (i = 0; i < 3; ++i) forward[i] = -forward[i];
    /* The API basis includes projection axis flips. Positive linear depth runs
     * along the supplied forward vector; translation lives in camera_delta. */
    for (i = 0; i < 3; ++i)
    {
        ((float *)&converted.view)[i * 4] = right[i];
        ((float *)&converted.view)[i * 4 + 1] = up[i];
        ((float *)&converted.view)[i * 4 + 2] = forward[i];
    }
    ((float *)&converted.view)[15] = 1;
    tangent = tanf(frame.cameraFovAngleVertical * 0.5f);
    ((float *)&converted.projection)[0] = 1 / (tangent * frame.cameraAspectRatio);
    ((float *)&converted.projection)[5] = 1 / tangent;
    ((float *)&converted.projection)[10] = frame.cameraFar / (frame.cameraFar - frame.cameraNear);
    ((float *)&converted.projection)[11] = 1;
    ((float *)&converted.projection)[14] = -frame.cameraNear * ((float *)&converted.projection)[10];
    for (i = 0; i < count; ++i)
    {
        static const uint64_t ids[] = {0, FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE,
            FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR, 0,
            FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE, FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR};
        signals[i].header.type = ids[types[i]];
        signals[i].signal.input = sources[i]->input; signals[i].signal.output = sources[i]->output;
        tail->pNext = &signals[i].header; tail = &signals[i].header;
    }
    if (old_light)
    {
        light.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT;
        light.signal.input = old_light->dominantLightVisibility.input;
        light.signal.output = old_light->dominantLightVisibility.output;
        light.direction = old_light->dominantLightDirection; light.emission = old_light->dominantLightEmission;
        /* Old ABI contains no radius: no added penumbra broadening. */
        tail->pNext = &light.header;
    }
    return dispatch_current(handle, &converted.header, mode, fused);
}
