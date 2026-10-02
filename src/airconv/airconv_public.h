#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#define AIRCONV_VERSION 32

#ifdef __cplusplus
#include <string>

enum class SM50BindingType : uint32_t {
  ConstantBuffer,
  Sampler,
  SRV,
  UAV,
};
#else
typedef uint32_t SM50BindingType;
#endif

enum SM50_BINDING_INDEX: uint32_t {
  SM50_BINDING_INDEX_CONSTANT_BUFFER = 0,
  SM50_BINDING_INDEX_ARGUMENT_TABLE = 1,
  SM50_BINDING_INDEX_VERTEX_BUFFER = 2,
  SM50_BINDING_INDEX_INDEX_BUFFER = 3,
  SM50_BINDING_INDEX_DRAW_ARGUMENTS = 4,
  SM50_BINDING_INDEX_INDIRECT_ARGUMENTS = 4,
  SM50_BINDING_INDEX_STREAM_OUTPUT0 = 5,
  /* For multiple stages in the same shader function */
  SM50_BINDING_INDEX_CONSTANT_BUFFER2 = 6,
  SM50_BINDING_INDEX_ARGUMENT_TABLE2 = 7,
  /* D3D12 TS+GS two-pass: pass-1 DS capture scratch, compacted primitive
   * list and SM50_TESS_CAPTURE_CONTROL (constant address space). */
  SM50_BINDING_INDEX_TESS_CAPTURE = 8,
  SM50_BINDING_INDEX_TESS_CAPTURE_LIST = 9,
  SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL = 10,
  /* Fragment stage, SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM: SM50_STATISTICS_CLAIM_HEADER and the
   * per-pixel key entries (raster_order_group(0)). */
  SM50_BINDING_INDEX_STATISTICS_CLAIM = 7,

  SM50_BINDING_INDEX_ROOT_ARGUMENTS = 0,
  SM50_BINDING_INDEX_STATIC_SAMPLERS = 1,
  /* D3D12 pipeline statistics: eleven low/high atomic uint32 pairs. */
  SM50_BINDING_INDEX_PIPELINE_STATISTICS = 25,
  SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS = 20,
  SM50_BINDING_INDEX_MSAA_MESH_CONTROL = 13,
  SM50_BINDING_INDEX_MSAA_PIXEL_INPUTS = 21,
  SM50_BINDING_INDEX_MSAA_PIXEL_OUTPUTS = 22,
  SM50_BINDING_INDEX_MSAA_TILE_CONTROL = 23,
  SM50_BINDING_INDEX_MSAA_COVERAGE = 24,
  /* D3D12 index pulling and stream-output capture share bindings 26/27.
   * Control is SM50_INDEX_PULL_CONTROL or SM50_SO_CAPTURE_CONTROL; the two
   * compilation modes are mutually exclusive. */
  SM50_BINDING_INDEX_VERTEX_INDICES = 26,
  SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL = 27,
  SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH = 28,
  /* Normal DXIL stages; slot 30 is reserved for indirect-command resolution. */
  SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS = 29,
  /* D3D12 work graph node kernels: SM50_WG_NODE_CONTEXT (dxil_workgraph.h). */
  SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT = 18,
};

enum MTL_SM50_SHADER_ARGUMENT_FLAG : uint32_t {
  MTL_SM50_SHADER_ARGUMENT_BUFFER = 1 << 0,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE = 1 << 1,
  MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH = 1 << 2,
  MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER = 1 << 3,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP = 1 << 4,
  MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET = 1 << 5,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY = 1 << 6,
  MTL_SM50_SHADER_ARGUMENT_READ_ACCESS = 1 << 10,
  MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS = 1 << 11,
};

struct MTL_SM50_SHADER_ARGUMENT {
  SM50BindingType Type;
  /**
  bind point of it's corresponding resource space
  constant buffer:    cb1 -> 1
  srv:                t10 -> 10
  uav:                u0  -> 0
  sampler:            s2  -> 2
  */
  uint32_t SM50BindingSlot;
  enum MTL_SM50_SHADER_ARGUMENT_FLAG Flags;
  uint32_t StructurePtrOffset;
};

enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE {
  MTL_TESSELLATOR_OUTPUT_POINT = 1,
  MTL_TESSELLATOR_OUTPUT_LINE = 2,
  MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW = 3,
  MTL_TESSELLATOR_TRIANGLE_CCW = 4
};

