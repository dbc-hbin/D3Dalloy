// Narrow allocation proof for overlapping MetalFX preprocessing commands.
#pragma once

#import <Metal/Metal.h>

namespace d3dalloy::metalfx {

inline id<MTLTexture> textureAllocationRoot(id<MTLTexture> texture) {
    while (texture.parentTexture) texture = texture.parentTexture;
    return texture;
}

inline bool independentTextureAllocation(id<MTLTexture> texture) {
#if !defined(__arm64__) && !defined(__aarch64__)
    // Remote views can have private storage but share another GPU's texture.
    // This API is inapplicable on Apple Silicon.
    if (texture.remoteStorageTexture) return false;
#endif
    // Shared/imported, buffer-backed and sparse storage can alias without a
    // common texture parent. They need the caller's original ordering.
    return texture.storageMode == MTLStorageModePrivate && !texture.buffer &&
           !texture.iosurface && !texture.shareable && !texture.isSparse;
}

inline bool texturesDisjoint(id<MTLTexture> first, id<MTLTexture> second) {
    if (!first || !second) return true;
    first = textureAllocationRoot(first);
    second = textureAllocationRoot(second);
    if (first == second || !independentTextureAllocation(first) ||
        !independentTextureAllocation(second)) return false;
    id<MTLHeap> a = first.heap, b = second.heap;
    // Distinct owned private allocations cannot share device memory; ordinary
    // texture views were reduced to their allocation above.
    if (!a || !b || a != b) return true;
    if (a.type != MTLHeapTypePlacement) return false;
    const NSUInteger aOffset = first.heapOffset, bOffset = second.heapOffset;
    const NSUInteger aSize = first.allocatedSize, bSize = second.allocatedSize;
    if (!aSize || !bSize || aOffset > a.size || aSize > a.size - aOffset ||
        bOffset > a.size || bSize > a.size - bOffset) return false;
    return aOffset <= bOffset ? aSize <= bOffset - aOffset : bSize <= aOffset - bOffset;
}

} // namespace d3dalloy::metalfx
