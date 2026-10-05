#include "stddef.h"
#include "airconv_public.h"
#include "dxil_raytracing.h"
#include "dxil_workgraph.h"

#ifndef __WINEMETAL_AIRCONV_THUNKS_H
#define __WINEMETAL_AIRCONV_THUNKS_H

enum airconv_unixcalls {
  unix_sm50_initialize = 74,
  unix_sm50_destroy,
  unix_sm50_compile,
  unix_sm50_get_compiled_bitcode,
  unix_sm50_destroy_bitcode,
  unix_sm50_get_error_message,
  unix_sm50_free_error,
  unix_sm50_compile_geometry_pipeline_vertex,
  unix_sm50_compile_geometry_pipeline_geometry,
  unix_sm50_compile_tessellation_vertex,
  unix_sm50_compile_tessellation_hull,
  unix_sm50_compile_tessellation_domain,
  unix_sm50_get_arguments_info = 88,
  /*
   * DXIL raytracing block. The native entries live at these absolute indices in
   * the winemetal unix call table; the PE side only ever reaches them through the
   * thunks below.
   */
  unix_sm50_rt_enumerate_exports = 146,
  unix_sm50_rt_compile_pipeline,
  unix_sm50_needs_raster_ordered_buffer_marker = 212,
  unix_sm50_copy_compiled_bitcode = 215,
  /* D3D12 work graph node shaders (dxil_workgraph.h). */
  unix_sm50_wg_reflect_library = 312,
  unix_sm50_wg_compile_node = 313,
};

/** One export of a DXIL library, as reported by SM50RTEnumerateLibraryExports. */
struct sm50_rt_enumerate_exports_params {
  const void *bytecode;
  size_t bytecode_size;
  /**
   * Array of sm50_rt_library_export_info (dxil_raytracing.h, the authoritative
   * declaration); opaque here so this header cannot drift from it.
   */
  void *exports;
  uint32_t export_capacity;
  char *names;
  size_t names_size;
  uint32_t *export_count;
  size_t *names_size_out;
  sm50_error_t *error;
  int ret;
};

/**
 * The thunk only forwards the description the runtime built against
 * dxil_raytracing.h, so this struct keeps the compiler types opaque.
 */
struct sm50_rt_compile_pipeline_params {
  const void *desc;
  sm50_bitcode_t *bitcode;
  struct SM50_COMPILED_BITCODE *metallib;
  void *functions;
  uint32_t function_capacity;
  uint32_t *function_count;
  sm50_error_t *error;
  int ret;
};

/** SM50WGReflectLibrary; the arrays are the dxil_workgraph.h types, opaque here. */
struct sm50_wg_reflect_library_params {
  const void *bytecode;
  uint64_t bytecode_size;
  void *nodes;
  uint32_t node_capacity;
  void *outputs;
  uint32_t output_capacity;
  char *names;
  uint64_t names_size;
  uint32_t *node_count;
  uint32_t *output_count;
  uint64_t *names_size_out;
  sm50_error_t *error;
  int ret;
};

/** SM50WGCompileNode; the description is forwarded verbatim. */
struct sm50_wg_compile_node_params {
  const void *desc;
  sm50_bitcode_t *bitcode;
  struct SM50_COMPILED_BITCODE *metallib;
  void *info;
  sm50_error_t *error;
  int ret;
};

struct sm50_initialize_params {
  const void *bytecode;
  size_t bytecode_size;
  sm50_shader_t *shader;
  void *reflection;
  sm50_error_t *error;
  int ret;
};

struct sm50_destroy_params {
  sm50_shader_t shader;
};

