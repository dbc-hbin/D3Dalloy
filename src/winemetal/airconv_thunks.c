#define WIN_EXPORT
#include "wineunixlib.h"
#include "airconv_thunks.h"
#include "dxil_raytracing.h"
#include "winemetal.h"

AIRCONV_API int
SM50Initialize(
    const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl,
    sm50_error_t *ppError
) {
  struct sm50_initialize_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.shader = ppShader;
  params.reflection = pRefl;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_initialize, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
SM50Destroy(sm50_shader_t pShader) {
  struct sm50_destroy_params params;
  params.shader = pShader;
  UNIX_CALL(sm50_destroy, &params);
}

AIRCONV_API int
SM50NeedsRasterOrderedBufferMarker(sm50_shader_t pShader, const void *pRootSignature, size_t RootSignatureLength) {
  struct sm50_needs_raster_ordered_buffer_marker_params params;
  params.shader = pShader;
  params.root_signature = pRootSignature;
  params.root_signature_length = RootSignatureLength;
  params.ret = 0;
  if (UNIX_CALL(sm50_needs_raster_ordered_buffer_marker, &params))
    return 0;
  return params.ret;
}

AIRCONV_API int
SM50Compile(
    sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_params params;
  params.shader = (sm50_shader_t)pShader;
  params.args = pArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
SM50GetCompiledBitcode(sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData) {
  struct sm50_get_compiled_bitcode_params params;
  params.bitcode = pBitcode;
  params.data_out = pData;
  UNIX_CALL(sm50_get_compiled_bitcode, &params);
}

AIRCONV_API int
SM50CopyCompiledBitcode(
    sm50_bitcode_t pBitcode, enum SM50_COMPILED_BITCODE_PART Part, void *pDestination, uint64_t Size
) {
  struct sm50_copy_compiled_bitcode_params params;
  params.bitcode = pBitcode;
  params.part = Part;
  params.destination = pDestination ? WMTGetGuestWindowBase() + (uint64_t)(uintptr_t)pDestination : 0;
  params.size = Size;
  params.ret = 1;
  if (UNIX_CALL(sm50_copy_compiled_bitcode, &params))
    return 1;
  return params.ret;
}

AIRCONV_API void
SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  struct sm50_destroy_bitcode_params params;
  params.bitcode = pBitcode;
  UNIX_CALL(sm50_destroy_bitcode, &params);
}

AIRCONV_API size_t
SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  struct sm50_get_error_message_params params;
  params.error = pError;
  params.buffer = pBuffer;
  params.buffer_size = BufferSize;
  params.ret_size = 0;
  UNIX_CALL(sm50_get_error_message, &params);
  return params.ret_size;
}

AIRCONV_API void
SM50FreeError(sm50_error_t pError) {
  struct sm50_free_error_params params;
  params.error = pError;
  UNIX_CALL(sm50_free_error, &params);
}

