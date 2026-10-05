#include "d3d12_acceleration_structure.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

namespace dxmt::raytracing {
namespace {

constexpr uint32_t kMetalUsageRefit = 1u;
constexpr uint32_t kMetalUsagePreferFastBuild = 2u;
constexpr uint32_t kMetalUsagePreferFastIntersection = 16u;
constexpr uint32_t kMetalUsageMinimizeMemory = 32u;

bool Add(uint64_t a, uint64_t b, uint64_t &result) {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    return false;
  result = a + b;
  return true;
}

bool Multiply(uint64_t a, uint64_t b, uint64_t &result) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a)
    return false;
  result = a * b;
  return true;
}

bool Align(uint64_t value, uint64_t alignment, uint64_t &result) {
  uint64_t sum;
  if (!alignment || (alignment & (alignment - 1)) || !Add(value, alignment - 1, sum))
    return false;
  result = sum & ~(alignment - 1);
  return true;
}

uint32_t Usage(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
  uint32_t usage = 0;
  if (flags & (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE))
    usage |= kMetalUsageRefit;
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
    usage |= kMetalUsagePreferFastBuild;
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE)
    usage |= kMetalUsagePreferFastIntersection;
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY)
    usage |= kMetalUsageMinimizeMemory;
  return usage;
}

/* Resolve `bytes` at `address`. Without a resolver (prebuild queries) only the nullness of the
 * address is checked: the operation must not dereference it. */
bool Resolve(ResolveGPUVA resolve, void *context, uint64_t address, uint64_t bytes,
    BufferLocation &location) {
  if (!address)
    return false;
  if (!resolve)
    return true;
  if (!resolve(context, address, bytes, location))
    return false;
  return location.offset <= std::numeric_limits<uint64_t>::max() - bytes && location.available >= bytes;
}

/* A D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC vertex format. Every format D3D12 allows maps onto
 * a Metal attribute format that reads exactly the components D3D12 uses: two-component formats
 * leave z at 0, and the four-component formats whose last component D3D12 ignores map onto the
 * three-component Metal format, which reads only the first three. */
struct VertexFormat {
  DXGI_FORMAT format;
  WMTAttributeFormat metal;
  uint32_t componentSize;
  uint32_t positionSize;
};

constexpr VertexFormat kVertexFormats[] = {
    {DXGI_FORMAT_R32G32B32_FLOAT, WMTAttributeFormatFloat3, 4, 12},
    {DXGI_FORMAT_R32G32_FLOAT, WMTAttributeFormatFloat2, 4, 8},
    {DXGI_FORMAT_R16G16_FLOAT, WMTAttributeFormatHalf2, 2, 4},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, WMTAttributeFormatHalf3, 2, 6},
    {DXGI_FORMAT_R16G16_SNORM, WMTAttributeFormatShort2Normalized, 2, 4},
    {DXGI_FORMAT_R16G16B16A16_SNORM, WMTAttributeFormatShort3Normalized, 2, 6},
    /* RaytracingTier 1.1 additions. */
    {DXGI_FORMAT_R16G16B16A16_UNORM, WMTAttributeFormatUShort3Normalized, 2, 6},
    {DXGI_FORMAT_R16G16_UNORM, WMTAttributeFormatUShort2Normalized, 2, 4},
    {DXGI_FORMAT_R10G10B10A2_UNORM, WMTAttributeFormatUInt1010102Normalized, 4, 4},
    {DXGI_FORMAT_R8G8B8A8_UNORM, WMTAttributeFormatUChar3Normalized, 1, 3},
    {DXGI_FORMAT_R8G8_UNORM, WMTAttributeFormatUChar2Normalized, 1, 2},
    {DXGI_FORMAT_R8G8B8A8_SNORM, WMTAttributeFormatChar3Normalized, 1, 3},
    {DXGI_FORMAT_R8G8_SNORM, WMTAttributeFormatChar2Normalized, 1, 2},
};

const VertexFormat *FindVertexFormat(DXGI_FORMAT format) {
  for (const auto &entry : kVertexFormats)
    if (entry.format == format)
      return &entry;
  return nullptr;
}