struct MTL_TESSELLATOR_REFLECTION {
  uint32_t Partition;
  float MaxFactor;
  enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE OutputPrimitive;
};

struct MTL_GEOMETRY_SHADER_PASS_THROUGH {
  uint8_t RenderTargetArrayIndexReg;
  uint8_t RenderTargetArrayIndexComponent;
  uint8_t ViewportArrayIndexReg;
  uint8_t ViewportArrayIndexComponent;
};

struct MTL_GEOMETRY_SHADER_REFLECTION {
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t GSPassThrough;
  };
  uint32_t Primitive;
  /* D3D10_SB_PRIMITIVE_TOPOLOGY of the output stream, dcl_maxout and dcl_gsinstances. */
  uint16_t MaxOutputVertexCount;
  uint8_t OutputTopology;
  uint8_t InstanceCount;
};

struct MTL_POST_TESSELLATOR_REFLECTION {
  uint32_t MaxPotentialTessFactor;
};

struct MTL_PIXEL_SHADER_REFLECTION {
  uint32_t ValidRenderTargets;
  uint32_t HasCoverageOutput;
  uint32_t SampleFrequency;
};

struct MTL_SHADER_REFLECTION {
  uint32_t ConstanttBufferTableBindIndex;
  uint32_t ArgumentBufferBindIndex;
  uint32_t NumConstantBuffers;
  uint32_t NumArguments;
  union {
    uint32_t ThreadgroupSize[3];
    struct MTL_TESSELLATOR_REFLECTION Tessellator;
    struct MTL_GEOMETRY_SHADER_REFLECTION GeometryShader;
    struct MTL_POST_TESSELLATOR_REFLECTION PostTessellator;
    /**
    \deprecated for compatibility, use PixelShader.ValidRenderTargets instead
     */
    uint32_t PSValidRenderTargets;
    struct MTL_PIXEL_SHADER_REFLECTION PixelShader;
  };
  uint16_t ConstantBufferSlotMask;
  uint16_t SamplerSlotMask;
  uint64_t UAVSlotMask;
  uint64_t SRVSlotMaskHi;
  uint64_t SRVSlotMaskLo;
  uint32_t NumOutputElement;
  uint32_t ThreadsPerPatch;
  uint32_t ArgumentTableQwords;
  /* Pixel shader: D3D11 ForceEarlyDepthStencil (DXBC global flag / DXIL shader flag). */
  uint32_t PSForceEarlyDepthStencil;
};

#if defined(__LP64__) || defined(_WIN64)
typedef void *sm50_ptr64_t;
#else
typedef struct sm50_ptr64_t {
  uint64_t impl;

#ifdef __cplusplus
  sm50_ptr64_t() {
    impl = 0;
  }

  sm50_ptr64_t(void * ptr) {
    impl = (uint64_t)ptr;
  }

  sm50_ptr64_t(uint64_t v) {
    impl = v;
  }

  operator uint64_t () const {
    return impl;
  }
#endif

} sm50_ptr64_t;
#endif

typedef sm50_ptr64_t sm50_shader_t;
typedef sm50_ptr64_t sm50_bitcode_t;
typedef sm50_ptr64_t sm50_error_t;

#ifdef _WIN32
#ifdef WIN_EXPORT
#define AIRCONV_API __declspec(dllexport)
#else
#define AIRCONV_API __declspec(dllimport)
#endif
#else
#define AIRCONV_API __attribute__((sysv_abi))
#endif

struct SM50_CONSTANT_RELOCATION {
  uint64_t Offset;
  uint64_t TargetOffset;
};

/*
 * Relocation target naming the device-global 64-bit atomic lock table instead of an
 * offset inside the immutable constant pool. The runtime writes the table's GPU
 * address at `Offset`. The table is SM50_ATOMIC_LOCK_TABLE_ENTRIES zero-initialized
 * 32-bit words shared by every pipeline on one MTLDevice, so returning 64-bit
 * atomics on raw/structured UAVs (which Apple GPUs do not provide natively) serialize
 * across aliasing resources, pipelines and queues.
 */
#define SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE (~(uint64_t)0)
#define SM50_ATOMIC_LOCK_TABLE_ENTRIES 65536u

/*
 * Highest DXIL shader model the compiler accepts (major << 4 | minor, the
 * D3D_SHADER_MODEL encoding). Containers declaring a higher model are rejected.
 */
#define SM50_MAX_DXIL_SHADER_MODEL 0x68u

