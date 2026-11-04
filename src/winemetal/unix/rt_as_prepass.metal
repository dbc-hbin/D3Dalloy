/*
 * D3Dalloy acceleration-structure prepass kernels.
 *
 * These run on the application's own command buffer immediately before a Metal
 * acceleration-structure build/refit/copy is encoded, so everything they write is ordered
 * against GPU work (a rebuild at the same D3D12 address can never clobber the header of an
 * in-flight frame from the CPU).
 *
 * Ordered GPU jobs:
 *  1. d3dalloy_as_prepass - publish the native identifiers in the 64-byte ASHeader
 *     that our own ray-tracing shaders dereference (the D3D12
 *     acceleration-structure GPUVA *is* the address of this header), and, for top-level
 *     builds, translate the application's D3D12_RAYTRACING_INSTANCE_DESC array into the
 *     MTLIndirectAccelerationStructureInstanceDescriptor array Metal's TLAS build consumes
 *     plus the per-instance contribution words our ray-tracing shaders read.
 *  2. d3dalloy_as_shadow - preserve self-contained build inputs and query metadata.
 *  3. d3dalloy_as_postbuild / serialize / deserialize / visualize - consume that shadow
 *     in application GPU command order, including public TLAS relocation pointers.
 *
 * D3D12_RAYTRACING_INSTANCE_DESC (64 B) is NOT layout-compatible with any Metal instance
 * descriptor: Metal puts options @48, mask @52, IFT offset @56, userID @60 and the
 * acceleration-structure resource id @64, while D3D puts InstanceID|InstanceMask @48,
 * InstanceContributionToHitGroupIndex|Flags @52 and a 64-bit acceleration-structure GPUVA
 * @56. The Metal side therefore needs a real translation; the resource id is recovered by
 * loading the d3dalloy header at that GPUVA (every acceleration-structure address d3dalloy builds
 * into carries one).
 */

#include <metal_stdlib>
using namespace metal;
struct ASHeader {
  ulong resourceID, contributionsAddress;
  uint magic, type, flags, descCount;
  ulong shadowSize, currentSize;
  uint nativeCompactedSize, reserved0;
  ulong deserializedSize;
};
struct ShadowGeometry {
  uint type, flags, vertexFormat, indexFormat, vertexCount, indexCount, primitiveCount, strideAndTransform;
  ulong dataOffset, indexOffset, transformOffset, reserved;
  ulong ommArrayAddress, ommIndexOffset;
  uint ommIndexStride, ommIndexBytes, ommBase, ommReserved;
};
struct ShadowJob {
  ulong source, destinationOffset, sourceStride;
  uint elementSize, destinationStride, count, mode, flags;
};
struct ShadowMetadata {
  uint magic, type, flags, descCount;
  ulong shadowSize, currentSize;
  uint compacted, reserved0;
  ulong deserializedSize;
};
struct ShadowParams {
  ulong asAddress, metadataSource;
  uint jobCount, metadataMode;
  ShadowMetadata metadata;
};

// All vector accesses are proved aligned and stay within the source payload.
// The last partial word and the R16 -> R32 padding never read past an element.
inline void copy_shadow_linear(device uchar *dst, device const uchar *src,
    ulong bytes, ulong paddedBytes, uint tid, uint lanes) {
  const uint width = ((ulong(dst) | ulong(src)) & 15) == 0 ? 16
      : ((ulong(dst) | ulong(src)) & 3) == 0 ? 4 : 1;
  for (ulong offset = ulong(tid) * width; offset < paddedBytes; offset += ulong(lanes) * width) {
    if (src && offset + width <= bytes && width == 16)
      *((device uint4 *)(dst + offset)) = *((device const uint4 *)(src + offset));
    else if (src && offset + width <= bytes && width == 4)
      *((device uint *)(dst + offset)) = *((device const uint *)(src + offset));
    else
      for (uint byte = 0; byte < width && offset + byte < paddedBytes; ++byte)
        dst[offset + byte] = src && offset + byte < bytes ? src[offset + byte] : 0;
  }
}

