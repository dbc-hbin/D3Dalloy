#pragma once

#include "d3d12.h"
#include "Metal.hpp"
#include <cstdint>
#include <vector>

namespace dxmt::raytracing {

/* Every acceleration structure dxmt builds carries this 64-byte header at its D3D12 GPU
 * virtual address. The AS prepass kernel (rt_as_prepass.metal) writes it on the GPU before the
 * build/refit/copy is encoded, so it can never race the CPU. Ray-tracing shaders read the
 * native structure through it: the resource id is what a dispatch binds, and the contribution
 * address is the per-instance array the hit-group index comes from. */
struct AccelerationStructureHeader {
  uint64_t resourceID;
  uint64_t contributionsAddress;
  uint64_t reserved[6];
};

static_assert(sizeof(AccelerationStructureHeader) == 64, "acceleration-structure header layout");

inline constexpr uint64_t kHeaderSize = sizeof(AccelerationStructureHeader);
inline constexpr uint64_t kAccelerationStructureAlignment =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
inline constexpr uint64_t kD3DInstanceDescriptorSize = 64;
inline constexpr uint64_t kMetalIndirectInstanceDescriptorSize = 72;

struct NativeSizes {
  uint64_t accelerationStructureSize;
  uint64_t buildScratchSize;
  uint64_t refitScratchSize;
};

/* A resolved D3D12 GPU virtual address range inside one allocation. The allocation pointer
 * doubles as the identity used to key the device's acceleration-structure sidecars. */
struct BufferLocation {
  WMT::Buffer buffer;
  uint64_t offset = 0;
  uint64_t available = 0;
  const void *allocationIdentity = nullptr;
};

using ResolveGPUVA = bool (*)(void *context, D3D12_GPU_VIRTUAL_ADDRESS address,
    uint64_t requiredBytes, BufferLocation &location);

struct Geometry {
  D3D12_RAYTRACING_GEOMETRY_TYPE type{};
  D3D12_RAYTRACING_GEOMETRY_FLAGS flags{};
  BufferLocation verticesOrBoxes{};
  BufferLocation indices{};
  BufferLocation transform{};
  uint64_t primitiveCount = 0;
  uint64_t stride = 0;
  DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
};

struct BuildInputs {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type{};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags{};
  std::vector<Geometry> geometries;
  BufferLocation instances{};
  uint32_t instanceCount = 0;
  uint32_t metalUsage = 0;
};

/* Identity of the acceleration structure stored at a D3D12 address: the allocation it lives in
 * plus the byte offset inside it. */
struct ArenaKey {
  const void *allocationIdentity;
  uint64_t offset;

  bool operator==(const ArenaKey &) const = default;
};

struct PrepassLayout {
  uint64_t headerOffset;
  uint64_t headerSize;
  uint64_t convertedInstanceSize;
  uint64_t contributionSize;
};

struct NativeDescriptor {
  WMT::Reference<WMT::AccelerationStructureDescriptor> descriptor;
  NativeSizes sizes{};
};

/* Validate and resolve the application's build description. Vertex/index/instance addresses are
 * resolved through `resolve` so the caller decides what "valid GPU address" means. */
HRESULT ConvertBuildInputs(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
    ResolveGPUVA resolve, void *context, BuildInputs &result);

HRESULT CreateNativeDescriptor(WMT::Device device, const BuildInputs &inputs,
    WMT::Buffer convertedInstances, NativeDescriptor &result);

/* D3D12 sizes: the native structure plus the header, rounded up to the D3D12 alignment. */
bool CalculatePrebuildInfo(const NativeSizes &native, uint64_t *resultDataMaxSize,
    uint64_t *scratchDataSize, uint64_t *updateScratchDataSize);

bool CalculatePrepassLayout(uint32_t instanceCount, PrepassLayout &layout);

} // namespace dxmt::raytracing
