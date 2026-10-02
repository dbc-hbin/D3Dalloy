#pragma once
#include "Metal.hpp"
#include "winemetal_metal4_pipeline.h"

namespace WMT4 {

/* These handles own Metal 4 function descriptors, not MTLFunction objects. The
 * descriptor retains its library and exact entry name for lazy PSO variants. */
class Function : public WMT::Object {};

class Library : public WMT::Object {
public:
  Library() = default;
  explicit Library(WMT::Library library) { handle = library.handle; }

  WMT::Reference<Function> newFunction(const char *name) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineLibraryFunction;
    call.args[0] = handle;
    call.args[1] = uint64_t(reinterpret_cast<uintptr_t>(name));
    WMTMetal4Call(&call);
    return WMT::Reference<Function>(call.ret[0]);
  }

  WMT::Reference<Function> newFunctionWithConstants(
      const char *name, const WMTFunctionConstant *constants, uint32_t count, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineSpecializedFunction;
    call.args[0] = handle;
    call.args[1] = uint64_t(reinterpret_cast<uintptr_t>(name));
    call.args[2] = uint64_t(reinterpret_cast<uintptr_t>(constants));
    call.args[3] = count;
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<Function>(call.ret[0]);
  }
};

class Compiler : public WMT::Object {
public:
  static WMT::Reference<Compiler> Create(WMT::Device device, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineNewCompiler;
    call.args[0] = device.handle;
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<Compiler>(call.ret[0]);
  }

  WMT::Reference<Library> newLibrary(WMT::DispatchData data, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineLoadLibrary;
    call.args[0] = handle;
    call.args[1] = data.handle;
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<Library>(call.ret[0]);
  }

  WMT::Reference<WMT::ComputePipelineState> newComputePipelineState(
      const WMTComputePipelineInfo &info, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineCompute;
    call.args[0] = handle;
    call.args[1] = uint64_t(reinterpret_cast<uintptr_t>(&info));
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<WMT::ComputePipelineState>(call.ret[0]);
  }

  WMT::Reference<WMT::ComputePipelineState> newComputePipelineState(Function function, WMT::Error &error) {
    WMTComputePipelineInfo info{};
    WMT::InitializeComputePipelineInfo(info);
    info.compute_function = function;
    return newComputePipelineState(info, error);
  }

  WMT::Reference<WMT::RenderPipelineState> newRenderPipelineState(
      const WMTRenderPipelineInfo &info, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineRender;
    call.args[0] = handle;
    call.args[1] = uint64_t(reinterpret_cast<uintptr_t>(&info));
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<WMT::RenderPipelineState>(call.ret[0]);
  }

  WMT::Reference<WMT::RenderPipelineState> newRenderPipelineState(
      const WMTMeshRenderPipelineInfo &info, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineMesh;
    call.args[0] = handle;
    call.args[1] = uint64_t(reinterpret_cast<uintptr_t>(&info));
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<WMT::RenderPipelineState>(call.ret[0]);
  }

  WMT::Reference<WMT::ComputePipelineState> newComputePipelineStateWithLinkedFunctions(
      const WMTComputePipelineInfo &info, const obj_handle_t *functions, uint32_t count, WMT::Error &error) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineLinkedCompute;
    call.args[0] = handle;
    call.args[1] = uint64_t(reinterpret_cast<uintptr_t>(&info));
    call.args[2] = uint64_t(reinterpret_cast<uintptr_t>(functions));
    call.args[3] = count;
    WMTMetal4Call(&call);
    error.handle = call.ret[1];
    return WMT::Reference<WMT::ComputePipelineState>(call.ret[0]);
  }

  WMT::Reference<WMT::FunctionHandle> functionHandle(WMT::ComputePipelineState pipeline, Function function) {
    struct WMTMetal4Call call{};
    call.operation = WMTMetal4PipelineFunctionHandle;
    call.args[0] = pipeline.handle;
    call.args[1] = function.handle;
    WMTMetal4Call(&call);
    return WMT::Reference<WMT::FunctionHandle>(call.ret[0]);
  }
};

} // namespace WMT4