inline void copy_shadow_element(device uchar *dst, device const uchar *src,
    uint bytes, uint stride) {
  if (bytes == 2 && stride == 4 && (ulong(dst) & 3) == 0 && (ulong(src) & 1) == 0) {
    *((device uint *)dst) = src ? *((device const ushort *)src) : 0;
    return;
  }
  uint offset = 0;
  if (src && ((ulong(dst) | ulong(src)) & 15) == 0)
    for (; offset + 16 <= bytes; offset += 16)
      *((device uint4 *)(dst + offset)) = *((device const uint4 *)(src + offset));
  if (src && ((ulong(dst + offset) | ulong(src + offset)) & 3) == 0)
    for (; offset + 4 <= bytes; offset += 4)
      *((device uint *)(dst + offset)) = *((device const uint *)(src + offset));
  for (; offset < bytes; ++offset) dst[offset] = src ? src[offset] : 0;
  for (; offset < stride; ++offset) dst[offset] = 0;
}

kernel void d3dalloy_as_shadow(constant ShadowParams &p [[buffer(0)]],
    device const ShadowGeometry *geometries [[buffer(1)]], device const ShadowJob *jobs [[buffer(2)]],
    uint2 tid [[thread_position_in_grid]], uint2 grid [[threads_per_grid]]) {
  device ASHeader *dst = (device ASHeader *)p.asAddress;
  if (p.metadataMode == 1) {
    device const ASHeader *src = (device const ASHeader *)p.metadataSource;
    // resourceID and contribution pointer belong to the destination sidecar.
    copy_shadow_linear((device uchar *)dst + 16, (device const uchar *)src + 16,
        48 + src->shadowSize, 48 + src->shadowSize, tid.x, grid.x);
  } else {
    if (all(tid == 0))
      *((device ShadowMetadata *)((device uchar *)dst + 16)) = p.metadata;
    for (ulong i = ulong(tid.y) * grid.x + tid.x; i < p.metadata.descCount && p.metadata.type != 0;
         i += ulong(grid.x) * grid.y)
      ((device ShadowGeometry *)((device uchar *)dst + 64))[i] = geometries[i];
    // Each row owns its jobs' disjoint shadow regions; x spans bytes or elements within that job.
    for (ulong j = tid.y; j < p.jobCount; j += grid.y) {
      const ShadowJob job = jobs[j];
      device uchar *output = (device uchar *)dst + job.destinationOffset;
      if (job.mode == 0 && (job.destinationStride == job.elementSize || job.count == 1) &&
          job.sourceStride == job.elementSize) {
        copy_shadow_linear(output, (device const uchar *)job.source,
            ulong(job.count) * job.elementSize, ulong(job.count) * job.destinationStride, tid.x, grid.x);
        continue;
      }
      for (ulong element = tid.x; element < job.count; element += grid.x) {
        device const uchar *src = job.mode == 1
          ? (device const uchar *)((device const ulong *)job.source)[element]
          : (device const uchar *)(job.source + (ulong)element * job.sourceStride);
        copy_shadow_element(output + element * job.destinationStride, src, job.elementSize, job.destinationStride);
      }
    }
  }
}