struct sm50_compile_params {
  sm50_shader_t shader;
  void *args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_get_compiled_bitcode_params {
  sm50_bitcode_t bitcode;
  struct SM50_COMPILED_BITCODE *data_out;
};

struct sm50_destroy_bitcode_params {
  sm50_bitcode_t bitcode;
};

struct sm50_copy_compiled_bitcode_params {
  sm50_bitcode_t bitcode;
  uint64_t part;
  uint64_t destination; /* caller-image address, zero-extended from a 32-bit PE */
  uint64_t size;
  int ret;
};

struct sm50_get_error_message_params {
  sm50_error_t error;
  char *buffer;
  size_t buffer_size;
  size_t ret_size;
};

struct sm50_free_error_params {
  sm50_error_t error;
};

struct sm50_compile_geometry_pipeline_vertex_params {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  void *vertex_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_geometry_pipeline_geometry_params {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  void *geometry_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_tessellation_pipeline_vertex_params {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  void *vertex_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_tessellation_pipeline_hull_params {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  void *hull_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_compile_tessellation_pipeline_domain_params {
  sm50_shader_t hull;
  sm50_shader_t domain;
  void *domain_args;
  const char *func_name;
  sm50_bitcode_t *bitcode;
  sm50_error_t *error;
  int ret;
};

struct sm50_get_arguments_info_params {
  sm50_shader_t shader;
  struct MTL_SM50_SHADER_ARGUMENT *constant_buffers;
  struct MTL_SM50_SHADER_ARGUMENT *arguments;
};

struct sm50_needs_raster_ordered_buffer_marker_params {
  sm50_shader_t shader;
  const void *root_signature;
  size_t root_signature_length;
  int ret;
};

#if defined(__LP64__) || defined(_WIN64)
#define COMPATIBLE_STRUCT32(stru, size) _Static_assert(sizeof(struct stru##32) == size, "incompatible struct size");
#else
#define COMPATIBLE_STRUCT32(stru, size) _Static_assert(sizeof(struct stru) == size, "incompatible struct size");
#endif

struct sm50_initialize_params32 {
  uint32_t bytecode;
  unsigned int bytecode_size;
  uint32_t shader;
  uint32_t reflection;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_initialize_params, 24)

struct sm50_compile_params32 {
  sm50_shader_t shader;
  uint32_t args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_params, 32)

struct sm50_get_compiled_bitcode_params32 {
  sm50_bitcode_t bitcode;
  uint32_t data_out;
};

COMPATIBLE_STRUCT32(sm50_get_compiled_bitcode_params, 16)

/* All fields are fixed-width, so the 32-bit and 64-bit layouts are identical and need no conversion. */
_Static_assert(sizeof(struct sm50_copy_compiled_bitcode_params) == 40, "incompatible struct size");

struct sm50_get_error_message_params32 {
  sm50_error_t error;
  uint32_t buffer;
  uint32_t buffer_size;
  uint32_t ret_size;
};

COMPATIBLE_STRUCT32(sm50_get_error_message_params, 24)

struct sm50_compile_geometry_pipeline_vertex_params32 {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  uint32_t vertex_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_geometry_pipeline_vertex_params, 40)

struct sm50_compile_geometry_pipeline_geometry_params32 {
  sm50_shader_t vertex;
  sm50_shader_t geometry;
  uint32_t geometry_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_geometry_pipeline_geometry_params, 40)

struct sm50_compile_tessellation_pipeline_vertex_params32 {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  uint32_t vertex_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_tessellation_pipeline_vertex_params, 40)

struct sm50_compile_tessellation_pipeline_hull_params32 {
  sm50_shader_t vertex;
  sm50_shader_t hull;
  uint32_t hull_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_tessellation_pipeline_hull_params, 40)

struct sm50_compile_tessellation_pipeline_domain_params32 {
  sm50_shader_t hull;
  sm50_shader_t domain;
  uint32_t domain_args;
  uint32_t func_name;
  uint32_t bitcode;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_compile_tessellation_pipeline_domain_params, 40)

struct sm50_get_arguments_info_params32 {
  sm50_shader_t shader;
  uint32_t constant_buffers;
  uint32_t arguments;
};

COMPATIBLE_STRUCT32(sm50_get_arguments_info_params, 16)

struct sm50_needs_raster_ordered_buffer_marker_params32 {
  sm50_shader_t shader;
  uint32_t root_signature;
  uint32_t root_signature_length;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_needs_raster_ordered_buffer_marker_params, 24)

struct sm50_rt_enumerate_exports_params32 {
  uint32_t bytecode;
  uint32_t bytecode_size;
  uint32_t exports;
  uint32_t export_capacity;
  uint32_t names;
  uint32_t names_size;
  uint32_t export_count;
  uint32_t names_size_out;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_rt_enumerate_exports_params, 40)

struct sm50_rt_compile_pipeline_params32 {
  uint32_t desc;
  uint32_t bitcode;
  uint32_t metallib;
  uint32_t functions;
  uint32_t function_capacity;
  uint32_t function_count;
  uint32_t error;
  int ret;
};

COMPATIBLE_STRUCT32(sm50_rt_compile_pipeline_params, 32)

struct sm50_rt_pipeline_desc32 {
  uint32_t Libraries, LibraryCount, Exports, ExportCount, GlobalRootSignature;
  uint64_t GlobalRootSignatureSize;
  uint32_t MaxRecursionDepth, MaxPayloadSize, MaxAttributeSize, Flags, MetalVersion;
  uint32_t LinkExports, LinkExportCount, FunctionNames;
  uint64_t FunctionNamesSize;
};
struct sm50_rt_library32 { uint32_t Bytecode; uint64_t Size; };
struct sm50_rt_export32 {
  uint32_t Stage, LibraryIndex, HitGroupType, ClosestHitIndex, AnyHitIndex, IntersectionIndex;
  uint32_t EntryPoint, ContainerEntryPoint, LocalRootSignature;
  uint64_t LocalRootSignatureSize;
};
struct sm50_rt_function_info32 {
  uint32_t Name, ExportName, Kind, ExportIndex, TableIndex, Flags;
};
_Static_assert(sizeof(struct sm50_rt_pipeline_desc32) == 72, "raytracing pipeline description ABI");
_Static_assert(sizeof(struct sm50_rt_export32) == 48, "raytracing export ABI");
#if !defined(__LP64__) && !defined(_WIN64)
_Static_assert(sizeof(struct SM50_RT_PIPELINE_DESC) == sizeof(struct sm50_rt_pipeline_desc32), "raytracing caller description ABI");
_Static_assert(sizeof(struct SM50_RT_EXPORT) == sizeof(struct sm50_rt_export32), "raytracing caller export ABI");
_Static_assert(sizeof(struct SM50_RT_LIBRARY) == sizeof(struct sm50_rt_library32), "raytracing caller library ABI");
_Static_assert(sizeof(struct SM50_RT_FUNCTION_INFO) == sizeof(struct sm50_rt_function_info32), "raytracing caller reflection ABI");
#endif

struct sm50_wg_reflect_library_params32 {
  uint32_t bytecode;
  uint64_t bytecode_size;
  uint32_t nodes, node_capacity, outputs, output_capacity, names;
  uint64_t names_size;
  uint32_t node_count, output_count, names_size_out, error;
  int ret;
};
COMPATIBLE_STRUCT32(sm50_wg_reflect_library_params, 72)

struct sm50_wg_compile_node_params32 {
  uint32_t desc, bitcode, metallib, info, error;
  int ret;
};
COMPATIBLE_STRUCT32(sm50_wg_compile_node_params, 24)

struct sm50_wg_node_compile_desc32 {
  uint32_t Library;
  uint64_t LibrarySize;
  uint32_t EntryPoint, FunctionName, GlobalRootSignature;
  uint64_t GlobalRootSignatureSize;
  uint32_t LocalRootSignature;
  uint64_t LocalRootSignatureSize;
  uint32_t MetalVersion, Launch, OutputCount, Reserved, OutputElementBase, OutputElementCount;
  uint32_t Libraries, Exports, LibraryCount, ExportCount, EntryLibraryIndex, AllowUnresolved, EntryExportIndex;
};
_Static_assert(sizeof(struct sm50_wg_node_compile_desc32) == 112, "work graph node description ABI");
struct sm50_wg_library32 { uint32_t Bytecode; uint64_t Size; };
struct sm50_wg_export32 {
  uint32_t Name, EntryPoint, LibraryIndex, Scope, Flags, Reserved;
  uint32_t GlobalRootSignature; uint64_t GlobalRootSignatureSize;
  uint32_t LocalRootSignature; uint64_t LocalRootSignatureSize;
};
_Static_assert(sizeof(struct sm50_wg_export32) == 56, "work graph export description ABI");

#if !defined(__LP64__) && !defined(_WIN64)
_Static_assert(sizeof(struct SM50_WG_NODE_COMPILE_DESC) == 112, "work graph caller node description ABI");
_Static_assert(sizeof(struct SM50_WG_LIBRARY) == sizeof(struct sm50_wg_library32), "work graph caller library ABI");
_Static_assert(sizeof(struct SM50_WG_EXPORT) == sizeof(struct sm50_wg_export32), "work graph caller export ABI");
#endif

#define UNIX_CALL(code, params) WINE_UNIX_CALL(unix_##code, params)

#endif