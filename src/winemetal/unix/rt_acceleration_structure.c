#import <Foundation/Foundation.h>
#import <Metal/Metal.h>


#include "rt_acceleration_structure.h"

@interface DXMTASDescriptor : NSObject
@property(nonatomic, retain) MTLAccelerationStructureDescriptor *metal;
@property(nonatomic, retain) NSMutableArray<MTLAccelerationStructureGeometryDescriptor *> *geometries;
/* Stands in for the vertex / box / instance buffer of a D3D12 input that names no primitives:
 * Metal requires a non-nil buffer even when the primitive count is 0. Never read. */
@property(nonatomic, retain) id<MTLBuffer> placeholder;
@end
@implementation DXMTASDescriptor
- (void)dealloc { [_metal release]; [_geometries release]; [_placeholder release]; [super dealloc]; }
@end

@interface DXMTASSidecar : NSObject
@property(nonatomic, retain) id<MTLAccelerationStructure> accelerationStructure;
@property(nonatomic, retain) id<MTLBuffer> instances;
@property(nonatomic, retain) id<MTLBuffer> contributions;
@property(nonatomic) uint64_t nativeSize;
@property(nonatomic) uint64_t instanceCount;
/* Old generations remain resident while recordings/submissions retain this sidecar. */
@property(nonatomic, retain) NSMutableArray *generations;
@end
@implementation DXMTASSidecar
- (void)dealloc {
  [_accelerationStructure release];
  [_instances release];
  [_contributions release];
  [_generations release];
  [super dealloc];
}
@end

static inline id dxmt_obj(dxmt_mtl_handle handle) { return (id)(uintptr_t)handle; }
static inline dxmt_mtl_handle dxmt_handle(id obj) { return (dxmt_mtl_handle)(uintptr_t)obj; }
static inline DXMTASDescriptor *dxmt_desc(dxmt_mtl_handle handle) { return (DXMTASDescriptor *)dxmt_obj(handle); }
static inline DXMTASSidecar *dxmt_sidecar(dxmt_mtl_handle handle) { return (DXMTASSidecar *)dxmt_obj(handle); }

static void dxmt_geometry_flags(MTLAccelerationStructureGeometryDescriptor *geometry, uint32_t flags) {
  geometry.opaque = (flags & 1u) != 0;
  geometry.allowDuplicateIntersectionFunctionInvocation = (flags & 2u) == 0;
}