struct PostbuildParams { ulong asAddress, destinationOrMarker; uint operation, batchCount; };
kernel void d3dalloy_as_postbuild(constant PostbuildParams *entries [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
  if (tid >= max(entries[0].batchCount, 1u)) return;
  constant PostbuildParams &p = entries[tid];
  device ASHeader *h = (device ASHeader *)p.asAddress;
  device ulong *out = (device ulong *)p.destinationOrMarker;
  const ulong compacted = (ulong(h->nativeCompactedSize) + 64 + h->shadowSize + 255) & ~255ul;
  if (p.operation == 0) out[0] = h->type == 2 ? h->currentSize : compacted;
  else if (p.operation == 1) out[0] = 8 + h->shadowSize + (h->type == 2 ? 352 + ulong(h->descCount) * 8
    : h->type == 1 && h->reserved0 ? ulong(h->descCount) * 80 : 0);
  else if (p.operation == 2) {
    const ulong ommPointers = h->type == 1 && h->reserved0 ? h->reserved0 - 1 : 0;
    out[0] = 56 + (h->type == 0 ? ulong(h->descCount) * 8 : ommPointers ? 16 + ommPointers * 8 : 0) + 64 + h->shadowSize;
    out[1] = h->type == 0 ? h->descCount : ommPointers ? 1 : 0;
  } else if (p.operation == 3) out[0] = h->currentSize;
  else {
    if (p.operation & 0x100) h->currentSize = compacted;
    if (p.operation & 0x200) {
      uint marker = uint(p.destinationOrMarker);
      if (!marker) {
        // Deserialization can replace original array pointers with null on the GPU.
        device const ShadowGeometry *g = (device const ShadowGeometry *)((device const uchar *)h + 64);
        marker = 1;
        for (uint i = 0; i < h->descCount; ++i) marker += g[i].ommArrayAddress != 0;
      }
      h->reserved0 = marker;
    }
  }
}
struct SerializeParams { ulong source, destination; uint identifier[8]; };
kernel void d3dalloy_as_serialize(constant SerializeParams &p [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
  device const ASHeader *h = (device const ASHeader *)p.source;
  const ulong pointers = h->type == 0 ? h->descCount : 0;
  const ulong ommPointers = h->type == 1 && h->reserved0 ? h->reserved0 - 1 : 0;
  const bool blocks = ommPointers != 0;
  const ulong offset = 56 + (blocks ? 16 + ommPointers * 8 : pointers * 8);
  device uchar *dst = (device uchar *)p.destination;
  if (tid == 0) {
    for (uint i = 0; i < 8; ++i) ((device uint *)dst)[i] = p.identifier[i];
    ((device ulong *)dst)[4] = offset + 64 + h->shadowSize;
    ((device ulong *)dst)[5] = h->deserializedSize;
    ((device ulong *)dst)[6] = blocks ? 0xffffffff00000001ul : pointers;
    if (blocks) { ((device ulong *)dst)[7] = 0; ((device ulong *)dst)[8] = ommPointers; }
  }
  if (blocks && tid == 0) {
    device const ShadowGeometry *g = (device const ShadowGeometry *)(p.source + 64);
    ulong pointer = 0;
    for (uint i = 0; i < h->descCount; ++i)
      if (g[i].ommArrayAddress) ((device ulong *)(dst + 72))[pointer++] = g[i].ommArrayAddress;
  }
  for (ulong i = tid; i < pointers; i += 4096)
    ((device ulong *)(dst + 56))[i] = ((device const ulong *)(p.source + 64 + i * 64))[7];
  for (ulong i = tid; i < 64 + h->shadowSize; i += 4096)
    dst[offset + i] = ((device const uchar *)h)[i];
}
struct DeserializeParams { ulong source, destination, currentSize; };
kernel void d3dalloy_as_deserialize(constant DeserializeParams &p [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
  device const ulong *src = (device const ulong *)p.source;
  const bool blocks = uint(src[6] >> 32) == 0xffffffffu;
  const ulong pointers = blocks ? src[8] : src[6];
  device const ASHeader *h = (device const ASHeader *)(p.source + 56 + (blocks ? 16 : 0) + pointers * 8);
  device ASHeader *dst = (device ASHeader *)p.destination;
  for (ulong i = 16 + tid; i < 64 + h->shadowSize; i += 4096) {
    if ((i >= 40 && i < 48) || (!blocks && pointers && i >= 64 && (i - 64) % 64 >= 56) ||
        (blocks && i >= 64 && i < 64 + ulong(h->descCount) * sizeof(ShadowGeometry) &&
         (i - 64) % sizeof(ShadowGeometry) >= 64 && (i - 64) % sizeof(ShadowGeometry) < 72)) continue;
    ((device uchar *)dst)[i] = ((device const uchar *)h)[i];
  }
  // TLAS pointers are the public relocation table, not the stale copies in the shadow.
  if (blocks && tid == 0) {
    device const ShadowGeometry *g = (device const ShadowGeometry *)((device const uchar *)h + 64);
    ulong pointer = 0;
    for (uint i = 0; i < h->descCount; ++i)
      ((device ShadowGeometry *)(p.destination + 64))[i].ommArrayAddress = g[i].ommArrayAddress ? src[9 + pointer++] : 0;
  }
  if (!blocks) for (ulong i = tid; i < pointers; i += 4096)
    ((device ulong *)(p.destination + 64 + i * 64))[7] = src[7 + i];
  if (tid == 0) dst->currentSize = p.currentSize;
}
struct VisualizeParams { ulong source, destination; };
kernel void d3dalloy_as_visualize(constant VisualizeParams &p [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
  device const ASHeader *h = (device const ASHeader *)p.source;
  device uchar *dst = (device uchar *)p.destination;
  if (tid == 0) { ((device uint *)dst)[0] = h->type; ((device uint *)dst)[1] = h->descCount; }
  // BLAS geometry descriptors are written by a separate invocation after this payload copy.
  for (ulong i = tid; i < h->shadowSize; i += 4096)
    dst[8 + i] = ((device const uchar *)h)[64 + i];
}
kernel void d3dalloy_as_visualize_descriptors(constant VisualizeParams &p [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
  device const ASHeader *h = (device const ASHeader *)p.source;
  if (h->type == 2) {
    device const ShadowGeometry *g = (device const ShadowGeometry *)(p.source + 64);
    const ulong histogram = p.destination + 8 + h->shadowSize + 40;
    const ulong descriptors = histogram + 312;
    if (tid == 0) {
      device uint *array = (device uint *)(p.destination + 8);
      array[0] = 26; array[1] = 0;
      ((device ulong *)array)[1] = histogram;
      ((device ulong *)array)[2] = p.destination + 8;
      ((device ulong *)array)[3] = descriptors;
      ((device ulong *)array)[4] = 8;
      ((device uint *)p.destination)[1] = 1;
    }
    for (uint bin = tid; bin < 26; bin += 4096) {
      uint count = 0;
      for (uint i = 0; i < h->descCount; ++i) count += g[i].indexFormat == bin / 2 && g[i].vertexFormat == bin % 2 + 1;
      device uint *entry = (device uint *)(histogram + bin * 12);
      entry[0] = count; entry[1] = bin / 2; entry[2] = bin % 2 + 1;
    }
    for (uint i = tid; i < h->descCount; i += 4096) {
      device uint *entry = (device uint *)(descriptors + ulong(i) * 8);
      entry[0] = uint(g[i].dataOffset - 64); entry[1] = g[i].indexFormat | (g[i].vertexFormat << 16);
    }
    return;
  }
  if (h->type != 1) return;
  device const ShadowGeometry *g = (device const ShadowGeometry *)(p.source + 64);
  for (uint i = tid; i < h->descCount; i += 4096) {
    device uint *d = (device uint *)(p.destination + 8 + ulong(i) * 56);
    d[0] = g[i].type; d[1] = g[i].flags;
    for (uint k = 2; k < 14; ++k) d[k] = 0;
    if (g[i].type != 1) {
      device uint *triangles = d + 2;
      if (g[i].type == 2) {
        const ulong extra = p.destination + 8 + h->shadowSize + ulong(i) * 80;
        ((device ulong *)d)[1] = extra; ((device ulong *)d)[2] = extra + 48;
        triangles = (device uint *)extra;
        device ulong *link = (device ulong *)(extra + 48);
        link[0] = g[i].ommIndexBytes ? p.destination + g[i].ommIndexOffset - 56 : 0;
        link[1] = g[i].ommIndexStride;
        link[2] = (ulong(g[i].ommBase) << 32) | (g[i].ommIndexBytes == 1 ? 62 : g[i].ommIndexBytes == 2 ? 57 : g[i].ommIndexBytes == 4 ? 42 : 0);
        link[3] = g[i].ommArrayAddress;
      }
      ((device ulong *)triangles)[0] = g[i].strideAndTransform & 0x80000000u ? p.destination + g[i].transformOffset - 56 : 0;
      triangles[2] = g[i].indexFormat == 57 ? 42 : g[i].indexFormat;
      triangles[3] = g[i].vertexFormat; triangles[4] = g[i].indexCount; triangles[5] = g[i].vertexCount;
      ((device ulong *)triangles)[3] = g[i].indexFormat != 0 ? p.destination + g[i].indexOffset - 56 : 0;
      ((device ulong *)triangles)[4] = p.destination + g[i].dataOffset - 56;
      ((device ulong *)triangles)[5] = g[i].strideAndTransform & 0x7fffffffu;
    } else {
      ((device ulong *)d)[1] = g[i].primitiveCount;
      ((device ulong *)d)[2] = p.destination + g[i].dataOffset - 56;
      ((device ulong *)d)[3] = 24;
    }
  }
}

struct D3DALLOYASPrepassParams
{
    /* MTLAccelerationStructure.gpuResourceID written into the header (opaque resource id). */
    ulong asResourceID;
    /* GPU address of the header's instance-contribution array (0 when there is none). */
    ulong contributionsAddress;
    /* Byte offset of instance 0 inside the D3D12 instance-descriptor buffer. */
    uint srcInstanceOffset;
    /* D3D12 instance-descriptor stride (D3D12_RAYTRACING_INSTANCE_DESC = 64). */
    uint srcInstanceStride;
    /* Metal instance-descriptor stride (MTLIndirectAccelerationStructureInstanceDescriptor = 72). */
    uint dstInstanceStride;
    /* Number of instances to translate. */
    uint instanceCount;
    /* 1: also translate instances into dstInstances / contributions. */
    uint convertInstances;
    /* 32-bit version token the prepass also publishes into liveMirror[liveMirrorSlot]. */
    uint versionToken;
    /* Slot in the shared version-mirror page this token is published into. */
    uint liveMirrorSlot;
    /* WMTASPrepassWriteShadow in winemetal.h: publish build/refit TLAS shadow and metadata. */
    uint flags;
    ShadowMetadata metadata;
};

constant uint kASPrepassWriteShadow = 1;
static_assert(sizeof(D3DALLOYASPrepassParams) == 96, "AS prepass ABI");

kernel void
d3dalloy_as_prepass(
    device const uchar *srcInstances [[buffer(0)]], device uchar *dstInstances [[buffer(1)]],
    device ulong *header [[buffer(2)]], device uint *contributions [[buffer(3)]],
    constant D3DALLOYASPrepassParams &params [[buffer(4)]],
    device uint *liveMirror [[buffer(5)]], uint tid [[thread_position_in_grid]]
) {
  if (tid == 0) {
    header[0] = params.asResourceID;
    header[1] = params.contributionsAddress;
    if (params.flags & kASPrepassWriteShadow)
      *((device ShadowMetadata *)((device uchar *)header + 16)) = params.metadata;

    /*
     * The version this build publishes, written beside the header in the same instruction
     * stream. The host reads it back through a shared page once this submission completes, so
     * it learns which native structure the GPU actually holds for the D3D12 address, without
     * reading the (private) acceleration-structure allocation itself.
     */
    liveMirror[params.liveMirrorSlot] = params.versionToken;
  }
  if (params.convertInstances == 0 || tid >= params.instanceCount)
    return;

  /* convertInstances 1: srcInstances is the D3D12_RAYTRACING_INSTANCE_DESC array.
   * convertInstances 2 (D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS): srcInstances is an array of
   * D3D12 GPU virtual addresses, which are Metal device addresses, one per instance desc. */
  device const uchar *instance;
  if (params.convertInstances == 2) {
    device const ulong *pointers = (device const ulong *)(srcInstances + params.srcInstanceOffset);
    instance = (device const uchar *)(pointers[tid]);
  } else {
    instance = srcInstances + (ulong)params.srcInstanceOffset + (ulong)tid * (ulong)params.srcInstanceStride;
  }
  const ulong dst = (ulong)tid * (ulong)params.dstInstanceStride;

  // One bit-exact instance snapshot feeds the D3D shadow, Metal descriptor and
  // contribution array, including GPU-produced arrays and arrays of pointers.
  uint4 row0 = 0, row1 = 0, row2 = 0, packed = 0;
  if (instance) {
    if ((ulong(instance) & 15) == 0) {
      device const uint4 *words = (device const uint4 *)instance;
      row0 = words[0]; row1 = words[1]; row2 = words[2]; packed = words[3];
    } else {
      device const uint *words = (device const uint *)instance;
      row0 = uint4(words[0], words[1], words[2], words[3]);
      row1 = uint4(words[4], words[5], words[6], words[7]);
      row2 = uint4(words[8], words[9], words[10], words[11]);
      packed = uint4(words[12], words[13], words[14], words[15]);
    }
  }
  if (params.flags & kASPrepassWriteShadow) {
    device uint4 *shadow = (device uint4 *)((device uchar *)header + 64 + ulong(tid) * 64);
    shadow[0] = row0; shadow[1] = row1; shadow[2] = row2; shadow[3] = packed;
  }

  /* D3D12 float Transform[3][4] is row-major 3x4; Metal's PackedFloat4x3 stores FOUR columns
   * of three floats, columns[k] = {T[0][k], T[1][k], T[2][k]} (k = 0..3), so the D3D
   * translation T[*][3] lands in columns[3]. */
  device uint *out = (device uint *)(dstInstances + dst);
  out[0] = row0.x;
  out[1] = row1.x;
  out[2] = row2.x;
  out[3] = row0.y;
  out[4] = row1.y;
  out[5] = row2.y;
  out[6] = row0.z;
  out[7] = row1.z;
  out[8] = row2.z;
  out[9] = row0.w;
  out[10] = row1.w;
  out[11] = row2.w;

  /* word0: InstanceID:24 | InstanceMask:8 ; word1: Contribution:24 | Flags:8 (little endian). */
  const uint instance_id = packed.x & 0xFFFFFFu;
  const uint instance_mask = packed.x >> 24;
  const uint contribution = packed.y & 0xFFFFFFu;
  const uint flags = packed.y >> 24;

  device uint *tail = (device uint *)(dstInstances + dst + 48);
  /* MTLAccelerationStructureInstanceOptions is a straight bit copy of the D3D flags
   * (0x1 cull-disable, 0x2 front-CCW, 0x4 opaque, 0x8 non-opaque). */
  tail[0] = flags & 15u; // OMM controls are software flags, not Metal instance options.
  tail[1] = instance_mask;
  /* Geometry selects the shared triangle/bounding-box callback. D3D12 instance
   * contributions remain in the separate GPU array, never in hardware IFT slots. */
  tail[2] = 0;
  tail[3] = instance_id;

  /* AccelerationStructure field: D3D12_GPU_VIRTUAL_ADDRESS of the bottom-level structure. Every
   * acceleration-structure address d3dalloy builds into holds the d3dalloy header, whose first 8 bytes
   * are the Metal resource id the TLAS build needs. */
  const ulong as_address = ulong(packed.z) | (ulong(packed.w) << 32);
  device ulong *resource_id = (device ulong *)(dstInstances + dst + 64);
  /* A NULL bottom-level address is a legal, inactive instance: resource id 0 is never
   * intersected (and the instance still occupies its InstanceIndex). */
  resource_id[0] = as_address ? ((device const ulong *)as_address)[0] : 0ul;

  contributions[tid] = contribution;
}