struct SM50_COMPILED_BITCODE {
  sm50_ptr64_t Data;
  uint64_t Size;
  /* Borrowed until SM50DestroyBitcode. Upload once per PSO, not per draw.
   * Relocations contain pool-relative 64-bit pointer targets. */
  sm50_ptr64_t ConstantData;
  uint64_t ConstantDataSize;
  uint64_t ConstantDataAlignment;
  sm50_ptr64_t ConstantRelocations;
  uint64_t ConstantRelocationCount;
};

#ifdef __cplusplus

inline uint32_t
GetArgumentIndex(SM50BindingType Type, uint32_t SM50BindingSlot) {
  switch (Type) {
  case SM50BindingType::ConstantBuffer:
    return SM50BindingSlot;
  case SM50BindingType::Sampler:
    return SM50BindingSlot + 32;
  case SM50BindingType::SRV:
    return SM50BindingSlot * 3 + 128;
  case SM50BindingType::UAV:
    return SM50BindingSlot * 3 + 512;
  }
};

inline uint32_t GetArgumentIndex(struct MTL_SM50_SHADER_ARGUMENT &Argument) {
  return GetArgumentIndex(Argument.Type, Argument.SM50BindingSlot);
};

extern "C" {
#endif

enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE {
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT = 1,
  SM50_SHADER_COMMON = 2,
  SM50_SHADER_PSO_PIXEL_SHADER = 3,
  SM50_SHADER_IA_INPUT_LAYOUT = 4,
  SM50_SHADER_GS_PASS_THROUGH = 5,
  SM50_SHADER_PSO_GEOMETRY_SHADER = 6,
  SM50_SHADER_PSO_TESSELLATOR = 7,
  SM50_SHADER_ROOT_SIGNATURE = 8,
  SM50_SHADER_ROOT_SIGNATURE2 = 9,
  SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE = 10,
  /* Header-only argument: interpret native vertex IDs as application IA positions. */
  SM50_SHADER_D3D12_INDEX_PULL = 11,
  /* Header-only argument: count this shader stage's actual invocations. */
  SM50_SHADER_D3D12_PIPELINE_STATS = 12,
  SM50_SHADER_D3D12_MSAA_PIXEL = 13,
  SM50_SHADER_D3D12_MSAA_VERTEX = 14,
  SM50_SHADER_D3D12_TESS_CAPTURE = 15,
  SM50_SHADER_GS_TESSELLATION_INPUT = 16,
  /* GS mesh: per-stream stream-output capture and rasterized-stream selection (SM50_SHADER_D3D12_GS_STREAMS_DATA).
   * The GS mesh function takes this instead of SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE. */
  SM50_SHADER_D3D12_GS_STREAMS = 17,
  /* Header-only argument, with SM50_SHADER_D3D12_PIPELINE_STATS on a pixel-frequency PS that runs natively
   * multisampled: count PSInvocations once per covered pixel. Metal runs such a PS once per covered sample, all
   * with the same [[sample_mask]]; only the invocation of the lowest covered sample counts. Not valid with
   * [[early_fragment_tests]] (the runtime rejects that combination: the lowest covered sample may not run). */
  SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL = 18,
  /* PS: apply an output-merger logic op in the epilogue by framebuffer fetch (SM50_SHADER_D3D12_LOGIC_OP_DATA). */
  SM50_SHADER_D3D12_LOGIC_OP = 19,
  /* Header-only argument, with SM50_SHADER_D3D12_MSAA_PIXEL (COVERAGE/PIXEL/REPLAY variants): variable-rate coarse
   * pixel shading. SM50_MSAA_TILE_CONTROL.shading_rate (a D3D12_SHADING_RATE) selects the coarse pixel size of the
   * tile; coarse pixels are aligned to the 8x8 tile. Input records are indexed by coarse pixel, inputs are
   * evaluated at the coarse pixel center, the pixel pass runs once per coarse pixel on a virtual coarse grid in the
   * tile's top-left corner, and the replay broadcasts the coarse output to every covered fine sample. Ignored
   * (fine shading, layout flag clear) for shaders D3D12 always shades at fine rate. */
  SM50_SHADER_D3D12_COARSE_SHADING = 31,
  /* Header-only argument, with SM50_SHADER_D3D12_MSAA_VERTEX (replay) or SM50_SHADER_D3D12_MSAA_PIXEL: D3D12
   * conservative rasterization of triangles. The replay vertex function reads 12-word index slots (9 polygon
   * corner ids slot * 8 + corner, then the 3 original ordinals) and emits the screen rectangle around the
   * primitive, clipped to positive W, with every output extrapolated on the primitive's plane; it also outputs
   * the primitive's clip positions for msaa_gather_conservative_depth. The pixel variants read their geometric
   * coverage from SM50_MSAA_PIXEL_LAYOUT.line_mask_dword. */
  SM50_SHADER_D3D12_CONSERVATIVE_RASTER = 29,
  /* Post-raster-stage mesh output capture and captured vertex replay (GS, DS and application MS). */
  SM50_SHADER_D3D12_MSAA_MESH = 30,
  /* Header-only argument for natively multisampled GS/DS mesh draws under pipeline statistics whose PS uses
   * ForceEarlyDepthStencil with depth or stencil: early tests then run an arbitrary subset of a pixel's covered
   * samples, so the lowest-covered-sample rule of PIPELINE_STATS_PER_PIXEL does not hold.
   * GS/DS mesh function (with SM50_SHADER_D3D12_PIPELINE_STATS): every output primitive carries a flat uint4 key
   * in primitive data 0 (user(dxmt_statistics_key)), unique among the draw's primitives and never all-zero.
   * PS (with SM50_SHADER_D3D12_PIPELINE_STATS, instead of PER_PIXEL): reads that key and, under
   * raster_order_group(0), claims its masked samples in the fragment buffer at
   * SM50_BINDING_INDEX_STATISTICS_CLAIM; PSInvocations counts once per (pixel, primitive) that ran at least one
   * invocation (see SM50_STATISTICS_CLAIM_HEADER). */
  SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM = 42,
  /* Native ICB vertex variant: route view locations in shader outputs, not encoder mappings. */
  SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS = 43,
  SM50_SHADER_ARGUMENT_TYPE_MAX = 0xffffffff,
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_STREAM_OUTPUT_ELEMENT {
  uint32_t reg_id;
  uint32_t component;
  uint32_t output_slot;
  uint32_t offset;
};

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  struct SM50_STREAM_OUTPUT_ELEMENT *elements;
};

/*
 * D3D12 stream-output capture for the vertex stage (no GS: stream 0 only).
 *
 * The capture happens inside the application's single real draw. Records are
 * keyed by IA position, never by index value:
 *
 *   key    = [[vertex_id]] - [[base_vertex]]      (IA position k in the instance)
 *   record = ([[instance_id]] - [[base_instance]]) * control.positions_per_instance
 *          + key - control.first_position
 *
 * Non-indexed draws are issued as-is (Metal [[base_vertex]] = vertexStart).
 * Indexed draws are issued by the runtime as indexed draws over a generated
 * uint32 buffer holding k (or 0xFFFFFFFF where the D3D12 strip cut restarts a
 * strip) with base vertex 0, and the shader pulls the application's index
 * itself (control.index_size != 0): SV_VertexID = the index value and the IA
 * vertex fetch uses index + control.base_vertex (D3D11.3 FS 8.16). The index is
 * read as 16-bit words from the buffer at SM50_BINDING_INDEX_VERTEX_INDICES
 * starting at word control.index_first.
 *
 * Scratch layout: record_count records of record_dwords dwords, followed by
 * record_count claim words that the runtime zeroes before every execution. The
 * first invocation of a key wins the claim word by compare-exchange and alone
 * writes the whole record, so a vertex the GPU shades more than once never
 * leaves a record mixing the outputs of two invocations. Records whose index
 * falls outside [0, control.record_count) are not written. A runtime compute
 * pass later assembles primitives in API order and copies them to the targets.
 *
 * Each element copies `component_count` dwords of the VS output element named
 * (`semantic_name`, `semantic_index`) starting at `start_component` to
 * `dword_offset` inside the record. `start_component` is relative to the
 * element's first masked component (D3D12_SO_DECLARATION_ENTRY::StartComponent),
 * not to the packed register. Names compare case-insensitively
 * ("SV_Position" matches SV_POSITION). An element the output signature lacks,
 * or a component range outside its mask, fails compilation. The record layout
 * is private to the translator (not the target buffer layout).
 */
struct SM50_SO_CAPTURE_ELEMENT {
  const char *semantic_name;
  uint32_t semantic_index;
  uint32_t start_component;
  uint32_t component_count;
  uint32_t dword_offset;
};

struct SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t record_dwords;
  uint32_t num_elements;
  const struct SM50_SO_CAPTURE_ELEMENT *elements;
};

