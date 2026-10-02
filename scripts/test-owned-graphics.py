#!/usr/bin/env python3
"""Run a source-built owned graphics fixture in a fresh, isolated Wine prefix."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from typing import NamedTuple
from owned_graphics_arch import execution_architecture, macho_architectures


SUITE_PREFIXES = {
    "smoke": "OWNED_D3D12_SMOKE",
    "multiqueue": "OWNED_D3D12_MULTIQUEUE",
    "capabilities": "OWNED_CAPABILITIES",
    "temporal": "OWNED_D3D12_TEMPORAL",
    "ngx": "OWNED_D3D12_NGX",
    "ngx-contracts": "OWNED_D3D12_NGX_CONTRACTS",
    "fsr": "OWNED_D3D12_FSR",
    "fsr-fg": "OWNED_D3D12_FSR_FG",
    "core-contracts": "OWNED_D3D12_CORE_CONTRACTS",
    "execution-safety": "OWNED_D3D12_EXECUTION_SAFETY",
    "fence-events": "OWNED_D3D12_FENCE_EVENTS",
    "pipeline-cache": "OWNED_D3D12_PIPELINE_CACHE",
    "pipeline-library": "OWNED_D3D12_PIPELINE_LIBRARY",
    "bundles": "OWNED_D3D12_BUNDLES",
    "queries": "OWNED_D3D12_QUERY",
    "pipeline-statistics": "OWNED_D3D12_PIPELINE_STATISTICS",
    "resources": "OWNED_D3D12_RESOURCES",
    "buffer-rtv": "OWNED_D3D12_BUFFER_RTV",
    "atomic-copy": "OWNED_D3D12_ATOMIC_COPY",
    "atomic-copy-live": "OWNED_D3D12_ATOMIC_COPY",
    "present-queues": "OWNED_D3D12_PRESENT_QUEUES",
    "hdr-metadata": "OWNED_D3D12_HDR_METADATA",
    "colorspace": "OWNED_D3D12_COLORSPACE",
    "fullscreen": "OWNED_DXGI_FULLSCREEN",
    "stream-output": "OWNED_D3D12_STREAM_OUTPUT",
    "so-format-support": "OWNED_D3D12_SO_FORMAT_SUPPORT",
    "format-support": "OWNED_D3D12_FORMAT_SUPPORT",
    "expanded-formats": "OWNED_D3D12_EXPANDED_FORMATS",
    "partial-3d-uav-dxil": "OWNED_D3D12_PARTIAL_3D_UAV",
    "packed-cubearray": "OWNED_D3D12_PACKED_CUBE",
    "standard-swizzle": "OWNED_D3D12_STANDARD_SWIZZLE",
    "multisample-capabilities": "OWNED_D3D12_MULTISAMPLE_CAPABILITIES",
    "msaa-lines": "OWNED_D3D12_MSAA_LINES",
    "raster-order": "OWNED_D3D12_RASTER_ORDER",
    "stencil-reference": "OWNED_D3D12_STENCIL_REFERENCE",
    "depth-plane-copy": "OWNED_D3D12_DEPTH_PLANE_COPY",
    "indexed-restart": "OWNED_D3D12_INDEXED_RESTART",
    "residency": "OWNED_D3D12_RESIDENCY",
    "copy-resolve": "OWNED_D3D12_COPY_RESOLVE",
    "copy-resource-casts": "OWNED_D3D12_COPY_RESOURCE_CASTS",
    "reinterpret-copy": "OWNED_D3D12_REINTERPRET",
    "predication-indirect": "OWNED_D3D12_PREDICATION_INDIRECT",
    "dxil": "OWNED_D3D12_DXIL",
    "dxil-ia": "OWNED_D3D12_DXIL",
    "dxil-static-sampler": "OWNED_D3D12_DXIL_STATIC_SAMPLER",
    "dxil-texture": "OWNED_D3D12_DXIL_TEXTURE",
    "raytracing": "OWNED_D3D12_RAYTRACING",
    # BINDLESS: Tier 3, SM 6.6 dynamic resources, root signature 1.2.
    "bindless": "OWNED_D3D12_BINDLESS",
    # BARRIERS: enhanced barriers, Device10 layouts/castable formats, CommandList8/9 dynamic state.
    "enhanced-barriers": "OWNED_D3D12_ENHANCED_BARRIERS",
    # BARRIERS: render passes (tier 0), GPU upload heaps, CreateSampler2 UINT borders.
    "render-passes": "OWNED_D3D12_RENDER_PASSES",
    # TILED: reserved resources at TiledResourcesTier 2.
    "tiled": "OWNED_D3D12_TILED",
    # GSTS: GS and HS/DS pipelines (DXBC + DXIL).
    "geometry": "OWNED_D3D12_GEOMETRY",
    "tessellation": "OWNED_D3D12_TESSELLATION",
    # SM: Shader Model 6.x DXIL features against CPU oracles.
    "sm6": "OWNED_D3D12_SM6",
    # SHARING: shared heaps/resources/fences across devices and processes.
    "sharing": "OWNED_D3D12_SHARING",
    # FL: feature level 11_1/12_0 requirements (logic ops, forced sample counts, TIR).
    "feature-level": "OWNED_D3D12_FEATURE_LEVEL",
    "conservative-raster": "OWNED_D3D12_CONSERVATIVE_RASTER",
    # VIEWINST: ViewInstancingTier 1 (draw-level looping) with exact pixels.
    "view-instancing": "OWNED_D3D12_VIEW_INSTANCING",
    # MESH: MeshShaderTier 1 (AS/MS pipelines, DispatchMesh, ExecuteIndirect DISPATCH_MESH) with exact pixels.
    "mesh-shader": "OWNED_D3D12_MESH_SHADER",
    # FEEDBACK: SamplerFeedbackTier 1.0 (feedback maps, WriteSamplerFeedback*, transcoding) against a CPU oracle.
    "sampler-feedback": "OWNED_D3D12_SAMPLER_FEEDBACK",
    "work-graphs": "OWNED_D3D12_WORK_GRAPHS",
    # VRS: Tier2 per-draw, primitive and image rates against exact coarse-pixel references.
    "vrs": "OWNED_D3D12_VRS",
}
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SHARING_CASES = (
    "buffer-a_to_b", "texture-a_to_b", "fence-a_to_b",
    "buffer-b_to_a", "texture-b_to_a", "fence-b_to_a",
    "cpu-signal-gpu-wait", "heap-placed", "heap-row-major-texture", "named-open", "errors",
    "fence-rewind", "fence-producer-completion-order", "fence-stress", "named-unrelated-process",
    "host-memory-address", "host-memory-file-mapping",
    "cross-process-parent-to-child", "cross-process-child-to-parent",
    "cross-process-unobserved-rewind-latches",
)
DEFAULT_LOCK = REPOSITORY_ROOT / "graphics-translator/upstream.lock.json"
DEFAULT_DXIL_DIR = REPOSITORY_ROOT / "graphics-translator/tests/dxil"
FG_CAPTURE_CHECKER = REPOSITORY_ROOT / "graphics-translator/tests/check-owned-fg-capture.py"
# NGX and FSR fixtures run on the temporal backend and publish its identity line.
BACKEND_PREFIXES = {
    "ngx": "OWNED_D3D12_TEMPORAL",
    "ngx-contracts": "OWNED_D3D12_TEMPORAL",
    "fsr": "OWNED_D3D12_TEMPORAL",
    "fullscreen": "OWNED_DXGI_FULLSCREEN",
}
# Extra inputs a suite consumes beside the fixture binary. Smoke reads its HLSL
# by name from the fixture directory; DXIL shaders are passed as argv, so their
# order is part of the contract and is spelled out instead of globbed.
SMOKE_SHADER_GLOB = "owned-d3d12-*.hlsl"
DXIL_SHADERS = ("compute.dxil", "vertex.dxil", "pixel.dxil")
DXIL_REGRESSION_SHADERS = ("register-space.dxil", "null-descriptor.dxil")
DXIL_IA_SHADERS = ("dxil-ia-vs.dxil", "dxil-ia-ps.dxil")
DXIL_STATIC_SAMPLER_SHADERS = ("vs.dxil", "ps.dxil")
DXIL_TEXTURE_SHADERS = ("basic-texture-vs.dxil", "basic-texture-ps.dxil")
PREDICATION_INDIRECT_SHADERS = ("predication-indirect-cs.dxil", "predication-indirect-vs.dxil")
ATOMIC_COPY_SHADERS = ("atomic-copy-reader.dxil",)
STREAM_OUTPUT_SHADERS = ("stream-output-packed-vs.dxil",)
# GS multi-stream cases load these by name beside the fixture (not passed on the command line).
STREAM_OUTPUT_GS_INPUTS = ("stream-output-gs-vs_gs.dxil", "stream-output-gs-gs_multi.dxil")
PARTIAL_3D_UAV_SHADERS = ("partial-3d-uav.dxil",)
# SM: frozen DXIL read by name (sm6-<entry>.dxil) beside the executable; the HLSL and
# its build script are frozen with them so the evidence names the exact sources.
SM6_SOURCES = ("sm6.hlsl", "sm6-16bit.hlsl", "sm6-heap.hlsl", "sm6-reject.hlsl", "sm6-denorm.hlsl", "build-sm6.sh")
SM6_ENTRIES = (
    "cs_wave", "cs_divergent", "cs_int64", "cs_atomic64", "cs_atomic64_ops", "cs_atomic64_heap", "cs_minprec",
    "cs_quad_deriv", "vs_main", "ps_main", "ps_helper", "ps_helper_all", "vs_tiny", "ps_atomic64", "ps_umax64",
    "vs_int64", "ps_int64", "vs_wave", "ps_null", "cs_16bit", "cs_packed", "vs_16bit", "ps_16bit", "cs_wavesize32", "cs_wavesize64",
    "cs_fp64", "cs_gs64", "cs_sm69", "vs_viewid", "ps_viewid",
    "ps_viewid_vi", "cs_denorm_any", "cs_denorm_ftz", "cs_denorm_preserve",
)
SM6_CASES = (
    "wave", "divergent", "int64", "atomic64_ops", "atomic64_heap", "16bit", "minprec", "packed", "quad_deriv",
    "rejections", "wavesize32", "barycentrics", "barycentrics_indexed", "helper_lanes", "helper_lanes_included",
    "16bit_io", "graphics_int64", "vs_wave", "graphics_atomic64_rejected", "sv_viewid", "sv_viewid_instanced",
    "denorm_any", "denorm_ftz", "denorm_preserve", "cs_atomic64_stress_1", "cs_atomic64_stress_16",
)
SM6_REJECTS = ["cs_wavesize64", "cs_fp64", "cs_gs64", "cs_sm69", "ps_atomic64", "ps_umax64"]
SM6_STRESS_REPS = 20
# MESH: frozen DXIL read by name (mesh-shader-<entry>.dxil) beside the executable, with its sources.
MESH_SHADER_SOURCES = ("mesh-shader.hlsl", "mesh-shader-contract.hlsl", "build-mesh-shader.sh")
MESH_SHADER_ENTRIES = (
    "ms_basic", "ms_grid", "ms_payload", "ms_cull", "ms_prim", "ms_layer", "ms_lines", "ms_max", "ms_count",
    "ms_partial", "ms_viewid", "ms_rtai", "ms_rootk", "ms_early", "as_main", "as_count", "ps_color", "ps_prim",
    "ps_layer", "ps_max", "ps_rows", "ps_early",
    "ms_contract124", "ms_contract125", "ms_contract128_perspective", "ms_contract128_affine",
    "ms_contract128_flat", "ms_contract128_scalars", "ms_contract128_budget", "ms_contract_cull", "ps_contract124", "ps_contract125",
    "ps_contract128_perspective", "ps_contract128_affine", "ps_contract128_flat", "ps_contract128_scalars", "ps_contract_cull",
    "ps_contract128_pull_perspective", "ps_contract128_pull_affine",
    "ms_contract128_split", "ps_contract128_split", "ms_contract_cull_line",
)
MESH_SHADER_CASES = (
    "ms_basic", "ms_grid", "as_payload", "ms_cull", "ms_primitive_attributes", "ms_layer_viewport", "ms_lines",
    "ms_max_256", "ms_partial_clip", "ms_indirect", "ms_basic_msaa4", "ms_cull_msaa4", "grid_limits", "rejects",
    "ms_statistics", "ms_statistics_early_depth_msaa", "ms_predication", "ms_pso_cache", "ms_view_instancing",
    "ms_indirect_root_constants",
    "ms_full_range_rtai",
    "ms_output124", "ms_output125", "ms_output128_perspective", "ms_output128_affine", "ms_output128_flat",
    "ms_output128_scalars", "ms_output128_pull_perspective", "ms_output128_pull_affine", "ms_output128_budget",
    "ms_output128_split",
    "ms_output128_coarse", "ms_output129_reject", "ms_cull_distance",
)
TILED_INPUTS = ("tiled.hlsl", "tiled-fxc.hlsl")
TILED_SHADERS = ("tiled-write.dxil", "tiled-buffer-status.dxil", "tiled-texture-status.dxil",
                 "tiled-dxbc-status.dxbc", "tiled-fxc-cs.dxbc", "tiled-fxc-vs.dxbc", "tiled-fxc-ps.dxbc",
                 "tiled-root-status.dxil", "tiled-rgb32-status.dxil")
TILED_CASES = (
    "buffer-tiling", "buffer-map-two-heaps", "buffer-null-tile", "copy-tile-mappings",
    "buffer-residency-status", "copy-tiles-buffer-to-linear", "mapping-update-at-execute-boundary",
    "invalid-update-dropped", "evict-make-resident", "reuse-single-tile", "cross-queue-mapping-order",
    "texture-tiling", "texture-null-tile", "texture-packed-mips", "texture-residency-status",
    "texture-remap-at-execute-boundary", "min-max-reduction", "dxbc-feedback-status", "fxc-feedback-status",
    "root-view-and-rgb32-status", "copy-tiles-round-trip",
    "copy-tiles-unmapped-reads-zero", "copy-tiles-invalid", "reserved-creation-validation", "caps",
)
MSAA_LINE_INPUTS = (
    "msaa-lines-input.hlsl", "msaa-lines-vsLine.dxil", "msaa-lines-vsLineEdge.dxil",
    "msaa-lines-psEval.dxil", "msaa-lines-psSample.dxil",
    "msaa-lines-psUnusedSampleDerivative.dxil",
    "msaa-lines-psSampleDerivative.dxil",
)
RASTER_ORDER_SHADERS = (
    "raster-order-vertex.dxil", "raster-order-rov-raw.dxil",
    "raster-order-rov-structured.dxbc", "raster-order-rov-structured.dxil",
    "raster-order-rov-typed.dxbc", "raster-order-rov-unbounded.dxil",
    "raster-order-mixed.dxbc", "raster-order-mixed.dxil", "raster-order-rov-spaces.dxil",
    "raster-order-rov-sample.dxil", "raster-order-layer-vertex.dxil", "raster-order-rov-layer.dxil",
)
STENCIL_REFERENCE_SHADERS = (
    "raster-order-vertex.dxil", "raster-order-stencil.dxbc", "raster-order-stencil.dxil",
)
INDEXED_RESTART_INPUTS = ("indexed-restart.hlsl", "indexed-restart-procedural-vs.dxil",
                          "indexed-restart-attribute-vs.dxil", "indexed-restart-ps.dxil")
# GSTS: the fixtures compile DXBC from the HLSL and load geometry-/tessellation-<entry>.dxil by name.
GEOMETRY_INPUTS = ("geometry.hlsl",)
TESSELLATION_INPUTS = ("tessellation.hlsl",)
PIPELINE_STATISTICS_SHADERS = ("pipeline-statistics-vs.dxil", "pipeline-statistics-ps.dxil",
                               "pipeline-statistics-cs.dxil")
# FL: frozen by dxil/build-feature-level.sh; the fixture reads feature-level-<entry>.dxil by name.
FEATURE_LEVEL_INPUTS = ("feature-level.hlsl", "build-feature-level.sh") + tuple(
    f"feature-level-{entry}.dxil" for entry in ("vs_main", "ps_coverage", "ps_color"))
# CONSRAST: frozen by dxil/build-conservative-raster.sh; the fixture reads conservative-raster-<entry>.dxil and
# compiles DXBC from the HLSL.
CONSERVATIVE_RASTER_INPUTS = ("conservative-raster.hlsl", "conservative-distance.hlsl", "build-conservative-raster.sh") + tuple(
    f"{source}-{entry}.dxil" for source in ("conservative-raster", "conservative-distance") for entry in
    ("vs_main", "ps_main", "gs_main", "hs_main", "ds_main", "ms_main", "ms_as", "as_main"))
CONSERVATIVE_RASTER_PATHS = ("vs_dxil", "vs_dxbc", "vs_gs_dxil", "vs_gs_dxbc", "vs_hs_ds_dxil", "vs_hs_ds_dxbc",
                             "vs_hs_ds_gs_dxil", "vs_hs_ds_gs_dxbc", "ms_dxil", "as_ms_dxil")
# VIEWINST: frozen by dxil/build-view-instancing.sh; the fixture reads view-instancing-<entry>.dxil by name.
VIEW_INSTANCING_INPUTS = ("view-instancing.hlsl", "build-view-instancing.sh") + tuple(
    f"view-instancing-{entry}.dxil"
    for entry in ("vs_main", "ps_uint", "ps_unorm", "gs_view", "vs_tess", "hs_view", "ds_view"))
VIEW_INSTANCING_CASES = (
    "rtai_2_views", "rtai_4_views", "rtai_out_of_range_to_0", "vpai_2_views", "vpai_4_views",
    "vpai_out_of_range_to_0", "mask_flag_0b1010", "mask_flag_upper_bits_ignored", "mask_flag_zero_draws_nothing",
    "mask_without_flag_ignored", "gs_rtai_2_views", "tess_rtai_2_views",
    "no_subobject_reads_zero_after_instanced_draw", "count0_reads_zero", "count1_location_slice2",
    "msaa4x_rtai_3_views_out_of_range_to_0", "msaa4x_vpai_3_views_out_of_range_to_0", "msaa4x_mask_0b10",
    "execute_indirect_2_views_2_commands", "bundle_mask_reset_and_leak", "bundle_starts_with_mask_0",
    "pipeline_statistics_count_views", "invalid_descs_rejected",
)
# VRS: frozen by dxil/build-vrs.sh; the fixture reads vrs-<entry>.dxil by name.
VRS_INPUTS = ("vrs.hlsl", "build-vrs.sh") + tuple(
    f"vrs-{entry}.dxil"
    for entry in ("vs_main", "ps_main", "ps_snapped", "ps_discard", "ps_coverage", "ps_depth", "ps_flat",
                  "vs_rate", "ps_rate", "ps_centroid", "ps_sample", "ps_wide_snapped",
                  "gs_main", "hs_main", "ds_main", "ms_main", "as_main", "ms_as", "gs_rate", "ms_rate", "ms_as_rate",
                  "vs_rate_view", "gs_rate_view", "ms_rate_view", "ms_as_rate_view"))
VRS_CASES = (
    "rate_1x1", "rate_1x2", "rate_2x1", "rate_2x2", "rate_2x4", "rate_4x2", "rate_4x4",
    "msaa2x_rate_2x2", "msaa2x_rate_2x4", "msaa4x_rate_1x2", "msaa4x_rate_2x2",
    "msaa4x_rate_2x4_sanitized_2x2", "msaa4x_rate_4x4_sanitized_2x2", "msaa2x_rate_4x4_sanitized_2x4",
    "msaa8x_rate_2x2_fine", "combiner_override_1x1", "combiner_min_1x1", "combiner_max_keeps_base",
    "combiner_sum_keeps_base", "discard_whole_coarse_pixels", "msaa4x_2x2_per_sample_depth",
    "msaa4x_partial_sample_mask_coarse", "depth_export_fine",
    "bundle_rate_persists", "clear_state_resets_rate",
) + tuple(
    f"stage_{stage}_{case}"
    for stage in ("gs", "ts", "tsgs", "ms", "as_ms")
    for case in ("rate_1x1", "rate_1x2", "rate_2x1", "rate_2x2", "rate_2x4", "rate_4x2", "rate_4x4", "stats_2x2")
        + (("indexed_2x2",) if stage in ("gs", "ts", "tsgs") else ())
        + ("indirect_2x2", "indirect_count_zero", "view_2x2")
) + ("stage_gs_so_2x2", "tier2_image_shapes") + tuple(f"tier2_combiners_{first}_{second}" for first in range(5) for second in range(5)) + (
    "tier2_null_image", "tier2_unbind_image", "tier2_clear_image",
    "tier2_small_image", "tier2_enhanced_image", "tier2_deny_srv_image",
    "tier2_stage_gs", "tier2_stage_ts", "tier2_stage_tsgs", "tier2_stage_ms", "tier2_stage_as_ms",
    "tier2_centroid_full", "tier2_centroid_partial", "tier2_wide_snapped", "tier2_snapped_negative",
) + tuple(f"tier2_sample_{sample}" for sample in range(16)) + ("tier2_coverage_input",) + tuple(
    f"tier2_viewport_{stage}" for stage in ("vs", "gs", "ms", "as_ms"))
# FEEDBACK: frozen by dxil/build-sampler-feedback.sh; the fixture reads sampler-feedback-<entry>.dxil by name.
SAMPLER_FEEDBACK_INPUTS = ("sampler-feedback.hlsl", "build-sampler-feedback.sh", "sampler-feedback-vs_main.dxil") + tuple(
    f"sampler-feedback-{shader}_{kind}{array}.dxil"
    for kind in ("minmip", "used") for array in ("", "_array")
    for shader in ("cs_main", "ps_implicit", "ps_clamp", "ps_bias"))
_SF_SAMPLERS = ("point_clamp", "linear_wrap", "aniso4", "linear_border", "mippoint_mirror_lod",
                "magpoint_mirroronce_bias")
_SF_SHAPES = ("pow2_minmip", "pow2_used", "npot_minmip", "npot_used", "array_minmip", "array_used")
SAMPLER_FEEDBACK_CASES = (
    "caps_and_format_support", "creation_validation", "clear_decodes_ff_and_0", "placed_map_clear",
    "encode_decode_round_trips", "copy_resource_allowed_copy_texture_region_rejected",
) + tuple(f"cs_{shape}_{sampler}" for shape in _SF_SHAPES for sampler in _SF_SAMPLERS) + (
    "cs_srv_subrange_minmip", "cs_srv_subrange_used", "cs_srv_array_subrange_used", "cs_static_sampler_used",
    "cs_null_paired_texture_drops_writes",
) + tuple(f"{op}_{_SF_SHAPES[shape]}_{_SF_SAMPLERS[sampler]}"
          for shape in (0, 1, 4, 5) for sampler in (1, 2, 4) for op in ("ps_implicit", "ps_bias", "ps_clamp"))
PIPELINE_STATISTICS_STAGE_INPUTS = (
    "pipeline-statistics-stages.hlsl",

    "pipeline-statistics-vsDistances.dxil",
    "pipeline-statistics-psDiscard.dxil",
    "pipeline-statistics-psEarly.dxil",
    "pipeline-statistics-psDepth.dxil",
    "pipeline-statistics-psPixel.dxil",
    "pipeline-statistics-psSample.dxil",
    "pipeline-statistics-vsMSAA.dxil",
    "pipeline-statistics-psMSCoverage.dxil",
    "pipeline-statistics-psMSAtomic.dxil",
    "pipeline-statistics-psMSDiscard.dxil",
    "pipeline-statistics-psMSZeroAlpha.dxil",
    "pipeline-statistics-psMSEarly.dxil",
    "pipeline-statistics-psMSDepth.dxil",
    "pipeline-statistics-psMSCentroid.dxil",
    "pipeline-statistics-psMSEvaluate.dxil",
    "pipeline-statistics-psMSPrepass.dxil",
    "pipeline-statistics-psMSOutputMask.dxil",
    "pipeline-statistics-psMSAllSnaps.dxil",
    "pipeline-statistics-vsMSArray.dxil",
    "pipeline-statistics-psMSMRT.dxil",
    "pipeline-statistics-psMSCentroidPosition.dxil",
    "pipeline-statistics-psMSEvalCentroid.dxil",
    "pipeline-statistics-psMSPrimitive.dxil",
    "pipeline-statistics-psMSDepthPosition.dxil",
    "pipeline-statistics-psMSSampleDepthPosition.dxil",
    "pipeline-statistics-psMSSampleInterpolatedDepthPosition.dxil",
    "pipeline-statistics-psMSCentroidOutputDepth.dxil",
    "pipeline-statistics-psMSSampleEarlyMask.dxil",
    "pipeline-statistics-psMSEarlyNonlinearDerivative.dxil",
    "pipeline-statistics-psMSCheckerboardPrepass.dxil",
    "pipeline-statistics-psMSPosition8.dxil",
)
DXIL_IA_MARKER = (
    "OWNED_D3D12_DXIL_IA_PASS "
    "nonindexed=(0.300,0.500,0.300,1.000) "
    "indexed=(0.600,0.200,0.410,1.000) "
    "strides=(20,24) drawparams=(2,3,2,2)"
)
DXIL_STATIC_SAMPLER_MARKER = (
    "OWNED_D3D12_DXIL_STATIC_SAMPLER_GPU "
    "cost_original=64 cost_lowered=66 vs=s6_space11 ps=s5_space7 "
    "actual=(0.625000,0.625000,0.625000,0.625000) "
    "expected=(0.625000,0.625000,0.625000,0.625000)"
)
# The owned DXIL shader compiler is built from src/airconv and linked into
# winemetal.so, so the DXIL suites prove the native
# compiler path by loading that exact module and exercising its contracts;
# there is no separate compiler dylib to load.
WINEMETAL_NATIVE_LIBRARY = "winemetal.so"
# The owned DXIL path must actually execute its descriptor-table binding and
# report the expected readback; an unsupported HRESULT is never a pass.
DXIL_DESCRIPTOR_TABLE_MARKER = "OWNED_D3D12_DXIL_DESCRIPTOR_TABLE_EXECUTED"
DXIL_DESCRIPTOR_TABLE_VALUE = "2468ace0"
# Ray tracing suite. The fixture builds a BLAS/TLAS, a DXR state object and shader
# binding tables, dispatches ten rays and asserts the exact GPU readback of what the
# ray generation / any-hit / closest-hit / miss / callable shaders did. The runner
# restates that readback contract from the fixture's own printed evidence, so a PASS
# line that is not backed by the exact per-lane payload is never accepted: a Tier-0
# capability report, a dispatch that traced nothing, or a stale fixture cannot produce
# it. Where DXR leaves the candidate enumeration order unspecified the fixture lists
# every admissible outcome and the runner accepts exactly that set.
RAYTRACING_SHADERS = ("raytracing.dxil", "raytracing-contract.dxil", "raytracing-stateobj.dxil",
                      "raytracing-rayquery-cs.dxil", "raytracing-rayquery-vs.dxil", "raytracing-rayquery-fs.dxil",
                      "raytracing-rayquery-ps.dxil", "raytracing-rayquery-lib.dxil", "raytracing-rayquery-args.dxil",
                      "raytracing-coverage.dxil", "raytracing-rootargs.dxil",
                      "raytracing-rayquery-stages-vs.dxil", "raytracing-rayquery-stages-hs.dxil",
                      "raytracing-rayquery-stages-hs-pass.dxil", "raytracing-rayquery-stages-ds.dxil",
                      "raytracing-rayquery-stages-ds-pass.dxil", "raytracing-rayquery-stages-gs.dxil",
                      "raytracing-rayquery-stages-as.dxil", "raytracing-rayquery-stages-ms.dxil",
                      "raytracing-rayquery-stages-ms-sink.dxil", "raytracing-rayquery-stages-lib.dxil",
                      "raytracing-linkage-consumer.dxil", "raytracing-linkage-helper.dxil",
                      "raytracing-linkage-final.dxil")
# Root-argument-changing ExecuteIndirect(DISPATCH_RAYS) of tests/owned-d3d12-raytracing-rootargs.inc.
RAYTRACING_ROOTARGS_CASES = ("dispatch-rays-root-arguments",)
# Coverage cases of tests/owned-d3d12-raytracing-coverage.inc (8x4x2 grids, every table at a
# non-default offset/stride with local root constants and descriptor tables), in fixture order.
RAYTRACING_COVERAGE_CASES = ("grid-direct", "grid-indirect", "predication", "bundle")
# Per-feature lane groups of the grid-direct case, each compared word by word by the fixture.
RAYTRACING_COVERAGE_FEATURES = (
    ("geometry-index", 24), ("hit-group-indexing", 8), ("miss-index", 8), ("traceray-skip-flags", 12),
    ("callable-chains", 8), ("rayquery-template-skip", 4),
)
# Every predicated dispatch: DispatchRays / ExecuteIndirect(DISPATCH_RAYS), both ops, predicate 0/1.
# EQUAL_ZERO culls on 0, NOT_EQUAL_ZERO culls on 1.
RAYTRACING_COVERAGE_PREDICATION = tuple(
    (f"predication-{kind}-{op}-{value}", int((op == "eq0") == (value == 0)))
    for op in ("eq0", "ne0") for value in (0, 1) for kind in ("direct", "indirect"))
RAYTRACING_COVERAGE_BUNDLE = (
    ("bundle-indirect", 0), ("bundle-indirect-predicated-culled", 1), ("bundle-indirect-predicated-run", 0),
    ("bundle-direct-predicated-culled", 1), ("bundle-direct-predicated-run", 0),
)
# Inline RayQuery cases of tests/owned-d3d12-raytracing-rayquery.inc (one per shader stage, plus the
# raygen dispatched through ExecuteIndirect(DISPATCH_RAYS) with GPU-written arguments), in fixture order.
RAYTRACING_RAYQUERY_CASES = ("cs", "vs", "ps", "raygen", "raygen-indirect",
                           "closesthit", "anyhit", "intersection", "miss", "callable",
                           "hs", "ds", "gs", "as", "ms")
# State-object cases of tests/owned-d3d12-raytracing-stateobj.inc, in fixture order.
RAYTRACING_STATEOBJ_CASES = (
    "dxil-subobjects", "config-flags", "recursion-depth", "stack-size", "bundle",
    "miss-ray-values", "geometry-skip-flags", "add-to-state-object",
    "generic-function-collection",
)
# Contract cases of tests/owned-d3d12-raytracing-contract.inc, in the order the fixture runs them.
RAYTRACING_CONTRACT_CASES = (
    "vertex-formats", "geometry-rules", "transforms", "instances-hit-groups",
)
RAYTRACING_AS_CASES = (
    "postbuild-inline", "postbuild-emit", "clone-blas", "clone-tlas", "compact-blas", "compact-tlas",
    "serialize-blas", "serialize-tlas-relocate", "serialize-aabb", "serialize-empty",
    "visualize-blas", "visualize-tlas", "update-blas-inplace", "update-blas-copy",
    "update-tlas-inplace", "update-tlas-copy",
)
RAYTRACING_VERTEX_FORMATS = (
    "r32g32b32_float", "r32g32_float", "r16g16_float", "r16g16b16a16_float", "r16g16_snorm",
    "r16g16b16a16_snorm", "r16g16b16a16_unorm", "r16g16_unorm", "r10g10b10a2_unorm", "r8g8b8a8_unorm",
    "r8g8_unorm", "r8g8b8a8_snorm", "r8g8_snorm",
)
# BARRIERS: DXIL library of root signature subobjects for ID3D12Device14, read beside the fixture.
ENHANCED_BARRIERS_INPUTS = ("enhanced-barriers-rs-library.dxil",)
# BINDLESS: argv order is the fixture contract.
BINDLESS_SHADERS = ("bindless-cs.dxil", "bindless-vs.dxil", "bindless-ps.dxil", "bindless-root-cs.dxil")
# Read by name from the fixture directory, like register-space.dxil for the dxil suite.
BINDLESS_INPUTS = ("bindless-dxbc51-cs.dxbc", "bindless-dxbc51-vs.dxbc", "bindless-dxbc51-ps.dxbc",
                   "bindless-rov-vs.dxil", "bindless-rov-ps.dxil")
BINDLESS_CASES = (
    "caps", "root-flags-validation", "root-signature-1.2", "cs66-dynamic-resources",
    "ps66-divergent-heap-sampling", "dxbc51-unbounded-nonuniform", "heap-order-and-past-heap",
    "ps66-heap-rasterizer-ordered-views",
)
RAYTRACING_LANE_COUNT = 10
RAYTRACING_PASS_MARKER = "OWNED_D3D12_RAYTRACING_PASS"
RAYTRACING_FAIL_MARKER = "OWNED_D3D12_RAYTRACING_FAIL"
RAYTRACING_FILL = 0xffffffff
RAYTRACING_INSTANCE = 7
RAYTRACING_FIRST_HIT_GROUP = 0x11111111
RAYTRACING_SECOND_HIT_GROUP = 0x22222222
RAYTRACING_ACCEPT_TAG = 0xA0000000
RAYTRACING_REJECT_TAG = 0xB0000000
# Lane 5 proves the hit-group SBT record's local root constant is what selects the
# record, so the callable's payload word is deliberately not a round number.
RAYTRACING_CALLABLE_VALUE = 0xCA11AB1E ^ 5
# Any-hit log encoding, mirrored by the shader: bit 31 marks the slot as written, bit 0
# is IgnoreHit, and the primitive index sits in between, so accepting and rejecting one
# primitive can never encode the same word.
RAYTRACING_ANY_HIT_LOGGED = 0x80000000
# The fixture compares ray t and barycentrics with these tolerances; the runner uses
# the same ones so a legitimate runtime is never failed over print rounding.
RAYTRACING_HIT_T_TOLERANCE = 0.001
RAYTRACING_BARY_TOLERANCE = 0.01
# The procedural dispatch's own constants. It is a second DispatchRays of the same fixture with
# its own ray generation and miss exports, and it shares the hit-group table with the triangle
# dispatch, so the AABB instance's NONZERO InstanceContributionToHitGroupIndex is what selects
# the procedural record rather than the table base.
RAYTRACING_PROCEDURAL_LANE_COUNT = 4
RAYTRACING_PROCEDURAL_INSTANCE = 8
RAYTRACING_PROCEDURAL_HIT_GROUP = 0x33333333
RAYTRACING_PROCEDURAL_ACCEPT_TAG = 0xC0000000
RAYTRACING_PROCEDURAL_REJECT_TAG = 0xD0000000
# How many ReportHit calls one intersection-shader invocation may make: a primitive that reports
# more than once gets one intersection-log word and one any-hit slot per crossing, so a crossing
# whose report was rejected before the any-hit ran leaves its slot at the fill pattern.
RAYTRACING_PROCEDURAL_CROSSINGS = 3
RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS = 4
# The intersection shader's own log encoding, mirrored by the shader: bit 31 marks the word as
# written by the shader, bit 0 is ReportHit's answer, bit 1 is whether the distance it reported
# was inside the interval the shader tracked, and bits 8-15 carry the primitive it reported for.
RAYTRACING_IS_INVOKED = 0x80000000
RAYTRACING_IS_REPORT_ACCEPTED = 0x1
RAYTRACING_IS_DISTANCE_IN_INTERVAL = 0x2
RAYTRACING_IS_PRIMITIVE_SHIFT = 8
RAYTRACING_PROCEDURAL_ATTRIBUTE_MARKER = 0x5EEDA77
RAYTRACING_PROCEDURAL_HIT_KIND_BASE = 0x70
# The reported distance and RayTCurrent() as their exact bit pattern: the shader computes the
# distance from the same constants the fixture filled the AABB buffer with, so no rounding is
# involved on either side and the runner needs no tolerance.
RAYTRACING_HALF_BITS = 0x3F000000
# The multi-report primitive: one intersection-shader invocation reporting three times along the
# same ray. Crossing 0 (t=1.0) is accepted and committed, which tightens the ray interval to 1.0;
# crossing 1 (t=5.0) is farther, so DXR must reject it without running the any-hit shader; crossing
# 2 (t=0.5) is nearer and inside the interval, so the any-hit shader runs and rejects it with
# IgnoreHit. The committed hit must stay crossing 0's.
RAYTRACING_MULTI_REPORT_PRIMITIVE = 3
RAYTRACING_MULTI_REPORT_ENTRY0_BITS = 0x3F800000
RAYTRACING_MULTI_REPORT_ENTRY1_BITS = 0x40A00000
RAYTRACING_MULTI_REPORT_ENTRY2_BITS = 0x3F000000
RAYTRACING_MULTI_REPORT_TIGHTENED_CROSSING = 1


def raytracing_intersection_log(primitive: int, accepted: bool, in_interval: bool) -> int:
    return (RAYTRACING_IS_INVOKED
            | (RAYTRACING_IS_REPORT_ACCEPTED if accepted else 0)
            | (RAYTRACING_IS_DISTANCE_IN_INTERVAL if in_interval else 0)
            | (primitive << RAYTRACING_IS_PRIMITIVE_SHIFT))


def raytracing_procedural_any_hit_slot(primitive: int, rejected: bool, distance_bits: int) -> tuple[int, int, int, int]:
    """One any-hit slot: decision, reported marker, hit kind, reported distance."""
    return (raytracing_any_hit(primitive, rejected), RAYTRACING_PROCEDURAL_ATTRIBUTE_MARKER + primitive,
            RAYTRACING_PROCEDURAL_HIT_KIND_BASE | primitive, distance_bits)


RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT = (RAYTRACING_FILL,) * RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS
RAYTRACING_PROCEDURAL_NO_ANY_HIT = (RAYTRACING_FILL,) * (
    RAYTRACING_PROCEDURAL_CROSSINGS * RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS)
RAYTRACING_PROCEDURAL_NO_HIT_LOG = (RAYTRACING_FILL,) * 8


def raytracing_procedural_any_hit_log(slots: tuple[tuple[int, ...], ...]) -> tuple[int, ...]:
    """A whole lane's any-hit log: one slot per crossing, in crossing order, with the slots of
    crossings whose report was rejected before the any-hit ran left at the fill pattern."""
    return tuple(word for slot in slots for word in slot)


def raytracing_procedural_hit_log(primitive: int, distance_bits: int, any_hit_tag: int,
                                  reject_tag: int = 0) -> tuple[int, ...]:
    """The procedural closest hit's evidence, all of it read from the accepted report. The reject
    tag is carried too, because a later rejected report's payload write survives alongside the
    committed hit's and the closest hit is where that is observable."""
    return (RAYTRACING_PROCEDURAL_ATTRIBUTE_MARKER + primitive, distance_bits, distance_bits,
            RAYTRACING_PROCEDURAL_HIT_KIND_BASE | primitive, primitive,
            RAYTRACING_PROCEDURAL_INSTANCE, any_hit_tag, reject_tag)


