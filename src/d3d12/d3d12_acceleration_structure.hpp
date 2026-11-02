#pragma once

#include "d3d12_interfaces_ext.hpp"
#include "Metal4.hpp"
#include "d3dalloy_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "DXBCParser/RtasSerialized.h"
#include <cstdint>
#include <vector>

namespace d3dalloy::raytracing {

/* Every acceleration structure d3dalloy builds carries this 64-byte header at its D3D12 GPU
 * virtual address, followed by the shadow copy of its build inputs. Everything is written on the
 * GPU, in the pass that builds/updates/copies the structure, so it can never race the CPU
 * (rt_as_prepass.metal). Ray-tracing shaders read the native structure through the first two
 * words: the resource id is what a dispatch binds, and the contribution address is the
 * per-instance array the hit-group index comes from. The rest describes the D3D12 structure for
 * the postbuild queries, serialization and visualization, which therefore never need the CPU to
 * know what the GPU built. */
struct AccelerationStructureHeader {
  uint64_t resourceID;
  uint64_t contributionsAddress;
  uint32_t magic;
  /* D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE. */
  uint32_t type;
  /* D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS of the build. */
  uint32_t flags;
  /* NumDescs: instances (top level) or geometries (bottom level). */
  uint32_t descCount;
  uint64_t shadowSize;
  /* POSTBUILD_INFO_CURRENT_SIZE: the prebuild size, or the compacted size after compaction. */
  uint64_t currentSize;
  /* Low32 bits of Metal's eight-byte compacted native size write. The ordered
   * postbuild pass restores reserved0 before any OMM metadata consumer. */
  uint32_t nativeCompactedSize;
  /* Nonzero for OMM BLAS: the ordered postbuild pass publishes array-reference count + 1. */
  uint32_t reserved0;
  /* The prebuild size of the uncompacted structure: what a deserialization rebuilds. */
  uint64_t deserializedSize;
};

static_assert(sizeof(AccelerationStructureHeader) == 64, "acceleration-structure header layout");

inline constexpr uint32_t kHeaderMagic = 0x53415844u; /* "DXAS" */
inline constexpr uint64_t kHeaderSize = sizeof(AccelerationStructureHeader);
inline constexpr uint64_t kShadowAlignment = 16;
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
  /* The D3D12 GPU virtual address that was resolved (a Metal device address). */
  uint64_t address = 0;
  Rc<Allocation> lookup_owner;
};

/* One geometry of a bottom-level shadow copy (rt_as_prepass.metal: D3DALLOYASShadowGeometry). The
 * offsets are relative to the acceleration-structure address, so a verbatim copy of the header and
 * shadow describes itself wherever it is copied to. */
struct ShadowGeometry {
  uint32_t type;
  uint32_t flags;
  /* DXGI formats of the vertex positions and the indices (triangles only). */
  uint32_t vertexFormat;
  uint32_t indexFormat;
  uint32_t vertexCount;
  uint32_t indexCount;
  /* Triangles or AABBs the geometry builds. */
  uint32_t primitiveCount;
  /* Packed bytes per vertex position (or per AABB); bit 31: a Transform3x4 is present. */
  uint32_t strideAndTransform;
  uint64_t dataOffset;
  uint64_t indexOffset;
  uint64_t transformOffset;
  uint64_t reserved;
  uint64_t ommArrayAddress, ommIndexOffset;
  uint32_t ommIndexStride, ommIndexBytes, ommBase, ommReserved;
};
static_assert(sizeof(ShadowGeometry) == 96, "shadow geometry layout");

inline constexpr uint32_t kShadowHasTransform = 0x80000000u;

/* One strided copy the shadow kernel performs (rt_as_prepass.metal: D3DALLOYASShadowJob). Element i
 * copies `elementSize` bytes from the source to `destinationOffset + i * destinationStride` of the
 * acceleration structure and zero-fills the rest of the destination stride. */
struct ShadowJob {
  uint64_t source;
  uint64_t destinationOffset;
  uint64_t sourceStride;
  uint32_t elementSize;
  uint32_t destinationStride;
  uint32_t count;
  uint32_t mode;
  uint32_t reserved;
};
static_assert(sizeof(ShadowJob) == 48, "shadow job layout");