dxmt_mtl_handle dxmt_as_descriptor_new(dxmt_mtl_handle device, uint32_t top_level, uint32_t geometry_count) {
  if (!device)
    return 0;
  DXMTASDescriptor *result = [[DXMTASDescriptor alloc] init];
  id<MTLBuffer> placeholder = [(id<MTLDevice>)dxmt_obj(device) newBufferWithLength:256
      options:MTLResourceStorageModePrivate];
  result.placeholder = placeholder;
  [placeholder release];
  if (!result.placeholder) {
    [result release];
    return 0;
  }
  if (top_level) {
    result.metal = [MTLInstanceAccelerationStructureDescriptor descriptor];
  } else {
    MTLPrimitiveAccelerationStructureDescriptor *primitive =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    result.geometries = [NSMutableArray arrayWithCapacity:geometry_count];
    for (uint32_t i = 0; i < geometry_count; ++i)
      [result.geometries addObject:(MTLAccelerationStructureGeometryDescriptor *)[NSNull null]];
    if (!geometry_count) {
      /* D3D12 allows a bottom-level structure with no geometry; Metal requires at least one
       * geometry descriptor, so the empty structure is one geometry with no triangles. */
      MTLAccelerationStructureTriangleGeometryDescriptor *empty =
          [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
      empty.vertexBuffer = result.placeholder;
      empty.vertexStride = 12;
      empty.triangleCount = 0;
      [result.geometries addObject:empty];
    }
    primitive.geometryDescriptors = result.geometries;
    result.metal = primitive;
  }
  return dxmt_handle(result);
}

void dxmt_as_descriptor_set_usage(dxmt_mtl_handle descriptor, uint32_t usage) {
  dxmt_desc(descriptor).metal.usage = (MTLAccelerationStructureUsage)usage;
}

static int dxmt_set_geometry(DXMTASDescriptor *owner, uint32_t index,
    MTLAccelerationStructureGeometryDescriptor *geometry) {
  if (!owner || !owner.geometries || index >= owner.geometries.count)
    return 0;
  [owner.geometries replaceObjectAtIndex:index withObject:geometry];
  ((MTLPrimitiveAccelerationStructureDescriptor *)owner.metal).geometryDescriptors = owner.geometries;
  return 1;
}

int dxmt_as_descriptor_set_triangles(dxmt_mtl_handle descriptor, uint32_t index,
    dxmt_mtl_handle vertices, uint64_t vertex_offset, uint64_t vertex_stride,
    dxmt_mtl_handle indices, uint64_t index_offset, uint32_t index_type,
    dxmt_mtl_handle transform, uint64_t transform_offset, uint64_t triangle_count,
    uint32_t intersection_function_table_offset, uint32_t geometry_flags, uint32_t vertex_format) {
  DXMTASDescriptor *owner = dxmt_desc(descriptor);
  if (!owner || !vertex_format || vertex_offset > NSUIntegerMax ||
      vertex_stride > NSUIntegerMax || index_offset > NSUIntegerMax || transform_offset > NSUIntegerMax ||
      triangle_count > NSUIntegerMax)
    return 0;
  MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
      [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
  /* A geometry with no triangles is legal in D3D12 and still occupies its GeometryIndex. A size
   * query (prebuild info) has no buffers at all; the sizes depend only on the counts. */
  geometry.vertexBuffer = vertices ? (id<MTLBuffer>)dxmt_obj(vertices) : (triangle_count ? nil : owner.placeholder);
  geometry.vertexBufferOffset = vertices ? (NSUInteger)vertex_offset : 0;
  geometry.vertexStride = (NSUInteger)vertex_stride;
  /* WMTAttributeFormat values are MTLAttributeFormat values. */
  geometry.vertexFormat = (MTLAttributeFormat)vertex_format;
  geometry.triangleCount = (NSUInteger)triangle_count;
  if (index_type != 0xffffffffu && triangle_count) {
    /* index_type 0xffffffff: not indexed. A size query has no index buffer yet. */
    geometry.indexBuffer = indices ? (id<MTLBuffer>)dxmt_obj(indices) : nil;
    geometry.indexBufferOffset = indices ? (NSUInteger)index_offset : 0;
    geometry.indexType = index_type ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
  }
  if (transform) {
    geometry.transformationMatrixBuffer = (id<MTLBuffer>)dxmt_obj(transform);
    geometry.transformationMatrixBufferOffset = (NSUInteger)transform_offset;
    /* D3D12 Transform3x4 is a row-major 3x4 matrix. */
    geometry.transformationMatrixLayout = MTLMatrixLayoutRowMajor;
  }
  geometry.intersectionFunctionTableOffset = intersection_function_table_offset;
  dxmt_geometry_flags(geometry, geometry_flags);
  return dxmt_set_geometry(owner, index, geometry);
}

int dxmt_as_descriptor_set_aabbs(dxmt_mtl_handle descriptor, uint32_t index,
    dxmt_mtl_handle boxes, uint64_t box_offset, uint64_t box_stride,
    uint64_t box_count, uint32_t intersection_function_table_offset, uint32_t geometry_flags) {
  DXMTASDescriptor *owner = dxmt_desc(descriptor);
  if (!owner || box_offset > NSUIntegerMax || box_stride > NSUIntegerMax ||
      box_count > NSUIntegerMax)
    return 0;
  MTLAccelerationStructureBoundingBoxGeometryDescriptor *geometry =
      [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
  geometry.boundingBoxBuffer = boxes ? (id<MTLBuffer>)dxmt_obj(boxes) : (box_count ? nil : owner.placeholder);
  geometry.boundingBoxBufferOffset = boxes ? (NSUInteger)box_offset : 0;
  geometry.boundingBoxStride = (NSUInteger)box_stride;
  geometry.boundingBoxCount = (NSUInteger)box_count;
  geometry.intersectionFunctionTableOffset = intersection_function_table_offset;
  dxmt_geometry_flags(geometry, geometry_flags);

  return dxmt_set_geometry(owner, index, geometry);
}

int dxmt_as_descriptor_set_instances(dxmt_mtl_handle descriptor,
    dxmt_mtl_handle converted_instances, uint64_t converted_offset, uint64_t instance_count) {
  DXMTASDescriptor *owner = dxmt_desc(descriptor);
  if (!owner || ![owner.metal isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]] ||
      converted_offset > NSUIntegerMax || instance_count > NSUIntegerMax)
    return 0;
  MTLInstanceAccelerationStructureDescriptor *instances =
      (MTLInstanceAccelerationStructureDescriptor *)owner.metal;
  /* An empty top-level structure is legal in D3D12; Metal still needs a buffer. */
  instances.instanceDescriptorBuffer =
      converted_instances ? (id<MTLBuffer>)dxmt_obj(converted_instances) : owner.placeholder;
  instances.instanceDescriptorBufferOffset = converted_instances ? (NSUInteger)converted_offset : 0;
  instances.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
  instances.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
  instances.instanceCount = (NSUInteger)instance_count;
  return 1;
}

struct dxmt_as_sizes dxmt_as_get_sizes(dxmt_mtl_handle device, dxmt_mtl_handle descriptor) {
  MTLAccelerationStructureSizes sizes = [(id<MTLDevice>)dxmt_obj(device)
      accelerationStructureSizesWithDescriptor:dxmt_desc(descriptor).metal];
  return (struct dxmt_as_sizes){sizes.accelerationStructureSize, sizes.buildScratchBufferSize,
      sizes.refitScratchBufferSize};
}

dxmt_mtl_handle dxmt_as_sidecar_new(dxmt_mtl_handle device, uint64_t native_size,
    uint64_t instance_count) {
  if (!device || native_size > NSUIntegerMax || instance_count > NSUIntegerMax / 4)
    return 0;
  id<MTLDevice> metal_device = (id<MTLDevice>)dxmt_obj(device);
  DXMTASSidecar *sidecar = [[DXMTASSidecar alloc] init];
  sidecar.instanceCount = instance_count;
  if (!native_size)
    return dxmt_handle(sidecar);
  id<MTLAccelerationStructure> native =
      [metal_device newAccelerationStructureWithSize:(NSUInteger)native_size];
  sidecar.accelerationStructure = native;
  sidecar.nativeSize = native_size;
  [native release];
  if (!sidecar.accelerationStructure) { [sidecar release]; return 0; }
  if (instance_count) {
    if (instance_count > NSUIntegerMax / sizeof(MTLIndirectAccelerationStructureInstanceDescriptor)) {
      [sidecar release]; return 0;
    }
    id<MTLBuffer> instances = [metal_device
        newBufferWithLength:(NSUInteger)instance_count * sizeof(MTLIndirectAccelerationStructureInstanceDescriptor)
        options:MTLResourceStorageModeShared];
    id<MTLBuffer> contributions = [metal_device
        newBufferWithLength:(NSUInteger)instance_count * sizeof(uint32_t)
        options:MTLResourceStorageModePrivate];
    sidecar.instances = instances;
    sidecar.contributions = contributions;
    [instances release];
    [contributions release];
    if (!sidecar.instances || !sidecar.contributions) { [sidecar release]; return 0; }
  }
  return dxmt_handle(sidecar);
}

dxmt_mtl_handle dxmt_as_sidecar_acceleration_structure(dxmt_mtl_handle sidecar) {
  return dxmt_handle(dxmt_sidecar(sidecar).accelerationStructure);
}
dxmt_mtl_handle dxmt_as_sidecar_instance_buffer(dxmt_mtl_handle sidecar) {
  return dxmt_handle(dxmt_sidecar(sidecar).instances);
}
dxmt_mtl_handle dxmt_as_sidecar_contribution_buffer(dxmt_mtl_handle sidecar) {
  return dxmt_handle(dxmt_sidecar(sidecar).contributions);
}
uint64_t dxmt_as_sidecar_resource_id(dxmt_mtl_handle sidecar) {
  return dxmt_sidecar(sidecar).accelerationStructure.gpuResourceID._impl;
}
uint64_t dxmt_as_sidecar_contribution_gpu_address(dxmt_mtl_handle sidecar) {
  return dxmt_sidecar(sidecar).contributions.gpuAddress;
}
uint64_t dxmt_as_sidecar_size(dxmt_mtl_handle sidecar) {
  return dxmt_sidecar(sidecar).nativeSize;
}

uint64_t dxmt_as_sidecar_instance_count(dxmt_mtl_handle sidecar) {
  return dxmt_sidecar(sidecar).instanceCount;
}

dxmt_mtl_handle dxmt_as_sidecar_generations(dxmt_mtl_handle sidecar) {
  return dxmt_handle(dxmt_sidecar(sidecar).generations);
}

int dxmt_as_sidecar_allocate(dxmt_mtl_handle handle, dxmt_mtl_handle device, uint64_t size, uint64_t count) {
  DXMTASSidecar *sidecar = dxmt_sidecar(handle);
  if (!sidecar || !size)
    return 0;
  if (sidecar.accelerationStructure && sidecar.nativeSize == size && sidecar.instanceCount == count)
    return 1;
  DXMTASSidecar *replacement = dxmt_sidecar(dxmt_as_sidecar_new(device, size, count));
  if (!replacement)
    return 0;
  if (!sidecar.generations)
    sidecar.generations = [NSMutableArray array];
  if (sidecar.accelerationStructure)
    [sidecar.generations addObject:sidecar.accelerationStructure];
  if (sidecar.instances)
    [sidecar.generations addObject:sidecar.instances];
  if (sidecar.contributions)
    [sidecar.generations addObject:sidecar.contributions];
  sidecar.accelerationStructure = replacement.accelerationStructure;
  sidecar.instances = replacement.instances;
  sidecar.contributions = replacement.contributions;
  sidecar.nativeSize = size;
  sidecar.instanceCount = count;
  [replacement release];
  return 1;
}

int dxmt_as_encode_prepass(dxmt_mtl_handle encoder, dxmt_mtl_handle pipeline,
    dxmt_mtl_handle source_instances, uint64_t source_offset,
    dxmt_mtl_handle destination_instances, uint64_t destination_offset,
    dxmt_mtl_handle header_buffer, uint64_t header_offset,
    dxmt_mtl_handle contribution_buffer, uint64_t contribution_offset,
    dxmt_mtl_handle live_mirror_buffer, uint64_t live_mirror_offset,
    const struct dxmt_as_prepass_params *params) {
  if (!encoder || !pipeline || !header_buffer || !live_mirror_buffer || !params ||
      source_offset > NSUIntegerMax ||
      destination_offset > NSUIntegerMax || header_offset > NSUIntegerMax ||
      contribution_offset > NSUIntegerMax || live_mirror_offset > NSUIntegerMax)
    return 0;
  id<MTLComputeCommandEncoder> compute = (id<MTLComputeCommandEncoder>)dxmt_obj(encoder);
  id<MTLComputePipelineState> state = (id<MTLComputePipelineState>)dxmt_obj(pipeline);
  id<MTLBuffer> fallback = (id<MTLBuffer>)dxmt_obj(header_buffer);
  [compute setComputePipelineState:state];
  [compute setBuffer:source_instances ? (id<MTLBuffer>)dxmt_obj(source_instances) : fallback
      offset:(NSUInteger)(source_instances ? source_offset : header_offset) atIndex:0];
  [compute setBuffer:destination_instances ? (id<MTLBuffer>)dxmt_obj(destination_instances) : fallback
      offset:(NSUInteger)(destination_instances ? destination_offset : header_offset) atIndex:1];
  [compute setBuffer:fallback offset:(NSUInteger)header_offset atIndex:2];
  [compute setBuffer:contribution_buffer ? (id<MTLBuffer>)dxmt_obj(contribution_buffer) : fallback
      offset:(NSUInteger)(contribution_buffer ? contribution_offset : header_offset) atIndex:3];
  [compute setBytes:params length:sizeof(*params) atIndex:4];
  /* The version mirror the host reads back after this submission completes. */
  [compute setBuffer:(id<MTLBuffer>)dxmt_obj(live_mirror_buffer)
      offset:(NSUInteger)live_mirror_offset atIndex:5];

  NSUInteger threads = params->convert_instances ? MAX((NSUInteger)params->instance_count, 1u) : 1u;
  NSUInteger width = MIN(threads, state.maxTotalThreadsPerThreadgroup);
  [compute dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
  return 1;
}





int dxmt_as_encode_build(dxmt_mtl_handle encoder, dxmt_mtl_handle descriptor,
    dxmt_mtl_handle destination_sidecar, dxmt_mtl_handle scratch, uint64_t scratch_offset) {
  if (!encoder || !descriptor || !destination_sidecar || !scratch || scratch_offset > NSUIntegerMax)
    return 0;
  [(id<MTLAccelerationStructureCommandEncoder>)dxmt_obj(encoder)
      buildAccelerationStructure:dxmt_sidecar(destination_sidecar).accelerationStructure
      descriptor:dxmt_desc(descriptor).metal
      scratchBuffer:(id<MTLBuffer>)dxmt_obj(scratch)
      scratchBufferOffset:(NSUInteger)scratch_offset];
  return 1;
}

int dxmt_as_encode_refit(dxmt_mtl_handle encoder, dxmt_mtl_handle descriptor,
    dxmt_mtl_handle source_sidecar, dxmt_mtl_handle destination_sidecar,
    dxmt_mtl_handle scratch, uint64_t scratch_offset) {
  if (!encoder || !descriptor || !source_sidecar || !destination_sidecar || !scratch ||
      scratch_offset > NSUIntegerMax)
    return 0;
  [(id<MTLAccelerationStructureCommandEncoder>)dxmt_obj(encoder)
      refitAccelerationStructure:dxmt_sidecar(source_sidecar).accelerationStructure
      descriptor:dxmt_desc(descriptor).metal
      destination:dxmt_sidecar(destination_sidecar).accelerationStructure
      scratchBuffer:(id<MTLBuffer>)dxmt_obj(scratch)
      scratchBufferOffset:(NSUInteger)scratch_offset];
  return 1;
}

int dxmt_as_encode_copy(dxmt_mtl_handle encoder, dxmt_mtl_handle source_sidecar,
    dxmt_mtl_handle destination_sidecar, uint32_t mode) {
  if (!encoder || !source_sidecar || !destination_sidecar)
    return 0;
  id<MTLAccelerationStructureCommandEncoder> as_encoder =
      (id<MTLAccelerationStructureCommandEncoder>)dxmt_obj(encoder);
  id<MTLAccelerationStructure> source = dxmt_sidecar(source_sidecar).accelerationStructure;
  id<MTLAccelerationStructure> destination = dxmt_sidecar(destination_sidecar).accelerationStructure;
  if (mode == DXMT_AS_COPY_CLONE)
    [as_encoder copyAccelerationStructure:source toAccelerationStructure:destination];
  else if (mode == DXMT_AS_COPY_COMPACT)
    [as_encoder copyAndCompactAccelerationStructure:source toAccelerationStructure:destination];
  else
    return 0;
  return 1;
}

void dxmt_as_encode_compacted_size(dxmt_mtl_handle encoder, dxmt_mtl_handle source_sidecar,
    dxmt_mtl_handle destination_buffer, uint64_t destination_offset) {
  [(id<MTLAccelerationStructureCommandEncoder>)dxmt_obj(encoder)
      writeCompactedAccelerationStructureSize:dxmt_sidecar(source_sidecar).accelerationStructure
      toBuffer:(id<MTLBuffer>)dxmt_obj(destination_buffer)
      offset:(NSUInteger)destination_offset];
}