def raytracing_intersection_logs(crossing0: int, crossing1: int = RAYTRACING_FILL,
                                 crossing2: int = RAYTRACING_FILL) -> tuple[int, ...]:
    """A lane's intersection log: one word per crossing, with crossings never reached left at the
    fill pattern."""
    return (crossing0, crossing1, crossing2)


def raytracing_any_hit(primitive: int, ignored: bool) -> int:
    return RAYTRACING_ANY_HIT_LOGGED | (primitive << 1) | int(ignored)


RAYTRACING_NO_ANY_HIT = (RAYTRACING_FILL,) * 4
RAYTRACING_ANY_HIT_TRI0_ACCEPTED = (raytracing_any_hit(0, False),) + (RAYTRACING_FILL,) * 3
RAYTRACING_ANY_HIT_TRI1_REJECTED = (RAYTRACING_FILL, raytracing_any_hit(1, True)) + (RAYTRACING_FILL,) * 2
RAYTRACING_ANY_HIT_TRI2_ACCEPTED = (RAYTRACING_FILL,) * 2 + (raytracing_any_hit(2, False), RAYTRACING_FILL)
RAYTRACING_ANY_HIT_TRI0_REJECTED_TRI2_ACCEPTED = (raytracing_any_hit(0, True), RAYTRACING_FILL,
                                                  raytracing_any_hit(2, False), RAYTRACING_FILL)