AIRCONV_API int
SM50CompileTessellationPipelineHull(
    sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode,
    sm50_error_t *ppError
) {
  struct sm50_compile_tessellation_pipeline_hull_params params;
  params.vertex = pVertexShader;
  params.hull = pHullShader;
  params.hull_args = pHullShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_tessellation_hull, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileTessellationPipelineDomain(
    sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_tessellation_pipeline_domain_params params;
  params.domain = pDomainShader;
  params.hull = pHullShader;
  params.domain_args = pDomainShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_tessellation_domain, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileGeometryPipelineVertex(
    sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_geometry_pipeline_vertex_params params;
  params.vertex = pVertexShader;
  params.geometry = pGeometryShader;
  params.vertex_args = pVertexShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_geometry_pipeline_vertex, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileGeometryPipelineGeometry(
    sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_geometry_pipeline_geometry_params params;
  params.vertex = pVertexShader;
  params.geometry = pGeometryShader;
  params.geometry_args = pGeometryShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_geometry_pipeline_geometry, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
) {
  struct sm50_get_arguments_info_params params;
  params.shader = pShader;
  params.constant_buffers = pConstantBuffers;
  params.arguments = pArguments;
  UNIX_CALL(sm50_get_arguments_info, &params);
};
/**
 * Whole-pipeline raytracing compilation. The description is built against
 * dxil_raytracing.h by the D3D12 state object; the native side implements it.
 */
AIRCONV_API int
SM50RTEnumerateLibraryExports(
    const void *pBytecode, uint64_t BytecodeSize, struct sm50_rt_library_export_info *pExports, uint32_t ExportCapacity,
    char *pNames, uint64_t NamesSize, uint32_t *pExportCount, uint64_t *pNamesSize, sm50_error_t *ppError
) {
  struct sm50_rt_enumerate_exports_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.exports = pExports;
  params.export_capacity = ExportCapacity;
  params.names = pNames;
  params.names_size = NamesSize;
  params.export_count = pExportCount;
#ifndef _WIN64
  /* The legacy thunk transports a pointer-sized count; the public compiler
   * ABI returns uint64_t on both PE architectures. Never alias those slots. */
  size_t names_size = 0;
  params.names_size_out = pNamesSize ? &names_size : NULL;
#else
  params.names_size_out = pNamesSize;
#endif
  params.error = ppError;
  params.ret = 0;
  NTSTATUS status = UNIX_CALL(sm50_rt_enumerate_exports, &params);
#ifndef _WIN64
  if (pNamesSize)
    *pNamesSize = names_size;
#endif
  if (status)
    return 0;
  return params.ret;
}

AIRCONV_API int
SM50CompileRaytracingPipeline(
    const struct SM50_RT_PIPELINE_DESC *pDesc, sm50_bitcode_t *ppBitcode, struct SM50_COMPILED_BITCODE *pMetallib,
    struct SM50_RT_FUNCTION_INFO *pFunctions, uint32_t FunctionCapacity, uint32_t *pFunctionCount,
    sm50_error_t *ppError
) {
  struct sm50_rt_compile_pipeline_params params;
  params.desc = pDesc;
  params.bitcode = ppBitcode;
  params.metallib = pMetallib;
  params.functions = pFunctions;
  params.function_capacity = FunctionCapacity;
  params.function_count = pFunctionCount;
  params.error = ppError;
  params.ret = 0;
  NTSTATUS status = UNIX_CALL(sm50_rt_compile_pipeline, &params);
  if (status)
    return 0;
  return params.ret;
}

/** D3D12 work graph node shaders; the native side implements dxil_workgraph.h. */
AIRCONV_API int
SM50WGReflectLibrary(
    const void *pBytecode, uint64_t BytecodeSize, struct SM50_WG_NODE_INFO *pNodes, uint32_t NodeCapacity,
    struct SM50_WG_NODE_IO *pOutputs, uint32_t OutputCapacity, char *pNames, uint64_t NamesSize,
    uint32_t *pNodeCount, uint32_t *pOutputCount, uint64_t *pNamesSize, sm50_error_t *ppError
) {
  struct sm50_wg_reflect_library_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.nodes = pNodes;
  params.node_capacity = NodeCapacity;
  params.outputs = pOutputs;
  params.output_capacity = OutputCapacity;
  params.names = pNames;
  params.names_size = NamesSize;
  params.node_count = pNodeCount;
  params.output_count = pOutputCount;
  params.names_size_out = pNamesSize;
  params.error = ppError;
  params.ret = 0;
  if (UNIX_CALL(sm50_wg_reflect_library, &params))
    return 0;
  return params.ret;
}

AIRCONV_API int
SM50WGCompileNode(
    const struct SM50_WG_NODE_COMPILE_DESC *pDesc, sm50_bitcode_t *ppBitcode, struct SM50_COMPILED_BITCODE *pMetallib,
    struct SM50_WG_NODE_COMPILED_INFO *pInfo, sm50_error_t *ppError
) {
  struct sm50_wg_compile_node_params params;
  params.desc = pDesc;
  params.bitcode = ppBitcode;
  params.metallib = pMetallib;
  params.info = pInfo;
  params.error = ppError;
  params.ret = 0;
  if (UNIX_CALL(sm50_wg_compile_node, &params))
    return 0;
  return params.ret;
}