/*
 * D3D12 stream output and rasterized-stream selection after a GS (mesh function).
 *
 * `rasterized_stream` (0-3, or ~0u for none) selects the stream whose emits and output
 * declarations become the mesh output; with none, stream 0 still drives the (unrasterized)
 * mesh output. Every stream s with streams[s].num_elements != 0 is captured: each GS
 * invocation key (see SM50_SO_CAPTURE_CONTROL; record_count is the key count K) owns
 * dcl_maxout records of streams[s].record_dwords + 1 dwords (the last is the vertex's
 * position inside its output strip), followed by K header words holding the number of
 * captured vertices. The regions of the captured streams follow each other in stream order
 * in the one scratch buffer: stream s starts at dword
 *   K * sum over captured t < s of (dcl_maxout * (streams[t].record_dwords + 1) + 1).
 * At most dcl_maxout vertices are captured per invocation and stream.
 * Element names refer to the output signature of their stream.
 */
struct SM50_GS_STREAM_CAPTURE {
  uint32_t record_dwords;
  uint32_t num_elements;
  const struct SM50_SO_CAPTURE_ELEMENT *elements;
};

struct SM50_SHADER_D3D12_GS_STREAMS_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t rasterized_stream;
  struct SM50_GS_STREAM_CAPTURE streams[4];
};

