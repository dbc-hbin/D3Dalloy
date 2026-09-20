#include "d3d12_acceleration_structure.hpp"

#include <cstdio>
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
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
    usage |= kMetalUsageRefit;
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
    usage |= kMetalUsagePreferFastBuild;
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE)
    usage |= kMetalUsagePreferFastIntersection;
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY)
    usage |= kMetalUsageMinimizeMemory;
  return usage;
}

bool Resolve(ResolveGPUVA resolve, void *context, uint64_t address, uint64_t bytes,
    BufferLocation &location) {
  if (!address || !resolve || !resolve(context, address, bytes, location))
    return false;
  return location.offset <= std::numeric_limits<uint64_t>::max() - bytes && location.available >= bytes;
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
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
  if (inputs.Flags & ~knownFlags)
    return E_INVALIDARG;
  if (inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
      inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
    return E_INVALIDARG;

  result = {};
  result.type = inputs.Type;
  result.flags = inputs.Flags;
  result.metalUsage = Usage(inputs.Flags);

  if (inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
    if (inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY || !inputs.NumDescs)
      return E_INVALIDARG;
    uint64_t bytes;
    if (!Multiply(inputs.NumDescs, kD3DInstanceDescriptorSize, bytes) ||
        !Resolve(resolve, context, inputs.InstanceDescs, bytes, result.instances))
      return E_INVALIDARG;
    result.instanceCount = inputs.NumDescs;
    return S_OK;
  }

  if (!inputs.NumDescs || (inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY &&
      inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS))
    return E_INVALIDARG;
  if ((inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY && !inputs.pGeometryDescs) ||
      (inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS && !inputs.ppGeometryDescs))
    return E_INVALIDARG;

  result.geometries.reserve(inputs.NumDescs);
  for (uint32_t i = 0; i < inputs.NumDescs; ++i) {
    const D3D12_RAYTRACING_GEOMETRY_DESC *source = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
        ? &inputs.pGeometryDescs[i] : inputs.ppGeometryDescs[i];
    if (!source || (source->Flags & ~(D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE |
        D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION)))
      return E_INVALIDARG;

    Geometry geometry{};
    geometry.type = source->Type;
    geometry.flags = source->Flags;
    if (source->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
      const auto &triangles = source->Triangles;
      if (!triangles.VertexCount || !triangles.VertexBuffer.StartAddress ||
          triangles.VertexBuffer.StrideInBytes < 12 || triangles.VertexFormat != DXGI_FORMAT_R32G32B32_FLOAT)
        return E_INVALIDARG;
      uint64_t vertexBytes;
      if (!Multiply(triangles.VertexCount - 1, triangles.VertexBuffer.StrideInBytes, vertexBytes) ||
          !Add(vertexBytes, 12, vertexBytes) || !Resolve(resolve, context,
              triangles.VertexBuffer.StartAddress, vertexBytes, geometry.verticesOrBoxes))
        return E_INVALIDARG;
      geometry.stride = triangles.VertexBuffer.StrideInBytes;
      if (triangles.IndexCount) {
        const uint64_t indexSize = triangles.IndexFormat == DXGI_FORMAT_R16_UINT ? 2 :
            triangles.IndexFormat == DXGI_FORMAT_R32_UINT ? 4 : 0;
        uint64_t indexBytes;
        if (!indexSize || triangles.IndexCount % 3 ||
            !Multiply(triangles.IndexCount, indexSize, indexBytes) ||
            !Resolve(resolve, context, triangles.IndexBuffer, indexBytes, geometry.indices))
          return E_INVALIDARG;
        geometry.indexFormat = triangles.IndexFormat;
        geometry.primitiveCount = triangles.IndexCount / 3;
      } else {
        if (triangles.VertexCount % 3)
          return E_INVALIDARG;
        geometry.primitiveCount = triangles.VertexCount / 3;
      }
      if (triangles.Transform3x4 && !Resolve(resolve, context, triangles.Transform3x4, 48, geometry.transform))
        return E_INVALIDARG;
    } else if (source->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
      const auto &aabbs = source->AABBs;
      if (!aabbs.AABBCount || aabbs.AABBs.StrideInBytes < 24)
        return E_INVALIDARG;
      uint64_t bytes;
      if (!Multiply(aabbs.AABBCount - 1, aabbs.AABBs.StrideInBytes, bytes) || !Add(bytes, 24, bytes) ||
          !Resolve(resolve, context, aabbs.AABBs.StartAddress, bytes, geometry.verticesOrBoxes))
        return E_INVALIDARG;
      geometry.stride = aabbs.AABBs.StrideInBytes;
      geometry.primitiveCount = aabbs.AABBCount;
    } else {
      return E_INVALIDARG;
    }
    result.geometries.push_back(geometry);
  }
  return S_OK;
}

bool CalculatePrebuildInfo(const NativeSizes &native, uint64_t *resultDataMaxSize,
    uint64_t *scratchDataSize, uint64_t *updateScratchDataSize) {
  uint64_t withHeader;
  if (!resultDataMaxSize || !scratchDataSize || !updateScratchDataSize ||
      !Add(native.accelerationStructureSize, kHeaderSize, withHeader) ||
      !Align(withHeader, kAccelerationStructureAlignment, *resultDataMaxSize) ||
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
  const bool topLevel = inputs.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  const uint32_t geometryCount = topLevel ? 0 : static_cast<uint32_t>(inputs.geometries.size());
  auto descriptor = device.newAccelerationStructureDescriptor(topLevel, geometryCount);
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
      if (geometry.type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
        configured = descriptor.setTriangles(i, geometry.verticesOrBoxes.buffer, geometry.verticesOrBoxes.offset,
            geometry.stride, geometry.indices.buffer, geometry.indices.offset,
            geometry.indexFormat == DXGI_FORMAT_R32_UINT ? 1 : 0, geometry.transform.buffer,
            geometry.transform.offset, geometry.primitiveCount, ift_offset, geometry.flags);
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

} // namespace dxmt::raytracing