class RaytracingVariant(NamedTuple):
    """One admissible outcome of a lane: committed-hit identity plus any-hit evidence."""

    primitive: int
    hit_t: float
    bary_u: float
    bary_v: float
    any_hit_tag: int
    reject_tag: int
    any_hit_log: tuple[int, int, int, int]


class RaytracingLane(NamedTuple):
    """A lane's full expected readback; the variant list carries the order-dependent part."""

    kind: int
    instance: int
    callable: int
    local_root: int
    depth: int
    recurse: int
    nested_miss: int
    closest_hit_log: int
    miss_log: int
    callable_log: int
    variants: tuple[RaytracingVariant, ...]


_TRI0_AT_025 = RaytracingVariant(0, 1.0, 0.25, 0.25, RAYTRACING_ACCEPT_TAG, 0, RAYTRACING_ANY_HIT_TRI0_ACCEPTED)
_TRI0_AT_050 = RaytracingVariant(0, 1.0, 0.5, 0.25, RAYTRACING_ACCEPT_TAG, 0, RAYTRACING_ANY_HIT_TRI0_ACCEPTED)
_TRI0_AT_010 = RaytracingVariant(0, 1.0, 0.1, 0.1, RAYTRACING_ACCEPT_TAG, 0, RAYTRACING_ANY_HIT_TRI0_ACCEPTED)
# Triangle 2's barycentrics at (0.1,0.1): a0=(0,0), a1=(0.3,0), a2=(0,0.3) puts 0.1/0.3 = 1/3
# on both weights.
_TRI2_ACCEPTED = RaytracingVariant(2, 7.0, 1.0 / 3.0, 1.0 / 3.0, RAYTRACING_ACCEPT_TAG | 2, 0,
                                   RAYTRACING_ANY_HIT_TRI2_ACCEPTED)
_TRI2_AFTER_REJECT = RaytracingVariant(2, 7.0, 1.0 / 3.0, 1.0 / 3.0, RAYTRACING_ACCEPT_TAG | 2,
                                       RAYTRACING_REJECT_TAG | 0,
                                       RAYTRACING_ANY_HIT_TRI0_REJECTED_TRI2_ACCEPTED)
_MISS_REJECTED_TRI1 = RaytracingVariant(RAYTRACING_FILL, 0.0, 0.0, 0.0, 0, RAYTRACING_REJECT_TAG | 1,
                                        RAYTRACING_ANY_HIT_TRI1_REJECTED)
_MISS_PRISTINE = RaytracingVariant(RAYTRACING_FILL, 0.0, 0.0, 0.0, 0, 0, RAYTRACING_NO_ANY_HIT)
# A committed hit whose closest hit shader was skipped: the payload keeps the any-hit's
# localRoot write but no hit shader stamped it, so primitive stays at the fill pattern and
# the barycentrics stay at their initialized zeroes.
_HIT_COMMITTED_SKIPPED = RaytracingVariant(RAYTRACING_FILL, 0.0, 0.0, 0.0, RAYTRACING_ACCEPT_TAG, 0,
                                           RAYTRACING_ANY_HIT_TRI0_ACCEPTED)

# Every value here is what a correct D3D12 implementation must produce; it is the runner's
# own restatement of the fixture's contract, not a copy of the fixture's printed output, so a
# stale fixture that merely prints PASS cannot satisfy it. Lanes 7, 8 and 9 list both
# admissible candidate-enumeration orders because DXR leaves that order unspecified, and the
# fill pattern in the untouched any-hit slots is what makes "only one candidate was visited"
# observable.
RAYTRACING_EXPECTATIONS = (
    RaytracingLane(1, RAYTRACING_INSTANCE, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, 1,
                   RAYTRACING_FILL, RAYTRACING_FILL, (_TRI0_AT_025,)),
    RaytracingLane(2, RAYTRACING_FILL, 0, 0, 0, 0, 0, RAYTRACING_FILL, 1,
                   RAYTRACING_FILL, (_MISS_PRISTINE,)),
    RaytracingLane(2, RAYTRACING_FILL, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, RAYTRACING_FILL, 1,
                   RAYTRACING_FILL, (_MISS_REJECTED_TRI1,)),
    RaytracingLane(1, RAYTRACING_INSTANCE, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, 1,
                   RAYTRACING_FILL, RAYTRACING_FILL, (_TRI2_AFTER_REJECT,)),
    RaytracingLane(0, RAYTRACING_FILL, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, RAYTRACING_FILL,
                   RAYTRACING_FILL, RAYTRACING_FILL, (_HIT_COMMITTED_SKIPPED,)),
    RaytracingLane(1, RAYTRACING_INSTANCE, RAYTRACING_CALLABLE_VALUE, RAYTRACING_SECOND_HIT_GROUP, 0, 0, 0, 1,
                   RAYTRACING_FILL, 1, (_TRI0_AT_050,)),
    RaytracingLane(1, RAYTRACING_INSTANCE, 0, RAYTRACING_FIRST_HIT_GROUP, 1, 1, 1, 1, 1,
                   RAYTRACING_FILL, (_TRI0_AT_025,)),
    RaytracingLane(1, RAYTRACING_INSTANCE, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, 1,
                   RAYTRACING_FILL, RAYTRACING_FILL, (_TRI0_AT_010, _TRI2_ACCEPTED)),
    RaytracingLane(1, RAYTRACING_INSTANCE, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, 1,
                   RAYTRACING_FILL, RAYTRACING_FILL, (_TRI0_AT_010, _TRI2_ACCEPTED)),
    RaytracingLane(1, RAYTRACING_INSTANCE, 0, RAYTRACING_FIRST_HIT_GROUP, 0, 0, 0, 1,
                   RAYTRACING_FILL, RAYTRACING_FILL, (_TRI2_AFTER_REJECT, _TRI2_ACCEPTED)),
)

class RaytracingProceduralLane(NamedTuple):
    """One procedural lane's exact expected readback; unlike the triangle lanes nothing here is
    order-dependent, because each procedural lane's ray is contained by exactly one AABB and only
    that AABB's intersection shader invocation can report for it."""

    kind: int
    primitive: int
    instance: int
    local_root: int
    hit_t_bits: int
    any_hit_tag: int
    reject_tag: int
    intersection_log: tuple[int, ...]
    any_hit_log: tuple[int, ...]
    closest_hit_log: int
    miss_log: int
    hit_log: tuple[int, ...]


# Lane 0's report is inside the ray's interval and accepted, so the hit is committed and the
# closest hit runs. Lane 1's report is inside the interval but its any-hit calls IgnoreHit, so no
# hit is committed, the rejecting invocation's writes survive and the miss shader runs. Lane 2's
# shape lies far outside the ray's interval, so DXR rejects the report before the any-hit shader
# runs at all: its any-hit and hit logs stay at the fill pattern and the payload carries no
# hit-group write, which is why local_root is 0 for that lane and nonzero for the others.
#
# Lane 3 is one intersection-shader invocation making three reports, in this order:
#   crossing 0  t = 1.0, accepted. The hit is committed, so the ray interval tightens to 1.0.
#   crossing 1  t = 5.0, farther than the committed hit, so it is outside the tightened interval:
#               ReportHit must answer false and the any-hit shader must NOT run - its slot stays at
#               the fill pattern. A runtime that kept testing later reports against the untightened
#               TMax would run the any-hit here.
#   crossing 2  t = 0.5, nearer and inside the tightened interval, so the any-hit shader runs and
#               calls IgnoreHit: ReportHit answers false, the committed hit stays crossing 0's
#               distance/kind/attributes/local root, and the rejecting invocation's writes survive.
#               A runtime that let a later rejected report clobber the committed hit would report
#               crossing 2's distance and attributes in the closest-hit log.
RAYTRACING_PROCEDURAL_EXPECTATIONS = (
    RaytracingProceduralLane(1, 0, RAYTRACING_PROCEDURAL_INSTANCE, RAYTRACING_PROCEDURAL_HIT_GROUP,
                             RAYTRACING_HALF_BITS, RAYTRACING_PROCEDURAL_ACCEPT_TAG, 0,
                             raytracing_intersection_logs(raytracing_intersection_log(0, True, True)),
                             raytracing_procedural_any_hit_log(
                                 (raytracing_procedural_any_hit_slot(0, False, RAYTRACING_HALF_BITS),
                                  RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT,
                                  RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT)),
                             1, RAYTRACING_FILL,
                             raytracing_procedural_hit_log(0, RAYTRACING_HALF_BITS,
                                                           RAYTRACING_PROCEDURAL_ACCEPT_TAG | 0)),
    RaytracingProceduralLane(2, RAYTRACING_FILL, RAYTRACING_FILL, RAYTRACING_PROCEDURAL_HIT_GROUP, 0, 0,
                             RAYTRACING_PROCEDURAL_REJECT_TAG | 1,
                             raytracing_intersection_logs(raytracing_intersection_log(1, False, True)),
                             raytracing_procedural_any_hit_log(
                                 (raytracing_procedural_any_hit_slot(1, True, RAYTRACING_HALF_BITS),
                                  RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT,
                                  RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT)),
                             RAYTRACING_FILL, 1, RAYTRACING_PROCEDURAL_NO_HIT_LOG),
    RaytracingProceduralLane(2, RAYTRACING_FILL, RAYTRACING_FILL, 0, 0, 0, 0,
                             raytracing_intersection_logs(raytracing_intersection_log(2, False, False)),
                             RAYTRACING_PROCEDURAL_NO_ANY_HIT, RAYTRACING_FILL, 1,
                             RAYTRACING_PROCEDURAL_NO_HIT_LOG),
    RaytracingProceduralLane(1, RAYTRACING_MULTI_REPORT_PRIMITIVE, RAYTRACING_PROCEDURAL_INSTANCE,
                             RAYTRACING_PROCEDURAL_HIT_GROUP, RAYTRACING_MULTI_REPORT_ENTRY0_BITS,
                             RAYTRACING_PROCEDURAL_ACCEPT_TAG | RAYTRACING_MULTI_REPORT_PRIMITIVE,
                             RAYTRACING_PROCEDURAL_REJECT_TAG | RAYTRACING_MULTI_REPORT_PRIMITIVE,
                             raytracing_intersection_logs(
                                 raytracing_intersection_log(RAYTRACING_MULTI_REPORT_PRIMITIVE, True, True),
                                 raytracing_intersection_log(RAYTRACING_MULTI_REPORT_PRIMITIVE, False, False),
                                 raytracing_intersection_log(RAYTRACING_MULTI_REPORT_PRIMITIVE, False, True)),
                             raytracing_procedural_any_hit_log(
                                 (raytracing_procedural_any_hit_slot(
                                     RAYTRACING_MULTI_REPORT_PRIMITIVE, False,
                                     RAYTRACING_MULTI_REPORT_ENTRY0_BITS),
                                  RAYTRACING_PROCEDURAL_NO_ANY_HIT_SLOT,
                                  raytracing_procedural_any_hit_slot(
                                      RAYTRACING_MULTI_REPORT_PRIMITIVE, True,
                                      RAYTRACING_MULTI_REPORT_ENTRY2_BITS))),
                             1, RAYTRACING_FILL,
                             raytracing_procedural_hit_log(RAYTRACING_MULTI_REPORT_PRIMITIVE,
                                                           RAYTRACING_MULTI_REPORT_ENTRY0_BITS,
                                                           RAYTRACING_PROCEDURAL_ACCEPT_TAG
                                                           | RAYTRACING_MULTI_REPORT_PRIMITIVE,
                                                           RAYTRACING_PROCEDURAL_REJECT_TAG
                                                           | RAYTRACING_MULTI_REPORT_PRIMITIVE)),
)

RAYTRACING_LANE_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_LANE lane=(\d+) kind=(\d+) primitive=(\d+) instance=(\d+) "
    r"hit_t=([-\d.]+) callable=0x([0-9a-f]{8}) local_root=0x([0-9a-f]{8}) lane_field=(\d+) "
    r"bary=\(([-\d.]+),([-\d.]+)\) dimensions=(\d+) depth=(\d+) recurse=(\d+) nested_miss=(\d+) "
    r"anyhit_tag=0x([0-9a-f]{8}) reject_tag=0x([0-9a-f]{8})")
RAYTRACING_LOG_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_LOG lane=(\d+) "
    r"anyhit=\(0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8})\) "
    r"closesthit=0x([0-9a-f]{8}) miss=0x([0-9a-f]{8}) callable=0x([0-9a-f]{8})")
RAYTRACING_PREBUILD_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_PREBUILD blas_max=(\d+) blas_scratch=(\d+) blas_update_scratch=(\d+)")
RAYTRACING_PREBUILD_TLAS_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_PREBUILD_TLAS tlas_max=(\d+) tlas_scratch=(\d+)")
RAYTRACING_PREBUILD_AABB_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_PREBUILD_AABB aabb_max=(\d+) aabb_scratch=(\d+)")
RAYTRACING_PROCEDURAL_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_PROCEDURAL lane=(\d+) kind=(\d+) primitive=(\d+) instance=(\d+) "
    r"hit_t=([-\d.]+) local_root=0x([0-9a-f]{8}) lane_field=(\d+) bary=\(([-\d.]+),([-\d.]+)\) "
    r"dimensions=(\d+) anyhit_tag=0x([0-9a-f]{8}) reject_tag=0x([0-9a-f]{8})")
RAYTRACING_PROCEDURAL_LOG_PATTERN = re.compile(
    r"OWNED_D3D12_RAYTRACING_PROCEDURAL_LOG lane=(\d+) "
    r"intersection=\(0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8})\) "
    r"anyhit=\(0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),"
    r"0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),"
    r"0x([0-9a-f]{8}),0x([0-9a-f]{8})\) "
    r"closesthit=0x([0-9a-f]{8}) miss=0x([0-9a-f]{8}) "
    r"hit=\(0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8}),"
    r"0x([0-9a-f]{8}),0x([0-9a-f]{8}),0x([0-9a-f]{8})\)")


def raytracing_failures(stdout: str, stderr: str) -> list[str]:
    """Re-derive the ray tracing readback contract from the fixture's printed evidence.

    The pass condition is the exact per-lane GPU readback the fixture observed, not the
    fixture's own verdict line: a Tier-0 capability report, an unsupported HRESULT, a
    dispatch that traced nothing, or a stale binary all leave the payload at the fill
    pattern and cannot produce these numbers.
    """
    failures: list[str] = []
    lanes = RAYTRACING_LANE_PATTERN.findall(stdout)
    logs = RAYTRACING_LOG_PATTERN.findall(stdout)
    if [int(lane[0]) for lane in lanes] != list(range(RAYTRACING_LANE_COUNT)):
        failures.append(f"raytracing lane readback count/order wrong: {len(lanes)} lanes")
        return failures
    if [int(log[0]) for log in logs] != list(range(RAYTRACING_LANE_COUNT)):
        failures.append(f"raytracing shader log count/order wrong: {len(logs)} logs")
        return failures
    for index, (lane, log, expected) in enumerate(zip(lanes, logs, RAYTRACING_EXPECTATIONS)):
        (lane_index, kind, primitive, instance, hit_t, callable, local_root, lane_field, bary_u, bary_v,
         dimensions, depth, recurse, nested_miss, any_hit_tag, reject_tag) = lane
        (_, any_hit_0, any_hit_1, any_hit_2, any_hit_3, closest_hit_log, miss_log, callable_log) = log
        any_hit_log = tuple(int(word, 16) for word in (any_hit_0, any_hit_1, any_hit_2, any_hit_3))
        observed = (int(kind), int(instance), int(callable, 16), int(local_root, 16), int(depth),
                    int(recurse), int(nested_miss), int(closest_hit_log, 16), int(miss_log, 16),
                    int(callable_log, 16))
        wanted = (expected.kind, expected.instance, expected.callable, expected.local_root, expected.depth,
                  expected.recurse, expected.nested_miss, expected.closest_hit_log, expected.miss_log,
                  expected.callable_log)
        if int(lane_index) != index or int(lane_field) != index:
            failures.append(f"raytracing lane {index} reported its own index wrong: {lane_index}/{lane_field}")
        if int(dimensions) != RAYTRACING_LANE_COUNT:
            failures.append(f"raytracing lane {index} dispatch dimensions {dimensions} != {RAYTRACING_LANE_COUNT}")
        if observed != wanted:
            failures.append(f"raytracing lane {index} payload mismatch: {observed} != {wanted}")
        hit_t_value = float(hit_t)
        bary = (float(bary_u), float(bary_v))
        matched = False
        for variant in expected.variants:
            if int(primitive) != variant.primitive or int(any_hit_tag, 16) != variant.any_hit_tag:
                continue
            if int(reject_tag, 16) != variant.reject_tag or any_hit_log != variant.any_hit_log:
                continue
            if abs(hit_t_value - variant.hit_t) >= RAYTRACING_HIT_T_TOLERANCE:
                continue
            if any(abs(observed_bary - variant_bary) >= RAYTRACING_BARY_TOLERANCE
                   for observed_bary, variant_bary in zip(bary, (variant.bary_u, variant.bary_v))):
                continue
            matched = True
            break
        if not matched:
            failures.append(f"raytracing lane {index} hit evidence matches no admissible outcome: "
                            f"primitive={primitive} hit_t={hit_t} bary={bary} "
                            f"anyhit_tag={any_hit_tag} reject_tag={reject_tag} anyhit={any_hit_log}")
    blas = RAYTRACING_PREBUILD_PATTERN.findall(stdout)
    # D3D12 reports zero update scratch when ALLOW_UPDATE was not requested.
    # Still require the complete record and positive result/initial-build sizes.
    if len(blas) != 1 or not all(int(size) > 0 for size in blas[0][:2]):
        failures.append(f"raytracing BLAS prebuild evidence missing or zero-sized: {blas}")
    tlas = RAYTRACING_PREBUILD_TLAS_PATTERN.findall(stdout)
    if len(tlas) != 1 or not all(int(size) > 0 for size in tlas[0]):
        failures.append(f"raytracing TLAS prebuild evidence missing or zero-sized: {tlas}")
    aabb = RAYTRACING_PREBUILD_AABB_PATTERN.findall(stdout)
    if len(aabb) != 1 or not all(int(size) > 0 for size in aabb[0]):
        failures.append(f"raytracing AABB BLAS prebuild evidence missing or zero-sized: {aabb}")
    failures.extend(raytracing_procedural_failures(stdout))
    for text, source in ((stdout, "stdout"), (stderr, "stderr")):
        for line in text.splitlines():
            if RAYTRACING_FAIL_MARKER in line or "RAYTRACING_LANE_MISMATCH" in line \
                    or "RAYTRACING_PAYLOAD_OVERRUN" in line or "RAYTRACING_REGION_OVERRUN" in line \
                    or "RAYTRACING_PROCEDURAL_MISMATCH" in line:
                failures.append(f"raytracing {source} reported failure evidence: {line}")
    return failures