/* Index size 0 preserves native vertex IDs; 2/4 pulls a R16/R32 application
 * index. index_first is in 16-bit words relative to the bound index buffer.
 * Native non-indexed draws start at vertex 0; BaseVertexLocation affects
 * vertex fetching, never SV_VertexID. */
struct SM50_INDEX_PULL_CONTROL {
  uint32_t index_size;
  uint32_t index_first;
  int32_t base_vertex;
  uint32_t reserved;
};

/* Bound at SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL (constant address space). */
struct SM50_SO_CAPTURE_CONTROL {
  uint32_t positions_per_instance;
  uint32_t first_position;
  uint32_t record_count;
  uint32_t index_size;  /* 0: non-indexed, 2: R16_UINT, 4: R32_UINT */
  uint32_t index_first; /* first index, in 16-bit words from the bound index buffer */
  int32_t base_vertex;  /* BaseVertexLocation of an indexed draw */
  uint32_t padding[2];
};

struct SM50_MSAA_VERTEX_CAPTURE_CONTROL {
  struct SM50_SO_CAPTURE_CONTROL capture;
  uint32_t strip_cut_enabled;
  uint32_t active_positions_per_instance;
};

/* Each fragment variant is compiled independently from the original shader.
 * Scratch strides are in 32-bit words. Each 8x8 tile has 64 real input records
 * followed by 64 separate quad-neighbor records; the latter retain values
 * from lanes outside primitive coverage for the 1x shader's helper derivatives.
 * Claim and output-valid words are zeroed before each primitive/tile.
 */
enum SM50_MSAA_VARIANT {
  SM50_MSAA_COVERAGE = 1,
  SM50_MSAA_PIXEL = 2,
  SM50_MSAA_REPLAY = 3,
  SM50_MSAA_VERTEX_CAPTURE = 4,
  SM50_MSAA_VERTEX_REPLAY = 5,
  SM50_MSAA_SAMPLE_APP = 6,
  SM50_MSAA_OBJECT_CAPTURE = 7,
  SM50_MSAA_OBJECT_REPLAY = 8,
};

enum SM50_MSAA_LINE_MODE {
  SM50_MSAA_LINE_NONE = 0,
  SM50_MSAA_LINE_ALIASED = 1,
  SM50_MSAA_LINE_ALPHA_AA = 2,
  SM50_MSAA_LINE_QUADRILATERAL = 3,
};

struct SM50_MSAA_INTERPOLATION_LAYOUT {
  uint32_t no_perspective_count;
  uint64_t no_perspective_user_hashes[128];
  uint32_t flat_count;
  uint64_t flat_user_hashes[128];
};

struct SM50_MSAA_TILE_CONTROL {
  uint32_t origin_x;
  uint32_t origin_y;
  uint32_t sample_count;
  uint32_t sample_mask;
  float viewport_min_depth;
  float viewport_max_depth;
  uint32_t primitive_id;
  uint32_t primitive_slot;
  float viewport_origin_x;
  float viewport_origin_y;
  float viewport_width;
  float viewport_height;
  /* Forced-sample draws: PSO SampleMask restricted to the output samples (occlusion of forced-1 MSAA RTVs). */
  uint32_t output_mask;
  /* ForcedSampleCount 16 runs two native 8x halves; logical sample index of this pass's native sample 0. */
  uint32_t sample_base;
  /* Coarse shading variants: D3D12_SHADING_RATE of this tile (log2 width << 2 | log2 height); 0 = 1x1. */
  uint32_t shading_rate;
  uint32_t shading_rate_image_width, shading_rate_image_height;
};

