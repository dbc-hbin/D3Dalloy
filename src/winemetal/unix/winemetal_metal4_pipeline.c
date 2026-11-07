#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#define WINEMETAL_API
#include "../winemetal_metal4_pipeline.h"
#include "metal4_pipeline_archive.inc"

extern MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format);

static void
metal4_colors(MTL4RenderPipelineColorAttachmentDescriptorArray *attachments,
              const struct WMTColorAttachmentBlendInfo *colors) {
  for (unsigned i = 0; i < 8; ++i) {
    MTL4RenderPipelineColorAttachmentDescriptor *attachment = attachments[i];
    attachment.pixelFormat = to_metal_pixel_format(colors[i].pixel_format);
    attachment.blendingState = colors[i].blending_enabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
    attachment.writeMask = (MTLColorWriteMask)colors[i].write_mask;
    attachment.alphaBlendOperation = (MTLBlendOperation)colors[i].alpha_blend_operation;
    attachment.rgbBlendOperation = (MTLBlendOperation)colors[i].rgb_blend_operation;
    attachment.sourceRGBBlendFactor = (MTLBlendFactor)colors[i].src_rgb_blend_factor;
    attachment.sourceAlphaBlendFactor = (MTLBlendFactor)colors[i].src_alpha_blend_factor;
    attachment.destinationRGBBlendFactor = (MTLBlendFactor)colors[i].dst_rgb_blend_factor;
    attachment.destinationAlphaBlendFactor = (MTLBlendFactor)colors[i].dst_alpha_blend_factor;
  }
}