HRESULT ConvertTriangles(const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC &triangles, ResolveGPUVA resolve,
    void *context, Geometry &geometry) {
  const VertexFormat *format = FindVertexFormat(triangles.VertexFormat);
  if (!format)
    return E_INVALIDARG;
  /* Only the low 32 bits of the stride are used. */
  const uint64_t stride = triangles.VertexBuffer.StrideInBytes & 0xffffffffu;
  if ((triangles.VertexBuffer.StartAddress % format->componentSize) || (stride % format->componentSize))
    return E_INVALIDARG;
  geometry.vertexFormat = format->metal;
  geometry.dxgiVertexFormat = triangles.VertexFormat;
  geometry.vertexCount = triangles.VertexCount;
  geometry.indexCount = triangles.IndexCount;
  geometry.positionSize = format->positionSize;
  geometry.stride = stride;

  const bool indexed = triangles.IndexBuffer != 0;
  if (!indexed && triangles.IndexCount)
    return E_INVALIDARG;
  if (indexed) {
    const uint64_t indexSize = triangles.IndexFormat == DXGI_FORMAT_R16_UINT ? 2 :
        triangles.IndexFormat == DXGI_FORMAT_R32_UINT ? 4 : 0;
    uint64_t indexBytes;
    if (!indexSize || (triangles.IndexBuffer % indexSize) || triangles.IndexCount % 3 ||
        !Multiply(triangles.IndexCount, indexSize, indexBytes))
      return E_INVALIDARG;
    geometry.indexFormat = triangles.IndexFormat;
    geometry.primitiveCount = triangles.IndexCount / 3;
    if (geometry.primitiveCount && !Resolve(resolve, context, triangles.IndexBuffer, indexBytes, geometry.indices))
      return E_INVALIDARG;
  } else {
    geometry.primitiveCount = triangles.VertexCount / 3;
  }

  /* A geometry with no triangles is legal: it builds nothing but keeps its GeometryIndex. */
  if (geometry.primitiveCount) {
    uint64_t vertexBytes;
    if (!triangles.VertexCount || !Multiply(triangles.VertexCount - 1, stride, vertexBytes) ||
        !Add(vertexBytes, format->positionSize, vertexBytes) ||
        !Resolve(resolve, context, triangles.VertexBuffer.StartAddress, vertexBytes, geometry.verticesOrBoxes))
      return E_INVALIDARG;
  }
  if (triangles.Transform3x4) {
    if (triangles.Transform3x4 % D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT)
      return E_INVALIDARG;
    geometry.hasTransform = true;
    if (geometry.primitiveCount && !Resolve(resolve, context, triangles.Transform3x4, 48, geometry.transform))
      return E_INVALIDARG;
  }
  return S_OK;
}

HRESULT ConvertAABBs(const D3D12_RAYTRACING_GEOMETRY_AABBS_DESC &aabbs, ResolveGPUVA resolve, void *context,
    Geometry &geometry) {
  const uint64_t stride = aabbs.AABBs.StrideInBytes;
  if ((aabbs.AABBs.StartAddress % D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT) ||
      (stride % D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT) || aabbs.AABBCount > std::numeric_limits<uint32_t>::max())
    return E_INVALIDARG;
  geometry.stride = stride;
  geometry.primitiveCount = aabbs.AABBCount;
  if (!aabbs.AABBCount)
    return S_OK;
  uint64_t bytes;
  if (stride < sizeof(D3D12_RAYTRACING_AABB) || !Multiply(aabbs.AABBCount - 1, stride, bytes) ||
      !Add(bytes, sizeof(D3D12_RAYTRACING_AABB), bytes) ||
      !Resolve(resolve, context, aabbs.AABBs.StartAddress, bytes, geometry.verticesOrBoxes))
    return E_INVALIDARG;
  return S_OK;
}

} // namespace