struct SM50_MSAA_PIXEL_LAYOUT {
  uint32_t input_record_dwords;
  uint32_t output_record_dwords;
  uint32_t input_claim_dword;
  uint32_t input_coverage_dword;
  uint32_t output_valid_dword;
  uint32_t output_coverage_dword;
  uint32_t flags;
  uint32_t tile_width;
  uint32_t input_record_count;
  uint32_t helper_record_base;
  uint32_t center_z_dword;
  uint32_t sample_z_dword;
  uint32_t z_claim_dword;
  uint32_t z_anchor_dword;
  uint32_t line_mode;
  uint32_t line_mask_dword;
  uint32_t line_alpha_dword;
};

enum SM50_MSAA_PIXEL_FLAG {
  SM50_MSAA_PIXEL_SAMPLE_FREQUENCY = 1 << 0,
  SM50_MSAA_PIXEL_FORCE_EARLY = 1 << 1,
  SM50_MSAA_PIXEL_DEPTH = 1 << 2,
  SM50_MSAA_PIXEL_STENCIL = 1 << 3,
  SM50_MSAA_PIXEL_COVERAGE = 1 << 4,
  SM50_MSAA_PIXEL_PULL_INPUT = 1 << 5,
  SM50_MSAA_PIXEL_CENTROID_POSITION = 1 << 6,
  SM50_MSAA_PIXEL_DISTANCE_INPUTS = 1 << 7,
  /* Compiled with SM50_SHADER_D3D12_COARSE_SHADING and eligible: tile.shading_rate is honored. */
  SM50_MSAA_PIXEL_COARSE = 1 << 8,
};

struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t variant;
  uint32_t sample_count;
  struct SM50_MSAA_PIXEL_LAYOUT *layout_out;
  bool depth_attachment;
  bool single_sample_target;
  uint32_t line_mode;
  struct SM50_MSAA_INTERPOLATION_LAYOUT *interpolation_out;
  const struct SM50_MSAA_VERTEX_LAYOUT *line_vertex_layout;
  bool center_pattern;
  const struct SM50_MSAA_VERTEX_LAYOUT *distance_vertex_layout;
  uint32_t distance_vertex_count;
  const struct SM50_MSAA_VERTEX_LAYOUT *coarse_vertex_layout;
};

struct SM50_MSAA_CAPTURED_USER {
  uint64_t hash;
  uint32_t offset_dwords;
  uint32_t size_bytes;
  uint32_t flags; /* bit 0: spilled; bit 1: per-primitive (always flat). */
};

struct SM50_MSAA_VERTEX_LAYOUT {
  uint32_t record_dwords;
  uint32_t position_offset_dwords;
  uint32_t viewport_index_offset_dwords;
  uint32_t render_target_array_index_offset_dwords;
  uint32_t clip_distance_offset_dwords;
  uint32_t cull_distance_offset_dwords;
  uint32_t user_count;
  uint32_t shading_rate_offset_dwords; /* ~0u when SV_ShadingRate is not written. */
  uint32_t primitive_id_offset_dwords; /* ~0u when the stage does not export a primitive ID. */
  uint32_t spill_count; /* Whole user fields reconstructed from capture rather than native interpolants. */
  struct SM50_MSAA_CAPTURED_USER users[128];
};

struct SM50_SHADER_D3D12_MSAA_VERTEX_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t variant;
  uint32_t sample_count;
  struct SM50_MSAA_VERTEX_LAYOUT *layout_out;
  uint32_t line_mode;
  const struct SM50_MSAA_INTERPOLATION_LAYOUT *interpolation;
  bool depth_clip_enable;
};

struct SM50_MSAA_MESH_LAYOUT {
  uint32_t max_vertices;
  uint32_t max_primitives;
  uint32_t vertices_per_primitive;
  uint32_t threads_per_group;
  /* Output records first, then device-memory output staging, indices, count and cull flags. */
  uint32_t capture_dwords_per_group;
  uint32_t max_workloads; /* DS workloads per patch; GS instances are supplied by the PSO. */
};