int32_t
winemetal_metal4_pipeline_call(struct WMTMetal4Call *params) {
  NSError *error = nil;
  switch (params->operation) {
  case WMTMetal4PipelineNewCompiler: {
    MTL4CompilerDescriptor *descriptor = [[MTL4CompilerDescriptor alloc] init];
    params->ret[0] = (uint64_t)[(id<MTLDevice>)params->args[0] newCompilerWithDescriptor:descriptor error:&error];
    [descriptor release];
    if (params->ret[0]) {
      @try {
        D3DalloyPipelineArchive *state=[[D3DalloyPipelineArchive alloc] initWithDevice:(id<MTLDevice>)params->args[0]];
        if (state) objc_setAssociatedObject((id)params->ret[0],&d3dalloy_archive_key,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [state release];
      } @catch (NSException *exception) {archive_trace("init_failure",1);}
    }
    break;
  }
  case WMTMetal4PipelineLoadLibrary:
    /* MTL4LibraryDescriptor compiles source only. Loading our existing AIR
     * container is deliberately the device's public metallib loader. */
    params->ret[0] = (uint64_t)[[(id<MTL4Compiler>)params->args[0] device]
        newLibraryWithData:(dispatch_data_t)params->args[1] error:&error];
    break;
  case WMTMetal4PipelineLibraryFunction:
  case WMTMetal4PipelineSpecializedFunction: {
    id<MTLLibrary> library = (id<MTLLibrary>)params->args[0];
    NSString *name = [NSString stringWithUTF8String:(const char *)(uintptr_t)params->args[1]];
    if (![library.functionNames containsObject:name]) {
      if (params->operation == WMTMetal4PipelineSpecializedFunction)
        error = [NSError errorWithDomain:@"D3DalloyMetal4Pipeline" code:1
                               userInfo:@{NSLocalizedDescriptionKey: @"Metallib entry point is missing"}];
      break;
    }
    MTL4LibraryFunctionDescriptor *function = [[MTL4LibraryFunctionDescriptor alloc] init];
    function.library = library;
    function.name = name;
    if (params->operation == WMTMetal4PipelineLibraryFunction) {
      params->ret[0] = (uint64_t)function;
      break;
    }
    const struct WMTFunctionConstant *constants = (const struct WMTFunctionConstant *)(uintptr_t)params->args[2];
    MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
    for (uint64_t i = 0; i < params->args[3]; ++i)
      [values setConstantValue:constants[i].data.ptr type:(MTLDataType)constants[i].type atIndex:constants[i].index];
    MTL4SpecializedFunctionDescriptor *specialized = [[MTL4SpecializedFunctionDescriptor alloc] init];
    specialized.functionDescriptor = function;
    specialized.specializedName = name;
    specialized.constantValues = values;
    params->ret[0] = (uint64_t)specialized;
    [values release];
    [function release];
    break;
  }
  case WMTMetal4PipelineCompute:
  case WMTMetal4PipelineLinkedCompute: {
    const struct WMTComputePipelineInfo *info = (const struct WMTComputePipelineInfo *)(uintptr_t)params->args[1];
    MTL4ComputePipelineDescriptor *descriptor = [[MTL4ComputePipelineDescriptor alloc] init];
    descriptor.computeFunctionDescriptor = (MTL4FunctionDescriptor *)info->compute_function;
    descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
    descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers
        ? MTL4IndirectCommandBufferSupportStateEnabled : MTL4IndirectCommandBufferSupportStateDisabled;
    MTL4PipelineStageDynamicLinkingDescriptor *dynamic = nil;
    if (params->operation == WMTMetal4PipelineLinkedCompute || info->max_call_stack_depth) {
      dynamic = [[MTL4PipelineStageDynamicLinkingDescriptor alloc] init];
      dynamic.maxCallStackDepth = info->max_call_stack_depth;
    }
    if (params->operation == WMTMetal4PipelineLinkedCompute) {
      MTL4StaticLinkingDescriptor *linking = [[MTL4StaticLinkingDescriptor alloc] init];
      linking.functionDescriptors = [NSArray arrayWithObjects:(id const *)(uintptr_t)params->args[2]
                                                        count:params->args[3]];
      descriptor.staticLinkingDescriptor = linking;
      [linking release];
    }
    params->ret[0] = (uint64_t)archive_pipeline((id<MTL4Compiler>)params->args[0],descriptor,dynamic,YES,&error);
    if (params->operation == WMTMetal4PipelineLinkedCompute && params->ret[0])
      params->ret[2] = [(id<MTLComputePipelineState>)params->ret[0] shaderValidation] == MTLShaderValidationEnabled;
    [dynamic release];
    [descriptor release];
    break;
  }
  case WMTMetal4PipelineRender: {
    const struct WMTRenderPipelineInfo *info = (const struct WMTRenderPipelineInfo *)(uintptr_t)params->args[1];
    MTL4RenderPipelineDescriptor *descriptor = [[MTL4RenderPipelineDescriptor alloc] init];
    descriptor.colorAttachmentMappingState = info->inherit_color_attachment_mapping
        ? MTL4LogicalToPhysicalColorAttachmentMappingStateInherited
        : MTL4LogicalToPhysicalColorAttachmentMappingStateIdentity;
    metal4_colors(descriptor.colorAttachments, info->colors);
    descriptor.vertexFunctionDescriptor = (MTL4FunctionDescriptor *)info->vertex_function;
    descriptor.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)info->fragment_function;
    descriptor.alphaToCoverageState = info->alpha_to_coverage_enabled
        ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    descriptor.rasterizationEnabled = info->rasterization_enabled;
    descriptor.rasterSampleCount = info->raster_sample_count;
    descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
    descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers
        ? MTL4IndirectCommandBufferSupportStateEnabled : MTL4IndirectCommandBufferSupportStateDisabled;
    if (info->max_vertex_amplification_count)
      descriptor.maxVertexAmplificationCount = info->max_vertex_amplification_count;
    params->ret[0] = (uint64_t)archive_pipeline((id<MTL4Compiler>)params->args[0],descriptor,nil,NO,&error);
    [descriptor release];
    break;
  }
  case WMTMetal4PipelineMesh: {
    const struct WMTMeshRenderPipelineInfo *info = (const struct WMTMeshRenderPipelineInfo *)(uintptr_t)params->args[1];
    MTL4MeshRenderPipelineDescriptor *descriptor = [[MTL4MeshRenderPipelineDescriptor alloc] init];
    metal4_colors(descriptor.colorAttachments, info->colors);
    descriptor.objectFunctionDescriptor = (MTL4FunctionDescriptor *)info->object_function;
    descriptor.meshFunctionDescriptor = (MTL4FunctionDescriptor *)info->mesh_function;
    descriptor.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)info->fragment_function;
    descriptor.alphaToCoverageState = info->alpha_to_coverage_enabled
        ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    descriptor.rasterizationEnabled = info->rasterization_enabled;
    descriptor.rasterSampleCount = info->raster_sample_count;
    descriptor.payloadMemoryLength = info->payload_memory_length;
    descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
    descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;
    descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers
        ? MTL4IndirectCommandBufferSupportStateEnabled : MTL4IndirectCommandBufferSupportStateDisabled;
    if (info->max_vertex_amplification_count)
      descriptor.maxVertexAmplificationCount = info->max_vertex_amplification_count;
    params->ret[0] = (uint64_t)archive_pipeline((id<MTL4Compiler>)params->args[0],descriptor,nil,NO,&error);
    [descriptor release];
    break;
  }
  case WMTMetal4PipelineFunctionHandle: {
    MTL4FunctionDescriptor *descriptor = (MTL4FunctionDescriptor *)params->args[1];
    NSString *name;
    if ([descriptor isKindOfClass:[MTL4LibraryFunctionDescriptor class]])
      name = [(MTL4LibraryFunctionDescriptor *)descriptor name];
    else
      name = [(MTL4SpecializedFunctionDescriptor *)descriptor specializedName];
    params->ret[0] = (uint64_t)[[(id<MTLComputePipelineState>)params->args[0] functionHandleWithName:name] retain];
    break;
  }
  case WMTMetal4PipelineArchiveFlush:
    @try {[pipeline_archive((id<MTL4Compiler>)params->args[0]) flush];}
    @catch (NSException *exception) {archive_trace("flush_failure",1);}
    break;
  case WMTMetal4PipelineArchiveControl:
    @try {[D3DalloyPipelineArchive controlAll:(uint32_t)params->args[0]];}
    @catch (NSException *exception) {archive_trace("control_failure",1);}
    break;
  default:
    return (int32_t)0xc0000002;
  }
  params->ret[1] = (uint64_t)[error retain];
  return 0;
}