enum : uint32_t {
  /* `source` is the GPU virtual address of element 0. */
  kShadowJobAddress = 0,
  /* `source` is the GPU virtual address of an array of GPU virtual addresses, one per element. */
  kShadowJobPointers = 1,
};

/* The shadow kernel's metadata source (D3DALLOYASShadowParams::metadataMode). */
enum : uint32_t {
  /* The header words come from the kernel parameters (build, update). */
  kShadowMetadataParams = 0,
  /* The header words and the whole shadow are copied from another acceleration structure
   * (clone, compaction). */
  kShadowMetadataCopy = 1,
};

/* Header words 16..63 as the shadow kernel writes them. */
struct ShadowMetadata {
  uint32_t magic;
  uint32_t type;
  uint32_t flags;
  uint32_t descCount;
  uint64_t shadowSize;
  uint64_t currentSize;
  uint32_t nativeCompactedSize;
  uint32_t reserved0;
  uint64_t deserializedSize;
};
static_assert(sizeof(ShadowMetadata) == 48, "shadow metadata layout");

struct ShadowParams {
  uint64_t asAddress;
  uint64_t metadataSource;
  uint32_t jobCount;
  uint32_t metadataMode;
  ShadowMetadata metadata;
};
static_assert(sizeof(ShadowParams) == 72, "shadow params layout");

/* d3dalloy_as_postbuild operations: the D3D12 postbuild info types, plus the step that turns the
 * Metal compacted size a compaction wrote into the header into the D3D12 current size. */
inline constexpr uint32_t kPostbuildFinalizeCompactedHeader = 0x100;
inline constexpr uint32_t kPostbuildRestoreOmmHeader = 0x101;

struct PostbuildParams {
  uint64_t asAddress;
  uint64_t destination;
  uint32_t operation;
  uint32_t reserved;
};
static_assert(sizeof(PostbuildParams) == 24, "postbuild params layout");

struct SerializeParams {
  uint64_t source;
  uint64_t destination;
  D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER identifier;
};
static_assert(sizeof(SerializeParams) == 48, "serialize params layout");

struct DeserializeParams {
  uint64_t source;
  uint64_t destination;
  uint64_t currentSize;
};
static_assert(sizeof(DeserializeParams) == 24, "deserialize params layout");

/* Every helper kernel walks its work with a grid-stride loop over this fixed grid, so the CPU
 * never has to know how much data the GPU-side header describes. */
inline constexpr uint32_t kHelperThreads = 4096;
inline constexpr uint32_t kHelperThreadgroup = 256;

/* What the shadow copy of one build holds, and the copies that produce it. */
struct ShadowPlan {
  uint64_t size = 0;
  std::vector<ShadowGeometry> geometries;
  std::vector<ShadowJob> jobs;
};

/* D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER; the serialized blob continues with
 * the bottom-level pointers (top level), a copy of the d3dalloy header and the shadow. */
inline constexpr uint64_t kSerializedHeaderSize = 56;
static_assert(sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER) == kSerializedHeaderSize);
static_assert(sizeof(D3D12_RAYTRACING_GEOMETRY_DESC) == 56);
/* Version of the serialized/shadow layout, part of the driver matching identifier. */
inline constexpr uint64_t kSerializedVersion = 2;

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
  /* WMTAttributeFormat of the vertex positions (triangles only). */
  uint32_t vertexFormat = 0;
  /* The D3D12 description the shadow copy records (triangles only). */
  DXGI_FORMAT dxgiVertexFormat = DXGI_FORMAT_UNKNOWN;
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  /* Bytes of one vertex position: the packed stride of the shadow copy. */
  uint32_t positionSize = 0;
  bool hasTransform = false;
  BufferLocation ommArray{}, ommIndices{};
  uint32_t ommIndexBytes = 0, ommBase = 0, ommIndexStride = 0;
};

/* D3D12_ELEMENTS_LAYOUT of a top-level build's instance descriptions. */
enum class InstanceLayout : uint32_t {
  Array = 1,
  ArrayOfPointers = 2,
};

