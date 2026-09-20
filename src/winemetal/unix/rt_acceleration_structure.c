#import <Foundation/Foundation.h>
#import <Metal/Metal.h>


#include "rt_acceleration_structure.h"

@interface DXMTASDescriptor : NSObject
@property(nonatomic, retain) MTLAccelerationStructureDescriptor *metal;
@property(nonatomic, retain) NSMutableArray<MTLAccelerationStructureGeometryDescriptor *> *geometries;
@end
@implementation DXMTASDescriptor
- (void)dealloc { [_metal release]; [_geometries release]; [super dealloc]; }
@end

@interface DXMTASSidecar : NSObject
@property(nonatomic, retain) id<MTLAccelerationStructure> accelerationStructure;
@property(nonatomic, retain) id<MTLBuffer> instances;
@property(nonatomic, retain) id<MTLBuffer> contributions;
@property(nonatomic) uint64_t nativeSize;
@end
@implementation DXMTASSidecar
- (void)dealloc {
  [_accelerationStructure release];
  [_instances release];
  [_contributions release];
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

dxmt_mtl_handle dxmt_as_descriptor_new(uint32_t top_level, uint32_t geometry_count) {
  DXMTASDescriptor *result = [[DXMTASDescriptor alloc] init];
  if (top_level) {
    result.metal = [MTLInstanceAccelerationStructureDescriptor descriptor];
  } else {
    MTLPrimitiveAccelerationStructureDescriptor *primitive =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    result.geometries = [NSMutableArray arrayWithCapacity:geometry_count];
    for (uint32_t i = 0; i < geometry_count; ++i)
      [result.geometries addObject:(MTLAccelerationStructureGeometryDescriptor *)[NSNull null]];
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
    uint32_t intersection_function_table_offset, uint32_t geometry_flags) {
  if (!vertices || !triangle_count || vertex_offset > NSUIntegerMax || vertex_stride > NSUIntegerMax ||
      index_offset > NSUIntegerMax || transform_offset > NSUIntegerMax || triangle_count > NSUIntegerMax)
    return 0;
  MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
      [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
  geometry.vertexBuffer = (id<MTLBuffer>)dxmt_obj(vertices);
  geometry.vertexBufferOffset = (NSUInteger)vertex_offset;
  geometry.vertexStride = (NSUInteger)vertex_stride;
  geometry.vertexFormat = MTLAttributeFormatFloat3;
  geometry.triangleCount = (NSUInteger)triangle_count;
  if (indices) {
    geometry.indexBuffer = (id<MTLBuffer>)dxmt_obj(indices);
    geometry.indexBufferOffset = (NSUInteger)index_offset;
    geometry.indexType = index_type ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
  }
  if (transform) {
    geometry.transformationMatrixBuffer = (id<MTLBuffer>)dxmt_obj(transform);
    geometry.transformationMatrixBufferOffset = (NSUInteger)transform_offset;
  }
  geometry.intersectionFunctionTableOffset = intersection_function_table_offset;
  dxmt_geometry_flags(geometry, geometry_flags);
  return dxmt_set_geometry(dxmt_desc(descriptor), index, geometry);
}

int dxmt_as_descriptor_set_aabbs(dxmt_mtl_handle descriptor, uint32_t index,
    dxmt_mtl_handle boxes, uint64_t box_offset, uint64_t box_stride,
    uint64_t box_count, uint32_t intersection_function_table_offset, uint32_t geometry_flags) {
  if (!boxes || !box_count || box_offset > NSUIntegerMax || box_stride > NSUIntegerMax ||
      box_count > NSUIntegerMax)
    return 0;
  MTLAccelerationStructureBoundingBoxGeometryDescriptor *geometry =
      [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
  geometry.boundingBoxBuffer = (id<MTLBuffer>)dxmt_obj(boxes);
  geometry.boundingBoxBufferOffset = (NSUInteger)box_offset;
  geometry.boundingBoxStride = (NSUInteger)box_stride;
  geometry.boundingBoxCount = (NSUInteger)box_count;
  geometry.intersectionFunctionTableOffset = intersection_function_table_offset;
  dxmt_geometry_flags(geometry, geometry_flags);

  return dxmt_set_geometry(dxmt_desc(descriptor), index, geometry);
}

int dxmt_as_descriptor_set_instances(dxmt_mtl_handle descriptor,
    dxmt_mtl_handle converted_instances, uint64_t converted_offset, uint64_t instance_count) {
  DXMTASDescriptor *owner = dxmt_desc(descriptor);
  if (!owner || ![owner.metal isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]] ||
      converted_offset > NSUIntegerMax || instance_count > NSUIntegerMax)
    return 0;
  MTLInstanceAccelerationStructureDescriptor *instances =
      (MTLInstanceAccelerationStructureDescriptor *)owner.metal;
  instances.instanceDescriptorBuffer = (id<MTLBuffer>)dxmt_obj(converted_instances);
  instances.instanceDescriptorBufferOffset = (NSUInteger)converted_offset;
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
  if (!device || !native_size || native_size > NSUIntegerMax || instance_count > NSUIntegerMax / 4)
    return 0;
  id<MTLDevice> metal_device = (id<MTLDevice>)dxmt_obj(device);
  DXMTASSidecar *sidecar = [[DXMTASSidecar alloc] init];
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



int dxmt_as_encode_write_u64(dxmt_mtl_handle encoder, dxmt_mtl_handle pipeline,
    dxmt_mtl_handle destination_buffer, uint64_t destination_offset, uint64_t value) {
  if (!encoder || !pipeline || !destination_buffer || destination_offset > NSUIntegerMax)
    return 0;
  id<MTLComputeCommandEncoder> compute = (id<MTLComputeCommandEncoder>)dxmt_obj(encoder);
  [compute setComputePipelineState:(id<MTLComputePipelineState>)dxmt_obj(pipeline)];
  [compute setBuffer:(id<MTLBuffer>)dxmt_obj(destination_buffer)
      offset:(NSUInteger)destination_offset atIndex:0];
  [compute setBytes:&value length:sizeof(value) atIndex:1];
  uint32_t offset = 0;
  [compute setBytes:&offset length:sizeof(offset) atIndex:2];
  [compute dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  return 1;
}

int dxmt_as_encode_finalize_compacted_size(dxmt_mtl_handle encoder, dxmt_mtl_handle pipeline,
    dxmt_mtl_handle destination_buffer, uint64_t destination_offset) {
  if (!encoder || !pipeline || !destination_buffer || destination_offset > NSUIntegerMax)
    return 0;
  id<MTLComputeCommandEncoder> compute = (id<MTLComputeCommandEncoder>)dxmt_obj(encoder);
  [compute setComputePipelineState:(id<MTLComputePipelineState>)dxmt_obj(pipeline)];
  [compute setBuffer:(id<MTLBuffer>)dxmt_obj(destination_buffer)
      offset:(NSUInteger)destination_offset atIndex:0];
  uint32_t offset = 0;
  [compute setBytes:&offset length:sizeof(offset) atIndex:1];
  [compute dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
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