HRESULT ConvertBuildInputs(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
    ResolveGPUVA resolve, void *context, BuildInputs &result) {
  constexpr auto knownFlags =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS;
  if (inputs.Flags & ~knownFlags)
    return E_INVALIDARG;
  if (inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
      inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL &&
      uint32_t(inputs.Type) != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY)
    return E_INVALIDARG;
  if (inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY &&
      inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS)
    return E_INVALIDARG;

  result = {};
  result.type = inputs.Type;
  result.flags = inputs.Flags;
  result.metalUsage = Usage(inputs.Flags);
  if ((inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE) && !(inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE))
    return E_INVALIDARG;
  if (uint32_t(inputs.Type) == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) {
    const auto &view = reinterpret_cast<const OmmBuildInputs &>(inputs);
    const auto *array = view.pOpacityMicromapArrayDesc;
    if (inputs.NumDescs != 1 || inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY || !array ||
        (array->NumOmmHistogramEntries && !array->pOmmHistogram) || (inputs.Flags & 0xe1))
      return E_INVALIDARG;
    uint64_t count = 0, size = 0;
    for (uint32_t i = 0; i < array->NumOmmHistogramEntries; ++i) {
      const auto entry = array->pOmmHistogram[i];
      if (entry.SubdivisionLevel > 12 || (entry.Format != 1 && entry.Format != 2)) return E_INVALIDARG;
      const uint64_t bytes = (((uint64_t(1) << (2 * entry.SubdivisionLevel)) * uint32_t(entry.Format) + 7) / 8 + 3) & ~3ull;
      uint64_t range;
      if (!Add(count, entry.Count, count) || count > UINT32_MAX ||
          !Multiply(entry.Count, bytes, range) || !Add(size, range, size)) return E_INVALIDARG;
      result.ommHistogram.push_back(entry);
    }
    result.ommCount = count;
    result.ommPackedBytes = size;
    result.ommDescStride = array->PerOmmDescs.StrideInBytes;
    if (!count) return S_OK;
    uint64_t descBytes;
    if ((array->InputBuffer % 128) || (array->PerOmmDescs.StartAddress % 4) ||
        (result.ommDescStride % 4) || result.ommDescStride < 8 ||
        !Multiply(count - 1, result.ommDescStride, descBytes) || !Add(descBytes, 8, descBytes) ||
        !Resolve(resolve, context, array->InputBuffer, 1, result.ommData) ||
        !Resolve(resolve, context, array->PerOmmDescs.StartAddress, descBytes, result.ommDescs)) return E_INVALIDARG;
    return S_OK;
  }

  if (inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
    /* An empty top-level structure is legal; it is never intersected. */
    result.instanceCount = inputs.NumDescs;
    if (!inputs.NumDescs)
      return S_OK;
    const bool pointers = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    result.instanceLayout = pointers ? InstanceLayout::ArrayOfPointers : InstanceLayout::Array;
    /* The pointed-to descriptions live in GPU memory and are read by the GPU prepass. */
    const uint64_t element = pointers ? sizeof(D3D12_GPU_VIRTUAL_ADDRESS) : kD3DInstanceDescriptorSize;
    const uint64_t alignment = pointers ? sizeof(D3D12_GPU_VIRTUAL_ADDRESS)
                                        : D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT;
    uint64_t bytes;
    if ((inputs.InstanceDescs % alignment) || !Multiply(inputs.NumDescs, element, bytes) ||
        !Resolve(resolve, context, inputs.InstanceDescs, bytes, result.instances))
      return E_INVALIDARG;
    return S_OK;
  }

  if (inputs.NumDescs &&
      ((inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY && !inputs.pGeometryDescs) ||
       (inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS && !inputs.ppGeometryDescs)))
    return E_INVALIDARG;

  result.geometries.reserve(inputs.NumDescs);
  D3D12_RAYTRACING_GEOMETRY_TYPE firstType{};
  for (uint32_t i = 0; i < inputs.NumDescs; ++i) {
    const D3D12_RAYTRACING_GEOMETRY_DESC *source = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
        ? &inputs.pGeometryDescs[i] : inputs.ppGeometryDescs[i];
    if (!source || (source->Flags & ~(D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE |
        D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION)))
      return E_INVALIDARG;
    /* Every geometry of one bottom-level structure has the same type. */
    if (i == 0)
      firstType = source->Type;
    else if ((source->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) !=
             (firstType == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS))
      return E_INVALIDARG;

    Geometry geometry{};
    geometry.type = source->Type;
    geometry.flags = source->Flags;
    HRESULT hr = E_INVALIDARG;
    if (source->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES)
      hr = ConvertTriangles(source->Triangles, resolve, context, geometry);
    else if (source->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
      hr = ConvertAABBs(source->AABBs, resolve, context, geometry);
    else if (uint32_t(source->Type) == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
      const auto &omm = reinterpret_cast<const OmmGeometryDesc *>(source)->OmmTriangles;
      if (!omm.pTriangles || !omm.pOmmLinkage) return E_INVALIDARG;
      hr = ConvertTriangles(*omm.pTriangles, resolve, context, geometry);
      if (FAILED(hr)) return hr;
      const auto &link = *omm.pOmmLinkage;
      geometry.ommBase = link.OpacityMicromapBaseLocation;
      if (geometry.primitiveCount) {
        if (!link.OpacityMicromapArray && !link.OpacityMicromapIndexBuffer.StartAddress) return E_INVALIDARG;
        if (link.OpacityMicromapArray && ((link.OpacityMicromapArray % 128) ||
            !Resolve(resolve, context, link.OpacityMicromapArray, 64, geometry.ommArray))) return E_INVALIDARG;
        if (link.OpacityMicromapIndexBuffer.StartAddress) {
          const uint32_t bytes = link.OpacityMicromapIndexFormat == DXGI_FORMAT_R8_UINT ? 1
            : link.OpacityMicromapIndexFormat == DXGI_FORMAT_R16_UINT ? 2
            : link.OpacityMicromapIndexFormat == DXGI_FORMAT_R32_UINT ? 4 : 0;
          const uint64_t stride = link.OpacityMicromapIndexBuffer.StrideInBytes;
          uint64_t range;
          if (!bytes || stride < bytes || stride > UINT32_MAX || (stride % bytes) ||
              (link.OpacityMicromapIndexBuffer.StartAddress % bytes) ||
              !Multiply(geometry.primitiveCount - 1, stride, range) || !Add(range, bytes, range) ||
              !Resolve(resolve, context, link.OpacityMicromapIndexBuffer.StartAddress, range, geometry.ommIndices)) return E_INVALIDARG;
          geometry.ommIndexBytes = bytes;
          geometry.ommIndexStride = stride;
        } else if (link.OpacityMicromapIndexFormat != DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
      }
    }
    if (FAILED(hr))
      return hr;
    result.geometries.push_back(geometry);
  }
  return S_OK;
}

bool CalculatePrebuildInfo(const NativeSizes &native, uint64_t shadowSize, uint64_t *resultDataMaxSize,
    uint64_t *scratchDataSize, uint64_t *updateScratchDataSize) {
  uint64_t withHeader;
  uint64_t withShadow;
  if (!resultDataMaxSize || !scratchDataSize || !updateScratchDataSize ||
      !Add(native.accelerationStructureSize, kHeaderSize, withHeader) ||
      !Add(withHeader, shadowSize, withShadow) ||
      !Align(withShadow, kAccelerationStructureAlignment, *resultDataMaxSize) ||
      !Align(native.buildScratchSize, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, *scratchDataSize) ||
      !Align(native.refitScratchSize, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
          *updateScratchDataSize))
    return false;
  return true;
}

bool CalculatePrepassLayout(uint32_t instanceCount, PrepassLayout &layout) {
  uint64_t converted;
  uint64_t contributions;
  if (!Multiply(instanceCount, kMetalIndirectInstanceDescriptorSize, converted) ||
      !Multiply(instanceCount, sizeof(uint32_t), contributions))
    return false;
  layout = {0, kHeaderSize, converted, contributions};
  return true;
}

HRESULT CreateNativeDescriptor(WMT::Device device, const BuildInputs &inputs,
    WMT::Buffer convertedInstances, NativeDescriptor &result) {
  if (uint32_t(inputs.type) == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) { result = {}; return S_OK; }
  const bool topLevel = inputs.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  const uint32_t geometryCount = topLevel ? 0 : static_cast<uint32_t>(inputs.geometries.size());
  auto descriptor = device.newMTL4AccelerationStructureDescriptor(topLevel, geometryCount);
  if (!descriptor)
    return E_OUTOFMEMORY;
  descriptor.setUsage(inputs.metalUsage);
  if (topLevel) {
    if (!descriptor.setInstances(convertedInstances, 0, inputs.instanceCount))
      return E_INVALIDARG;
  } else {
    for (uint32_t i = 0; i < geometryCount; ++i) {
      const auto &geometry = inputs.geometries[i];
      /* Hardware selects a callback by geometry kind. That callback independently
       * resolves the D3D12 hit group from GPU SBT records and ray contributions. */
      const uint32_t ift_offset =
          geometry.type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS ? 1 : 0;
      bool configured = false;
      if (geometry.type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES || uint32_t(geometry.type) == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
        configured = descriptor.setTriangles(i, geometry.verticesOrBoxes.buffer, geometry.verticesOrBoxes.offset,
            geometry.stride, geometry.indices.buffer, geometry.indices.offset,
            geometry.indexFormat == DXGI_FORMAT_UNKNOWN ? 0xffffffffu
                : geometry.indexFormat == DXGI_FORMAT_R32_UINT ? 1u : 0u,
            geometry.transform.buffer,
            geometry.transform.offset, geometry.primitiveCount, ift_offset, geometry.flags,
            WMTAttributeFormat(geometry.vertexFormat));
      } else if (geometry.type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
        configured = descriptor.setAABBs(i, geometry.verticesOrBoxes.buffer, geometry.verticesOrBoxes.offset,
            geometry.stride, geometry.primitiveCount, ift_offset, geometry.flags);
      }
      if (!configured)
        return E_INVALIDARG;
    }
  }
  const auto sizes = device.accelerationStructureSizes(descriptor);
  // Metal reports a zero refit scratch size for descriptors whose refit needs no temporary
  // storage, and D3D12 accepts that: only the sizes required to create and initially build the
  // structure are mandatory.
  if (!sizes.acceleration_structure_size || !sizes.build_scratch_size)
    return E_FAIL;
  result.descriptor = std::move(descriptor);
  result.sizes = {sizes.acceleration_structure_size, sizes.build_scratch_size, sizes.refit_scratch_size};
  return S_OK;
}

bool PlanShadow(const BuildInputs &inputs, ShadowPlan &plan) {
  plan = {};
  uint64_t cursor = kHeaderSize;
  if (uint32_t(inputs.type) == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) {
    if (!Multiply(inputs.ommCount, sizeof(ShadowGeometry), cursor) ||
        !Add(cursor, inputs.ommPackedBytes, cursor) || !Align(cursor, 16, plan.size)) return false;
    plan.geometries.resize(inputs.ommCount);
    return true;
  }
  if (inputs.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
    uint64_t bytes;
    if (!Multiply(inputs.instanceCount, kD3DInstanceDescriptorSize, bytes))
      return false;
    plan.size = bytes;
    if (inputs.instanceCount)
      plan.jobs.push_back({inputs.instances.address, cursor,
          inputs.instanceLayout == InstanceLayout::Array ? 64u : 8u, 64, 64, inputs.instanceCount,
          inputs.instanceLayout == InstanceLayout::Array ? kShadowJobAddress : kShadowJobPointers, 0});
    return true;
  }
  if (!Add(cursor, inputs.geometries.size() * sizeof(ShadowGeometry), cursor))
    return false;
  auto append = [&](uint64_t source, uint64_t sourceStride, uint32_t bytes, uint32_t count,
                    uint64_t &offset) {
    uint64_t size;
    const uint32_t stride = (bytes + 3u) & ~3u;
    if (!Align(cursor, kShadowAlignment, cursor) || !Multiply(count, stride, size))
      return false;
    offset = cursor;
    if (!Add(cursor, size, cursor))
      return false;
    if (count)
      plan.jobs.push_back({source, offset, sourceStride, bytes, stride, count, kShadowJobAddress, 0});
    return true;
  };
  for (const auto &geometry : inputs.geometries) {
    const bool triangles = geometry.type != D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    ShadowGeometry record{};
    record.type = geometry.type;
    record.flags = geometry.flags;
    record.vertexFormat = geometry.dxgiVertexFormat;
    record.indexFormat = geometry.indexFormat;
    record.vertexCount = geometry.vertexCount;
    record.indexCount = geometry.indexCount;
    record.primitiveCount = geometry.primitiveCount;
    const uint32_t bytes = triangles ? geometry.positionSize : 24;
    record.strideAndTransform = ((bytes + 3u) & ~3u) | (geometry.hasTransform ? kShadowHasTransform : 0);
    // Empty geometries may legally carry inaccessible buffers. Never read those inputs.
    if (!append(geometry.verticesOrBoxes.address, geometry.stride, bytes,
                geometry.primitiveCount ? (triangles ? geometry.vertexCount : geometry.primitiveCount) : 0,
                record.dataOffset))
      return false;
    if (triangles && geometry.indexFormat != DXGI_FORMAT_UNKNOWN &&
        !append(geometry.indices.address, geometry.indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4,
                geometry.indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4,
                geometry.primitiveCount ? geometry.indexCount : 0, record.indexOffset))
      return false;
    if (geometry.hasTransform && !append(geometry.transform.address, 48, 48,
                                        geometry.primitiveCount ? 1 : 0, record.transformOffset))
      return false;
    record.ommArrayAddress = geometry.ommArray.address;
    record.ommIndexBytes = geometry.ommIndexBytes;
    record.ommBase = geometry.ommBase;
    // The shadow owns the linkage indices, not the build input buffer.
    record.ommIndexStride = geometry.ommIndexBytes ? 4 : 0;
    if (geometry.ommIndexBytes && !append(geometry.ommIndices.address, geometry.ommIndexStride,
        geometry.ommIndexBytes, geometry.primitiveCount, record.ommIndexOffset)) return false;
    plan.geometries.push_back(record);
  }
  if (!Align(cursor, kShadowAlignment, cursor))
    return false;
  plan.size = cursor - kHeaderSize;
  return true;
}

StructureInfo DescribeStructure(const BuildInputs &inputs, const ShadowPlan &plan) {
  return {inputs.type, inputs.flags, inputs.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
      ? inputs.instanceCount : uint32_t(inputs.type) == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY ? inputs.ommCount
      : uint32_t(inputs.geometries.size()), plan.size, plan.geometries};
}

bool PlanOmmShadow(const BuildInputs &inputs, const uint8_t *descs, uint64_t bytes, ShadowPlan &plan) {
  if (!PlanShadow(inputs, plan)) return false;
  uint32_t expected[13][2]{}, actual[13][2]{};
  for (const auto &entry : inputs.ommHistogram) expected[entry.SubdivisionLevel][uint32_t(entry.Format) - 1] += entry.Count;
  uint64_t cursor = kHeaderSize + uint64_t(inputs.ommCount) * sizeof(ShadowGeometry);
  for (uint32_t i = 0; i < inputs.ommCount; ++i) {
    const uint64_t offset = uint64_t(i) * inputs.ommDescStride;
    if (offset > bytes || bytes - offset < 8) return false;
    D3D12_RAYTRACING_OPACITY_MICROMAP_DESC desc;
    memcpy(&desc, descs + offset, 8);
    const uint32_t level = desc.SubdivisionLevel, format = desc.Format;
    if (level > 12 || format < 1 || format > 2) return false;
    ++actual[level][format - 1];
    const uint32_t size = ((uint32_t(1) << (2 * level)) * format + 7) / 8;
    if (desc.ByteOffset > inputs.ommData.available || size > inputs.ommData.available - desc.ByteOffset) return false;
    auto &record = plan.geometries[i];
    record.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
    record.vertexFormat = format; record.indexFormat = level; record.vertexCount = size;
    record.dataOffset = cursor;
    const uint32_t padded = (size + 3) & ~3u;
    plan.jobs.push_back({inputs.ommData.address + desc.ByteOffset, cursor, size, size, padded, 1, kShadowJobAddress, 0});
    if (!Add(cursor, padded, cursor)) return false;
  }
  return !memcmp(expected, actual, sizeof(expected)) && cursor <= kHeaderSize + plan.size;
}

bool IsCompatibleUpdate(const StructureInfo &source, const BuildInputs &inputs, const ShadowPlan &plan) {
  constexpr uint32_t mutableFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
  if (!(source.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) ||
      ((uint32_t(source.flags) ^ uint32_t(inputs.flags)) & ~mutableFlags) ||
      source.type != inputs.type || source.descCount != (inputs.type == 0 ? inputs.instanceCount : inputs.geometries.size()) ||
      source.geometries.size() != plan.geometries.size())
    return false;
  for (size_t i = 0; i < plan.geometries.size(); ++i) {
    const auto &a = source.geometries[i];
    const auto &b = plan.geometries[i];
    if (a.type != b.type || a.flags != b.flags || a.vertexFormat != b.vertexFormat ||
        a.indexFormat != b.indexFormat || a.vertexCount != b.vertexCount || a.indexCount != b.indexCount ||
        a.primitiveCount != b.primitiveCount ||
        (a.strideAndTransform & kShadowHasTransform) != (b.strideAndTransform & kShadowHasTransform) ||
        (!(source.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE) && (a.ommArrayAddress != b.ommArrayAddress ||
          a.ommIndexBytes != b.ommIndexBytes || a.ommBase != b.ommBase)))
      return false;
  }
  return true;
}

D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER DriverMatchingIdentifier(uint64_t registryID) {
  D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER result{};
  const GUID driver = {0x44584d54, 0x4153, 0x5348, {0x41, 0x44, 0x4f, 0x57, 0x44, 0x58, 0x52, 1}};
  result.DriverOpaqueGUID = driver;
  memcpy(result.DriverOpaqueVersioningData, &registryID, 8);
  const uint64_t version = kSerializedVersion;
  memcpy(result.DriverOpaqueVersioningData + 8, &version, 8);
  return result;
}

D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS CheckDriverMatchingIdentifier(uint64_t registryID,
    D3D12_SERIALIZED_DATA_TYPE type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier) {
  if (!identifier || type != D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE)
    return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
  const auto expected = DriverMatchingIdentifier(registryID);
  if (memcmp(&identifier->DriverOpaqueGUID, &expected.DriverOpaqueGUID, sizeof(GUID)))
    return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
  if (memcmp(identifier->DriverOpaqueVersioningData, expected.DriverOpaqueVersioningData, 8))
    return D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_TYPE;
  if (memcmp(identifier->DriverOpaqueVersioningData + 8, expected.DriverOpaqueVersioningData + 8, 8))
    return D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
  return D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
}

HRESULT ParseSerialized(const D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER &serialized,
    const AccelerationStructureHeader &header, const uint8_t *geometryData, uint64_t geometryBytes,
    uint64_t available, uint64_t registryID,
    const BufferLocation &destination, DeserializedStructure &result, uint64_t headerOffset) {
  if (available < kSerializedHeaderSize)
    return E_INVALIDARG;
  if (CheckDriverMatchingIdentifier(registryID, D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
          &serialized.DriverMatchingIdentifier) != D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE)
    return E_INVALIDARG;
  const bool blocks = uint32_t(serialized.NumBottomLevelAccelerationStructurePointersAfterHeader >> 32) == UINT32_MAX;
  if (!blocks && (!Multiply(serialized.NumBottomLevelAccelerationStructurePointersAfterHeader, 8, headerOffset) ||
      !Add(headerOffset, kSerializedHeaderSize, headerOffset) || headerOffset > available ||
      available - headerOffset < kHeaderSize))
    return E_INVALIDARG;
  uint64_t ommPointers = 0;
  if (blocks) {
    if (!geometryData || geometryBytes < uint64_t(header.descCount) * sizeof(ShadowGeometry)) return E_INVALIDARG;
    for (uint32_t i = 0; i < header.descCount; ++i) {
      ShadowGeometry geometry; memcpy(&geometry, geometryData + uint64_t(i) * sizeof(geometry), sizeof(geometry));
      ommPointers += geometry.ommArrayAddress != 0;
    }
  }
  if (blocks && (serialized.NumBottomLevelAccelerationStructurePointersAfterHeader != 0xffffffff00000001ull ||
      header.type != 1 || !header.reserved0 || !ommPointers || ommPointers != header.reserved0 - 1 || headerOffset != 72 + ommPointers * 8 ||
      headerOffset > available || available - headerOffset < kHeaderSize)) return E_INVALIDARG;
  if (header.magic != kHeaderMagic || header.type > 2 || header.shadowSize > available - headerOffset - kHeaderSize ||
      header.deserializedSize > destination.available || header.deserializedSize < kHeaderSize + header.shadowSize ||
      serialized.SerializedSizeInBytesIncludingHeader != headerOffset + kHeaderSize + header.shadowSize ||
      serialized.DeserializedSizeInBytes != header.deserializedSize ||
      (!blocks && serialized.NumBottomLevelAccelerationStructurePointersAfterHeader != (header.type == 0 ? header.descCount : 0)))
    return E_INVALIDARG;
  result = {};
  result.shadowSize = header.shadowSize;
  result.deserializedSize = header.deserializedSize;
  result.descCount = header.descCount;
  auto &inputs = result.inputs;
  inputs.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE(header.type);
  inputs.flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(header.flags & ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
  inputs.metalUsage = Usage(inputs.flags);
  auto location = [&](uint64_t offset, uint64_t count, uint64_t stride, BufferLocation &out) {
    uint64_t bytes;
    if (offset < kHeaderSize || !Multiply(count, stride, bytes) || offset > kHeaderSize + header.shadowSize ||
        bytes > kHeaderSize + header.shadowSize - offset)
      return false;
    out = {destination.buffer, destination.offset + offset, bytes, destination.allocationIdentity,
           destination.address + offset};
    return true;
  };
  if (header.type == 0) {
    inputs.instanceCount = header.descCount;
    if (!location(kHeaderSize, header.descCount, 64, inputs.instances))
      return E_INVALIDARG;
  } else if (header.type == 2) {
    if (header.descCount > header.shadowSize / sizeof(ShadowGeometry) ||
        header.descCount > geometryBytes / sizeof(ShadowGeometry)) return E_INVALIDARG;
    inputs.ommCount = header.descCount;
    for (uint32_t i = 0; i < header.descCount; ++i) {
      ShadowGeometry record;
      memcpy(&record, geometryData + uint64_t(i) * sizeof(record), sizeof(record));
      if (record.type != 2 || record.indexFormat > 12 || record.vertexFormat < 1 || record.vertexFormat > 2 ||
          record.vertexCount != (((1u << (2 * record.indexFormat)) * record.vertexFormat + 7) / 8)) return E_INVALIDARG;
      BufferLocation data;
      if (!location(record.dataOffset, 1, record.vertexCount, data)) return E_INVALIDARG;
      result.geometries.push_back(record);
    }
  } else {
    if (header.descCount > header.shadowSize / sizeof(ShadowGeometry) ||
        header.descCount > geometryBytes / sizeof(ShadowGeometry))
      return E_INVALIDARG;
    inputs.geometries.reserve(header.descCount);
    result.geometries.reserve(header.descCount);
    for (uint32_t i = 0; i < header.descCount; ++i) {
      ShadowGeometry record;
      memcpy(&record, geometryData + i * sizeof(record), sizeof(record));
      Geometry geometry{};
      geometry.type = D3D12_RAYTRACING_GEOMETRY_TYPE(record.type);
      geometry.flags = D3D12_RAYTRACING_GEOMETRY_FLAGS(record.flags);
      geometry.primitiveCount = record.primitiveCount;
      geometry.vertexCount = record.vertexCount;
      geometry.indexCount = record.indexCount;
      geometry.dxgiVertexFormat = DXGI_FORMAT(record.vertexFormat);
      geometry.indexFormat = DXGI_FORMAT(record.indexFormat);
      geometry.stride = record.strideAndTransform & ~kShadowHasTransform;
      geometry.hasTransform = record.strideAndTransform & kShadowHasTransform;
      if (record.type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES || record.type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
        const auto *format = FindVertexFormat(geometry.dxgiVertexFormat);
        if (!format)
          return E_INVALIDARG;
        geometry.vertexFormat = format->metal;
        geometry.positionSize = format->positionSize;
        if (geometry.indexFormat != DXGI_FORMAT_UNKNOWN &&
            !location(record.indexOffset, geometry.primitiveCount ? record.indexCount : 0, 4, geometry.indices))
          return E_INVALIDARG;
        // Shadow index elements use a 4-byte stride; R16 is packed by the GPU deserialize kernel.
        if (geometry.indexFormat == DXGI_FORMAT_R16_UINT)
          geometry.indexFormat = DXGI_FORMAT_R32_UINT;
        if (record.type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
          geometry.ommArray.address = record.ommArrayAddress;
          geometry.ommIndexBytes = record.ommIndexBytes;
          geometry.ommIndexStride = record.ommIndexStride;
          geometry.ommBase = record.ommBase;
          if (record.ommIndexBytes && !location(record.ommIndexOffset, geometry.primitiveCount,
              record.ommIndexStride, geometry.ommIndices)) return E_INVALIDARG;
        }
      } else if (record.type != D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
        return E_INVALIDARG;
      if (!location(record.dataOffset, geometry.primitiveCount ? (record.type != 1 ? record.vertexCount : record.primitiveCount) : 0,
                    geometry.stride, geometry.verticesOrBoxes) ||
          (geometry.hasTransform && !location(record.transformOffset, geometry.primitiveCount ? 1 : 0, 48, geometry.transform)))
        return E_INVALIDARG;
      inputs.geometries.push_back(geometry);
      result.geometries.push_back(record);
    }
  }
  return S_OK;
}

} // namespace dxmt::raytracing