struct BuildInputs {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type{};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags{};
  std::vector<Geometry> geometries;
  BufferLocation instances{};
  InstanceLayout instanceLayout = InstanceLayout::Array;
  uint32_t instanceCount = 0;
  uint32_t metalUsage = 0;
  BufferLocation ommData{}, ommDescs{};
  uint64_t ommDescStride = 0, ommPackedBytes = 0;
  uint32_t ommCount = 0;
  std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> ommHistogram;
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
  WMT::Reference<WMT::MTL4AccelerationStructureDescriptor> descriptor;
  NativeSizes sizes{};
};

/* Validate and resolve the application's build description. Vertex/index/instance addresses are
 * resolved through `resolve` so the caller decides what "valid GPU address" means. A null
 * `resolve` validates the description only: GetRaytracingAccelerationStructurePrebuildInfo never
 * dereferences GPU addresses (they may be dummy non-zero values), so only their nullness counts. */
HRESULT ConvertBuildInputs(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
    ResolveGPUVA resolve, void *context, BuildInputs &result);

HRESULT CreateNativeDescriptor(WMT::Device device, const BuildInputs &inputs,
    WMT::Buffer convertedInstances, NativeDescriptor &result);

/* D3D12 sizes: the native structure plus the header and the shadow copy, rounded up to the D3D12
 * alignment. */
bool CalculatePrebuildInfo(const NativeSizes &native, uint64_t shadowSize, uint64_t *resultDataMaxSize,
    uint64_t *scratchDataSize, uint64_t *updateScratchDataSize);

bool CalculatePrepassLayout(uint32_t instanceCount, PrepassLayout &layout);

/* Lay out the shadow copy of `inputs`. Without resolved addresses (prebuild) only the size and the
 * geometry records are meaningful. */
bool PlanShadow(const BuildInputs &inputs, ShadowPlan &plan);
bool PlanOmmShadow(const BuildInputs &inputs, const uint8_t *descs, uint64_t bytes, ShadowPlan &plan);

/* What the CPU knows about the structure one published version holds: enough to validate an
 * update against it, to size copies of it and to rebuild it. */
struct StructureInfo {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type{};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags{};
  /* Instances (top level) or geometries (bottom level). */
  uint32_t descCount = 0;
  uint64_t shadowSize = 0;
  std::vector<ShadowGeometry> geometries;
};

StructureInfo DescribeStructure(const BuildInputs &inputs, const ShadowPlan &plan);

/* The update rules of D3D12 (Acceleration structure update constraints): only positions,
 * transforms, AABBs and instance descriptions may change. */
bool IsCompatibleUpdate(const StructureInfo &source, const BuildInputs &inputs, const ShadowPlan &plan);


D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER DriverMatchingIdentifier(uint64_t registryID);

D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS CheckDriverMatchingIdentifier(uint64_t registryID,
    D3D12_SERIALIZED_DATA_TYPE type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier);

/* A serialized structure, validated and turned back into build inputs whose data lives in the
 * shadow copy the deserialization writes at `destination`. */
struct DeserializedStructure {
  BuildInputs inputs;
  uint64_t shadowSize = 0;
  uint64_t deserializedSize = 0;
  uint32_t descCount = 0;
  std::vector<ShadowGeometry> geometries;
};

/* `layout` comes from d3dalloy_rtas_serialized_prefix/_block over the same header bytes. */
HRESULT ParseSerialized(const D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER &serialized,
    const D3DalloyRtasSerializedLayout &layout, const AccelerationStructureHeader &header, const uint8_t *geometryData,
    uint64_t geometryBytes, uint64_t registryID, const BufferLocation &destination, DeserializedStructure &result);

} // namespace d3dalloy::raytracing

namespace d3dalloy {
struct RTASComputePipelines {
  WMT::Reference<WMT::ComputePipelineState> prepass;
  WMT::Reference<WMT::ComputePipelineState> shadow, postbuild, serialize, deserialize, visualize, visualizeDescriptors;
};
}