def raytracing_procedural_failures(stdout: str) -> list[str]:
    """Re-derive the procedural dispatch's contract from its own printed evidence.

    The procedural coverage is a second DispatchRays of the same fixture: an AABB bottom-level
    structure with its own instance, whose InstanceContributionToHitGroupIndex is NONZERO, and an
    intersection shader that reports candidates through ReportHit. The runner restates that
    contract independently of the fixture's verdict, so a run whose procedural dispatch never
    executed - or which executed with the triangle hit group, or without the any-hit shader - is
    rejected here rather than accepted on the fixture's PASS line.
    """
    failures: list[str] = []
    lanes = RAYTRACING_PROCEDURAL_PATTERN.findall(stdout)
    logs = RAYTRACING_PROCEDURAL_LOG_PATTERN.findall(stdout)
    if [int(lane[0]) for lane in lanes] != list(range(RAYTRACING_PROCEDURAL_LANE_COUNT)):
        failures.append(f"raytracing procedural lane readback count/order wrong: {len(lanes)} lanes")
        return failures
    if [int(log[0]) for log in logs] != list(range(RAYTRACING_PROCEDURAL_LANE_COUNT)):
        failures.append(f"raytracing procedural shader log count/order wrong: {len(logs)} logs")
        return failures
    for index, (lane, log, expected) in enumerate(zip(lanes, logs, RAYTRACING_PROCEDURAL_EXPECTATIONS)):
        (lane_index, kind, primitive, instance, hit_t, local_root, lane_field, bary_u, bary_v,
         dimensions, any_hit_tag, reject_tag) = lane
        (_, *log_words) = log
        intersection_log = tuple(int(word, 16) for word in log_words[:RAYTRACING_PROCEDURAL_CROSSINGS])
        any_hit_log = tuple(int(word, 16) for word in log_words[
            RAYTRACING_PROCEDURAL_CROSSINGS:
            RAYTRACING_PROCEDURAL_CROSSINGS + RAYTRACING_PROCEDURAL_CROSSINGS
            * RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS])
        closest_hit_log, miss_log = (int(word, 16) for word in log_words[
            RAYTRACING_PROCEDURAL_CROSSINGS
            + RAYTRACING_PROCEDURAL_CROSSINGS * RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS:
            RAYTRACING_PROCEDURAL_CROSSINGS
            + RAYTRACING_PROCEDURAL_CROSSINGS * RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS + 2])
        hit_log = tuple(int(word, 16) for word in log_words[
            RAYTRACING_PROCEDURAL_CROSSINGS
            + RAYTRACING_PROCEDURAL_CROSSINGS * RAYTRACING_PROCEDURAL_ANY_HIT_CROSSING_WORDS + 2:])
        if int(lane_index) != index or int(lane_field) != index:
            failures.append(f"raytracing procedural lane {index} reported its own index wrong: "
                            f"{lane_index}/{lane_field}")
        if int(dimensions) != RAYTRACING_PROCEDURAL_LANE_COUNT:
            failures.append(f"raytracing procedural lane {index} dispatch dimensions {dimensions} != "
                            f"{RAYTRACING_PROCEDURAL_LANE_COUNT}")
        observed = (int(kind), int(primitive), int(instance), int(local_root, 16),
                    *struct.unpack(">I", struct.pack(">f", float(hit_t))),
                    int(any_hit_tag, 16), int(reject_tag, 16))
        wanted = (expected.kind, expected.primitive, expected.instance, expected.local_root,
                  expected.hit_t_bits, expected.any_hit_tag, expected.reject_tag)
        if observed != wanted:
            failures.append(f"raytracing procedural lane {index} payload mismatch: {observed} != {wanted}")
        # A procedural hit has no barycentrics; a runtime that filled them from a triangle path
        # would be reporting triangle attributes for a procedural hit.
        if (float(bary_u), float(bary_v)) != (0.0, 0.0):
            failures.append(f"raytracing procedural lane {index} barycentrics are not zero: "
                            f"({bary_u},{bary_v})")
        if intersection_log != expected.intersection_log:
            failures.append(f"raytracing procedural lane {index} intersection log "
                            f"{tuple(hex(word) for word in intersection_log)} != "
                            f"{tuple(hex(word) for word in expected.intersection_log)}")
        if any_hit_log != expected.any_hit_log:
            failures.append(f"raytracing procedural lane {index} any-hit log "
                            f"{tuple(hex(word) for word in any_hit_log)} != "
                            f"{tuple(hex(word) for word in expected.any_hit_log)}")
        if hit_log != expected.hit_log:
            failures.append(f"raytracing procedural lane {index} closest-hit log "
                            f"{tuple(hex(word) for word in hit_log)} != "
                            f"{tuple(hex(word) for word in expected.hit_log)}")
        if (closest_hit_log, miss_log) != (expected.closest_hit_log, expected.miss_log):
            failures.append(f"raytracing procedural lane {index} stage logs "
                            f"(0x{closest_hit_log:08x},0x{miss_log:08x}) != "
                            f"(0x{expected.closest_hit_log:08x},0x{expected.miss_log:08x})")
    return failures


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def suite_inputs(suite: str, fixture: Path, dxil_dir: Path) -> list[Path]:
    """Inputs a suite reads beside its own executable, frozen before launch."""
    if suite == "pipeline-cache":
        return [dxil_dir / "pipeline-cache-a.dxil", dxil_dir / "pipeline-cache-b.dxil"]
    if suite == "smoke":
        return sorted(fixture.parent.glob(SMOKE_SHADER_GLOB))
    if suite == "dxil":
        return [dxil_dir / name for name in DXIL_SHADERS + DXIL_REGRESSION_SHADERS]
    if suite == "dxil-ia":
        return ([dxil_dir / name for name in DXIL_SHADERS]
                + [dxil_dir / name for name in DXIL_IA_SHADERS])
    if suite == "dxil-static-sampler":
        return [fixture.parent / name for name in DXIL_STATIC_SAMPLER_SHADERS]
    if suite == "dxil-texture":
        return [dxil_dir / name for name in DXIL_TEXTURE_SHADERS]
    if suite == "predication-indirect":
        return [dxil_dir / name for name in PREDICATION_INDIRECT_SHADERS]
    if suite in ("atomic-copy", "atomic-copy-live"):
        return [dxil_dir / name for name in ATOMIC_COPY_SHADERS]
    if suite == "raytracing":
        return [dxil_dir / name for name in RAYTRACING_SHADERS]
    if suite == "enhanced-barriers":
        return [dxil_dir / name for name in ENHANCED_BARRIERS_INPUTS]
    if suite == "stream-output":
        return [dxil_dir / name for name in STREAM_OUTPUT_SHADERS + STREAM_OUTPUT_GS_INPUTS]
    if suite == "partial-3d-uav-dxil":
        return [dxil_dir / name for name in PARTIAL_3D_UAV_SHADERS]
    if suite == "tiled":
        return [dxil_dir / name for name in TILED_SHADERS + TILED_INPUTS]
    if suite == "msaa-lines":
        return [dxil_dir / name for name in MSAA_LINE_INPUTS]
    if suite == "raster-order":
        return [dxil_dir / name for name in RASTER_ORDER_SHADERS]
    if suite == "stencil-reference":
        return [dxil_dir / name for name in STENCIL_REFERENCE_SHADERS]
    if suite == "indexed-restart":
        return [dxil_dir / name for name in INDEXED_RESTART_INPUTS]
    if suite in ("geometry", "tessellation"):
        stem = suite
        names = GEOMETRY_INPUTS if suite == "geometry" else TESSELLATION_INPUTS
        return ([dxil_dir / name for name in names] + sorted(dxil_dir.glob(f"{stem}-*.dxil"))
                + [dxil_dir / "gsindirect.hlsl"] + sorted(dxil_dir.glob("gsindirect-*.dxil")))
    if suite == "bindless":
        return [dxil_dir / name for name in BINDLESS_SHADERS + BINDLESS_INPUTS]
    if suite == "pipeline-statistics":
        return [dxil_dir / name for name in PIPELINE_STATISTICS_SHADERS + PIPELINE_STATISTICS_STAGE_INPUTS]
    if suite == "sm6":
        return [dxil_dir / f"sm6-{entry}.dxil" for entry in SM6_ENTRIES] + [dxil_dir / name for name in SM6_SOURCES]
    if suite == "mesh-shader":
        return ([dxil_dir / f"mesh-shader-{entry}.dxil" for entry in MESH_SHADER_ENTRIES]
                + [dxil_dir / name for name in MESH_SHADER_SOURCES])
    if suite == "view-instancing":
        return [dxil_dir / name for name in VIEW_INSTANCING_INPUTS]
    if suite == "work-graphs":
        return [dxil_dir / name for name in ("work-graphs.hlsl", "work-graphs-sharing.hlsl", "work-graphs-linkage.hlsl", "build-work-graphs.sh",
                                           "work-graphs-nodes.dxil", "work-graphs-sharing.dxil", "work-graphs-link-nodes.dxil", "work-graphs-link-helpers.dxil", "work-graphs-root-subobjects.dxil")]
    if suite == "vrs":
        return [dxil_dir / name for name in VRS_INPUTS]
    if suite == "sampler-feedback":
        return [dxil_dir / name for name in SAMPLER_FEEDBACK_INPUTS]
    if suite == "feature-level":
        return [dxil_dir / name for name in FEATURE_LEVEL_INPUTS]
    if suite == "conservative-raster":
        return [dxil_dir / name for name in CONSERVATIVE_RASTER_INPUTS]
    return []