/* Slot 13, constant uint4. GS/DS use groups_x as the input primitive/patch count.
 * Application MS uses groups_x/y to flatten the native group position. The stage-specific
 * dxmt.msaa.mesh.slot marker overrides that flattening for GS/DS. */
struct SM50_MSAA_MESH_CONTROL {
  uint32_t groups_x;
  uint32_t groups_y;
  uint32_t group_base;
  uint32_t total_slots;
};

struct SM50_SHADER_D3D12_MSAA_MESH_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t variant; /* SM50_MSAA_VERTEX_CAPTURE or SM50_MSAA_VERTEX_REPLAY */
  struct SM50_MSAA_VERTEX_LAYOUT *layout_out;
  struct SM50_MSAA_MESH_LAYOUT *mesh_layout_out;
  uint32_t line_mode;
  const struct SM50_MSAA_INTERPOLATION_LAYOUT *interpolation;
  bool depth_clip_enable;
  struct SM50_MSAA_OBJECT_LAYOUT *object_layout_out;
};

struct SM50_MSAA_OBJECT_LAYOUT {
  uint32_t payload_bytes;
  uint32_t record_dwords; /* uint4 child grid followed by byte-exact payload. */
};

/* D3D12 TS+GS two-pass capture layout (per PSO, compile-time constants).
 * Scratch (SM50_BINDING_INDEX_TESS_CAPTURE), in dwords:
 *   counts at 0: uint prim_count[slot_count] (zero-filled by the runtime)
 *   A = align_up(slot_count, 4): vertex (s, v, r) int4 at A + ((s*V + v)*R + r)*4
 *   B = A + slot_count*V*R*4: index (s, p, k) at B + (s*P + p)*K + k
 * where slot s = (instance * patch_count + patch_id) * W + rank. */
typedef struct SM50_TESS_CAPTURE_LAYOUT {
  uint32_t max_workloads;      /* W */
  uint32_t max_vertices;       /* V */
  uint32_t max_primitives;     /* P */
  uint32_t vertex_registers;   /* R */
  uint32_t primitive_vertices; /* K */
} SM50_TESS_CAPTURE_LAYOUT;

/* SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM: bound at SM50_BINDING_INDEX_STATISTICS_CLAIM (fragment, device).
 * `entries` is the dword offset from the header of the per-sample entries: 4 dwords {key.xyz, epoch} per sample,
 * sample s of pixel (x, y, layer) at entries + (((layer * height + y) * width + x) * samples + s) * 4, zero-filled
 * before their first draw. `epoch` (never 0) separates draws sharing the entries.
 * Metal runs a pixel-frequency PS once per covered sample, every invocation with the same [[sample_mask]], and
 * raster order groups only order invocations whose samples overlap, so two primitives with disjoint masks may
 * interleave in one pixel (probe build/local/p3/p2last/probe/quads3.mm). The claim therefore writes the key to every
 * masked sample and an invocation counts unless one of its masked samples already holds its key (quads4.mm). */
typedef struct SM50_STATISTICS_CLAIM_HEADER {
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t epoch;
  int32_t entries;
  uint32_t mesh_groups; /* mesh-shader key counter: 0 before the draw; each MS threadgroup takes one value */
  uint32_t samples;     /* raster sample count (entries per pixel) */
  uint32_t reserved;
} SM50_STATISTICS_CLAIM_HEADER;

/* Bound at SM50_BINDING_INDEX_TESS_CAPTURE_CONTROL (constant address space). */
typedef struct SM50_TESS_CAPTURE_CONTROL {
  uint32_t slot_count;   /* instance_count * patch_count * W */
  uint32_t patch_count;  /* VertexCountPerInstance / input_control_points */
  uint32_t total_offset; /* list dword index of the compacted total (0) */
  uint32_t reserved;
} SM50_TESS_CAPTURE_CONTROL;

/* DS mesh in capture mode (pass 1). When paired with STREAM_OUTPUT_CAPTURE, this only returns the SO scratch
 * layout: max_vertices counts expanded primitive records plus vertex/index staging; rasterization is independent. */
typedef struct SM50_SHADER_D3D12_TESS_CAPTURE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  SM50_TESS_CAPTURE_LAYOUT *layout_out;
} SM50_SHADER_D3D12_TESS_CAPTURE_DATA;

/* GS object + GS mesh reading the pass-1 capture (pass 2). */
typedef struct SM50_SHADER_GS_TESSELLATION_INPUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  SM50_TESS_CAPTURE_LAYOUT layout;
} SM50_SHADER_GS_TESSELLATION_INPUT_DATA;

