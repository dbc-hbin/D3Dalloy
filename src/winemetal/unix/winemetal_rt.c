#import <Metal/Metal.h>

#include "../winemetal.h"

obj_handle_t
winemetal_rt_new_compute_pipeline(
    obj_handle_t device_handle, const struct WMTComputePipelineInfo *info,
    const obj_handle_t *linked_functions, uint32_t linked_function_count,
    obj_handle_t *error_out
) {
  if (error_out)
    *error_out = NULL_OBJECT_HANDLE;
  if (!device_handle || !info || !info->compute_function || (linked_function_count && !linked_functions))
    return NULL_OBJECT_HANDLE;

  id<MTLDevice> device = (id<MTLDevice>)device_handle;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;
  if (info->max_call_stack_depth > 0)
    descriptor.maxCallStackDepth = info->max_call_stack_depth;
  for (unsigned i = 0; i < 31; ++i) {
    if (info->immutable_buffers & (1u << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];

  if (linked_function_count) {
    NSMutableArray<id<MTLFunction>> *functions = [NSMutableArray arrayWithCapacity:linked_function_count];
    for (uint32_t i = 0; i < linked_function_count; ++i) {
      if (linked_functions[i])
        [functions addObject:(id<MTLFunction>)linked_functions[i]];
    }
    MTLLinkedFunctions *linked = [[MTLLinkedFunctions alloc] init];
    linked.functions = functions;
    descriptor.linkedFunctions = linked;
    [linked release];
  }

  const MTLPipelineOption options = info->fail_on_binary_archive_miss
      ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  NSError *error = nil;
  id<MTLComputePipelineState> pipeline =
      [device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&error];
  if (error_out)
    *error_out = (obj_handle_t)error;
  if (pipeline && !error && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
        addComputePipelineFunctionsWithDescriptor:descriptor error:&error];
    if (error_out)
      *error_out = (obj_handle_t)error;
  }
  [descriptor release];
  return (obj_handle_t)pipeline;
}

obj_handle_t
winemetal_rt_new_visible_function_table(obj_handle_t pipeline_handle, uint32_t function_count) {
  if (!pipeline_handle)
    return NULL_OBJECT_HANDLE;
  MTLVisibleFunctionTableDescriptor *descriptor = [[MTLVisibleFunctionTableDescriptor alloc] init];
  descriptor.functionCount = function_count;
  id<MTLVisibleFunctionTable> table =
      [(id<MTLComputePipelineState>)pipeline_handle newVisibleFunctionTableWithDescriptor:descriptor];
  [descriptor release];
  return (obj_handle_t)table;
}

obj_handle_t
winemetal_rt_new_intersection_function_table(obj_handle_t pipeline_handle, uint32_t function_count) {
  if (!pipeline_handle)
    return NULL_OBJECT_HANDLE;
  MTLIntersectionFunctionTableDescriptor *descriptor = [[MTLIntersectionFunctionTableDescriptor alloc] init];
  descriptor.functionCount = function_count;
  id<MTLIntersectionFunctionTable> table =
      [(id<MTLComputePipelineState>)pipeline_handle newIntersectionFunctionTableWithDescriptor:descriptor];
  [descriptor release];
  return (obj_handle_t)table;
}

obj_handle_t
winemetal_rt_function_handle(obj_handle_t pipeline_handle, obj_handle_t function_handle) {
  if (!pipeline_handle || !function_handle)
    return NULL_OBJECT_HANDLE;
  return (obj_handle_t)[[(id<MTLComputePipelineState>)pipeline_handle
      functionHandleWithFunction:(id<MTLFunction>)function_handle] retain];
}

uint64_t
winemetal_rt_max_total_threads_per_threadgroup(obj_handle_t pipeline_handle) {
  return pipeline_handle ? [(id<MTLComputePipelineState>)pipeline_handle maxTotalThreadsPerThreadgroup] : 0;
}

void
winemetal_rt_visible_function_table_set(obj_handle_t table_handle, obj_handle_t function_handle, uint64_t index) {
  if (table_handle)
    [(id<MTLVisibleFunctionTable>)table_handle setFunction:(id<MTLFunctionHandle>)function_handle atIndex:index];
}

void
winemetal_rt_intersection_function_table_set(
    obj_handle_t table_handle, obj_handle_t function_handle, uint64_t index
) {
  if (table_handle)
    [(id<MTLIntersectionFunctionTable>)table_handle setFunction:(id<MTLFunctionHandle>)function_handle atIndex:index];
}
