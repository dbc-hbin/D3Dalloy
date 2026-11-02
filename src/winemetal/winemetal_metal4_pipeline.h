#pragma once
#include "winemetal_metal4.h"

/* Pipeline domain of Unix ABI 314. Requests reuse WMT pipeline-info PODs, but
 * stage handles are retained MTL4FunctionDescriptor objects rather than MTLFunction. */
enum WMTMetal4PipelineOperation : uint32_t {
  WMTMetal4PipelineNewCompiler = 128,
  WMTMetal4PipelineLoadLibrary,
  WMTMetal4PipelineLibraryFunction,
  WMTMetal4PipelineSpecializedFunction,
  WMTMetal4PipelineCompute,
  WMTMetal4PipelineRender,
  WMTMetal4PipelineMesh,
  WMTMetal4PipelineLinkedCompute,
  WMTMetal4PipelineFunctionHandle,
  WMTMetal4PipelineArchiveFlush,
  WMTMetal4PipelineArchiveControl,
};