enum SM50_SHADER_METAL_VERSION {
  SM50_SHADER_METAL_310 = 310,
  SM50_SHADER_METAL_320 = 320,
  SM50_SHADER_METAL_MAX = 0xffffffff,
};

enum SM50_SHADER_FLAG {
  SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO = 1 << 0,
  SM50_SHADER_FLAG_DEFUSE_FMA = 1 << 1,
};

struct SM50_SHADER_COMMON_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flags;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  /** MTLPixelFormat */
  uint32_t pixel_formats[8];
  /** Actual Metal sample count for this native pixel entry (1 for virtual APP passes). */
  uint32_t native_sample_count;
};

/* Output-merger logic op by framebuffer fetch. For every render target bit in rt_mask the PS output is replaced by
 * op(output, [[color(n)]]) masked to component_bits[n][c] (0 = component absent). rt_mask contains UINT
 * attachments only (FS 17.7.1); mixed non-UINT outputs retain their ordinary format conversion/quantization.
 * Color write masks remain pipeline state. D3D12 requires one op and IndependentBlendEnable FALSE.
 * Fetch runs at sample rate: pixel-frequency MSAA must execute the application/stats once in MSAA_PIXEL,
 * save its outputs, and apply this argument only to MSAA_REPLAY. MSAA_SAMPLE_APP may apply it directly.
 * Single-sample entries apply it directly, including UAV effects, coverage and depth outputs. */
struct SM50_SHADER_D3D12_LOGIC_OP_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  /** D3D12_LOGIC_OP */
  uint32_t op;
  uint32_t rt_mask;
  uint8_t component_bits[8][4];
};

/* Only native ICB vertex variants request this argument; direct draws retain native view mappings.
 * Offsets are indexed by the logical SV_ViewID, not the ordinal of an enabled view.
 * The existing root view_id qword packs logical ViewID in bits0..31, effective viewport count
 * in bits32..47 and effective render-target array length in bits48..63. Each output is its
 * shader-provided uint index (or0 if absent) plus the selected offset, clamped to0 when out of
 * its respective unsigned bound. No new root layout or binding is introduced. */
struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t view_count;
  uint32_t viewport_offsets[4];
  uint32_t render_target_offsets[4];
};

struct SM50_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  /** MTLAttributeFormat */
  uint32_t format;
  uint32_t step_function: 1;
  uint32_t step_rate: 31;
};

enum SM50_INDEX_BUFFER_FORMAT {
  SM50_INDEX_BUFFER_FORMAT_NONE = 0,
  SM50_INDEX_BUFFER_FORMAT_UINT16 = 1,
  SM50_INDEX_BUFFER_FORMAT_UINT32 = 2,
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  struct SM50_IA_INPUT_ELEMENT *elements;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

struct SM50_SHADER_ROOT_SIGNATURE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const void* bytecode;
  size_t bytecode_length;
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
);
AIRCONV_API void SM50Destroy(sm50_shader_t pShader);
/** Whether this pixel shader has an ordered buffer UAV reached through a visible descriptor table. */
AIRCONV_API int SM50NeedsRasterOrderedBufferMarker(
  sm50_shader_t pShader, const void *pRootSignature, size_t RootSignatureLength
);
AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
enum SM50_COMPILED_BITCODE_PART {
  SM50_COMPILED_BITCODE_PART_LIBRARY = 0,
  SM50_COMPILED_BITCODE_PART_CONSTANTS = 1,
  SM50_COMPILED_BITCODE_PART_RELOCATIONS = 2,
};

/**
 * Copies one part of a compiled bitcode into caller memory. Size must equal the part's byte size
 * (ConstantRelocationCount * sizeof(SM50_CONSTANT_RELOCATION) for relocations). This is how a
 * 32-bit (WoW64) image reads parts whose compiler-owned address is outside its address space.
 * Returns 0 on success.
 */
AIRCONV_API int SM50CopyCompiledBitcode(
  sm50_bitcode_t pBitcode, enum SM50_COMPILED_BITCODE_PART Part, void *pDestination, uint64_t Size
);
AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode);
AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize);
AIRCONV_API void SM50FreeError(sm50_error_t pError);

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
);

#ifdef __cplusplus
};

inline std::string SM50GetErrorMessageString(sm50_error_t pError) {
  std::string str;
  str.resize(256);
  auto size = SM50GetErrorMessage(pError, str.data(), str.size());
  str.resize(size);
  return str;
};

#endif

#endif
