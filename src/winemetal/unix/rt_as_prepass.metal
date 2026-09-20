/*
 * DXMT acceleration-structure prepass kernels.
 *
 * These run on the application's own command buffer immediately before a Metal
 * acceleration-structure build/refit/copy is encoded, so everything they write is ordered
 * against GPU work (a rebuild at the same D3D12 address can never clobber the header of an
 * in-flight frame from the CPU).
 *
 * Two jobs:
 *  1. dxmt_as_prepass - write the dxmt acceleration-structure GPU header (DXMTASHeader,
 *     64 bytes) that our own ray-tracing shaders dereference (the D3D12
 *     acceleration-structure GPUVA *is* the address of this header), and, for top-level
 *     builds, translate the application's D3D12_RAYTRACING_INSTANCE_DESC array into the
 *     MTLIndirectAccelerationStructureInstanceDescriptor array Metal's TLAS build consumes
 *     plus the per-instance contribution words our ray-tracing shaders read.
 *  2. dxmt_as_write_u64 - emit a postbuild-info value into an application buffer without
 *     assuming anything about its alignment.
 *
 * D3D12_RAYTRACING_INSTANCE_DESC (64 B) is NOT layout-compatible with any Metal instance
 * descriptor: Metal puts options @48, mask @52, IFT offset @56, userID @60 and the
 * acceleration-structure resource id @64, while D3D puts InstanceID|InstanceMask @48,
 * InstanceContributionToHitGroupIndex|Flags @52 and a 64-bit acceleration-structure GPUVA
 * @56. The Metal side therefore needs a real translation; the resource id is recovered by
 * loading the dxmt header at that GPUVA (every acceleration-structure address dxmt builds
 * into carries one).
 */

#include <metal_stdlib>
using namespace metal;

struct DXMTASPrepassParams
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
};

/* dxmt acceleration-structure GPU header: 64 bytes living at the D3D12 GPU virtual address of
 * every acceleration structure dxmt builds. It is the only extra state dxmt keeps, and it is
 * written on the GPU by the prepass so it is always ordered against the build/refit/copy that
 * publishes it. Ray-tracing shaders read the native structure through this header. */
struct DXMTASHeader
{
    /* MTLAccelerationStructure.gpuResourceID written into the header (opaque resource id). */
    ulong resourceID;
    /* GPU address of the header's instance-contribution array (0 when there is none). */
    ulong contributionsAddress;
    ulong reserved[6];
};

#define DXMT_AS_HEADER_QWORDS (sizeof(struct DXMTASHeader) / sizeof(ulong))

kernel void
dxmt_as_prepass(
    device const uchar *srcInstances [[buffer(0)]], device uchar *dstInstances [[buffer(1)]],
    device ulong *header [[buffer(2)]], device uint *contributions [[buffer(3)]],
    constant DXMTASPrepassParams &params [[buffer(4)]],
    device uint *liveMirror [[buffer(5)]], uint tid [[thread_position_in_grid]]
) {
  if (tid == 0) {
    header[0] = params.asResourceID;
    header[1] = params.contributionsAddress;
    for (uint k = 2; k < DXMT_AS_HEADER_QWORDS; ++k)
      header[k] = 0;
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

  const ulong src = (ulong)params.srcInstanceOffset + (ulong)tid * (ulong)params.srcInstanceStride;
  const ulong dst = (ulong)tid * (ulong)params.dstInstanceStride;

  /* D3D12 float Transform[3][4] is row-major 3x4; Metal's PackedFloat4x3 stores FOUR columns
   * of three floats, columns[k] = {T[0][k], T[1][k], T[2][k]} (k = 0..3), so the D3D
   * translation T[*][3] lands in columns[3]. */
  device const float *tf = (device const float *)(srcInstances + src);
  device float *out = (device float *)(dstInstances + dst);
  out[0] = tf[0];
  out[1] = tf[4];
  out[2] = tf[8];
  out[3] = tf[1];
  out[4] = tf[5];
  out[5] = tf[9];
  out[6] = tf[2];
  out[7] = tf[6];
  out[8] = tf[10];
  out[9] = tf[3];
  out[10] = tf[7];
  out[11] = tf[11];

  /* word0: InstanceID:24 | InstanceMask:8 ; word1: Contribution:24 | Flags:8 (little endian). */
  device const uint *words = (device const uint *)(srcInstances + src + 48);
  const uint instance_id = words[0] & 0xFFFFFFu;
  const uint instance_mask = words[0] >> 24;
  const uint contribution = words[1] & 0xFFFFFFu;
  const uint flags = words[1] >> 24;

  device uint *tail = (device uint *)(dstInstances + dst + 48);
  /* MTLAccelerationStructureInstanceOptions is a straight bit copy of the D3D flags
   * (0x1 cull-disable, 0x2 front-CCW, 0x4 opaque, 0x8 non-opaque). */
  tail[0] = flags;
  tail[1] = instance_mask;
  /* Geometry selects the shared triangle/bounding-box callback. D3D12 instance
   * contributions remain in the separate GPU array, never in hardware IFT slots. */
  tail[2] = 0;
  tail[3] = instance_id;

  /* AccelerationStructure field: D3D12_GPU_VIRTUAL_ADDRESS of the bottom-level structure. Every
   * acceleration-structure address dxmt builds into holds the dxmt header, whose first 8 bytes
   * are the Metal resource id the TLAS build needs. */
  device const ulong *as_address = (device const ulong *)(srcInstances + src + 56);
  device const ulong *blas_header = (device const ulong *)(as_address[0]);
  device ulong *resource_id = (device ulong *)(dstInstances + dst + 64);
  resource_id[0] = blas_header[0];

  contributions[tid] = contribution;
}

kernel void
dxmt_as_write_u64(
    device uchar *dst [[buffer(0)]], constant ulong &value [[buffer(1)]],
    constant uint &byteOffset [[buffer(2)]], uint tid [[thread_position_in_grid]]
) {
  if (tid != 0)
    return;
  for (uint k = 0; k < 8; ++k)
    dst[byteOffset + k] = (uchar)((value >> (k * 8u)) & 0xFFu);
}

/* Metal reports its private compacted byte count. D3D12's compacted AS allocation also has to
 * contain the dxmt header and satisfy the AS alignment. Run this after ending the AS encoder
 * that wrote the raw value. */
kernel void
dxmt_as_finalize_compacted_size(
    device uchar *dst [[buffer(0)]], constant uint &byteOffset [[buffer(1)]],
    uint tid [[thread_position_in_grid]]
) {
  if (tid != 0)
    return;
  device ulong *value = (device ulong *)(dst + byteOffset);
  value[0] = (value[0] + (ulong)sizeof(struct DXMTASHeader) + 255ul) & ~255ul;
}