def run(args: argparse.Namespace) -> int:
    runtime = args.runtime.resolve(strict=True)
    fixture = args.fixture.resolve(strict=True)
    out = args.out.absolute()
    if out.exists() or out.is_symlink():
        raise ValueError("--out must be a new directory; existing evidence is never overwritten")
    out.mkdir(parents=True)
    verifier = Path(__file__).with_name("stage-owned-graphics.py")
    lock = args.lock.resolve(strict=True)
    verify = subprocess.run(
        [sys.executable, str(verifier), "verify", "--runtime", str(runtime)],
        text=True, capture_output=True, timeout=30,
    )
    (out / "runtime-verification.log").write_text(verify.stdout + verify.stderr)
    if verify.returncode:
        raise RuntimeError("runtime verification failed; see runtime-verification.log")

    # Freeze the fixture and shaders so an independent compiler cannot replace
    # files used by a test already running. No original game/prefix is involved.
    frozen = out / "fixture"
    frozen.mkdir()
    sources = [fixture, *suite_inputs(args.suite, fixture, args.dxil_dir)]
    fg_observer = getattr(args, "fg_observer", None)
    if args.suite == "fsr-fg" and args.mode == "present" and fg_observer is None:
        raise ValueError("--suite fsr-fg --mode present requires --fg-observer for native presentation evidence")
    fg_observer_dependencies = []
    fg_observer_architectures = []
    fg_observer_dependencies_by_architecture = {}
    if fg_observer is not None:
        if args.suite != "fsr-fg":
            raise ValueError("--fg-observer is only supported with --suite fsr-fg")
        if fg_observer.is_symlink() or not fg_observer.is_file():
            raise ValueError("--fg-observer must be a regular non-symlink file")
        fg_observer_architectures = macho_architectures(fg_observer)
        if set(fg_observer_architectures) not in ({"x86_64"}, {"x86_64", "arm64"}):
            raise ValueError("--fg-observer must be x86_64 or universal x86_64/arm64 Mach-O")
        for observer_arch in fg_observer_architectures:
            libraries = subprocess.run(["/usr/bin/otool", "-arch", observer_arch, "-l", str(fg_observer)],
                                       check=True, capture_output=True, text=True, timeout=30)
            dependencies = []
            # LC_ID_DYLIB and multiarch headers are not load dependencies.
            for command in libraries.stdout.split("Load command"):
                if re.search(r"\bcmd LC_(?:LOAD|LOAD_WEAK|REEXPORT|LAZY_LOAD|LOAD_UPWARD)_DYLIB\b", command):
                    dependency = re.search(r"\n\s*name (.+) \(offset \d+\)", command)
                    if dependency is None:
                        raise ValueError(f"cannot read --fg-observer {observer_arch} native dependency")
                    dependencies.append(dependency.group(1))
            if not dependencies or any(not dependency.startswith(("/System/Library/", "/usr/lib/"))
                                       for dependency in dependencies):
                raise ValueError(f"--fg-observer {observer_arch} dependencies must be native macOS system libraries/frameworks only")
            fg_observer_dependencies_by_architecture[observer_arch] = dependencies
            fg_observer_dependencies.extend(dependencies)
        sources.append(fg_observer)
        sources.append(FG_CAPTURE_CHECKER)
    for source in sources:
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"fixture input must be a regular file: {source}")
        shutil.copy2(source, frozen / source.name)
    executable = frozen / fixture.name
    manifest = json.loads((runtime / "owned-graphics-runtime.json").read_text())
    if args.suite in ("fsr", "fsr-fg"):
        selection = "fsr" if args.suite == "fsr" else "fsr_framegeneration"
        if manifest.get("optional_modules", {}).get(selection) is not True:
            raise ValueError(f"--suite {args.suite} requires a runtime staged with --with-fsr ({selection}=true)")
    arch = execution_architecture(manifest,
                                  executable, getattr(args, "guest_arch", None))
    env = dict(os.environ)
    for key in tuple(env):
        if (key.startswith(("D3DM", "GPTK_", "DXVK_", "DXMT_", "OWNED_GRAPHICS_", "DYLD_"))
                or key in {"WINEDLLPATH", "WINEDLLPATH_PREPEND", "WINEDLLOVERRIDES",
                           "OWNED_FG_OBSERVER", "FG_CAPTURE_DIR",
                           "CX_ACTIVE_GRAPHICS_BACKEND", "CX_APPLEGPTK_LIBD3DSHARED_PATH"}):
            env.pop(key)
    env.update({
        "WINEPREFIX": str(out / "prefix"),
        "WINEDLLPATH": str(runtime / "lib/wine"),
        "WINEDEBUG": "+loaddll",
        "WINEDLLOVERRIDES": "winemenubuilder.exe=d",
        "DXMT_LOG_LEVEL": "info",
        "DXMT_LOG_PATH": str(out),
        "DXMT_SHADER_CACHE_PATH": str(out / "shader-cache"),
        "OWNED_GRAPHICS_TRACE": "1",
        "OWNED_GRAPHICS_TRACE_LIMIT": "8192",
        # macOS strips DYLD_* while entering /bin/sh. The owned wrapper sets
        # this diagnostic variable itself on the final hop into real Wine.
        "OWNED_GRAPHICS_DYLD_AUDIT": "1",
    })
    if fg_observer is not None:
        capture_dir = out / "fg-capture"
        capture_dir.mkdir()
        env["OWNED_FG_OBSERVER"] = str(frozen / fg_observer.name)
        env["FG_CAPTURE_DIR"] = str(capture_dir)
    validation_mode = ("api-and-shader" if args.validation else
                       "api-only" if args.api_validation else "none")
    if args.validation:
        # Shader Validation otherwise reports faults only to the system log.
        env.update({"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1",
                    "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING": "1",
                    "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1"})
    elif args.api_validation:
        # API diagnostics remain active, but shader instrumentation can serialize
        # independent compute queues. Override inherited/default shader validation.
        env.update({"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "0"})
        env.pop("MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING", None)
        env.pop("MTL_SHADER_VALIDATION_REPORT_TO_STDERR", None)
    else:
        env.pop("MTL_DEBUG_LAYER", None)
        env.pop("MTL_SHADER_VALIDATION", None)
        env.pop("MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING", None)
        env.pop("MTL_SHADER_VALIDATION_REPORT_TO_STDERR", None)

    command = [str(runtime / "bin/wine"), str(executable)]
    if args.suite == "smoke":
        command.append("--barrier-stress")
        if args.mode == "no-present":
            command.append("--no-present")
    elif args.suite in ("dxil", "dxil-ia"):
        # DXIL fixture inputs are frozen beside the executable, so argv stays
        # path-independent and records the exact shader contract exercised.
        command.extend(DXIL_SHADERS)
        if args.suite == "dxil-ia":
            command.extend(DXIL_IA_SHADERS)
    elif args.suite == "dxil-static-sampler":
        command.extend(DXIL_STATIC_SAMPLER_SHADERS)
    elif args.suite == "dxil-texture":
        command.extend(DXIL_TEXTURE_SHADERS)
    elif args.suite == "predication-indirect":
        command.extend(PREDICATION_INDIRECT_SHADERS)
    elif args.suite in ("atomic-copy", "atomic-copy-live"):
        command.extend(ATOMIC_COPY_SHADERS)
        if args.suite == "atomic-copy-live":
            command.append("--live-reader")
    elif args.suite == "stream-output":
        command.extend(STREAM_OUTPUT_SHADERS)
    elif args.suite == "partial-3d-uav-dxil":
        command.extend(PARTIAL_3D_UAV_SHADERS)
    elif args.suite == "tiled":
        command.extend(TILED_SHADERS)
    elif args.suite == "indexed-restart":
        command.extend(INDEXED_RESTART_INPUTS)
    elif args.suite == "pipeline-statistics":
        command.extend(PIPELINE_STATISTICS_SHADERS)
    elif args.suite == "pipeline-cache":
        command.extend(["pipeline-cache-a.dxil", "pipeline-cache-b.dxil"])
    elif args.suite == "bindless":
        command.extend(BINDLESS_SHADERS)
    elif args.suite == "sm6":
        command.extend(["--stress-reps", str(SM6_STRESS_REPS)])
    elif args.suite == "raytracing":
        # The fixture reads exactly one DXR library by name from its own directory, so the
        # frozen file beside the executable is the shader under test.
        command.extend(RAYTRACING_SHADERS)
    evidence = {
        "schema": 1, "suite": args.suite, "mode": args.mode, "validation": args.validation,
        "validation_mode": validation_mode,
        "architecture": arch,
        "command": command, "prefix": env["WINEPREFIX"],
        "runtime_manifest_sha256": digest(runtime / "owned-graphics-runtime.json"),
        "lock": str(lock), "lock_sha256": digest(lock),
        "fixture_sha256": {source.name: digest(frozen / source.name) for source in sources},
        "timeout_seconds": args.timeout,
        "fg_observer_dependencies": fg_observer_dependencies,
        "fg_observer_architectures": fg_observer_architectures,
        "fg_observer_dependencies_by_architecture": fg_observer_dependencies_by_architecture,
        "oracle_environment": ({"OWNED_FSR_NATIVE_MAX_SCALE": env.get("OWNED_FSR_NATIVE_MAX_SCALE")}
                               if args.suite == "fsr" else {}),
    }
    (out / "invocation.json").write_text(json.dumps(evidence, indent=2) + "\n")
    timed_out = False
    code = -1
    with (out / "stdout.log").open("w") as stdout, (out / "stderr.log").open("w") as stderr:
        try:
            result = subprocess.run(command, env=env, cwd=frozen, stdout=stdout, stderr=stderr,
                                    timeout=args.timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
        finally:
            # This prefix was created by this invocation; never kill another
            # user's/global wineserver. Stop our prefix services even on error.
            try:
                subprocess.run([str(runtime / "bin/wineserver"), "-k"], env=env,
                               stdout=subprocess.DEVNULL, stderr=stderr, timeout=15)
            except (OSError, subprocess.TimeoutExpired) as error:
                stderr.write(f"prefix cleanup: {error}\n")
    stdout = (out / "stdout.log").read_text(errors="replace")
    stderr = (out / "stderr.log").read_text(errors="replace")
    dyld = [line for line in stderr.splitlines() if line.startswith("dyld[")]
    foreign = [line for line in dyld if "d3dmetal.framework" in line.lower()
               or "libd3dshared" in line.lower()]
    native_path = str(runtime / arch["unix_directory"] / WINEMETAL_NATIVE_LIBRARY)
    native_paths = [native_path]
    native_loads = [line for line in dyld if any(path in line for path in native_paths)]
    # The owned DXIL compiler is inside this module, so a DXIL run only proves
    # the native compiler path when dyld loaded the staged module itself.
    native_module = runtime / arch["unix_directory"] / WINEMETAL_NATIVE_LIBRARY
    native_compiler_proof = native_module.is_file() and any(native_path in line for line in dyld)
    prefix = SUITE_PREFIXES[args.suite]
    backend_prefix = BACKEND_PREFIXES.get(args.suite, prefix)
    marker_ok = (args.suite == "dxil-static-sampler"
                 or f"{backend_prefix}_BACKEND id=owned-graphics-d3d12" in stdout)
    frames = re.findall(r"OWNED_D3D12_SMOKE_FRAME frame=(\d+) readback=ok", stdout)
    barrier_ok = "OWNED_D3D12_SMOKE_BARRIER_STRESS readback=ok" in stdout
    success = (code == 0 and not timed_out and marker_ok and f"{prefix}_PASS" in stdout
               and native_loads and not foreign)
    fsr_module_loads = []
    if args.suite in ("fsr", "fsr-fg"):
        module_name = ("amd_fidelityfx_upscaler_dx12.dll" if args.suite == "fsr"
                       else "amd_fidelityfx_framegeneration_dx12.dll")
        module = rf'Loaded L"C:\\windows\\system32\\{module_name}"'
        fsr_module_loads = [line for line in stderr.splitlines()
                            if module.lower() in line.lower() and "builtin" in line.lower()]
        success = (success and native_compiler_proof and fsr_module_loads
                   and (runtime / arch["windows_directory"] / module_name).is_file())
    dxil_compiler_loaded = False
    raytracing_evidence = None
    fence_rewind_cases: list[str] = []
    if args.suite == "smoke":
        success = success and barrier_ok and frames == [str(n) for n in range(8)]
    if args.suite == "render-passes":
        success = (success
                   and stdout.count("OWNED_D3D12_RENDER_PASSES_CASE_PASS ") == 8
                   and "OWNED_D3D12_RENDER_PASSES_CLEAR_PRESERVE pixel=ff402080" in stdout
                   and "OWNED_D3D12_RENDER_PASSES_SUSPEND_RESUME pixel=ff204010" in stdout
                   and "OWNED_D3D12_RENDER_PASSES_VALIDATION_PASS rejected=10 conformant_barrier=1" in stdout
                   and "OWNED_D3D12_RENDER_PASSES_UINT_BORDER b0000=0000 b0001=0001 b1111=1111" in stdout
                   and "OWNED_D3D12_RENDER_PASSES_PASS cases=8" in stdout
                   and "_FAIL" not in stdout)
    if args.suite == "enhanced-barriers":
        success = (success
                   and stdout.count("OWNED_D3D12_ENHANCED_BARRIERS_CASE_PASS ") == 19
                   and "OWNED_D3D12_ENHANCED_BARRIERS_INVALID_BARRIERS_PASS rejected=10" in stdout
                   and "OWNED_D3D12_ENHANCED_BARRIERS_STENCIL front=11 back=f2" in stdout
                   and "OWNED_D3D12_ENHANCED_BARRIERS_INTERFACES device14=1 command_list9=1" in stdout
                   and "OWNED_D3D12_ENHANCED_BARRIERS_PASS cases=19" in stdout
                   and "_FAIL" not in stdout)
    if args.suite == "multiqueue":
        frames = re.findall(r"OWNED_D3D12_MULTIQUEUE_FRAME frame=(\d+) words=1024 exact=ok", stdout)
        success = success and frames == [str(n) for n in range(32)]
    if args.suite == "queries":
        calibration = re.findall(
            r"OWNED_D3D12_QUERY_CLOCK_CALIBRATION gpu=(\d+) cpu_qpc=(\d+) monotonic=1", stdout)
        success = (success and len(calibration) == 1
                   and all(int(value) > 0 for value in calibration[0]))
    if args.suite == "pipeline-statistics":
        dxil_compiler_loaded = native_compiler_proof
        success = (success and dxil_compiler_loaded
                   and stdout.count("OWNED_D3D12_PIPELINE_STATISTICS_GPU_CASE_PASS ") == 346
                   and stdout.count("OWNED_D3D12_PIPELINE_STATISTICS_MATRIX_PASS ") == 2
                   and "OWNED_D3D12_PIPELINE_STATISTICS_MATRIX_PASS shader=dxbc cases=168 fields=11" in stdout
                   and "OWNED_D3D12_PIPELINE_STATISTICS_MATRIX_PASS shader=dxil cases=178 fields=11" in stdout
                   and stdout.count("OWNED_D3D12_PIPELINE_STATISTICS_VALIDATION_PASS ") == 26
                   and stdout.count("OWNED_D3D12_PIPELINE_STATISTICS_PASS "
                                    "shaders=dxbc,dxil fields=11 independent_gpu_oracle=1") == 1
                   and "OWNED_D3D12_PIPELINE_STATISTICS_FAIL" not in stdout + stderr)
    if args.suite == "tiled":
        # Every case line must pass, in order, and the fixture must see exactly Tier 2 with
        # Tier 3 (SRV-only) unreported and no Metal validation fault.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_TILED_CASE ([\w.-]+) status=PASS$", stdout, re.MULTILINE)
        success = (success and tuple(cases) == TILED_CASES
                   and "OWNED_D3D12_TILED_CAPS tier=2 srv_only_tier3=0 " in stdout
                   and f"OWNED_D3D12_TILED_PASS cases={len(TILED_CASES)} tier=2" in stdout
                   and "OWNED_D3D12_TILED_FAIL" not in stdout + stderr)
    if args.suite == "bindless":
        # Every case line must pass, in order, with the exact caps the fixture checks.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_BINDLESS_CASE ([\w.-]+) status=PASS$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == BINDLESS_CASES
                   and "OWNED_D3D12_BINDLESS_CAPS binding_tier=3 heap_tier=2 root_signature=0x3" in stdout
                   and "OWNED_D3D12_BINDLESS_PASS cases=8 heap=1000000 samplers=2048 max_index=999999" in stdout
                   and re.search(r"^OWNED_D3D12_BINDLESS_ROV triangles=512 pixels=256 expected=0x[0-9a-f]{8} "
                                 r"structured_mismatches=0 typed_mismatches=0 texture_mismatches=0$", stdout, re.MULTILINE)
                   and "OWNED_D3D12_BINDLESS_MISMATCH" not in stdout
                   and "OWNED_D3D12_BINDLESS_FAIL" not in stdout + stderr)
    if args.suite == "sm6":
        # Every case passes in order; each atomic stress run is exact on every repetition;
        # every negative compile is rejected; the caps line is exactly what was verified.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SM6_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        stress = re.findall(r"^OWNED_D3D12_SM6_STRESS case=(\w+) threads=65536 targets=(\d+) reps=(\d+) "
                            r"exact=(\d+) ", stdout, re.MULTILINE)
        rejects = re.findall(r"^OWNED_D3D12_SM6_REJECT entry=(\w+) hr=0x8\w{7}$", stdout, re.MULTILINE)
        # A GPU hang anywhere (the fragment-lock failure mode that keeps 64 bit atomics
        # compute-only) fails the suite even if every case line printed.
        gpu_hang = "kIOGPUCommandBufferCallbackError" in stderr
        success = (success and dxil_compiler_loaded and tuple(cases) == SM6_CASES
                   and [name for name, *_ in stress] == ["cs_atomic64_stress_1", "cs_atomic64_stress_16"]
                   and all(reps == exact == str(SM6_STRESS_REPS) for _, _, reps, exact in stress)
                   and rejects == SM6_REJECTS and not gpu_hang
                   and re.search(r"^OWNED_D3D12_SM6_CAPS shader_model=0x65/0x0 double=0 minprec=2 wave=1 "
                                 r"lanes=32-32 total_lanes=\d+ int64=1 barycentrics=1 view_instancing=1 "
                                 r"native16=1 atomic64_typed=0 atomic64_groupshared=0 atomic64_heap=0 "
                                 r"wave_mma=0$", stdout, re.MULTILINE)
                   and f"OWNED_D3D12_SM6_PASS stress_reps={SM6_STRESS_REPS}" in stdout
                   and "OWNED_D3D12_SM6_MISMATCH" not in stdout
                   and "OWNED_D3D12_SM6_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_SM6_FAIL" not in stdout + stderr)
    if args.suite == "mesh-shader":
        # Every case passes in order with exact pixels/counters; the caps line is exactly what was verified.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_MESH_SHADER_CASE_PASS ([\w.-]+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == MESH_SHADER_CASES
                   and "kIOGPUCommandBufferCallbackError" not in stderr
                   and re.search(r"^OWNED_D3D12_MESH_SHADER_CAPS tier=10 stats=1 full_range_rtai=1 derivatives=0 "
                                 r"ms_primitives_culled=0$", stdout, re.MULTILINE)
                   and "OWNED_D3D12_MESH_SHADER_PASS" in stdout
                   and "OWNED_D3D12_MESH_SHADER_MISMATCH" not in stdout
                   and "OWNED_D3D12_MESH_SHADER_CASE_FAIL" not in stdout
                   and "OWNED_D3D12_MESH_SHADER_PSO_FAIL" not in stdout
                   and "OWNED_D3D12_MESH_SHADER_FAIL" not in stdout + stderr)
    if args.suite == "raster-order":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^RASTER_ORDER_CASE ([\w-]+) status=PASS$", stdout, re.MULTILINE)
        expected_cases = {
            "raw-direct-dxil", "structured-direct-dxbc", "structured-direct-dxil",
            "raw-table-dxil", "typed-table-dxbc", "structured-table-dxbc",
            "structured-table-dxil", "unbounded-dxil", "mixed-dxbc", "mixed-dxil",
            "spaces-dxil", "layer-table-dxil", "raw-table-dxil-cached",
            "sample-table-4x-dxil", "sample-direct-4x-dxil",
        }
        success = (success and dxil_compiler_loaded
                   and len(cases) == len(expected_cases) and set(cases) == expected_cases
                   and stdout.count("RASTER_ORDER_SAMPLES values=27,27,27,27,27,27,27,27 expected=27") == 2
                   and stdout.count("RASTER_ORDER_LAYERS values=27,27,90,90 expected=27,27,90,90") == 1
                   and stdout.count("OWNED_D3D12_RASTER_ORDER_PASS") == 1
                   and "status=FAIL" not in stdout + stderr)
    if args.suite == "feature-level":
        # FL 11_1/12_0: OutputMergerLogicOp (16 ops x 7 UINT formats + R32G32B32A32_UINT MRT), UAV-only
        # ForcedSampleCount 1/4/8/16 (16 = two programmed 8x halves) with exact coverage/invocation/occlusion,
        # ForcedSampleCount 1 on 4x/8x RTVs with SampleMask/occlusion/statistics, and the 18 required typed UAV
        # load formats exact through texture2d and buffer views.
        logic = re.findall(r"OWNED_D3D12_FEATURE_LEVEL_LOGIC_OP format=(\w+) ops=16 pixels=16 "
                           r"mrt=R32G32B32A32_UINT", stdout)
        forced = re.findall(r"OWNED_D3D12_FEATURE_LEVEL_UAV_ONLY_FORCED samples=(\d+) triangles=40 "
                            r"pixels=10240 partial=(\d+)", stdout)
        forced1 = re.findall(r"OWNED_D3D12_FEATURE_LEVEL_FORCED1_MSAA_RTV samples=(\d) mask=(0x[0-9a-f]+) "
                             r"triangles=6", stdout)
        typed = re.findall(r"OWNED_D3D12_FEATURE_LEVEL_TYPED_UAV_LOAD format=(\w+) texels=32 "
                           r"views=texture2d,buffer", stdout)
        cases = re.findall(r"^OWNED_D3D12_FEATURE_LEVEL_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        success = (success and logic == ["R8G8B8A8_UINT", "R16G16_UINT", "R32_UINT", "R10G10B10A2_UINT",
                                         "R8_UINT", "R16_UINT", "R8G8_UINT"]
                   and [samples for samples, _ in forced] == ["1", "4", "8", "16"]
                   and all(int(partial) > 0 for samples, partial in forced if samples != "1")
                   and forced1 == [("4", "0xffffffff"), ("4", "0x5"), ("8", "0xffffffff"), ("8", "0x81"),
                                   ("4", "0x0")]
                   and len(typed) == 18 and len(set(typed)) == 18
                   and re.search(r"OWNED_D3D12_FEATURE_LEVEL_TIR16_RTV triangles=40 partial=[1-9]\d*", stdout)
                   and cases == ["logic_ops_16_uint_formats_7_mrt_and_rejections",
                                 "uav_only_forced_1_4_8_16_and_tir16_rtv_coverage_invocation_occlusion_exact",
                                 "forced1_multisample_rtv_4x_8x_samplemask_occlusion_statistics",
                                 "typed_uav_load_18_required_formats_texture2d_and_buffer_exact"]
                   and stdout.count("OWNED_D3D12_FEATURE_LEVEL_PASS cases=4") == 1
                   and "OWNED_D3D12_FEATURE_LEVEL_FAIL" not in stdout + stderr)
    if args.suite == "conservative-raster":
        # CONSRAST: the spec bring-up, 1/2 pixel, TIR and 4x MSAA examples, then every pipeline path at 1x and 8x
        # against the CPU oracle (exact outside the 1/128 px float-projection band), back-face culling and per-sample
        # depth extrapolated and clamped to the viewport, distances, view masks and SO/statistics/indirect
        # composition. Every family must pass; Tier 0 remains valid until the proof permits publication.
        cases = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        oracle = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_ORACLE path=(\w+) samples=(\d) triangles=64 "
                            r"covered=(\d+) ambiguous=\d+ invocations=\d+$", stdout, re.MULTILINE)
        rejected = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_PATH_REJECTED path=(\w+)$", stdout, re.MULTILINE)
        passed = [path for path in CONSERVATIVE_RASTER_PATHS if path not in rejected]
        caps = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_CAPS tier=(1)$", stdout, re.MULTILINE)
        tier = caps[0] if len(caps) == 1 else "invalid"
        distances = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_DISTANCE path=(\w+) samples=(\d) covered=\d+ invocations=\d+$", stdout, re.MULTILINE)
        interpolation = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_DISTANCE_INTERPOLATION path=(\w+) samples=8 indirect=1 mask=2 invocations=\d+$", stdout, re.MULTILINE)
        views = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_VIEW path=(\w+) mask=(\d) samples=8 invocations=\d+$", stdout, re.MULTILINE)
        combinations = re.findall(r"^OWNED_D3D12_CONSERVATIVE_RASTER_COMBINATION path=(\w+) indirect=([01]) samples=8 .*", stdout, re.MULTILINE)
        success = (success and not rejected and len(passed) == 10 and len(caps) == 1
                   and distances == [(path, samples) for path in passed for samples in ("1", "8")]
                   and interpolation == passed
                   and views == [(path, mask) for path in passed for mask in ("0", "2", "3")]
                   and combinations == [(path, indirect) for path in passed for indirect in ("0", "1")]
                   and [(path, samples) for path, samples, _ in oracle]
                       == [(path, samples) for path in passed for samples in ("1", "8")]
                   and cases == ["ia_capture_parameter_integrity", "pso_validation", "spec_bring_up", "spec_half_pixel",
                                 "spec_tir_forced4_coverage_0xf", "spec_msaa4_coverage_0xf_samplemask_0x5"]
                                + [f"oracle_{path}" for path in passed]
                                + ["cull_back", "depth_extrapolated_per_sample_clamped"]
                   and re.search(r"OWNED_D3D12_CONSERVATIVE_RASTER_DEPTH samples=4 checked=\d+ clamped=[1-9]", stdout)
                   and f"OWNED_D3D12_CONSERVATIVE_RASTER_CAPS tier={tier}" in stdout
                   and stdout.count(f"OWNED_D3D12_CONSERVATIVE_RASTER_PASS paths={len(passed)} "
                                    f"rejected={len(rejected)} tier={tier}") == 1
                   and "OWNED_D3D12_CONSERVATIVE_RASTER_FAIL" not in stdout + stderr)
    if args.suite == "work-graphs":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_WORK_GRAPHS_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        results = re.findall(r"^OWNED_D3D12_WORK_GRAPHS_RESULT mode=(\d) sum=(\d+) expected_sum=(\d+) "
                             r"count=(\d+) expected_count=(\d+)$", stdout, re.MULTILINE)
        linked_results = re.findall(r"^OWNED_D3D12_WORK_GRAPHS_LINK_RESULT program=([AB]) exact_min=(\d+) "
                                    r"sum=(\d+) expected_sum=(\d+) count=(\d+) expected_count=4 "
                                    r"helper_sum=(\d+) expected_helper_sum=54 helper_count=(\d+) expected_helper_count=4$",
                                    stdout, re.MULTILINE)
        sampler_results = re.findall(r"^OWNED_D3D12_WORK_GRAPHS_SAMPLER_RESULT exact_min=(\d+) "
                                     r"sum=(\d+) expected_sum=68 count=(\d+) expected_count=4 local_argument_bytes=0$",
                                     stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded
                   and cases == [f"mode_{mode}_min_backing_depth32_fanout16" for mode in range(4)] +
                       ["shared_budget_sparse_array_input_sharing_local_roots_cross_group", "collection_external_namespace_root_contract_negatives",
                        "global_roots_collection_functions_program_A", "global_roots_collection_functions_program_B", "local_static_sampler_union_exact"]
                   and [mode for mode, *_ in results] == ["0", "1", "2", "3", "4"]
                   and all(actual == expected and count == expected_count
                       for _, actual, expected, count, expected_count in results)
                   and [program for program, *_ in linked_results] == ["A", "B"]
                   and all(actual == expected and count == "4" and helper_sum == "54" and helper_count == "4"
                       for _, minimum, actual, expected, count, helper_sum, helper_count in linked_results)
                   and len(sampler_results) == 1 and sampler_results[0][1:] == ("68", "4")
                   and stdout.count("OWNED_D3D12_WORK_GRAPHS_CAPS tier=10 options21_bytes=16") == 1
                   and "OWNED_D3D12_WORK_GRAPHS_PASS cases=9 tier=1_0" in stdout
                   and "OWNED_D3D12_WORK_GRAPHS_FAIL" not in stdout + stderr)
    if args.suite == "view-instancing":
        # VIEWINST: every case passes in order with exact pixels, Tier 1 statistics count one draw per enabled
        # view, and the device reports exactly ViewInstancingTier 1.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_VIEW_INSTANCING_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        statistics = re.findall(r"^OWNED_D3D12_VIEW_INSTANCING_STATISTICS views=(\d) ia_vertices=(\d+) "
                                r"ia_primitives=(\d+) vs=(\d+) c_invocations=(\d+) c_primitives=(\d+) ps=(\d+)$",
                                stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == VIEW_INSTANCING_CASES
                   and statistics == [("3", "9", "3", "9", "3", "3", "192"), ("2", "6", "2", "6", "2", "2", "128")]
                   and f"OWNED_D3D12_VIEW_INSTANCING_PASS cases={len(VIEW_INSTANCING_CASES)} tier=1" in stdout
                   and "OWNED_D3D12_VIEW_INSTANCING_MISMATCH" not in stderr
                   and "OWNED_D3D12_VIEW_INSTANCING_FAIL" not in stdout + stderr)
    if args.suite == "vrs":
        # VRS: the advertised Tier2 contract must pass every stage, source, viewport and submission case.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_VRS_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == VRS_CASES
                   and "OWNED_D3D12_VRS_COMMON_SUBMISSION_PASS capture=S_OK submission=887a0005 promotion=0" in stdout
                   and "OWNED_D3D12_VRS_CAPS tier=2 additional=1 tile=8 per_primitive_viewports=1" in stdout
                   and f"OWNED_D3D12_VRS_PASS cases={len(VRS_CASES)}" in stdout
                   and "OWNED_D3D12_VRS_MISMATCH" not in stderr
                   and "OWNED_D3D12_VRS_FAIL" not in stdout + stderr)
    if args.suite == "sampler-feedback":
        # FEEDBACK: every case passes in order against the CPU oracle and the device reports SamplerFeedbackTier 1.0.
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^OWNED_D3D12_SAMPLER_FEEDBACK_CASE_PASS (\w+)$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and tuple(cases) == SAMPLER_FEEDBACK_CASES
                   and f"OWNED_D3D12_SAMPLER_FEEDBACK_PASS cases={len(SAMPLER_FEEDBACK_CASES)} tier=100" in stdout
                   and "OWNED_D3D12_SAMPLER_FEEDBACK_MISMATCH" not in stderr
                   and "OWNED_D3D12_SAMPLER_FEEDBACK_FAIL" not in stdout + stderr)
    if args.suite == "stencil-reference":
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"^STENCIL_REFERENCE_CASE (dxbc|dxil) status=PASS$", stdout, re.MULTILINE)
        success = (success and dxil_compiler_loaded and cases == ["dxbc", "dxil"]
                   and stdout.count("STENCIL_OWNED_READBACK left=30 right=35 row=256 bytes=2") == 2
                   and stdout.count("OWNED_D3D12_STENCIL_REFERENCE_PASS") == 1)
    if args.suite == "sharing":
        # Every sharing case passes in order, the child process reported success, and
        # only verified cross-adapter caps are claimed.
        cases = re.findall(r"^OWNED_D3D12_SHARING_CASE ([\w-]+) status=PASS$", stdout, re.MULTILINE)
        success = (success and tuple(cases) == SHARING_CASES
                   and stdout.count("OWNED_D3D12_SHARING_CHILD ok") == 1
                   and stdout.count("OWNED_D3D12_SHARING_CHILD_REWIND value=15") == 1
                   and stdout.count("OWNED_D3D12_SHARING_CHILD_LATCH event=1 queue=1 late=1 current=0") == 1
                   and stdout.count("OWNED_D3D12_SHARING_STRESS signals=3000 final=3000") == 1
                   and stdout.count("OWNED_D3D12_SHARING_NAMED_CHILD ok rewind=40") == 1
                   and "OWNED_D3D12_SHARING_CAPS cross_node=0 row_major=1" in stdout
                   and f"OWNED_D3D12_SHARING_PASS cases={len(SHARING_CASES)}" in stdout
                   and "OWNED_D3D12_SHARING_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_SHARING_MISMATCH" not in stdout + stderr)
    if args.suite == "residency":
        success = (success
                   and stdout.count("OWNED_D3D12_RESIDENCY_CASE validation=1 refcounts=1 "
                                    "create_not_resident=1 priority=1") == 1
                   and stdout.count("OWNED_D3D12_RESIDENCY_FENCE value=7 completed=7") == 1
                   and stdout.count("OWNED_D3D12_RESIDENCY_READBACK bytes=256 checksum=0x59047300") == 1)
    if args.suite == "core-contracts":
        success = (success
                   and stdout.count("OWNED_D3D12_CORE_CONTRACTS_ROOT_VALIDATION_PASS") == 1
                   and stdout.count("OWNED_D3D12_CORE_CONTRACTS_RANGE_FLAGS_PASS") == 1)
    if args.suite == "resources":
        success = (success
                   and stdout.count("OWNED_D3D12_RESOURCES_PASS owned_backend=1 resource_contracts=1") == 1
                   and stdout.count("OWNED_D3D12_RESOURCES_ALIASING_GPU_PASS ") == 1
                   and stdout.count("ordered_snapshots=4 barriers=first_to_second,second_to_first "
                                    "inheritance=bidirectional") == 1)
    if args.suite == "buffer-rtv":
        success = (success
                   and stdout.count("OWNED_D3D12_BUFFER_RTV_DRAW_READBACK_PASS") == 1
                   and stdout.count("OWNED_D3D12_BUFFER_RTV_PREDICATED_CLEAR_PASS") == 1
                   and stdout.count("OWNED_D3D12_BUFFER_RTV_BOUNDARY_PASS") == 1)
    if args.suite in ("atomic-copy", "atomic-copy-live"):
        dxil_compiler_loaded = native_compiler_proof
        cases = re.findall(r"OWNED_D3D12_ATOMIC_COPY_CASE_PASS list=(\w+) uint32=1 dependencies=0,1 ordered=1",
                           stdout)
        cases64 = re.findall(r"OWNED_D3D12_ATOMIC_COPY64_CASE_PASS list=(\w+) scenarios=6 "
                             r"dependencies=0,2 exact=1", stdout)
        live = re.findall(r"OWNED_D3D12_ATOMIC_COPY64_LIVE_(OBSERVED|INCONCLUSIVE) observed=(\d+) "
                          r"changed_threads=(\d+) first=(\d+) last=(\d+) atomic64_snapshot=unproven", stdout)
        live_observed = (len(live) == 1 and live[0][0] == "OBSERVED"
                         and 0 < int(live[0][2]) <= int(live[0][1])
                         and int(live[0][3]) < int(live[0][4]))
        success = (success and dxil_compiler_loaded and cases == ["direct", "compute", "copy"]
                   and cases64 == ["direct", "compute", "copy"]
                   and (args.suite != "atomic-copy-live" or live_observed)
                   and stdout.count("OWNED_D3D12_ATOMIC_COPY64_COMPUTE_PASS list=0 "
                                    "predication_false=1 payload=1") == 1
                   and stdout.count("OWNED_D3D12_ATOMIC_COPY64_COMPUTE_PASS list=2 "
                                    "predication_false=0 payload=1") == 1
                   and stdout.count("OWNED_D3D12_ATOMIC_COPY64_LIFETIME_PASS replay=2 released_list=1") == 1
                   and stdout.count("OWNED_D3D12_ATOMIC_COPY64_CONTROL_PASS threads=256 final=128 payload=1") == 1
                   and stdout.count("OWNED_D3D12_ATOMIC_COPY_VALIDATION_PASS cases=33") == 1
                   and stdout.count("OWNED_D3D12_ATOMIC_COPY_PASS uint32=1 uint64=1") == 1)
    if args.suite == "present-queues":
        success = (success
                   and stdout.count("OWNED_D3D12_PRESENT_QUEUES_VALIDATION_PASS rejected=12 preserved=1") == 1
                   and stdout.count("OWNED_D3D12_PRESENT_QUEUES_ROTATION_PASS buffer0=queue_a buffer1=queue_b "
                                    "blocked=1 released=1 ordered=1") == 1
                   and stdout.count("OWNED_D3D12_PRESENT_QUEUES_PLAIN_RESIZE_PASS buffers=3 "
                                    "creation_queue=1") == 1
                   and stdout.count("OWNED_D3D12_PRESENT_QUEUES_RESTART_PASS queued_discard=1 credit=1 "
                                    "release_restores=1") == 1
                   and stdout.count("OWNED_D3D12_PRESENT_QUEUES_RESTART_NATIVE_PASS discarded=2 credit=1 "
                                    "presented=restart_only") == 1)
        # Native decision per drawable: only the restart frame may be handed to Core Animation.
        native_window = stderr.partition("RESTART_NATIVE_BEGIN")[2].partition("RESTART_NATIVE_END")
        native_decisions = re.findall(r"OWNED_GRAPHICS_NATIVE_PRESENT_DECISION present_id=(\d+) presented=([01]) "
                                      r"reason=(\w+)", native_window[0])
        success = (success and bool(native_window[1])
                   and sorted(native_decisions) == [("2", "0", "epoch"), ("3", "1", "current")])
    if args.suite == "hdr-metadata":
        frames = re.findall(r"OWNED_D3D12_HDR_METADATA_FRAME frame=(\d+) source_pixels=3072 "
                            r"exact=1 fence=complete present=ok", stdout)
        success = (success and frames == [str(n) for n in range(7)]
                   and stdout.count("OWNED_D3D12_HDR_METADATA_VALIDATION_PASS invalid=4 unsupported=2") == 1
                   and stdout.count("OWNED_D3D12_HDR_METADATA_LIFECYCLE_PASS before=1 set=1 "
                                    "replaced=1 cleared=1 caller_modified=1") == 1
                   and stdout.count("OWNED_D3D12_HDR_METADATA_RESIZE_PASS format=r10g10b10a2 frames=2") == 1
                   and stdout.count("OWNED_D3D12_HDR_METADATA_PENDING_PASS accepted=1 "
                                    "replaced_before_release=1 next_present=1") == 1
                   and stdout.count("OWNED_D3D12_HDR_METADATA_PASS frames=7 source_readback=1 "
                                    "gpu_fence=1 present=1") == 1
                   and "OWNED_D3D12_HDR_METADATA_FAIL" not in stdout + stderr)
    if args.suite == "colorspace":
        frames = re.findall(r"OWNED_D3D12_COLORSPACE_FRAME format=(\d+) frame=(\d+) "
                            r"source_pixels=3072 exact=1 fence=complete present=ok", stdout)
        success = (success and len(frames) == 13
                   and [int(frame) for _, frame in frames] == list(range(13))
                   and {int(fmt) for fmt, _ in frames} == {28, 10, 24}
                   and stdout.count("OWNED_D3D12_COLORSPACE_CONTRACT mapped=4 rejected=23 "
                                    "null=invalidarg post_reject_frame=ok") == 1
                   and stdout.count("OWNED_D3D12_COLORSPACE_FAILED_RESIZE rejected=1 "
                                    "desc_preserved=1 next_frame=ok") == 1
                   and stdout.count("OWNED_D3D12_COLORSPACE_RESIZE same=1 format=28 frame=ok") == 1
                   and stdout.count("OWNED_D3D12_COLORSPACE_RESIZE same=0 format=24 frame=ok") == 1
                   and stdout.count("OWNED_D3D12_COLORSPACE_RESIZE same=0 format=10 frame=ok") == 1
                   and stdout.count("OWNED_D3D12_COLORSPACE_PENDING accepted=1 "
                                    "setter_before_release=1 queue_rotation=1 next_frame=ok") == 1
                   and stdout.count("OWNED_D3D12_COLORSPACE_PASS frames=13 formats=3 "
                                    "source_readback=1 present=1") == 1
                   and "OWNED_D3D12_COLORSPACE_FAIL " not in stdout + stderr)
    if args.suite == "fullscreen":
        # D3D12 has a product export for its identity; D3D11 does not. The
        # verified staged runtime and Wine's actual D3D11 builtin module load
        # prove the latter instead of inventing an identity export for that API.
        module = r'Loaded L"C:\\windows\\system32\\d3d11.dll"'
        module_loads = (runtime / "lib/wine/x86_64-windows/d3d11.dll").is_file() and any(
            module.lower() in line.lower() and "builtin" in line.lower() for line in stderr.splitlines())
        success = (success and module_loads
                   and stdout.count("OWNED_DXGI_FULLSCREEN_D3D11_DEVICE feature_level=0x") == 1
                   and sorted(re.findall(r"OWNED_DXGI_FULLSCREEN_FRAME api=(d3d11|d3d12) "
                                         r"pixels=3888 exact=1 (?:fence|query)=complete present=ok", stdout))
                       == ["d3d11", "d3d12"]
                   and sorted(re.findall(r"OWNED_DXGI_FULLSCREEN_TRANSITION api=(d3d11|d3d12) "
                                         r"enter=1 resize=1 frame=1 leave=1 rect=restored", stdout))
                       == ["d3d11", "d3d12"]
                   and sorted(re.findall(r"OWNED_DXGI_FULLSCREEN_REPEAT api=(d3d11|d3d12) "
                                         r"cycles=2 restored=1", stdout)) == ["d3d11", "d3d12"]
                   and sorted(re.findall(r"OWNED_DXGI_FULLSCREEN_ERRORS api=(d3d11|d3d12) "
                                         r"invalid=1 qi=1 desc=1 mode=1 resize=1 reentrant=1 "
                                         r"native_enter=1 native_leave=1 preserved=1", stdout)) == ["d3d11", "d3d12"]
                   and sorted(re.findall(r"OWNED_DXGI_FULLSCREEN_CREATION api=(d3d11|d3d12) "
                                         r"native=1 windowed=1 rect=restored", stdout)) == ["d3d11", "d3d12"]
                   and sorted(re.findall(r"OWNED_DXGI_FULLSCREEN_INVALID_CREATION api=(d3d11|d3d12) "
                                         r"rejected=1 null_output=1", stdout)) == ["d3d11", "d3d12"])
    if args.suite == "stream-output":
        # GS streams 0-3: per-stream slot bytes, BufferFilledSize and SO_STATISTICS_STREAM0..3, DXBC then DXIL
        # (3 + 3 points; emissions per point: stream 0 one, 1 two, 2 one (never declared), 3 three).
        gs_streams = [
            ("gs-streams-0-1", "6,12,0,0", "6,12,0,0", "96,96,0,0"),
            ("gs-streams-3-split-1", "0,12,0,18", "0,12,0,18", "216,0,72,48"),
            ("gs-streams-overflow", "6,12,0,5", "6,12,0,18", "96,96,20,40"),
            ("gs-streams-raster0", "0,12,0,18", "0,12,0,18", "96,72,0,0"),
        ]
        found_gs = re.findall(r"^OWNED_D3D12_STREAM_OUTPUT_GS_STREAM_PASS case=([\w-]+) shader=(dxbc|dxil) "
                              r"written=([\d,]+) needed=([\d,]+) filled=([\d,]+)$", stdout, re.MULTILINE)
        success = success and found_gs == [(name, shader, *rest) for shader in ("dxbc", "dxil")
                                           for name, *rest in gs_streams]
        success = (success and stdout.count("OWNED_D3D12_STREAM_OUTPUT_VALIDATION_PASS cases=112") == 1
                   and stdout.count("OWNED_D3D12_STREAM_OUTPUT_CAPTURE_PASS cases=43") == 1
                   and stdout.count("OWNED_D3D12_STREAM_OUTPUT_GPU_CASE_PASS ") == 45
                   and stdout.count("OWNED_D3D12_STREAM_OUTPUT_NO_RASTER_DISTANCE_PASS cases=2") == 1
                   and "OWNED_D3D12_STREAM_OUTPUT_PASS validation=1 capture=1" in stdout
                   and "OWNED_D3D12_STREAM_OUTPUT_FAIL" not in stdout + stderr
                   and stdout.count("OWNED_D3D12_STREAM_OUTPUT_QUERY_GPU_PASS streams=0,1,2,3 "
                                    "written=0 needed=0 offset=24 guards=1") == 1)
    if args.suite == "so-format-support":
        names = [f"R32{'G32' if n >= 2 else ''}{'B32' if n >= 3 else ''}{'A32' if n >= 4 else ''}_{kind}"
                 for kind in ("FLOAT", "SINT", "UINT") for n in range(1, 5)]
        words = (("3fa00000", "c0200000", "40700000", "c0900000"),
                 ("fffffff9", "12345678", "ffffff9d", "7fffffff"),
                 ("89abcdef", "00000011", "fedcba98", "76543210"))
        gpu = re.findall(r"OWNED_D3D12_SO_FORMAT_SUPPORT_GPU format=(\w+) words=(\d+) "
                         r"bits=([0-9a-f,]+) bytes=(\d+) counters=(\d+),(\d+) guards=1 ia=1", stdout)
        expected_gpu = [(name, str(n), ",".join(words[k][:n]), str(4 * n), str(4 * n), str(4 * n))
                        for k in range(3) for n in range(1, 5) for name in [names[4 * k + n - 1]]]
        capabilities = re.findall(r"OWNED_D3D12_SO_FORMAT_SUPPORT_CAP format=(\w+) so_buffer=1", stdout)
        negatives = re.findall(r"OWNED_D3D12_SO_FORMAT_SUPPORT_NEGATIVE format=(\w+) so_buffer=0", stdout)
        success = (success and gpu == expected_gpu and capabilities == names
                   and negatives == ["UNKNOWN", "R32_TYPELESS", "R32G32B32_TYPELESS",
                                     "R16_FLOAT", "R16G16B16A16_UINT", "R8G8B8A8_UNORM",
                                     "R32_FLOAT_X8X24_TYPELESS", "D32_FLOAT"]
                   and stdout.count("OWNED_D3D12_SO_FORMAT_SUPPORT_PASS gpu=12 capabilities=12 "
                                    "negatives=8 ia=12") == 1
                   and "OWNED_D3D12_SO_FORMAT_SUPPORT_FAIL" not in stdout + stderr)
    if args.suite == "depth-plane-copy":
        cases = re.findall(r"OWNED_D3D12_DEPTH_PLANE_COPY_CASE_PASS format=(\d+) removed=00000000", stdout)
        success = (success and cases == ["44", "45", "19", "20"]
                   and stdout.count("OWNED_D3D12_DEPTH_PLANE_COPY_PASS "
                                    "cases=4 depth=packed24 stencil=exact controls=d32s8") == 1
                   and "OWNED_D3D12_DEPTH_PLANE_COPY_FAIL" not in stdout + stderr)
    if args.suite == "format-support":
        success = (success and re.search(r"OWNED_D3D12_FORMAT_SUPPORT_MATRIX formats=98 "
                                         r"texture2d=\d+ buffer=\d+ compressed=21", stdout) is not None
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_SRGB_UAV_PASS "
                                    "formats=RGBA8,BGRA8 bytes=64 preserved=1") == 1
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_DEPTH_GPU_PASS "
                                    "planes=depth,stencil typed_combined=dsv_only mip=1") == 1
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_SINGLE_DEPTH_GPU_PASS "
                                    "typed=dsv_only aliases=R32_FLOAT,R16_UNORM load=1 comparison=1") == 1
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_CAST_GPU_PASS "
                                    "typed_resource=R8G8B8A8_UNORM views=linear,srgb") == 1
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_CAST_GROUPS_GPU_PASS "
                                    "r16=uint_to_half rgba32=uint_to_float bc1=linear_to_srgb") == 1
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_MIP_GPU_PASS "
                                    "formats=R32_UINT,R8G8B8A8_TYPELESS mip=1") == 1
                   and stdout.count("OWNED_D3D12_FORMAT_SUPPORT_PASS") == 1
                   and "OWNED_D3D12_FORMAT_SUPPORT_FAIL" not in stdout + stderr)
    if args.suite == "multisample-capabilities":
        gpu_cases = re.findall(
            r"OWNED_D3D12_MULTISAMPLE_CAPABILITIES_GPU_CASE_PASS "
            r"format=([0-9]+) view=([0-9]+) count=([0-9]+) quality=([0-9]+) depth=([0-9]+)", stdout)
        expected_cases = {(str(resource), str(view), str(count), str(quality), str(depth))
                          for count in (2, 4, 8)
                          for resource, view, quality, depth in
                          ((28, 28, 0xffffffff, 0), (28, 28, 0, 0),
                           (27, 28, 0, 0), (42, 42, 0, 0),
                           (40, 40, 0, 1), (39, 40, 0, 1), (20, 20, 0, 1))}
        rejected = re.findall(
            r"OWNED_D3D12_MULTISAMPLE_CAPABILITIES_REJECT "
            r"format=([0-9]+) count=([0-9]+) quality=([0-9]+) hr=([0-9a-f]+)", stdout)
        rejected_cases = {(int(fmt), int(count), int(quality)) for fmt, count, quality, _ in rejected}
        required_rejections = {(28, count, 0) for count in (0, 3, 16, 32, 0xffffffff)}
        required_rejections |= {(28, count, 1) for count in (2, 4, 8)}
        required_rejections |= {(71, 4, 0), (28, 1, 1), (28, 1, 0xfffffffe)}
        center_cases = re.findall(
            r"OWNED_D3D12_MULTISAMPLE_CAPABILITIES_CENTER_GPU_PASS count=([0-9]+) coverage=([0-9]+)", stdout)
        center_masks = re.findall(
            r"OWNED_D3D12_MULTISAMPLE_CAPABILITIES_CENTER_MASK_GPU_PASS count=([0-9]+) mask=([0-9]+)", stdout)
        center_depth = re.findall(
            r"OWNED_D3D12_MULTISAMPLE_CAPABILITIES_CENTER_DEPTH_GPU_PASS count=([0-9]+)", stdout)
        success = (success and len(gpu_cases) == len(expected_cases)
                   and set(gpu_cases) == expected_cases
                   and len(center_cases) == 3
                   and set(center_cases) == {("2", "3"), ("4", "15"), ("8", "255")}
                   and len(center_masks) == 3
                   and set(center_masks) == {("2", "1"), ("4", "5"), ("8", "85")}
                   and len(center_depth) == 3 and set(center_depth) == {"2", "4", "8"}
                   and stdout.count("OWNED_D3D12_MULTISAMPLE_CAPABILITIES_SEMANTIC_GPU_PASS "
                                    "rgba=0.750,0.500,0.125,0.250") == 1
                   and required_rejections.issubset(rejected_cases)
                   and stdout.count("OWNED_D3D12_MULTISAMPLE_CAPABILITIES_PASS") == 1
                   and "OWNED_D3D12_MULTISAMPLE_CAPABILITIES_FAIL" not in stdout + stderr)
    if args.suite in ("geometry", "tessellation"):
        # GSTS: the DXBC matrix always runs; once frozen <suite>-*.dxil inputs exist the DXIL matrix must pass with
        # exactly the DXBC case list.
        prefix = SUITE_PREFIXES[args.suite]
        matrix_cases = {"geometry": 87, "tessellation": 166}[args.suite]
        dxil_frozen = (args.dxil_dir / f"{args.suite}-vs_pass.dxil").exists()
        # A case is identified by its name plus the tessellation domain/partition it ran with.
        case_names = [(shader, name + qualifier) for shader, name, qualifier in re.findall(
            rf"^{prefix}_CASE_PASS shader=(dxbc|dxil) case=([\w-]+)((?: domain=\w+ partition=\w+)?)",
            stdout, re.MULTILINE)]
        dxbc_cases = [name for shader, name in case_names if shader == "dxbc"]
        dxil_cases = [name for shader, name in case_names if shader == "dxil"]
        matrices = re.findall(rf"^{prefix}_MATRIX_PASS shader=(dxbc|dxil) cases=(\d+)$", stdout, re.MULTILINE)
        success = (success
                   and len(dxbc_cases) == len(set(dxbc_cases)) and bool(dxbc_cases)
                   and matrices == ([("dxbc", str(matrix_cases)), ("dxil", str(matrix_cases))] if dxil_frozen
                                    else [("dxbc", str(matrix_cases))])
                   and (dxil_cases == dxbc_cases if dxil_frozen
                        else not dxil_cases and f"{prefix}_DXIL_SKIPPED" in stdout)
                   and re.findall(rf"^{prefix}_PASS$", stdout, re.MULTILINE) == [f"{prefix}_PASS"]
                   and f"{prefix}_CASE_FAIL" not in stdout + stderr
                   and f"{prefix}_MISMATCH" not in stdout + stderr)
        mutation_kinds = ["gs", "stripcut", "adjacency"] if args.suite == "geometry" else ["ts", "tsgs"]
        mutation_cases = [f"indirect-mutation-{kind}{suffix}" for kind in mutation_kinds for suffix in ("", "-so")]
        if args.suite == "tessellation":
            mutation_cases += ["indirect-mutation-ts-so-no-raster", "indirect-mutation-ts-so-overflow",
                               "indirect-mutation-ts-so-no-raster-overflow",
                               "ds-so-fractional-tri-order", "ds-so-fractional-quad-order",
                               "ds-so-isoline-points", "ds-so-isoline-lines"]
            domain_so = re.findall(rf"^{prefix}_DOMAIN_SO shader=(dxbc|dxil) case=([\w-]+) bytes=(\d+) "
                                   rf"written=(\d+) needed=(\d+) ds=(\d+) c=(\d+) ps=(\d+)$",
                                   stdout, re.MULTILINE)
            expected_domain_so = [(shader, "indirect-mutation-ts-so" + suffix, size, written, "2", "6", clip, ps)
                                  for shader in (("dxbc", "dxil") if dxil_frozen else ("dxbc",))
                                  for suffix, size, written, clip, ps in (
                                      ("", "192", "2", "2", "2"),
                                      ("-no-raster", "192", "2", "0", "0"),
                                      ("-overflow", "96", "1", "2", "2"),
                                      ("-no-raster-overflow", "96", "1", "0", "0"))]
            success = success and sorted(domain_so) == sorted(expected_domain_so)
        success = success and all(dxbc_cases.count(case) == 1 for case in mutation_cases)
        if args.suite == "geometry":
            # GS streams 0-3: per-stream written/needed counters, GSPrimitives over every stream and the clipper
            # count of the rasterized stream, for both shader kinds.
            multi_stream = [
                ("so-streams2-raster0", "2,4,0,0", "2,4,0,0", "6", "2"),
                ("so-streams2-raster1", "2,4,0,0", "2,4,0,0", "6", "4"),
                ("so-streams4-raster2", "2,4,6,8", "2,4,6,8", "20", "6"),
                ("so-streams4-no-raster", "2,4,6,8", "2,4,6,8", "20", "0"),
                ("so-streams4-overflow", "2,3,6,5", "2,4,6,8", "20", "8"),
                # ExecuteIndirect: two GPU-counted commands of both points append in API order.
                ("so-streams2-indirect", "4,8,0,0", "4,8,0,0", "12", "8"),
                ("so-streams4-indirect-overflow", "4,3,8,5", "4,8,12,16", "40", "16"),
            ]
            found = re.findall(rf"^{prefix}_CASE_PASS shader=(dxbc|dxil) case=(so-streams[\w-]+) written=([\d,]+) "
                               rf"needed=([\d,]+) gs=(\d+) c=(\d+)$", stdout, re.MULTILINE)
            success = success and found == [(shader, *case) for shader in
                                            (("dxbc", "dxil") if dxil_frozen else ("dxbc",))
                                            for case in multi_stream]
            # MSAA GS draws with a forced-early-depth PS (claim buffer): PSInvocations of the direct, indirect and
            # indirect-SO draws with an occluder, and of the direct draw without it, equal the CPU oracle.
            early = re.findall(rf"^{prefix}_EARLY_DEPTH_STATISTICS shader=(dxbc|dxil) samples=(\d) "
                               rf"ps=([\d,]+) oracle=([\d,]+)$", stdout, re.MULTILINE)
            expected_early = [("2", "57,57,57,75", "57,75"), ("4", "71,71,71,95", "71,95"),
                              ("8", "86,86,86,113", "86,113")]
            # Indirect GS strips with a cut / triangle strip with adjacency: indirect == direct (filled SO bytes).
            strips = re.findall(rf"^{prefix}_STRIP_EXPANSION shader=(dxbc|dxil) case=([\w-]+) .* "
                                rf"filled=(\d+)/(\d+)$", stdout, re.MULTILINE)
            expected_strips = [("indirect-strip-cut", "0", "0"), ("indirect-strip-cut-so", "2304", "2304"),
                               ("indirect-triadj-strip", "0", "0"), ("indirect-triadj-strip-so", "1920", "1920")]
            shaders = ("dxbc", "dxil") if dxil_frozen else ("dxbc",)
            success = (success
                       and early == [(shader, *case) for shader in shaders for case in expected_early]
                       and strips == [(shader, *case) for shader in shaders for case in expected_strips])
        if args.suite == "tessellation":
            # MSAA HS/DS and HS/DS/GS draws with a forced-early-depth PS (claim buffer): direct and indirect
            # PSInvocations equal the CPU oracle at 2x/4x/8x.
            early = re.findall(rf"^{prefix}_EARLY_DEPTH_STATISTICS shader=(dxbc|dxil) case=([\w-]+) "
                               rf"ps=([\d,]+) oracle=(\d+) c=(\d+)$", stdout, re.MULTILINE)
            expected_early = [("ds-early-depth-msaa2", "269,269", "269", "4"),
                              ("tsgs-early-depth-msaa2", "538,538", "538", "8"),
                              ("ds-early-depth-msaa4", "287,287", "287", "4"),
                              ("tsgs-early-depth-msaa4", "574,574", "574", "8"),
                              ("ds-early-depth-msaa8", "301,301", "301", "4"),
                              ("tsgs-early-depth-msaa8", "602,602", "602", "8")]
            success = success and early == [(shader, *case) for shader in
                                             (("dxbc", "dxil") if dxil_frozen else ("dxbc",))
                                             for case in expected_early]
    if args.suite == "indexed-restart":
        dxil_compiler_loaded = native_compiler_proof
        case_passes = re.findall(
            r"OWNED_D3D12_INDEXED_RESTART_CASE_PASS shader=(dxbc|dxil) case=([\w-]+) "
            r"compared=(\d+) nonzero=(\d+) primitives=(\d+)", stdout)
        success = (success and dxil_compiler_loaded
                   and stdout.count("OWNED_D3D12_INDEXED_RESTART_MATRIX_PASS "
                                    "shaders=dxbc,dxil cases=31") == 1
                   and len(case_passes) == 62
                   and [name for shader, name, *_ in case_passes if shader == "dxbc"] ==
                       [name for shader, name, *_ in case_passes if shader == "dxil"]
                   and "OWNED_D3D12_INDEXED_RESTART_CASE_FAIL" not in stdout + stderr)
    if args.suite == "copy-resolve":
        success = (success
                   and stdout.count("OWNED_D3D12_COPY_RESOLVE_PASS average=2.5 min=1 max=4 "
                                    "guard=99 uav=1 aliasing=1") == 1
                   and stdout.count("OWNED_D3D12_COPY_RESOLVE_MSAA_PREDICATION_PASS") == 1
                   and stdout.count("OWNED_D3D12_COPY_RESOLVE_PREDICATE_SNAPSHOT_PASS") == 1
                   and stdout.count("OWNED_D3D12_COPY_RESOLVE_RGBA_PASS") == 1
                   and stdout.count("OWNED_D3D12_COPY_RESOLVE_DEPTH_STENCIL_PASS") == 1)
    if args.suite == "predication-indirect":
        dxil_compiler_loaded = native_compiler_proof
        success = (success and dxil_compiler_loaded
                   and stdout.count("OWNED_D3D12_PREDICATION_INDIRECT_CLEAR_CASES_PASS") == 1
                   and stdout.count("OWNED_D3D12_PREDICATION_INDIRECT_INDIRECT_CASES_PASS") == 1
                   and stdout.count("OWNED_D3D12_PREDICATION_INDIRECT_PREDICATE_SNAPSHOT_PASS "
                                    "saved=1 source_updated=0 copy_predicated=1 resnap_skip_observed=1") == 1
                   and stdout.count("OWNED_D3D12_PREDICATION_INDIRECT_TYPED_MULTILAYER_CLEARS_PASS "
                                    "rtv=R16G16B16A16_UINT dsv=D32_FLOAT_S8X24_UINT "
                                    "uav=R32G32B32A32_UINT layers=2 rect_preservation=1 "
                                    "component_preservation=1 predicate_preservation=1") == 1
                   and stdout.count("OWNED_D3D12_PREDICATION_INDIRECT_TYPED_FLOAT_UAV_PASS "
                                    "format=R32G32B32A32_FLOAT layers=2 rect_preservation=1 "
                                    "predicate_preservation=1") == 1
                   and stdout.count("OWNED_D3D12_PREDICATION_INDIRECT_BUFFER_UAV_PASS cases=60 "
                                    "view_relative=1 clipping=1 sentinels=1 predicate_preservation=1") == 1)
    if args.suite == "fence-events":
        fence_rewind_cases = re.findall(r"OWNED_D3D12_FENCE_EVENTS_CASE_PASS case=([a-z0-9_]+)", stdout)
        required_rewind_cases = {"cpu_rewind_events", "gpu_rewind_mixed"}
        success = (success and required_rewind_cases.issubset(fence_rewind_cases)
                   and stdout.count("case=cpu_rewind_events from=10 to=5 single=1 any=1 all=1 null=1 equal=1") == 1
                   and stdout.count("case=gpu_rewind_mixed from=10 to=5 equal=1 gated_cpu_gpu=1 "
                                    "competing_queues=1 api_wait_latch=1 retained=1") == 1)
    if args.suite == "ngx":
        frames = re.findall(r"OWNED_D3D12_NGX_FRAME_PASS frame=(\d+)\b", stdout)
        success = success and frames == [str(n) for n in range(8)]
    if args.suite == "fsr":
        frames = re.findall(r"OWNED_D3D12_FSR_FRAME_PASS frame=(\d+)\b", stdout)
        replays = re.findall(r"OWNED_D3D12_FSR_REPLAY_PASS replay=(\d+) .*"
                             r"destroyed_before_submit=1 sentinel=1 changed_gpu_input=(\d+)", stdout)
        exposures = re.findall(r"OWNED_D3D12_FSR_EXPOSURE_PASS format=(\d+) value=1 .* sentinel=1", stdout)
        cases = Counter(re.findall(r"OWNED_D3D12_FSR_BEHAVIOR_PASS case=(\w+)\b", stdout))
        required_cases = {
            "zero_motion_resolution_jitter_equivalence": 2, "auto_exposure_ignores_poison_texture": 3,
            "nonzero_motion_scale_equivalence": 3, "nonzero_motion_effect": 3,
            "sharpen_constant_invariance": 1, "sharpen_edge_alpha": 1,
            "constant_transfer_roundtrip": 2, "transfer_independent_scene": 2,
            "reactive_composition_equivalence": 1, "mask_idempotence": 1,
            "history_mask_channel_equivalence": 2, "reactive_history_effect": 1,
            "composition_below_reactive_no_change": 1,
            "output_generation_switch_resets_history": 1, "history_reset_matches_fresh": 1,
            "cap_center_matches_native_scale_control": 1, "cap_scale_geometry": 1,
            "inflight_release": 1, "command_list_reset_discards_work": 1, "exposure_gpu_replay": 3,
        }
        motion_effects = re.findall(r"case=nonzero_motion_effect option=(\d+) rgb_byte_delta_sum=(\d+) jitter_nonzero=(\d+)", stdout)
        transfers = re.findall(r"case=transfer_independent_scene transfer=(\d+) linear_mean_abs_error=(\S+) "
                               r"wrong_flag_byte_delta_sum=(\d+) alpha_unchanged=1", stdout)
        constant_transfers = re.findall(r"case=constant_transfer_roundtrip transfer=(\d+) linear_max_error=(\S+) "
                                        r"alpha_unchanged=1 sentinel=1", stdout)
        gpu_exposures = re.findall(r"case=exposure_gpu_replay replay=(\d+) value=(\S+) half_bits=([0-9a-f]{4}) "
                                  r"max_ulp=(\d+) contexts_destroyed=1 gpu_produced_r32=1", stdout)
        caps = re.findall(r"case=cap_scale_geometry native_max=(\S+) content=(\d+)x(\d+) caller=256x224 "
                          r"center=(\d+),(\d+) black_opaque=1 sentinel=1", stdout)
        cap_ok = False
        try:
            if len(caps) == 1:
                scale, width, height, x, y = caps[0]
                scale = float(scale)
                extent = math.floor(64 * scale)
                cap_ok = (1 <= scale < 3.5 and (int(width), int(height), int(x), int(y))
                          == (extent, extent, (256 - extent) // 2, (224 - extent) // 2))
            numeric_ok = (sorted((int(option), int(jitter)) for option, _, jitter in motion_effects) == [(0, 0), (2, 0), (4, 1)]
                          and all(int(delta) > 0 for _, delta, _ in motion_effects)
                          and sorted(transfer for transfer, _, _ in transfers) == ["2", "4"]
                          and all(0 <= float(error) <= 0.02 and int(delta) > 0 for _, error, delta in transfers)
                          and sorted(transfer for transfer, _ in constant_transfers) == ["2", "4"]
                          and all(0 <= float(error) <= 1.5 / 255 for _, error in constant_transfers)
                          and [(replay, bits) for replay, _, bits, _ in gpu_exposures]
                              == [("0", "3555"), ("1", "3cf0"), ("2", "4248")]
                          and all(int(ulp) <= 1 and abs(float(value) - expected) < 0.00001
                                  for (_, value, _, ulp), expected in zip(gpu_exposures, (0.3333, 1.234567, 3.14159))))
        except (ValueError, OverflowError):
            numeric_ok = False
        success = (success and frames == ["0", "1", "2", "3"]
                   and replays == [("0", "0"), ("1", "0"), ("2", "1")]
                   and exposures == ["0", "54", "41"]
                   and "OWNED_D3D12_FSR_CASE_PASS case=rejected_dispatch_output_unchanged result=0" in stdout
                   and all(cases[name] == count for name, count in required_cases.items())
                   and cap_ok and numeric_ok
                   and "OWNED_D3D12_FSR_FULL_SR_PASS motion=1 masks=1 transfer=1 sharpening_constant=1 output_generation=1" in stdout
                   and "case=inflight_release fence_gate=1 context_destroyed=1 caller_resources_released=1 sentinel=1" in stdout
                   and "case=command_list_reset_discards_work output_unchanged=1 sentinel=1" in stdout
                   and "OWNED_D3D12_FSR_FAIL" not in stdout + stderr)
    if args.suite == "fsr-fg":
        present_calls = re.findall(r"OWNED_D3D12_FSR_FG_PRESENT_CALL frame=(\d+) enabled=(\d+) only_generated=(\d+) "
                                   r"generation_error=(\d+) present_error=(\d+) qpc_start=(\d+) qpc_end=(\d+) frequency=(\d+)", stdout)
        callbacks = re.findall(r"OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame=(\d+) generated=(\d+) width=128 height=128 "
                               r"hash=[0-9a-f]{16} marker=(\d+),(\d+),220", stdout)
        expected_callbacks = [(frame, generated) for frame in range(1, 10)
                              for generated in ((1, 0) if frame in (1, 2, 3, 4, 7) else (1,) if frame == 8 else (0,))]
        ui_frames = re.findall(r"OWNED_D3D12_FSR_FG_UI_FRAME frame=(\d+) width=(\d+) height=(\d+) source=(\d+),60,100 "
                               r"ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1", stdout)
        statistics = re.findall(r"OWNED_D3D12_FSR_FG_STATISTICS frame=(\d+) native_present=(\d+) "
                                r"present_refresh=(\d+) sync_refresh=(\d+) sync_qpc=(\d+) frequency=(\d+) "
                                r"source=public_dxgi_actual_presentation", stdout)
        success = (success
                   and "OWNED_D3D12_FSR_FG_DIRECT_PASS snapshot=exact rect_guard=exact frames=4" in stdout
                   and "OWNED_D3D12_FSR_FG_HISTORY_PASS reset=fresh gap=reset off_on=fresh stale_record=no_history_publication negative_output=unchanged" in stdout
                   and "OWNED_D3D12_FSR_FG_LIFETIME_PASS context_destroy_before_execute=1 input_release=1 allocation_balance=1" in stdout
                   and "OWNED_D3D12_FSR_FG_CONFIG_PASS repeated_id=replace retained_distinct_limit=64 overflow=runtime_error off_clears=1" in stdout
                   and "OWNED_D3D12_FSR_FG_PROXY_PASS create=hwnd callbacks=gpu-readback generated_before_real=1 only_generated=1 disabled=1 callback_error=1 drain=1 destroy=1" in stdout
                   and "OWNED_D3D12_FSR_FG_PACING_PASS ordered=1 bounded_progress=1 qpc=observed native_timestamps=external_observer_required" in stdout
                   and "OWNED_D3D12_FSR_FG_UI_PASS wrap=real_hwnd ui=builtin_shader premultiplied=1 doublebuffer=gpu_snapshot resize=96x80 lifetime=drained" in stdout
                   and [tuple(map(int, row)) for row in ui_frames]
                       == [(11, 128, 128, 88), (12, 128, 128, 96), (13, 96, 80, 104), (14, 96, 80, 112)]
                   and [int(frame) for frame, *_ in statistics] == [*range(1, 10), *range(11, 15)]
                   and all(int(native_present) > 0 and int(qpc) > 0 and int(frequency) > 0
                           for _, native_present, _, _, qpc, frequency in statistics)
                   and [int(frame) for frame, *_ in present_calls] == list(range(1, 11))
                   and [(int(frame), int(generated)) for frame, generated, _, _ in callbacks] == expected_callbacks
                   and all(int(red) == int(frame) + 32 and int(green) == (190 if generated == "1" else 70)
                           for frame, generated, red, green in callbacks)
                   and all((int(enabled), int(only), int(generation_error), int(present_error))
                           == (int(int(frame) not in (5, 6)), int(frame == "8"), int(frame == "9"), int(frame == "10"))
                           and int(frequency) > 0 and 0 <= int(end) - int(start) < 60 * int(frequency)
                           for frame, enabled, only, generation_error, present_error, start, end, frequency in present_calls)
                   and "OWNED_D3D12_FSR_FG_FAIL" not in stdout + stderr
                   and "OWNED_D3D12_FSR_FG_OBSERVER_FAIL" not in stdout + stderr)
    if args.suite == "temporal":
        frames = re.findall(r"OWNED_D3D12_TEMPORAL_FRAME_PASS variant=(\w+) frame=(\d+)\b", stdout)
        exposures = re.findall(r"case=r32_exposure_numeric replay=(\d+) exposure=\S+ "
                               r"r16_bits=0x([0-9a-f]{4}) max_output_ulp=(\d+) "
                               r"gpu_producer=rtv_clear release_before_completion=(\d+)", stdout)
        success = (success and frames == [(variant, str(n)) for variant in ("typed", "typeless_zzz")
                                         for n in range(8)]
                   and [(replay, bits, released) for replay, bits, ulp, released in exposures]
                   == [("0", "3555", "0"), ("1", "3cf0", "0"), ("2", "4248", "1")]
                   and all(int(ulp) <= 1 for _, _, ulp, _ in exposures))
    if args.suite in ("dxil", "dxil-ia"):
        # A DXIL run is only meaningful when the staged native module carrying the
        # owned compiler is present and dyld actually loaded that exact file.
        dxil_compiler_loaded = native_compiler_proof
        success = success and dxil_compiler_loaded
        compute = re.findall(
            r"OWNED_D3D12_DXIL_COMPUTE index=(\d+) expected=0x([0-9a-f]{8}) actual=0x([0-9a-f]{8})",
            stdout)
        success = success and len(compute) == 1 and compute[0][1] == compute[0][2]
        # The owned compiler's descriptor-table binding must execute and read back
        # the expected value; an unsupported HRESULT is never accepted as evidence.
        if args.suite == "dxil":
            table = re.findall(re.escape(DXIL_DESCRIPTOR_TABLE_MARKER)
                               + r" expected=0x([0-9a-f]{8}) actual=0x([0-9a-f]{8})", stdout)
            success = success and len(table) == 1 and table[0] == (
                DXIL_DESCRIPTOR_TABLE_VALUE, DXIL_DESCRIPTOR_TABLE_VALUE)
            spaces = re.findall(r"OWNED_D3D12_DXIL_REGISTER_SPACES b0_space2=0x([0-9a-f]{8}) "
                                r"b0_space9=0x([0-9a-f]{8})", stdout)
            success = success and spaces == [("13579bdf", "2468ace0")]
            success = (success and stdout.count("OWNED_D3D12_DXIL_STATIC_BOUNDS_PASS null_reads=zero "
                                               "oob_atomic=zero oob_writes=dropped sentinels=preserved "
                                               "in_range=exact root_uav=exact") == 1)
        varying = re.findall(r"OWNED_D3D12_DXIL_VARYING slot=(\d+) ", stdout)
        success = success and varying == [str(n) for n in range(4)]
        depth = re.findall(r"OWNED_D3D12_DXIL_DEPTH center=([0-9.]+) ", stdout)
        depth_ok = False
        if len(depth) == 1:
            try:
                depth_ok = float(depth[0]) < 0.0001
            except ValueError:
                depth_ok = False
        success = success and depth_ok
        if args.suite == "dxil-ia":
            pairs = re.findall(re.escape(DXIL_IA_MARKER) + r" pair=(DXIL-DXIL|DXBC-DXIL|DXIL-DXBC)", stdout)
            clip_link = DXIL_IA_MARKER.replace("DXIL_IA_PASS", "DXIL_CLIP_LINK_PASS") + " pair=DXBC-DXBC-CLIP"
            packed_link = DXIL_IA_MARKER.replace("DXIL_IA_PASS", "DXIL_PACKED_LINK_PASS") + " pair=DXBC-DXBC-PACKED"
            success = (success and len(pairs) == 3
                       and set(pairs) == {"DXIL-DXIL", "DXBC-DXIL", "DXIL-DXBC"}
                       and stdout.count(clip_link) == 1 and stdout.count(packed_link) == 1
                       and re.findall(r"OWNED_D3D12_DISTANCE_PASS samples=(\d+)", stdout) == ["1", "4"])
        frames = compute
    if args.suite == "dxil-static-sampler":
        dxil_compiler_loaded = native_compiler_proof
        success = (success and dxil_compiler_loaded
                   and stdout.count(DXIL_STATIC_SAMPLER_MARKER) == 1)
    if args.suite == "dxil-texture":
        dxil_compiler_loaded = native_compiler_proof
        lod = re.findall(
            r"OWNED_D3D12_DXIL_TEXTURE_LOD clamped=(-?\d+\.\d+) unclamped=(-?\d+\.\d+)", stdout)
        success = (success and dxil_compiler_loaded
                   and stdout.count("OWNED_D3D12_DXIL_TEXTURE_GATHER lanes=(0.000,1.000,0.000,1.000) "
                                    "array=(0.000,1.000,1.000,0.000)") == 1
                   and len(lod) == 1 and abs(float(lod[0][0])) <= 0.05
                   and abs(float(lod[0][1]) + 4.0) <= 0.05
                   and stdout.count("OWNED_D3D12_DXIL_TEXTURE_SAMPLE_POS texture=(-0.375,0.125) "
                                    "target=(0.375,-0.125) count=4") == 1
                   and set(re.findall(
                       r"OWNED_D3D12_DXIL_TEXTURE_PATTERN_PASS count=([0-9]+) "
                       r"target=([0-9]+) source=([0-9]+)", stdout))
                   == {(str(count), "4294967294", str(source))
                       for count in (2, 4, 8) for source in (4294967294, 0)}
                   | {("4", "0", "4294967294")}
                   and stdout.count("OWNED_D3D12_DXIL_TEXTURE_PASS") == 1)
    if args.suite == "raytracing":
        # The DXR library is lowered by the same owned DXIL compiler that lives inside the
        # staged native module, so the run only proves the ray tracing path when that exact
        # module was loaded. The verdict is then re-derived from the per-lane readback
        # rather than trusted from the fixture's PASS line.
        dxil_compiler_loaded = native_compiler_proof
        raytracing_failures_found = raytracing_failures(stdout, stderr)
        raytracing_evidence = {"lanes": [int(lane[0]) for lane in RAYTRACING_LANE_PATTERN.findall(stdout)],
                               "procedural_lanes": [int(lane[0]) for lane in
                                                    RAYTRACING_PROCEDURAL_PATTERN.findall(stdout)],
                               "failures": raytracing_failures_found}
        contract_cases = tuple(re.findall(r"^OWNED_D3D12_RAYTRACING_CASE ([\w.-]+) status=PASS$", stdout,
                                          re.MULTILINE))
        vertex_formats = tuple(re.findall(r"^OWNED_D3D12_RAYTRACING_FORMAT ([\w.-]+) status=PASS$", stdout,
                                          re.MULTILINE))
        raytracing_evidence["contract_cases"] = list(contract_cases)
        raytracing_evidence["vertex_formats"] = list(vertex_formats)
        as_cases = tuple(re.findall(r"^RAYTRACING_AS case=([\w-]+) failures=0 status=PASS$",
                                    stdout, re.MULTILINE))
        raytracing_evidence["as_cases"] = list(as_cases)
        success = (success and as_cases == RAYTRACING_AS_CASES
                   and stdout.count("RAYTRACING_AS_SUMMARY cases=16 failures=0") == 1
                   and not re.search(r"^RAYTRACING_AS .*status=FAIL$", stdout, re.MULTILINE)
                   and "RAYTRACING_AS FAIL" not in stderr)
        # Inline RayQuery: every stage's 24 lanes compared word by word by the fixture.
        rayquery_cases = tuple(re.findall(r"^OWNED_D3D12_RAYTRACING_RAYQUERY_CASE ([\w-]+) lanes=24 status=PASS$",
                                          stdout, re.MULTILINE))
        raytracing_evidence["rayquery_cases"] = list(rayquery_cases)
        coverage_cases = tuple(re.findall(r"^OWNED_D3D12_RAYTRACING_COVERAGE_CASE ([\w-]+) status=PASS$",
                                          stdout, re.MULTILINE))
        coverage_features = tuple((name, int(lanes)) for name, lanes in re.findall(
            r"^OWNED_D3D12_RAYTRACING_COVERAGE_FEATURE ([\w-]+) lanes=(\d+) status=PASS$", stdout, re.MULTILINE))
        coverage_predication = tuple((name, int(flag)) for name, flag in re.findall(
            r"^OWNED_D3D12_RAYTRACING_COVERAGE_PREDICATION ([\w-]+) culled=(\d) status=PASS$", stdout, re.MULTILINE))
        coverage_bundle = tuple((name, int(flag)) for name, flag in re.findall(
            r"^OWNED_D3D12_RAYTRACING_COVERAGE_BUNDLE ([\w-]+) culled=(\d) status=PASS$", stdout, re.MULTILINE))
        raytracing_evidence["coverage_cases"] = list(coverage_cases)
        raytracing_evidence["coverage_features"] = [name for name, _ in coverage_features]
        raytracing_evidence["coverage_predication"] = [name for name, _ in coverage_predication]
        raytracing_evidence["coverage_bundle"] = [name for name, _ in coverage_bundle]
        success = (success and coverage_cases == RAYTRACING_COVERAGE_CASES
                   and coverage_features == RAYTRACING_COVERAGE_FEATURES
                   and coverage_predication == RAYTRACING_COVERAGE_PREDICATION
                   and coverage_bundle == RAYTRACING_COVERAGE_BUNDLE
                   and "OWNED_D3D12_RAYTRACING_COVERAGE failures=0" in stdout)
        # Root-argument-changing ExecuteIndirect(DISPATCH_RAYS): per-slot root constants and root UAV.
        rootargs_cases = tuple(re.findall(
            r"^OWNED_D3D12_RAYTRACING_ROOTARGS_CASE ([\w-]+) slots=4 status=PASS$", stdout, re.MULTILINE))
        raytracing_evidence["rootargs_cases"] = list(rootargs_cases)
        success = (success and rootargs_cases == RAYTRACING_ROOTARGS_CASES
                   and "OWNED_D3D12_RAYTRACING_ROOTARGS_MISMATCH" not in stderr)
        success = (success and dxil_compiler_loaded and not raytracing_failures_found
                   and rayquery_cases == RAYTRACING_RAYQUERY_CASES
                   and "OWNED_D3D12_RAYTRACING_RAYQUERY failures=0" in stdout
                   and "OWNED_D3D12_RAYTRACING_RAYQUERY_STAGES failures=0" in stdout
                   and "OWNED_D3D12_RAYTRACING_RAYQUERY_MISMATCH" not in stderr
                   and contract_cases[:len(RAYTRACING_CONTRACT_CASES)] == RAYTRACING_CONTRACT_CASES
                   and contract_cases[len(RAYTRACING_CONTRACT_CASES):][:len(RAYTRACING_STATEOBJ_CASES)]
                   == RAYTRACING_STATEOBJ_CASES
                   and vertex_formats == RAYTRACING_VERTEX_FORMATS
                   and "OWNED_D3D12_RAYTRACING_CONTRACT failures=0" in stdout
                   and "OWNED_D3D12_RAYTRACING_STATEOBJ failures=0" in stdout
                   and "OWNED_D3D12_RAYTRACING_CASE_MISMATCH" not in stderr
                   and stdout.count(RAYTRACING_PASS_MARKER) == 1)
    if args.suite == "smoke" and args.mode == "present":
        success = success and (stdout.count("present_backbuffer=exact present=ok") == 8
                               and "OWNED_D3D12_SMOKE_PRESENT_DRAIN fence=complete" in stdout)
    success = bool(success)
    # DXMT's Logger writes to separate DLL logs when DXMT_LOG_PATH is set.
    # A GPU error there must not disappear merely because stderr was clean.
    backend_logs = {path.name: path.read_text(errors="replace")
                    for path in sorted(out.glob("*_d3d12.log"))}
    diagnostic_text = "\n".join([stderr, *backend_logs.values()])
    validation_errors = [line for line in diagnostic_text.splitlines()
                         if any(term in line for term in ("validateCompute", "validateRender",
                                                           "Execution of the command buffer was aborted",
                                                           "Shader Validation Error", "Invalid Metal usage:",
                                                           "Device error:"))]
    trace_events = []
    for text in backend_logs.values():
        for line in text.splitlines():
            marker = "OWNED_GRAPHICS_TRACE "
            if marker not in line:
                continue
            try:
                event = json.loads(line.split(marker, 1)[1])
            except (json.JSONDecodeError, ValueError):
                validation_errors.append("malformed owned trace: " + line)
                continue
            trace_events.append(event)
            if event.get("event") == "complete" and event.get("status") != 4:
                validation_errors.append("non-completed Metal command buffer: " + line)
    if args.suite == "pipeline-library":
        # The re-executed child writes its own log (named after its executable). Every PSO it
        # creates must be restored from the serialized library; any cold compile is a failure.
        child_events = [json.loads(line.split("OWNED_GRAPHICS_TRACE ", 1)[1])
                        for line in backend_logs.get("owned-d3d12-pipeline-library-child_d3d12.log", "").splitlines()
                        if "OWNED_GRAPHICS_TRACE " in line]
        child_restores = sum(event.get("event") == "pipeline_cache_restore" for event in child_events)
        child_colds = sum(event.get("event") == "pipeline_cache_cold" for event in child_events)
        cases = set(re.findall(r"^OWNED_D3D12_PIPELINE_LIBRARY_CASE_PASS (\w+)$", stdout, re.MULTILINE))
        gpu = re.findall(r"^OWNED_D3D12_PIPELINE_LIBRARY_GPU case=(\w+) ", stdout, re.MULTILINE)
        success = (success and child_restores == 4 and child_colds == 0
                   and cases == {"feature_reporting", "stream_subobject_coverage", "store", "load_rejections",
                                 "serialize", "blob_rejections", "deserialize_same_process",
                                 "concurrent_load_store", "cross_process_reuse"}
                   and sorted(gpu) == sorted(f"{phase}_{kind}" for phase in ("stored", "deserialized", "child")
                                             for kind in ("compute", "stream_compute", "graphics",
                                                          "stream_graphics")) + ["thread_entry"]
                   and stdout.count("OWNED_D3D12_PIPELINE_LIBRARY_THREADS threads=8 rounds=24 entries=12") == 1
                   and stdout.count("OWNED_D3D12_PIPELINE_LIBRARY_CHILD_PASS ") == 1
                   and "OWNED_D3D12_PIPELINE_LIBRARY_FAIL" not in stdout + stderr)
        if not (child_restores == 4 and child_colds == 0):
            validation_errors.append(f"child process restores={child_restores} cold_compiles={child_colds}")
    if validation_errors:
        success = False
    fg_capture_analysis = None
    fg_output_artifacts = {}
    if args.suite == "fsr-fg":
        for image in (*sorted(frozen.glob("fg-*.bin")),
                      *sorted((out / "fg-capture").glob("drawable-*.bin"))):
            if image.is_symlink() or not image.is_file():
                validation_errors.append(f"FG output is not a regular file: {image.name}")
                success = False
            else:
                fg_output_artifacts[str(image.relative_to(out))] = {"sha256": digest(image), "size": image.stat().st_size}
        if fg_observer is not None:
            analysis_path = out / "fg-capture-analysis.json"
            analysis = subprocess.run(
                [sys.executable, str(frozen / FG_CAPTURE_CHECKER.name),
                 "--stdout", str(out / "stdout.log"), "--stderr", str(out / "stderr.log"),
                 "--captures", str(out / "fg-capture"), "--out", str(analysis_path)],
                capture_output=True, text=True, timeout=30,
            )
            (out / "fg-capture-analysis.log").write_text(analysis.stdout + analysis.stderr)
            capture_ok = False
            if analysis_path.is_file():
                capture_report = json.loads(analysis_path.read_text())
                capture_ok = capture_report.get("pass") is True
                fg_capture_analysis = {"file": analysis_path.name, "sha256": digest(analysis_path),
                                       "pass": capture_ok}
            if analysis.returncode or not capture_ok:
                validation_errors.append("native FG drawable content, order or presentation timestamps check failed")
                success = False
    temporal_analysis = None
    if args.suite == "temporal" and success:
        if len(backend_logs) != 1:
            validation_errors.append("expected exactly one owned D3D12 trace log")
            success = False
        else:
            analysis_path = out / "temporal-trace-analysis.json"
            analysis = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("analyze-owned-temporal-trace.py")),
                 str(out / next(iter(backend_logs))), "--out", str(analysis_path)],
                capture_output=True, text=True, timeout=30,
            )
            (out / "temporal-trace-analysis.log").write_text(analysis.stdout + analysis.stderr)
            if analysis.returncode:
                validation_errors.append("temporal event ordering or lifetime check failed")
                success = False
            if analysis_path.is_file():
                temporal_analysis = {"file": analysis_path.name, "sha256": digest(analysis_path)}
    report = {**evidence, "exit_code": code, "timed_out": timed_out,
              "backend_marker_verified": marker_ok, "frames_verified": frames,
              "uav_barrier_verified": barrier_ok, "dyld_lines": len(dyld),
              "owned_native_loads": native_loads,
              "owned_fsr_module_loads": fsr_module_loads,
              "fg_output_artifacts": fg_output_artifacts, "fg_capture_analysis": fg_capture_analysis,
              "dxil_native_compiler_verified": dxil_compiler_loaded,
              "raytracing": raytracing_evidence,
              "fence_rewind_cases": fence_rewind_cases,
              "foreign_backend_loads": foreign, "validation_errors": validation_errors,
              "backend_logs": list(backend_logs), "trace_event_count": len(trace_events),
              "temporal_analysis": temporal_analysis,
              "pass": success,
              "scope": ("synthetic owned D3D12/D3D11 fullscreen fixture; not general game compatibility"
                        if args.suite == "fullscreen" else
                        "synthetic owned D3D12 fixture; not ZZZ compatibility or a nameplate fix")}
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(stdout, end="")
    print(f"{'PASS' if success else 'FAIL'} owned graphics {args.mode} "
          f"validation={validation_mode}: {out}")
    return 0 if success else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--guest-arch", choices=("x86", "x64", "arm64"),
                        help="assert the application architecture (otherwise inspect the fixture); not a translation switch")
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK,
                        help="authoritative owned-build lock recorded as evidence for this run")
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--fg-observer", type=Path,
                        help="x86_64 or universal x86_64/arm64 native drawable observer dylib, required for fsr-fg --mode present; frozen as evidence")
    parser.add_argument("--dxil-dir", type=Path, default=DEFAULT_DXIL_DIR,
                        help="directory holding the frozen .dxil inputs a suite reads by name "
                             "(compute/vertex/pixel/register-space for the DXIL suites, "
                             "raytracing.dxil for ray tracing, atomic-copy-reader.dxil for "
                             "atomic-copy and atomic-copy-live)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--mode", choices=("no-present", "present"), default="no-present")
    parser.add_argument("--suite", choices=tuple(SUITE_PREFIXES), default="smoke",
                        help="atomic-copy checks core GPU results; atomic-copy-live also requires "
                             "observed concurrent reader progress")
    validation = parser.add_mutually_exclusive_group()
    validation.add_argument("--validation", action="store_true",
                            help="enable Metal API and shader validation")
    validation.add_argument("--api-validation", action="store_true",
                            help="enable Metal API validation without shader instrumentation")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 600:
        parser.error("--timeout must be between 1 and 600 seconds")
    return run(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
